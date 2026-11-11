// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/exchange.hpp"

#include "vehicle_sil/j1939/detail/user_call.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] ExchangeOptions checked(const ExchangeOptions& options)
{
    if (options.timeout <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::Exchange: timeout must be > 0");
    }
    if (options.capacity == 0)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Exchange: capacity must be > 0");
    }
    if (options.answer_transfer_limit <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::Exchange: answer_transfer_limit must be > 0");
    }
    if (options.max_length == 0 || options.max_length > kMaxTransportLength)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Exchange: max_length must be 1 .. kMaxTransportLength");
    }
    return options;
}

} // namespace

Exchange::Exchange(Node& node, ExchangeOptions options)
  : node_(node),
    options_(checked(options)),
    slots_(options_.capacity),
    send_(node, TimerKind::Scheduled, [this] { static_cast<void>(on_send_timer()); }),
    wait_(node, TimerKind::Timeout, [this] { static_cast<void>(on_wait_timer()); })
{
    for (Slot& s : slots_)
    {
        s.command.resize(options_.max_length);
    }
    messages_ = Subscription{node, node.on_any_message([this](const Message& m) { static_cast<void>(on_message(m)); })};
    address_ = Subscription{node, node.on_address_change([this](const AddressChange& c) { static_cast<void>(on_address(c)); })};
    transfers_ = Subscription{node, node.on_transfer_end([this](const TransferEnd& e) { static_cast<void>(on_transfer_end(e)); })};
    incoming_ = Subscription{node, node.on_incoming_transfer([this](const IncomingTransfer& t) { on_incoming(t); })};
}

Exchange::~Exchange() { detail::mark_gone(gone_); }

ExchangeId Exchange::start(Pgn pgn, std::span<const std::uint8_t> command, Address peer, ReplyMatcher match,
                           ExchangeCallback done, CommandOptions options)
{
    if (!is_unicast(peer))
    {
        throw std::invalid_argument("vehicle_sil::j1939::Exchange: the counterpart's address must be 0..253");
    }
    return add(pgn, command, peer, std::nullopt, std::move(match), std::move(done), options);
}

ExchangeId Exchange::start(Pgn pgn, std::span<const std::uint8_t> command, Name peer, ReplyMatcher match,
                           ExchangeCallback done, CommandOptions options)
{
    return add(pgn, command, kNullAddress, peer, std::move(match), std::move(done), options);
}

ExchangeId Exchange::add(Pgn pgn, std::span<const std::uint8_t> command, Address to, std::optional<Name> toName,
                         ReplyMatcher match, ExchangeCallback done, const CommandOptions& options)
{
    if (!messages_.active())
    {
        throw std::logic_error("vehicle_sil::j1939::Exchange: its network was destroyed");
    }
    if (!pgn.is_valid() || options.priority > kMaxPriority || command.empty() || command.size() > options_.max_length ||
        !match || !done || (options.long_answer && !options.long_answer->is_valid()) ||
        (options.timeout && *options.timeout <= SimDuration::zero()))
    {
        throw std::invalid_argument("vehicle_sil::j1939::Exchange: invalid PGN, long_answer, priority or timeout, "
                                    "empty command or one longer than max_length, or empty matcher or callback");
    }
    const auto free = std::find_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.phase == Phase::Free; });
    if (free == slots_.end())
    {
        ++statistics_.refused;
        return {};
    }
    Slot& s = *free;
    s.phase = Phase::Waiting;
    s.id = next_id_++;
    s.pgn = pgn;
    s.priority = options.priority;
    s.timeout = options.timeout.value_or(options_.timeout);
    s.retries = options.retries.value_or(options_.retries);
    s.on_address_change = options.on_address_change;
    s.unconfirmed = false;
    s.transfer_cancelled = false;
    std::copy(command.begin(), command.end(), s.command.begin());
    s.length = command.size();
    s.to = to;
    s.to_name = toName;
    s.peer = kNullAddress;
    s.peer_name.reset();
    s.time = node_.network().event_time();
    s.attempts = 0;
    s.failures = 0;
    s.pending = false;
    s.transfer = {};
    s.resolve_by.reset();
    s.long_answer = options.long_answer;
    new_attempt(s);
    s.match = std::move(match);
    s.done = std::move(done);
    try
    {
        arm();
    }
    catch (...)
    {
        s.phase = Phase::Free;
        s.match = nullptr;
        s.done = nullptr;
        throw;
    }
    return ExchangeId{s.id};
}

bool Exchange::cancel(ExchangeId id) noexcept
{
    if (!id)
    {
        return false;
    }
    for (Slot& s : slots_)
    {
        if (s.phase != Phase::Free && s.id == id.value)
        {
            // Its timers may fire for nothing; they find nothing due and are set again.
            s.phase = Phase::Free;
            s.match = nullptr;
            s.done = nullptr;
            return true;
        }
    }
    return false;
}

void Exchange::cancel_all() noexcept
{
    for (Slot& s : slots_)
    {
        s.phase = Phase::Free;
        s.match = nullptr;
        s.done = nullptr;
    }
}

bool Exchange::running(ExchangeId id) const noexcept
{
    return id && std::any_of(slots_.begin(), slots_.end(),
                             [&](const Slot& s) { return s.phase != Phase::Free && s.id == id.value; });
}

std::size_t Exchange::pending() const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.phase != Phase::Free; }));
}

Exchange::Slot* Exchange::next_slot(std::uint64_t after, std::uint64_t limit) noexcept
{
    Slot* next = nullptr;
    for (Slot& s : slots_)
    {
        if (s.phase != Phase::Free && s.id > after && s.id < limit && (next == nullptr || s.id < next->id))
        {
            next = &s;
        }
    }
    return next;
}

void Exchange::arm()
{
    std::optional<SimTime> send;
    std::optional<SimTime> wait;
    for (const Slot& s : slots_)
    {
        if (s.phase == Phase::Waiting && (!send || s.time < *send))
        {
            send = s.time;
        }
        else if ((s.phase == Phase::Sent || s.phase == Phase::Resolving) && (!wait || s.time < *wait))
        {
            wait = s.time;
        }
    }
    // Without an address nothing is sent: the address handler arms again. A node that cannot
    // claim one ends the waiting exchanges (send()).
    if (send && (node_.can_send_now() || node_.address_state() == AddressState::CannotClaim))
    {
        if (!send_.running() || send_.deadline() != send)
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
        if (!wait_.running() || wait_.deadline() != wait)
        {
            wait_.start_at(*wait);
        }
    }
    else
    {
        wait_.cancel();
    }
}

bool Exchange::on_send_timer()
{
    if (!send_due(*send_.deadline()))
    {
        return false;
    }
    arm();
    return true;
}

bool Exchange::send_due(SimTime upTo)
{
    if (!node_.can_send_now() && node_.address_state() != AddressState::CannotClaim)
    {
        return true;
    }
    const std::uint64_t limit = next_id_; // not the ones a callback adds
    std::uint64_t after = 0;
    while (Slot* s = next_slot(after, limit))
    {
        after = s->id;
        if (s->phase == Phase::Waiting && s->time <= upTo && !send(*s))
        {
            return false;
        }
    }
    return true;
}

bool Exchange::send(Slot& slot)
{
    Network& network = node_.network();
    const SimTime now = *network.now();
    ExchangeResult result;
    result.time = now;
    if (slot.unconfirmed && slot.on_address_change == AddressChangePolicy::Stop)
    {
        result.outcome = ExchangeOutcome::Interrupted; // its transfer was cancelled after its last packet
        result.transfer = slot.transfer_cancelled ? TransferResult::Cancelled : TransferResult::Completed;
        return finish(slot, result);
    }
    if (!node_.can_send_now())
    {
        // CannotClaim: a command sent before may have been carried out.
        result.outcome = slot.attempts == 0 ? ExchangeOutcome::NotSent : ExchangeOutcome::Interrupted;
        result.send_status = SendStatus::NoAddress;
        return finish(slot, result);
    }
    if (slot.to_name)
    {
        const auto address = network.address_of(*slot.to_name);
        if (!address && slot.attempts == 0)
        {
            // Not claimed yet (e.g. at start-up): wait for its Address Claimed (on_message()).
            slot.phase = Phase::Resolving;
            if (!slot.resolve_by) // keep the first deadline when the node's address changed meanwhile
            {
                slot.resolve_by = now + slot.timeout;
            }
            slot.time = *slot.resolve_by;
            return true;
        }
        if (!address)
        {
            result.outcome = ExchangeOutcome::PeerLost; // it was there for the last attempt
            return finish(slot, result);
        }
        slot.peer = *address;
        slot.peer_name = slot.to_name;
    }
    else
    {
        if (slot.attempts == 0)
        {
            slot.peer_name = network.name_at(slot.to);
        }
        slot.peer = slot.to;
        if (peer_lost(slot))
        {
            result.outcome = ExchangeOutcome::PeerLost;
            return finish(slot, result);
        }
    }
    const bool transfer = slot.length > 8;
    const Address destination = slot.pgn.is_pdu1() ? slot.peer : kGlobalAddress;
    const SendStatus status =
        node_.send(slot.pgn, std::span<const std::uint8_t>(slot.command.data(), slot.length), destination,
                   slot.priority, transfer ? WhenBusy::Queue : WhenBusy::Fail);
    if (status != SendStatus::Ok)
    {
        // Refused now; an attempt sent before may still have reached the counterpart.
        result.outcome = slot.attempts == 0 ? ExchangeOutcome::NotSent : ExchangeOutcome::Interrupted;
        result.send_status = status;
        return finish(slot, result);
    }
    ++slot.attempts;
    slot.unconfirmed = false;
    slot.transfer_cancelled = false;
    new_attempt(slot);
    ++statistics_.sent;
    if (transfer)
    {
        slot.phase = Phase::Transferring;
        slot.transfer = node_.last_transfer_id();
    }
    else
    {
        slot.phase = Phase::Sent;
        slot.time = now + slot.timeout; // the command leaves in this step's flush
    }
    return true;
}

bool Exchange::on_wait_timer()
{
    const SimTime deadline = *wait_.deadline();
    const std::uint64_t limit = next_id_;
    std::uint64_t after = 0;
    while (Slot* s = next_slot(after, limit))
    {
        after = s->id;
        if ((s->phase != Phase::Sent && s->phase != Phase::Resolving) || s->time > deadline)
        {
            continue;
        }
        if (s->phase == Phase::Resolving)
        {
            ExchangeResult result;
            result.outcome = ExchangeOutcome::PeerLost; // its NAME did not appear
            result.time = deadline;
            if (!finish(*s, result))
            {
                return false;
            }
            continue;
        }
        if (!s->pending && s->failures < s->retries)
        {
            ++s->failures;
            s->phase = Phase::Waiting;
            s->time = deadline;
            if (node_.can_send_now() && !send(*s)) // in this step, not the next one
            {
                return false;
            }
            continue;
        }
        ExchangeResult result;
        result.outcome = ExchangeOutcome::Timeout;
        result.time = deadline;
        if (!finish(*s, result))
        {
            return false;
        }
    }
    arm();
    return true;
}

bool Exchange::on_transfer_end(const TransferEnd& end)
{
    Slot* slot = nullptr;
    for (Slot& s : slots_)
    {
        if (s.phase == Phase::Transferring && s.transfer == end.id)
        {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr)
    {
        return true; // another sender's, or one whose exchange ended (answered, cancelled)
    }
    slot->transfer = {};
    if (end.result == TransferResult::Completed)
    {
        slot->phase = Phase::Sent;
        slot->time = held(*slot, end.time + slot->timeout);
    }
    else if (end.result == TransferResult::Cancelled)
    {
        // The node's or the counterpart's address changed (the address change or the counterpart's
        // Address Claimed comes next and decides), or, rarely, the held transfer could not start.
        // Before its last packet the counterpart cannot have it: it simply waits to be sent again.
        slot->phase = Phase::Waiting;
        slot->unconfirmed = end.all_packets_sent;
        slot->transfer_cancelled = end.all_packets_sent;
        slot->time = end.time;
    }
    else if (!slot->pending && slot->failures < slot->retries)
    {
        ++slot->failures;
        slot->phase = Phase::Waiting;
        slot->time = end.time;
    }
    else
    {
        ExchangeResult result;
        // Refused by the counterpart: not carried out. Given up by this node (no clear-to-send
        // or no acknowledgement of the last packets): it may have the whole command.
        result.outcome = end.result == TransferResult::Aborted && end.all_packets_sent ? ExchangeOutcome::Interrupted
                                                                                        : ExchangeOutcome::NotSent;
        result.transfer = end.result;
        result.reason = end.reason;
        result.time = end.time;
        if (!finish(*slot, result))
        {
            return false;
        }
    }
    arm();
    return true;
}

SimTime Exchange::held(const Slot& slot, SimTime base) noexcept
{
    return slot.receiving && slot.hold_until ? std::max(base, *slot.hold_until) : base;
}

void Exchange::new_attempt(Slot& slot) noexcept
{
    slot.receiving = {};
    slot.hold_until.reset();
}

void Exchange::on_incoming(const IncomingTransfer& transfer)
{
    // No user code here: only the deadlines change.
    bool changed = false;
    for (Slot& s : slots_)
    {
        if (s.phase != Phase::Sent && s.phase != Phase::Transferring)
        {
            continue;
        }
        if (transfer.stage == TransferStage::Started)
        {
            if (!s.long_answer || *s.long_answer != transfer.pgn || transfer.source != s.peer)
            {
                continue; // another group or another sender: does not hold the wait
            }
            if (!s.hold_until)
            {
                s.hold_until = transfer.time + options_.answer_transfer_limit;
            }
            if (transfer.time >= *s.hold_until)
            {
                continue; // the attempt held its wait long enough
            }
            s.receiving = transfer.id;
            if (s.phase == Phase::Sent)
            {
                s.time = held(s, s.time);
                changed = true;
            }
        }
        else if (s.receiving && s.receiving == transfer.id)
        {
            s.receiving = {};
            // After the limit the transfers do not move the wait any more (it ends at most
            // `timeout` after the limit).
            if (s.phase == Phase::Sent && transfer.time < *s.hold_until)
            {
                // Completed: its message comes right after this; otherwise wait again from here.
                s.time = transfer.time + s.timeout;
                changed = true;
            }
        }
    }
    if (changed)
    {
        arm();
    }
}

bool Exchange::peer_lost(const Slot& slot) const noexcept
{
    if (!slot.peer_name)
    {
        return false;
    }
    const auto address = node_.network().address_of(*slot.peer_name);
    return !address || *address != slot.peer;
}

bool Exchange::on_message(const Message& message)
{
    const std::uint64_t limit = next_id_; // not the exchanges a callback starts
    bool changed = false;
    if (message.pgn == kPgnAddressClaimed)
    {
        // The address table (and the local nodes) already show the claim.
        bool resolved = false;
        std::uint64_t after = 0;
        while (Slot* s = next_slot(after, limit))
        {
            after = s->id;
            if (s->phase == Phase::Resolving && node_.network().address_of(*s->to_name))
            {
                s->phase = Phase::Waiting;
                s->time = message.time;
                resolved = true;
                continue;
            }
            if (s->attempts > 0 && peer_lost(*s))
            {
                ExchangeResult result;
                result.outcome = ExchangeOutcome::PeerLost;
                result.time = message.time;
                if (!finish(*s, result))
                {
                    return false;
                }
                changed = true;
            }
        }
        if (resolved && !send_due(message.time))
        {
            return false;
        }
        changed = changed || resolved;
    }
    std::uint64_t after = 0;
    while (Slot* s = next_slot(after, limit))
    {
        after = s->id;
        // Answers come after the command was sent (also while it waits to be sent again).
        if (s->attempts == 0 || s->peer != message.source)
        {
            continue;
        }
        const std::uint64_t id = s->id;
        // Held here while it runs: it may cancel its exchange or destroy the Exchange.
        ReplyMatcher match = std::move(s->match);
        ReplyMatch verdict = ReplyMatch::Ignore;
        {
            const detail::UserCall guard(gone_);
            verdict = match(message);
            if (guard.gone())
            {
                return false;
            }
        }
        if (s->phase == Phase::Free || s->id != id)
        {
            continue; // cancelled by its matcher (the slot may hold a newer exchange now)
        }
        s->match = std::move(match);
        if (verdict == ReplyMatch::Ignore)
        {
            continue;
        }
        changed = true;
        if (verdict == ReplyMatch::Pending)
        {
            s->pending = true;
            s->phase = Phase::Sent;
            s->transfer = {};
            s->time = held(*s, message.time + s->timeout);
            break;
        }
        ExchangeResult result;
        result.outcome = ExchangeOutcome::Reply;
        result.reply = message;
        result.time = message.time;
        if (!finish(*s, result))
        {
            return false;
        }
        break; // one answer for one exchange
    }
    if (changed)
    {
        arm();
    }
    return true;
}

bool Exchange::on_address(const AddressChange& change)
{
    if (!node_.can_send_now() || change.address != change.previous_address)
    {
        // The answers to what was sent would go to the old address: what was sent is
        // unconfirmed (sent again or Interrupted); what was not sent yet waits.
        for (Slot& s : slots_)
        {
            if (s.phase == Phase::Sent || s.phase == Phase::Transferring || s.phase == Phase::Resolving)
            {
                s.unconfirmed = s.unconfirmed || s.phase != Phase::Resolving;
                s.phase = Phase::Waiting;
                s.transfer = {};
                new_attempt(s); // a long answer to the old address is not this node's any more
                s.time = change.time;
            }
        }
        const std::uint64_t limit = next_id_; // not the exchanges a callback starts
        std::uint64_t after = 0;
        while (Slot* s = next_slot(after, limit))
        {
            after = s->id;
            if (s->phase == Phase::Waiting && s->unconfirmed && s->on_address_change == AddressChangePolicy::Stop)
            {
                ExchangeResult result;
                result.outcome = ExchangeOutcome::Interrupted;
                result.send_status = node_.address_state() == AddressState::CannotClaim ? SendStatus::NoAddress
                                                                                          : SendStatus::Ok;
                if (s->transfer_cancelled)
                {
                    result.transfer = TransferResult::Cancelled;
                }
                result.time = change.time;
                if (!finish(*s, result))
                {
                    return false;
                }
            }
        }
    }
    // The handler runs in the step (often in the timer that ended the wait after the claim,
    // where a Timer started now would wait for the next step): send what is due at once.
    if (node_.can_send_now() && !send_due(change.time))
    {
        return false;
    }
    arm();
    return true;
}

bool Exchange::finish(Slot& slot, ExchangeResult result)
{
    result.pgn = slot.pgn;
    result.peer = slot.peer;
    result.peer_name = slot.peer_name;
    result.attempts = slot.attempts;
    result.pending = slot.pending;
    switch (result.outcome)
    {
    case ExchangeOutcome::Reply:
        ++statistics_.replies;
        break;
    case ExchangeOutcome::Timeout:
        ++statistics_.timeouts;
        break;
    case ExchangeOutcome::NotSent:
        ++statistics_.not_sent;
        break;
    case ExchangeOutcome::PeerLost:
        ++statistics_.peer_lost;
        break;
    case ExchangeOutcome::Interrupted:
        ++statistics_.interrupted;
        break;
    }
    // Free before the callback, which may start another exchange in this slot.
    ExchangeCallback done = std::move(slot.done);
    slot.phase = Phase::Free;
    slot.done = nullptr;
    slot.match = nullptr;
    const detail::UserCall guard(gone_);
    done(result);
    return !guard.gone();
}

} // namespace vehicle_sil::j1939
