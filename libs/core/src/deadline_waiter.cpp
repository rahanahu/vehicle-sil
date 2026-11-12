// SPDX-License-Identifier: MIT
#include "deadline_waiter.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <timeapi.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#endif

namespace vehicle_sil::detail {

#ifdef _WIN32

namespace {

// Windows 10 1803 and later; older SDK headers do not name it.
constexpr DWORD kHighResolutionTimer = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION

} // namespace

DeadlineWaiter::DeadlineWaiter(Timer timer)
{
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr)
    {
        throw std::runtime_error("DeadlineWaiter: CreateEvent failed");
    }
    high_resolution_ = false;
    if (timer == Timer::Best)
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer, TIMER_ALL_ACCESS);
        high_resolution_ = timer_ != nullptr;
    }
    if (timer_ == nullptr)
    {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (timer_ == nullptr)
        {
            CloseHandle(event_);
            throw std::runtime_error("DeadlineWaiter: CreateWaitableTimer failed");
        }
        period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
}

DeadlineWaiter::~DeadlineWaiter()
{
    if (period_)
    {
        timeEndPeriod(1);
    }
    CloseHandle(timer_);
    CloseHandle(event_);
}

void DeadlineWaiter::notify() noexcept
{
    SetEvent(event_);
}

void DeadlineWaiter::wait(std::unique_lock<std::mutex>& lock)
{
    lock.unlock();
    WaitForSingleObject(event_, INFINITE);
    lock.lock();
}

void DeadlineWaiter::wait_until(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline)
{
    lock.unlock();
    // A relative time (negative, in 100 ns): it runs on the same monotonic clock as steady_clock,
    // not on the system time. If the timer cannot be set, the caller simply looks again.
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() > 0)
    {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<std::int64_t>(1, (left.count() + 99) / 100);
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != 0)
        {
            HANDLE handles[2] = {event_, timer_};
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }
        else
        {
            WaitForSingleObject(event_, 1);
        }
    }
    lock.lock();
}

#else

DeadlineWaiter::DeadlineWaiter(Timer) {}

DeadlineWaiter::~DeadlineWaiter() = default;

void DeadlineWaiter::notify() noexcept
{
    cv_.notify_all();
}

void DeadlineWaiter::wait(std::unique_lock<std::mutex>& lock)
{
    cv_.wait(lock);
}

void DeadlineWaiter::wait_until(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline)
{
    cv_.wait_until(lock, deadline);
}

#endif

} // namespace vehicle_sil::detail
