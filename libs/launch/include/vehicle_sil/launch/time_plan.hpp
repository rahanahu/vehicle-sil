// SPDX-License-Identifier: MIT
//
// The step of the launcher's controller for a run, and what it means for the ECUs' steps
// (plan_time_step(), coarse_time_step()), with the timing numbers a run reports (RunTiming). Pure
// functions of the configuration and what the check found: no state, no thread, no lock.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"

#include <cstdint>
#include <optional>

namespace vehicle_sil::launch {

/// How the simulation's time went (the result's "timing"). The pacing numbers are the time gate's
/// (TimeControlOptions in vehicle_sil/core/simulation_controller.hpp has the definitions): they
/// run from the first step of the launcher's controller to the last one, and include a step that
/// had not begun when the run was ended although its real time had come.
struct RunTiming
{
    /// The speed at the end; empty: as fast as possible.
    std::optional<double> speed{};
    /// The step of the launcher's controller (detail::plan_time_step()), and whether the file's
    /// time_step_ms gave it.
    std::int64_t time_step_ms = 0;
    bool time_step_written = false;
    /// The smallest step of the ECUs in nanoseconds; empty if a participant's step is not known
    /// (describe: false), or none has one.
    std::optional<std::int64_t> ecu_step_ns{};
    /// How long after the real time its virtual time stands for an ECU step may run although
    /// nothing is late, because the controller's step is coarser than the ECUs' (nanoseconds; 0:
    /// every ECU step starts at a controller step); empty if a participant's step is not known.
    std::optional<std::int64_t> step_delay_ns{};
    /// The time_step_ms with which step_delay_ns is 0.
    std::int64_t fine_step_ms = 0;
    /// The check found coarse-time-step already (the file has speed and time_step_ms, and the run
    /// keeps that speed): assess_run() does not report it again.
    bool coarse_step_checked = false;
    std::int64_t late_tolerance_ms = 0;
    /// The virtual time the simulation reached (the controller's last step).
    std::int64_t sim_ms = 0;
    /// From the beginning of the controller's first step to the beginning of its last one (to the
    /// end of the run if the next step was overdue then, or the speed was max): without the
    /// start-up, the shutdown and the time until a failure was noticed after the simulation ended.
    std::int64_t wall_ms = 0;
    /// Of wall_ms, the time it stood still (paused).
    std::int64_t paused_ms = 0;
    /// The controller steps that were measured against real time (a speed was set), and those
    /// that began more than the tolerance late.
    std::uint64_t paced_steps = 0;
    std::uint64_t late_steps = 0;
    /// The largest lateness of a paced step, in milliseconds (rounded up).
    std::int64_t max_late_ms = 0;
    /// The sum of the lateness of the late steps (rounded up): the simulation does not catch up
    /// after a late step, so this is how far behind the speed asked for it came in all.
    std::int64_t slipped_ms = 0;
    /// How late the step was that had not begun when the run ended (rounded up); 0 if none was
    /// overdue.
    std::int64_t late_ms = 0;

    /// sim_ms / (wall_ms - paused_ms) to three decimals; empty if it did not run for a
    /// millisecond.
    [[nodiscard]] std::optional<double> actual_speed() const;
};

namespace detail {
/// The step of the launcher's controller for a run at `speed` (empty: as fast as possible; `fine`:
/// as with a speed, whatever the speed is: a run with a scenario, whose results must not depend on
/// the speed), and
/// what it means for the ECUs' steps: RunTiming's time_step_ms, time_step_written, ecu_step_ns,
/// step_delay_ns and fine_step_ms (the rest is left as it is in the result).
/// - time_step_ms as written, if it is.
/// - Without a speed: launch::time_step_ms() (kDefaultTimeStepMs or what of it divides stop_at_ms).
/// - With a speed: the greatest common divisor of kDefaultTimeStepMs, stop_at_ms and the steps of
///   the enabled ECUs as `check` has them, so that every ECU step starts at a controller step (the
///   gate keeps real time only there); 1 ms if a participant's step is not known (describe: false)
///   or the divisor is no whole number of milliseconds.
[[nodiscard]] RunTiming plan_time_step(const SimulationConfig& config, const CheckReport& check,
                                       const std::optional<double>& speed, bool fine = false);

/// The warning coarse-time-step for `timing` (plan_time_step() with speed set as the run has it),
/// if it applies: a speed counts (timing.speed, or steps were paced) and an ECU step starts
/// between two controller steps. assess_run() reports it after a run; check_file() before one, if
/// the file has speed and time_step_ms.
[[nodiscard]] std::optional<Finding> coarse_time_step(const RunTiming& timing);
} // namespace detail

} // namespace vehicle_sil::launch
