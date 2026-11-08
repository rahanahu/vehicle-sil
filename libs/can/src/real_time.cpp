// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/can/real_time_receiver.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::can {

namespace {

constexpr SimDuration kTick{1}; // 1 ns

} // namespace

// ---- SteadyRealTimeClock ----

SimTime SteadyRealTimeClock::now() const
{
    return SimTime{} + std::chrono::duration_cast<SimDuration>(std::chrono::steady_clock::now() - origin_);
}

void SteadyRealTimeClock::wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, SimTime deadline) const
{
    cv.wait_until(lock, origin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(deadline.time_since_epoch()));
}

// ---- ManualRealTimeClock ----

SimTime ManualRealTimeClock::now() const
{
    const std::lock_guard lock(time_mutex_);
    return now_;
}

void ManualRealTimeClock::wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, SimTime deadline) const
{
    // The caller holds its lock from its check until cv.wait() releases it, and a listener takes
    // that lock before it notifies: an advance_to() in between is not lost.
    if (now() < deadline)
    {
        cv.wait(lock);
    }
}

void ManualRealTimeClock::add_listener(Listener& listener)
{
    const std::lock_guard lock(listener_mutex_);
    listeners_.push_back(&listener);
}

void ManualRealTimeClock::remove_listener(Listener& listener) noexcept
{
    const std::lock_guard lock(listener_mutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), &listener), listeners_.end());
}

void ManualRealTimeClock::advance_to(SimTime time)
{
    {
        const std::lock_guard lock(time_mutex_);
        if (time < now_)
        {
            throw std::invalid_argument("vehicle_sil::can::ManualRealTimeClock: time must not go back");
        }
        now_ = time;
    }
    const std::lock_guard lock(listener_mutex_);
    for (Listener* listener : listeners_)
    {
        listener->on_clock_changed();
    }
}

void ManualRealTimeClock::advance(SimDuration duration)
{
    if (duration < SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::can::ManualRealTimeClock: duration must be >= 0");
    }
    advance_to(now() + duration);
}

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

// ---- RealTimeDriver ----

RealTimeDriver::RealTimeDriver(Host& host, RealTimeClock& clock, RealTimeDriverOptions options)
  : host_(host), clock_(clock), options_(options)
{
    if (options_.max_idle <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::can::RealTimeDriver: max_idle must be > 0");
    }
    host_.require_time_domain(TimeDomain::RealTime);
    host_.set_step_size(options_.max_idle);
}

RealTimeDriver::~RealTimeDriver()
{
    stop();
}

void RealTimeDriver::start()
{
    const std::lock_guard control(control_mutex_);
    {
        const std::lock_guard lock(mutex_);
        if (started_ || stop_)
        {
            throw std::logic_error("vehicle_sil::can::RealTimeDriver: start() is only allowed once, before stop()");
        }
        started_ = true;
        listening_ = true; // registered below, before the thread can call stop()
    }
    host_.set_wakeup([this](SimTime arrival) { on_arrival(arrival); });
    clock_.add_listener(*this);
    try
    {
        thread_ = std::thread([this] { run(); });
    }
    catch (...)
    {
        host_.set_wakeup({});
        clock_.remove_listener(*this);
        const std::lock_guard lock(mutex_);
        listening_ = false;
        ended_ = true;
        throw;
    }
}

void RealTimeDriver::stop() noexcept
{
    bool detach = false;
    bool own = false;
    {
        const std::lock_guard lock(mutex_);
        stop_ = true;
        detach = listening_;
        listening_ = false;
        own = started_ && thread_id_ == std::this_thread::get_id();
    }
    cv_.notify_all();
    if (detach)
    {
        // Also from the driver's own thread: after the driver is destroyed, nothing may call it.
        try
        {
            host_.set_wakeup({});
        }
        catch (...)
        {
        }
        clock_.remove_listener(*this);
    }
    if (own)
    {
        return; // from a step: the thread ends after it; a later stop() joins it
    }
    const std::lock_guard control(control_mutex_);
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void RealTimeDriver::on_arrival(SimTime arrival) noexcept
{
    {
        const std::lock_guard lock(mutex_);
        kicked_ = true;
        const SimTime after = arrival + kTick;
        if (!earliest_next_ || after > *earliest_next_)
        {
            earliest_next_ = after;
        }
    }
    cv_.notify_all();
}

void RealTimeDriver::on_clock_changed() noexcept
{
    {
        const std::lock_guard lock(mutex_);
        clock_changed_ = true;
    }
    cv_.notify_all();
}

bool RealTimeDriver::wait_idle(std::chrono::nanoseconds timeout)
{
    std::unique_lock lock(mutex_);
    return idle_cv_.wait_for(lock, timeout, [this] { return !started_ || ended_ || (sleeping_ && !kicked_ && !clock_changed_); });
}

bool RealTimeDriver::running() const
{
    const std::lock_guard lock(mutex_);
    return started_ && !ended_;
}

bool RealTimeDriver::failed() const
{
    const std::lock_guard lock(mutex_);
    return failed_;
}

std::string RealTimeDriver::error() const
{
    const std::lock_guard lock(mutex_);
    return error_;
}

std::uint64_t RealTimeDriver::steps() const
{
    const std::lock_guard lock(mutex_);
    return steps_;
}

std::optional<SimTime> RealTimeDriver::last_step() const
{
    const std::lock_guard lock(mutex_);
    return last_step_;
}

void RealTimeDriver::run()
{
    std::unique_lock lock(mutex_);
    thread_id_ = std::this_thread::get_id();
    SimTime wake = clock_.now(); // the first step runs right away
    while (!stop_)
    {
        clock_changed_ = false;
        const bool due = kicked_ || clock_.now() >= wake;
        if (!due)
        {
            sleeping_ = true;
            idle_cv_.notify_all();
            clock_.wait_until(cv_, lock, wake);
            sleeping_ = false;
            continue;
        }
        kicked_ = false;
        SimTime now = clock_.now();
        if (earliest_next_ && now < *earliest_next_)
        {
            now = *earliest_next_;
        }
        if (last_step_ && now <= *last_step_)
        {
            now = *last_step_ + kTick;
        }
        last_step_ = now;
        lock.unlock();
        try
        {
            host_.step(now);
        }
        catch (const std::exception& e)
        {
            lock.lock();
            failed_ = true;
            error_ = e.what();
            break;
        }
        catch (...)
        {
            lock.lock();
            failed_ = true;
            error_ = "unknown exception";
            break;
        }
        wake = now + options_.max_idle;
        if (const auto deadline = host_.next_deadline(); deadline && *deadline + kTick < wake)
        {
            // A deadline the step has already passed (a timer started late in the step) asks for
            // the next step; it never makes the step times run ahead of the clock.
            wake = std::max(*deadline + kTick, now + kTick);
        }
        lock.lock();
        ++steps_;
    }
    ended_ = true;
    sleeping_ = false;
    idle_cv_.notify_all();
}

} // namespace vehicle_sil::can
