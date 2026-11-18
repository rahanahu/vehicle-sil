// SPDX-License-Identifier: MIT
//
// One run from its setup to the end of its participants (Runner), as run_file() drives it: its two
// stages and what its caller reads of it. Internal: in detail/ only because run_file() is on the
// other side of the run's boundary. The parts of the run, its threads and what it has of SIL Kit
// are behind a pointer (RunnerImpl, defined in the launcher's sources, runner.hpp beside run.cpp),
// so that this header reaches no SIL Kit header, no header of the controller (core's
// simulation_controller.hpp) and none of the run's private headers. The parts and the threads:
// the comment at the top of run.cpp.
#pragma once

#include "vehicle_sil/launch/run_controls.hpp"
#include "vehicle_sil/launch/run_events.hpp"
#include "vehicle_sil/launch/run_outcome.hpp"
#include "vehicle_sil/launch/run_plan.hpp"

#include <memory>
#include <optional>
#include <string>

namespace vehicle_sil::control {
class Hub;
} // namespace vehicle_sil::control

namespace vehicle_sil::launch::detail {

/// What run_file() did between the two stages of a Runner (prepare(), then run()): whether it
/// started a control server (RunHost::control), and how that went. As made: none.
struct ControlServerStart
{
    std::optional<ControlServerInfo> listening; ///< the server listens there
    std::optional<std::string> error;           ///< it could not be started: why (what its start threw)
};

class RunnerImpl; // the parts of the run and the order of its phases

/// One run: its parts, and the order of its phases in two stages. prepare() opens the run's files;
/// run() does everything from the registry to the end. Between them the caller starts what serves
/// the run's hub from outside (a control server), so that it listens before the registry does.
/// The hub is made with the Runner (by its RunControl) and lives as long as it (and as whoever else
/// holds it). The registry, the controller and the hub's transport live in run() only, in a
/// RunConnection; the parts are given the controller in the calls that need it. A Runner can be
/// destroyed whenever run() has returned, also while the closing thread of its run is still closing
/// (it was left at its limit). Neither copied nor moved.
class Runner
{
public:
    /// `plan`: plan_run() of the check that passed. `observers`: the caller's functions, copied.
    /// `events`: what the run tells while it lasts (the object of RunLent::events, or null);
    /// it outlives the Runner. `controls`: RunLent::controls, or the run's own; they outlive the Runner.
    Runner(RunPlan plan, RunObservers observers, RunEvents* events, RunControls& controls);
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    /// The first stage: the phase "starting", then the run's files (the participants' logs, the
    /// timeline). Returns the run's hub (never null, for whoever serves it; the holder keeps the
    /// hub only, see RunConnection) if the run goes on; null if it does not (it was interrupted
    /// before it began, or a file cannot be opened: the outcome says so). Nothing of SIL Kit
    /// exists yet, and no other thread of the run. run() must follow, whatever this returned.
    [[nodiscard]] std::shared_ptr<control::Hub> prepare();

    /// The second stage, once, after prepare(): the registry, the participants, the simulation and
    /// its end; all of the outcome. After a prepare() that returned null it starts nothing and
    /// only ends the time commands. `server`: what the caller did in between; the run says where
    /// the server listens, or starts nothing if it could not be started.
    RunOutcome run(const ControlServerStart& server);

    /// run --events: the run's findings that have not been written yet. Before the participants
    /// have ended (`final` false) only those that their end cannot change (settled_findings()):
    /// what is written as an event is in the result as it was written.
    void publish_findings(bool final);

    /// The plan of this run, as the Runner keeps it (with the bridges of --connect).
    [[nodiscard]] const RunPlan& plan() const;

    /// run --timeline: the timeline file was opened (prepare()) and has not been written yet.
    [[nodiscard]] bool timeline_open() const;

    /// run --timeline: writes `text` (the timeline of the run that is over, as the caller made
    /// it) to the timeline file, and says where it is. Nothing if the file was not opened (no
    /// --timeline, or the run ended before it got there). If it cannot be written: why (also in
    /// the outcome: OutcomeScenario::timeline_error), else "".
    std::string write_timeline(const std::string& text);

private:
    std::unique_ptr<RunnerImpl> impl_; ///< never null
};

} // namespace vehicle_sil::launch::detail
