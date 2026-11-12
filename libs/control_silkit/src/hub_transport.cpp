// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_silkit/hub_transport.hpp"

#include "topics.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <silkit/services/pubsub/all.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::control_silkit {
namespace {

namespace pubsub = SilKit::Services::PubSub;
namespace orch = SilKit::Services::Orchestration;
namespace wire = control::wire;

std::string hex(std::uint64_t v)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4)
    {
        out[static_cast<std::size_t>(i)] = kDigits[v & 0xF];
    }
    return out;
}

std::span<const std::uint8_t> span_of(const pubsub::DataMessageEvent& event)
{
    return {event.data.data(), event.data.size()};
}

} // namespace

/// Shared with the handlers the participant keeps (they cannot be removed from it) and with the
/// links the hub holds: after the transport has gone they find it closed.
struct HubTransport::State
{
    /// The publisher of the commands, shared with the links (the hub may hold a link a little
    /// longer than the transport lives): serialises their send() with the end of the transport.
    struct Sender
    {
        std::mutex mutex;
        pubsub::IDataPublisher* commands = nullptr; ///< the participant's; not used once `closed`
        ParticipantRuntime* runtime = nullptr;      ///< not used once `closed`
        bool closed = false;
        wire::Bytes bytes;
    };

    /// One agent's connection: the hub's link to it, and what the receive handlers keep for it.
    struct Link : control::AgentLink
    {
        std::shared_ptr<Sender> sender;
        std::uint64_t session = 0;
        control::ConnectionId connection = 0;
        LinkInfo info;
        bool open = true;   ///< guarded by Sender::mutex
        bool ready = false; ///< guarded by Sender::mutex: the agent answered a probe
        std::vector<control::ValueType> types; ///< of its variables, in order (fixed)
        // The receive handlers only (one thread): a sample that arrives in parts.
        control::Snapshot partial;
        bool assembling = false;
        std::uint32_t unanswered = 0; ///< step messages since the hello without an answer to the probe

        bool send(const control::Command& command) override;
        void close() override;
        /// Asks the agent whether it receives this side's commands, unless it has said so.
        /// @returns false if it is ready (no probe was sent).
        bool probe();
    };

    control::Hub* hub = nullptr;
    ParticipantRuntime* runtime = nullptr;
    bool synchronised = false; ///< the runtime takes part in the time synchronisation
    std::size_t max_links = 256;
    std::shared_ptr<Sender> sender = std::make_shared<Sender>();

    // Held by a receive handler while it runs, so that the destructor waits for it. Before
    // `mutex` in the lock order; the hub is called with it held (and never calls back into a
    // handler).
    std::mutex handler_mutex;
    bool closed = false; ///< guarded by handler_mutex and mutex (written with both)

    // The directory. Never held while the hub or SIL Kit is called.
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<std::uint64_t, std::shared_ptr<Link>> sessions;
    std::map<std::string, LinkInfo, std::less<>> names;    ///< by participant: the latest hello
    std::map<std::string, std::uint64_t, std::less<>> current; ///< by participant: its connected session
    /// The sessions that ended last (at most kEndedKept, oldest first in ended_order): their hello,
    /// seen again, makes no new link.
    static constexpr std::size_t kEndedKept = 1024;
    std::set<std::uint64_t> ended;
    std::deque<std::uint64_t> ended_order;

    /// With `mutex` held.
    void remember_ended(std::uint64_t session)
    {
        if (ended.insert(session).second)
        {
            ended_order.push_back(session);
            if (ended_order.size() > kEndedKept)
            {
                ended.erase(ended_order.front());
                ended_order.pop_front();
            }
        }
    }

    // The receive handlers only.
    wire::Hello hello;
    wire::StepMessage step;
    wire::SubmittedMessage submitted;

    std::atomic<std::uint64_t> n_hellos{0}, n_messages{0}, n_bytes{0}, n_undecodable{0}, n_unknown{0}, n_late{0}, n_calls{0},
        n_refused{0};
    std::atomic<std::int64_t> handler_ns{0}, max_handler_ns{0};

    /// Times a receive handler and holds handler_mutex for it.
    struct Handling
    {
        State& s;
        std::unique_lock<std::mutex> lock;
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        explicit Handling(State& state, std::size_t bytes) : s(state), lock(state.handler_mutex)
        {
            s.n_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        ~Handling()
        {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
            s.n_calls.fetch_add(1, std::memory_order_relaxed);
            s.handler_ns.fetch_add(ns, std::memory_order_relaxed);
            // Only the handlers write it, one at a time.
            if (ns > s.max_handler_ns.load(std::memory_order_relaxed))
            {
                s.max_handler_ns.store(ns, std::memory_order_relaxed);
            }
        }
    };

    /// A message of a session that is not connected: counted, unless the session has ended (what
    /// an agent still sends after its connection was ended for its participant's failure, such
    /// as its last word, is expected).
    void count_unknown(std::uint64_t session)
    {
        const std::lock_guard lock(mutex);
        if (ended.count(session) == 0)
        {
            n_unknown.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::shared_ptr<Link> find(std::uint64_t session) const
    {
        const std::lock_guard lock(mutex);
        const auto it = sessions.find(session);
        return it == sessions.end() ? nullptr : it->second;
    }

    /// Ends the link: no more sends, out of the directory, and the hub settles what is open.
    /// Not with `mutex` held.
    void end(const std::shared_ptr<Link>& link, const std::string& reason)
    {
        link->close();
        {
            const std::lock_guard lock(mutex);
            remember_ended(link->session);
            if (sessions.erase(link->session) == 0)
            {
                return; // ended already (by the other of: the last word, the lifecycle, the destructor)
            }
            if (const auto it = current.find(link->info.participant); it != current.end() && it->second == link->session)
            {
                current.erase(it);
                names[link->info.participant].state = LinkState::Ended;
                names[link->info.participant].late_messages = link->info.late_messages;
            }
        }
        changed.notify_all();
        hub->agent_ended(link->connection, reason);
    }

    void on_hello(const pubsub::DataMessageEvent& event)
    {
        const Handling handling(*this, event.data.size());
        if (closed)
        {
            return;
        }
        if (const wire::Error error = wire::decode_hello(span_of(event), hello))
        {
            n_undecodable.fetch_add(1, std::memory_order_relaxed);
            if (error.code == wire::ErrorCode::VersionMismatch && !hello.participant.empty())
            {
                {
                    const std::lock_guard lock(mutex);
                    if (names.find(hello.participant) == names.end() && names.size() >= max_links)
                    {
                        n_refused.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    // A connected agent of this name stays what it is: this hello is another's.
                    if (current.find(hello.participant) == current.end())
                    {
                        LinkInfo info;
                        info.participant = hello.participant;
                        info.state = LinkState::VersionMismatch;
                        info.version = error.version;
                        names[hello.participant] = std::move(info);
                    }
                }
                changed.notify_all();
            }
            return;
        }
        {
            const std::lock_guard lock(mutex);
            if (const auto known = sessions.find(hello.session); known != sessions.end())
            {
                if (known->second->info.participant != hello.participant)
                {
                    // Another participant with the session number of a connected one: it cannot
                    // be told apart from it, so it is not taken.
                    n_refused.fetch_add(1, std::memory_order_relaxed);
                }
                return; // else: the same hello again
            }
            if (ended.count(hello.session) != 0)
            {
                return; // the hello of a session that has ended, again
            }
            // A bound on what a run remembers: participants beyond it are not taken.
            if (names.find(hello.participant) == names.end() && names.size() >= max_links)
            {
                n_refused.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        auto link = std::make_shared<Link>();
        link->sender = sender;
        link->session = hello.session;
        link->info.participant = hello.participant;
        link->info.state = LinkState::Connected;
        link->info.version = wire::kVersion;
        link->info.virtual_time = hello.virtual_time;
        link->info.step = hello.step;
        link->info.sample_period = hello.sample_period;
        link->info.variables = hello.schema.size();
        link->types.reserve(hello.schema.size());
        for (const auto& variable : hello.schema)
        {
            link->types.push_back(variable.type);
        }
        control::PeerHello peer;
        peer.participant = hello.participant;
        peer.incarnation = hex(hello.session);
        peer.schema = std::move(hello.schema);
        peer.max_pending = hello.max_pending;
        std::shared_ptr<Link> replaced;
        try
        {
            // The hub closes the link of an earlier session of this participant (Link::close()).
            link->connection = hub->agent_connected(std::move(peer), link);
        }
        catch (const std::exception&)
        {
            n_undecodable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        {
            const std::lock_guard lock(mutex);
            if (const auto it = current.find(link->info.participant); it != current.end())
            {
                if (const auto old = sessions.find(it->second); old != sessions.end())
                {
                    replaced = old->second;
                    remember_ended(old->first);
                    sessions.erase(old);
                }
            }
            sessions[link->session] = link;
            current[link->info.participant] = link->session;
            names[link->info.participant] = link->info;
        }
        if (replaced)
        {
            // The same name again (the process was started anew): the hub has settled the old
            // incarnation's commands; its connection is only taken off the hub's books.
            hub->agent_disconnected(replaced->connection);
        }
        n_hellos.fetch_add(1, std::memory_order_relaxed);
        changed.notify_all();
        // The participant's lifecycle state is not looked at here: what is known of it may be the
        // state of an earlier process of the same name (one that failed and was started anew).
        // Whether the agent receives this side's commands: it may not yet (the subscriptions of
        // the two directions are made one by one); asked again with what it sends.
        link->probe();
    }

    void on_step(const pubsub::DataMessageEvent& event)
    {
        const Handling handling(*this, event.data.size());
        if (closed)
        {
            return;
        }
        if (wire::decode_step(span_of(event), step))
        {
            n_undecodable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto link = find(step.session);
        if (!link)
        {
            count_unknown(step.session);
            return;
        }
        n_messages.fetch_add(1, std::memory_order_relaxed);
        // Until it has answered: with the first message, then with every eighth.
        if (!link->info.ready && link->unanswered++ % 8 == 0)
        {
            link->probe();
        }
        // The promise of the time synchronisation, checked on every message: once this
        // participant's step at N has begun, nothing of a step before N arrives any more. The
        // handlers and the steps of this participant run on one thread, so now() is exact here.
        if (synchronised && link->info.virtual_time && !step.final && step.time < runtime->now())
        {
            n_late.fetch_add(1, std::memory_order_relaxed);
            const std::lock_guard lock(mutex);
            ++link->info.late_messages;
            names[link->info.participant].late_messages = link->info.late_messages;
        }
        for (const auto& result : step.results)
        {
            hub->agent_result(link->connection, result);
        }
        if (!step.events.empty())
        {
            hub->agent_events(link->connection, step.events);
        }
        if (step.has_values)
        {
            take_values(*link, step.values);
        }
        if (step.final)
        {
            end(link, "the participant's control agent has ended");
        }
    }

    /// A part of a sample: the parts of one sample come one after the other, the first first.
    void take_values(Link& link, wire::ValuesPart& part)
    {
        bool fits = part.total == link.info.variables;
        // Each value of the type its variable has (the count was checked against the total).
        for (std::size_t i = 0; fits && i < part.values.size(); ++i)
        {
            const control::ValueType type = link.types[part.first + i];
            const std::size_t index = part.values[i].index();
            fits = type == control::ValueType::Bool ? index == 0 : type == control::ValueType::Real ? index == 2 : index == 1;
        }
        if (!fits)
        {
            n_unknown.fetch_add(1, std::memory_order_relaxed);
            link.assembling = false;
            return;
        }
        if (part.first == 0)
        {
            link.partial.time = part.time;
            link.partial.sequence = part.sequence;
            link.partial.values.clear();
            link.partial.values.reserve(part.total);
            link.assembling = true;
        }
        else if (!link.assembling || part.sequence != link.partial.sequence || part.first != link.partial.values.size())
        {
            n_unknown.fetch_add(1, std::memory_order_relaxed);
            link.assembling = false;
            return;
        }
        link.partial.values.insert(link.partial.values.end(), part.values.begin(), part.values.end());
        if (link.partial.values.size() == part.total)
        {
            link.assembling = false;
            hub->agent_values(link.connection, std::move(link.partial));
            link.partial = {};
        }
    }

    void on_submitted(const pubsub::DataMessageEvent& event)
    {
        const Handling handling(*this, event.data.size());
        if (closed)
        {
            return;
        }
        if (const auto ready = wire::addressed(span_of(event)); ready && ready->kind == wire::Kind::Ready)
        {
            if (const auto link = find(ready->session))
            {
                {
                    const std::lock_guard lock(sender->mutex);
                    link->ready = true;
                }
                {
                    const std::lock_guard lock(mutex);
                    link->info.ready = true;
                    if (const auto it = current.find(link->info.participant); it != current.end() && it->second == link->session)
                    {
                        names[link->info.participant].ready = true;
                    }
                }
                changed.notify_all();
            }
            else
            {
                count_unknown(ready->session);
            }
            return;
        }
        if (wire::decode_submitted(span_of(event), submitted))
        {
            n_undecodable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto link = find(submitted.session);
        if (!link)
        {
            count_unknown(submitted.session);
            return;
        }
        n_messages.fetch_add(1, std::memory_order_relaxed);
        hub->agent_submitted(link->connection, submitted.seq, submitted.result);
    }

    /// The runtime's peer status observer (the SIL Kit thread, or a thread that re-reads the
    /// statuses).
    void on_status(const std::string& participant, const PeerStatus& status)
    {
        if (status.state != orch::ParticipantState::Error)
        {
            if (has_finished(status))
            {
                changed.notify_all(); // wait_for_hellos() stops waiting for it
            }
            return;
        }
        // As a receive handler: the destructor waits for it, and it does not run beside one.
        const std::lock_guard handling(handler_mutex);
        if (closed)
        {
            return;
        }
        std::shared_ptr<Link> link;
        {
            const std::lock_guard lock(mutex);
            if (const auto it = current.find(participant); it != current.end())
            {
                if (const auto s = sessions.find(it->second); s != sessions.end())
                {
                    link = s->second;
                }
            }
        }
        if (link)
        {
            end(link, "the participant failed or its connection was lost (lifecycle state Error)");
        }
        changed.notify_all();
    }
};

bool HubTransport::State::Link::send(const control::Command& command)
{
    const std::lock_guard lock(sender->mutex);
    // Not to an agent that has not said that it receives commands (the command might reach
    // nobody, and nothing would ever settle it), nor to a participant that is leaving or has left
    // (its agent's last word may not arrive).
    if (!open || sender->closed || !ready || has_finished(sender->runtime->peer_status(info.participant)))
    {
        return false;
    }
    if (!wire::encode_command(wire::CommandMessage{session, command}, sender->bytes))
    {
        return false; // too large for one message: not sent
    }
    sender->commands->Publish(detail::span_of(sender->bytes));
    return true;
}

void HubTransport::State::Link::close()
{
    const std::lock_guard lock(sender->mutex);
    open = false;
}

bool HubTransport::State::Link::probe()
{
    const std::lock_guard lock(sender->mutex);
    if (ready || !open || sender->closed)
    {
        return false;
    }
    wire::encode_session(wire::Kind::Probe, session, sender->bytes);
    sender->commands->Publish(detail::span_of(sender->bytes));
    return true;
}

HubTransport::HubTransport(control::Hub& hub, ParticipantRuntime& runtime, HubTransportOptions options)
{
    if (options.max_links == 0)
    {
        throw std::invalid_argument("vehicle_sil::control_silkit::HubTransport: max_links must be > 0");
    }
    if (runtime.started())
    {
        throw std::logic_error("vehicle_sil::control_silkit::HubTransport: the participant \"" + runtime.name() +
                               "\" has started; make the transport before it starts (SIL Kit connects the publishers "
                               "and subscribers made before the start)");
    }
    auto state = std::make_shared<State>();
    state->hub = &hub;
    state->runtime = &runtime;
    state->synchronised = runtime.uses_virtual_time();
    state->max_links = options.max_links;
    state->sender->runtime = &runtime;
    runtime.set_peer_status_observer(
        [state](const std::string& participant, const PeerStatus& status) { state->on_status(participant, status); });
    auto& participant = runtime.participant();
    state->sender->commands = participant.CreateDataPublisher(detail::kCommandTopic, detail::spec(detail::kCommandTopic), 0);
    participant.CreateDataSubscriber(detail::kHelloTopic, detail::spec(detail::kHelloTopic),
                                     [state](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                         state->on_hello(event);
                                     });
    participant.CreateDataSubscriber(detail::kStepTopic, detail::spec(detail::kStepTopic),
                                     [state](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                         state->on_step(event);
                                     });
    participant.CreateDataSubscriber(detail::kSubmittedTopic, detail::spec(detail::kSubmittedTopic),
                                     [state](pubsub::IDataSubscriber*, const pubsub::DataMessageEvent& event) {
                                         state->on_submitted(event);
                                     });
    state_ = std::move(state);
}

HubTransport::~HubTransport()
{
    std::vector<std::shared_ptr<State::Link>> links;
    {
        // Waits for a handler that is running; later ones (and the observer) do nothing.
        const std::lock_guard handler(state_->handler_mutex);
        const std::lock_guard lock(state_->mutex);
        state_->closed = true;
        for (const auto& [session, link] : state_->sessions)
        {
            links.push_back(link);
        }
    }
    {
        // Waits for a send that is going out; none follows.
        const std::lock_guard lock(state_->sender->mutex);
        state_->sender->closed = true;
    }
    for (const auto& link : links)
    {
        state_->end(link, "the control transport was closed");
    }
}

HelloReport HubTransport::wait_for_hellos(const std::vector<std::string>& participants,
                                          std::chrono::steady_clock::time_point deadline, const std::function<bool()>& cancelled)
{
    auto& s = *state_;
    while (true)
    {
        HelloReport report;
        // The lifecycle states first, without the directory's lock (the observer takes that lock
        // under the runtime's).
        std::vector<bool> finished;
        finished.reserve(participants.size());
        for (const auto& name : participants)
        {
            finished.push_back(has_finished(s.runtime->peer_status(name)));
        }
        // Every agent that has not said that it receives commands is asked (again): here, in
        // the window before the start, the question reaches every required participant.
        std::vector<std::shared_ptr<State::Link>> links;
        {
            const std::lock_guard lock(s.mutex);
            for (const auto& [session, link] : s.sessions)
            {
                if (!link->info.ready)
                {
                    links.push_back(link);
                }
            }
        }
        for (const auto& link : links)
        {
            link->probe();
        }
        std::unique_lock lock(s.mutex);
        for (std::size_t i = 0; i < participants.size(); ++i)
        {
            const auto it = s.names.find(participants[i]);
            if (it != s.names.end() && it->second.state == LinkState::Connected && it->second.ready)
            {
                report.connected.push_back(participants[i]);
            }
            else if (it != s.names.end() && it->second.state == LinkState::Connected && !finished[i])
            {
                report.missing.push_back(participants[i]); // said hello, has not answered yet
            }
            else if (it != s.names.end() && it->second.state == LinkState::VersionMismatch)
            {
                report.mismatched.push_back(participants[i]);
            }
            else if (it != s.names.end() || finished[i])
            {
                report.ended.push_back(participants[i]);
            }
            else
            {
                report.missing.push_back(participants[i]);
            }
        }
        if (report.missing.empty())
        {
            return report;
        }
        report.cancelled = cancelled && cancelled();
        const auto now = std::chrono::steady_clock::now();
        if (report.cancelled || now >= deadline)
        {
            return report;
        }
        s.changed.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(10)));
    }
}

HelloReport HubTransport::wait_for_announced(const std::vector<std::string>& participants,
                                             std::chrono::steady_clock::time_point deadline, const std::function<bool()>& cancelled)
{
    std::vector<std::string> announced;
    std::vector<std::string> silent;
    {
        const std::lock_guard lock(state_->mutex);
        for (const auto& name : participants)
        {
            (state_->names.count(name) != 0 ? announced : silent).push_back(name);
        }
    }
    HelloReport report = wait_for_hellos(announced, deadline, cancelled);
    report.not_announced = std::move(silent);
    return report;
}

std::vector<LinkInfo> HubTransport::links() const
{
    const std::lock_guard lock(state_->mutex);
    std::vector<LinkInfo> out;
    out.reserve(state_->names.size());
    for (const auto& [name, info] : state_->names)
    {
        out.push_back(info);
    }
    return out;
}

HubTransportStats HubTransport::stats() const
{
    const auto& s = *state_;
    HubTransportStats out;
    out.hellos = s.n_hellos.load(std::memory_order_relaxed);
    out.messages = s.n_messages.load(std::memory_order_relaxed);
    out.bytes = s.n_bytes.load(std::memory_order_relaxed);
    out.undecodable = s.n_undecodable.load(std::memory_order_relaxed);
    out.unknown = s.n_unknown.load(std::memory_order_relaxed);
    out.late_messages = s.n_late.load(std::memory_order_relaxed);
    out.refused_hellos = s.n_refused.load(std::memory_order_relaxed);
    out.handler_calls = s.n_calls.load(std::memory_order_relaxed);
    out.handler_time = std::chrono::nanoseconds(s.handler_ns.load(std::memory_order_relaxed));
    out.max_handler_time = std::chrono::nanoseconds(s.max_handler_ns.load(std::memory_order_relaxed));
    return out;
}

} // namespace vehicle_sil::control_silkit
