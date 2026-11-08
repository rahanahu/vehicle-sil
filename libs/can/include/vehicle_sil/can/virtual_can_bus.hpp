// SPDX-License-Identifier: MIT
//
// In-process deterministic CAN bus for tests and single-process simulations (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/bus_model.hpp"
#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/overflow_collector.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/core/step_inbox.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace vehicle_sil::can {

/// What the virtual bus does with one frame an endpoint sends (fault injection for tests).
struct FaultAction
{
    enum class Kind : std::uint8_t
    {
        Deliver,   ///< Delivered normally (Accepted).
        Drop,      ///< Accepted but lost: no endpoint receives it.
        Duplicate, ///< Accepted and delivered twice with the same time.
        Delay,     ///< Accepted and delivered with time + `delay` (frames sent later can overtake it).
        QueueFull, ///< Not accepted: send() returns QueueFull and nothing is delivered.
        Failed,    ///< Not accepted: send() returns Failed and nothing is delivered.
        /// Accepted, then not transmitted: nothing is delivered (with timing it does not take
        /// the bus either), and the sender gets a PortEvent::TransmitFailed at the send time.
        TransmitFailed,
    };
    Kind kind = Kind::Deliver;
    SimDuration delay{}; ///< For Delay; must be > 0.

    [[nodiscard]] static FaultAction deliver() noexcept { return {}; }
    [[nodiscard]] static FaultAction drop() noexcept { return {Kind::Drop, {}}; }
    [[nodiscard]] static FaultAction duplicate() noexcept { return {Kind::Duplicate, {}}; }
    [[nodiscard]] static FaultAction delay_by(SimDuration d) noexcept { return {Kind::Delay, d}; }
    [[nodiscard]] static FaultAction queue_full() noexcept { return {Kind::QueueFull, {}}; }
    [[nodiscard]] static FaultAction failed() noexcept { return {Kind::Failed, {}}; }
    [[nodiscard]] static FaultAction transmit_failed() noexcept { return {Kind::TransmitFailed, {}}; }
};

/// Decides the FaultAction for each valid frame an endpoint sends. Called once per send(), in
/// send order, so a deterministic function gives a deterministic run.
using FaultInjector = std::function<FaultAction(const TimedFrame& frame)>;

/// Which way an outage (VirtualCanBus::Endpoint::add_outage()) cuts an endpoint off the bus.
enum class OutageDirection : std::uint8_t
{
    Receive, ///< The endpoint receives nothing; its own frames still go out.
    Send,    ///< The endpoint's frames reach nobody; it still receives.
    Both,    ///< Disconnected: neither.
};

/// Arbitration and transmission time on the virtual bus (CanBusModel).
struct VirtualCanBusTiming
{
    /// Bit rate in bit/s (> 0).
    std::uint32_t bitrate = 500'000;
    /// Frames an endpoint can have waiting for the bus (> 0): sent and not yet started as far
    /// as the bus is decided (for an endpoint stepped by a Host: at the step time, since the
    /// host releases frames before it sends). send() returns QueueFull when they are all taken.
    std::size_t transmit_queue_capacity = 64;
    /// Zero: the exact model. A frame competes for the bus from its send time on.
    ///
    /// Greater than zero: the bus is decided in steps of this size, the way the simple CAN
    /// network simulator (vehicle_sil::can_netsim) runs with SimulationStepSizeNanoseconds:
    /// at each step boundary T, the frames sent before T join the queues, and the
    /// transmissions that start in [T, T + simulator_step) are decided. A frame sent at t
    /// starts no earlier than the first boundary after t. With the same step size and bit rate,
    /// and while no transmit queue overflows, the virtual bus gives the same frame times as SIL
    /// Kit with that simulator (each sender a participant of its own).
    SimDuration simulator_step{};
};

struct VirtualCanBusOptions
{
    /// Receive inbox of each endpoint, in frames. Sized for the most frames an endpoint can
    /// receive in one step of its host. Also the capacity of an endpoint's event queue.
    /// Allocated once per endpoint.
    std::size_t inbox_capacity = 1024;
    /// Empty: no bus model; a frame reaches the others at its send time. Set: arbitration and
    /// transmission time (see VirtualCanBus).
    std::optional<VirtualCanBusTiming> timing = std::nullopt;
};

/// A CAN bus in virtual time. Each attach() creates an endpoint, a Port that stands for one CAN
/// controller on the bus. An endpoint does not receive its own frames.
///
/// Without timing (VirtualCanBusOptions::timing empty), a frame sent with time t reaches every
/// other endpoint with time t; the receiver releases it at its first step after t. There is no
/// arbitration and no transmission time.
///
/// With timing, the frames go through a CanBusModel: each endpoint has a transmit FIFO, the
/// frames compete by identifier when the bus is free, and each occupies the bus for
/// frame_duration(). A frame reaches the other endpoints with the time its transmission ended,
/// and the sender gets a PortEvent::TransmitCompleted with that time. The bus is decided
/// lazily: when an endpoint releases frames or events before `now`, every transmission that
/// starts before `now` is decided. With the exact model (simulator_step zero) the frame times do
/// not depend on the step sizes of the receiving hosts, only on the send times.
///
/// Causality: a frame must not be older than what the bus has already decided. Without timing,
/// a frame with time < the `now` an endpoint already released before is refused; with timing,
/// a frame sent before the time up to which the bus is decided. send() throws
/// std::logic_error in both cases instead of rewriting the past: the hosts were stepped out of
/// time order (or the frame time is wrong). StepDriver always steps in an order that satisfies
/// this.
///
/// Each endpoint's inbox has a fixed capacity; a frame that does not fit is dropped, counted in
/// VirtualCanBus::Endpoint::overflows() and reported as a PortEvent::ReceiveOverflow (the frames
/// dropped until the endpoint releases its events become one event, with the time of the first).
/// Overflow is deterministic on the virtual bus, but frames are lost; tests should check that
/// there were none.
///
/// Fault injection (Endpoint::set_fault_injector()): each frame an endpoint sends can be
/// dropped, duplicated, delayed (which also reorders it after later frames), or refused with
/// QueueFull or Failed, or accepted and then fail (TransmitFailed), as a test-supplied function
/// decides. With timing, a dropped frame still takes its turn on the bus but no endpoint
/// receives it, a duplicated frame is transmitted twice, and a delayed frame competes from its
/// send time + the delay. Endpoint::inject_event() hands an endpoint any PortEvent (a bus state,
/// an error frame).
///
/// Outages (Endpoint::add_outage()) cut one endpoint off the bus for a span of time, by the
/// time of the frames: the endpoint does not receive the frames that reach it in the span, and
/// the frames it sends in the span reach nobody (accepted, then a PortEvent::TransmitFailed at
/// the send time; with timing they do not take the bus). Sending is judged by the send time:
/// with timing, a frame sent before the span that waits for the bus still goes out during it.
/// This reproduces an ECU that loses its connection and comes back, deterministically.
///
/// Not thread safe: use the bus and its endpoints from one thread (a StepDriver loop).
class VirtualCanBus
{
public:
    class Endpoint final : public Port
    {
    public:
        [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
        std::size_t release_before(SimTime now, FrameSink sink) override;
        /// Delivers a valid frame to every other endpoint, or with timing queues it for the bus
        /// (Accepted). An invalid frame (is_valid() false) is not sent (Failed). With timing, a
        /// full transmit queue gives QueueFull. A frame older than what the bus has already
        /// handed out throws std::logic_error (see the class comment).
        SendResult send(const TimedFrame& frame) override;
        /// With timing: a TransmitCompleted for each frame this endpoint sent, at the end of its
        /// transmission. With or without timing: TransmitFailed for the frames that failed (fault
        /// injection, outages), ReceiveOverflow, and the injected events.
        std::size_t release_events_before(SimTime now, PortEventSink sink) override;

        /// Frames dropped because the inbox was full.
        [[nodiscard]] std::uint64_t overflows() const { return inbox_.overflows(); }
        /// Frames waiting in the inbox.
        [[nodiscard]] std::size_t pending() const { return inbox_.size(); }
        /// With timing: frames sent and not yet transmitted.
        [[nodiscard]] std::size_t transmit_pending() const;
        /// Events dropped because the event queue was full.
        [[nodiscard]] std::uint64_t event_overflows() const { return events_.overflows(); }
        /// Sets (or with an empty function removes) the fault injector for the frames this
        /// endpoint sends. Invalid frames are Failed before the injector is asked, and frames
        /// sent during an outage of the Send direction do not ask it.
        void set_fault_injector(FaultInjector injector) { injector_ = std::move(injector); }

        /// Queues `event` for this endpoint at event.time, to be released like the bus's own
        /// events (e.g. a BusStateChanged or an ErrorFrame). Returns false if the event queue
        /// was full (counted in event_overflows()).
        bool inject_event(const PortEvent& event) { return events_.push(event.time, event); }

        /// Cuts this endpoint off the bus for the frames with a time in [from, until) (see the
        /// class comment): with Receive, the frames that reach it; with Send, the frames it
        /// sends. Outages may overlap. std::invalid_argument if `until` <= `from`.
        void add_outage(SimTime from, SimTime until, OutageDirection direction = OutageDirection::Both);
        /// Frames this endpoint did not receive because of an outage.
        [[nodiscard]] std::uint64_t outage_dropped() const noexcept { return outage_dropped_; }
        /// Frames this endpoint sent during an outage (none reached the bus).
        [[nodiscard]] std::uint64_t outage_lost() const noexcept { return outage_lost_; }

    private:
        friend class VirtualCanBus;
        struct Staged
        {
            Frame frame{};
            SimTime send_time{};
            bool lost = false;
        };
        struct Outage
        {
            SimTime from;
            SimTime until;
            OutageDirection direction;
        };
        [[nodiscard]] bool cut_off(SimTime time, OutageDirection direction) const noexcept;
        /// A frame reaches this endpoint at `time`.
        void receive_frame(SimTime time, const Frame& frame);
        /// A frame this endpoint sent at `time` was not transmitted.
        void transmit_failed(const TimedFrame& frame);
        Endpoint(VirtualCanBus& bus, std::size_t index, std::size_t capacity, std::size_t event_capacity,
                 std::size_t transmit_capacity)
          : bus_(bus), index_(index), inbox_(capacity), events_(event_capacity), staged_(transmit_capacity)
        {
        }

        VirtualCanBus& bus_;
        std::size_t index_;
        StepInbox<Frame> inbox_;
        StepInbox<PortEvent> events_;
        StepInbox<Staged> staged_; ///< with timing: sent, not yet handed to the bus model (by ready time)
        std::optional<SimTime> released_before_; ///< `now` of the last release_before() (frames before it are gone)
        FaultInjector injector_;
        std::vector<Outage> outages_;
        OverflowCollector overflow_;
        std::uint64_t outage_dropped_ = 0;
        std::uint64_t outage_lost_ = 0;
    };

    explicit VirtualCanBus(VirtualCanBusOptions options = {});
    VirtualCanBus(const VirtualCanBus&) = delete;
    VirtualCanBus& operator=(const VirtualCanBus&) = delete;

    /// Creates an endpoint. It lives as long as the bus. It receives only frames sent after
    /// it was attached.
    Endpoint& attach();

    [[nodiscard]] std::size_t endpoint_count() const noexcept { return endpoints_.size(); }
    /// With timing: transmissions so far. Without timing: 0.
    [[nodiscard]] std::uint64_t transmissions() const noexcept { return model_ ? model_->transmissions() : 0; }

private:
    void deliver(const Endpoint& from, const TimedFrame& frame);
    /// Throws std::logic_error if `frame` is older than what the bus has already decided.
    void check_causality(const Endpoint& from, const TimedFrame& frame) const;
    SendResult submit(Endpoint& from, const TimedFrame& frame, const FaultAction& action);
    void advance(SimTime now);
    void hand_over_before(SimTime time);

    VirtualCanBusOptions options_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
    std::optional<CanBusModel> model_;
    SimTime decided_until_{}; ///< with timing: every transmission that starts before this is decided
    SimTime next_window_{};   ///< with simulator_step: start of the next step of the bus
};

} // namespace vehicle_sil::can
