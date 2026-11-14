// SPDX-License-Identifier: MIT
//
// Collects the frames a port drops into one PortEvent::ReceiveOverflow (vehicle_sil::eth).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/eth/port.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace vehicle_sil::eth {

/// For port implementations: collects dropped received frames into one
/// PortEvent::ReceiveOverflow and hands it over without taking a place in the event queue, so it
/// never pushes out a transmit acknowledgement. Its time is the earliest time of the dropped
/// frames, its count the number dropped until it is released.
///
/// Not thread safe: the port guards it with its own lock.
class OverflowCollector
{
public:
    /// A received frame with time `time` was dropped.
    void note(SimTime time) noexcept
    {
        if (!event_)
        {
            event_ = PortEvent{PortEvent::Kind::ReceiveOverflow, time, 0};
        }
        else if (time < event_->time)
        {
            event_->time = time;
        }
        ++event_->count;
    }

    /// Removes and returns the event if its time is before `now`.
    [[nodiscard]] std::optional<PortEvent> take_before(SimTime now) noexcept
    {
        if (!event_ || !(event_->time < now))
        {
            return std::nullopt;
        }
        const PortEvent event = *event_;
        event_.reset();
        return event;
    }

    void clear() noexcept { event_.reset(); }

    /// Calls `drain(forward)`, where `drain` hands the queued events (time ordered) to `forward`,
    /// and passes `due` (the result of take_before()) to `sink` at its place in time order:
    /// after the queued events with a time not later than it. Returns the number of events.
    template <typename Drain>
    static std::size_t release_merged(std::optional<PortEvent> due, PortEventSink sink, Drain&& drain)
    {
        std::size_t extra = 0;
        const std::size_t queued = drain([&](const PortEvent& event) {
            if (due && event.time > due->time)
            {
                sink(*due);
                due.reset();
                ++extra;
            }
            sink(event);
        });
        if (due)
        {
            sink(*due);
            ++extra;
        }
        return queued + extra;
    }

private:
    std::optional<PortEvent> event_;
};

} // namespace vehicle_sil::eth
