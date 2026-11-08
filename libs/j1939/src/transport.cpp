// SPDX-License-Identifier: MIT
#include "transport.hpp"
#include "vehicle_sil/j1939/observer.hpp"

#include <algorithm>
#include <chrono>
#include <optional>

namespace vehicle_sil::j1939 {

namespace {

constexpr std::uint8_t kRts = 16;
constexpr std::uint8_t kCts = 17;
constexpr std::uint8_t kEndOfMessageAck = 19;
constexpr std::uint8_t kBam = 32;
constexpr std::uint8_t kAbort = 255;

constexpr SimDuration kT1 = std::chrono::milliseconds(750);
constexpr SimDuration kT2 = std::chrono::milliseconds(1250);
constexpr SimDuration kT3 = std::chrono::milliseconds(1250);
constexpr SimDuration kT4 = std::chrono::milliseconds(1050);
constexpr std::uint8_t kMaxRetransmitRequests = 2;

[[nodiscard]] std::uint16_t packets_for(std::size_t length) noexcept
{
    return static_cast<std::uint16_t>((length + 6) / 7);
}

[[nodiscard]] std::uint32_t raw_pgn_of(std::span<const std::uint8_t> data) noexcept
{
    return static_cast<std::uint32_t>(data[5]) | (static_cast<std::uint32_t>(data[6]) << 8) |
           (static_cast<std::uint32_t>(data[7]) << 16);
}

[[nodiscard]] std::array<std::uint8_t, 8> control(std::uint8_t code, std::uint8_t b1, std::uint8_t b2, std::uint8_t b3,
                                                  std::uint8_t b4, std::uint32_t rawPgn) noexcept
{
    return {code, b1, b2, b3, b4, static_cast<std::uint8_t>(rawPgn), static_cast<std::uint8_t>(rawPgn >> 8),
            static_cast<std::uint8_t>(rawPgn >> 16)};
}

} // namespace

void Transport::Session::on_timer(std::uint32_t /*tag*/) { transport->on_timer(*this); }

Transport::Transport(Network& network, const NetworkOptions& options) : network_(network), options_(options)
{
    sessions_.resize(options.bam_receive_sessions + options.rts_receive_sessions + options.bam_transmit_sessions +
                     options.rts_transmit_sessions);
    std::size_t i = 0;
    const auto init = [&](Kind kind, std::size_t count) {
        for (std::size_t k = 0; k < count; ++k, ++i)
        {
            Session& s = sessions_[i];
            s.transport = this;
            s.index = static_cast<std::uint32_t>(i);
            s.kind = kind;
            s.buffer.resize(kMaxTransportLength);
        }
    };
    init(Kind::BamRx, options.bam_receive_sessions);
    init(Kind::RtsRx, options.rts_receive_sessions);
    init(Kind::BamTx, options.bam_transmit_sessions);
    init(Kind::RtsTx, options.rts_transmit_sessions);
    responses_.resize(options.response_queue_capacity);
    for (auto& r : responses_)
    {
        r.data.resize(kMaxTransportLength);
    }
}

// --- lookup -------------------------------------------------------------------------------

Transport::Session* Transport::find(std::uint32_t tag, std::uint32_t generation) noexcept
{
    if (tag == 0 || tag > sessions_.size())
    {
        return nullptr;
    }
    Session& s = sessions_[tag - 1];
    return (s.state != State::Free && s.generation == generation) ? &s : nullptr;
}

Transport::Session* Transport::allocate(Kind kind) noexcept
{
    for (auto& s : sessions_)
    {
        if (s.kind == kind && s.state == State::Free)
        {
            s.order = next_order_++;
            s.transfer = 0;
            s.rx_sender = -1;
            s.announced = false;
            s.rx_nodes = 0;
            s.next = 1;
            s.window_start = 1;
            s.window_end = 0;
            s.sent_high = 0;
            s.retries = 0;
            s.gap = false;
            s.ready = false;
            return &s;
        }
    }
    return nullptr;
}

Transport::Session* Transport::find_rx(Kind kind, Address source, Address destination) noexcept
{
    for (auto& s : sessions_)
    {
        if (s.kind == kind && s.state != State::Free && s.source == source && s.destination == destination)
        {
            return &s;
        }
    }
    return nullptr;
}

Transport::Session* Transport::find_tx(Address local, Address peer, std::uint32_t rawPgn, std::int32_t sender) noexcept
{
    for (auto& s : sessions_)
    {
        if (s.kind == Kind::RtsTx && s.state != State::Free && s.state != State::Waiting && s.node != sender &&
            s.source == local && s.destination == peer && s.raw_pgn == rawPgn)
        {
            return &s;
        }
    }
    return nullptr;
}

bool Transport::busy(std::size_t node, Address destination) const noexcept
{
    return std::any_of(sessions_.begin(), sessions_.end(), [&](const Session& s) {
        return s.state != State::Free && s.node == static_cast<std::int32_t>(node) &&
               ((s.kind == Kind::BamTx && destination == kGlobalAddress) ||
                (s.kind == Kind::RtsTx && s.destination == destination));
    });
}

std::int32_t Transport::responder_for(Address address, std::int32_t sender) const noexcept
{
    const auto& nodes = network_.nodes_;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const Node& n = *nodes[i];
        if (static_cast<std::int32_t>(i) != sender && n.active_ && n.address_ == address &&
            (n.state_ == AddressState::Fixed || n.holds_address()))
        {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

bool Transport::for_us(Address destination, std::int32_t sender) const noexcept
{
    if (destination == kGlobalAddress)
    {
        return true;
    }
    const auto& nodes = network_.nodes_;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        if (static_cast<std::int32_t>(i) != sender && nodes[i]->active_ && nodes[i]->address_ == destination)
        {
            return true;
        }
    }
    return false;
}

Node& Transport::node_of(const Session& session) const { return *network_.nodes_[static_cast<std::size_t>(session.node)]; }

// --- session life -------------------------------------------------------------------------

void Transport::arm(Session& session, TimerKind kind, SimTime deadline)
{
    disarm(session);
    session.deadline = deadline;
    session.timer = network_.timers_.start(kind, deadline, session, 0);
}

void Transport::disarm(Session& session) noexcept
{
    network_.timers_.cancel(session.timer);
    session.timer = {};
}

void Transport::release(Session& session) noexcept
{
    disarm(session);
    session.state = State::Free;
    session.node = -1;
    ++session.generation;
}

void Transport::report_receive_start(const Session& session, SimTime time)
{
    if (!network_.observed())
    {
        return;
    }
    Event e;
    e.kind = EventKind::TransferStarted;
    e.time = time;
    e.node = session.node;
    e.pgn = Pgn{session.raw_pgn};
    e.priority = session.priority;
    e.source = session.source;
    e.destination = session.destination;
    e.size = session.length;
    e.direction = TransferDirection::Receive;
    network_.observe(e);
}

void Transport::announce_receive(Session& session, SimTime time)
{
    session.transfer = next_transfer_++;
    session.announced = true;
    // The nodes taking part now: those added during the step come last and take part from the
    // next one; they are not told of this start, nor of its end.
    std::uint32_t taking = 0;
    while (taking < network_.nodes_.size() && network_.nodes_[taking]->active_)
    {
        ++taking;
    }
    session.rx_nodes = taking;
    IncomingTransfer t;
    t.stage = TransferStage::Started;
    t.id = TransferId{session.transfer};
    t.pgn = Pgn{session.raw_pgn};
    t.source = session.source;
    t.destination = session.destination;
    t.length = session.length;
    t.time = time;
    notify_receive(t, session.node, session.rx_sender, session.rx_nodes);
}

void Transport::notify_receive(const IncomingTransfer& transfer, std::int32_t node, std::int32_t sender, std::size_t limit)
{
    auto& nodes = network_.nodes_;
    if (node >= 0)
    {
        if (static_cast<std::size_t>(node) < nodes.size())
        {
            nodes[static_cast<std::size_t>(node)]->notify_incoming(transfer);
        }
        return;
    }
    // A broadcast: every node that took part at its start, but its sender (as for the message).
    const std::size_t count = std::min(nodes.size(), limit);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (static_cast<std::int32_t>(i) != sender && nodes[i]->active_)
        {
            nodes[i]->notify_incoming(transfer);
        }
    }
}

void Transport::end_receive(Session& session, TransferResult result, AbortReason reason, SimTime time)
{
    // A copy first: the session is released before anyone is told (a handler may start others).
    std::optional<IncomingTransfer> ended;
    const std::int32_t node = session.node;
    const std::int32_t sender = session.rx_sender;
    const std::size_t told = session.rx_nodes;
    if (session.announced)
    {
        IncomingTransfer t;
        t.stage = TransferStage::Ended;
        t.id = TransferId{session.transfer};
        t.pgn = Pgn{session.raw_pgn};
        t.source = session.source;
        t.destination = session.destination;
        t.length = session.length;
        t.result = result;
        t.reason = reason;
        t.time = time;
        ended = t;
    }
    end_receive_released(session, result, reason, time);
    if (ended)
    {
        notify_receive(*ended, node, sender, told);
    }
}

void Transport::end_receive_released(Session& session, TransferResult result, AbortReason reason, SimTime time)
{
    if (network_.observed())
    {
        Event e;
        e.kind = EventKind::TransferEnded;
        e.time = time;
        e.node = session.node;
        e.pgn = Pgn{session.raw_pgn};
        e.priority = session.priority;
        e.source = session.source;
        e.destination = session.destination;
        e.size = session.length;
        e.direction = TransferDirection::Receive;
        e.result = result;
        e.reason = reason;
        release(session);
        network_.observe(e);
        return;
    }
    release(session);
}

void Transport::finish(Session& session, TransferResult result, AbortReason reason, SimTime time)
{
    Node& node = node_of(session);
    const TransferEnd end{Pgn{session.raw_pgn}, session.destination, session.length, result, reason, time,
                          TransferId{session.transfer}, session.packets > 0 && session.sent_high >= session.packets};
    release(session);
    if (result == TransferResult::Completed)
    {
        ++network_.statistics_.transport_messages_sent;
    }
    node.notify_transfer_end(end, session.source);
}

void Transport::send_abort(std::size_t node, Address destination, std::uint32_t rawPgn, AbortReason reason)
{
    const auto data = control(kAbort, static_cast<std::uint8_t>(reason), 0xFF, 0xFF, 0xFF, rawPgn);
    if (network_.nodes_[node]->send_transport_control(destination, data, 0, 0))
    {
        ++network_.statistics_.transport_aborts_sent;
    }
}

void Transport::abort(Session& session, AbortReason reason, SimTime time)
{
    const bool transmit = session.kind == Kind::RtsTx;
    send_abort(static_cast<std::size_t>(session.node), transmit ? session.destination : session.source, session.raw_pgn,
               reason);
    if (transmit)
    {
        finish(session, TransferResult::Aborted, reason, time);
    }
    else
    {
        end_receive(session, TransferResult::Aborted, reason, time);
    }
}

// --- sending ------------------------------------------------------------------------------

SendStatus Transport::send(Node& node, Pgn pgn, std::span<const std::uint8_t> data, Address destination)
{
    if (busy(node.index_, destination))
    {
        return SendStatus::Busy;
    }
    const std::uint64_t transfer = next_transfer_;
    const SendStatus status = open_transmit(node, pgn, data, destination, transfer);
    if (status == SendStatus::Ok)
    {
        ++next_transfer_;
        node.last_transfer_ = TransferId{transfer};
    }
    return status;
}

SendStatus Transport::respond(Node& node, Pgn pgn, std::span<const std::uint8_t> data, Address destination)
{
    // Behind a queued response of the same node to the same destination (or a broadcast).
    const bool queued = std::any_of(responses_.begin(), responses_.end(), [&](const Response& r) {
        return r.used && r.node == node.index_ && r.destination == destination;
    });
    const std::uint64_t transfer = next_transfer_;
    if (!queued && !busy(node.index_, destination))
    {
        const SendStatus status = open_transmit(node, pgn, data, destination, transfer);
        if (status == SendStatus::Ok)
        {
            ++next_transfer_;
            node.last_transfer_ = TransferId{transfer};
        }
        if (status != SendStatus::QueueFull)
        {
            return status;
        }
    }
    for (auto& r : responses_)
    {
        if (!r.used)
        {
            r.used = true;
            r.order = next_order_++;
            r.transfer = next_transfer_++;
            node.last_transfer_ = TransferId{r.transfer};
            r.node = static_cast<std::uint32_t>(node.index_);
            r.source = node.address_;
            r.pgn = pgn;
            r.destination = destination;
            r.length = static_cast<std::uint16_t>(data.size());
            std::copy(data.begin(), data.end(), r.data.begin());
            return SendStatus::Ok;
        }
    }
    ++network_.statistics_.transport_responses_dropped;
    return SendStatus::QueueFull;
}

SendStatus Transport::open_transmit(Node& node, Pgn pgn, std::span<const std::uint8_t> data, Address destination,
                                    std::uint64_t transfer)
{
    Session* s = allocate(destination == kGlobalAddress ? Kind::BamTx : Kind::RtsTx);
    if (s == nullptr)
    {
        return SendStatus::QueueFull;
    }
    s->node = static_cast<std::int32_t>(node.index_);
    s->transfer = transfer;
    s->source = node.address_;
    s->destination = destination;
    s->raw_pgn = pgn.value();
    s->priority = kTransportPriority;
    s->length = static_cast<std::uint16_t>(data.size());
    s->packets = static_cast<std::uint8_t>(packets_for(data.size()));
    s->rts_limit = options_.rts_max_packets_per_cts;
    std::copy(data.begin(), data.end(), s->buffer.begin());
    if (!node.can_send_now())
    {
        // Waiting after the claim: the transfer starts when the node's hold area is released.
        if (node.held_.size() >= options_.hold_capacity)
        {
            release(*s);
            return SendStatus::QueueFull;
        }
        s->state = State::Waiting;
        Node::Held held{};
        held.kind = Node::Held::Kind::Transfer;
        held.session = s->index + 1;
        held.generation = s->generation;
        node.held_.push_back(held);
        return SendStatus::Ok;
    }
    if (!begin_transmit(*s))
    {
        release(*s);
        return SendStatus::QueueFull;
    }
    return SendStatus::Ok;
}

bool Transport::begin_transmit(Session& session)
{
    Node& node = node_of(session);
    session.source = node.address_;
    const auto lo = static_cast<std::uint8_t>(session.length);
    const auto hi = static_cast<std::uint8_t>(session.length >> 8);
    std::array<std::uint8_t, 8> data{};
    if (session.kind == Kind::BamTx)
    {
        session.state = State::BamAnnounce;
        data = control(kBam, lo, hi, session.packets, 0xFF, session.raw_pgn);
    }
    else
    {
        session.state = State::RtsSent;
        data = control(kRts, lo, hi, session.packets, session.rts_limit, session.raw_pgn);
    }
    return node.send_transport_control(session.destination, data, session.index + 1, session.generation);
}

void Transport::on_control_dropped(std::uint32_t tag, std::uint32_t generation)
{
    Session* s = find(tag, generation);
    if (s != nullptr && s->state == State::CtsPending)
    {
        end_receive(*s, TransferResult::Cancelled, AbortReason::None, network_.event_time());
    }
}

void Transport::start_waiting(std::uint32_t tag, std::uint32_t generation)
{
    Session* s = find(tag, generation);
    if (s == nullptr || s->state != State::Waiting)
    {
        return;
    }
    if (!begin_transmit(*s))
    {
        finish(*s, TransferResult::Cancelled, AbortReason::None, *network_.now_);
    }
}

void Transport::close_node_sessions(std::size_t node, SimTime time)
{
    // Handlers told about the end may send again at once: what they start is newer than `before`
    // and stays.
    const std::uint64_t before = next_order_;
    for (auto& s : sessions_)
    {
        // A broadcast whose last packet left is complete (reported at the next step).
        if (s.state == State::Free || s.state == State::BamFinished || s.order >= before ||
            s.node != static_cast<std::int32_t>(node))
        {
            continue;
        }
        ++network_.statistics_.transport_sessions_cancelled;
        if (s.kind == Kind::RtsTx || s.kind == Kind::BamTx)
        {
            finish(s, TransferResult::Cancelled, AbortReason::None, time);
        }
        else
        {
            end_receive(s, TransferResult::Cancelled, AbortReason::None, time);
        }
    }
    for (auto& r : responses_)
    {
        if (r.used && r.order < before && r.node == node)
        {
            r.used = false;
            ++network_.statistics_.transport_sessions_cancelled;
            network_.nodes_[node]->notify_transfer_end(
                TransferEnd{r.pgn, r.destination, r.length, TransferResult::Cancelled, AbortReason::None, time,
                            TransferId{r.transfer}},
                r.source);
        }
    }
}

void Transport::close_peer_sessions(Address address, SimTime time)
{
    const std::uint64_t before = next_order_; // see close_node_sessions()
    for (auto& s : sessions_)
    {
        if (s.state == State::Free || s.order >= before)
        {
            continue;
        }
        if (s.kind == Kind::RtsTx && s.destination == address)
        {
            ++network_.statistics_.transport_sessions_cancelled;
            finish(s, TransferResult::Cancelled, AbortReason::None, time);
        }
        else if ((s.kind == Kind::RtsRx || s.kind == Kind::BamRx) && s.source == address)
        {
            ++network_.statistics_.transport_sessions_cancelled;
            end_receive(s, TransferResult::Cancelled, AbortReason::None, time);
        }
    }
    for (auto& r : responses_)
    {
        if (r.used && r.order < before && r.destination == address)
        {
            r.used = false;
            ++network_.statistics_.transport_sessions_cancelled;
            network_.nodes_[r.node]->notify_transfer_end(
                TransferEnd{r.pgn, r.destination, r.length, TransferResult::Cancelled, AbortReason::None, time,
                            TransferId{r.transfer}},
                r.source);
        }
    }
}

void Transport::pump_responses()
{
    bool bamFull = false;
    bool rtsFull = false;
    std::uint64_t last = 0;
    while (true)
    {
        Response* next = nullptr;
        for (auto& r : responses_)
        {
            if (r.used && r.order > last && (next == nullptr || r.order < next->order))
            {
                next = &r;
            }
        }
        if (next == nullptr)
        {
            return;
        }
        last = next->order;
        Node& node = *network_.nodes_[next->node];
        const bool broadcast = next->destination == kGlobalAddress;
        // Later responses of the same node and destination stay behind this one: they are
        // stopped by the same condition.
        if (!node.can_send_now() || busy(node.index_, next->destination) || (broadcast ? bamFull : rtsFull))
        {
            continue;
        }
        const SendStatus status =
            open_transmit(node, next->pgn, std::span<const std::uint8_t>(next->data.data(), next->length),
                          next->destination, next->transfer);
        if (status == SendStatus::QueueFull)
        {
            (broadcast ? bamFull : rtsFull) = true;
            continue;
        }
        next->used = false;
    }
}

void Transport::on_control_sent(std::uint32_t tag, std::uint32_t generation)
{
    Session* s = find(tag, generation);
    if (s == nullptr)
    {
        return;
    }
    const SimTime now = *network_.now_;
    switch (s->state)
    {
    case State::BamAnnounce:
        s->state = State::BamSending;
        s->ready = false;
        arm(*s, TimerKind::Scheduled, now + options_.bam_packet_interval);
        break;
    case State::RtsSent:
        arm(*s, TimerKind::Timeout, now + kT3);
        break;
    case State::CtsPending:
        s->state = State::WaitData;
        arm(*s, TimerKind::Timeout, now + kT2);
        break;
    default:
        break;
    }
}

can::Frame Transport::data_frame(const Session& session, std::uint8_t sequence) const
{
    can::Frame frame;
    frame.id = Identifier{kTransportPriority, kPgnTransportData, session.destination, session.source}.encode();
    frame.format = can::IdFormat::Extended;
    frame.length = 8;
    frame.data.fill(0xFF);
    frame.data[0] = sequence;
    const std::size_t offset = static_cast<std::size_t>(sequence - 1) * 7;
    const std::size_t count = std::min<std::size_t>(7, session.length - offset);
    std::copy_n(session.buffer.begin() + static_cast<std::ptrdiff_t>(offset), count, frame.data.begin() + 1);
    return frame;
}

void Transport::flush_data(can::Port& port, std::size_t limit)
{
    const SimTime now = *network_.now_;
    std::size_t sent = 0;
    std::uint64_t last = 0;
    while (true)
    {
        Session* s = nullptr;
        for (auto& c : sessions_)
        {
            const bool due = (c.state == State::BamSending && c.ready) || c.state == State::Sending;
            if (due && c.order > last && (s == nullptr || c.order < s->order))
            {
                s = &c;
            }
        }
        if (s == nullptr)
        {
            return;
        }
        last = s->order;
        if (s->state == State::BamSending)
        {
            if (sent >= limit)
            {
                return;
            }
            const auto sequence = static_cast<std::uint8_t>(s->next);
            if (network_.send_frame(data_frame(*s, sequence), s->node, port) == can::SendResult::QueueFull)
            {
                return;
            }
            ++sent;
            s->ready = false;
            s->sent_high = s->next;
            if (s->next >= s->packets)
            {
                // Ends at the next step: handlers are not called while flushing.
                s->state = State::BamFinished;
                arm(*s, TimerKind::Scheduled, now);
            }
            else
            {
                ++s->next;
                arm(*s, TimerKind::Scheduled, now + options_.bam_packet_interval);
            }
            continue;
        }
        while (s->next <= s->window_end)
        {
            if (sent >= limit)
            {
                return;
            }
            const auto sequence = static_cast<std::uint8_t>(s->next);
            if (network_.send_frame(data_frame(*s, sequence), s->node, port) == can::SendResult::QueueFull)
            {
                return;
            }
            ++sent;
            s->sent_high = std::max(s->sent_high, s->next);
            ++s->next;
        }
        s->state = s->window_end >= s->packets ? State::WaitEoma : State::WaitCts;
        arm(*s, TimerKind::Timeout, now + kT3);
    }
}

// --- receiving ----------------------------------------------------------------------------

void Transport::on_frame(const Message& message, std::int32_t sender)
{
    if (message.pgn == kPgnTransportData)
    {
        on_data(message, sender);
        return;
    }
    if (message.data.size() < 8)
    {
        if (for_us(message.destination, sender))
        {
            ++network_.statistics_.transport_frames_ignored;
        }
        return;
    }
    switch (message.data[0])
    {
    case kRts:
        on_rts(message, sender);
        break;
    case kCts:
        on_cts(message, sender);
        break;
    case kEndOfMessageAck:
        on_eoma(message, sender);
        break;
    case kBam:
        on_bam(message, sender);
        break;
    case kAbort:
        on_abort(message, sender);
        break;
    default:
        if (for_us(message.destination, sender))
        {
            ++network_.statistics_.transport_frames_ignored;
        }
        break;
    }
}

void Transport::on_rts(const Message& message, std::int32_t sender)
{
    if (message.destination == kGlobalAddress)
    {
        ++network_.statistics_.transport_frames_ignored;
        return;
    }
    const std::int32_t responder = responder_for(message.destination, sender);
    if (responder < 0)
    {
        return; // for another node on the bus
    }
    const auto node = static_cast<std::size_t>(responder);
    const std::size_t size = static_cast<std::size_t>(message.data[1]) | (static_cast<std::size_t>(message.data[2]) << 8);
    const std::uint8_t packets = message.data[3];
    const std::uint8_t limit = message.data[4];
    const std::uint32_t raw = raw_pgn_of(message.data);
    if (Session* existing = find_rx(Kind::RtsRx, message.source, message.destination))
    {
        if (existing->raw_pgn != raw)
        {
            // The running connection goes on; the new one is refused.
            ++network_.statistics_.transport_rejected;
            send_abort(node, message.source, raw, AbortReason::Busy);
            return;
        }
        end_receive(*existing, TransferResult::Cancelled, AbortReason::None, message.time); // the same group again: the new RTS replaces the connection
    }
    AbortReason reason = AbortReason::None;
    if (size > kMaxTransportLength)
    {
        reason = AbortReason::TooLarge;
    }
    else if (size <= can::kMaxPayload || packets != packets_for(size) || limit == 0 || !Pgn{raw}.is_valid())
    {
        reason = AbortReason::Other;
    }
    Session* s = reason == AbortReason::None ? allocate(Kind::RtsRx) : nullptr;
    if (s == nullptr)
    {
        ++network_.statistics_.transport_rejected;
        send_abort(node, message.source, raw, reason == AbortReason::None ? AbortReason::Busy : reason);
        return;
    }
    s->node = responder;
    s->source = message.source;
    s->destination = message.destination;
    s->raw_pgn = raw;
    s->priority = message.priority;
    s->length = static_cast<std::uint16_t>(size);
    s->packets = packets;
    s->rts_limit = limit;
    report_receive_start(*s, message.time);
    const std::uint32_t generation = s->generation;
    send_cts(*s);
    if (s->state != State::Free && s->generation == generation) // not ended by a full control queue
    {
        announce_receive(*s, message.time);
    }
}

void Transport::send_cts(Session& session)
{
    const auto remaining = static_cast<std::uint16_t>(session.packets - session.next + 1);
    const auto count =
        static_cast<std::uint8_t>(std::min<std::uint16_t>(remaining, std::min(session.rts_limit, options_.cts_window)));
    session.window_start = session.next;
    session.window_end = static_cast<std::uint16_t>(session.next + count - 1);
    session.gap = false;
    session.state = State::CtsPending;
    disarm(session);
    const auto data = control(kCts, count, static_cast<std::uint8_t>(session.next), 0xFF, 0xFF, session.raw_pgn);
    if (!node_of(session).send_transport_control(session.source, data, session.index + 1, session.generation))
    {
        end_receive(session, TransferResult::Cancelled, AbortReason::None, network_.event_time()); // could not be queued: the originator times out
    }
}

void Transport::request_retransmit(Session& session, SimTime time)
{
    if (session.retries >= kMaxRetransmitRequests)
    {
        abort(session, AbortReason::RetransmitLimit, time);
        return;
    }
    ++session.retries;
    ++network_.statistics_.transport_retransmit_requests;
    send_cts(session);
}

void Transport::on_cts(const Message& message, std::int32_t sender)
{
    if (message.destination == kGlobalAddress)
    {
        ++network_.statistics_.transport_frames_ignored;
        return;
    }
    Session* s = find_tx(message.destination, message.source, raw_pgn_of(message.data), sender);
    if (s == nullptr)
    {
        if (for_us(message.destination, sender))
        {
            ++network_.statistics_.transport_frames_ignored;
        }
        return;
    }
    if (s->state == State::Sending)
    {
        abort(*s, AbortReason::CtsWhileSending, message.time);
        return;
    }
    const std::uint8_t count = message.data[1];
    const std::uint16_t first = message.data[2];
    if (count == 0)
    {
        s->state = State::Hold;
        arm(*s, TimerKind::Timeout, message.time + kT4);
        return;
    }
    // Packets before the last window's first one were acknowledged by asking for it; packets
    // after the highest sent one + 1 would skip some.
    if (first < 1 || first > s->packets || first < s->window_start || first > s->sent_high + 1)
    {
        abort(*s, AbortReason::BadSequence, message.time);
        return;
    }
    if (s->rts_limit != 0xFF && count > s->rts_limit)
    {
        abort(*s, AbortReason::Other, message.time);
        return;
    }
    disarm(*s);
    s->window_start = first;
    s->next = first;
    s->window_end = static_cast<std::uint16_t>(std::min<unsigned>(first + count - 1u, s->packets));
    s->state = State::Sending;
}

void Transport::on_eoma(const Message& message, std::int32_t sender)
{
    Session* s = message.destination == kGlobalAddress
                     ? nullptr
                     : find_tx(message.destination, message.source, raw_pgn_of(message.data), sender);
    if (s == nullptr || s->sent_high < s->packets || s->state == State::Sending)
    {
        // Not for one of our connections, or before every packet was sent.
        if (for_us(message.destination, sender))
        {
            ++network_.statistics_.transport_frames_ignored;
        }
        return;
    }
    finish(*s, TransferResult::Completed, AbortReason::None, message.time);
}

void Transport::on_bam(const Message& message, std::int32_t sender)
{
    if (message.destination != kGlobalAddress)
    {
        ++network_.statistics_.transport_frames_ignored;
        return;
    }
    const std::size_t size = static_cast<std::size_t>(message.data[1]) | (static_cast<std::size_t>(message.data[2]) << 8);
    const std::uint8_t packets = message.data[3];
    const std::uint32_t raw = raw_pgn_of(message.data);
    if (size <= can::kMaxPayload || size > kMaxTransportLength || packets != packets_for(size) || !Pgn{raw}.is_valid())
    {
        ++network_.statistics_.transport_rejected; // the running broadcast (if any) goes on
        return;
    }
    if (Session* existing = find_rx(Kind::BamRx, message.source, kGlobalAddress))
    {
        end_receive(*existing, TransferResult::Cancelled, AbortReason::None, message.time); // a new broadcast from the same sender replaces it
    }
    Session* s = allocate(Kind::BamRx);
    if (s == nullptr)
    {
        ++network_.statistics_.transport_rejected;
        return;
    }
    s->node = -1;
    s->source = message.source;
    s->destination = kGlobalAddress;
    s->raw_pgn = raw;
    s->priority = message.priority;
    s->length = static_cast<std::uint16_t>(size);
    s->packets = packets;
    s->rx_sender = sender;
    s->state = State::BamReceiving;
    arm(*s, TimerKind::Timeout, message.time + kT1);
    report_receive_start(*s, message.time);
    announce_receive(*s, message.time);
}

void Transport::on_abort(const Message& message, std::int32_t sender)
{
    if (message.destination == kGlobalAddress)
    {
        ++network_.statistics_.transport_frames_ignored;
        return;
    }
    const std::uint32_t raw = raw_pgn_of(message.data);
    Session* s = find_tx(message.destination, message.source, raw, sender);
    if (s != nullptr)
    {
        ++network_.statistics_.transport_aborts_received;
        finish(*s, TransferResult::AbortedByPeer, static_cast<AbortReason>(message.data[1]), message.time);
        return;
    }
    s = find_rx(Kind::RtsRx, message.source, message.destination);
    if (s != nullptr && s->node != sender && s->raw_pgn == raw)
    {
        ++network_.statistics_.transport_aborts_received;
        end_receive(*s, TransferResult::AbortedByPeer, static_cast<AbortReason>(message.data[1]), message.time);
        return;
    }
    if (for_us(message.destination, sender))
    {
        ++network_.statistics_.transport_frames_ignored; // also aborts of our broadcasts
    }
}

void Transport::on_data(const Message& message, std::int32_t sender)
{
    if (message.data.empty())
    {
        if (for_us(message.destination, sender))
        {
            ++network_.statistics_.transport_frames_ignored;
        }
        return;
    }
    if (message.destination == kGlobalAddress)
    {
        if (Session* s = find_rx(Kind::BamRx, message.source, kGlobalAddress))
        {
            on_bam_data(*s, message, sender);
        }
        else
        {
            ++network_.statistics_.transport_frames_ignored;
        }
        return;
    }
    Session* s = find_rx(Kind::RtsRx, message.source, message.destination);
    if (s == nullptr || s->node == sender)
    {
        if (for_us(message.destination, sender))
        {
            ++network_.statistics_.transport_frames_ignored;
        }
        return;
    }
    on_rts_data(*s, message, sender);
}

bool Transport::store(Session& session, std::uint8_t sequence, std::span<const std::uint8_t> data) noexcept
{
    const std::size_t offset = static_cast<std::size_t>(sequence - 1) * 7;
    const std::size_t count = std::min<std::size_t>(7, session.length - offset);
    if (data.size() < 1 + count)
    {
        return false;
    }
    std::copy_n(data.begin() + 1, count, session.buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

void Transport::deliver(Session& session, SimTime time, std::int32_t sender)
{
    const Pgn pgn{session.raw_pgn};
    if (pgn == kPgnAddressClaimed || pgn == kPgnRequest)
    {
        // Single-frame groups: not taken from the transport protocol.
        end_receive(session, TransferResult::Cancelled, AbortReason::None, time);
        ++network_.statistics_.transport_frames_ignored;
        return;
    }
    const Message message{Pgn{session.raw_pgn}, session.priority, session.source, session.destination, time,
                          std::span<const std::uint8_t>(session.buffer.data(), session.length)};
    // Closed first: a handler may change addresses. The buffer stays as it is until a new
    // session starts, which only a later frame can do.
    end_receive(session, TransferResult::Completed, AbortReason::None, time);
    ++network_.statistics_.transport_messages_received;
    network_.dispatch_message(message, sender, false);
}

void Transport::on_bam_data(Session& session, const Message& message, std::int32_t sender)
{
    const std::uint8_t sequence = message.data[0];
    if (sequence != session.next)
    {
        // A broadcast cannot ask for packets again: a missing, repeated or out-of-order packet
        // loses the message (no abort).
        ++network_.statistics_.transport_bam_dropped;
        end_receive(session, TransferResult::Lost, AbortReason::BadSequence, message.time);
        return;
    }
    if (!store(session, sequence, message.data))
    {
        ++network_.statistics_.transport_frames_ignored; // too short: as if it had not arrived
        return;
    }
    ++session.next;
    if (session.next > session.packets)
    {
        deliver(session, message.time, sender);
        return;
    }
    arm(session, TimerKind::Timeout, message.time + kT1);
}

void Transport::on_rts_data(Session& session, const Message& message, std::int32_t sender)
{
    if (session.state != State::WaitData && session.state != State::Receiving)
    {
        ++network_.statistics_.transport_frames_ignored; // not asked for (yet)
        return;
    }
    session.state = State::Receiving;
    arm(session, TimerKind::Timeout, message.time + kT1);
    const std::uint8_t sequence = message.data[0];
    if (sequence != 0 && sequence < session.window_start)
    {
        // From an earlier window (taken already), e.g. late after a retransmission request.
        ++network_.statistics_.transport_frames_ignored;
        return;
    }
    if (session.gap)
    {
        // A packet was missed: wait for the end of the window, then ask from the first missing one.
        if (sequence == session.window_end)
        {
            request_retransmit(session, message.time);
        }
        return;
    }
    if (sequence == session.next && store(session, sequence, message.data))
    {
        ++session.next;
        if (session.next <= session.window_end)
        {
            return;
        }
        session.retries = 0;
        if (session.next <= session.packets)
        {
            send_cts(session);
            return;
        }
        const auto lo = static_cast<std::uint8_t>(session.length);
        const auto hi = static_cast<std::uint8_t>(session.length >> 8);
        node_of(session).send_transport_control(
            session.source, control(kEndOfMessageAck, lo, hi, session.packets, 0xFF, session.raw_pgn), 0, 0);
        deliver(session, message.time, sender);
        return;
    }
    if (sequence != 0 && sequence + 1 == session.next)
    {
        return; // the previous packet again: ignored
    }
    // Missing packets, an out-of-order or zero sequence number, or a packet too short for its data.
    session.gap = true;
    if (sequence == session.window_end)
    {
        request_retransmit(session, message.time);
    }
}

void Transport::on_timer(Session& session)
{
    session.timer = {};
    switch (session.state)
    {
    case State::BamSending:
        session.ready = true;
        break;
    case State::BamFinished:
        finish(session, TransferResult::Completed, AbortReason::None, session.deadline);
        break;
    case State::RtsSent:
    case State::WaitCts:
    case State::Hold:
    case State::WaitEoma:
    case State::WaitData:
    case State::Receiving:
        ++network_.statistics_.transport_timeouts;
        abort(session, AbortReason::Timeout, session.deadline);
        break;
    case State::BamReceiving:
        ++network_.statistics_.transport_timeouts;
        end_receive(session, TransferResult::Lost, AbortReason::Timeout, session.deadline);
        break;
    default:
        break;
    }
}

} // namespace vehicle_sil::j1939
