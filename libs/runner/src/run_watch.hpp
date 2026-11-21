// SPDX-License-Identifier: MIT
//
// The watch of a run's simulation while it runs: a stop request, a participant that failed,
// --timeout-ms, the scenario's end; and, called after it, what is settled when it is over.
// Internal.
#pragma once

#include "run_context.hpp"
#include "run_notes.hpp"
#include "run_output.hpp"
#include "run_participants.hpp"
#include "vehicle_sil/runner/detail/run_samples.hpp"
#include "run_scenario.hpp"
#include "run_start.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"

namespace vehicle_sil::runner::detail {

// The scenario notification only wakes the existing watch. Keeping the wait operations
// explicit lets tests fix the ordering of controller completion and scenario notification.
struct ScenarioWatchWait
{
    using State = SilKit::Services::Orchestration::ParticipantState;
    std::function<std::optional<State>(std::chrono::milliseconds)> controller;
    std::function<bool(std::chrono::milliseconds)> scenario;
    std::function<bool()> stopping;
    std::function<std::chrono::steady_clock::time_point()> now;
};

[[nodiscard]] std::optional<ScenarioWatchWait::State> wait_scenario_watch(
    const ScenarioWatchWait& wait, std::chrono::milliseconds timeout);

/// Watches the simulation from its start to its end (watch()), then settles what is known when it
/// is over (settle(), which the run calls right after).
///
/// Threads and locks: the main thread only; no lock of its own, and no state but what it says of
/// a simulation that stands still (StandStill). What it reads of
/// other threads is atomic (RunScenario::over(), the hold's times of RunStart) or the
/// controller's. settle() ends the time commands (TimeCommands::detach()) once the simulation is
/// over, and asks the run's samples (RunSamples, which keeps itself) for the traffic of the result.
class RunWatch
{
public:
    RunWatch(const RunContext& ctx, RunOutput& out, RunNotes& notes, RunSamples& samples, RunScenario& scenario,
             const RunStart& start, RunParticipants& participants, RunOutcome& outcome) :
      ctx_(ctx), out_(out), notes_(notes), samples_(samples), scenario_(scenario), start_(start), participants_(participants),
      outcome_(outcome)
    {
    }

    /// How the watch ended.
    struct Watched
    {
        WatchResult result;
        bool for_scenario = false; ///< the scenario was over: the watch stopped the simulation for it
    };

    /// Watches until the simulation is over (stopping or aborting it if it has to).
    [[nodiscard]] Watched watch(SimulationController& controller);

    /// The simulation that `over` watched is over: ends the scenario, notes its timing and
    /// traffic, ends the time commands, and notes in the outcome how it ended. Returns the stop
    /// requests that had come when it had ended (one from then on comes while the participants
    /// end).
    [[nodiscard]] int settle(SimulationController& controller, const Watched& over);

private:
    /// The simulation is over: how its time went. The gate's numbers stopped when the run ended
    /// (a stop, an abort, stop_at_ms), so the shutdown is not in them.
    void note_timing(SimulationController& controller);

    const RunContext& ctx_;
    RunOutput& out_;
    RunNotes& notes_;
    RunSamples& samples_;
    RunScenario& scenario_;
    const RunStart& start_;
    RunParticipants& participants_;
    RunOutcome& outcome_; ///< the main thread's
    StandStill stand_still_;
};

} // namespace vehicle_sil::runner::detail
