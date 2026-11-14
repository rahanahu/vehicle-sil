// SPDX-License-Identifier: MIT
#include "vehicle_sil/step/real_time_clock.hpp"

#include <algorithm>
#include <stdexcept>

namespace vehicle_sil::step {

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
            throw std::invalid_argument("vehicle_sil::step::ManualRealTimeClock: time must not go back");
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
        throw std::invalid_argument("vehicle_sil::step::ManualRealTimeClock: duration must be >= 0");
    }
    advance_to(now() + duration);
}

} // namespace vehicle_sil::step
