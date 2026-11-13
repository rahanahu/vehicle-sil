// SPDX-License-Identifier: MIT
#include "time_gate.hpp"

#include <algorithm>
#include <utility>

namespace vehicle_sil::detail {
namespace {

using Nanos = std::chrono::nanoseconds;

Nanos nanos(TimeGate::Wall::duration d)
{
    return std::chrono::duration_cast<Nanos>(d);
}

} // namespace

TimeGate::TimeGate(std::optional<double> speed, Wall::duration late_tolerance, bool start_paused) :
  late_tolerance_(late_tolerance)
{
    state_.speed = speed;
    if (start_paused)
    {
        state_.pause_target = SimTime{};
    }
}

TimeGate::Wall::time_point TimeGate::wall_of(SimTime t) const
{
    const auto simElapsed = std::chrono::duration<double, std::nano>(t - base_->sim);
    return base_->wall + std::chrono::duration_cast<Wall::duration>(simElapsed / *state_.speed);
}

void TimeGate::count(TimeState& s, Wall::duration lateness) const
{
    ++s.paced_steps;
    s.max_lateness = std::max(s.max_lateness, nanos(lateness));
    if (lateness > late_tolerance_)
    {
        ++s.late_steps;
        s.slipped += nanos(lateness);
    }
}

bool TimeGate::begin_step(SimTime now, SimDuration step, Wall::time_point wall)
{
    if (ended_)
    {
        return false;
    }
    state_.now = now;
    held_step_ = step;
    stepped_ = true;
    if (!first_)
    {
        first_ = wall;
    }
    // No step begins while the simulation stands still: every pause so far is over.
    ran_ = wall - *first_ - state_.paused_for;
    if (state_.speed && base_)
    {
        const auto lateness = std::max(Wall::duration::zero(), wall - wall_of(now));
        count(state_, lateness);
        if (lateness > late_tolerance_)
        {
            base_ = Base{now, wall}; // slip: no catching up
        }
    }
    else if (unbegun_lateness_)
    {
        count(state_, *unbegun_lateness_);
    }
    unbegun_lateness_.reset();
    // The guard holds the step whatever else does: it is let go only when the guard has moved on.
    guarded_ = guard_ && now >= *guard_;
    guard_overran_ = false;
    if (std::exchange(pause_next_, false) || (state_.pause_target && now >= *state_.pause_target))
    {
        set_paused(true, wall);
        state_.pause_target.reset();
        held_ = true;
        return true;
    }
    if (state_.speed)
    {
        if (!base_)
        {
            base_ = Base{now, wall};
        }
        deadline_ = wall_of(now + step);
        if (deadline_ > wall)
        {
            held_ = true;
            return true;
        }
    }
    held_ = guarded_;
    return held_;
}

void TimeGate::begin_last_step(SimTime now, Wall::time_point wall)
{
    if (ended_)
    {
        return;
    }
    static_cast<void>(begin_step(now, held_step_, wall));
    guarded_ = false;
    // Nothing comes after it: no step is in progress, whatever state() would make of the hold.
    if (state_.paused)
    {
        set_paused(false, wall);
    }
    held_ = false;
    state_.running_for = nanos(ran_);
    state_.current_lateness = Nanos::zero();
    ended_ = true;
}

void TimeGate::end(Wall::time_point wall)
{
    if (ended_)
    {
        return;
    }
    state_ = state(wall);
    ended_ = true;
}

std::optional<TimeGate::Wall::time_point> TimeGate::release_at() const
{
    if (!held_ || state_.paused || guarded_)
    {
        return std::nullopt;
    }
    return state_.speed ? deadline_ : Wall::time_point::min();
}

void TimeGate::released(Wall::time_point wall)
{
    if (held_ && !state_.paused && state_.speed && base_ && !ended_ && !guard_overran_)
    {
        state_.max_gate_lateness = std::max(state_.max_gate_lateness, nanos(wall - deadline_));
    }
    held_ = false;
}

void TimeGate::guard_from(std::optional<SimTime> t, Wall::time_point wall)
{
    guard_ = t;
    if (guarded_ && (!guard_ || state_.now < *guard_))
    {
        guarded_ = false;
        // What the guard's owner took beyond the step's deadline makes the next step late; it is
        // not the gate's own late wake-up.
        guard_overran_ = state_.speed && base_ && wall > deadline_;
    }
    else if (!guarded_ && held_ && guard_ && state_.now >= *guard_)
    {
        guarded_ = true; // the step that is held for another reason stays for the guard as well
    }
}

void TimeGate::set_paused(bool paused, Wall::time_point wall)
{
    if (paused == state_.paused)
    {
        return;
    }
    if (paused)
    {
        paused_since_ = wall;
    }
    else if (!ended_)
    {
        state_.paused_for += nanos(wall - paused_since_);
    }
    state_.paused = paused;
}

void TimeGate::rebase(Wall::time_point wall)
{
    base_.reset();
    if (held_ && state_.speed)
    {
        base_ = Base{state_.now, wall};
        deadline_ = wall_of(state_.now + held_step_);
    }
}

void TimeGate::pause_now_or_next(Wall::time_point wall)
{
    if (held_)
    {
        set_paused(true, wall);
        state_.pause_target.reset();
    }
    else
    {
        pause_next_ = true;
    }
}

void TimeGate::pause(Wall::time_point wall)
{
    if (!state_.paused)
    {
        pause_now_or_next(wall);
    }
}

void TimeGate::pause_at(SimTime t, Wall::time_point wall)
{
    pause_next_ = false;
    state_.pause_target = t;
    if (!state_.paused && held_ && state_.now >= t)
    {
        pause_now_or_next(wall);
    }
}

void TimeGate::resume(Wall::time_point wall)
{
    pause_next_ = false;
    state_.pause_target.reset();
    if (state_.paused)
    {
        set_paused(false, wall);
        rebase(wall);
    }
}

void TimeGate::run_until(SimTime t, Wall::time_point wall)
{
    if (state_.paused && state_.now >= t)
    {
        return; // already there
    }
    pause_next_ = false;
    state_.pause_target = t;
    if (state_.paused)
    {
        set_paused(false, wall);
        rebase(wall);
    }
    else if (held_ && state_.now >= t)
    {
        pause_now_or_next(wall);
    }
}

void TimeGate::step(SimDuration d, Wall::time_point wall)
{
    // Saturates: a duration from outside (the Control API) may reach past the end of time.
    run_until(d >= SimTime::max() - state_.now ? SimTime::max() : state_.now + d, wall);
}

void TimeGate::set_speed(std::optional<double> speed, Wall::time_point wall)
{
    if (speed == state_.speed)
    {
        return;
    }
    const bool paced = state_.speed && base_ && !state_.paused && !ended_;
    if (paced && held_ && speed)
    {
        // The held step [now, now + held_step) keeps the part of its wait that has passed and
        // waits for the rest at the new speed (a deadline that has passed stays where it was).
        const auto from = std::min(wall, deadline_);
        const auto remainingSim = std::chrono::duration<double, std::nano>(deadline_ - from) * *state_.speed;
        state_.speed = speed;
        base_ = Base{state_.now + held_step_ - std::chrono::duration_cast<SimDuration>(remainingSim), from};
        deadline_ = wall_of(state_.now + held_step_);
        return;
    }
    if (paced && !held_ && stepped_)
    {
        // Between two steps: the next one keeps its deadline, and with it the lateness it has;
        // the new speed counts from there.
        const SimTime next = state_.now + held_step_;
        const auto due = std::min(wall_of(next), wall);
        if (speed)
        {
            state_.speed = speed;
            base_ = Base{next, due};
        }
        else
        {
            // As fast as possible: nothing is measured from here on, so what the next step is
            // late by now is what it is late by when it begins (state() counts it until then).
            if (wall > due)
            {
                unbegun_lateness_ = wall - due;
            }
            state_.speed = speed;
            base_.reset();
        }
        return;
    }
    state_.speed = speed;
    if (!held_ && speed)
    {
        // Pacing starts between two steps: the next one is due now. Before the first step there
        // is no base: the first step takes it (else the time until the start would be lateness).
        if (stepped_)
        {
            // A step late from before as fast as possible stays as late as it was then, and is
            // counted once, when it begins.
            base_ = Base{state_.now + held_step_, wall - unbegun_lateness_.value_or(Wall::duration::zero())};
            unbegun_lateness_.reset();
        }
        else
        {
            base_.reset();
        }
        return;
    }
    rebase(wall);
}

TimeState TimeGate::state(Wall::time_point wall) const
{
    TimeState out = state_;
    if (ended_)
    {
        return out;
    }
    if (state_.paused)
    {
        out.paused_for += nanos(wall - paused_since_);
    }
    out.running_for = nanos(ran_);
    out.current_lateness = Nanos::zero();
    // While the guard holds the step, the next one is not overdue: its owner decides when the
    // simulation goes on (or that it ends here). What the owner takes beyond the deadline is
    // counted when the next step begins.
    if (!stepped_ || state_.paused || guarded_)
    {
        return out;
    }
    // The step in progress: the next one has not begun.
    bool overdue = false;
    if (state_.speed && base_)
    {
        const auto lateness = wall - wall_of(state_.now + held_step_);
        if (lateness > Wall::duration::zero())
        {
            overdue = true;
            out.current_lateness = nanos(lateness);
            count(out, lateness);
        }
    }
    else if (!state_.speed)
    {
        overdue = !held_; // as fast as possible, the next step is due at once
        if (unbegun_lateness_)
        {
            count(out, *unbegun_lateness_);
        }
    }
    if (overdue)
    {
        out.running_for = nanos(wall - *first_) - out.paused_for;
    }
    return out;
}

} // namespace vehicle_sil::detail
