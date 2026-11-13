// SPDX-License-Identifier: MIT
#include "vehicle_sil/control/hub.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace vehicle_sil::control {

bool is_final(TargetStatus status) noexcept
{
    return status != TargetStatus::Sent && status != TargetStatus::Queued && status != TargetStatus::Disconnected;
}

bool OperationResult::done() const noexcept
{
    return std::all_of(targets.begin(), targets.end(), [](const TargetResult& t) { return is_final(t.status); });
}

// ---- Subscription --------------------------------------------------------------------------

bool Subscription::wants(const std::string& peer) const
{
    return options_.peers.empty() || std::find(options_.peers.begin(), options_.peers.end(), peer) != options_.peers.end();
}

void Subscription::put_peer(const PeerInfo& info, bool removed)
{
    {
        const std::lock_guard lock(mutex_);
        if (closed_)
        {
            return;
        }
        peers_[info.participant] = PeerUpdate{info, removed};
        if (removed)
        {
            values_.erase(info.participant);
        }
    }
    cv_.notify_all();
}

void Subscription::put_operation(const OperationResult& op)
{
    {
        const std::lock_guard lock(mutex_);
        if (closed_)
        {
            return;
        }
        auto& entry = operations_[op.seq];
        if (entry.first != 0)
        {
            change_index_.erase(entry.first);
        }
        entry = {++change_order_, op};
        change_index_.emplace(entry.first, op.seq);
        if (operations_.size() > options_.capacity)
        {
            // Drop the operation that changed longest ago (not the one that just changed).
            const auto oldest = change_index_.begin();
            operations_.erase(oldest->second);
            change_index_.erase(oldest);
            ++dropped_;
        }
    }
    cv_.notify_all();
}

void Subscription::put_event(EventUpdate update)
{
    {
        const std::lock_guard lock(mutex_);
        if (closed_)
        {
            return;
        }
        if (events_.size() >= options_.capacity)
        {
            events_.pop_front();
            ++dropped_;
        }
        events_.push_back(std::move(update));
    }
    cv_.notify_all();
}

void Subscription::put_values(const std::string& peer, const std::string& incarnation, std::shared_ptr<const Snapshot> snapshot)
{
    {
        const std::lock_guard lock(mutex_);
        if (closed_)
        {
            return;
        }
        values_[peer] = ValuesUpdate{peer, incarnation, std::move(snapshot)};
    }
    cv_.notify_all();
}

void Subscription::drop_values(const std::string& peer)
{
    const std::lock_guard lock(mutex_);
    values_.erase(peer);
}

std::vector<Update> Subscription::poll(std::size_t max, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [&] {
        return closed_ || !peers_.empty() || !operations_.empty() || !events_.empty() || !values_.empty();
    });
    const auto room = [&](const std::vector<Update>& out) { return max == 0 || out.size() < max; };
    std::vector<Update> out;
    while (!peers_.empty() && room(out))
    {
        auto node = peers_.extract(peers_.begin());
        out.emplace_back(std::move(node.mapped()));
    }
    while (!operations_.empty() && room(out))
    {
        auto node = operations_.extract(operations_.begin());
        change_index_.erase(node.mapped().first);
        out.emplace_back(OperationUpdate{std::move(node.mapped().second)});
    }
    while (!events_.empty() && room(out))
    {
        out.emplace_back(std::move(events_.front()));
        events_.pop_front();
    }
    while (!values_.empty() && room(out))
    {
        auto node = values_.extract(values_.begin());
        out.emplace_back(std::move(node.mapped()));
    }
    return out;
}

std::uint64_t Subscription::dropped() const
{
    const std::lock_guard lock(mutex_);
    return dropped_;
}

void Subscription::close()
{
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        peers_.clear();
        operations_.clear();
        change_index_.clear();
        events_.clear();
        values_.clear();
    }
    cv_.notify_all();
}

bool Subscription::closed() const
{
    const std::lock_guard lock(mutex_);
    return closed_;
}

// ---- Hub -----------------------------------------------------------------------------------

namespace {

std::uint64_t default_first_seq()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(std::max<std::int64_t>(1, us.count()));
}

} // namespace

Hub::Hub(HubOptions options) : options_(options), next_seq_(options.first_seq.value_or(default_first_seq()))
{
    if (options_.max_operations == 0)
    {
        throw std::invalid_argument("HubOptions max_operations must be > 0");
    }
}

OperationResult Hub::view(const Operation& op)
{
    OperationResult result;
    result.seq = op.seq;
    result.request = op.request;
    for (const auto& t : op.targets)
    {
        result.targets.push_back(t.result);
    }
    return result;
}

const Hub::Connection* Hub::connection(ConnectionId id) const
{
    const auto it = connections_.find(id);
    return it == connections_.end() ? nullptr : &it->second;
}

void Hub::for_subscribers(const std::string& peer, const std::function<void(Subscription&)>& fn)
{
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();)
    {
        auto sub = it->lock();
        if (!sub || sub->closed())
        {
            it = subscriptions_.erase(it);
            continue;
        }
        if (sub->wants(peer))
        {
            fn(*sub);
        }
        ++it;
    }
}

void Hub::announce_peer(const PeerEntry& entry)
{
    for_subscribers(entry.info.participant, [&](Subscription& s) { s.put_peer(entry.info); });
}

void Hub::target_changed(Operation& op)
{
    changed_.notify_all();
    const auto result = view(op);
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();)
    {
        auto sub = it->lock();
        if (!sub || sub->closed())
        {
            it = subscriptions_.erase(it);
            continue;
        }
        if (sub->options_.operations
            && std::any_of(op.targets.begin(), op.targets.end(), [&](const Target& t) { return sub->wants(t.result.peer); }))
        {
            sub->put_operation(result);
        }
        ++it;
    }
    if (!op.finished && result.done())
    {
        op.finished = true;
        finished_.push_back(op.seq);
    }
}

void Hub::forget_finished()
{
    while (finished_.size() > options_.max_operations)
    {
        operations_.erase(finished_.front());
        finished_.pop_front();
    }
}

Hub::Target* Hub::find_target(const std::string& participant, const std::string& incarnation, std::uint64_t seq)
{
    auto op = operations_.find(seq);
    if (op == operations_.end())
    {
        return nullptr;
    }
    for (auto& t : op->second.targets)
    {
        if (t.result.peer == participant && t.incarnation == incarnation)
        {
            return &t;
        }
    }
    return nullptr;
}

std::size_t Hub::open_targets(const std::string& participant) const
{
    std::size_t n = 0;
    for (const auto& [seq, op] : operations_)
    {
        if (op.finished)
        {
            continue;
        }
        for (const auto& t : op.targets)
        {
            n += t.result.peer == participant && !is_final(t.result.status) ? 1 : 0;
        }
    }
    return n;
}

void Hub::settle_open(const std::string& participant, const std::string& incarnation, TargetStatus from,
                      TargetStatus status, const std::string& reason)
{
    // `from` selects the targets: Sent (not yet queued), Queued and Disconnected, or Unknown for
    // "every open one".
    for (auto& [seq, op] : operations_)
    {
        if (op.finished)
        {
            continue;
        }
        bool changed = false;
        for (auto& t : op.targets)
        {
            if (t.result.peer != participant || t.incarnation != incarnation || is_final(t.result.status))
            {
                continue;
            }
            if (from != TargetStatus::Unknown && t.result.status != from)
            {
                continue;
            }
            t.result.status = status;
            t.result.reason = reason;
            changed = true;
        }
        if (changed)
        {
            target_changed(op);
        }
    }
}

void Hub::drop_outbox(PeerEntry& entry, const std::string& reason)
{
    // Not handed to the link yet: never sent.
    for (const auto seq : std::exchange(entry.outbox, {}))
    {
        if (auto* t = find_target(entry.info.participant, entry.info.incarnation, seq);
            t != nullptr && t->result.status == TargetStatus::Sent)
        {
            t->result.status = TargetStatus::Invalid;
            t->result.reason = reason;
            target_changed(operations_.at(seq));
        }
    }
}

ConnectionId Hub::agent_connected(PeerHello hello, std::shared_ptr<AgentLink> link)
{
    if (hello.participant.empty() || !link || hello.max_pending == 0)
    {
        throw std::invalid_argument("Hub: an agent needs a participant name, a link and max_pending > 0");
    }
    if (hello.open_seqs)
    {
        std::sort(hello.open_seqs->begin(), hello.open_seqs->end()); // looked up under the lock
    }
    std::shared_ptr<AgentLink> old_link; // released after the lock: its destructor may wait for a thread that calls the hub
    std::unique_lock lock(mutex_);
    const auto id = next_connection_++;
    connections_[id] = Connection{hello.participant, hello.incarnation};
    auto [it, inserted] = peers_.try_emplace(hello.participant);
    auto& entry = it->second;
    if (!inserted && entry.info.incarnation != hello.incarnation)
    {
        drop_outbox(entry, "the peer restarted before the command was sent");
        settle_open(hello.participant, entry.info.incarnation, TargetStatus::Unknown, TargetStatus::Unknown,
                    "the peer restarted");
        entry.latest.reset();
        for_subscribers(hello.participant, [&](Subscription& s) { s.drop_values(hello.participant); });
        entry.info.status = PeerStatus{};
    }
    else if (!inserted && hello.open_seqs)
    {
        // The same process again: what it no longer holds will not report a result.
        const auto& open = *hello.open_seqs;
        for (auto& [seq, op] : operations_)
        {
            if (op.finished)
            {
                continue;
            }
            bool changed = false;
            for (auto& t : op.targets)
            {
                if (t.result.peer != hello.participant || t.incarnation != hello.incarnation || is_final(t.result.status)
                    || t.handed_to == 0)
                {
                    continue;
                }
                if (!std::binary_search(open.begin(), open.end(), seq))
                {
                    t.result.status = TargetStatus::Unknown;
                    t.result.reason = "the agent no longer holds it after the reconnect";
                }
                else
                {
                    // The agent holds it: queued, and now on the new connection (the end of the
                    // old one does not touch it).
                    t.result.status = TargetStatus::Queued;
                    t.result.reason.clear();
                    t.handed_to = id;
                }
                changed = true;
            }
            if (changed)
            {
                target_changed(op);
            }
        }
    }
    if (inserted)
    {
        // Unique across the hub: an entry made again after forget_peer() is told apart from the
        // old one by a sender that was in send() meanwhile. A restart keeps the entry (and its
        // sender, which goes on with the new link).
        entry.generation = id;
    }
    old_link = std::exchange(entry.link, std::move(link));
    entry.info.participant = hello.participant;
    entry.info.incarnation = std::move(hello.incarnation);
    entry.info.schema = std::move(hello.schema);
    entry.info.connected = true;
    entry.connection = id;
    entry.max_pending = hello.max_pending;
    announce_peer(entry);
    lock.unlock();
    if (old_link)
    {
        try
        {
            old_link->close(); // the replaced connection: let its agent see the end
        }
        catch (const std::exception&)
        {
            // Best effort: the new connection is in place either way.
        }
    }
    old_link.reset();
    return id;
}

void Hub::agent_disconnected(ConnectionId id)
{
    std::shared_ptr<AgentLink> old_link;
    std::unique_lock lock(mutex_);
    const auto* c = connection(id);
    if (c == nullptr)
    {
        return;
    }
    const Connection conn = *c;
    connections_.erase(id);
    // Handed to this connection and not queued: it may or may not have reached the agent. This
    // also holds for a connection that was replaced already.
    for (auto& [seq, op] : operations_)
    {
        if (op.finished)
        {
            continue;
        }
        bool changed = false;
        for (auto& t : op.targets)
        {
            if (t.handed_to == id && t.result.status == TargetStatus::Sent)
            {
                t.result.status = TargetStatus::Unknown;
                t.result.reason = "the link was lost before the agent queued it";
                changed = true;
            }
        }
        if (changed)
        {
            target_changed(op);
        }
    }
    auto it = peers_.find(conn.participant);
    if (it != peers_.end() && it->second.connection == id)
    {
        auto& entry = it->second;
        entry.info.connected = false;
        entry.connection = 0;
        old_link = std::move(entry.link);
        drop_outbox(entry, "the link was lost before the command was sent");
        settle_open(conn.participant, conn.incarnation, TargetStatus::Queued, TargetStatus::Disconnected,
                    "the link was lost; the agent may still report the result");
        announce_peer(entry);
    }
    forget_finished();
    lock.unlock();
    old_link.reset();
}

void Hub::agent_ended(ConnectionId id, const std::string& reason)
{
    Connection conn;
    {
        const std::lock_guard lock(mutex_);
        const auto* c = connection(id);
        if (c == nullptr)
        {
            return;
        }
        conn = *c;
    }
    agent_disconnected(id);
    const std::lock_guard lock(mutex_);
    // Not if the same incarnation is connected (again, or by another connection all along), and
    // a new incarnation has settled the old one's commands itself.
    const auto it = peers_.find(conn.participant);
    if (it != peers_.end() && it->second.info.incarnation == conn.incarnation && !it->second.info.connected)
    {
        settle_open(conn.participant, conn.incarnation, TargetStatus::Unknown, TargetStatus::Unknown, reason);
        forget_finished();
    }
}

void Hub::agent_submitted(ConnectionId id, std::uint64_t seq, const SubmitResult& result)
{
    const std::lock_guard lock(mutex_);
    const auto* c = connection(id);
    if (c == nullptr)
    {
        return;
    }
    auto* t = find_target(c->participant, c->incarnation, seq);
    if (t == nullptr || t->result.status != TargetStatus::Sent)
    {
        return;
    }
    switch (result.status)
    {
    case SubmitStatus::Queued:
        t->result.status = TargetStatus::Queued;
        break;
    case SubmitStatus::Busy:
        t->result.status = TargetStatus::Busy;
        break;
    case SubmitStatus::Invalid:
        t->result.status = TargetStatus::Invalid;
        break;
    }
    t->result.reason = result.reason;
    target_changed(operations_.at(seq));
    forget_finished();
}

void Hub::agent_result(ConnectionId id, const CommandResult& result)
{
    const std::lock_guard lock(mutex_);
    const auto* c = connection(id);
    if (c == nullptr)
    {
        return;
    }
    auto* t = find_target(c->participant, c->incarnation, result.seq);
    if (t == nullptr || is_final(t->result.status))
    {
        return;
    }
    switch (result.status)
    {
    case CommandStatus::Applied:
        t->result.status = TargetStatus::Applied;
        break;
    case CommandStatus::Late:
        t->result.status = TargetStatus::Late;
        break;
    case CommandStatus::Rejected:
        t->result.status = TargetStatus::Rejected;
        break;
    case CommandStatus::Cancelled:
        t->result.status = TargetStatus::Cancelled;
        break;
    }
    t->result.applied_at = result.applied_at;
    t->result.late = result.late;
    t->result.reason = result.reason;
    target_changed(operations_.at(result.seq));
    forget_finished();
}

void Hub::agent_values(ConnectionId id, Snapshot snapshot)
{
    const std::lock_guard lock(mutex_);
    const auto* c = connection(id);
    if (c == nullptr)
    {
        return;
    }
    auto it = peers_.find(c->participant);
    if (it == peers_.end() || it->second.connection != id)
    {
        return;
    }
    auto& entry = it->second;
    if (snapshot.values.size() != entry.info.schema.size()
        || (entry.latest && snapshot.sequence <= entry.latest->sequence))
    {
        return; // not for this schema, or not newer
    }
    entry.latest = std::make_shared<const Snapshot>(std::move(snapshot));
    if (values_tap_)
    {
        values_tap_(entry.info.participant, entry.info.incarnation, entry.latest);
    }
    for_subscribers(c->participant, [&](Subscription& s) {
        if (s.options_.values)
        {
            s.put_values(entry.info.participant, entry.info.incarnation, entry.latest);
        }
    });
}

void Hub::set_values_tap(ValuesTap tap)
{
    const std::lock_guard lock(mutex_);
    values_tap_ = std::move(tap);
}

void Hub::agent_events(ConnectionId id, const std::vector<Event>& events)
{
    const std::lock_guard lock(mutex_);
    const auto* c = connection(id);
    if (c == nullptr)
    {
        return;
    }
    auto it = peers_.find(c->participant);
    if (it == peers_.end() || it->second.connection != id)
    {
        return;
    }
    for_subscribers(c->participant, [&](Subscription& s) {
        if (s.options_.events)
        {
            for (const auto& e : events)
            {
                s.put_event(EventUpdate{c->participant, c->incarnation, e});
            }
        }
    });
}

void Hub::agent_status(ConnectionId id, PeerStatus status)
{
    const std::lock_guard lock(mutex_);
    const auto* c = connection(id);
    if (c == nullptr)
    {
        return;
    }
    auto it = peers_.find(c->participant);
    if (it == peers_.end() || it->second.connection != id)
    {
        return;
    }
    if (it->second.info.status.failed == status.failed && it->second.info.status.error == status.error)
    {
        return;
    }
    it->second.info.status = std::move(status);
    announce_peer(it->second);
}

std::vector<PeerInfo> Hub::peers() const
{
    const std::lock_guard lock(mutex_);
    std::vector<PeerInfo> out;
    for (const auto& [name, entry] : peers_)
    {
        out.push_back(entry.info);
    }
    return out;
}

std::shared_ptr<const Snapshot> Hub::latest(const std::string& peer) const
{
    const std::lock_guard lock(mutex_);
    const auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : it->second.latest;
}

std::vector<LatestValues> Hub::latest_of(const std::vector<std::string>& peers) const
{
    std::vector<LatestValues> out;
    // The strings are made outside the lock where they can be: the transport's handler waits for it.
    out.reserve(peers.size());
    for (const auto& name : peers)
    {
        const bool again = std::any_of(out.begin(), out.end(), [&](const LatestValues& v) { return v.participant == name; });
        if (!again)
        {
            out.push_back(LatestValues{name, {}, nullptr});
        }
    }
    const std::lock_guard lock(mutex_);
    if (peers.empty())
    {
        out.reserve(peers_.size());
        for (const auto& [name, entry] : peers_)
        {
            out.push_back(LatestValues{name, entry.info.incarnation, entry.latest});
        }
        return out;
    }
    for (auto& values : out)
    {
        const auto it = peers_.find(values.participant);
        if (it != peers_.end())
        {
            values.incarnation = it->second.info.incarnation;
            values.snapshot = it->second.latest;
        }
    }
    return out;
}

bool none_after(const std::vector<LatestValues>& values, SimTime now) noexcept
{
    return std::none_of(values.begin(), values.end(),
                        [&](const LatestValues& v) { return v.snapshot != nullptr && v.snapshot->time > now; });
}

bool Hub::forget_peer(const std::string& peer)
{
    const std::lock_guard lock(mutex_);
    const auto it = peers_.find(peer);
    if (it == peers_.end() || it->second.info.connected)
    {
        return false;
    }
    settle_open(peer, it->second.info.incarnation, TargetStatus::Unknown, TargetStatus::Unknown, "the peer was forgotten");
    for_subscribers(peer, [&](Subscription& s) { s.put_peer(it->second.info, true); });
    peers_.erase(it);
    forget_finished();
    return true;
}

std::uint64_t Hub::write(OperationRequest request)
{
    if (request.writes.empty())
    {
        throw std::invalid_argument("Hub::write() needs at least one write");
    }
    Operation op;
    op.request = std::move(request);
    // Group by peer, keeping the order in which the peers and their writes appear. Done before
    // taking the lock: its cost grows with the request, which comes from outside.
    {
        std::unordered_map<std::string_view, std::size_t> index; // peer -> its target
        for (const auto& w : op.request.writes)
        {
            const auto [it, inserted] = index.try_emplace(w.peer, op.targets.size());
            if (inserted)
            {
                Target target;
                target.result.peer = w.peer;
                target.command = Command{0, {}, op.request.at, op.request.late};
                op.targets.push_back(std::move(target));
            }
            op.targets[it->second].command.writes.push_back({w.name, w.value});
        }
    }
    std::unique_lock lock(mutex_);
    const auto seq = next_seq_++;
    op.seq = seq;
    for (auto& t : op.targets)
    {
        t.command.seq = seq;
    }
    std::vector<std::string> to_flush;
    for (auto& t : op.targets)
    {
        const auto entry = peers_.find(t.result.peer);
        std::string why;
        auto status = TargetStatus::Invalid;
        if (entry == peers_.end() || !entry->second.info.connected)
        {
            why = "the peer is not connected";
        }
        else
        {
            t.incarnation = entry->second.info.incarnation;
            const auto& schema = entry->second.info.schema;
            for (const auto& w : t.command.writes)
            {
                const auto v = std::find_if(schema.begin(), schema.end(), [&](const VariableInfo& i) { return i.name == w.name; });
                if (v == schema.end())
                {
                    why = "unknown variable '" + w.name + "'";
                    break;
                }
                if (v->causality == Causality::Output)
                {
                    why = w.name + ": an output is not writable";
                    break;
                }
            }
            if (why.empty() && open_targets(t.result.peer) >= entry->second.max_pending)
            {
                why = "the peer has too many commands in flight";
                status = TargetStatus::Busy;
            }
        }
        if (why.empty())
        {
            t.result.status = TargetStatus::Sent;
            entry->second.outbox.push_back(seq);
            to_flush.push_back(t.result.peer);
        }
        else
        {
            t.result.status = status;
            t.result.reason = std::move(why);
        }
    }
    auto [it, inserted] = operations_.emplace(seq, std::move(op));
    target_changed(it->second);
    forget_finished();
    for (const auto& peer : to_flush)
    {
        flush(peer, lock);
    }
    return seq;
}

void Hub::flush(const std::string& participant, std::unique_lock<std::mutex>& lock)
{
    while (true)
    {
        auto it = peers_.find(participant);
        if (it == peers_.end() || it->second.sending || it->second.outbox.empty() || !it->second.link)
        {
            return; // another thread sends, or nothing to send
        }
        auto& entry = it->second;
        const auto seq = entry.outbox.front();
        entry.outbox.pop_front();
        auto* t = find_target(participant, entry.info.incarnation, seq);
        if (t == nullptr || t->result.status != TargetStatus::Sent || t->handed_to != 0)
        {
            continue;
        }
        const Command command = t->command;
        auto link = entry.link;
        const auto incarnation = entry.info.incarnation;
        const auto connection = entry.connection;
        const auto generation = entry.generation;
        t->handed_to = connection; // a disconnect of it during send() settles the command
        entry.sending = true;
        lock.unlock();
        enum class Outcome
        {
            Sent,
            Refused,
            Threw,
        } outcome = Outcome::Refused;
        try
        {
            outcome = link->send(command) ? Outcome::Sent : Outcome::Refused;
        }
        catch (...)
        {
            outcome = Outcome::Threw;
        }
        link.reset(); // never the last reference under the lock
        lock.lock();
        auto again = peers_.find(participant);
        const bool same_entry = again != peers_.end() && again->second.generation == generation;
        if (same_entry)
        {
            again->second.sending = false;
        }
        if (auto* target = find_target(participant, incarnation, seq))
        {
            if (outcome == Outcome::Sent)
            {
                target->command = Command{}; // no longer needed
            }
            else if (target->result.status == TargetStatus::Sent)
            {
                target->result.status = outcome == Outcome::Refused ? TargetStatus::Invalid : TargetStatus::Unknown;
                target->result.reason = outcome == Outcome::Refused ? "the link refused the command"
                                                                    : "the link failed while sending the command";
                target_changed(operations_.at(seq));
                forget_finished();
            }
        }
        if (!same_entry)
        {
            return; // the peer was replaced meanwhile: its own sender takes over
        }
    }
}

std::optional<OperationResult> Hub::operation(std::uint64_t seq) const
{
    const std::lock_guard lock(mutex_);
    const auto it = operations_.find(seq);
    if (it == operations_.end())
    {
        return std::nullopt;
    }
    return view(it->second);
}

std::optional<OperationResult> Hub::wait_queued(std::uint64_t seq, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    std::optional<OperationResult> result;
    changed_.wait_for(lock, timeout, [&] {
        const auto it = operations_.find(seq);
        if (it == operations_.end())
        {
            return true; // unknown: nothing to wait for
        }
        if (std::none_of(it->second.targets.begin(), it->second.targets.end(),
                         [](const Target& t) { return t.result.status == TargetStatus::Sent; }))
        {
            result = view(it->second);
            return true;
        }
        return false;
    });
    return result;
}

std::optional<OperationResult> Hub::wait_done(std::uint64_t seq, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    std::optional<OperationResult> result;
    changed_.wait_for(lock, timeout, [&] {
        const auto it = operations_.find(seq);
        if (it == operations_.end())
        {
            return true;
        }
        auto v = view(it->second);
        if (v.done())
        {
            result = std::move(v);
            return true;
        }
        return false;
    });
    return result;
}

std::shared_ptr<Subscription> Hub::subscribe(SubscriptionOptions options)
{
    if (options.capacity == 0)
    {
        throw std::invalid_argument("SubscriptionOptions capacity must be > 0");
    }
    std::shared_ptr<Subscription> sub(new Subscription(std::move(options)));
    const std::lock_guard lock(mutex_);
    std::erase_if(subscriptions_, [](const std::weak_ptr<Subscription>& w) { return w.expired(); });
    subscriptions_.push_back(sub);
    for (const auto& [name, entry] : peers_)
    {
        if (sub->wants(name))
        {
            sub->put_peer(entry.info);
            if (sub->options_.values && entry.latest)
            {
                sub->put_values(name, entry.info.incarnation, entry.latest);
            }
        }
    }
    return sub;
}

} // namespace vehicle_sil::control
