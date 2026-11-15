// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_silkit/agent_transport.hpp"

#include "topics.hpp"

#include <silkit/services/pubsub/all.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::control_silkit {
namespace {

namespace pubsub = SilKit::Services::PubSub;
namespace wire = control::wire;

/// A number that no other transport of the simulation has, in practice: 64 bits from the system's
/// random source, mixed with the clocks and this thread (a random source that repeats itself
/// still gives two processes different numbers).
std::uint64_t new_session(const void* salt)
{
    std::uint64_t x = 0;
    try
    {
        std::random_device device;
        x = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }
    catch (const std::exception&)
    {
        // No random source: the clocks below.
    }
    const auto mix = [&x](std::uint64_t v) {
        x ^= v + 0x9E3779B97F4A7C15ULL + (x << 6) + (x >> 2);
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 31;
    };
    mix(static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
    mix(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    mix(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    mix(reinterpret_cast<std::uintptr_t>(salt));
    return x != 0 ? x : 1;
}

} // namespace

/// Shared with the handlers the participant keeps (they cannot be removed from it): after the
/// transport has gone they find it closed and touch nothing else.
struct AgentTransport::State
{
    control::Agent* agent = nullptr;
    std::uint64_t session = 0;
    std::size_t max_bytes = wire::kMaxMessageBytes;
    pubsub::IDataPublisher* hello = nullptr;
    pubsub::IDataPublisher* step = nullptr;
    pubsub::IDataPublisher* submitted = nullptr;

    // The steps (the step sink) against the destructor. Only this transport takes it; the step
    // thread never waits for it but while the transport closes.
    std::mutex step_mutex;
    bool step_closed = false; ///< guarded by step_mutex
    SimTime last_step{};      ///< guarded by step_mutex
    std::vector<control::CommandResult> results;
    std::vector<control::Event> events;
    wire::Bytes step_bytes;
    std::function<void(const wire::Bytes&)> emit;

    // The receive handler against the destructor.
    std::mutex command_mutex;
    bool command_closed = false; ///< guarded by command_mutex
    /// Guarded by command_mutex: a hub has asked this agent (a probe) or sent it a command, so
    /// there is one that can answer the last word.
    bool hub_heard = false;
    /// Guarded by command_mutex: the hub answered the last word. The destructor waits for it on
    /// `last_word_answer` (the handler that sets it runs on SIL Kit's thread).
    bool last_word_received = false;
    std::condition_variable last_word_answer;
    wire::CommandMessage command;
    wire::Bytes submitted_bytes;

    std::atomic<std::uint64_t> n_messages{0}, n_bytes{0}, n_commands{0}, n_ignored{0}, n_undecodable{0};

    /// With step_mutex held: what the agent has produced since the last call, published now.
    void publish(SimTime now, const control::Snapshot* sample, bool final)
    {
        results.clear();
        events.clear();
        agent->drain_results(results);
        agent->drain_events(events);
        wire::encode_step(session, now, final, results, events, sample, max_bytes, step_bytes, emit);
    }

    /// The step thread, at the end of a step.
    void on_step(SimTime now, const control::Snapshot* sample)
    {
        const std::lock_guard lock(step_mutex);
        if (step_closed)
        {
            return;
        }
        last_step = now;
        publish(now, sample, false);
    }

    /// SIL Kit's thread, outside the steps.
    void on_command(SilKit::Util::Span<const std::uint8_t> data)
    {
        const std::lock_guard lock(command_mutex);
        const std::span<const std::uint8_t> bytes(data.data(), data.size());
        // Whom it is for, before anything else is read: every agent receives every command.
        const auto to = wire::addressed(bytes);
        if (command_closed)
        {
            // The transport is closing or has gone: only the hub's answer to the last word is
            // still of interest (the destructor may be waiting for it).
            if (to && to->kind == wire::Kind::LastWordReceived && to->session == session)
            {
                last_word_received = true;
                last_word_answer.notify_all();
            }
            return;
        }
        if (!to || to->kind == wire::Kind::Ready)
        {
            n_undecodable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (to->session != session)
        {
            n_ignored.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (to->kind == wire::Kind::LastWordReceived)
        {
            // An answer to a last word this agent has not said.
            n_undecodable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        hub_heard = true;
        if (to->kind == wire::Kind::Probe)
        {
            // The hub's commands arrive here: say so (each time it asks).
            wire::encode_session(wire::Kind::Ready, session, submitted_bytes);
            submitted->Publish(detail::span_of(submitted_bytes));
            return;
        }
        if (wire::decode_command(bytes, command))
        {
            n_undecodable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        n_commands.fetch_add(1, std::memory_order_relaxed);
        wire::SubmittedMessage answer;
        answer.session = session;
        answer.seq = command.command.seq;
        try
        {
            answer.result = agent->submit(std::move(command.command));
        }
        catch (const std::exception& e)
        {
            answer.result = control::SubmitResult{control::SubmitStatus::Invalid, e.what()};
        }
        wire::encode_submitted(answer, submitted_bytes);
        submitted->Publish(detail::span_of(submitted_bytes));
    }
};

AgentTransport::AgentTransport(control::Agent& agent, ParticipantRuntime& runtime, AgentTransportOptions options)
{
    if (options.max_message_bytes < wire::kMinMessageBytes || options.max_message_bytes > wire::kMaxMessageBytes)
    {
        throw std::invalid_argument("vehicle_sil::control_silkit::AgentTransport: max_message_bytes must be within " +
                                    std::to_string(wire::kMinMessageBytes) + ".." + std::to_string(wire::kMaxMessageBytes));
    }
    if (options.last_word_wait < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument("vehicle_sil::control_silkit::AgentTransport: last_word_wait must not be negative "
                                    "(0: do not wait for the hub's answer to the last word)");
    }
    if (runtime.started())
    {
        throw std::logic_error("vehicle_sil::control_silkit::AgentTransport: the participant \"" + runtime.name() +
                               "\" has started; make the transport before the ECU starts (SIL Kit connects the "
                               "publishers and subscribers made before the start)");
    }
    agent.complete_schema(); // on the set-up thread; what the driver's completion throws comes out here

    auto state = std::make_shared<State>();
    state->agent = &agent;
    state->session = new_session(state.get());
    state->max_bytes = options.max_message_bytes;
    last_word_wait_ = options.last_word_wait;

    wire::Hello hello;
    hello.participant = runtime.name();
    hello.session = state->session;
    hello.virtual_time = runtime.uses_virtual_time();
    hello.step = options.step > SimDuration::zero() ? options.step : SimDuration::zero();
    hello.sample_period = agent.options().sample_period;
    hello.max_pending = agent.options().max_pending;
    hello.schema = agent.schema();
    wire::Bytes hello_bytes;
    std::string why;
    if (!wire::encode_hello(hello, hello_bytes, why))
    {
        throw std::invalid_argument("vehicle_sil::control_silkit::AgentTransport: the control agent of \"" + runtime.name() +
                                    "\" cannot be announced: " + why);
    }

    // The sink first: it is what can be refused (another transport has it, the steps have begun),
    // and nothing made below can be taken back from the participant.
    agent.set_step_sink([state](SimTime now, const control::Snapshot* sample) { state->on_step(now, sample); });
    {
        // Until everything below exists, the sink does nothing.
        const std::lock_guard lock(state->step_mutex);
        state->step_closed = true;
    }
    auto& participant = runtime.participant();
    state->hello = participant.CreateDataPublisher(detail::kHelloTopic, detail::spec(detail::kHelloTopic), 1);
    state->step = participant.CreateDataPublisher(detail::kStepTopic, detail::spec(detail::kStepTopic), 0);
    state->submitted = participant.CreateDataPublisher(detail::kSubmittedTopic, detail::spec(detail::kSubmittedTopic), 0);
    state->emit = [s = state.get()](const wire::Bytes& message) {
        s->step->Publish(detail::span_of(message));
        s->n_messages.fetch_add(1, std::memory_order_relaxed);
        s->n_bytes.fetch_add(message.size(), std::memory_order_relaxed);
    };
    {
        const std::lock_guard lock(state->command_mutex);
        state->command_closed = true; // until the constructor is through
    }
    participant.CreateDataSubscriber(detail::kCommandTopic, detail::spec(detail::kCommandTopic),
                                     [state](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                         state->on_command(event.data);
                                     });
    // Kept by SIL Kit (history 1) for a hub that subscribes later.
    state->hello->Publish(detail::span_of(hello_bytes));

    lease_ = std::make_unique<control::TransportLease>(agent);
    {
        const std::lock_guard lock(state->command_mutex);
        state->command_closed = false;
    }
    {
        const std::lock_guard lock(state->step_mutex);
        state->step_closed = false;
    }
    state_ = std::move(state);
}

AgentTransport::~AgentTransport()
{
    // Whether there is a hub to wait for: one that has asked this agent or sent it a command.
    // Without one (a simulation without a hub, or one whose commands do not arrive here) nothing
    // waits for this agent, and nobody would answer.
    bool wait = false;
    {
        // Waits for a command being submitted; later ones are ignored.
        const std::lock_guard lock(state_->command_mutex);
        state_->command_closed = true;
        wait = state_->hub_heard && last_word_wait_ > std::chrono::milliseconds::zero();
    }
    {
        // Waits for a step being published; later steps publish nothing.
        const std::lock_guard lock(state_->step_mutex);
        state_->step_closed = true;
        try
        {
            // The last word: what was not sent yet (after Peer::stop(), the cancelled writes).
            state_->publish(state_->last_step, nullptr, true);
        }
        catch (const std::exception&)
        {
            // The hub learns of the end from the participant's lifecycle.
            wait = false;
        }
    }
    if (wait)
    {
        // The owner destroys the participant next, and SIL Kit then drops what it has not written
        // to the socket yet: the last word, if an earlier send is still under way. So the hub's
        // answer is waited for, for a bounded time (the hub may have gone, or be a version that
        // does not answer). The answer arrives on SIL Kit's thread, which takes command_mutex
        // only while this thread does not hold it.
        std::unique_lock lock(state_->command_mutex);
        state_->last_word_answer.wait_for(lock, last_word_wait_, [this] { return state_->last_word_received; });
    }
    lease_.reset();
}

std::uint64_t AgentTransport::session() const noexcept
{
    return state_->session;
}

AgentTransportStats AgentTransport::stats() const
{
    AgentTransportStats s;
    s.step_messages = state_->n_messages.load(std::memory_order_relaxed);
    s.step_bytes = state_->n_bytes.load(std::memory_order_relaxed);
    s.commands = state_->n_commands.load(std::memory_order_relaxed);
    s.ignored = state_->n_ignored.load(std::memory_order_relaxed);
    s.undecodable = state_->n_undecodable.load(std::memory_order_relaxed);
    return s;
}

} // namespace vehicle_sil::control_silkit
