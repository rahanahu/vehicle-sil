// SPDX-License-Identifier: MIT
//
// SimTime, StepInbox, TickScheduler and RealTimePacer: pure logic, no SIL Kit connection.
#include "test_harness.hpp"

#include "vehicle_sil/core/real_time_pacer.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"
#include "vehicle_sil/core/tick_scheduler.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;

SimTime at(SimDuration d)
{
    return SimTime{d};
}

template <typename F>
bool throws_invalid_argument(F&& f)
{
    return vehicle_sil::unit::throws<std::invalid_argument>(std::forward<F>(f));
}

} // namespace

TEST_CASE(sim_time_is_a_distinct_type_from_wall_clock)
{
    static_assert(!std::is_convertible_v<std::chrono::steady_clock::time_point, SimTime>);
    static_assert(!std::is_convertible_v<SimTime, std::chrono::steady_clock::time_point>);
    static_assert(!std::is_convertible_v<std::chrono::system_clock::time_point, SimTime>);
    static_assert(std::is_same_v<SimDuration, std::chrono::nanoseconds>);

    const SimTime t = from_silkit_time(1500us);
    CHECK_EQ(to_ns(t), 1'500'000);
    CHECK(to_silkit_time(t) == 1500us);
    CHECK(SimTime{} + 10ms == from_silkit_time(10ms));
    CHECK_EQ(to_ns(SimTime{}), 0);
}

TEST_CASE(inbox_releases_only_messages_before_the_step)
{
    StepInbox<int> inbox(8);
    CHECK(inbox.push(at(5ms), 1));
    CHECK(inbox.push(at(10ms), 2)); // stamped exactly at the step: must wait for the next step
    CHECK(inbox.push(at(3ms), 3));

    std::vector<std::pair<SimTime, int>> out;
    const auto sink = [&](SimTime ts, int& v) { out.emplace_back(ts, v); };
    CHECK_EQ(inbox.drain_before(at(10ms), sink), 2u);
    CHECK_EQ(out.size(), 2u);
    CHECK(out[0].first == at(3ms));
    CHECK_EQ(out[0].second, 3);
    CHECK(out[1].first == at(5ms));
    CHECK_EQ(out[1].second, 1);
    CHECK_EQ(inbox.size(), 1u);

    out.clear();
    CHECK_EQ(inbox.drain_before(at(10ms), sink), 0u);
    CHECK_EQ(inbox.drain_before(at(20ms), sink), 1u);
    CHECK_EQ(out.at(0).second, 2);
    CHECK_EQ(inbox.size(), 0u);
}

TEST_CASE(inbox_orders_by_time_then_tie_break_then_arrival)
{
    struct Msg
    {
        int sender;
        int seq;
    };
    struct BySender
    {
        bool operator()(const Msg& a, const Msg& b) const { return a.sender < b.sender; }
    };
    StepInbox<Msg, BySender> inbox(16);
    // Arrival order interleaves two senders; sender 2's messages arrive first.
    inbox.push(at(1ms), {2, 0});
    inbox.push(at(1ms), {1, 0});
    inbox.push(at(1ms), {2, 1});
    inbox.push(at(0ms), {9, 0});
    inbox.push(at(1ms), {1, 1});

    std::vector<std::pair<int, int>> out;
    inbox.drain_before(at(2ms), [&](SimTime, Msg& m) { out.emplace_back(m.sender, m.seq); });
    const std::vector<std::pair<int, int>> expected{{9, 0}, {1, 0}, {1, 1}, {2, 0}, {2, 1}};
    CHECK(out == expected);
}

TEST_CASE(inbox_capacity_is_fixed_and_overflow_is_counted)
{
    StepInbox<int> inbox(2);
    CHECK(inbox.push(at(0ms), 1));
    CHECK(inbox.push(at(0ms), 2));
    CHECK(!inbox.push(at(0ms), 3));
    CHECK_EQ(inbox.overflows(), 1u);
    CHECK_EQ(inbox.capacity(), 2u);
    int sum = 0;
    inbox.drain_before(at(1ms), [&](SimTime, int& v) { sum += v; });
    CHECK_EQ(sum, 3);
    CHECK(inbox.push(at(1ms), 4));
    inbox.clear();
    CHECK_EQ(inbox.size(), 0u);
    CHECK(throws_invalid_argument([] { StepInbox<int> bad(0); }));
}

TEST_CASE(inbox_accepts_pushes_from_other_threads_while_draining)
{
    StepInbox<int> inbox(100'000);
    constexpr int kPerThread = 20'000;
    std::thread a([&] {
        for (int i = 0; i < kPerThread; ++i)
        {
            inbox.push(at(SimDuration(i)), i);
        }
    });
    std::thread b([&] {
        for (int i = 0; i < kPerThread; ++i)
        {
            inbox.push(at(SimDuration(i)), kPerThread + i);
        }
    });
    std::size_t drained = 0;
    long long lastTs = -1;
    bool ordered = true;
    while (drained < 2u * kPerThread)
    {
        lastTs = -1; // each drain is ordered on its own
        drained += inbox.drain_before(at(SimDuration(kPerThread)), [&](SimTime ts, int&) {
            ordered = ordered && to_ns(ts) >= lastTs;
            lastTs = to_ns(ts);
        });
    }
    a.join();
    b.join();
    CHECK(ordered);
    CHECK_EQ(drained, 2u * kPerThread);
    CHECK_EQ(inbox.overflows(), 0u);
}

TEST_CASE(tick_scheduler_runs_tasks_on_their_periods)
{
    TickScheduler scheduler(5ms);
    std::vector<std::string> log;
    const auto tick10 = scheduler.add("tick10", 10ms, [&](SimTime t) { log.push_back("a@" + std::to_string(to_ns(t) / 1'000'000)); });
    const auto tick5 = scheduler.add("tick5", 5ms, [&](SimTime t) { log.push_back("b@" + std::to_string(to_ns(t) / 1'000'000)); });
    const auto offset = scheduler.add(
        "tick20+5", 20ms, [&](SimTime t) { log.push_back("c@" + std::to_string(to_ns(t) / 1'000'000)); }, 5ms);

    for (SimDuration t = 0ms; t < 30ms; t += 5ms)
    {
        scheduler.run_due(at(t));
    }
    const std::vector<std::string> expected{"a@0", "b@0", "b@5", "c@5", "a@10", "b@10", "b@15",
                                            "a@20", "b@20", "b@25", "c@25"};
    CHECK(log == expected);
    CHECK_EQ(scheduler.tick_count(tick10), 3u);
    CHECK_EQ(scheduler.tick_count(tick5), 6u);
    CHECK_EQ(scheduler.tick_count(offset), 2u);
    CHECK(scheduler.name(offset) == "tick20+5");
}

TEST_CASE(tick_scheduler_rejects_periods_off_the_step_grid)
{
    CHECK(throws_invalid_argument([] { TickScheduler s(0ms); }));
    TickScheduler scheduler(10ms);
    const auto noop = [](SimTime) {};
    CHECK(throws_invalid_argument([&] { scheduler.add("x", 15ms, noop); }));
    CHECK(throws_invalid_argument([&] { scheduler.add("x", 0ms, noop); }));
    CHECK(throws_invalid_argument([&] { scheduler.add("x", 20ms, noop, 5ms); }));
    CHECK(throws_invalid_argument([&] { scheduler.add("x", 20ms, nullptr); }));
    CHECK(throws_invalid_argument([&] { scheduler.run_due(at(5ms)); }));
    CHECK_EQ(scheduler.task_count(), 0u);
}

TEST_CASE(pacer_maps_simulation_time_to_wall_clock)
{
    using Wall = RealTimePacer::WallClock;
    const Wall::time_point w0{std::chrono::seconds(100)};

    RealTimePacer fast(4.0);
    // First step anchors: [0, 10 ms) may end 2.5 ms after the start.
    CHECK(fast.step_deadline(at(0ms), 10ms, w0) == w0 + 2500us);
    // Later steps are relative to the anchor, not to when they started.
    CHECK(fast.step_deadline(at(40ms), 10ms, w0 + 1s) == w0 + 12500us);

    RealTimePacer slow(0.5);
    const Wall::time_point w1{std::chrono::seconds(7)};
    CHECK(slow.step_deadline(at(30ms), 10ms, w1) == w1 + 20ms);
    CHECK(slow.step_deadline(at(40ms), 10ms, w1 + 1ms) == w1 + 40ms);

    CHECK(throws_invalid_argument([] { RealTimePacer bad(0.0); }));
    CHECK(throws_invalid_argument([] { RealTimePacer bad(-1.0); }));
}
