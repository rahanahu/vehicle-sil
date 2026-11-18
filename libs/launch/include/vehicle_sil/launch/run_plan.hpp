// SPDX-License-Identifier: MIT
//
// What is settled about a run before it begins (RunPlan), made from the check that passed and
// what the caller asked for (plan_run()), and the outcome a run begins with (planned_outcome()).
// Values and pure functions: no state, no thread, no lock, nothing that the run calls back.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/launch/time_plan.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace vehicle_sil::launch {

/// One run as it is settled before anything starts: a value, made by plan_run() and never written
/// while the run lasts. What the run calls back (the lines, the events) and what it borrows (its
/// controls, a control server) are not in it.
struct RunPlan
{
    /// The check that passed, with one participant at the end for each bridge of --connect
    /// (detail::with_bridges()): the participants of the run, in the order the run keeps
    /// everywhere (config->participants[i], participants[i], RunOutcome::participants[i]).
    CheckReport check;
    /// The directory of the simulation file: what the participants' relative paths start from.
    std::filesystem::path base;
    /// The step of the launcher's controller, the speed the run starts at (empty: as fast as
    /// possible) and the late tolerance; the numbers a run measures are 0.
    RunTiming timing;
    /// --strict (CheckOptions::strict): a bridge that shares its device stops the run.
    bool strict = false;
    /// run --start-paused with a control server: the run holds before the first step.
    bool holds_before_start = false;
    /// RunOptions::log_dir, timeline and timeline_shown, as given.
    std::filesystem::path log_dir;
    std::filesystem::path timeline;
    std::string timeline_shown;
    /// RunOptions::poll, outside_lost_after, timeout and control_hello_timeout, as given.
    std::chrono::milliseconds poll{};
    std::chrono::milliseconds outside_lost_after{};
    std::optional<std::chrono::milliseconds> timeout;
    std::chrono::milliseconds control_hello_timeout{};

    /// The configuration of `check` (a check that passed has one).
    [[nodiscard]] const SimulationConfig& config() const { return *check.config; }
    /// run --scenario: the scenario the check read; null without one. Points into `check`.
    [[nodiscard]] const Scenario* scenario() const
    {
        return check.scenario && check.scenario->scenario ? &*check.scenario->scenario : nullptr;
    }
};

/// The plan of the run that `options` asks for. `check`: the check of the file, which passed (as
/// check_file() returned it: the bridges of --connect are added here); `base`: the directory of
/// the file. Reads the values of `options` only (of its pointers, whether there is a control
/// server), and calls nothing of it.
/// - The speed is planned_speed(): 1 with the time lock, else RunOptions::speed, else the file's.
/// - With a scenario the step does not follow the speed: the same steps at every speed.
/// - The bridges are participants at the end of the plan's check and do not count for the step.
[[nodiscard]] RunPlan plan_run(const CheckReport& check, std::filesystem::path base, const RunOptions& options);

/// The outcome a run of `plan` begins with: what the plan already says of the result (the limits,
/// the timing, the participants in the plan's order, the scenario as not run, the networks
/// connected outside), and nothing of what the run finds.
[[nodiscard]] RunOutcome planned_outcome(const RunPlan& plan);

} // namespace vehicle_sil::launch
