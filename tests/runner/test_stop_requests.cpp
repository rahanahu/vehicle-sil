// SPDX-License-Identifier: MIT
//
// The requests to stop a run (vehicle_sil::runner::StopRequests) by themselves: the count that the run reads
// (1 stops in order, 2 or more skip the waits) and the source of the first request.

#include "test_harness.hpp"

#include "vehicle_sil/runner/stop_requests.hpp"

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

using vehicle_sil::runner::StopKind;
using vehicle_sil::runner::StopRequests;
namespace stop_source = vehicle_sil::runner::stop_source;

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
    namespace stop_level = vehicle_sil::runner::stop_level;
    StopRequests stops;
    stops.request(kOther, stop_level::kForce);
    CHECK_EQ(stops.count(), stop_level::kForce);
    CHECK_EQ(stops.first(), kOther);

    // After an ordinary request too: the count is added, the source is the first one's.
    StopRequests later;
    later.request(kSignal);
    later.request(kOther, stop_level::kForce);
    CHECK_EQ(later.count(), stop_level::kGraceful + stop_level::kForce);
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

TEST_CASE(the_known_stop_sources_keep_their_values_and_names)
{
    using vehicle_sil::runner::stop_source_name;
    CHECK_EQ(stop_source::kStopCommand, -1);
    CHECK_EQ(stop_source::kEndOfInput, -2);
    CHECK_EQ(stop_source::kOutputClosed, -3);
    CHECK_EQ(stop_source::kControlStop, -4);
    CHECK(stop_source_name(stop_source::kStopCommand) == std::string("stop command"));
    CHECK(stop_source_name(stop_source::kEndOfInput) == std::string("end of input"));
    CHECK(stop_source_name(stop_source::kOutputClosed) == std::string("output closed"));
    CHECK(stop_source_name(stop_source::kControlStop) == std::string("control server stop"));
    // A signal or a console event is named by the caller, not here.
    CHECK(stop_source_name(kSignal) == std::string());
    CHECK(stop_source_name(-5) == std::string());
}

TEST_CASE(a_stop_source_has_the_kind_of_who_asked)
{
    using vehicle_sil::runner::stop_kind;
    using vehicle_sil::runner::stop_kind_name;
    CHECK(stop_kind(stop_source::kStopCommand) == StopKind::Operator);
    CHECK(stop_kind(stop_source::kControlStop) == StopKind::Operator);
    CHECK(stop_kind(stop_source::kEndOfInput) == StopKind::Supervisor);
    CHECK(stop_kind(stop_source::kOutputClosed) == StopKind::Supervisor);
    CHECK(stop_kind(kSignal) == StopKind::Signal);
    CHECK(stop_kind(kOther) == StopKind::Signal);
    CHECK(std::string(stop_kind_name(StopKind::Operator)) == std::string("operator"));
    CHECK(std::string(stop_kind_name(StopKind::Signal)) == std::string("signal"));
    CHECK(std::string(stop_kind_name(StopKind::Supervisor)) == std::string("supervisor"));
}

TEST_CASE(stop_requests_raise_to_is_idempotent_monotonic_and_preserves_first_source)
{
    namespace stop_level = vehicle_sil::runner::stop_level;
    StopRequests stops;
    CHECK_EQ(stops.raise_to(kSignal, stop_level::kNone), stop_level::kNone);
    CHECK_EQ(stops.raise_to(kSignal, -2), stop_level::kNone);
    CHECK_EQ(stops.count(), stop_level::kNone);
    CHECK_EQ(stops.first(), 0);

    CHECK_EQ(stops.raise_to(kSignal, stop_level::kGraceful), stop_level::kGraceful);
    CHECK_EQ(stops.count(), stop_level::kGraceful);
    CHECK_EQ(stops.first(), kSignal);
    CHECK_EQ(stops.raise_to(kOther, -1), stop_level::kGraceful);
    CHECK_EQ(stops.raise_to(kOther, stop_level::kNone), stop_level::kGraceful);
    CHECK_EQ(stops.count(), stop_level::kGraceful);
    CHECK_EQ(stops.first(), kSignal);

    // Repeating the same level is a no-op and preserves the first source.
    CHECK_EQ(stops.raise_to(kOther, stop_level::kGraceful), stop_level::kGraceful);
    CHECK_EQ(stops.count(), stop_level::kGraceful);
    CHECK_EQ(stops.first(), kSignal);

    // Escalating to kForce raises count_ to kForce and preserves the first source.
    CHECK_EQ(stops.raise_to(kOther, stop_level::kForce), stop_level::kForce);
    CHECK_EQ(stops.count(), stop_level::kForce);
    CHECK_EQ(stops.first(), kSignal);

    // Repeating kForce or passing a lower level never lowers count_.
    CHECK_EQ(stops.raise_to(kEnd, stop_level::kForce), stop_level::kForce);
    CHECK_EQ(stops.raise_to(kEnd, stop_level::kGraceful), stop_level::kForce);
    CHECK_EQ(stops.raise_to(kEnd, stop_level::kNone), stop_level::kForce);
    CHECK_EQ(stops.raise_to(kEnd, -1), stop_level::kForce);
    CHECK_EQ(stops.count(), stop_level::kForce);
    CHECK_EQ(stops.first(), kSignal);

    // Direct jump from kNone to kForce.
    StopRequests direct;
    CHECK_EQ(direct.raise_to(kOther, stop_level::kForce), stop_level::kForce);
    CHECK_EQ(direct.count(), stop_level::kForce);
    CHECK_EQ(direct.first(), kOther);

    // Interaction with request() and request_first().
    StopRequests mixed;
    mixed.request(kSignal, 3);
    CHECK_EQ(mixed.raise_to(kOther, stop_level::kForce), 3);
    CHECK_EQ(mixed.count(), 3);
    CHECK_EQ(mixed.first(), kSignal);

    StopRequests mixed2;
    CHECK_EQ(mixed2.raise_to(kSignal, stop_level::kGraceful), stop_level::kGraceful);
    CHECK(!mixed2.request_first(kEnd, [] {}));
    mixed2.request(kOther);
    CHECK_EQ(mixed2.count(), stop_level::kForce);
    CHECK_EQ(mixed2.first(), kSignal);
}

TEST_CASE(stop_requests_raise_to_under_multithreaded_contention)
{
    constexpr int kRounds = 8;
    constexpr int kActiveThreads = 8;
    constexpr int kNoOpThreads = 4;

    struct ThreadResult
    {
        int r_neg = -1;
        int r0 = -1;
        int r1 = -1;
        int first_after_r1 = 0;
        int r2 = -1;
        int r3 = -1;
        int r_final_neg = -1;
        int first_after_r2 = 0;
    };

    for (int round = 0; round < kRounds; ++round)
    {
        StopRequests stops;
        std::atomic<bool> go{false};
        std::vector<ThreadResult> active_results(kActiveThreads);
        std::vector<ThreadResult> noop_results(kNoOpThreads);
        std::vector<std::thread> threads;
        threads.reserve(kActiveThreads + kNoOpThreads);

        for (int i = 0; i < kActiveThreads; ++i)
        {
            threads.emplace_back([&, i, source = i + 1] {
                while (!go.load())
                {
                    std::this_thread::yield();
                }
                namespace stop_level = vehicle_sil::runner::stop_level;
                ThreadResult& res = active_results[static_cast<std::size_t>(i)];
                res.r_neg = stops.raise_to(source, -1);
                res.r0 = stops.raise_to(source, stop_level::kNone);
                res.r1 = stops.raise_to(source, stop_level::kGraceful);
                res.first_after_r1 = stops.first();
                res.r2 = stops.raise_to(source, stop_level::kForce);
                res.r3 = stops.raise_to(source, stop_level::kGraceful);
                res.r_final_neg = stops.raise_to(source, -1);
                res.first_after_r2 = stops.first();
            });
        }
        for (int j = 0; j < kNoOpThreads; ++j)
        {
            threads.emplace_back([&, j, source = 100 + j] {
                while (!go.load())
                {
                    std::this_thread::yield();
                }
                ThreadResult& res = noop_results[static_cast<std::size_t>(j)];
                res.r_neg = stops.raise_to(source, -1);
                res.r0 = stops.raise_to(source, vehicle_sil::runner::stop_level::kNone);
            });
        }

        go = true;
        for (std::thread& t : threads)
        {
            t.join();
        }

        CHECK_EQ(stops.count(), 2);
        const int first = stops.first();
        CHECK(first >= 1 && first <= kActiveThreads);

        for (const ThreadResult& res : active_results)
        {
            CHECK(res.r_neg >= 0 && res.r_neg <= 2);
            CHECK(res.r0 >= res.r_neg && res.r0 <= 2);
            CHECK(res.r1 >= 1 && res.r1 <= 2);
            CHECK(res.r1 >= res.r0);
            CHECK_EQ(res.first_after_r1, first);
            CHECK_EQ(res.r2, 2);
            CHECK_EQ(res.r3, 2);
            CHECK_EQ(res.r_final_neg, 2);
            CHECK_EQ(res.first_after_r2, first);
        }
        for (const ThreadResult& res : noop_results)
        {
            CHECK(res.r_neg >= 0 && res.r_neg <= 2);
            CHECK(res.r0 >= res.r_neg && res.r0 <= 2);
        }
    }
}
