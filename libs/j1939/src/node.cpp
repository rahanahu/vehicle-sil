// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/observer.hpp"

#include "transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::j1939 {

namespace {

[[nodiscard]] can::Frame make_frame(const Identifier& id, std::span<const std::uint8_t> data)
{
    can::Frame frame;
    frame.id = id.encode();
    frame.format = can::IdFormat::Extended;
    frame.length = static_cast<std::uint8_t>(data.size());
    std::copy(data.begin(), data.end(), frame.data.begin());
    return frame;
}

[[nodiscard]] std::array<std::uint8_t, 3> le24(std::uint32_t value) noexcept
{
    return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16)};
}

/// Only a single-address node with an address of 0..127 or 248..253 sends at once after its
/// claim; every other node waits 250 ms.
[[nodiscard]] bool waits_after_claim(const Name& name, Address address) noexcept
{
    return name.arbitrary_address_capable() || (address >= 128 && address <= 247);
}

} // namespace

SendStatus RequestContext::respond(std::span<const std::uint8_t> data, Priority priority)
{
    return node_.respond(request_, data, priority);
}

bool RequestContext::acknowledge(AckCode code)
{
    return node_.acknowledge(request_, code);
}

SendStatus Node::respond(const PendingRequest& request, std::span<const std::uint8_t> data, Priority priority)
{
    network_.require_in_step("respond()");
    if (request.node_ != this)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: respond() to a request of another node");
    }
    // The transport protocol (more than 8 bytes): to the requester for a destination-specific
    // request (also a PDU2 group), to all nodes otherwise. A single frame: to the requester only
    // for a destination-specific request of a PDU1 group.
    const bool transfer = data.size() > can::kMaxPayload;
    const Address destination =
        (request.specific_ && (transfer || request.pgn_.is_pdu1())) ? request.requester_ : kGlobalAddress;
    const bool wait = !can_send_now();
    last_transfer_ = {};
    // The request was for the address the node had then: no answer from another one.
    const SendStatus status = request.epoch_ != epoch_ ? SendStatus::NoAddress
                                                       : send_from(request.pgn_, data, destination, priority, transfer);
    report_send(request.pgn_, priority, destination, data, status, wait);
    return status;
}

void Node::report_send(Pgn pgn, Priority priority, Address destination, std::span<const std::uint8_t> data,
                       SendStatus status, bool wait)
{
    if (!network_.observed())
    {
        return;
    }
    Event e;
    e.kind = EventKind::SendRequested;
    e.time = network_.now_.value_or(SimTime{});
    e.node = static_cast<std::int32_t>(index_);
    e.pgn = pgn;
    e.priority = priority;
    e.source = address_;
    e.destination = destination;
    e.size = data.size();
    e.data = data;
    e.status = status;
    e.held = status == SendStatus::Ok && wait;
    network_.observe(e);
    if (status == SendStatus::Ok && data.size() > can::kMaxPayload)
    {
        Event started;
        started.kind = EventKind::TransferStarted;
        started.time = e.time;
        started.node = e.node;
        started.pgn = pgn;
        started.priority = kTransportPriority;
        started.source = address_;
        started.destination = destination;
        started.size = data.size();
        started.direction = TransferDirection::Send;
        network_.observe(started);
    }
}

void Node::report_dropped(const Held& held, DropReason reason, SimTime time)
{
    if (!network_.observed())
    {
        return;
    }
    Event e;
    e.kind = EventKind::MessageDropped;
    e.time = time;
    e.node = static_cast<std::int32_t>(index_);
    e.source = address_;
    e.drop_reason = reason;
    if (held.kind == Held::Kind::Acknowledgement)
    {
        e.pgn = kPgnAcknowledgement;
        e.destination = kGlobalAddress;
        e.size = 8;
    }
    else
    {
        e.pgn = held.pgn;
        e.priority = held.priority;
        e.destination = held.destination;
        e.size = held.length;
    }
    network_.observe(e);
}

bool Node::acknowledge(const PendingRequest& request, AckCode code)
{
    network_.require_in_step("acknowledge()");
    if (request.node_ != this)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: acknowledge() of a request of another node");
    }
    if (!request.specific_ || request.epoch_ != epoch_)
    {
        return false;
    }
    return send_acknowledgement(code, request.requester_, request.raw_pgn_);
}

std::optional<Acknowledgement> parse_acknowledgement(const Message& message) noexcept
{
    if (message.pgn != kPgnAcknowledgement || message.data.size() < 8)
    {
        return std::nullopt;
    }
    const auto& d = message.data;
    return Acknowledgement{static_cast<AckCode>(d[0]), d[1], d[4],
                           Pgn{static_cast<std::uint32_t>(d[5]) | (static_cast<std::uint32_t>(d[6]) << 8) |
                               (static_cast<std::uint32_t>(d[7]) << 16)}};
}

SimDuration cannot_claim_delay(Name name) noexcept
{
    // splitmix64 of the NAME: deterministic, and different NAMEs spread over the range.
    std::uint64_t z = name.value() + 0x9E3779B97F4A7C15u;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    z ^= z >> 31;
    return std::chrono::microseconds(600) * static_cast<int>(z % 256u);
}

Node::Node(Network& network, std::size_t index, const NodeOptions& options)
  : network_(network),
    index_(index),
    address_(options.address),
    name_(options.name),
    state_(options.name ? AddressState::NotStarted : AddressState::Fixed),
    holding_(options.name && waits_after_claim(*options.name, options.address))
{
    if (name_)
    {
        held_.reserve(network.options_.hold_capacity);
    }
}

void Node::start_claim()
{
    const SimTime now = *network_.now_;
    Address address = address_;
    if (!is_unicast(address))
    {
        // Arbitrary address capable without a configured address.
        address = network_.free_address(*this);
        if (address == kNullAddress)
        {
            lose_address(now);
            return;
        }
    }
    claim(address, now);
}

void Node::claim(Address address, SimTime since)
{
    const AddressState previousState = state_;
    const Address previousAddress = address_;
    const bool hadAddress = holds_address();
    network_.timers_.cancel(claim_wait_);
    network_.timers_.cancel(cannot_claim_timer_);
    state_ = AddressState::Claiming;
    address_ = address;
    ++epoch_;
    holding_ = waits_after_claim(*name_, address);
    last_claim_cause_ = since; // this claim also answers contention with the same time
    if (hadAddress)
    {
        // The transfers from or to the old address end without a frame (not on the first claim).
        network_.transport_->close_node_sessions(index_, since);
    }
    // Whoever the table has at the address loses to us if its NAME is higher.
    auto& table = network_.address_table_;
    if (const auto there = table.name_at(address); there && *name_ < *there)
    {
        network_.table_erase(address, since);
    }
    send_claim(address);
    if (!holding_)
    {
        release_held(); // sent before the first claim (added in the application stage)
    }
    // Counted from the step that sends the claim (it leaves at this step's flush).
    claim_wait_ = network_.timers_.start(TimerKind::Scheduled, *network_.now_ + std::chrono::milliseconds(250), *this, kClaimWait);
    notify_change(previousState, previousAddress, since);
}

void Node::notify_change(AddressState previousState, Address previousAddress, SimTime time)
{
    if (previousState == state_ && previousAddress == address_)
    {
        return;
    }
    const AddressChange change{state_, address_, previousState, previousAddress, time};
    if (network_.observed())
    {
        Event e;
        e.kind = EventKind::AddressChanged;
        e.time = time;
        e.node = static_cast<std::int32_t>(index_);
        e.address = change;
        network_.observe(e);
    }
    const std::optional<SimTime> outerTime = std::exchange(network_.event_time_, time);
    const std::size_t count = entries_.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        if (entries_[i].kind == EntryKind::AddressChange && !entries_[i].removed)
        {
            entries_[i].address_handler(change);
        }
    }
    network_.event_time_ = outerTime;
}

void Node::send_claim(Address source)
{
    const auto name = name_->to_bytes();
    network_.enqueue_control(make_frame(Identifier{kDefaultPriority, kPgnAddressClaimed, kGlobalAddress, source}, name), index_);
}

void Node::handle_claim(const Message& message)
{
    if (!name_ || message.data.size() < 8 || (state_ != AddressState::Claiming && state_ != AddressState::Claimed) ||
        message.source != address_)
    {
        return;
    }
    const Name theirs = Name::from_bytes(message.data);
    if (*name_ < theirs)
    {
        claim_again(message.time);
    }
    else
    {
        // A lower NAME wins. The same NAME from another sender is treated as a loss too.
        lose_address(message.time);
    }
}

void Node::handle_violation(SimTime time)
{
    // Another sender uses our address: claim it again.
    claim_again(time);
}

void Node::claim_again(SimTime cause)
{
    // Decided by the frame times only, so the number of claims does not depend on the step size.
    if (last_claim_cause_ != cause)
    {
        last_claim_cause_ = cause;
        send_claim(address_);
    }
}

void Node::handle_commanded_address(const Message& message)
{
    if (!name_ || message.data.size() < 8 || Name::from_bytes(message.data.first(8)) != *name_)
    {
        return; // for another control function
    }
    if (message.data.size() != 9 || !is_unicast(message.data[8]) || !name_->arbitrary_address_capable() ||
        state_ == AddressState::NotStarted)
    {
        ++network_.statistics_.commanded_addresses_ignored;
        return;
    }
    const Address address = message.data[8];
    if (address == address_ && holds_address())
    {
        send_claim(address_); // already there: confirm it
        return;
    }
    // Moving: what the node holds was meant for its old address.
    drop_held(message.time);
    claim(address, message.time);
}

void Node::drop_held(SimTime time)
{
    // No handler is called here: the node's state is still the old one. A held transfer is a
    // waiting session of the node; the address change that follows closes it (claim() or
    // lose_address() calls Transport::close_node_sessions() after changing the state, and the
    // node holds only while it has an address), and on_transfer_end() reports it there.
    network_.statistics_.held_dropped += held_.size();
    for (const Held& held : held_)
    {
        if (held.kind != Held::Kind::Transfer && held.kind != Held::Kind::Control)
        {
            report_dropped(held, DropReason::AddressChanged, time); // the Observer only
        }
    }
    held_.clear();
}

void Node::lose_address(SimTime since)
{
    drop_held(since);
    if (name_->arbitrary_address_capable())
    {
        // The table already has the winner, so the lost address is not free.
        const Address next = network_.free_address(*this);
        if (next != kNullAddress)
        {
            claim(next, since);
            return;
        }
    }
    const AddressState previousState = state_;
    const Address previousAddress = address_;
    network_.timers_.cancel(claim_wait_);
    state_ = AddressState::CannotClaim;
    address_ = kNullAddress;
    ++epoch_;
    holding_ = false;
    // Also without an address before (the first claim found none): queued responses end.
    network_.transport_->close_node_sessions(index_, since);
    schedule_cannot_claim(since);
    notify_change(previousState, previousAddress, since);
}

void Node::schedule_cannot_claim(SimTime since)
{
    if (network_.timers_.is_running(cannot_claim_timer_))
    {
        return; // one Cannot Claim answers them all
    }
    cannot_claim_timer_ = network_.timers_.start(TimerKind::Scheduled, since + cannot_claim_delay(*name_), *this, kCannotClaim);
}

void Node::on_timer(std::uint32_t tag)
{
    if (tag == kClaimWait)
    {
        state_ = AddressState::Claimed;
        holding_ = false;
        release_held();
        notify_change(AddressState::Claiming, address_, *network_.now_);
    }
    else if (tag == kCannotClaim && state_ == AddressState::CannotClaim)
    {
        send_claim(kNullAddress);
    }
}

void Node::release_held()
{
    // Index loop: starting a transfer cannot add to held_ (the node no longer holds), but a
    // cancelled one calls handlers.
    for (std::size_t i = 0; i < held_.size(); ++i)
    {
        const Held held = held_[i];
        switch (held.kind)
        {
        case Held::Kind::Acknowledgement:
            // A full control queue counts the drop itself (control_frames_dropped).
            if (!send_acknowledgement(held.code, held.requester, held.raw_pgn))
            {
                report_dropped(held, DropReason::QueueFull, network_.now_.value_or(SimTime{}));
            }
            break;
        case Held::Kind::Message:
            if (send_from(held.pgn, std::span<const std::uint8_t>(held.data.data(), held.length), held.destination,
                          held.priority) != SendStatus::Ok)
            {
                ++network_.statistics_.held_dropped;
                report_dropped(held, DropReason::QueueFull, network_.now_.value_or(SimTime{}));
            }
            break;
        case Held::Kind::Control:
            if (!network_.enqueue_control(
                    make_frame(Identifier{kTransportPriority, kPgnTransportControl, held.destination, address_}, held.data),
                    index_, held.session, held.generation))
            {
                // The session would wait for a frame that never leaves (counted as dropped).
                network_.transport_->on_control_dropped(held.session, held.generation);
            }
            break;
        case Held::Kind::Transfer:
            network_.transport_->start_waiting(held.session, held.generation);
            break;
        }
    }
    held_.clear();
}

SendStatus Node::send(Pgn pgn, std::span<const std::uint8_t> data, Address destination, Priority priority,
                      WhenBusy whenBusy)
{
    network_.require_in_step("send()");
    const bool wait = !can_send_now();
    const SendStatus status = send_from(pgn, data, destination, priority, whenBusy == WhenBusy::Queue);
    report_send(pgn, priority, destination, data, status, wait);
    return status;
}

SendStatus Node::request(Pgn pgn, Address destination)
{
    network_.require_in_step("request()");
    // A request for Address Claimed of a node with a NAME is part of address claiming: never held.
    const bool wait = !can_send_now() && (pgn != kPgnAddressClaimed || !name_);
    const SendStatus status = request_from(pgn, destination);
    if (network_.observed())
    {
        const auto data = le24(pgn.value());
        report_send(kPgnRequest, kDefaultPriority, destination, data, status, wait);
    }
    return status;
}

SendStatus Node::request_from(Pgn pgn, Address destination)
{
    if (!pgn.is_valid() || destination == kNullAddress)
    {
        return SendStatus::InvalidArgument;
    }
    const auto data = le24(pgn.value());
    if (pgn != kPgnAddressClaimed || !name_)
    {
        return send_from(kPgnRequest, data, destination, kDefaultPriority);
    }
    // Part of address claiming: never held, allowed without an address, and answered by this
    // node too (its own frames do not loop back to it).
    // Before its first claim the node has no address yet: it asks from the null address.
    const Address source = state_ == AddressState::NotStarted ? kNullAddress : address_;
    if (!network_.enqueue_control(make_frame(Identifier{kDefaultPriority, kPgnRequest, destination, source}, data), index_))
    {
        return SendStatus::QueueFull;
    }
    if (destination == kGlobalAddress || (destination == address_ && state_ != AddressState::NotStarted))
    {
        if (state_ == AddressState::CannotClaim)
        {
            schedule_cannot_claim(*network_.now_);
        }
        else if (state_ != AddressState::NotStarted)
        {
            send_claim(address_);
        }
    }
    return SendStatus::Ok;
}

SendStatus Node::send_from(Pgn pgn, std::span<const std::uint8_t> data, Address destination, Priority priority,
                           bool queue_if_busy)
{
    last_transfer_ = {};
    const bool transfer = data.size() > can::kMaxPayload;
    // A transfer may carry a PDU2 group to one destination; a single frame cannot.
    if (!pgn.is_valid() || priority > kMaxPriority || destination == kNullAddress ||
        (!transfer && pgn.is_pdu2() && destination != kGlobalAddress))
    {
        return SendStatus::InvalidArgument;
    }
    if (data.size() > kMaxTransportLength)
    {
        return SendStatus::TooLong;
    }
    if (state_ == AddressState::CannotClaim)
    {
        return SendStatus::NoAddress;
    }
    if (transfer)
    {
        return queue_if_busy ? network_.transport_->respond(*this, pgn, data, destination)
                             : network_.transport_->send(*this, pgn, data, destination);
    }
    if (holding_ || state_ == AddressState::NotStarted)
    {
        if (held_.size() >= network_.options_.hold_capacity)
        {
            return SendStatus::QueueFull;
        }
        Held held{Held::Kind::Message, pgn, destination, priority, static_cast<std::uint8_t>(data.size()), {}, AckCode::Positive, 0, 0};
        std::copy(data.begin(), data.end(), held.data.begin());
        held_.push_back(held);
        return SendStatus::Ok;
    }
    const can::Frame frame = make_frame(Identifier{priority, pgn, destination, address_}, data);
    return network_.enqueue_data(frame, index_) ? SendStatus::Ok : SendStatus::QueueFull;
}

bool Node::send_acknowledgement(AckCode code, Address requester, std::uint32_t rawPgn)
{
    if (state_ == AddressState::CannotClaim)
    {
        return false;
    }
    if (holding_ || state_ == AddressState::NotStarted)
    {
        if (held_.size() >= network_.options_.hold_capacity)
        {
            ++network_.statistics_.held_dropped;
            return false;
        }
        held_.push_back(Held{Held::Kind::Acknowledgement, Pgn{}, kGlobalAddress, kDefaultPriority, 0, {}, code, requester, rawPgn});
        return true;
    }
    const auto pgn = le24(rawPgn);
    const std::array<std::uint8_t, 8> data{static_cast<std::uint8_t>(code), 0xFF, 0xFF, 0xFF, requester, pgn[0], pgn[1], pgn[2]};
    return network_.enqueue_control(
        make_frame(Identifier{kDefaultPriority, kPgnAcknowledgement, kGlobalAddress, address_}, data), index_);
}

bool Node::send_transport_control(Address destination, const std::array<std::uint8_t, 8>& data, std::uint32_t session,
                                  std::uint32_t generation)
{
    if (state_ == AddressState::CannotClaim)
    {
        return false;
    }
    if (holding_ || state_ == AddressState::NotStarted)
    {
        if (held_.size() >= network_.options_.hold_capacity)
        {
            ++network_.statistics_.held_dropped;
            return false;
        }
        Held held{Held::Kind::Control, kPgnTransportControl, destination, kTransportPriority, 8, data, AckCode::Positive, 0, 0};
        held.session = session;
        held.generation = generation;
        held_.push_back(held);
        return true;
    }
    return network_.enqueue_control(make_frame(Identifier{kTransportPriority, kPgnTransportControl, destination, address_}, data),
                                    index_, session, generation);
}

void Node::notify_transfer_end(const TransferEnd& end, Address source)
{
    if (network_.observed())
    {
        Event e;
        e.kind = EventKind::TransferEnded;
        e.time = end.time;
        e.node = static_cast<std::int32_t>(index_);
        e.pgn = end.pgn;
        e.priority = kTransportPriority;
        e.source = source;
        e.destination = end.destination;
        e.size = end.length;
        e.direction = TransferDirection::Send;
        e.result = end.result;
        e.reason = end.reason;
        network_.observe(e);
    }
    const std::optional<SimTime> outerTime = std::exchange(network_.event_time_, end.time);
    const std::size_t count = entries_.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        if (entries_[i].kind == EntryKind::TransferEnd && !entries_[i].removed)
        {
            entries_[i].transfer_handler(end);
        }
    }
    network_.event_time_ = outerTime;
}

HandlerId Node::add_entry(Entry entry)
{
    network_.require_not_observing("registering a handler");
    entry.id = HandlerId{network_.next_handler_id_++};
    const HandlerId id = entry.id;
    if (network_.dispatching_)
    {
        pending_add_.push_back(std::move(entry));
    }
    else
    {
        entries_.push_back(std::move(entry));
    }
    return id;
}

void Node::add_part(const std::type_info& type, void* part, void (*destroy)(void*) noexcept)
{
    network_.require_not_observing("attach()");
    network_.attached_.push_back(Network::Attached{index_, std::type_index(type), part, destroy});
}

void* Node::find_part(const std::type_info& type) const noexcept
{
    for (const auto& attached : network_.attached_)
    {
        if (attached.node == index_ && attached.type == std::type_index(type))
        {
            return attached.part;
        }
    }
    return nullptr;
}

HandlerId Node::on_message(Pgn pgn, MessageHandler handler)
{
    if (!handler)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: empty handler");
    }
    return add_entry(Entry{{}, EntryKind::Message, false, false, pgn, std::move(handler), {}, {}, {}});
}

HandlerId Node::on_any_message(MessageHandler handler)
{
    if (!handler)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: empty handler");
    }
    return add_entry(Entry{{}, EntryKind::Message, true, false, Pgn{}, std::move(handler), {}, {}, {}});
}

HandlerId Node::on_request(Pgn pgn, RequestResponder responder)
{
    if (!responder)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: empty request responder");
    }
    return add_entry(Entry{{}, EntryKind::Request, false, false, pgn, {}, std::move(responder), {}, {}});
}

HandlerId Node::on_any_request(RequestResponder responder)
{
    if (!responder)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: empty request responder");
    }
    return add_entry(Entry{{}, EntryKind::Request, true, false, Pgn{}, {}, std::move(responder), {}, {}});
}

HandlerId Node::on_address_change(AddressHandler handler)
{
    if (!handler)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: empty address handler");
    }
    return add_entry(Entry{{}, EntryKind::AddressChange, false, false, Pgn{}, {}, {}, std::move(handler), {}});
}

HandlerId Node::on_transfer_end(TransferHandler handler)
{
    if (!handler)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Node: empty transfer handler");
    }
    return add_entry(Entry{{}, EntryKind::TransferEnd, false, false, Pgn{}, {}, {}, {}, std::move(handler)});
}

bool Node::remove_handler(HandlerId id)
{
    if (network_.fail_if_observing("remove_handler()"))
    {
        return false;
    }
    const auto matches = [id](const Entry& e) { return e.id == id; };
    const auto pending = std::find_if(pending_add_.begin(), pending_add_.end(), matches);
    if (pending != pending_add_.end())
    {
        // Destroyed after the erase: the handler's captures may remove other handlers.
        const Entry gone = std::move(*pending);
        pending_add_.erase(pending);
        return true;
    }
    const auto active = std::find_if(entries_.begin(), entries_.end(), matches);
    if (active == entries_.end() || active->removed)
    {
        return false;
    }
    if (network_.dispatching_)
    {
        // The list being delivered keeps its entries (a handler may be the one running); the
        // removed one is skipped from now on and erased after the delivery.
        active->removed = true;
        pending_remove_ = true;
    }
    else
    {
        const Entry gone = std::move(*active); // destroyed after the erase (see above)
        entries_.erase(active);
    }
    return true;
}

void Node::apply_pending()
{
    if (pending_remove_)
    {
        pending_remove_ = false;
        // One at a time, each destroyed after its erase: destroying a handler's captures may
        // remove (or add) handlers again, so the vector is searched afresh every time.
        while (true)
        {
            const auto removed = std::find_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.removed; });
            if (removed == entries_.end())
            {
                break;
            }
            const Entry gone = std::move(*removed);
            entries_.erase(removed);
        }
    }
    if (!pending_add_.empty())
    {
        for (auto& entry : pending_add_)
        {
            entries_.push_back(std::move(entry));
        }
        pending_add_.clear();
    }
}

void Node::deliver(const Message& message)
{
    if (message.pgn == kPgnRequest)
    {
        handle_request(message);
    }
    else if (message.pgn == kPgnAddressClaimed)
    {
        handle_claim(message);
    }
    else if (message.pgn == kPgnCommandedAddress)
    {
        handle_commanded_address(message);
    }
    const std::size_t count = entries_.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const Entry& entry = entries_[i];
        if (entry.kind == EntryKind::Message && !entry.removed && (entry.any || entry.pgn == message.pgn))
        {
            entry.handler(message);
        }
    }
}

void Node::handle_request(const Message& message)
{
    if (message.data.size() < 3)
    {
        ++network_.statistics_.requests_malformed;
        return;
    }
    // Longer requests are accepted: the requested PGN is the first 3 bytes.
    const std::uint32_t raw = static_cast<std::uint32_t>(message.data[0]) | (static_cast<std::uint32_t>(message.data[1]) << 8) |
                              (static_cast<std::uint32_t>(message.data[2]) << 16);
    const Pgn requested{raw};
    if (requested == kPgnAddressClaimed)
    {
        // Part of address claiming: answered with the claim (also during the wait), never
        // acknowledged. Nodes without a NAME do not answer.
        if (state_ == AddressState::CannotClaim)
        {
            schedule_cannot_claim(message.time);
        }
        else if (state_ == AddressState::Claiming || state_ == AddressState::Claimed)
        {
            send_claim(address_);
        }
        return;
    }
    if (message.source == kNullAddress || state_ == AddressState::CannotClaim)
    {
        // A node without an address may only ask for address claims; a node that lost its
        // address does not answer.
        return;
    }
    const bool specific = message.destination != kGlobalAddress;

    const Entry* responder = nullptr;
    for (const auto& entry : entries_)
    {
        if (entry.kind == EntryKind::Request && !entry.removed && !entry.any && entry.pgn == requested)
        {
            responder = &entry;
            break;
        }
    }
    if (responder == nullptr)
    {
        for (const auto& entry : entries_)
        {
            if (entry.kind == EntryKind::Request && !entry.removed && entry.any)
            {
                responder = &entry;
                break;
            }
        }
    }
    if (responder != nullptr)
    {
        RequestContext context(*this, PendingRequest(this, epoch_, requested, raw, message.source, specific, message.time));
        responder->responder(context);
        return;
    }
    if (specific)
    {
        send_acknowledgement(AckCode::Negative, message.source, raw);
    }
}

} // namespace vehicle_sil::j1939
