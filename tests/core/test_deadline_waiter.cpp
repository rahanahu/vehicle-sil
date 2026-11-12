// SPDX-License-Identifier: MIT
//
// The time gate's timer (detail::DeadlineWaiter): it does not end before the deadline, a notify
// ends it, and it wakes close to the deadline. The last one measures real time: its bound is on
// the median of many waits, far above what a working timer needs, and it prints what it measured.
#include "test_harness.hpp"

#include "deadline_waiter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::detail::DeadlineWaiter;
using Wall = std::chrono::steady_clock;

struct Overshoot
{
    std::chrono::microseconds median{};
    std::chrono::microseconds max{};
};

/// `count` waits of `wait` each; by how much each ended after its deadline. `wait_until` waits like
/// the gate's thread: it may end early, and is called again.
template <class WaitUntil>
Overshoot measure(int count, std::chrono::microseconds wait, WaitUntil wait_until)
{
    std::vector<std::chrono::microseconds> over;
    for (int i = 0; i < count; ++i)
    {
        const auto deadline = Wall::now() + wait;
        while (Wall::now() < deadline)
        {
            wait_until(deadline);
        }
        over.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Wall::now() - deadline));
    }
    std::sort(over.begin(), over.end());
    return Overshoot{over[over.size() / 2], over.back()};
}

void print(const std::string& what, const Overshoot& o)
{
    std::printf("wake-up after a 10 ms deadline, %s: median %lld us, max %lld us\n", what.c_str(),
                static_cast<long long>(o.median.count()), static_cast<long long>(o.max.count()));
}

} // namespace

TEST_CASE(deadline_waiter_a_notify_ends_the_wait_and_is_not_lost)
{
    DeadlineWaiter waiter;
    std::mutex mutex;
    bool flag = false;
    // The notify comes before the wait begins: the state changed under the mutex, so the waiter
    // sees it without waiting.
    {
        const std::lock_guard lock(mutex);
        flag = true;
    }
    waiter.notify();
    {
        std::unique_lock lock(mutex);
        while (!flag)
        {
            waiter.wait(lock);
        }
    }
    // The notify comes while it waits: for a deadline, and without one.
    for (const bool with_deadline : {true, false})
    {
        flag = false;
        const auto begin = Wall::now();
        std::thread notifier([&] {
            std::this_thread::sleep_for(50ms);
            {
                const std::lock_guard lock(mutex);
                flag = true;
            }
            waiter.notify();
        });
        {
            std::unique_lock lock(mutex);
            while (!flag)
            {
                if (with_deadline)
                {
                    waiter.wait_until(lock, begin + 60s);
                }
                else
                {
                    waiter.wait(lock);
                }
            }
        }
        notifier.join();
        CHECK(Wall::now() - begin < 30s);
    }
}

TEST_CASE(deadline_waiter_a_deadline_that_has_passed_does_not_wait)
{
    DeadlineWaiter waiter;
    std::mutex mutex;
    std::unique_lock lock(mutex);
    const auto begin = Wall::now();
    waiter.wait_until(lock, begin - 1s);
    waiter.wait_until(lock, begin);
    CHECK(lock.owns_lock());
    CHECK(Wall::now() - begin < 10s);
}

TEST_CASE(virtual_time_deadline_waiter_wakes_close_to_the_deadline)
{
    std::mutex mutex;
    const auto with = [&mutex](DeadlineWaiter& waiter) {
        return measure(100, 10ms, [&](Wall::time_point deadline) {
            std::unique_lock lock(mutex);
            waiter.wait_until(lock, deadline);
        });
    };
    // First, and printed only: a plain condition variable, before anything here changes the
    // system's timer tick.
    std::condition_variable cv;
    print("std::condition_variable", measure(100, 10ms, [&](Wall::time_point deadline) {
              std::unique_lock lock(mutex);
              cv.wait_until(lock, deadline);
          }));
    DeadlineWaiter best;
    const Overshoot o = with(best);
    print(best.high_resolution() ? "the gate's timer" : "the gate's timer (no high-resolution timer here)", o);
    // A timer that ends on a 15.6 ms tick is 5 ms late or more at the median of 10 ms waits.
    CHECK(o.median < 5ms);
    // Printed only: the fallback.
    DeadlineWaiter ordinary(DeadlineWaiter::Timer::Ordinary);
    print("the fallback timer", with(ordinary));
}
