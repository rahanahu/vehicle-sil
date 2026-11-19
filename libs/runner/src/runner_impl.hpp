// SPDX-License-Identifier: MIT
//
// The body of a Runner (RunnerImpl): the parts of one run and the order of its phases. Runner
// (vehicle_sil/runner/detail/runner.hpp) is what run_file() sees; it holds a RunnerImpl and
// forwards to it, so that what this header includes (SIL Kit, the controller, the run's private
// headers) stays on this side. The parts and the threads: the comment at the top of run.cpp.
// Internal.
#pragma once

#include "run_connection.hpp"
#include "run_context.hpp"
#include "run_control.hpp"
#include "run_notes.hpp"
#include "run_output.hpp"
#include "run_participants.hpp"
#include "vehicle_sil/runner/detail/run_samples.hpp"
#include "run_scenario.hpp"
#include "run_start.hpp"
#include "run_watch.hpp"

#include "vehicle_sil/runner/detail/runner.hpp"
#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/runner/run_events.hpp"
#include "vehicle_sil/runner/run_outcome.hpp"
#include "vehicle_sil/runner/run_plan.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/vendor/ISilKitRegistry.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace vehicle_sil::runner::detail {

/// What a run has of SIL Kit, with its hub (run_connection.hpp).
using RunConnection = ConnectionOf<control::Hub, SilKit::Vendor::Vector::ISilKitRegistry, SimulationController,
                                   control_silkit::HubTransport>;

/// The body of a Runner: its parts, and the order of the run's phases. Each member function of
/// the same name as one of Runner does what Runner's says (detail/runner.hpp). Made by Runner
/// only, and destroyed with it.
class RunnerImpl
{
public:
    RunnerImpl(RunPlan plan, RunObservers observers, RunEvents* events, RunControls& controls);
    [[nodiscard]] std::shared_ptr<control::Hub> prepare();
    RunOutcome run(const ControlServerStart& server);
    void publish_findings(bool final) { notes_.publish_findings(outcome_, final); }
    [[nodiscard]] const RunPlan& plan() const { return plan_; }
    [[nodiscard]] bool timeline_open() const { return timeline_file_.is_open(); }
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

} // namespace vehicle_sil::runner::detail
