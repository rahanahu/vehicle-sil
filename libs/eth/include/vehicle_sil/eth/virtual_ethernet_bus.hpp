// SPDX-License-Identifier: MIT
//
// In-process deterministic Ethernet network for tests and single-process simulations
// (vehicle_sil::eth).
#pragma once

#include "vehicle_sil/core/step_inbox.hpp"
#include "vehicle_sil/eth/frame.hpp"
#include "vehicle_sil/eth/overflow_collector.hpp"
#include "vehicle_sil/eth/port.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace vehicle_sil::eth {

struct VirtualEthernetBusOptions
{
    /// Receive inbox of an endpoint made by attach() without a capacity, in frames (> 0). It
    /// holds the frames of one step of the endpoint's host and the frames that senders stepped
    /// earlier have sent with the time of the next one (for a Channel on the endpoint:
    /// Channel::port_inbox_capacity(); the default fits a Channel with its default
    /// receive_capacity). Allocated once per endpoint.
    std::size_t inbox_capacity = 2048;
    /// Frames shorter than this reach the receivers padded with zero bytes to this length, as
    /// SIL Kit's Ethernet controller pads them (0: no padding). At most Frame::kCapacity.
    std::size_t min_frame_bytes = kDefaultMinFrameBytes;
    /// Event queue capacity of an endpoint made by attach() without an override (> 0).
    std::size_t event_capacity = 1024;
    /// Maximum unacknowledged frames per endpoint (0: unlimited; otherwise <= event_capacity).
    std::size_t transmit_queue_capacity = 0;
};

/// Per-endpoint capacity overrides for VirtualEthernetBus::attach(). Zero inherits the
/// corresponding value from VirtualEthernetBusOptions.
struct VirtualEthernetEndpointOptions
{
    std::size_t inbox_capacity = 0;
    std::size_t event_capacity = 0;
    std::size_t transmit_queue_capacity = 0;
};

/// An Ethernet network in virtual time that behaves like SIL Kit's Ethernet without a network
/// simulator: a hub. Each attach() creates an endpoint, a Port that stands for one Ethernet
/// controller.
///
/// A frame sent with time t reaches every other endpoint with time t, whatever its destination
/// address; the receiver releases it at its first step after t. Each accepted send also queues a
/// token-correlated PortEvent::TransmitCompleted with time t, released at the sender's next step
/// (matching SIL Kit trivial simulation). An endpoint does not receive its own frames. Short
/// frames are padded on delivery (VirtualEthernetBusOptions::min_frame_bytes).
///
/// Causality: a frame with time < the `now` another endpoint has already released before is
/// refused with std::logic_error (the hosts were stepped out of time order). StepDriver always
/// steps in an order that satisfies this.
///
/// Each endpoint's inbox and event queue have fixed capacities; a frame that does not fit is
/// dropped, counted in Endpoint::overflows() and reported as a PortEvent::ReceiveOverflow. An
/// event that does not fit is dropped, counted in Endpoint::event_overflows() and reported as a
/// PortEvent::EventOverflow.
///
/// Not thread safe: use the bus and its endpoints from one thread (a StepDriver loop).
class VirtualEthernetBus
{
public:
    class Endpoint final : public Port
    {
    public:
        [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
        [[nodiscard]] TransmitCapabilities transmit_capabilities() const noexcept override
        {
            return TransmitCapabilities::Correlated;
        }
        std::size_t release_before(SimTime now, FrameSink sink) override;
        /// Delivers the frame to every other endpoint (Accepted) and queues a correlated
        /// TransmitCompleted event at frame.time. A frame shorter than kHeaderBytes is not sent
        /// (Failed). When transmit_queue_capacity > 0 and outstanding() has reached it, returns
        /// QueueFull without delivering. A frame older than what a receiver has already released
        /// throws std::logic_error (see the class comment).
        SendResult send(const TimedFrame& frame) override;
        SendResult send(const TimedFrame& frame, std::uint64_t token) override;
        /// ReceiveOverflow, EventOverflow, and correlated TransmitCompleted events.
        std::size_t release_events_before(SimTime now, PortEventSink sink) override;

        /// Frames dropped because the inbox was full.
        [[nodiscard]] std::uint64_t overflows() const { return inbox_.overflows(); }
        /// Transmit events dropped because the event queue was full.
        [[nodiscard]] std::uint64_t event_overflows() const { return events_.overflows(); }
        /// Frames waiting in the inbox.
        [[nodiscard]] std::size_t pending() const { return inbox_.size(); }
        /// Accepted sends whose transmit event has not yet been released.
        [[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_; }

    private:
        friend class VirtualEthernetBus;
        Endpoint(VirtualEthernetBus& bus, std::size_t inbox_capacity, std::size_t event_capacity,
                 std::size_t transmit_queue_capacity) :
          bus_(bus),
          inbox_(inbox_capacity),
          events_(event_capacity),
          transmit_queue_capacity_(transmit_queue_capacity)
        {
        }
        void receive_frame(SimTime time, const Frame& frame);

        VirtualEthernetBus& bus_;
        StepInbox<Frame> inbox_;
        StepInbox<PortEvent> events_;
        OverflowCollector overflow_;
        OverflowCollector event_overflow_{PortEvent::Kind::EventOverflow};
        std::size_t transmit_queue_capacity_ = 0;
        std::size_t outstanding_ = 0;
        std::optional<SimTime> released_before_; ///< `now` of the last release_before()
    };

    /// std::invalid_argument for an inbox_capacity or event_capacity of 0, a min_frame_bytes
    /// above Frame::kCapacity, or transmit_queue_capacity > event_capacity.
    explicit VirtualEthernetBus(VirtualEthernetBusOptions options = {});
    VirtualEthernetBus(const VirtualEthernetBus&) = delete;
    VirtualEthernetBus& operator=(const VirtualEthernetBus&) = delete;

    /// Creates an endpoint. It lives as long as the bus. It receives only frames sent after it
    /// was attached.
    Endpoint& attach();
    /// The same, with a receive inbox of `inbox_capacity` frames instead of the options' value.
    /// std::invalid_argument for 0.
    Endpoint& attach(std::size_t inbox_capacity);
    /// The same, overriding any non-zero capacity in `options`. std::invalid_argument if the
    /// resolved transmit_queue_capacity exceeds the resolved event_capacity.
    Endpoint& attach(VirtualEthernetEndpointOptions options);

    [[nodiscard]] std::size_t endpoint_count() const noexcept { return endpoints_.size(); }

private:
    void deliver(const Endpoint& from, const TimedFrame& frame);

    VirtualEthernetBusOptions options_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
};

} // namespace vehicle_sil::eth
