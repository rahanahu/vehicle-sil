// SPDX-License-Identifier: MIT
//
// One run from its setup to the end of its participants (Runner), as run_file() drives it: the
// parts of the run and the order of its phases. The parts and the threads: the comment at the top
// of run.cpp. Internal.
#pragma once

#include "run_connection.hpp"
#include "run_context.hpp"
#include "run_control.hpp"
#include "run_notes.hpp"
#include "run_output.hpp"
#include "run_participants.hpp"
#include "run_samples.hpp"
#include "run_scenario.hpp"
#include "run_start.hpp"
#include "run_watch.hpp"

#include "vehicle_sil/launch/run_controls.hpp"
#include "vehicle_sil/launch/run_events.hpp"
#include "vehicle_sil/launch/run_outcome.hpp"
#include "vehicle_sil/launch/run_plan.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/vendor/ISilKitRegistry.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace vehicle_sil::launch::detail {

/// What a run has of SIL Kit, with its hub (run_connection.hpp).
using RunConnection = ConnectionOf<control::Hub, SilKit::Vendor::Vector::ISilKitRegistry, SimulationController,
                                   control_silkit::HubTransport>;

/// What run_file() did between the two stages of a Runner (prepare(), then run()): whether it
/// started a control server (RunHost::control), and how that went. As made: none.
struct ControlServerStart
{
    std::optional<ControlServerInfo> listening; ///< the server listens there
    std::optional<std::string> error;           ///< it could not be started: why (what its start threw)
};

/// One run: its parts, and the order of its phases in two stages. prepare() opens the run's files;
/// run() does everything from the registry to the end. Between them the caller starts what serves
/// the run's hub from outside (a control server), so that it listens before the registry does.
/// The hub is made with the Runner (by its RunControl) and lives as long as it (and as whoever else holds it). The registry, the
/// controller and the hub's transport live in run() only, in a RunConnection; the parts are given
/// the controller in the calls that need it. A Runner can be destroyed whenever run() has
/// returned, also while the closing thread of its run is still closing (it was left at its limit).
class Runner
{
public:
    /// `plan`: plan_run() of the check that passed. `observers`: the caller's functions, copied.
    /// `events`: what the run tells while it lasts (the object of RunObservers::events, or null);
    /// it outlives the Runner. `controls`: RunLent::controls, or the run's own; they outlive the Runner.
    Runner(RunPlan plan, RunObservers observers, RunEvents* events, RunControls& controls);

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
    void publish_findings(bool final) { notes_.publish_findings(outcome_, final); }

    /// The plan of this run, as the Runner keeps it (with the bridges of --connect).
    [[nodiscard]] const RunPlan& plan() const { return plan_; }

    /// run --timeline: the timeline file was opened (prepare()) and has not been written yet.
    [[nodiscard]] bool timeline_open() const { return timeline_file_.is_open(); }

    /// run --timeline: writes `text` (the timeline of the run that is over, as the caller made
    /// it) to the timeline file, and says where it is. Nothing if the file was not opened (no
    /// --timeline, or the run ended before it got there). If it cannot be written: why (also in
    /// the outcome: OutcomeScenario::timeline_error), else "".
    std::string write_timeline(const std::string& text);

private:
    /// run --timeline: opens the timeline file, before anything starts. false (with
    /// outcome_.setup.error) if it cannot be written.
    [[nodiscard]] bool open_timeline();

    /// The end of what the run has of SIL Kit: destroys the hub's transport, the controller and
    /// the registry, in this order, and waits for that close_limit_ at most (one more interrupt
    /// ends the wait). SIL Kit joins the thread of its connections when a participant is
    /// destroyed, and a connection that does not close would keep the run from its result for
    /// ever (seen on Windows before its connections were TCP ones, with a participant whose
    /// threads were suspended: docs/dependencies.md). So the three are destroyed on a thread of
    /// their own (detail::close_connection_within()), which takes over `connection`, the only
    /// hold of them; if it has not ended in time, it is left with them and with its share of the
    /// hub (OutcomeClose::not_closed_ms), and the run goes on to its result.
    void close_connection(std::shared_ptr<RunConnection> connection);


    // The order matters. Each part keeps references to those it uses (all declared here, so they
    // outlive run()); they are destroyed in the reverse order.
    /// What ctx_ refers to: the plan (with the check that has the bridges of --connect, which is
    /// not run_file()'s report) and the observers. Never written after the constructor.
    const RunPlan plan_;
    const RunObservers observers_;
    const RunContext ctx_;
    RunOutcome outcome_; ///< the main thread's: the other threads of the run never see it
    RunOutput out_;
    /// Makes and holds the hub of this run; the RunConnection of run() and whoever was given the
    /// hub by a callback of RunObservers hold it too. Before samples_ and scenario_, which use the
    /// hub until they are destroyed (the tap, the scenario's engine and port).
    RunControl control_;
    RunSamples samples_; ///< after control_ (its tap is on the hub), before scenario_ (which takes from it)
    RunNotes notes_;
    RunScenario scenario_;
    RunStart start_;
    RunWatch watch_;
    bool prepared_ = false;     ///< prepare() got through: run() goes on
    int interrupts_at_end_ = 0; ///< ctx_.interrupts() when the simulation had ended
    std::ofstream timeline_file_; ///< run --timeline: open from open_timeline() to write_timeline(); the main thread's
    static constexpr std::chrono::milliseconds kCloseMargin{5000};
    std::chrono::milliseconds close_limit_{0}; ///< close_connection(): how long the SIL Kit objects may take to go
    // Last, so that it goes first: a Process ends its reader thread, which uses the output (out_),
    // also when run() leaves with an exception.
    RunParticipants participants_;
};

} // namespace vehicle_sil::launch::detail
