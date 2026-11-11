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
#include <type_traits>
#include <utility>
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
    /// kGlobalAddress, or one address for a PDU1 group (PeriodicSender::set_destination()).
    Address destination = kGlobalAddress;
    Priority priority = kDefaultPriority; ///< PeriodicSender::set_priority()
    MissedDeadlines missed_deadlines = MissedDeadlines::Coalesce;
    /// true: does not send while BroadcastSuppression::of(node) is suspended (DM13). false:
    /// sends anyway, and the part does not use BroadcastSuppression. Empty (the default): true
    /// while the destination is kGlobalAddress, false while it is one address (whether DM13
    /// stops those is not settled; choose it here); set_destination() decides again.
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
/// - The data: fixed bytes the sender keeps (set_data()); or an Encoder that writes the message
///   into the sender's buffer at each send and returns its length; or a VectorEncoder that
///   appends the message to a vector, for messages whose length changes from send to send.
/// - trigger() sends at the next chance (for a change that should go out now) and counts the
///   period from there.
/// - The period, the destination and the priority can be changed later (set_period(),
///   set_destination(), set_priority()), also while paused or while the node cannot send: the
///   sender then starts with the new values.
/// - Send failures (SendStatus) are counted, not thrown; the next deadline tries again. An
///   encoder that throws, or makes a message longer than max_length, fails the network. An
///   encoder may destroy its sender (owned by the application): that message is not sent (`out`
///   stays valid until the encoder returns).
///
/// Uses kTimers Timers of the network (Network::timers_free()). A sender that follows DM13 when
/// it is created (`suppressible` true, or empty with destination kGlobalAddress) also uses
/// BroadcastSuppression::of(node), which the constructor attaches if the node has none
/// (BroadcastSuppression::kTimers more). Create it with node.attach<PeriodicSender>(...) or
/// owned by the application (which may destroy it in a handler: Node::attach()). Everything is
/// allowed everywhere on the thread that steps the network (other threads: Host::post()).
class PeriodicSender
{
public:
    /// Writes the message into `out` (max_length bytes) and returns its length.
    using Encoder = std::function<std::size_t(std::span<std::uint8_t> out)>;
    /// Appends the message to `out`, which it gets empty (the sender keeps the vector, with room
    /// for max_length bytes, from send to send: no allocation per send as long as it only
    /// appends; assigning another vector to it gives that room up). Its size is the length.
    using VectorEncoder = std::function<void(std::vector<std::uint8_t>& out)>;
    /// The Timers of the network one sender uses (without BroadcastSuppression's).
    static constexpr std::size_t kTimers = 1;

    /// Sends `data` every `period`. std::invalid_argument for an invalid PGN, period <= 0,
    /// priority > 7, a PDU2 group to one address or a PDU1 group to kNullAddress, max_length
    /// outside 1 .. kMaxTransportLength; std::length_error if `data` is longer than max_length;
    /// std::length_error if the network has no timer left.
    PeriodicSender(Node& node, Pgn pgn, SimDuration period, std::span<const std::uint8_t> data,
                   PeriodicSenderOptions options = {});
    /// Sends what `encoder` writes every `period` (same errors; std::invalid_argument for an
    /// empty encoder).
    PeriodicSender(Node& node, Pgn pgn, SimDuration period, Encoder encoder, PeriodicSenderOptions options = {});
    /// Sends what `encoder` appends every `period` (same errors): a function that cannot be called
    /// with the span of an Encoder, takes a std::vector<std::uint8_t>& (not a copy, not a const& or
    /// an rvalue) and returns nothing. Write the parameter's type (or `auto&`): a function that
    /// takes a span goes to the Encoder constructor and must return the length.
    template <typename F>
        requires(!std::is_invocable_v<F&, std::span<std::uint8_t>>) && std::is_invocable_v<F&, std::vector<std::uint8_t>&> &&
                (!std::is_invocable_v<F&, std::vector<std::uint8_t>&&>) &&
                std::is_void_v<std::invoke_result_t<F&, std::vector<std::uint8_t>&>>
    PeriodicSender(Node& node, Pgn pgn, SimDuration period, F&& encoder, PeriodicSenderOptions options = {})
      : PeriodicSender(node, pgn, period, options, {}, Encoder{}, required(pgn, VectorEncoder(std::forward<F>(encoder))))
    {
    }
    ~PeriodicSender();
    PeriodicSender(const PeriodicSender&) = delete;
    PeriodicSender& operator=(const PeriodicSender&) = delete;

    /// Replaces the data the next sends carry (with an encoder: none; the encoder wins).
    /// std::length_error above max_length.
    void set_data(std::span<const std::uint8_t> data);
    /// Sets the period and counts it from now: the next message goes one new period after the
    /// time of the event being handled (as Timer::start_after(): a handler's message, a timer's
    /// deadline, otherwise the step time), on a new grid; a start at the next chance that is
    /// still to come (trigger(), resume()) stays, and the new period counts from it. To send at
    /// once as well, call trigger(). While paused or while the node cannot send, it only keeps
    /// the period. Also from the encoder (the next deadline then follows the new period).
    /// std::invalid_argument for period <= 0, and nothing changes. Not from an Observer: there it
    /// throws std::logic_error (and fails the network, the period unchanged) while the sender's
    /// timer runs, and only keeps the period while it does not.
    void set_period(SimDuration period);
    /// Sends the next messages to `destination` (kGlobalAddress, or one address for a PDU1
    /// group), on the same grid. With `suppressible` empty, whether DM13 stops them follows the
    /// new destination: a sender that did not follow DM13 and goes to kGlobalAddress uses the
    /// node's BroadcastSuppression (node.find(); it does not attach one, which would take timers
    /// and would not have seen the DM13 that came before). std::invalid_argument as the
    /// constructor (a PDU2 group to one address, a PDU1 group to kNullAddress), std::logic_error
    /// if the node has no BroadcastSuppression then (call BroadcastSuppression::of(node) at
    /// set-up, or set `suppressible`); nothing changes on either.
    void set_destination(Address destination);
    /// Sends the next messages with `priority`. std::invalid_argument above 7, and nothing
    /// changes.
    void set_priority(Priority priority);
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
    [[nodiscard]] Address destination() const noexcept { return options_.destination; }
    [[nodiscard]] Priority priority() const noexcept { return options_.priority; }
    [[nodiscard]] const PeriodicSenderStatistics& statistics() const noexcept { return statistics_; }

private:
    /// Checks everything before it attaches or reserves anything; one of `data`, `encoder` and
    /// `vector_encoder` is used (the public constructors check that an encoder is not empty).
    PeriodicSender(Node& node, Pgn pgn, SimDuration period, PeriodicSenderOptions options,
                   std::span<const std::uint8_t> data, Encoder encoder, VectorEncoder vector_encoder);
    /// `encoder`; std::invalid_argument if it is empty.
    static Encoder required(Pgn pgn, Encoder encoder);
    static VectorEncoder required(Pgn pgn, VectorEncoder encoder);
    /// true when DM13 stops the messages to the current destination.
    [[nodiscard]] bool suppressible() const noexcept;
    enum class Source : std::uint8_t
    {
        Data,
        Encoder,
        Vector,
    };
    void on_deadline();
    /// Starts at the next chance (trigger(), resume(), creation).
    void restart();
    /// Sends, or counts the deadline as suppressed (DM13). false if the encoder destroyed the
    /// sender (touch nothing then).
    bool send_or_suppress();
    bool send_now();

    Node& node_;
    Pgn pgn_;
    SimDuration period_;
    PeriodicSenderOptions options_;
    Encoder encoder_;
    VectorEncoder vector_encoder_;
    Source source_;
    /// The message: max_length bytes for the fixed data and an Encoder; for a VectorEncoder the
    /// vector it appends to (capacity max_length).
    std::vector<std::uint8_t> buffer_;
    std::size_t length_ = 0;
    /// The node's BroadcastSuppression while the sender follows DM13 or did once; else nullptr.
    BroadcastSuppression* suppression_ = nullptr;
    bool paused_ = false;
    bool restarted_ = false; ///< The timer runs for a restart (see on_deadline()).
    PeriodicSenderStatistics statistics_;
    bool* gone_ = nullptr; ///< Set while the encoder runs: tells it the sender is gone.
    Timer timer_;
    Subscription address_;
};

} // namespace vehicle_sil::j1939
