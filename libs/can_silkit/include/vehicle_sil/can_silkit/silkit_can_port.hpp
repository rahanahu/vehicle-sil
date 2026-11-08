// SPDX-License-Identifier: MIT
//
// vehicle_sil::can port on a SIL Kit CAN controller (vehicle_sil::can_silkit).
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/overflow_collector.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_receiver.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/step_inbox.hpp"

#include <silkit/services/can/ICanController.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vehicle_sil::can {

/// Converts a received SIL Kit frame into a vehicle_sil::can::Frame. Only classic CAN data
/// frames convert: remote, CAN FD and CAN XL frames, a DLC that does not match the payload and
/// an identifier out of range yield std::nullopt.
[[nodiscard]] std::optional<Frame> from_silkit_frame(const SilKit::Services::Can::CanFrame& in) noexcept;

/// Converts `in` into a SIL Kit classic CAN data frame. `out.dataField` points into `in.data`,
/// so `out` must not outlive `in`. Returns false (and leaves `out` unspecified) if `in` is not
/// valid (vehicle_sil::can::is_valid).
[[nodiscard]] bool to_silkit_frame(const Frame& in, SilKit::Services::Can::CanFrame& out) noexcept;

struct SilKitCanPortOptions
{
    /// Receive inbox capacity in frames (allocated once, > 0). Sized for the most frames that
    /// can arrive between two steps of the participant.
    std::size_t inbox_capacity = 1024;
    /// Event queue capacity (> 0): transmit acknowledgements waiting for the next step.
    std::size_t event_capacity = 1024;
    /// Bit rate passed to the controller with SetBaudRate() before it starts (0: not set).
    /// SIL Kit's trivial simulation ignores it; a network simulator uses it for the
    /// transmission time.
    std::uint32_t bitrate = 0;
    /// Frames sent and not yet acknowledged (the acknowledgement released as a PortEvent) that
    /// the port allows; send() returns QueueFull beyond it, so the stack keeps the frame for a
    /// later step. 0: no limit. With a network simulator that refuses frames beyond its own
    /// transmit queue, set it to at most that queue's size, so that frames wait in the stack
    /// instead of being refused (and lost) by the simulator. Must not exceed event_capacity
    /// (std::invalid_argument). A frame that is never acknowledged (SIL Kit does not send it,
    /// e.g. the controller is not started) keeps its place; `outstanding` in the statistics
    /// shows it. With SIL Kit's trivial simulation the acknowledgements come back at the next
    /// step, so the limit is the most frames per step.
    std::size_t transmit_queue_capacity = 0;
    /// Empty: the participant uses virtual time; the port is of TimeDomain::Virtual and
    /// stamps with SIL Kit's timestamps. Set: the participant runs without time
    /// synchronisation and a RealTimeDriver with this clock steps the host; the port is of
    /// TimeDomain::RealTime and stamps arrivals with this clock (SIL Kit's timestamps of
    /// unsynchronised participants are not usable). The clock must outlive the port.
    const RealTimeClock* real_time_clock = nullptr;
};

/// What a SilKitCanPort has seen. Counters only grow.
struct SilKitCanPortStatistics
{
    std::uint64_t received = 0;          ///< Frames queued for the stack.
    std::uint64_t receive_rejected = 0;  ///< Frames that are not classic CAN data frames.
    std::uint64_t receive_overflows = 0; ///< Frames dropped because the inbox was full.
    std::uint64_t error_state_changes = 0; ///< Error state changes SIL Kit reported (BusStateChanged).
    std::uint64_t sent = 0;              ///< Frames handed to SendFrame().
    std::uint64_t send_failed = 0;       ///< send() calls that returned Failed.
    std::uint64_t send_queue_full = 0;   ///< send() calls that returned QueueFull (transmit_queue_capacity).
    std::uint64_t tx_transmitted = 0;    ///< Transmit acknowledgements: Transmitted.
    std::uint64_t tx_canceled = 0;       ///< Transmit acknowledgements: Canceled.
    std::uint64_t tx_queue_full = 0;     ///< Transmit acknowledgements: TransmitQueueFull.
    std::uint64_t event_overflows = 0;   ///< Events dropped because the event queue was full (or,
                                         ///< for error states, had no place besides the ones kept
                                         ///< for outstanding acknowledgements).
    std::size_t outstanding = 0;         ///< Frames sent whose acknowledgement was not released yet.
};

/// A vehicle_sil::can::Port on a SIL Kit CAN controller: of a participant with virtual time
/// (TimeDomain::Virtual, the default), or of one without time synchronisation
/// (TimeDomain::RealTime, SilKitCanPortOptions::real_time_clock).
///
/// The constructor adds a receive handler (RX direction only, so the port never sees its own
/// frames), a transmit handler and an error state handler, then starts the controller. close()
/// removes the handlers and stops the controller; after close() returns, SIL Kit no longer
/// calls into the port.
///
/// Real time: frames, acknowledgements and error states are stamped with their arrival on the
/// driver's clock (RealTimeReceiver), and each arrival wakes the driver (set_wakeup()). The
/// send time is not put into the userContext (a network simulator needs virtual time).
/// Otherwise the port works as described below.
///
/// Receiving: SIL Kit's receive callback only queues the frame in an inbox (StepInbox), stamped
/// with SIL Kit's timestamp, which is the sender's step time. release_before(now) hands over the
/// frames stamped before `now` in (time, arrival) order. Frames that are not classic CAN data
/// frames are dropped and counted.
///
/// Sending: SendFrame() at the time of the current step; SIL Kit stamps the frame with the step
/// time itself. TimedFrame::time goes into the userContext (encode_send_time()), so that a
/// network simulator knows when the frame was sent. SendFrame() has no synchronous outcome, so
/// a valid frame yields Accepted, unless SilKitCanPortOptions::transmit_queue_capacity frames
/// are still unacknowledged (QueueFull). An invalid frame or an exception from SendFrame()
/// yields Failed.
///
/// Transmit acknowledgements are counted and queued as PortEvents (Transmitted:
/// TransmitCompleted, Canceled and TransmitQueueFull: TransmitFailed), stamped with SIL Kit's
/// timestamp, and released by release_events_before() like frames. They are not handed to the
/// stack from SIL Kit's handler: with the trivial simulation they arrive inside SendFrame(),
/// and calling into the stack there would be re-entrant. With the trivial simulation the
/// timestamp is the send step's time; with a network simulator, the end of the transmission.
///
/// Error states: SIL Kit's error state changes (a network simulator reports them; the trivial
/// simulation does not) become PortEvent::BusStateChanged. They are queued only while the event
/// queue keeps a place for the acknowledgement of every outstanding frame, so they never cost
/// an acknowledgement (and a place of transmit_queue_capacity). A full inbox reports
/// PortEvent::ReceiveOverflow, which takes no place in the event queue (OverflowCollector).
///
/// Shutdown order: stop the ParticipantRuntime (no more steps), close() (or destroy) the port,
/// destroy the stack, then destroy the ParticipantRuntime (which owns the controller).
///
/// Threading: SIL Kit calls the handlers on its own thread; release_before() and send() run on
/// the step handler's thread. close() may be called from any thread except a SIL Kit handler of
/// this controller.
class SilKitCanPort final : public Port
{
public:
    SilKitCanPort(SilKit::Services::Can::ICanController& controller, SilKitCanPortOptions options = {});
    ~SilKitCanPort() override;

    SilKitCanPort(const SilKitCanPort&) = delete;
    SilKitCanPort& operator=(const SilKitCanPort&) = delete;

    [[nodiscard]] TimeDomain time_domain() const noexcept override
    {
        return real_time_ ? TimeDomain::RealTime : TimeDomain::Virtual;
    }
    std::size_t release_before(SimTime now, FrameSink sink) override;
    SendResult send(const TimedFrame& frame) override;
    std::size_t release_events_before(SimTime now, PortEventSink sink) override;
    void set_wakeup(Wakeup wakeup) override;

    /// Removes the handlers, stops the controller and discards queued frames. Idempotent.
    /// send() after close() returns Failed; release_before() releases nothing.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return open_.load(std::memory_order_acquire); }
    [[nodiscard]] SilKitCanPortStatistics statistics() const;

private:
    void on_frame(const SilKit::Services::Can::CanFrameEvent& event);
    void on_transmit(const SilKit::Services::Can::CanFrameTransmitEvent& event) noexcept;
    void on_error_state(const SilKit::Services::Can::CanErrorStateChangeEvent& event) noexcept;
    /// Queues an event (virtual time: at event.time; real time: stamped on arrival). An event
    /// that is not a transmit acknowledgement keeps a place for each outstanding frame.
    void push_event(const PortEvent& event, bool acknowledgement) noexcept;

    SilKit::Services::Can::ICanController& controller_;
    StepInbox<Frame> inbox_;      ///< virtual time
    StepInbox<PortEvent> events_; ///< virtual time
    std::mutex overflow_mutex_;   ///< virtual time: guards overflow_
    OverflowCollector overflow_;
    std::optional<RealTimeReceiver> real_time_;
    std::atomic<std::uint64_t> event_refusals_{0}; ///< virtual time: error states without a place
    std::size_t transmit_capacity_;
    std::atomic<std::size_t> outstanding_{0}; ///< changed on the driver's thread only; atomic for statistics()
    std::mutex lifecycle_mutex_;
    std::atomic<bool> open_{false};
    std::optional<SilKit::Util::HandlerId> frame_handler_;
    std::optional<SilKit::Util::HandlerId> transmit_handler_;
    std::optional<SilKit::Util::HandlerId> error_state_handler_;

    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> receive_rejected_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> send_failed_{0};
    std::atomic<std::uint64_t> send_queue_full_{0};
    std::atomic<std::uint64_t> tx_transmitted_{0};
    std::atomic<std::uint64_t> tx_canceled_{0};
    std::atomic<std::uint64_t> tx_queue_full_{0};
    std::atomic<std::uint64_t> error_state_changes_{0};
};

/// Steps `host` from the step handler of `runtime`: each SIL Kit step at `now` runs
/// host.step(now). Requires TimeDomain::Virtual of the host's ports
/// (Host::require_time_domain(), std::logic_error). Before the first step it passes the
/// participant's step size to Host::set_step_size(). `runtime` must use virtual time (std::invalid_argument otherwise) and must
/// not have started. An exception from the host makes the participant go to Error (see
/// ParticipantRuntime). `host` must outlive the runtime's steps: stop the runtime first.
void bind_host(ParticipantRuntime& runtime, Host& host);

} // namespace vehicle_sil::can
