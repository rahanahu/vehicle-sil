// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth/virtual_ethernet_bus.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace vehicle_sil::eth {

namespace {
constexpr const char* kWho = "vehicle_sil::eth::VirtualEthernetBus: ";
} // namespace

VirtualEthernetBus::VirtualEthernetBus(VirtualEthernetBusOptions options) : options_(options)
{
    if (options_.inbox_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "inbox_capacity must be > 0");
    }
    if (options_.min_frame_bytes > Frame::kCapacity)
    {
        throw std::invalid_argument(std::string(kWho) + "min_frame_bytes must be at most " +
                                    std::to_string(Frame::kCapacity));
    }
    if (options_.event_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "event_capacity must be > 0");
    }
    if (options_.transmit_queue_capacity > options_.event_capacity)
    {
        throw std::invalid_argument(std::string(kWho) + "transmit_queue_capacity must be <= event_capacity");
    }
}

VirtualEthernetBus::Endpoint& VirtualEthernetBus::attach()
{
    return attach(VirtualEthernetEndpointOptions{});
}

VirtualEthernetBus::Endpoint& VirtualEthernetBus::attach(std::size_t inbox_capacity)
{
    if (inbox_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "attach(): inbox_capacity must be > 0");
    }
    return attach(VirtualEthernetEndpointOptions{inbox_capacity, 0, 0});
}

VirtualEthernetBus::Endpoint& VirtualEthernetBus::attach(VirtualEthernetEndpointOptions options)
{
    const std::size_t inboxCapacity = options.inbox_capacity != 0 ? options.inbox_capacity : options_.inbox_capacity;
    const std::size_t eventCapacity = options.event_capacity != 0 ? options.event_capacity : options_.event_capacity;
    const std::size_t txQueueCapacity =
        options.transmit_queue_capacity != 0 ? options.transmit_queue_capacity : options_.transmit_queue_capacity;
    if (txQueueCapacity > eventCapacity)
    {
        throw std::invalid_argument(std::string(kWho) + "attach(): transmit_queue_capacity must be <= event_capacity");
    }
    // Endpoint's constructor is private; make_unique cannot reach it.
    endpoints_.push_back(std::unique_ptr<Endpoint>(new Endpoint(*this, inboxCapacity, eventCapacity, txQueueCapacity)));
    return *endpoints_.back();
}

void VirtualEthernetBus::deliver(const Endpoint& from, const TimedFrame& frame)
{
    for (const auto& endpoint : endpoints_)
    {
        if (endpoint.get() != &from && endpoint->released_before_ && frame.time < *endpoint->released_before_)
        {
            throw std::logic_error(std::string(kWho) + "frame at t=" + std::to_string(to_ns(frame.time)) +
                                   " ns is older than a receiver's last step (t=" +
                                   std::to_string(to_ns(*endpoint->released_before_)) +
                                   " ns); step the hosts in time order");
        }
    }
    Frame padded = frame.frame;
    padded.pad_to(options_.min_frame_bytes);
    for (const auto& endpoint : endpoints_)
    {
        if (endpoint.get() != &from)
        {
            endpoint->receive_frame(frame.time, padded);
        }
    }
}

void VirtualEthernetBus::Endpoint::receive_frame(SimTime time, const Frame& frame)
{
    if (!inbox_.push(time, frame))
    {
        overflow_.note(time);
    }
}

std::size_t VirtualEthernetBus::Endpoint::release_before(SimTime now, FrameSink sink)
{
    if (!released_before_ || now > *released_before_)
    {
        released_before_ = now;
    }
    return inbox_.drain_before(now, [&sink](SimTime time, Frame& frame) { sink(TimedFrame{time, frame}); });
}

SendResult VirtualEthernetBus::Endpoint::send(const TimedFrame& frame)
{
    return send(frame, 0);
}

SendResult VirtualEthernetBus::Endpoint::send(const TimedFrame& frame, std::uint64_t token)
{
    if (frame.frame.size() < kHeaderBytes)
    {
        return SendResult::Failed;
    }
    if (transmit_queue_capacity_ > 0 && outstanding_ >= transmit_queue_capacity_)
    {
        return SendResult::QueueFull;
    }
    bus_.deliver(*this, frame);
    ++outstanding_;
    const PortEvent event{PortEvent::Kind::TransmitCompleted, frame.time, 0, frame.time, token, true};
    if (!events_.push(event.time, event))
    {
        event_overflow_.note(event.time);
    }
    return SendResult::Accepted;
}

std::size_t VirtualEthernetBus::Endpoint::release_events_before(SimTime now, PortEventSink sink)
{
    std::optional<PortEvent> overflow = overflow_.take_before(now);
    std::optional<PortEvent> eventOverflow = event_overflow_.take_before(now);
    auto accountingSink = [&](const PortEvent& event) {
        if ((event.kind == PortEvent::Kind::TransmitCompleted || event.kind == PortEvent::Kind::TransmitFailed) &&
            event.correlated)
        {
            if (outstanding_ > 0)
            {
                --outstanding_;
            }
        }
        else if (event.kind == PortEvent::Kind::EventOverflow)
        {
            const std::size_t drop = static_cast<std::size_t>(std::min<std::uint64_t>(outstanding_, event.count));
            outstanding_ -= drop;
        }
        sink(event);
    };
    return OverflowCollector::release_merged(overflow, accountingSink, [&](auto&& forward) {
        return OverflowCollector::release_merged(eventOverflow, forward, [&](auto&& inner) {
            return events_.drain_before(now, [&inner](SimTime, PortEvent& event) { inner(event); });
        });
    });
}

} // namespace vehicle_sil::eth
