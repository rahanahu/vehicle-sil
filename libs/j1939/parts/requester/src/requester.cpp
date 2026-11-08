// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/requester.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] RequesterOptions checked(const RequesterOptions& options)
{
    if (options.timeout <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::Requester: timeout must be > 0");
    }
    if (options.min_interval < SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::Requester: min_interval must be >= 0");
    }
    if (options.capacity == 0)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Requester: capacity must be > 0");
    }
    return options;
}

} // namespace

Requester& Requester::of(Node& node)
{
    if (auto* existing = node.find<Requester>())
    {
        return *existing;
    }
    return node.attach<Requester>(Key{}, RequesterOptions{});
}

Requester& Requester::attach(Node& node, RequesterOptions options)
{
    return node.attach<Requester>(Key{}, options);
}

Requester::Requester(Node& node, Key /*key*/, RequesterOptions options)
  : node_(node),
    options_(checked(options)),
    transactions_(options_.capacity),
    waiters_(options_.capacity),
    recent_(options_.min_interval > SimDuration::zero() ? options_.capacity : 0),
    send_(node, TimerKind::Scheduled, [this] { on_send_timer(); }),
    wait_(node, TimerKind::Timeout, [this] { on_wait_timer(); })
{
    if (node.find<Requester>() != nullptr)
    {
        throw std::logic_error("vehicle_sil::j1939::Requester: the node already has one (attach it before using of())");
    }
    messages_ = Subscription{node, node.on_any_message([this](const Message& m) { on_message(m); })};
    address_ = Subscription{node, node.on_address_change([this](const AddressChange& c) { on_address(c); })};
}

RequestId Requester::request(Pgn pgn, Address destination, RequestCallback callback)
{
    if (!pgn.is_valid() || destination == kNullAddress || !callback)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Requester: invalid PGN, destination kNullAddress or empty callback");
    }
    const auto freeWaiter = std::find_if(waiters_.begin(), waiters_.end(), [](const Waiter& w) { return w.id == 0; });
    if (freeWaiter == waiters_.end())
    {
        ++statistics_.refused;
        return {};
    }
    std::size_t index = transactions_.size();
    for (std::size_t i = 0; i < transactions_.size(); ++i)
    {
        const Transaction& t = transactions_[i];
        if (t.phase != Phase::Free && t.pgn == pgn && t.destination == destination)
        {
            index = i;
            ++statistics_.joined;
            break;
        }
    }
    if (index == transactions_.size())
    {
        // A transaction has at least one waiter, so a free waiter means a free transaction.
        const auto freeTransaction = std::find_if(transactions_.begin(), transactions_.end(),
                                                  [](const Transaction& t) { return t.phase == Phase::Free; });
        index = static_cast<std::size_t>(freeTransaction - transactions_.begin());
        Transaction& t = *freeTransaction;
        t.phase = Phase::Waiting;
        t.pgn = pgn;
        t.destination = destination;
        t.time = earliest_send(pgn, destination, node_.network().event_time());
        t.attempts = 0;
        t.timeouts = 0;
        t.serial = next_serial_++;
        ++t.generation;
    }
    const RequestId id{next_id_++};
    *freeWaiter = Waiter{id.value, index, transactions_[index].generation, std::move(callback)};
    arm();
    return id;
}

bool Requester::cancel(RequestId id) noexcept
{
    if (!id)
    {
        return false;
    }
    for (Waiter& w : waiters_)
    {
        if (w.id != id.value)
        {
            continue;
        }
        const std::size_t index = w.transaction;
        const std::uint32_t generation = w.generation;
        w.id = 0;
        w.callback = nullptr;
        const bool others = std::any_of(waiters_.begin(), waiters_.end(), [&](const Waiter& o) {
            return o.id != 0 && o.transaction == index && o.generation == generation;
        });
        if (!others && transactions_[index].generation == generation)
        {
            // Its timers may fire for nothing; they find nothing due and are set again.
            transactions_[index].phase = Phase::Free;
        }
        return true;
    }
    return false;
}

std::size_t Requester::pending() const noexcept
{
    return static_cast<std::size_t>(std::count_if(waiters_.begin(), waiters_.end(), [](const Waiter& w) { return w.id != 0; }));
}


SimTime Requester::earliest_send(Pgn pgn, Address destination, SimTime from) const noexcept
{
    for (const Recent& r : recent_)
    {
        if (r.used && r.pgn == pgn && r.destination == destination)
        {
            return std::max(from, r.sent + options_.min_interval);
        }
    }
    return from;
}

void Requester::remember_sent(Pgn pgn, Address destination, SimTime time) noexcept
{
    if (recent_.empty())
    {
        return;
    }
    Recent* slot = nullptr;
    for (Recent& r : recent_)
    {
        if (r.used && r.pgn == pgn && r.destination == destination)
        {
            slot = &r;
            break;
        }
        // Otherwise a free one, or the oldest (an interval long past no longer matters).
        if (slot == nullptr || !r.used || (slot->used && r.sent < slot->sent))
        {
            slot = &r;
        }
    }
    *slot = Recent{pgn, destination, time, true};
}

void Requester::arm()
{
    std::optional<SimTime> send;
    std::optional<SimTime> wait;
    for (const Transaction& t : transactions_)
    {
        if (t.phase == Phase::Waiting && (!send || t.time < *send))
        {
            send = t.time;
        }
        else if (t.phase == Phase::Sent && (!wait || t.time < *wait))
        {
            wait = t.time;
        }
    }
    // Without an address nothing is sent: the address handler arms again. A node that cannot
    // claim one ends the waiting requests (on_send_timer()).
    if (send && (node_.can_send_now() || node_.address_state() == AddressState::CannotClaim))
    {
        if (send_.deadline() != send || !send_.running())
        {
            send_.start_at(*send);
        }
    }
    else
    {
        send_.cancel();
    }
    if (wait)
    {
        if (wait_.deadline() != wait || !wait_.running())
        {
            wait_.start_at(*wait);
        }
    }
    else
    {
        wait_.cancel();
    }
}

void Requester::on_send_timer()
{
    const SimTime deadline = *send_.deadline();
    if (node_.can_send_now() || node_.address_state() == AddressState::CannotClaim)
    {
        const std::uint64_t limit = next_serial_; // not the ones a callback adds
        for (std::size_t i = 0; i < transactions_.size(); ++i)
        {
            const Transaction& t = transactions_[i];
            if (t.phase == Phase::Waiting && t.serial < limit && t.time <= deadline)
            {
                send(i);
            }
        }
    }
    arm();
}

void Requester::on_wait_timer()
{
    const SimTime deadline = *wait_.deadline();
    const SimTime now = *node_.network().now();
    const std::uint64_t limit = next_serial_;
    for (std::size_t i = 0; i < transactions_.size(); ++i)
    {
        Transaction& t = transactions_[i];
        if (t.phase != Phase::Sent || t.serial >= limit || t.time > deadline)
        {
            continue;
        }
        if (t.destination != kGlobalAddress && t.timeouts < options_.retries)
        {
            ++t.timeouts;
            t.phase = Phase::Waiting;
            t.time = earliest_send(t.pgn, t.destination, deadline);
            if (t.time <= now && node_.can_send_now())
            {
                send(i); // in this step, not the next one
            }
            continue;
        }
        RequestResult result;
        result.outcome = RequestOutcome::Timeout;
        result.time = deadline;
        report(i, result);
    }
    arm();
}

void Requester::send(std::size_t index)
{
    Transaction& t = transactions_[index];
    const SimTime now = *node_.network().now();
    const SendStatus status =
        node_.can_send_now() ? node_.request(t.pgn, t.destination) : SendStatus::NoAddress; // CannotClaim
    if (status == SendStatus::Ok)
    {
        t.phase = Phase::Sent;
        ++t.attempts;
        t.time = now + options_.timeout; // the Request leaves in this step's flush
        ++statistics_.requests_sent;
        remember_sent(t.pgn, t.destination, now);
        return;
    }
    RequestResult result;
    result.outcome = RequestOutcome::NotSent;
    result.send_status = status;
    result.time = now;
    report(index, result);
}

void Requester::on_message(const Message& message)
{
    std::optional<Acknowledgement> ack;
    if (message.pgn == kPgnAcknowledgement)
    {
        ack = parse_acknowledgement(message);
        if (!ack || ack->address != node_.address())
        {
            return; // for another node
        }
    }
    const Pgn answered = ack ? ack->pgn : message.pgn;
    const std::uint64_t limit = next_serial_;
    bool reported = false;
    for (std::size_t i = 0; i < transactions_.size(); ++i)
    {
        const Transaction& t = transactions_[i];
        if (t.phase == Phase::Free || t.serial >= limit || t.pgn != answered ||
            (t.destination != kGlobalAddress && t.destination != message.source))
        {
            continue;
        }
        RequestResult result;
        result.outcome = ack ? RequestOutcome::Acknowledged : RequestOutcome::Response;
        result.source = message.source;
        if (ack)
        {
            result.ack = ack->code;
        }
        else
        {
            result.data = message.data;
        }
        result.last = t.destination != kGlobalAddress;
        result.time = message.time;
        report(i, result);
        reported = true;
    }
    if (reported)
    {
        arm();
    }
}

void Requester::on_address(const AddressChange& change)
{
    if (!node_.can_send_now() || change.address != change.previous_address)
    {
        // The answers to what was sent would go to the old address: send it again later.
        for (Transaction& t : transactions_)
        {
            if (t.phase == Phase::Sent)
            {
                t.phase = Phase::Waiting;
                t.time = earliest_send(t.pgn, t.destination, change.time);
            }
        }
    }
    if (node_.can_send_now())
    {
        // The handler runs in the step (often in the timer that ended the wait after the claim,
        // where a Timer started now would wait for the next step): send what is due at once.
        const std::uint64_t limit = next_serial_;
        for (std::size_t i = 0; i < transactions_.size(); ++i)
        {
            const Transaction& t = transactions_[i];
            if (t.phase == Phase::Waiting && t.serial < limit && t.time <= change.time)
            {
                send(i);
            }
        }
    }
    arm();
}

void Requester::report(std::size_t index, RequestResult result)
{
    Transaction& t = transactions_[index];
    result.pgn = t.pgn;
    result.destination = t.destination;
    result.attempts = t.attempts;
    const std::uint32_t generation = t.generation;
    if (result.last)
    {
        t.phase = Phase::Free; // before the callbacks, which may request again
    }
    const std::uint64_t limit = next_id_; // not the requests a callback adds
    for (Waiter& w : waiters_)
    {
        if (w.id == 0 || w.id >= limit || w.transaction != index || w.generation != generation)
        {
            continue;
        }
        const std::uint64_t id = w.id;
        RequestCallback callback = std::move(w.callback);
        w.callback = nullptr;
        if (result.last)
        {
            w.id = 0;
        }
        callback(result);
        // A response to all nodes: the request goes on, unless the callback cancelled it.
        if (!result.last && w.id == id)
        {
            w.callback = std::move(callback);
        }
    }
}

} // namespace vehicle_sil::j1939
