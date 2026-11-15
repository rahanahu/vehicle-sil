// SPDX-License-Identifier: MIT
//
// Collects what a port drops into one overflow event, for every bus type (vehicle_sil::step).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/step/sink_ref.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace vehicle_sil::step {

/// For port implementations: collects dropped received frames (or dropped events) into one
/// `Event` of the kind given to the constructor (default: `Event::Kind::ReceiveOverflow`), and
/// hands it over without taking a place in the event queue, so it never pushes out a transmit
/// acknowledgement.
///
/// The event's time is the earliest time of what was dropped; its count is the number dropped
/// until it is released, or 0 if an unknown number was part of it.
///
/// `Event` is a bus's port event (`can::PortEvent`, `eth::PortEvent`): a default constructible
/// struct with an enum `Kind` and the members `kind`, `time` (SimTime) and `count`
/// (std::uint64_t). Each bus names its own instance (`can::OverflowCollector`,
/// `eth::OverflowCollector`).
///
/// Not thread safe: the port guards it with its own lock.
template <typename Event>
class OverflowCollector
{
public:
    using Kind = typename Event::Kind;

    explicit OverflowCollector(Kind kind = Kind::ReceiveOverflow) noexcept : kind_(kind) {}

    /// `count` frames (or events) were dropped at `time` (0: an unknown number).
    void note(SimTime time, std::uint64_t count = 1) noexcept
    {
        if (!event_)
        {
            Event event;
            event.kind = kind_;
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
    [[nodiscard]] std::optional<Event> take_before(SimTime now) noexcept
    {
        if (!event_ || !(event_->time < now))
        {
            return std::nullopt;
        }
        Event event = *event_;
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
    static std::size_t release_merged(std::optional<Event> due, SinkRef<Event> sink, Drain&& drain)
    {
        std::size_t extra = 0;
        const std::size_t queued = drain([&](const Event& event) {
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
    Kind kind_;
    std::optional<Event> event_;
    bool unknown_ = false;
};

} // namespace vehicle_sil::step
