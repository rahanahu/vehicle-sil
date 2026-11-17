// SPDX-License-Identifier: MIT
//
// The step of the launcher's controller for a run, and what it means for the ECUs' steps
// (plan_time_step(), coarse_time_step()), with the timing numbers a run reports (RunTiming); and
// whether a run that is connected outside the simulation keeps real time (TimePolicy, TimeLock,
// time_lock(), planned_speed(), time_lock_findings()). Pure functions of the configuration, the
// request and what the check found: no state, no thread, no lock.
#pragma once

#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/connect.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

struct CheckReport; // check.hpp, which includes this header

/// --time-policy: what a run does about time when it is connected outside the simulation.
enum class TimePolicy
{
    /// It goes at speed 1, and nothing stops the time, moves it or changes the speed.
    Realtime,
    /// The time is the user's to change, as in a run that is not connected outside.
    Free,
};
[[nodiscard]] const char* to_string(TimePolicy policy);
/// "realtime" or "free"; empty for anything else.
[[nodiscard]] std::optional<TimePolicy> time_policy_named(std::string_view name) noexcept;

/// Whether a run keeps real time, and what connects it outside the simulation (time_lock()).
struct TimeLock
{
    /// The run goes at speed 1 and its time is not changed: the policy is TimePolicy::Realtime and
    /// `connected` or `bridges` has a name.
    bool locked = false;
    /// The networks that --connect connects (a target other than kNoTarget), in the order given.
    std::vector<std::string> connected{};
    /// The enabled participants with role: bridge, in the file's order.
    std::vector<std::string> bridges{};

    /// `connected`, then `bridges`: what locks the time, or would with TimePolicy::Realtime
    /// (check --json: time_lock.because).
    [[nodiscard]] std::vector<std::string> because() const;

    friend bool operator==(const TimeLock&, const TimeLock&) = default;
};

/// The time lock of a check or a run of `config` (after --variant, --disable and --enable) with
/// the --connect requests `connect` and `policy`. What connects a run outside:
/// - a --connect request whose target is not kNoTarget (whether or not the rest of the check
///   accepts it);
/// - an enabled participant with role: bridge, also one with describe: false.
/// A disabled participant does not count, and neither does a real-time peer (an ecu or a test
/// participant without a step): that is the user's own software, which can be written to stand
/// still. Needs no --describe output: the file and the request decide.
[[nodiscard]] TimeLock time_lock(const SimulationConfig& config, std::span<const ConnectRequest> connect, TimePolicy policy);

/// The speed a run starts at: 1 with `lock.locked`; else `asked` (--speed; the inner empty: max)
/// if it is set, else `otherwise` (the file's speed; empty: as fast as possible).
[[nodiscard]] std::optional<double> planned_speed(const TimeLock& lock, const std::optional<std::optional<double>>& asked,
                                                  const std::optional<double>& otherwise);

/// What the check says about the time of a run that is connected outside (none if `lock` names
/// nothing). `asked`, `otherwise`: as for planned_speed(); `start_paused`: run --start-paused.
/// - With `lock.locked`: the error outside-real-time for an `asked` speed other than 1, and one
///   for `start_paused`; the note outside-speed-set if no speed is asked for and `otherwise` is
///   not 1 (the run goes at 1 instead).
/// - Not locked (TimePolicy::Free): the note outside-not-real-time if the run's speed is not 1,
///   and one for `start_paused`.
[[nodiscard]] std::vector<Finding> time_lock_findings(const TimeLock& lock, const std::optional<std::optional<double>>& asked,
                                                      const std::optional<double>& otherwise, bool start_paused);

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
