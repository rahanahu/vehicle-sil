// SPDX-License-Identifier: MIT
//
// The watch of a run's simulation while it runs: a stop request, a participant that failed,
// --timeout-ms, the scenario's end; and what is noted when it is over. Internal.
#pragma once

#include "run_context.hpp"
#include "run_notes.hpp"
#include "run_output.hpp"
#include "run_participants.hpp"
#include "run_scenario.hpp"
#include "run_start.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

namespace vehicle_sil::launch::detail {

/// Watches the simulation from its start to its end.
///
/// Threads and locks: the main thread only; no state and no lock of its own. What it reads of
/// other threads is atomic (RunScenario::over(), the hold's times of RunStart) or the
/// controller's. It ends the time commands (TimeCommands::detach()) once the simulation is over.
class RunWatch
{
public:
    RunWatch(const RunContext& ctx, RunOutput& out, RunNotes& notes, RunScenario& scenario, const RunStart& start,
             RunParticipants& participants, RunOutcome& outcome) :
      ctx_(ctx), out_(out), notes_(notes), scenario_(scenario), start_(start), participants_(participants), outcome_(outcome)
    {
    }

    /// Watches until the simulation is over (stopping or aborting it if it has to), and notes in
    /// the outcome how it ended. Returns the stop requests that had come when it had ended (one
    /// from then on comes while the participants end).
    [[nodiscard]] int watch(SimulationController& controller);

private:
    /// The simulation is over: how its time went. The gate's numbers stopped when the run ended
    /// (a stop, an abort, stop_at_ms), so the shutdown is not in them.
    void note_timing(SimulationController& controller);

    const RunContext& ctx_;
    RunOutput& out_;
    RunNotes& notes_;
    RunScenario& scenario_;
    const RunStart& start_;
    RunParticipants& participants_;
    RunOutcome& outcome_; ///< the main thread's
};

} // namespace vehicle_sil::launch::detail
