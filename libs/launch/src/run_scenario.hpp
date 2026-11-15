// SPDX-License-Identifier: MIT
//
// run --scenario, joined to the run: the scenario's engine (scenario_run.hpp) between the hub, the
// controller's time gate and the run's result. Internal.
#pragma once

#include "run_context.hpp"
#include "run_control.hpp"
#include "run_output.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"
#include "vehicle_sil/launch/scenario_run.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::launch::detail {

/// The scenario of a run. Every call but end() and over() is made only in a run with a scenario
/// (RunContext::scenario is not null), or does nothing without one (start(), finish()).
///
/// The threads. The start window's thread begins the scenario (begin(): the writes for 0) while
/// every participant waits before its first step. Then the scenario's thread (loop()) handles
/// each step of the controller in turn, while the time gate's guard holds that step: it gives the
/// engine the samples that have arrived and lets it judge and write (ScenarioEngine::tick()), then
/// moves the guard on. The hub's tap (the controller's SIL Kit thread) only puts each sample into
/// samples_. The main thread watches the simulation as always; it stops it when the scenario is
/// over (over()), and finishes the scenario (finish()) after the scenario's thread has ended.
///
/// The state, who touches it, and under which lock:
/// - samples_ (its mutex): the hub's tap appends; begin() sets whose samples count; drain() takes
///   them (the scenario's thread; the main thread in finish(), after that thread has ended). The
///   mutex is taken inside the hub's call of the tap, and nothing is taken while it is held.
/// - port_, engine_: no lock; they go from thread to thread. begin() makes them on the start
///   window's thread. The main thread calls start() only after SimulationController::start() has
///   returned, and then the window's function does not run any more; from start() until the
///   thread is joined they are the scenario's thread's; after the join (finish(), end()) the main
///   thread's.
/// - quit_, over_: atomics (main says "end"; the scenario's thread says "the scenario is over").
/// - thread_error_ (error_mutex_): the scenario's thread writes it, finish() reads it. Nothing is
///   taken while the mutex is held.
/// - step_, tapped_, thread_: the main thread only (step_ is set by tap() before the controller
///   starts, so the window's thread only reads it).
/// The engine's callback for a step (note_step()) writes through the RunOutput, on the window's
/// thread (in begin()) and the scenario's thread: it takes the output's lock, with none of the
/// locks above held.
///
/// The RunOutcome is the main thread's: only start() and finish() get it.
class RunScenario
{
public:
    RunScenario(const RunContext& ctx, RunOutput& out, RunControl& control);
    ~RunScenario();

    /// The tap on the hub: every sample of a scenario participant, in the order taken. `step`:
    /// the step of the launcher's controller. Main thread, in the setup (no controller thread yet).
    void tap(std::chrono::milliseconds step);

    /// In the start window (its thread): who is there, then the writes for 0. `cancelled` ends
    /// the wait of a write while the window lasts.
    void begin(control_silkit::HubTransport& transport, const std::function<bool()>& cancelled);

    /// The simulation runs: the scenario's thread takes the steps from here. Main thread.
    void start(SimulationController& controller, RunOutcome& outcome);

    /// The scenario's thread has found the scenario over (or has failed): stop the simulation.
    [[nodiscard]] bool over() const { return over_.load(); }

    /// The simulation is over (main thread): ends the scenario's thread and decides what is left.
    void finish(SimulationController& controller, const WatchResult& watched, bool for_scenario, RunOutcome& outcome);

    /// Whatever happened: no thread and no tap of the scenario is left. Main thread; before the
    /// controller and the hub's transport go.
    void end();

private:
    class HubPort;

    void note_step(const ScenarioStepRun& step);
    /// Gives the engine the samples that have arrived since the last call.
    void drain();
    void loop(SimulationController& controller);

    const RunContext& ctx_;
    RunOutput& out_;
    RunControl& control_;
    std::chrono::milliseconds step_{}; ///< the controller's step (tap())
    struct ScenarioSamples
    {
        std::mutex mutex;
        std::vector<std::string> incarnations; ///< per scenario participant: the agent whose samples count
        std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> taken;
    };
    ScenarioSamples samples_;
    bool tapped_ = false;
    std::unique_ptr<HubPort> port_;
    /// run --timeline: what the engine notes (made in begin(), before the engine). Touched as
    /// engine_ is: the engine fills it; finish() gives it to the outcome.
    std::shared_ptr<TimelineRecord> record_;
    std::unique_ptr<ScenarioEngine> engine_; ///< the window's thread, then the scenario's, then main
    std::thread thread_;
    std::atomic<bool> quit_{false}; ///< main: the simulation is over
    std::atomic<bool> over_{false}; ///< the scenario's thread: the scenario is over, stop the simulation
    std::mutex error_mutex_;
    std::string thread_error_; ///< guarded by error_mutex_
};

} // namespace vehicle_sil::launch::detail
