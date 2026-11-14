// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth/virtual_ethernet_bus.hpp"

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
}

VirtualEthernetBus::Endpoint& VirtualEthernetBus::attach()
{
    // Endpoint's constructor is private; make_unique cannot reach it.
    endpoints_.push_back(std::unique_ptr<Endpoint>(new Endpoint(*this, options_.inbox_capacity)));
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
    if (frame.frame.size() < kHeaderBytes)
    {
        return SendResult::Failed;
    }
    bus_.deliver(*this, frame);
    return SendResult::Accepted;
}

std::size_t VirtualEthernetBus::Endpoint::release_events_before(SimTime now, PortEventSink sink)
{
    const std::optional<PortEvent> due = overflow_.take_before(now);
    if (!due)
    {
        return 0;
    }
    sink(*due);
    return 1;
}

} // namespace vehicle_sil::eth
