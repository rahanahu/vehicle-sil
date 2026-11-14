// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/transfer_sniffer.hpp"

#include "transport_wire.hpp"

#include <algorithm>
#include <bitset>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::j1939 {

using namespace transport_wire;

struct TransferSniffer::Session
{
    bool active = false;
    bool broadcast = false;
    /// An RTS for another group was ignored: only an acknowledgement completes the transfer.
    bool ignored = false;
    /// The receiver acknowledged while packets were missing: decided when the time moves on.
    bool acknowledged = false;
    /// `completed_at` is set (see on_data()).
    bool settled = false;
    Address source = kNullAddress;
    Address destination = kGlobalAddress;
    Priority priority = kDefaultPriority;
    std::uint32_t raw_pgn = 0;
    std::uint32_t ignored_pgn = 0;
    std::uint16_t length = 0;
    std::uint16_t packets = 0;
    std::uint16_t seen = 0;          ///< different packets stored
    std::uint16_t next = 1;          ///< broadcast: the packet expected
    std::uint16_t highest = 0;       ///< the highest sequence number seen
    std::uint16_t requested_end = 0; ///< the last packet the latest clear-to-send asked for
    SimTime started{};
    SimTime last{};            ///< the last frame that keeps the transfer alive
    SimTime last_data{};       ///< the last data packet
    SimTime completed_at{};    ///< the packet with which the receiver had what it asked for
    SimTime acknowledged_at{}; ///< with `acknowledged`
    std::bitset<256> have;
    std::bitset<256> latest;   ///< the packets with the time `last_data`
    std::vector<std::uint8_t> buffer;
};

namespace {

struct Flag
{
    explicit Flag(bool& flag) noexcept : flag_(flag) { flag_ = true; }
    ~Flag() { flag_ = false; }
    Flag(const Flag&) = delete;
    Flag& operator=(const Flag&) = delete;

private:
    bool& flag_;
};

[[nodiscard]] std::string nanoseconds(SimDuration d) { return std::to_string(d.count()) + " ns"; }

} // namespace

TransferSniffer::TransferSniffer(SnifferOptions options) : options_(options)
{
    if (options_.sessions == 0)
    {
        throw std::invalid_argument("vehicle_sil::j1939::TransferSniffer: SnifferOptions::sessions is 0. Give the number "
                                    "of transfers to follow at the same time (at least 1; the default is 32)");
    }
    if (options_.timeout <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::TransferSniffer: SnifferOptions::timeout is " +
                                    nanoseconds(options_.timeout) + ". It must be greater than 0 (the default is 1250 ms)");
    }
    if (options_.broadcast_timeout <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::TransferSniffer: SnifferOptions::broadcast_timeout is " +
                                    nanoseconds(options_.broadcast_timeout) +
                                    ". It must be greater than 0 (the default is 750 ms)");
    }
    if (options_.grace < SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::j1939::TransferSniffer: SnifferOptions::grace is " +
                                    nanoseconds(options_.grace) + ". It must be 0 or more (the default is 250 ms)");
    }
    sessions_.resize(options_.sessions);
    for (Session& s : sessions_)
    {
        s.buffer.resize(kMaxTransportLength);
    }
}

TransferSniffer::~TransferSniffer() = default;

void TransferSniffer::check_not_in_handler(const char* what) const
{
    if (in_handler_)
    {
        throw std::logic_error(std::string("vehicle_sil::j1939::TransferSniffer: ") + what +
                               " was called from the on_transfer handler. Copy what the handler needs and call it "
                               "after feed() or advance() has returned");
    }
}

void TransferSniffer::on_transfer(Handler handler)
{
    check_not_in_handler("on_transfer()");
    handler_ = std::move(handler);
}

// --- time ---------------------------------------------------------------------------------

bool TransferSniffer::expired(const Session& session, SimTime time) const noexcept
{
    // "Longer than": a frame exactly at the timeout still belongs to the transfer, as the
    // engine handles a frame before a timer that is due at the frame's time. For a connection
    // the grace is added: the ECU that gives up sends its abort after its own timeout, and that
    // abort is to end the transfer, not to find it gone. Decided by the frame times alone, so
    // the result does not depend on when advance() is called.
    const SimDuration limit = session.broadcast ? options_.broadcast_timeout : options_.timeout + options_.grace;
    return time - session.last > limit;
}

void TransferSniffer::time_out(Session& session)
{
    end(session, SniffEnd::TimedOut, session.last + (session.broadcast ? options_.broadcast_timeout : options_.timeout));
}

void TransferSniffer::move_time(SimTime time)
{
    if (!started_ || time > now_)
    {
        now_ = time;
        started_ = true;
    }
    for (Session& s : sessions_)
    {
        if (!s.active)
        {
            continue;
        }
        if (s.acknowledged)
        {
            // Packets with the time of the acknowledgement could still fill the gaps; a later
            // time means they will not.
            if (time > s.acknowledged_at)
            {
                end(s, SniffEnd::Incomplete, s.acknowledged_at);
            }
            continue;
        }
        if (expired(s, time))
        {
            time_out(s);
        }
    }
}

void TransferSniffer::advance(SimTime now)
{
    check_not_in_handler("advance()");
    if (started_ && now < now_)
    {
        return;
    }
    move_time(now);
}

// --- frames -------------------------------------------------------------------------------

TransferSniffer::Session* TransferSniffer::find(Address source, Address destination) noexcept
{
    for (Session& s : sessions_)
    {
        if (s.active && s.source == source && s.destination == destination)
        {
            return &s;
        }
    }
    return nullptr;
}

void TransferSniffer::feed(const SniffFrame& frame)
{
    check_not_in_handler("feed()");
    // First the time: transfers that stopped make room before a new one looks for a place.
    move_time(frame.time);
    if (frame.pgn == kPgnTransportControl)
    {
        on_control(frame);
    }
    else if (frame.pgn == kPgnTransportData)
    {
        on_data(frame);
    }
}

void TransferSniffer::feed(const can::TimedFrame& frame)
{
    const auto id = Identifier::decode(frame.frame);
    if (!id)
    {
        check_not_in_handler("feed()");
        move_time(frame.time);
        return;
    }
    feed(SniffFrame{frame.time, id->pgn, id->priority, id->source, id->destination, frame.frame.payload()});
}

void TransferSniffer::end(Session& session, SniffEnd how, SimTime time, std::uint8_t reason)
{
    // Released first: the handler sees a sniffer without this transfer. The buffer stays as it
    // is until a later frame starts a transfer in it.
    session.active = false;
    --active_;
    switch (how)
    {
    case SniffEnd::Completed:
        ++statistics_.completed;
        break;
    case SniffEnd::Incomplete:
        ++statistics_.incomplete;
        break;
    case SniffEnd::AbortedBySender:
        ++statistics_.aborted_by_sender;
        break;
    case SniffEnd::AbortedByReceiver:
        ++statistics_.aborted_by_receiver;
        break;
    case SniffEnd::Replaced:
        ++statistics_.replaced;
        break;
    case SniffEnd::Lost:
        ++statistics_.lost;
        break;
    case SniffEnd::TimedOut:
        ++statistics_.timed_out;
        break;
    case SniffEnd::Refused:
        ++statistics_.refused;
        break;
    }
    if (!handler_)
    {
        return;
    }
    SniffedTransfer transfer;
    transfer.end = how;
    transfer.pgn = Pgn{session.raw_pgn};
    transfer.priority = session.priority;
    transfer.source = session.source;
    transfer.destination = session.destination;
    transfer.size = session.length;
    transfer.started = session.started;
    transfer.time = time;
    transfer.noticed = now_;
    transfer.reason = reason;
    if (how == SniffEnd::Completed)
    {
        transfer.data = std::span<const std::uint8_t>(session.buffer.data(), session.length);
    }
    const Flag calling(in_handler_);
    handler_(transfer);
}

void TransferSniffer::complete(Session& session, SimTime time)
{
    // The same decision as the engine's receiver makes before it hands the message on.
    end(session, single_frame_only(Pgn{session.raw_pgn}) ? SniffEnd::Refused : SniffEnd::Completed, time);
}

void TransferSniffer::start(const SniffFrame& frame, bool broadcast)
{
    const auto free = std::find_if(sessions_.begin(), sessions_.end(), [](const Session& s) { return !s.active; });
    if (free == sessions_.end())
    {
        ++statistics_.no_session;
        return;
    }
    Session& s = *free;
    s.active = true;
    s.broadcast = broadcast;
    s.ignored = false;
    s.acknowledged = false;
    s.settled = false;
    s.source = frame.source;
    s.destination = frame.destination;
    s.priority = frame.priority;
    s.raw_pgn = raw_pgn_of(frame.data);
    s.ignored_pgn = 0;
    s.length = static_cast<std::uint16_t>(announced_size(frame.data));
    s.packets = frame.data[3];
    s.seen = 0;
    s.next = 1;
    s.highest = 0;
    s.requested_end = s.packets;
    s.started = frame.time;
    s.last = frame.time;
    s.last_data = frame.time;
    s.have.reset();
    s.latest.reset();
    ++active_;
    ++statistics_.started;
}

void TransferSniffer::on_control(const SniffFrame& frame)
{
    if (frame.data.size() < 8)
    {
        return;
    }
    switch (frame.data[0])
    {
    case kRts:
        on_rts(frame);
        break;
    case kBam:
        on_bam(frame);
        break;
    case kCts:
        on_cts(frame);
        break;
    case kEndOfMessageAck:
        on_acknowledgement(frame);
        break;
    case kAbort:
        on_abort(frame);
        break;
    default:
        break;
    }
}

void TransferSniffer::on_rts(const SniffFrame& frame)
{
    if (frame.destination == kGlobalAddress)
    {
        ++statistics_.announcements_malformed;
        return;
    }
    const std::uint32_t raw = raw_pgn_of(frame.data);
    const bool wellFormed = check_rts(announced_size(frame.data), frame.data[3], frame.data[4], raw) == Shape::Ok;
    // In the order of the engine's receiver: what the announcement does to the running transfer
    // is decided before its shape is checked.
    if (Session* existing = find(frame.source, frame.destination))
    {
        if (existing->raw_pgn != raw)
        {
            if (options_.other_group == SniffSecondAnnouncement::KeepRunning)
            {
                ++statistics_.announcements_ignored;
                existing->ignored = true;
                existing->ignored_pgn = raw;
                return;
            }
            if (!wellFormed)
            {
                ++statistics_.announcements_malformed;
                return;
            }
        }
        end(*existing, SniffEnd::Replaced, frame.time);
    }
    if (!wellFormed)
    {
        ++statistics_.announcements_malformed;
        return;
    }
    start(frame, false);
}

void TransferSniffer::on_bam(const SniffFrame& frame)
{
    if (!options_.broadcasts)
    {
        return;
    }
    if (frame.destination != kGlobalAddress ||
        check_bam(announced_size(frame.data), frame.data[3], raw_pgn_of(frame.data)) != Shape::Ok)
    {
        ++statistics_.announcements_malformed; // the running broadcast (if any) goes on
        return;
    }
    if (Session* existing = find(frame.source, kGlobalAddress))
    {
        end(*existing, SniffEnd::Replaced, frame.time);
    }
    start(frame, true);
}

void TransferSniffer::on_cts(const SniffFrame& frame)
{
    // From the receiver to the sender.
    Session* s = frame.destination == kGlobalAddress ? nullptr : find(frame.destination, frame.source);
    if (s == nullptr || s->raw_pgn != raw_pgn_of(frame.data))
    {
        ++statistics_.orphan_control;
        return;
    }
    s->last = frame.time;
    const std::uint8_t count = frame.data[1];
    const std::uint8_t first = frame.data[2];
    if (count == 0)
    {
        return; // holds the connection
    }
    if (first <= s->highest)
    {
        ++statistics_.retransmit_requests;
    }
    // The data is stored by number whatever was asked for. The request only tells when the
    // receiver has everything: with the last packet it asked for (see on_data()).
    s->requested_end = static_cast<std::uint16_t>(std::min<unsigned>(first + count - 1u, s->packets));
    // A packet with the time of the request counts as its answer, whichever of the two frames
    // came first here: the receiver handles the frames of that time after it has sent the request.
    s->settled = s->seen == s->packets && s->last_data == frame.time && s->latest.test(s->requested_end);
    s->completed_at = frame.time;
}

void TransferSniffer::on_acknowledgement(const SniffFrame& frame)
{
    Session* s = frame.destination == kGlobalAddress ? nullptr : find(frame.destination, frame.source);
    if (s == nullptr || s->raw_pgn != raw_pgn_of(frame.data))
    {
        ++statistics_.orphan_control;
        return;
    }
    if (s->seen == s->packets)
    {
        complete(*s, s->settled ? s->completed_at : s->last_data);
        return;
    }
    if (!s->acknowledged)
    {
        s->acknowledged = true;
        s->acknowledged_at = frame.time;
    }
}

void TransferSniffer::on_abort(const SniffFrame& frame)
{
    if (frame.destination == kGlobalAddress)
    {
        ++statistics_.orphan_control;
        return;
    }
    const std::uint32_t raw = raw_pgn_of(frame.data);
    // The receiver's abort first (the transfer to the sender of this frame), as the engine
    // looks for the transfer it sends first.
    Session* toSender = find(frame.destination, frame.source);
    if (toSender != nullptr && toSender->raw_pgn == raw)
    {
        end(*toSender, SniffEnd::AbortedByReceiver, frame.time, frame.data[1]);
        return;
    }
    if (Session* fromSender = find(frame.source, frame.destination); fromSender != nullptr && fromSender->raw_pgn == raw)
    {
        end(*fromSender, SniffEnd::AbortedBySender, frame.time, frame.data[1]);
        return;
    }
    if (toSender != nullptr && toSender->ignored && toSender->ignored_pgn == raw)
    {
        return; // the receiver refuses the announcement that was ignored here too
    }
    ++statistics_.orphan_control;
}

void TransferSniffer::on_data(const SniffFrame& frame)
{
    Session* s = find(frame.source, frame.destination);
    if (s == nullptr)
    {
        ++statistics_.orphan_data;
        return;
    }
    const std::uint8_t sequence = frame.data.empty() ? std::uint8_t{0} : frame.data[0];
    if (s->broadcast)
    {
        if (frame.data.empty())
        {
            ++statistics_.bad_packets;
            return;
        }
        if (sequence != s->next)
        {
            // A broadcast cannot ask for packets again.
            end(*s, SniffEnd::Lost, frame.time);
            return;
        }
    }
    else
    {
        s->last = frame.time; // any packet keeps a connection alive, as for the engine's receiver
        if (sequence == 0 || sequence > s->packets)
        {
            ++statistics_.bad_packets;
            return;
        }
    }
    const std::size_t offset = static_cast<std::size_t>(sequence - 1) * 7;
    const std::size_t count = std::min<std::size_t>(7, s->length - offset);
    if (frame.data.size() < 1 + count)
    {
        ++statistics_.bad_packets; // too short: as if it had not arrived
        return;
    }
    const auto at = s->buffer.begin() + static_cast<std::ptrdiff_t>(offset);
    if (s->have.test(sequence))
    {
        // The first one stays: the receiver took it (or asks again, and gets the same bytes).
        ++statistics_.repeated_packets;
        if (!std::equal(frame.data.begin() + 1, frame.data.begin() + 1 + static_cast<std::ptrdiff_t>(count), at))
        {
            ++statistics_.conflicting_packets;
        }
    }
    else
    {
        std::copy_n(frame.data.begin() + 1, count, at);
        s->have.set(sequence);
        ++s->seen;
    }
    s->highest = std::max<std::uint16_t>(s->highest, sequence);
    if (s->last_data != frame.time)
    {
        s->latest.reset();
    }
    s->latest.set(sequence);
    s->last = frame.time;
    s->last_data = frame.time;
    const bool all = s->seen == s->packets;
    if (s->broadcast)
    {
        ++s->next;
        if (all)
        {
            complete(*s, frame.time);
        }
        return;
    }
    if (!all)
    {
        return;
    }
    // The receiver has the message with the last packet it asked for, which can be later than
    // the packet that filled the last gap here (it asks again from the first packet it missed).
    // That packet's time is the time of the receiver's message.
    if (!s->settled && sequence == s->requested_end)
    {
        s->settled = true;
        s->completed_at = frame.time;
    }
    if (s->acknowledged || (options_.delivery == SniffDelivery::WhenAllPacketsSeen && !s->ignored))
    {
        complete(*s, frame.time);
    }
}

} // namespace vehicle_sil::j1939
