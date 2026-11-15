// SPDX-License-Identifier: MIT
//
// The agent's side of the control messages as an ECU built with vehicle-sil 0.1.0 speaks it:
// version 1 only. For the tests of what a hub of a later version does with such an ECU.
//
// What is written by hand here, and not taken from the library, is the rule a 0.1.0 agent applies
// to what arrives on the command topic: a message whose version is not 1 is dropped, whomever it
// is for (addressed_by_hand()). A test that sees this side answer therefore knows that the hub
// wrote version 1, without relying on the library's readers to say so. What this side writes comes
// from the library's encoders told to write version 1; that those write the bytes of 0.1.0 is
// pinned by the control.wire_v1_* tests.
#pragma once

#include "topics.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/pubsub/all.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::test {

/// The header of a control message and the session behind it, read byte by byte.
struct AddressedByHand
{
    std::uint16_t version = 0;
    std::uint8_t kind = 0; ///< control::wire::Kind as a number
    std::uint64_t session = 0;
};

/// Empty unless `bytes` begins with 'V' 'S' 'C' 'W' and is long enough for a session.
inline std::optional<AddressedByHand> addressed_by_hand(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 16 || bytes[0] != 'V' || bytes[1] != 'S' || bytes[2] != 'C' || bytes[3] != 'W')
    {
        return std::nullopt;
    }
    AddressedByHand out;
    out.version = static_cast<std::uint16_t>(bytes[4] | (bytes[5] << 8));
    out.kind = bytes[6];
    for (std::size_t i = 0; i < 8; ++i)
    {
        out.session |= static_cast<std::uint64_t>(bytes[8 + i]) << (8 * i);
    }
    return out;
}

/// A transport for a control::Agent that speaks version 1 and nothing else, as
/// control_silkit::AgentTransport of vehicle-sil 0.1.0 does: it says hello, answers the hub's
/// probes, gives the commands for its session to the agent and says what became of them, sends
/// what each step produced, and says a last word when it is destroyed (and waits for the answer).
/// Made before the participant starts, destroyed before the participant.
class Version1Transport
{
public:
    static constexpr std::uint16_t kVersion = 1;

    Version1Transport(control::Agent& agent, ParticipantRuntime& runtime, SimDuration step, std::uint64_t session,
                      std::chrono::milliseconds last_word_wait = std::chrono::seconds(30)) :
      state_(std::make_shared<State>()), last_word_wait_(last_word_wait)
    {
        namespace pubsub = SilKit::Services::PubSub;
        namespace topics = control_silkit::detail;
        namespace wire = control::wire;
        agent.complete_schema();
        state_->agent = &agent;
        state_->session = session;

        wire::Hello hello;
        hello.participant = runtime.name();
        hello.session = session;
        hello.virtual_time = runtime.uses_virtual_time();
        hello.step = step;
        hello.sample_period = agent.options().sample_period;
        hello.max_pending = agent.options().max_pending;
        hello.schema = agent.schema();
        wire::Bytes hello_bytes;
        std::string why;
        if (!wire::encode_hello(hello, hello_bytes, why, kVersion))
        {
            throw std::invalid_argument("Version1Transport: " + why);
        }
        agent.set_step_sink([state = state_](SimTime now, const control::Snapshot* sample) {
            const std::lock_guard lock(state->step_mutex);
            if (!state->step_closed)
            {
                state->last_step = now;
                state->publish(now, sample, false);
            }
        });
        auto& participant = runtime.participant();
        auto* hello_out = participant.CreateDataPublisher(topics::kHelloTopic, topics::spec(topics::kHelloTopic), 1);
        state_->step = participant.CreateDataPublisher(topics::kStepTopic, topics::spec(topics::kStepTopic), 0);
        state_->submitted = participant.CreateDataPublisher(topics::kSubmittedTopic, topics::spec(topics::kSubmittedTopic), 0);
        participant.CreateDataSubscriber(topics::kCommandTopic, topics::spec(topics::kCommandTopic),
                                         [state = state_](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                             state->on_command({event.data.data(), event.data.size()});
                                         });
        hello_out->Publish(topics::span_of(hello_bytes));
        lease_ = std::make_unique<control::TransportLease>(agent);
    }

    Version1Transport(const Version1Transport&) = delete;
    Version1Transport& operator=(const Version1Transport&) = delete;

    ~Version1Transport()
    {
        {
            const std::lock_guard lock(state_->command_mutex);
            state_->command_closed = true;
        }
        {
            const std::lock_guard lock(state_->step_mutex);
            state_->step_closed = true;
            state_->publish(state_->last_step, nullptr, true); // the last word
        }
        if (state_->probes.load() > 0)
        {
            std::unique_lock lock(state_->command_mutex);
            state_->answer.wait_for(lock, last_word_wait_, [this] { return state_->last_word_answered.load(); });
        }
        lease_.reset();
    }

    /// Probes of version 1 for this session (each was answered).
    [[nodiscard]] int probes() const noexcept { return state_->probes.load(); }
    /// Commands of version 1 for this session that were given to the agent.
    [[nodiscard]] int commands() const noexcept { return state_->commands.load(); }
    /// Messages on the command topic that were dropped because their version is not 1.
    [[nodiscard]] int of_another_version() const noexcept { return state_->of_another_version.load(); }
    /// Of those: the ones for this session (a hub that writes to this agent in a version it does
    /// not speak).
    [[nodiscard]] int of_another_version_for_this() const noexcept { return state_->of_another_version_for_this.load(); }
    /// What the destructor leaves: whether the hub answered the last word, in version 1.
    [[nodiscard]] std::shared_ptr<const std::atomic<bool>> last_word_answered() const
    {
        return {state_, &state_->last_word_answered};
    }

private:
    /// Shared with the handlers the participant keeps.
    struct State
    {
        control::Agent* agent = nullptr;
        std::uint64_t session = 0;
        SilKit::Services::PubSub::IDataPublisher* step = nullptr;
        SilKit::Services::PubSub::IDataPublisher* submitted = nullptr;

        std::mutex step_mutex;
        bool step_closed = false;
        SimTime last_step{};
        std::vector<control::CommandResult> results;
        std::vector<control::Event> events;
        control::wire::Bytes step_bytes;

        std::mutex command_mutex;
        bool command_closed = false;
        std::condition_variable answer;
        control::wire::CommandMessage command;
        control::wire::Bytes submitted_bytes;

        std::atomic<int> probes{0}, commands{0}, of_another_version{0}, of_another_version_for_this{0};
        std::atomic<bool> last_word_answered{false};

        /// With step_mutex held.
        void publish(SimTime now, const control::Snapshot* sample, bool final)
        {
            results.clear();
            events.clear();
            agent->drain_results(results);
            agent->drain_events(events);
            control::wire::encode_step(
                session, now, final, results, events, sample, control::wire::kMaxMessageBytes, step_bytes,
                [this](const control::wire::Bytes& message) { step->Publish(control_silkit::detail::span_of(message)); },
                kVersion);
        }

        void on_command(std::span<const std::uint8_t> bytes)
        {
            namespace wire = control::wire;
            const std::lock_guard lock(command_mutex);
            const auto to = addressed_by_hand(bytes);
            if (!to)
            {
                return;
            }
            // The rule of 0.1.0, before the session is looked at: only version 1 is read.
            if (to->version != kVersion)
            {
                ++of_another_version;
                if (to->session == session)
                {
                    ++of_another_version_for_this;
                }
                return;
            }
            if (to->session != session)
            {
                return;
            }
            if (to->kind == static_cast<std::uint8_t>(wire::Kind::LastWordReceived))
            {
                if (command_closed)
                {
                    last_word_answered = true;
                    answer.notify_all();
                }
                return;
            }
            if (command_closed)
            {
                return;
            }
            if (to->kind == static_cast<std::uint8_t>(wire::Kind::Probe))
            {
                ++probes;
                wire::encode_session(wire::Kind::Ready, session, submitted_bytes, kVersion);
                submitted->Publish(control_silkit::detail::span_of(submitted_bytes));
                return;
            }
            if (to->kind != static_cast<std::uint8_t>(wire::Kind::Command) || wire::decode_command(bytes, command))
            {
                return;
            }
            ++commands;
            wire::SubmittedMessage said;
            said.session = session;
            said.seq = command.command.seq;
            said.result = agent->submit(std::move(command.command));
            wire::encode_submitted(said, submitted_bytes, kVersion);
            submitted->Publish(control_silkit::detail::span_of(submitted_bytes));
        }
    };

    std::shared_ptr<State> state_;
    std::chrono::milliseconds last_word_wait_;
    std::unique_ptr<control::TransportLease> lease_;
};

} // namespace vehicle_sil::test
