// SPDX-License-Identifier: MIT
//
// Collects the frames a port drops into one PortEvent::ReceiveOverflow (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace vehicle_sil::can {

/// For port implementations: collects dropped received frames into one
/// PortEvent::ReceiveOverflow, and hands it over without taking a place in the event queue (so
/// it can never push out a transmit acknowledgement).
///
/// The event's time is the earliest time of the frames dropped; its count is the number of
/// frames dropped until it is released, or 0 if an unknown number was part of it.
///
/// Not thread safe: the port guards it with its own lock.
class OverflowCollector
{
public:
    /// `count` frames were dropped at `time` (0: an unknown number).
    void note(SimTime time, std::uint64_t count) noexcept
    {
        if (!event_)
        {
            PortEvent event;
            event.kind = PortEvent::Kind::ReceiveOverflow;
            event.time = time;
            event_ = event;
        }
        else if (time < event_->time)
        {
            event_->time = time;
        }
        if (count == 0)
        {
            unknown_ = true;
        }
        event_->count += count;
    }

    /// Removes and returns the event if its time is before `now`.
    [[nodiscard]] std::optional<PortEvent> take_before(SimTime now) noexcept
    {
        if (!event_ || !(event_->time < now))
        {
            return std::nullopt;
        }
        PortEvent event = *event_;
        if (unknown_)
        {
            event.count = 0;
        }
        clear();
        return event;
    }

    void clear() noexcept
    {
        event_.reset();
        unknown_ = false;
    }

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
    bool unknown_ = false;
};

} // namespace vehicle_sil::can
