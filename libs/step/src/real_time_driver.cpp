// SPDX-License-Identifier: MIT
#include "vehicle_sil/step/real_time_driver.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::step {

namespace {

constexpr SimDuration kTick{1}; // 1 ns

} // namespace

// ---- RealTimeDriver ----

RealTimeDriver::RealTimeDriver(Host& host, RealTimeClock& clock, RealTimeDriverOptions options)
  : host_(host), clock_(clock), options_(options)
{
    if (options_.max_idle <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::step::RealTimeDriver: max_idle must be > 0");
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
            throw std::logic_error("vehicle_sil::step::RealTimeDriver: start() is only allowed once, before stop()");
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
    if (failed_ && options_.on_failure)
    {
        const std::string error = error_;
        lock.unlock();
        try
        {
            options_.on_failure(error);
        }
        catch (...)
        {
            // The driver has ended; nothing to do about a failing callback.
        }
    }
}

} // namespace vehicle_sil::step
