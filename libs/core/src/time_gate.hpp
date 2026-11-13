// SPDX-License-Identifier: MIT
//
// The state and the arithmetic of SimulationController's time gate, without a thread, a lock,
// SIL Kit or a clock: every function is told the wall-clock time, so the pacing numbers can be
// tested with exact times. Internal to vehicle_sil::core; the definitions are at
// TimeControlOptions (simulation_controller.hpp).
#pragma once

#include "vehicle_sil/core/simulation_controller.hpp"

#include <chrono>
#include <optional>

namespace vehicle_sil::detail {

/// Not thread safe: SimulationController calls it under the gate's mutex.
class TimeGate
{
public:
    using Wall = std::chrono::steady_clock;

    TimeGate(std::optional<double> speed, Wall::duration late_tolerance, bool start_paused);

    /// The controller's step [now, now + step) began at `wall`. @returns whether to hold it back
    /// (it stands still, or its deadline lies ahead); released() ends the hold. After end(): false.
    bool begin_step(SimTime now, SimDuration step, Wall::time_point wall);
    /// The controller's step at `now` began at `wall` and is the last one (stop_at): it is
    /// measured like any other, and the numbers stop there (end()).
    void begin_last_step(SimTime now, Wall::time_point wall);
    /// The run is over at `wall`: the numbers stay as state(wall) gives them. Only the first call
    /// counts.
    void end(Wall::time_point wall);

    /// When the held step may complete: empty if none is held or the simulation stands still;
    /// else a time (one that has passed: at once).
    [[nodiscard]] std::optional<Wall::time_point> release_at() const;
    /// The held step was let go at `wall`.
    void released(Wall::time_point wall);

    // SimulationController's functions of the same names, at `wall`.
    void pause(Wall::time_point wall);
    void pause_at(SimTime t, Wall::time_point wall);
    void resume(Wall::time_point wall);
    void run_until(SimTime t, Wall::time_point wall);
    void step(SimDuration d, Wall::time_point wall); ///< d > 0; saturates at the end of time
    void set_speed(std::optional<double> speed, Wall::time_point wall);
    /// SimulationController::guard_from() at `wall`.
    void guard_from(std::optional<SimTime> t, Wall::time_point wall);
    /// The step at now() is held for the guard: the time it began at, for wait_guarded().
    [[nodiscard]] bool guarded() const noexcept { return guarded_; }

    /// The state as if the run ended at `wall` (after end(): as it ended).
    [[nodiscard]] TimeState state(Wall::time_point wall) const;
    [[nodiscard]] bool paused() const noexcept { return state_.paused; }
    [[nodiscard]] SimTime now() const noexcept { return state_.now; }

private:
    struct Base
    {
        SimTime sim;
        Wall::time_point wall;
    };

    [[nodiscard]] Wall::time_point wall_of(SimTime t) const;
    void count(Wall::duration lateness);
    void set_paused(bool paused, Wall::time_point wall);
    void rebase(Wall::time_point wall);
    void pause_now_or_next(Wall::time_point wall);

    TimeState state_;              ///< without the step in progress (state() adds it)
    Wall::duration late_tolerance_;
    bool pause_next_ = false;      ///< pause() while no step was held: stand still at the next one
    bool held_ = false;            ///< the step at state_.now is held (paused, paced, or guarded)
    std::optional<SimTime> guard_; ///< guard_from(): steps that start at or after it are held
    bool guarded_ = false;         ///< the step at state_.now is held for the guard
    bool guard_overran_ = false;   ///< the guard kept the held step beyond its deadline (not the gate's lateness)
    bool stepped_ = false;         ///< the controller has begun a step (the simulation runs)
    SimDuration held_step_{};      ///< size of the step at state_.now
    std::optional<Base> base_;     ///< pacing: the wall-clock time of a virtual time
    Wall::time_point deadline_{};  ///< pacing: when the held step may complete (the next one's deadline)
    Wall::time_point paused_since_{};
    std::optional<Wall::time_point> first_; ///< when the first step began
    Wall::duration ran_{};         ///< running time up to the beginning of the step at state_.now
    bool ended_ = false;
};

} // namespace vehicle_sil::detail
