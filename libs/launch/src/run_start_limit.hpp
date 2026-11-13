// SPDX-License-Identifier: MIT
//
// The limit in time of a run's start (the give-up of SimulationController::start()), shared by
// the main thread and the start window's thread. Internal; it has no clock of its own (the
// callers pass the time), so its order of events can be tested as written.
#pragma once

#include <chrono>
#include <mutex>

namespace vehicle_sil::launch::detail {

/// When the start is given up, and what the message says about it.
///
/// Thread safe: the main thread reads it every few milliseconds (take()) while the window's
/// thread changes it (at(), begin_hold(), end_hold()). Every call writes what belongs together
/// under the one mutex in one go, and nothing is taken while the mutex is held. (Written in two
/// goes, a look in between could find the hold's cleared length and no hold yet, and set a limit
/// of 0 ms.)
class StartLimit
{
public:
    using Clock = std::chrono::steady_clock;

    /// The limit as the give-up of the start reads it.
    struct Now
    {
        Clock::time_point at = Clock::time_point::max(); ///< max(): none (yet)
        std::chrono::milliseconds length{};              ///< how long it was, for the message
        bool after_hold = false;                         ///< it is the one after the hold
    };

    /// The participants have until `at` (`length` from now) to get to their first step.
    void set(Clock::time_point at, std::chrono::milliseconds length)
    {
        const std::lock_guard lock(mutex_);
        now_ = Now{at, length, false};
        armed_ = true;
    }
    /// A run that waits for participants started by hand: no limit until they are ready for their
    /// first step, then `length` (take() begins it).
    void set_once_manual_ready(std::chrono::milliseconds length)
    {
        const std::lock_guard lock(mutex_);
        now_ = Now{Clock::time_point::max(), length, false};
        armed_ = false;
    }
    /// The run stands still before its first step: no limit, and none begins.
    void begin_hold()
    {
        const std::lock_guard lock(mutex_);
        now_ = Now{};
        holding_ = true;
    }
    /// The hold is over at `now`: `length` from here, or, with participants started by hand
    /// (`manual`), from when those are ready (take() begins it).
    void end_hold(Clock::time_point now, std::chrono::milliseconds length, bool manual)
    {
        const std::lock_guard lock(mutex_);
        now_ = Now{manual ? Clock::time_point::max() : now + length, length, true};
        holding_ = false;
        armed_ = !manual;
    }
    /// The limit at `now`. With `manual_ready` (the run waits for participants started by hand, and
    /// every one of them is ready for its first step), a limit that has not begun begins here,
    /// unless the run stands still before the first step.
    [[nodiscard]] Now take(Clock::time_point now, bool manual_ready)
    {
        const std::lock_guard lock(mutex_);
        if (!armed_ && !holding_ && manual_ready)
        {
            armed_ = true;
            now_.at = now + now_.length;
        }
        return now_;
    }
    [[nodiscard]] Clock::time_point at() const
    {
        const std::lock_guard lock(mutex_);
        return now_.at;
    }

private:
    mutable std::mutex mutex_;
    Now now_{};
    /// false in a run with participants started by hand, until they are ready (take()).
    bool armed_ = true;
    /// The run stands still before its first step (no limit then).
    bool holding_ = false;
};

} // namespace vehicle_sil::launch::detail
