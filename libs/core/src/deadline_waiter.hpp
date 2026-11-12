// SPDX-License-Identifier: MIT
//
// A wait for one thread that ends at a steady_clock deadline, or earlier when another thread
// notifies it; made to wake close to the deadline. Internal to vehicle_sil::core (the time gate).
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace vehicle_sil::detail {

/// Like a condition variable for a single waiting thread: the caller holds `lock`, looks at its
/// state, and waits; the wait may end early or without a reason, so the caller looks again. A
/// notify() between the look and the wait is not lost as long as the notifier changed the state
/// under the same mutex.
///
/// std::condition_variable::wait_until() is close enough where it waits on the monotonic clock
/// with the kernel's timers (Linux: tens of microseconds). On Windows it is not: winpthreads
/// turns the deadline into a relative wait in whole milliseconds, which ends on the system's
/// timer tick (about 15.6 ms apart unless some program asked for a finer one). There this class
/// waits on a high-resolution waitable timer; on a Windows without one (before Windows 10 1803),
/// on an ordinary waitable timer with the timer tick at 1 ms for as long as the object lives.
class DeadlineWaiter
{
public:
    enum class Timer
    {
        Best,     ///< the most precise one there is
        Ordinary, ///< Windows: the fallback, although a better one may exist (for tests)
    };

    explicit DeadlineWaiter(Timer timer = Timer::Best);
    ~DeadlineWaiter();
    DeadlineWaiter(const DeadlineWaiter&) = delete;
    DeadlineWaiter& operator=(const DeadlineWaiter&) = delete;

    /// Ends a wait that is going on, or the next one. Thread safe; never throws.
    void notify() noexcept;
    void wait(std::unique_lock<std::mutex>& lock);
    void wait_until(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline);

    /// Windows: whether the high-resolution timer is in use. Elsewhere: true.
    [[nodiscard]] bool high_resolution() const noexcept { return high_resolution_; }

private:
    bool high_resolution_ = true;
#ifdef _WIN32
    void* event_ = nullptr; ///< auto-reset: a notify() stays until a wait takes it
    void* timer_ = nullptr;
    bool period_ = false;   ///< timeBeginPeriod(1) is in effect
#else
    std::condition_variable cv_;
#endif
};

} // namespace vehicle_sil::detail
