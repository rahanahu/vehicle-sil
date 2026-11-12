// SPDX-License-Identifier: MIT
//
// The pacing numbers of the time gate (TimeControlOptions has the definitions), as worked
// examples with exact times: the gate's state (detail::TimeGate) is told every wall-clock time, so
// nothing here depends on the machine.
#include "test_harness.hpp"

#include "time_gate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using detail::TimeGate;
using Wall = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

/// A wall-clock time, `ms` after the moment the examples call 0.
Wall::time_point w(std::int64_t ms)
{
    return Wall::time_point{} + std::chrono::hours(1) + Ms(ms);
}

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

std::int64_t ms(std::chrono::nanoseconds d)
{
    CHECK_EQ(d.count() % 1'000'000, 0); // the examples use whole milliseconds
    return d.count() / 1'000'000;
}

/// A simulation whose participants have the controller's step: the controller's step k begins
/// when the gate has let step k - 1 go and the participants have finished their steps at k - 1,
/// which they began when step k - 1 began.
struct Example
{
    TimeGate gate;
    Ms step;
    std::int64_t next = 0;     ///< index of the controller step that begins next
    std::int64_t begun = 0;    ///< when the last one began (ms)
    std::int64_t let_go = 0;   ///< when the gate let it go (ms)

    Example(std::optional<double> speed, Ms tolerance, Ms step_size = 10ms, bool start_paused = false) :
      gate(speed, tolerance, start_paused), step(step_size)
    {
    }

    [[nodiscard]] std::int64_t when_next(std::int64_t work) const
    {
        return next == 0 ? 0 : std::max(begun + work, let_go);
    }

    /// The participants need `work` ms for their steps of the interval before; the gate's thread
    /// wakes `wake` ms after the time it waits for. @returns when the step began (ms).
    std::int64_t run_step(std::int64_t work = 1, std::int64_t wake = 0)
    {
        begun = when_next(work);
        const bool held = gate.begin_step(at(step * next), step, w(begun));
        ++next;
        let_go = begun;
        if (held)
        {
            const auto due = gate.release_at();
            CHECK(due.has_value()); // the examples that pause do not come here
            let_go = std::max(begun, ms(*due - w(0))) + wake;
            gate.released(w(let_go));
        }
        return begun;
    }

    void run_steps(std::int64_t count, std::int64_t work = 1, std::int64_t wake = 0)
    {
        for (std::int64_t i = 0; i < count; ++i)
        {
            run_step(work, wake);
        }
    }

    /// The step at stop_at: the last one.
    std::int64_t run_last_step(std::int64_t work = 1)
    {
        begun = when_next(work);
        gate.begin_last_step(at(step * next), w(begun));
        return begun;
    }
};

struct Numbers
{
    std::uint64_t paced;
    std::uint64_t late;
    std::int64_t max_late_ms;
    std::int64_t slipped_ms;
    std::int64_t running_ms;
};

void check(const TimeState& s, const Numbers& n)
{
    CHECK_EQ(s.paced_steps, n.paced);
    CHECK_EQ(s.late_steps, n.late);
    CHECK_EQ(ms(s.max_lateness), n.max_late_ms);
    CHECK_EQ(ms(s.slipped), n.slipped_ms);
    CHECK_EQ(ms(s.running_for), n.running_ms);
}

} // namespace

TEST_CASE(time_gate_model_a_simulation_that_keeps_up_is_not_late)
{
    // Speed 1, 10 ms steps, to 1000 ms; the participants need 2 ms per step. Every step begins at
    // its deadline; the 100 steps after the first are measured, the one at stop_at too.
    Example e(1.0, 10ms);
    e.run_steps(100, 2);
    CHECK_EQ(e.begun, 990);
    CHECK_EQ(e.run_last_step(2), 1000);
    const TimeState s = e.gate.state(w(5000)); // long after: the numbers stopped at the last step
    check(s, {.paced = 100, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 1000});
    CHECK_EQ(s.now.time_since_epoch().count(), SimDuration(1000ms).count());
    CHECK_EQ(s.current_lateness.count(), 0);

    // Speed 2: a step every 5 ms. Speed 0.5: every 20 ms.
    Example fast(2.0, 10ms);
    fast.run_steps(100, 2);
    CHECK_EQ(fast.run_last_step(2), 500);
    check(fast.gate.state(w(500)), {.paced = 100, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 500});
    Example slow(0.5, 10ms);
    slow.run_steps(100, 2);
    CHECK_EQ(slow.run_last_step(2), 2000);
    check(slow.gate.state(w(2000)), {.paced = 100, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 2000});
}

TEST_CASE(time_gate_model_one_slow_interval_is_one_late_step_and_a_slip)
{
    // The participants' steps at 300 ms take 500 ms. The step at 310 ms begins at 800 ms: 490 ms
    // late. The simulation does not catch up: the steps after it come 10 ms apart, 490 ms later
    // than first planned, and the run takes 1000 + 490 ms.
    Example e(1.0, 10ms);
    e.run_steps(31);
    CHECK_EQ(e.begun, 300);
    CHECK_EQ(e.run_step(500), 800);
    CHECK_EQ(e.run_step(), 810);
    CHECK_EQ(e.run_step(), 820);
    e.run_steps(66);
    CHECK_EQ(e.run_last_step(), 1490);
    check(e.gate.state(w(1490)), {.paced = 100, .late = 1, .max_late_ms = 490, .slipped_ms = 490, .running_ms = 1490});
}

TEST_CASE(time_gate_model_the_interval_before_stop_at_is_measured)
{
    // Only the last interval (990 to 1000 ms) is slow: the step at stop_at, which the gate never
    // holds, begins 490 ms late.
    Example e(1.0, 10ms);
    e.run_steps(100);
    CHECK_EQ(e.begun, 990);
    CHECK_EQ(e.run_last_step(500), 1490);
    check(e.gate.state(w(1490)), {.paced = 100, .late = 1, .max_late_ms = 490, .slipped_ms = 490, .running_ms = 1490});

    // stop_at at the first step after 0: the one step there is, is measured.
    Example one(1.0, 10ms);
    one.run_step();
    CHECK_EQ(one.run_last_step(200), 200);
    check(one.gate.state(w(200)), {.paced = 1, .late = 1, .max_late_ms = 190, .slipped_ms = 190, .running_ms = 200});
}

TEST_CASE(time_gate_model_lateness_within_the_tolerance_is_made_up_for)
{
    // One interval takes 15 ms: the next step is 5 ms late, within the tolerance of 10 ms. The
    // deadlines stay, so the step after it is on time again and the run takes 1000 ms.
    Example e(1.0, 10ms);
    e.run_steps(31);
    CHECK_EQ(e.run_step(15), 315);
    CHECK_EQ(e.run_step(), 320);
    e.run_steps(67);
    CHECK_EQ(e.run_last_step(), 1000);
    check(e.gate.state(w(1000)), {.paced = 100, .late = 0, .max_late_ms = 5, .slipped_ms = 0, .running_ms = 1000});

    // 5 ms steps: an interval of 13 ms makes the next step 8 ms late, more than a step. The steps
    // after it are not held back (their deadlines have passed) until the simulation is on time:
    // 8, 5 and 2 ms late with 2 ms of work each, then at the deadline.
    Example fine(1.0, 10ms, 5ms);
    fine.run_steps(11);
    CHECK_EQ(fine.begun, 50);
    CHECK_EQ(fine.run_step(13), 63);
    CHECK_EQ(fine.run_step(2), 65);
    CHECK_EQ(fine.run_step(2), 67);
    CHECK_EQ(fine.run_step(2), 70);
    CHECK_EQ(fine.run_step(2), 75);
    check(fine.gate.state(w(75)), {.paced = 15, .late = 0, .max_late_ms = 8, .slipped_ms = 0, .running_ms = 75});

    // Exactly the tolerance is not late; one millisecond more is.
    Example edge(1.0, 10ms);
    edge.run_steps(2);
    CHECK_EQ(edge.run_step(20), 30);
    check(edge.gate.state(w(30)), {.paced = 2, .late = 0, .max_late_ms = 10, .slipped_ms = 0, .running_ms = 30});
    Example over(1.0, 10ms);
    over.run_steps(2);
    CHECK_EQ(over.run_step(21), 31);
    check(over.gate.state(w(31)), {.paced = 2, .late = 1, .max_late_ms = 11, .slipped_ms = 11, .running_ms = 31});
}

TEST_CASE(time_gate_model_a_simulation_that_is_always_slow_slips_all_the_time)
{
    // Every interval takes 30 ms instead of 10: each step is 20 ms late and slips. 300 ms of
    // simulation take 900 ms: 300 at the speed asked for, and 600 slipped.
    Example e(1.0, 10ms);
    e.run_step();
    e.run_steps(29, 30);
    CHECK_EQ(e.run_last_step(30), 900);
    check(e.gate.state(w(900)), {.paced = 30, .late = 30, .max_late_ms = 20, .slipped_ms = 600, .running_ms = 900});

    // Every interval takes 15 ms: the lateness grows by 5 ms per step and passes the tolerance at
    // every third (5, 10, 15: slip). 300 ms take 450 ms: 10 late steps, 150 ms slipped.
    Example b(1.0, 10ms);
    b.run_step();
    b.run_steps(29, 15);
    CHECK_EQ(b.run_last_step(15), 450);
    check(b.gate.state(w(450)), {.paced = 30, .late = 10, .max_late_ms = 15, .slipped_ms = 150, .running_ms = 450});
}

TEST_CASE(time_gate_model_a_stall_in_progress_is_counted_while_it_lasts_and_at_the_end)
{
    // On time up to the step at 500 ms, which the gate lets go at 510 ms. The participants never
    // finish their steps at 500 ms, so the step at 510 ms does not begin.
    Example e(1.0, 10ms);
    e.run_steps(51);
    CHECK_EQ(e.begun, 500);
    CHECK_EQ(e.let_go, 510);
    // While the gate holds the step at 500 ms, nothing is overdue.
    TimeState s = e.gate.state(w(505));
    check(s, {.paced = 50, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 500});
    CHECK_EQ(s.current_lateness.count(), 0);
    // 5 ms overdue: counted, within the tolerance.
    s = e.gate.state(w(515));
    check(s, {.paced = 51, .late = 0, .max_late_ms = 5, .slipped_ms = 0, .running_ms = 515});
    CHECK_EQ(ms(s.current_lateness), 5);
    // At 3000 ms the step at 510 ms is 2490 ms overdue: one late step so far.
    s = e.gate.state(w(3000));
    check(s, {.paced = 51, .late = 1, .max_late_ms = 2490, .slipped_ms = 2490, .running_ms = 3000});
    CHECK_EQ(ms(s.current_lateness), 2490);
    CHECK_EQ(s.now.time_since_epoch().count(), SimDuration(500ms).count());
    // Looking does not count anything twice.
    check(e.gate.state(w(3000)), {.paced = 51, .late = 1, .max_late_ms = 2490, .slipped_ms = 2490, .running_ms = 3000});

    // The run is ended at 5000 ms (a timeout, a failed participant, a stop): 4490 ms late, and
    // 500 ms of simulation in 5000 ms. The numbers stay.
    Example ended = e;
    ended.gate.end(w(5000));
    s = ended.gate.state(w(9000));
    check(s, {.paced = 51, .late = 1, .max_late_ms = 4490, .slipped_ms = 4490, .running_ms = 5000});
    CHECK_EQ(ms(s.current_lateness), 4490);
    ended.gate.end(w(7000)); // only the first end counts
    CHECK(!ended.gate.begin_step(at(510ms), 10ms, w(7000))); // a step that comes after the end is not held, not counted
    ended.gate.set_speed(std::nullopt, w(7000));
    s = ended.gate.state(w(9000));
    check(s, {.paced = 51, .late = 1, .max_late_ms = 4490, .slipped_ms = 4490, .running_ms = 5000});
    CHECK_EQ(s.now.time_since_epoch().count(), SimDuration(500ms).count());

    // Or the stall ends at 3000 ms: the step that was overdue begins, as the one late step it was.
    Example went_on = e;
    CHECK_EQ(went_on.run_step(2500), 3000);
    check(went_on.gate.state(w(3000)), {.paced = 51, .late = 1, .max_late_ms = 2490, .slipped_ms = 2490, .running_ms = 3000});
    CHECK_EQ(went_on.run_step(), 3010);
    check(went_on.gate.state(w(3010)), {.paced = 52, .late = 1, .max_late_ms = 2490, .slipped_ms = 2490, .running_ms = 3010});
}

TEST_CASE(time_gate_model_a_run_ended_while_the_gate_holds_a_step_is_not_late)
{
    // Stopped at 505 ms, while the gate holds the step at 500 ms: nothing is overdue, and the
    // time runs to the beginning of that step.
    Example e(1.0, 10ms);
    e.run_steps(50);
    CHECK(e.gate.begin_step(at(500ms), 10ms, w(500)));
    e.gate.end(w(505));
    const TimeState s = e.gate.state(w(800));
    check(s, {.paced = 50, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 500});
    CHECK_EQ(s.current_lateness.count(), 0);
}

TEST_CASE(time_gate_model_time_stood_still_is_not_late_and_not_running_time)
{
    // Starts paused; resumed at 1000 ms. The step at 0 takes its base there.
    Example e(1.0, 10ms, 10ms, true);
    CHECK(e.gate.begin_step(at(0ms), 10ms, w(0)));
    CHECK(e.gate.paused());
    CHECK(!e.gate.release_at().has_value());
    CHECK_EQ(ms(e.gate.state(w(400)).paused_for), 400);
    e.gate.resume(w(1000));
    CHECK(e.gate.release_at() == std::optional<Wall::time_point>(w(1010)));
    e.gate.released(w(1010));
    CHECK(e.gate.begin_step(at(10ms), 10ms, w(1010)));
    // pause() while the step at 10 ms is held for pacing, at 1015 ms; two seconds; a step of
    // 10 ms: it waits a whole step again, then stands still at 20 ms.
    e.gate.pause(w(1015));
    CHECK(e.gate.paused());
    e.gate.step(10ms, w(3015));
    CHECK(!e.gate.paused());
    CHECK(e.gate.release_at() == std::optional<Wall::time_point>(w(3025)));
    e.gate.released(w(3025));
    CHECK(e.gate.begin_step(at(20ms), 10ms, w(3025)));
    CHECK(e.gate.paused());
    TimeState s = e.gate.state(w(3525));
    // 20 ms of simulation in 25 ms of running time (the step that was paused began anew).
    check(s, {.paced = 2, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 25});
    CHECK_EQ(ms(s.paused_for), 1000 + 2000 + 500);
    CHECK_EQ(s.current_lateness.count(), 0);
    // Ended while it stands still: the pause counts up to the end.
    e.gate.end(w(4025));
    s = e.gate.state(w(9000));
    check(s, {.paced = 2, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 25});
    CHECK_EQ(ms(s.paused_for), 1000 + 2000 + 1000);
}

TEST_CASE(time_gate_model_a_pause_asked_for_during_a_stall_keeps_the_lateness)
{
    // pause() at 200 ms while the participants are stuck in their steps at 100 ms: the simulation
    // stands still only when the step at 110 ms begins, at 400 ms, and that step is 290 ms late.
    Example e(1.0, 10ms);
    e.run_steps(11);
    e.gate.pause(w(200));
    CHECK(!e.gate.paused());
    CHECK(e.gate.begin_step(at(110ms), 10ms, w(400)));
    CHECK(e.gate.paused());
    check(e.gate.state(w(600)), {.paced = 11, .late = 1, .max_late_ms = 290, .slipped_ms = 290, .running_ms = 400});
}

TEST_CASE(time_gate_model_a_change_of_speed_keeps_the_lateness_of_a_stall)
{
    // The participants are stuck in their steps at 100 ms (let go at 110 ms). At 300 ms the speed
    // becomes 2. The step at 110 ms begins at 400 ms: 290 ms late, not 100.
    Example e(1.0, 10ms);
    e.run_steps(11);
    CHECK_EQ(e.let_go, 110);
    Example faster = e;
    faster.gate.set_speed(2.0, w(300));
    CHECK_EQ(ms(faster.gate.state(w(300)).current_lateness), 190);
    CHECK_EQ(faster.run_step(300), 400);
    check(faster.gate.state(w(400)), {.paced = 11, .late = 1, .max_late_ms = 290, .slipped_ms = 290, .running_ms = 400});
    // From there at speed 2: a step every 5 ms.
    CHECK_EQ(faster.run_step(), 405);
    CHECK_EQ(faster.run_step(), 410);

    // To as fast as possible at 300 ms: nothing is measured afterwards, so the 190 ms the step is
    // late by then are counted then.
    Example max = e;
    max.gate.set_speed(std::nullopt, w(300));
    check(max.gate.state(w(300)), {.paced = 11, .late = 1, .max_late_ms = 190, .slipped_ms = 190, .running_ms = 300});
    CHECK_EQ(max.run_step(300), 400);
    CHECK_EQ(max.run_step(3), 403);
    check(max.gate.state(w(403)), {.paced = 11, .late = 1, .max_late_ms = 190, .slipped_ms = 190, .running_ms = 403});

    // A step held for pacing keeps the part of its wait that has passed: the step at 100 ms,
    // held from 100 to 110 ms, gets speed 0.5 at 104 ms: 6 ms of simulation are left, 12 ms.
    Example held(1.0, 10ms);
    held.run_steps(10);
    CHECK(held.gate.begin_step(at(100ms), 10ms, w(100)));
    held.gate.set_speed(0.5, w(104));
    CHECK(held.gate.release_at() == std::optional<Wall::time_point>(w(116)));
    held.gate.set_speed(0.5, w(110)); // the same speed again: nothing changes
    CHECK(held.gate.release_at() == std::optional<Wall::time_point>(w(116)));
    held.gate.released(w(116));
    CHECK(held.gate.begin_step(at(110ms), 10ms, w(116)));
    check(held.gate.state(w(116)), {.paced = 11, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 116});
}

TEST_CASE(time_gate_model_a_speed_set_before_the_start_takes_its_base_at_the_first_step)
{
    // The speed is set 300 ms before the first step: that time is neither lateness nor caught up.
    Example e(std::nullopt, 10ms);
    e.gate.set_speed(1.0, w(-300));
    e.run_steps(20);
    CHECK_EQ(e.run_last_step(), 200);
    check(e.gate.state(w(200)), {.paced = 20, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 200});

    // Pacing that starts between two steps: the next step is due then.
    Example later(std::nullopt, 10ms);
    later.run_steps(5, 3);
    CHECK_EQ(later.begun, 12);
    later.gate.set_speed(1.0, w(14));
    CHECK_EQ(later.run_step(3), 15);
    CHECK_EQ(later.run_step(3), 24);
    CHECK_EQ(later.run_step(3), 34);
    check(later.gate.state(w(34)), {.paced = 3, .late = 0, .max_late_ms = 1, .slipped_ms = 0, .running_ms = 34});
}

TEST_CASE(time_gate_model_the_gates_own_late_wake_up_is_lateness)
{
    // The gate's thread wakes 3 ms after every deadline: every step begins 3 ms late (it does
    // not add up: the deadlines stay), and the gate's share is reported apart.
    Example e(1.0, 10ms);
    e.run_steps(100, 1, 3);
    CHECK_EQ(e.run_last_step(), 1003);
    TimeState s = e.gate.state(w(1003));
    check(s, {.paced = 100, .late = 0, .max_late_ms = 3, .slipped_ms = 0, .running_ms = 1003});
    CHECK_EQ(ms(s.max_gate_lateness), 3);

    // A timer that wakes 15 ms late with a tolerance of 10 ms: every step is late and slips,
    // and 1000 ms of simulation take 2500 ms. (This is why the gate waits with a precise timer.)
    Example coarse(1.0, 10ms);
    coarse.run_steps(100, 1, 15);
    CHECK_EQ(coarse.run_last_step(), 2500);
    s = coarse.gate.state(w(2500));
    check(s, {.paced = 100, .late = 100, .max_late_ms = 15, .slipped_ms = 1500, .running_ms = 2500});
    CHECK_EQ(ms(s.max_gate_lateness), 15);
}

TEST_CASE(time_gate_model_as_fast_as_possible_nothing_is_paced)
{
    Example e(std::nullopt, 10ms);
    e.run_steps(100, 3);
    CHECK_EQ(e.begun, 297);
    // The running time goes on while the simulation works on the next step.
    check(e.gate.state(w(299)), {.paced = 0, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 299});
    CHECK_EQ(e.run_last_step(3), 300);
    const TimeState s = e.gate.state(w(900));
    check(s, {.paced = 0, .late = 0, .max_late_ms = 0, .slipped_ms = 0, .running_ms = 300});
    CHECK_EQ(s.max_gate_lateness.count(), 0);
}
