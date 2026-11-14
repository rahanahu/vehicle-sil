// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/real_time_receiver.hpp"

#include <utility>

namespace vehicle_sil::can {

namespace {

constexpr SimDuration kTick{1}; // 1 ns

} // namespace

// ---- RealTimeReceiver ----

RealTimeReceiver::RealTimeReceiver(const RealTimeClock& clock, std::size_t frame_capacity, std::size_t event_capacity)
  : clock_(clock), frames_(frame_capacity), events_(event_capacity)
{
}

SimTime RealTimeReceiver::stamp_locked()
{
    SimTime stamp = clock_.now();
    if (released_ && stamp < *released_)
    {
        stamp = *released_;
    }
    if (last_stamp_ && stamp <= *last_stamp_)
    {
        stamp = *last_stamp_ + kTick;
    }
    last_stamp_ = stamp;
    return stamp;
}

void RealTimeReceiver::wake_locked(SimTime time) const
{
    if (wakeup_)
    {
        wakeup_(time);
    }
}

bool RealTimeReceiver::push_frame(const Frame& frame)
{
    const std::lock_guard lock(mutex_);
    const SimTime stamp = stamp_locked();
    const bool queued = frames_.push(stamp, frame);
    if (!queued)
    {
        overflow_.note(stamp, 1);
    }
    wake_locked(stamp);
    return queued;
}

bool RealTimeReceiver::push_event(PortEvent event, std::size_t reserve)
{
    const std::lock_guard lock(mutex_);
    if (reserve > 0 && events_.size() + reserve >= events_.capacity())
    {
        ++reserve_refusals_;
        return false;
    }
    const SimTime stamp = stamp_locked();
    event.time = stamp;
    const bool queued = events_.push(stamp, event);
    wake_locked(stamp);
    return queued;
}

std::uint64_t RealTimeReceiver::event_overflows() const
{
    const std::lock_guard lock(mutex_);
    return events_.overflows() + reserve_refusals_;
}

void RealTimeReceiver::report_overflow(std::uint64_t count)
{
    const std::lock_guard lock(mutex_);
    const SimTime stamp = stamp_locked();
    overflow_.note(stamp, count);
    wake_locked(stamp);
}

std::size_t RealTimeReceiver::release_before(SimTime now, FrameSink sink)
{
    {
        const std::lock_guard lock(mutex_);
        if (!released_ || now > *released_)
        {
            released_ = now; // later arrivals are stamped >= now
        }
    }
    return frames_.drain_before(now, [&sink](SimTime time, Frame& frame) { sink(TimedFrame{time, frame}); });
}

std::size_t RealTimeReceiver::release_events_before(SimTime now, PortEventSink sink)
{
    std::optional<PortEvent> overflow;
    {
        const std::lock_guard lock(mutex_);
        if (!released_ || now > *released_)
        {
            released_ = now;
        }
        overflow = overflow_.take_before(now);
    }
    return OverflowCollector::release_merged(overflow, sink, [&](auto&& forward) {
        return events_.drain_before(now, [&forward](SimTime, PortEvent& event) { forward(event); });
    });
}

void RealTimeReceiver::set_wakeup(Wakeup wakeup)
{
    const std::lock_guard lock(mutex_);
    wakeup_ = std::move(wakeup);
}

void RealTimeReceiver::clear()
{
    const std::lock_guard lock(mutex_);
    frames_.clear();
    events_.clear();
    overflow_.clear();
}

} // namespace vehicle_sil::can
