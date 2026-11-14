// SPDX-License-Identifier: MIT
//
// vehicle_sil::eth port on a SIL Kit Ethernet controller, on virtual time
// (vehicle_sil::eth_silkit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"
#include "vehicle_sil/eth/frame.hpp"
#include "vehicle_sil/eth/overflow_collector.hpp"
#include "vehicle_sil/eth/port.hpp"

#include <silkit/services/ethernet/IEthernetController.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vehicle_sil::eth {

struct SilKitEthPortOptions
{
    /// Receive inbox capacity in frames (allocated once, > 0). Sized for the most frames that
    /// can arrive between two steps of the participant.
    std::size_t inbox_capacity = 1024;
    /// Event queue capacity (> 0): transmit acknowledgements waiting for the next step. SIL Kit
    /// acknowledges every frame, so it must hold at least the frames sent in one step (the
    /// channel's send_capacity); acknowledgements that do not fit are dropped, counted and
    /// reported as a PortEvent::EventOverflow.
    std::size_t event_capacity = 1024;
};

/// What a SilKitEthPort has seen. Counters only grow.
struct SilKitEthPortStatistics
{
    std::uint64_t received = 0;              ///< Frames queued for the channel.
    std::uint64_t received_without_time = 0; ///< Of those, frames from a sender without time
                                             ///< synchronisation (see SilKitEthPort).
    std::uint64_t receive_rejected = 0;      ///< Frames longer than Frame::kCapacity.
    std::uint64_t receive_overflows = 0;     ///< Frames dropped because the inbox was full.
    std::uint64_t sent = 0;                  ///< Frames handed to SendFrame().
    std::uint64_t send_failed = 0;           ///< send() calls that returned Failed.
    std::uint64_t tx_transmitted = 0;        ///< Transmit acknowledgements: Transmitted.
    std::uint64_t tx_controller_inactive = 0; ///< Transmit acknowledgements: ControllerInactive.
    std::uint64_t tx_link_down = 0;          ///< Transmit acknowledgements: LinkDown.
    std::uint64_t tx_dropped = 0;            ///< Transmit acknowledgements: Dropped.
    std::uint64_t tx_invalid_frame = 0;      ///< Transmit acknowledgements: InvalidFrameFormat.
    std::uint64_t event_overflows = 0;       ///< Acknowledgements dropped because the event queue
                                             ///< was full.
    bool link_up = false;                    ///< The controller's last state was LinkUp.
};

/// A vehicle_sil::eth::Port on a SIL Kit Ethernet controller of a participant with virtual time
/// (TimeDomain::Virtual).
///
/// The constructor adds a receive handler (RX direction only, so the port never sees its own
/// frames), a transmit handler and a state handler, then activates the controller. close()
/// removes the handlers and deactivates the controller; after close() returns, SIL Kit no
/// longer calls into the port.
///
/// Receiving: SIL Kit's receive callback only queues the frame in an inbox (StepInbox), stamped
/// with SIL Kit's timestamp, which is the sender's step time. release_before(now) hands over the
/// frames stamped before `now` in (time, arrival) order. SIL Kit pads frames shorter than 60
/// bytes with zero bytes before they arrive.
///
/// A sender without time synchronisation (e.g. a bridge to a real network) has no step time:
/// SIL Kit stamps its frames with std::chrono::nanoseconds::min(). The port does not pass that
/// on: such a frame (any negative timestamp) gets the time of this participant's latest step
/// when it arrives (0 before the first step), and is released at the next step, like a frame
/// sent at that step's time. Which step that is depends on when the frame arrives, so it is not
/// deterministic. Counted in received_without_time.
///
/// Sending: SendFrame() at the time of the current step; SIL Kit stamps the frame with the step
/// time itself. A frame of at least kHeaderBytes yields Accepted; a shorter one, a closed port
/// or an exception from SendFrame() yields Failed.
///
/// Transmit acknowledgements are counted and queued as PortEvents (Transmitted:
/// TransmitCompleted; ControllerInactive, LinkDown, Dropped, InvalidFrameFormat:
/// TransmitFailed), stamped with SIL Kit's timestamp and released by release_events_before().
/// They are not handed to the channel from SIL Kit's handler: without a network simulator they
/// arrive inside SendFrame(). A full inbox reports PortEvent::ReceiveOverflow and a full event
/// queue PortEvent::EventOverflow; neither takes a place in the event queue.
///
/// Shutdown order: stop the ParticipantRuntime (no more steps), close() (or destroy) the port,
/// destroy the channel, then destroy the ParticipantRuntime (which owns the controller).
///
/// Threading: SIL Kit calls the handlers on its own thread; release_before() and send() run on
/// the step handler's thread. close() may be called from any thread except a SIL Kit handler of
/// this controller.
class SilKitEthPort final : public Port
{
public:
    /// std::invalid_argument for a capacity of 0.
    explicit SilKitEthPort(SilKit::Services::Ethernet::IEthernetController& controller, SilKitEthPortOptions options = {});
    ~SilKitEthPort() override;

    SilKitEthPort(const SilKitEthPort&) = delete;
    SilKitEthPort& operator=(const SilKitEthPort&) = delete;

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
    std::size_t release_before(SimTime now, FrameSink sink) override;
    SendResult send(const TimedFrame& frame) override;
    std::size_t release_events_before(SimTime now, PortEventSink sink) override;

    /// Removes the handlers, deactivates the controller and discards queued frames and events.
    /// Idempotent. send() after close() returns Failed; release_before() releases nothing.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return open_.load(std::memory_order_acquire); }
    [[nodiscard]] SilKitEthPortStatistics statistics() const;

private:
    void on_frame(const SilKit::Services::Ethernet::EthernetFrameEvent& event);
    void on_transmit(const SilKit::Services::Ethernet::EthernetFrameTransmitEvent& event) noexcept;
    /// The time of a received frame or event: SIL Kit's timestamp, or for a negative one the
    /// time of the latest step.
    [[nodiscard]] SimTime stamp(std::chrono::nanoseconds timestamp) const noexcept;

    SilKit::Services::Ethernet::IEthernetController& controller_;
    StepInbox<Frame> inbox_;
    StepInbox<PortEvent> events_;
    std::mutex overflow_mutex_; ///< guards overflow_ and event_overflow_
    OverflowCollector overflow_;
    OverflowCollector event_overflow_{PortEvent::Kind::EventOverflow};
    /// `now` of the latest release_before(), in ns (written on the driver's thread, read on
    /// SIL Kit's).
    std::atomic<std::int64_t> step_time_ns_{0};
    std::mutex lifecycle_mutex_;
    std::atomic<bool> open_{false};
    std::optional<SilKit::Util::HandlerId> frame_handler_;
    std::optional<SilKit::Util::HandlerId> transmit_handler_;
    std::optional<SilKit::Util::HandlerId> state_handler_;

    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> received_without_time_{0};
    std::atomic<std::uint64_t> receive_rejected_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> send_failed_{0};
    std::atomic<std::uint64_t> tx_transmitted_{0};
    std::atomic<std::uint64_t> tx_controller_inactive_{0};
    std::atomic<std::uint64_t> tx_link_down_{0};
    std::atomic<std::uint64_t> tx_dropped_{0};
    std::atomic<std::uint64_t> tx_invalid_frame_{0};
    std::atomic<bool> link_up_{false};
};

} // namespace vehicle_sil::eth
