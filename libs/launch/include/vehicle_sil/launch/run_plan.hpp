// SPDX-License-Identifier: MIT
//
// What is settled about a run before it begins (RunPlan), made from the check that passed, what
// the caller chose for the run (RunChoices) and the run's waits (RunLimits) by plan_run(), and the
// outcome a run begins with (planned_outcome()).
// Values and pure functions: no state, no thread, no lock, nothing that the run calls back.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/launch/run_outcome.hpp"
#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/scenario/scenario_check.hpp"
#include "vehicle_sil/config/time_plan.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

/// How long a bridge of --connect may report the link or the bus down before the launcher stops the
/// run (RunLimits::outside_lost_after).
inline constexpr std::chrono::milliseconds kOutsideLostAfter{30000};
/// With --strict: how long a bridge of --connect may share its device with a bridge that is not of
/// this run before the launcher stops the run (OutsideState::shared_for()). A bridge that another
/// run started later and that ends before its ECUs start must not take this run down with it.
inline constexpr std::chrono::milliseconds kOutsideSharedAfter{3000};
/// With --strict: the longest the launcher waits, after the bridges run, for the first link line of
/// each, which says whether the device is shared and comes before the bridge starts its lifecycle.
/// A bridge that has not said by then is taken as one that shares nothing.
inline constexpr std::chrono::milliseconds kOutsideFirstLineWait{2000};

/// The waits of a run that no user sets: only tests make them shorter.
struct RunLimits
{
    /// How often the launcher looks at the participants and the simulation.
    std::chrono::milliseconds poll{20};
    /// How long a bridge of --connect may report the link or the bus down (link=down, bus=bus-off)
    /// before the launcher stops the run as for a bridge that ended (OutsideState::lost()).
    std::chrono::milliseconds outside_lost_after = kOutsideLostAfter;
    /// How long the start waits for the hello of each required ECU that describes variables and
    /// offers them in the launcher's version of the messages (it is there before the wait begins,
    /// by the order in which SIL Kit sets the participants up: the limit is a last line).
    std::chrono::milliseconds control_hello_timeout{5000};
};

/// What the caller chose for one run, as plan_run() takes it: the request's fields that the plan
/// reads (RunIntent and RunHost of request.hpp, which the launcher copies here: run_choices()).
/// The plan, and so the Runner, reads nothing else of the request.
struct RunChoices
{
    /// RunIntent::speed: replaces the file's speed (the inner empty: as fast as possible); the
    /// outer empty: the file's speed counts.
    std::optional<std::optional<double>> speed{};
    /// RunIntent::start_paused: with a control server, the run holds before the first step.
    bool start_paused = false;
    /// RunIntent::strict (--strict): a bridge that shares its device stops the run.
    bool strict = false;
    /// RunHost::log_dir: a file of each participant's output in this directory; empty: none.
    std::filesystem::path log_dir{};
    /// RunHost::timeline: the file of the scenario's timeline as the user gave it (UTF-8); empty: none.
    std::string timeline{};
    /// RunHost::timeout: the longest the simulation may run (wall clock); empty: no limit.
    std::optional<std::chrono::milliseconds> timeout{};
};

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
    /// run --scenario: a copy of the scenario that the check read (plan_run()'s `scenario`); empty
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
    /// --strict (RunChoices::strict): a bridge that shares its device stops the run.
    bool strict = false;
    /// run --start-paused with a control server: the run holds before the first step.
    bool holds_before_start = false;
    /// RunChoices::log_dir and timeline: the path of the timeline's file, and its name as given.
    std::filesystem::path log_dir;
    std::filesystem::path timeline;
    std::string timeline_shown;
    /// RunLimits::poll, outside_lost_after and control_hello_timeout and RunChoices::timeout, as given.
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

/// The plan of a run of the check `check` with what the caller chose (`choices`). `check`: the
/// check of the file, which passed (as check_file() returned it: the bridges of --connect are added
/// here); `scenario`: run --scenario, the scenario the check read (copied to
/// RunPlan::scenario_check), or null; `base`: the directory of the file; `control_server`: the run
/// has one (RunHost::control, or a lent RunLent::control_endpoint). Throws std::invalid_argument for
/// a check without a configuration (CheckReport::config is empty: the file could not be read, so
/// there is nothing to run).
/// The plan has copies of what it reads of `scenario`, `choices` and `limits`: none is kept.
/// - The speed is planned_speed(): 1 with the time lock, else RunChoices::speed, else the file's.
/// - With a scenario the step does not follow the speed: the same steps at every speed.
/// - The bridges are participants at the end of the plan's check, marked in RunPlan::members, and
///   do not count for the step: the step is planned from the participants of the file. One whose
///   role in the file is bridge is a participant of the file, and counts as any other.
[[nodiscard]] RunPlan plan_run(const CheckReport& check, const ScenarioCheck* scenario, std::filesystem::path base,
                               const RunChoices& choices, const RunLimits& limits = {}, bool control_server = false);

/// The outcome a run of `plan` begins with: what the plan already says of the result (the limits,
/// the timing, the participants in the plan's order, the scenario as not run, the networks
/// connected outside), and nothing of what the run finds.
[[nodiscard]] RunOutcome planned_outcome(const RunPlan& plan);

} // namespace vehicle_sil::launch
