// SPDX-License-Identifier: MIT
//
// The requests to stop a run (launch::StopRequests) by themselves: the count that the run reads
// (1 stops in order, 2 or more skip the waits) and the source of the first request.

#include "test_harness.hpp"

#include "vehicle_sil/launch/stop_requests.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace {

using vehicle_sil::launch::StopRequests;

constexpr int kSignal = 2;
constexpr int kOther = 15;
constexpr int kEnd = -2;

} // namespace

TEST_CASE(stop_requests_begin_with_none)
{
    const StopRequests stops;
    CHECK_EQ(stops.count(), 0);
    CHECK_EQ(stops.first(), 0);
}

TEST_CASE(a_second_stop_request_counts_two_and_the_first_source_stays)
{
    StopRequests stops;
    stops.request(kSignal);
    CHECK_EQ(stops.count(), 1);
    CHECK_EQ(stops.first(), kSignal);
    stops.request(kOther);
    CHECK_EQ(stops.count(), 2);
    CHECK_EQ(stops.first(), kSignal);
}

TEST_CASE(a_stop_request_counted_twice_is_two_at_once)
{
    StopRequests stops;
    stops.request(kOther, 2);
    CHECK_EQ(stops.count(), 2);
    CHECK_EQ(stops.first(), kOther);

    // After an ordinary request too: the count is added, the source is the first one's.
    StopRequests later;
    later.request(kSignal);
    later.request(kOther, 2);
    CHECK_EQ(later.count(), 3);
    CHECK_EQ(later.first(), kSignal);
}

TEST_CASE(a_first_stop_request_runs_before_between_the_decision_and_the_request)
{
    StopRequests stops;
    int calls = 0;
    const bool asked = stops.request_first(kEnd, [&] {
        ++calls;
        // Decided (nothing else can be the first), but the run does not see a request yet.
        CHECK_EQ(stops.first(), kEnd);
        CHECK_EQ(stops.count(), 0);
    });
    CHECK(asked);
    CHECK_EQ(calls, 1);
    CHECK_EQ(stops.count(), 1);
    CHECK_EQ(stops.first(), kEnd);

    // Never a second request.
    CHECK(!stops.request_first(kEnd, [&] { ++calls; }));
    CHECK_EQ(calls, 1);
    CHECK_EQ(stops.count(), 1);
}

TEST_CASE(a_first_stop_request_after_another_request_does_nothing)
{
    StopRequests stops;
    stops.request(kSignal);
    bool called = false;
    CHECK(!stops.request_first(kEnd, [&] { called = true; }));
    CHECK(!called);
    CHECK_EQ(stops.count(), 1);
    CHECK_EQ(stops.first(), kSignal);
}

TEST_CASE(a_stop_request_that_comes_during_before_is_the_first_request_not_a_second)
{
    StopRequests stops;
    CHECK(stops.request_first(kEnd, [&] { stops.request(kSignal); }));
    CHECK_EQ(stops.count(), 1);
    CHECK_EQ(stops.first(), kEnd);

    // Two of them during `before` are two requests: the waits are skipped.
    StopRequests twice;
    CHECK(twice.request_first(kEnd, [&] {
        twice.request(kSignal);
        twice.request(kSignal);
    }));
    CHECK_EQ(twice.count(), 2);
    CHECK_EQ(twice.first(), kEnd);
}

TEST_CASE(stop_requests_from_several_threads_are_all_counted_and_one_is_the_first)
{
    constexpr int kThreads = 8;
    constexpr int kEach = 200;
    StopRequests stops;
    std::atomic<int> firsts{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&, source = i + 1] {
            while (!go.load())
            {
                std::this_thread::yield();
            }
            if (stops.request_first(source, [] {}))
            {
                ++firsts;
            }
            for (int n = 0; n < kEach; ++n)
            {
                stops.request(source);
            }
        });
    }
    go = true;
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    // At most one is the first by request_first() (none if a request() of another thread came
    // before every decision); it adds one to the count only if no request() had been counted.
    CHECK(firsts.load() <= 1);
    CHECK(stops.count() >= kThreads * kEach && stops.count() <= kThreads * kEach + 1);
    CHECK(stops.first() >= 1 && stops.first() <= kThreads);
}
