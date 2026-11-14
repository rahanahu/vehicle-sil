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
};

/// An Ethernet network in virtual time that behaves like SIL Kit's Ethernet without a network
/// simulator: a hub. Each attach() creates an endpoint, a Port that stands for one Ethernet
/// controller.
///
/// A frame sent with time t reaches every other endpoint with time t, whatever its destination
/// address; the receiver releases it at its first step after t. No transmission time, no
/// transmit acknowledgements. An endpoint does not receive its own frames. Short frames are
/// padded on delivery (VirtualEthernetBusOptions::min_frame_bytes).
///
/// Causality: a frame with time < the `now` another endpoint has already released before is
/// refused with std::logic_error (the hosts were stepped out of time order). StepDriver always
/// steps in an order that satisfies this.
///
/// Each endpoint's inbox has a fixed capacity; a frame that does not fit is dropped, counted in
/// Endpoint::overflows() and reported as a PortEvent::ReceiveOverflow.
///
/// Not thread safe: use the bus and its endpoints from one thread (a StepDriver loop).
class VirtualEthernetBus
{
public:
    class Endpoint final : public Port
    {
    public:
        [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
        std::size_t release_before(SimTime now, FrameSink sink) override;
        /// Delivers the frame to every other endpoint (Accepted). A frame shorter than
        /// kHeaderBytes is not sent (Failed). A frame older than what a receiver has already
        /// released throws std::logic_error (see the class comment).
        SendResult send(const TimedFrame& frame) override;
        /// ReceiveOverflow only.
        std::size_t release_events_before(SimTime now, PortEventSink sink) override;

        /// Frames dropped because the inbox was full.
        [[nodiscard]] std::uint64_t overflows() const { return inbox_.overflows(); }
        /// Frames waiting in the inbox.
        [[nodiscard]] std::size_t pending() const { return inbox_.size(); }

    private:
        friend class VirtualEthernetBus;
        Endpoint(VirtualEthernetBus& bus, std::size_t capacity) : bus_(bus), inbox_(capacity) {}
        void receive_frame(SimTime time, const Frame& frame);

        VirtualEthernetBus& bus_;
        StepInbox<Frame> inbox_;
        OverflowCollector overflow_;
        std::optional<SimTime> released_before_; ///< `now` of the last release_before()
    };

    /// std::invalid_argument for an inbox_capacity of 0 or a min_frame_bytes above
    /// Frame::kCapacity.
    explicit VirtualEthernetBus(VirtualEthernetBusOptions options = {});
    VirtualEthernetBus(const VirtualEthernetBus&) = delete;
    VirtualEthernetBus& operator=(const VirtualEthernetBus&) = delete;

    /// Creates an endpoint. It lives as long as the bus. It receives only frames sent after it
    /// was attached.
    Endpoint& attach();
    /// The same, with a receive inbox of `inbox_capacity` frames instead of the options' value.
    /// std::invalid_argument for 0.
    Endpoint& attach(std::size_t inbox_capacity);

    [[nodiscard]] std::size_t endpoint_count() const noexcept { return endpoints_.size(); }

private:
    void deliver(const Endpoint& from, const TimedFrame& frame);

    VirtualEthernetBusOptions options_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
};

} // namespace vehicle_sil::eth
