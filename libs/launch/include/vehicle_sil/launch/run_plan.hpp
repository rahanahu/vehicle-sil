// SPDX-License-Identifier: MIT
//
// What is settled about a run before it begins (RunPlan), made from the check that passed and
// what the caller asked for (plan_run()), and the outcome a run begins with (planned_outcome()).
// Values and pure functions: no state, no thread, no lock, nothing that the run calls back.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/launch/scenario_check.hpp"
#include "vehicle_sil/launch/time_plan.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

/// What the plan knows of one participant of the run beside its entry in the configuration: what
/// the run itself adds to the file. The configuration (ParticipantConfig) is what the file says.
struct RunMember
{
    /// Set for a bridge that the run starts for --connect (detail::with_bridges() added its
    /// participant): the network it connects, the target as given, its tool and its arguments
    /// (the entry of CheckReport::outside). Such a participant is started before the others, is
    /// waited for, and stops the run when it ends. Empty for a participant of the file, also one
    /// whose role in the file is bridge.
    std::optional<NetworkOutside> outside{};
};

/// One run as it is settled before anything starts: a value, made by plan_run() and never written
/// while the run lasts. What the run calls back (the lines, the events) and what it borrows (its
/// controls, a control server) are not in it.
struct RunPlan
{
    /// The check that passed, with one participant at the end for each bridge of --connect
    /// (detail::with_bridges()): the participants of the run, in the order the run keeps
    /// everywhere (config->participants[i], participants[i], RunOutcome::participants[i]).
    CheckReport check;
    /// run --scenario: the scenario that the check read, as Checked::scenario had it; empty
    /// without one. scenario() is the scenario itself.
    std::optional<ScenarioCheck> scenario_check;
    /// One for each participant of `check`, in the same order (members[i] is of
    /// config().participants[i]): which of them are bridges of --connect, and what each connects.
    /// What changes while a bridge runs (its link, its counts) is kept per participant by the
    /// run's output, under its lock; this says only where to look.
    std::vector<RunMember> members;
    /// The directory of the simulation file: what the participants' relative paths start from.
    std::filesystem::path base;
    /// The step of the launcher's controller, the speed the run starts at (empty: as fast as
    /// possible) and the late tolerance; the numbers a run measures are 0. The run reads its
    /// settings here; the speed at its end and what it measured are in RunOutcome::timing.
    RunTiming timing;
    /// --strict (RunIntent::strict): a bridge that shares its device stops the run.
    bool strict = false;
    /// run --start-paused with a control server: the run holds before the first step.
    bool holds_before_start = false;
    /// RunHost::log_dir and timeline: the path of the timeline's file, and its name as given.
    std::filesystem::path log_dir;
    std::filesystem::path timeline;
    std::string timeline_shown;
    /// RunLimits::poll, outside_lost_after and control_hello_timeout and RunHost::timeout, as given.
    std::chrono::milliseconds poll{};
    std::chrono::milliseconds outside_lost_after{};
    std::optional<std::chrono::milliseconds> timeout;
    std::chrono::milliseconds control_hello_timeout{};

    /// The configuration of `check` (a check that passed has one).
    [[nodiscard]] const SimulationConfig& config() const { return *check.config; }
    /// run --scenario: the scenario the check read; null without one. Points into `scenario_check`.
    [[nodiscard]] const Scenario* scenario() const
    {
        return scenario_check && scenario_check->scenario ? &*scenario_check->scenario : nullptr;
    }
};

/// The plan of the run that `request` asks for. `check`: the check of the file, which passed (as
/// check_file() returned it: the bridges of --connect are added here; its scenario goes to
/// RunPlan::scenario_check); `base`: the directory of
/// the file; `control_server`: the run has one (RunHost::control, or a lent RunLent::control_endpoint). Throws
/// std::invalid_argument for a check without a configuration (CheckReport::config is empty: the
/// file could not be read, so there is nothing to run).
/// The plan has copies of what it reads of `request` and `limits`: neither is kept.
/// - The speed is planned_speed(): 1 with the time lock, else RunIntent::speed, else the file's.
/// - With a scenario the step does not follow the speed: the same steps at every speed.
/// - The bridges are participants at the end of the plan's check, marked in RunPlan::members, and
///   do not count for the step: the step is planned from the participants of the file. One whose
///   role in the file is bridge is a participant of the file, and counts as any other.
[[nodiscard]] RunPlan plan_run(const Checked& check, std::filesystem::path base, const RunRequest& request,
                               const RunLimits& limits = {}, bool control_server = false);

/// The outcome a run of `plan` begins with: what the plan already says of the result (the limits,
/// the timing, the participants in the plan's order, the scenario as not run, the networks
/// connected outside), and nothing of what the run finds.
[[nodiscard]] RunOutcome planned_outcome(const RunPlan& plan);

} // namespace vehicle_sil::launch
