// SPDX-License-Identifier: MIT
//
// The core of a control server: the directory of connected agents, the routing of writes to them
// with one sequence number per operation, the results per peer, the latest values, and bounded
// subscriptions (vehicle_sil::control). It has no transport: a server plugs its connections in.
#pragma once

#include "vehicle_sil/control/agent.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vehicle_sil::control {

/// The server's end of one connection to an agent (implemented by the transport).
class AgentLink
{
public:
    virtual ~AgentLink() = default;
    /// Hands the command to the agent. The hub calls it without holding its lock, for one peer
    /// from one thread at a time and in sequence order, so it may block briefly and may call
    /// back into the hub. @returns false if the link is down (the command was not sent).
    virtual bool send(const Command& command) = 0;
    /// Ends the connection. The hub calls it, without holding its lock, on a link that another
    /// connection of the same participant replaced, so that the replaced agent sees the end and
    /// does not go on believing it is connected. The transport reports the end with
    /// agent_disconnected() as usual (see there: for a replaced connection it settles only the
    /// commands handed to it and not yet queued). An exception from it is ignored.
    virtual void close() {}
};

using ConnectionId = std::uint64_t;

/// What an agent says about itself when it connects. Build it from a complete schema
/// (Agent::complete_schema(): a Peer adds its monitoring outputs then).
struct PeerHello
{
    std::string participant; ///< the SIL Kit participant name (unique in the simulation, not empty)
    std::string incarnation; ///< new for every start of the agent's process
    std::vector<VariableInfo> schema;
    std::size_t max_pending = 256; ///< the agent's AgentOptions::max_pending (> 0)
    /// On a reconnect of the same process: the commands of this incarnation that the agent still
    /// holds, i.e. that it queued and whose result the transport has not delivered yet (the
    /// transport resends those results on the new connection). They become Queued on the new
    /// connection; every other command of this incarnation that is not final becomes Unknown. Empty: the transport does not know (nothing
    /// is settled; a result that never comes leaves its command open).
    std::optional<std::vector<std::uint64_t>> open_seqs{};
};

/// What the transport learns about a peer outside its steps (Peer::failed(), Peer::error()).
struct PeerStatus
{
    bool failed = false;
    std::string error;
};

struct PeerInfo
{
    std::string participant;
    std::string incarnation;
    bool connected = false;
    std::vector<VariableInfo> schema;
    PeerStatus status;
};

/// One write of a client: a variable of a peer.
struct PeerWrite
{
    std::string peer;
    std::string name;
    Value value;
};

struct OperationRequest
{
    std::vector<PeerWrite> writes;
    std::optional<SimTime> at{};
    LatePolicy late = LatePolicy::Reject;
    std::string client{}; ///< who asked (for the record)
};

/// Where the part of an operation for one peer stands.
enum class TargetStatus
{
    Sent,         ///< handed to (or waiting for) the link
    Queued,       ///< the agent queued it (it will report a result)
    Disconnected, ///< the link was lost after it was queued; the same process may still report a result
    Applied,
    Late,
    Rejected,
    Cancelled,
    Invalid,      ///< refused before it took effect (unknown peer or variable, the agent refused it, not sent)
    Busy,         ///< the peer had too many commands in flight
    Unknown,      ///< no result will come (lost while being sent, the peer restarted or was forgotten): it may or may not have taken effect
};

/// Sent, Queued and Disconnected may still change; every other status is final.
[[nodiscard]] bool is_final(TargetStatus status) noexcept;

struct TargetResult
{
    std::string peer;
    TargetStatus status = TargetStatus::Sent;
    std::optional<SimTime> applied_at;
    bool late = false;
    std::string reason;
};

struct OperationResult
{
    std::uint64_t seq = 0;
    OperationRequest request;
    std::vector<TargetResult> targets; ///< one per peer, in the order the peers first appear
    [[nodiscard]] bool done() const noexcept;
};

/// What a subscriber receives. Peers, operations and values are kept as the latest state per
/// peer or operation; events are kept in order.
struct PeerUpdate
{
    PeerInfo peer;
    bool removed = false; ///< forget_peer(): the peer is gone from the directory
};
struct ValuesUpdate
{
    std::string peer;
    std::string incarnation; ///< the schema the values follow
    std::shared_ptr<const Snapshot> snapshot;
};
struct EventUpdate
{
    std::string peer;
    std::string incarnation;
    Event event;
};
struct OperationUpdate
{
    OperationResult operation;
};
using Update = std::variant<PeerUpdate, ValuesUpdate, EventUpdate, OperationUpdate>;

struct SubscriptionOptions
{
    std::vector<std::string> peers{}; ///< empty: all
    bool values = true;
    bool events = true;
    bool operations = true;
    /// Bound of the unread events and of the unread operation updates (each). Beyond it the
    /// oldest event, or the operation that changed longest ago, is dropped and counted. Peers and
    /// values keep one entry per peer. Must be > 0.
    std::size_t capacity = 1024;
};

class Hub;

/// The updates for one client. Thread safe.
class Subscription
{
public:
    /// Takes up to `max` updates (0: all), in this order: peers, operations (by seq), events (in
    /// order), values. Waits up to `timeout` if there is none and the subscription is open.
    std::vector<Update> poll(std::size_t max, std::chrono::milliseconds timeout);
    /// Updates dropped because a queue was full, since the subscription began.
    [[nodiscard]] std::uint64_t dropped() const;
    /// Ends the subscription: a waiting poll() returns, no more updates arrive.
    void close();
    [[nodiscard]] bool closed() const;

private:
    friend class Hub;
    explicit Subscription(SubscriptionOptions options) : options_(std::move(options)) {}
    [[nodiscard]] bool wants(const std::string& peer) const;
    // Called by the hub with its lock held.
    void put_peer(const PeerInfo& info, bool removed = false);
    void put_operation(const OperationResult& op);
    void put_event(EventUpdate update);
    void put_values(const std::string& peer, const std::string& incarnation, std::shared_ptr<const Snapshot> snapshot);
    void drop_values(const std::string& peer);

    SubscriptionOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;
    std::map<std::string, PeerUpdate> peers_;
    std::map<std::uint64_t, std::pair<std::uint64_t, OperationResult>> operations_; ///< seq -> (change order, latest)
    std::map<std::uint64_t, std::uint64_t> change_index_; ///< change order -> seq
    std::uint64_t change_order_ = 0;
    std::deque<EventUpdate> events_;
    std::map<std::string, ValuesUpdate> values_;
    std::uint64_t dropped_ = 0;
};

struct HubOptions
{
    /// Finished operations remembered for operation(); the oldest are forgotten beyond it. Open
    /// operations are not forgotten; they are bounded by the peers' max_pending. Must be > 0.
    std::size_t max_operations = 4096;
    /// The first sequence number. The default (the wall clock in microseconds) keeps numbers of
    /// a restarted server above those of the previous one, which agents may still hold.
    std::optional<std::uint64_t> first_seq{};
};

/// The core of a control server. Thread safe: transports call the agent side, clients the client
/// side, from any threads. The hub never calls a link while it holds its own lock.
///
/// Every write() gets one sequence number, increasing across the hub, and is split into one
/// Command per peer that carries that number. The commands of a peer are sent in sequence order,
/// one at a time. The hub checks only that the peer is connected, has the variable, can write it
/// and has room (its max_pending); the agent checks the rest and decides the step.
class Hub
{
public:
    explicit Hub(HubOptions options = {});

    // ---- agent side (transports) -----------------------------------------------------------

    /// An agent connected. The same participant with the same incarnation is the same process
    /// reconnecting: the new connection replaces the old one, and its open commands go on.
    /// Another incarnation is a restarted process: what was open for the old one becomes Unknown.
    /// In both cases the old link, if any, is closed (AgentLink::close()): also when the old
    /// process still runs (two processes with the same name), the old one learns that it was
    /// replaced instead of being ignored silently.
    /// std::invalid_argument for an empty participant name, no link or max_pending 0.
    ConnectionId agent_connected(PeerHello hello, std::shared_ptr<AgentLink> link);
    /// The connection is gone. Commands handed to it and not yet queued become Unknown (they may
    /// or may not have reached the agent), also when the connection was replaced already. If it is
    /// the peer's current connection, the peer is disconnected: its queued commands become
    /// Disconnected and commands not yet handed to a link become Invalid.
    void agent_disconnected(ConnectionId connection);
    /// The connection is gone and the agent's process will not connect again as this incarnation
    /// (a transport that knows: the participant left the simulation, or the agent said its last
    /// word): as agent_disconnected(), and if it was the peer's current connection, every command
    /// still open for the incarnation becomes Unknown with `reason` (no result will come), so
    /// that nothing waits for it.
    void agent_ended(ConnectionId connection, const std::string& reason);
    /// What Agent::submit() returned for the command `seq`.
    void agent_submitted(ConnectionId connection, std::uint64_t seq, const SubmitResult& result);
    /// A result of the agent. Accepted from any connection of the incarnation the command was
    /// sent to (also after a reconnect, for a Disconnected command).
    void agent_result(ConnectionId connection, const CommandResult& result);
    /// The agent's latest sample (only from the current connection; an older sample and one that
    /// does not match the schema are ignored).
    void agent_values(ConnectionId connection, Snapshot snapshot);
    void agent_events(ConnectionId connection, const std::vector<Event>& events);
    void agent_status(ConnectionId connection, PeerStatus status);

    // ---- client side ----------------------------------------------------------------------

    [[nodiscard]] std::vector<PeerInfo> peers() const;
    /// The latest sample of a peer (nullptr if none since its current incarnation connected).
    [[nodiscard]] std::shared_ptr<const Snapshot> latest(const std::string& peer) const;
    /// Removes a disconnected peer (subscribers get a PeerUpdate with removed); its open commands
    /// become Unknown. @returns false if it is connected or unknown.
    bool forget_peer(const std::string& peer);
    /// Routes the writes (grouped per peer, in the order given) and returns the operation's
    /// sequence number. std::invalid_argument if there are no writes. The calling thread may
    /// also send commands of earlier operations to the same peers (one sender per peer), so it
    /// may wait for AgentLink::send().
    std::uint64_t write(OperationRequest request);
    [[nodiscard]] std::optional<OperationResult> operation(std::uint64_t seq) const;
    /// Waits until no target of `seq` is Sent any more, e.g. before resuming a paused simulation.
    /// @returns the operation then (check that every target is Queued or final as wanted), or
    /// nullopt on timeout or for an unknown seq.
    std::optional<OperationResult> wait_queued(std::uint64_t seq, std::chrono::milliseconds timeout);
    /// Waits until every target is final. @returns the operation, or nullopt on timeout or for an
    /// unknown seq.
    std::optional<OperationResult> wait_done(std::uint64_t seq, std::chrono::milliseconds timeout);
    /// The current peers and their latest values are delivered first.
    std::shared_ptr<Subscription> subscribe(SubscriptionOptions options = {});

private:
    struct PeerEntry
    {
        PeerInfo info;
        std::shared_ptr<AgentLink> link;
        ConnectionId connection = 0;
        std::size_t max_pending = 0;
        std::shared_ptr<const Snapshot> latest;
        std::deque<std::uint64_t> outbox; ///< seqs to send, in order
        bool sending = false;            ///< a thread is in AgentLink::send() for this peer
        std::uint64_t generation = 0;    ///< unique per entry: tells an entry made again after forget_peer() apart
    };
    struct Target
    {
        TargetResult result;
        std::string incarnation; ///< the one the command was meant for
        Command command;         ///< kept until it is sent
        ConnectionId handed_to = 0; ///< the connection whose link took it
    };
    struct Operation
    {
        std::uint64_t seq = 0;
        OperationRequest request;
        std::vector<Target> targets;
        bool finished = false;
    };
    struct Connection
    {
        std::string participant;
        std::string incarnation;
    };

    [[nodiscard]] static OperationResult view(const Operation& op);
    [[nodiscard]] const Connection* connection(ConnectionId id) const;
    Target* find_target(const std::string& participant, const std::string& incarnation, std::uint64_t seq);
    [[nodiscard]] std::size_t open_targets(const std::string& participant) const;
    /// Settles the open targets of `participant`/`incarnation` whose status is `from` (Unknown:
    /// every open one) to `status`.
    void settle_open(const std::string& participant, const std::string& incarnation, TargetStatus from_at_least,
                     TargetStatus status, const std::string& reason);
    void target_changed(Operation& op);
    void for_subscribers(const std::string& peer, const std::function<void(Subscription&)>& fn);
    void announce_peer(const PeerEntry& entry);
    void forget_finished();
    /// Commands still in the outbox (never handed to the link) become Invalid.
    void drop_outbox(PeerEntry& entry, const std::string& reason);
    /// Sends what the peer's outbox holds, one command at a time, releasing `lock` around send().
    void flush(const std::string& participant, std::unique_lock<std::mutex>& lock);

    HubOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t next_seq_;
    ConnectionId next_connection_ = 1;
    std::map<std::string, PeerEntry, std::less<>> peers_;
    std::map<ConnectionId, Connection> connections_;
    std::map<std::uint64_t, Operation> operations_;
    std::deque<std::uint64_t> finished_; ///< finished operations, oldest first
    std::vector<std::weak_ptr<Subscription>> subscriptions_;
};

} // namespace vehicle_sil::control
