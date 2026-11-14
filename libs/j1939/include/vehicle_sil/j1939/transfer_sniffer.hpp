// SPDX-License-Identifier: MIT
//
// TransferSniffer: follows the transport protocol transfers of other ECUs from the frames on
// the bus and reports how each one ended, with the assembled message if it completed. It only
// listens: it sends nothing, has no timers and reads no clock.
//
// Depends on the value types only (types.hpp): no Network, so it also works on raw CAN frames
// (can::RawChannel) without the rest of the stack.
//
// NOT COMPARED WITH THE STANDARD. The rules below follow the receiver of this repository's
// engine (a completed transfer gives the message the receiving node of the engine gets). Where
// another stack may differ, SnifferOptions has a value to change: the timeouts, when a
// transfer counts as complete and what a second announcement does.
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace vehicle_sil::j1939 {

/// One frame of the bus with its 29-bit identifier decoded (a frame, not an assembled message).
struct SniffFrame
{
    SimTime time{};
    Pgn pgn{};
    Priority priority = kDefaultPriority;
    Address source = kNullAddress;
    Address destination = kGlobalAddress;
    std::span<const std::uint8_t> data{}; ///< Read during the call only.
};

/// How a followed transfer ended.
enum class SniffEnd : std::uint8_t
{
    Completed,         ///< Every packet was seen (and, by default, the receiver acknowledged): `data` is the message.
    Incomplete,        ///< The receiver acknowledged, but the sniffer missed packets.
    AbortedBySender,   ///< Connection Abort from the sender; `reason` is its reason byte.
    AbortedByReceiver, ///< Connection Abort from the receiver; `reason` is its reason byte.
    Replaced,          ///< The sender announced again before the transfer ended.
    Lost,              ///< A broadcast with a missing, repeated or out-of-order packet.
    TimedOut,          ///< No frame of the transfer for too long.
    Refused,           ///< Complete, but of a group that is single-frame only (Request, Address
                       ///< Claimed): the engine's receiver does not hand such a message on either.
};

/// A transfer that ended.
///
/// The three times:
/// - `started`: the time of the announcement (RTS or BAM).
/// - `time`: when the transfer ended on the bus. Use this one in conditions; it follows from the
///   frame times alone. Completed and Refused: the time of the data packet with which the
///   receiver had the message, which is the time of the receiver's message (after a
///   retransmission: the last packet the receiver asked for). Incomplete: the time of the
///   acknowledgement. AbortedBySender, AbortedByReceiver, Replaced, Lost: the time of the frame
///   that ended it. TimedOut: the time of the transfer's last frame + the timeout
///   (SnifferOptions::timeout or, for a broadcast, broadcast_timeout; without the grace).
/// - `noticed`: the time at which the sniffer reported it (the frame or advance() that made it
///   decide). It depends on the listener's step size and on what else is on the bus.
struct SniffedTransfer
{
    SniffEnd end = SniffEnd::Completed;
    Pgn pgn{};
    Priority priority = kDefaultPriority; ///< Of the announcement.
    Address source = kNullAddress;
    Address destination = kGlobalAddress; ///< kGlobalAddress for a broadcast.
    std::size_t size = 0;                 ///< The announced size in bytes.
    SimTime started{};
    SimTime time{};
    SimTime noticed{};
    std::uint8_t reason = 0;              ///< AbortedBySender, AbortedByReceiver: the reason byte; else 0.
    /// Completed: the message (`size` bytes), valid during the handler call only. Else empty.
    std::span<const std::uint8_t> data{};

    /// The transfer as a Message. For a Completed transfer it has the fields of the message the
    /// receiving node of this repository's engine gets for the same frames: group, priority (of
    /// the announcement), source, destination, time (of the packet that completed it) and bytes.
    /// Code that reads a Message from a node's handler can read this one. `data` stays valid
    /// during the handler call only.
    [[nodiscard]] Message message() const noexcept { return Message{pgn, priority, source, destination, time, data}; }
};

/// When a connection (RTS/CTS) counts as complete.
enum class SniffDelivery : std::uint8_t
{
    /// When the receiver acknowledged the message (End of Message Acknowledgement) and every
    /// packet was seen. What is reported is what the receiver took.
    WhenAcknowledged,
    /// As soon as every packet was seen, without waiting for the acknowledgement (for receivers
    /// whose acknowledgement cannot be relied on). The frames of the transfer that follow
    /// (the acknowledgement, repeated packets) then find no transfer and are counted in
    /// orphan_control and orphan_data, and `time` is the time of the packet that filled the
    /// last gap, which can be earlier than the time of the receiver's message.
    WhenAllPacketsSeen,
};

/// What an RTS for another group does while a connection of the same sender and receiver runs.
enum class SniffSecondAnnouncement : std::uint8_t
{
    /// The running transfer goes on and the new announcement is ignored (counted in
    /// announcements_ignored), as the engine's receiver refuses it. A transfer during which an
    /// announcement was ignored completes only by an acknowledgement for its group, also with
    /// SniffDelivery::WhenAllPacketsSeen: the packets may belong to the ignored message.
    KeepRunning,
    /// The new announcement replaces the running transfer (which ends as Replaced).
    Replace,
};

/// The defaults are the values of this repository's engine. They were not compared with the
/// standard: change them if the ECUs on your bus behave differently.
struct SnifferOptions
{
    /// The transfers followed at the same time (connections and broadcasts together). The
    /// constructor reserves sessions x kMaxTransportLength bytes; nothing is allocated later.
    /// An announcement that finds no free place is counted in no_session and not followed.
    std::size_t sessions = 32;
    /// A connection without a frame for longer than this is over.
    SimDuration timeout = std::chrono::milliseconds(1250);
    /// How much longer the sniffer waits before it reports a connection as TimedOut. The ECU
    /// that gives up sends a Connection Abort, which arrives a little after the timeout (its
    /// step, the transmission, the listener's step); within the grace it ends the transfer as
    /// AbortedBySender or AbortedByReceiver instead. Raise it for step sizes above a few 10 ms.
    SimDuration grace = std::chrono::milliseconds(250);
    /// A broadcast without a packet for longer than this is over (no grace: nobody aborts one).
    SimDuration broadcast_timeout = std::chrono::milliseconds(750);
    SniffDelivery delivery = SniffDelivery::WhenAcknowledged;
    SniffSecondAnnouncement other_group = SniffSecondAnnouncement::KeepRunning;
    /// Also follow broadcasts (BAM). Off by default: a Network assembles the broadcasts itself
    /// and hands them to its handlers. Turn it on when feeding raw CAN frames.
    bool broadcasts = false;
};

/// Counters since construction. The names are stable: tools show them and scenarios may name
/// variables after them.
struct SnifferStatistics
{
    std::uint64_t started = 0;             ///< Transfers followed (announcements taken).
    // One per ended transfer, by SniffEnd:
    std::uint64_t completed = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t aborted_by_sender = 0;
    std::uint64_t aborted_by_receiver = 0;
    std::uint64_t replaced = 0;
    std::uint64_t lost = 0;
    std::uint64_t timed_out = 0;
    std::uint64_t refused = 0;
    // Announcements not followed:
    std::uint64_t announcements_malformed = 0; ///< An RTS or BAM the engine's receiver would not accept.
    std::uint64_t announcements_ignored = 0;   ///< An RTS for another group during a transfer (KeepRunning).
    std::uint64_t no_session = 0;              ///< No free place (SnifferOptions::sessions).
    // Frames that belong to no followed transfer: the sniffer started listening in the middle
    // of a transfer, the transfer has ended already, or the announcement was not followed.
    std::uint64_t orphan_data = 0;    ///< TP.DT.
    std::uint64_t orphan_control = 0; ///< Clear-to-send, acknowledgement, abort.
    // Within followed transfers:
    std::uint64_t bad_packets = 0;         ///< Sequence number 0 or beyond the last packet, or too short for its data.
    std::uint64_t repeated_packets = 0;    ///< A packet seen before (the first one is kept).
    std::uint64_t conflicting_packets = 0; ///< A repeated packet whose bytes differ from the first one.
    std::uint64_t retransmit_requests = 0; ///< A clear-to-send for packets that were sent already.

    [[nodiscard]] friend bool operator==(const SnifferStatistics&, const SnifferStatistics&) noexcept = default;
};

/// Follows the transfers on a bus from its frames.
///
/// Feed every frame in the order of the frame times (the sniffer does not sort) and call
/// advance() once per step, so that transfers that stopped are reported even when the bus is
/// silent. Results depend only on the frames and their times, not on when advance() is called
/// (except SniffedTransfer::noticed).
///
/// Rules (those of the engine's receiver unless said otherwise):
/// - A transfer is known by its sender and receiver (a broadcast: the sender). An RTS of the
///   same sender, receiver and group replaces the running transfer; for another group see
///   SniffSecondAnnouncement.
/// - Data packets are stored by their sequence number, in any order; of a repeated packet the
///   first one is kept. Clear-to-send frames are not needed to follow the data, so
///   retransmissions and windows of any size are followed; they are read only to tell with
///   which packet the receiver had the message (SniffedTransfer::time). A broadcast takes its
///   packets in order only and is Lost otherwise.
/// - Timeouts compare with "longer than": a frame exactly at the timeout still belongs to the
///   transfer. A connection is reported as TimedOut only after the timeout and the grace, so
///   that the abort of the ECU that gave up ends it (SnifferOptions::grace).
/// - An acknowledgement that arrives while packets are missing is decided when the time moves
///   on: packets with the same time as the acknowledgement still count (frames with equal
///   times may arrive in either order).
/// - Not known (the standard was not read): which transfer an abort ends when the same two
///   ECUs send the same group to each other at the same time (here: the transfer to the sender
///   of the abort first), and whether the size in an acknowledgement must match the
///   announcement (not checked here).
///
/// No allocation after the constructor. Not thread safe: use it from one thread (the step).
class TransferSniffer
{
public:
    using Handler = std::function<void(const SniffedTransfer&)>;

    /// Throws std::invalid_argument if sessions is 0, timeout or broadcast_timeout is not
    /// positive, or grace is negative.
    explicit TransferSniffer(SnifferOptions options = {});
    ~TransferSniffer();
    TransferSniffer(const TransferSniffer&) = delete;
    TransferSniffer& operator=(const TransferSniffer&) = delete;

    /// Sets the handler called for every transfer that ends, from feed() or advance(). The
    /// handler must not call feed(), advance() or on_transfer() of this sniffer
    /// (std::logic_error). An exception of the handler leaves feed() or advance(); the transfer
    /// it was called for has ended.
    void on_transfer(Handler handler);

    /// One frame of the bus. Frames other than TP.CM and TP.DT only move the time on.
    void feed(const SniffFrame& frame);
    /// A raw CAN frame. Only 29-bit frames are read; an 11-bit frame only moves the time on.
    void feed(const can::TimedFrame& frame);

    /// Moves the time on to `now` without a frame: reports the transfers that timed out and
    /// decides the acknowledgements that waited for the time to move. A `now` before the latest
    /// time seen does nothing.
    void advance(SimTime now);

    /// The transfers followed right now.
    [[nodiscard]] std::size_t active() const noexcept { return active_; }
    [[nodiscard]] const SnifferStatistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] const SnifferOptions& options() const noexcept { return options_; }

private:
    struct Session;

    [[nodiscard]] Session* find(Address source, Address destination) noexcept;
    void check_not_in_handler(const char* what) const;
    void move_time(SimTime time);
    [[nodiscard]] bool expired(const Session& session, SimTime time) const noexcept;
    void time_out(Session& session);
    void start(const SniffFrame& frame, bool broadcast);
    void end(Session& session, SniffEnd how, SimTime time, std::uint8_t reason = 0);
    void complete(Session& session, SimTime time);
    void on_control(const SniffFrame& frame);
    void on_rts(const SniffFrame& frame);
    void on_bam(const SniffFrame& frame);
    void on_cts(const SniffFrame& frame);
    void on_acknowledgement(const SniffFrame& frame);
    void on_abort(const SniffFrame& frame);
    void on_data(const SniffFrame& frame);

    SnifferOptions options_;
    std::vector<Session> sessions_;
    Handler handler_;
    SnifferStatistics statistics_;
    std::size_t active_ = 0;
    SimTime now_{};
    bool started_ = false; ///< a time was seen
    bool in_handler_ = false;
};

} // namespace vehicle_sil::j1939
