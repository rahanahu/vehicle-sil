// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack: sends one parameter group of a node periodically, while
// the node can send from its address. Built on the node's public API and the toolbox only.
#pragma once

#include "vehicle_sil/j1939/toolbox.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace vehicle_sil::j1939 {

class BroadcastSuppression;

/// What a PeriodicSender does with deadlines that fall into the same step (only when the step
/// is longer than the period: with step <= period every step has at most one deadline, and both
/// send once per deadline).
enum class MissedDeadlines : std::uint8_t
{
    /// Send once for all of them (counted in PeriodicSenderStatistics::coalesced); the next
    /// deadline is the first one after the step. The deadlines stay on their grid.
    Coalesce,
    /// Send once per deadline: several messages with the same time in one step. A message longer
    /// than 8 bytes cannot start while the one before is still being sent (counted as a failure).
    CatchUp,
};

struct PeriodicSenderOptions
{
    /// kGlobalAddress, or one address for a PDU1 group.
    Address destination = kGlobalAddress;
    Priority priority = kDefaultPriority;
    MissedDeadlines missed_deadlines = MissedDeadlines::Coalesce;
    /// true: does not send while BroadcastSuppression::of(node) is suspended (DM13). false:
    /// sends anyway, and the part does not use BroadcastSuppression. Empty (the default): true
    /// for a broadcast (destination kGlobalAddress), false for a group sent to one address
    /// (whether DM13 stops those is not settled; choose it here).
    std::optional<bool> suppressible{};
    /// The longest message (bytes; 9 .. kMaxTransportLength travel with the transport protocol).
    /// Its buffer is allocated when the sender is created.
    std::size_t max_length = 8;
};

struct PeriodicSenderStatistics
{
    std::uint64_t sent = 0;       ///< Node::send() returned Ok.
    std::uint64_t suppressed = 0; ///< Deadlines not sent because of BroadcastSuppression.
    std::uint64_t coalesced = 0;  ///< Deadlines merged into the send of an earlier one (Coalesce).
    std::uint64_t failures = 0;   ///< Node::send() returned another status (last_failure).
    SendStatus last_failure = SendStatus::Ok;
};

/// Sends `pgn` from the node every `period`:
///
///     auto& eec1 = ecu.attach<PeriodicSender>(kPgnEec1, 10ms, [&](std::span<std::uint8_t> out) {
///         return engine.encode_eec1(out); // bytes written
///     });
///
/// - Only while the node can send from its address (Node::can_send_now(): a node without a
///   NAME, a Claimed one, or a Claiming one that does not wait after its claim; never during the
///   wait after the claim, when the engine would hold the messages). The first message goes out
///   when the node gets there (in that step; at the next chance for a node that is there
///   already), then every period, counted on a grid from there: Timer deadlines in event time,
///   so the deadlines do not drift whatever the step size. A message leaves in the step of its
///   deadline (the first step at or after it), with that step's time. When the node loses or
///   changes its address, the sender stops; it starts again when the node can send.
/// - Deadlines (the first message too) that pass while the node's broadcasts are suspended
///   (DM13; by default only for a broadcast, see `suppressible`) are not sent and not made up
///   for.
/// - A start at the next chance (trigger(), resume(), creation) from a time already handled (the
///   application stage, outside a step) goes out in the next step; if a period has passed by
///   then, the period counts from that step (nothing is merged or caught up for it).
/// - The data: either fixed bytes the sender keeps (set_data()), or an Encoder that writes the
///   message into the sender's buffer at each send and returns its length.
/// - trigger() sends at the next chance (for a change that should go out now) and counts the
///   period from there.
/// - Send failures (SendStatus) are counted, not thrown; the next deadline tries again. An
///   Encoder that throws, or returns more than max_length, fails the network.
///
/// Uses one Timer of the network (NetworkOptions::timer_capacity). Create it with
/// node.attach<PeriodicSender>(...) or owned by the application. Everything is allowed
/// everywhere on the thread that steps the network (other threads: Host::post()).
class PeriodicSender
{
public:
    /// Writes the message into `out` (max_length bytes) and returns its length.
    using Encoder = std::function<std::size_t(std::span<std::uint8_t> out)>;

    /// Sends `data` every `period`. std::invalid_argument for an invalid PGN, period <= 0,
    /// priority > 7, a PDU2 group to one address or a PDU1 group to kNullAddress, max_length
    /// outside 1 .. kMaxTransportLength; std::length_error if `data` is longer than max_length;
    /// std::length_error if the network has no timer left.
    PeriodicSender(Node& node, Pgn pgn, SimDuration period, std::span<const std::uint8_t> data,
                   PeriodicSenderOptions options = {});
    /// Sends what `encoder` writes every `period` (same errors; std::invalid_argument for an
    /// empty encoder).
    PeriodicSender(Node& node, Pgn pgn, SimDuration period, Encoder encoder, PeriodicSenderOptions options = {});
    PeriodicSender(const PeriodicSender&) = delete;
    PeriodicSender& operator=(const PeriodicSender&) = delete;

    /// Replaces the data the next sends carry (with an Encoder: none; the encoder wins).
    /// std::length_error above max_length.
    void set_data(std::span<const std::uint8_t> data);
    /// Sends at the next chance and counts the period from there: in a handler, in this step; in a
    /// Timer's callback, in this step or the next (a timer started at the deadline being fired
    /// waits for the next round of firing); in the application stage or outside a step, in the
    /// next step (to send at once there, use Node::send()). false (nothing scheduled) while the
    /// sender is paused or the node cannot send.
    bool trigger();
    /// Stops sending until resume() (keeps the data and the statistics).
    void pause() noexcept;
    /// Sends again from the next chance (once the node can send).
    void resume();
    [[nodiscard]] bool paused() const noexcept { return paused_; }

    [[nodiscard]] Pgn pgn() const noexcept { return pgn_; }
    [[nodiscard]] SimDuration period() const noexcept { return period_; }
    [[nodiscard]] const PeriodicSenderStatistics& statistics() const noexcept { return statistics_; }

private:
    PeriodicSender(Node& node, Pgn pgn, SimDuration period, PeriodicSenderOptions options, Encoder encoder);
    void on_deadline();
    /// Starts at the next chance (trigger(), resume(), creation).
    void restart();
    /// Sends, or counts the deadline as suppressed (DM13).
    void send_or_suppress();
    void send_now();

    Node& node_;
    Pgn pgn_;
    SimDuration period_;
    PeriodicSenderOptions options_;
    Encoder encoder_;
    std::vector<std::uint8_t> buffer_;
    std::size_t length_ = 0;
    BroadcastSuppression* suppression_ = nullptr;
    bool paused_ = false;
    bool restarted_ = false; ///< The timer runs for a restart (see on_deadline()).
    PeriodicSenderStatistics statistics_;
    Timer timer_;
    Subscription address_;
};

} // namespace vehicle_sil::j1939
