// SPDX-License-Identifier: MIT
//
// run --scenario, joined to the run: the scenario's engine (scenario_run.hpp) between the hub, the
// controller's time gate and the run's result. Internal.
#pragma once

#include "run_context.hpp"
#include "run_control.hpp"
#include "run_output.hpp"
#include "vehicle_sil/runner/detail/run_samples.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::runner::detail {

/// The scenario of a run. Every call but end() and over() is made only in a run with a scenario
/// (RunContext::scenario is not null), or does nothing without one (start(), finish()).
///
/// The threads. The start window's thread begins the scenario (begin(): the writes for 0) while
/// every participant waits before its first step. Then the scenario's thread (loop()) handles
/// each step of the controller in turn, while the time gate's guard holds that step: it gives the
/// engine the samples that have arrived (it takes them from the run's samples, RunSamples, whose
/// sink for the scenario is sample()) and lets it judge and write (ScenarioEngine::tick()), then
/// moves the guard on. The main thread watches the simulation as always; it stops it when the
/// scenario is over (over()), and finishes the scenario (finish()) after the scenario's thread
/// has ended.
///
/// The state, who touches it, and under which lock:
/// - The samples: RunSamples keeps them (run_samples.hpp: the hub's tap, its queue for the
///   scenario, its lock). The scenario gets those the hub takes after begin() has said so
///   (RunSamples::scenario_begins()), of the control agents that were there then; drain() takes
///   them (the scenario's thread; the main thread in finish(), after that thread has ended).
///   finish() and end() end that (RunSamples::scenario_ends()).
/// - port_, engine_: no lock; they go from thread to thread. begin() makes them on
///   the start window's thread. The main thread calls start() only after
///   SimulationController::start() has returned, and then the window's function does not run any
///   more; from start() until the thread is joined they are the scenario's thread's; after the
///   join (finish(), end()) the main thread's.
/// - quit_, over_: atomics (main says "end"; the scenario's thread says "the scenario is over").
/// - thread_error_ (error_mutex_): the scenario's thread writes it, finish() reads it. Nothing is
///   taken while the mutex is held.
/// - step_, thread_: the main thread only (step_ is set by prepare() before the controller
///   starts, so the window's thread only reads it).
/// The engine's callback for a step (note_step()) writes through the RunOutput, on the window's
/// thread (in begin()) and the scenario's thread: it takes the output's lock, with none of the
/// locks above held.
///
/// The RunOutcome is the main thread's: only start() and finish() get it.
class RunScenario
{
public:
    RunScenario(const RunContext& ctx, RunOutput& out, RunControl& control, RunSamples& samples);
    ~RunScenario();

    /// `step`: the step of the launcher's controller. Main thread, in the setup (no controller
    /// thread yet).
    void prepare(std::chrono::milliseconds step) { step_ = step; }

    /// In the start window (its thread): who is there, then the writes for 0. `cancelled` ends
    /// the wait of a write while the window lasts. A scenario that judges with every step
    /// (ScenarioSamples::EveryStep) does not begin if a participant whose values it reads sends
    /// them less often than its step (its hello's sample_period against the step of its
    /// --describe): no engine is made, start() says that the scenario is over, and finish() gives
    /// the outcome who it was (OutcomeScenario::not_observed).
    void begin(control_silkit::HubTransport& transport, const std::function<bool()>& cancelled);

    /// begin() refused to begin the scenario (an ECU sends the values it reads less often than its
    /// step): the run does not hold before the first step for it, and says so when it stops. The
    /// window's thread after begin(), the main thread after the controller's start (as engine_).
    [[nodiscard]] bool refused() const { return !not_observed_.empty(); }

    /// The simulation runs: the scenario's thread takes the steps from here. Main thread.
    void start(SimulationController& controller, RunOutcome& outcome);

    /// The scenario's thread has found the scenario over (or has failed): stop the simulation.
    [[nodiscard]] bool over() const { return over_.load(); }

    /// Wait for a new completion notification, or the polling limit. The caller keeps its own
    /// generation so an already observed completion does not make later checks spin. The
    /// optional test probe runs under the completion mutex and must not wait or re-enter it.
    [[nodiscard]] bool wait_for_completion(std::uint64_t& generation, std::chrono::milliseconds timeout,
                                           const std::function<void()>& before_wait = {}) const;

    /// Wakes wait_for_completion() early (for non-signal stop requests while watching a scenario).
    /// Thread safe; must NOT be called from a signal handler.
    void wake_waiters();

    /// The simulation is over (main thread): ends the scenario's thread and decides what is left.
    void finish(SimulationController& controller, const WatchResult& watched, bool for_scenario, RunOutcome& outcome);

    /// Whatever happened: no thread of the scenario is left, and no sample goes to it. Main
    /// thread; before the controller and the hub's transport go.
    void end();

private:
    class HubPort;

    void mark_over();
    void note_step(const scenario::ScenarioStepRun& step);
    /// Gives the engine the samples that have arrived since the last call.
    void drain();
    void loop(SimulationController& controller);

    const RunContext& ctx_;
    RunOutput& out_;
    RunControl& control_;
    RunSamples& samples_;
    std::chrono::milliseconds step_{}; ///< the controller's step (prepare())
    /// Per participant of the configuration: its place among the scenario's participants (none:
    /// the scenario does not use it). Never written after the constructor.
    std::vector<std::optional<std::size_t>> places_;
    std::unique_ptr<HubPort> port_;
    /// run --timeline: what the engine notes (made in begin(), before the engine). Touched as
    /// engine_ is: the engine fills it; finish() gives it to the outcome.
    std::shared_ptr<scenario::TimelineRecord> record_;
    std::unique_ptr<scenario::ScenarioEngine> engine_; ///< the window's thread, then the scenario's, then main
    /// begin() found these and made no engine (the scenario does not begin). Touched as engine_ is.
    std::vector<ScenarioNotObserved> not_observed_;
    std::thread thread_;
    std::atomic<bool> quit_{false}; ///< main: the simulation is over
    std::atomic<bool> over_{false}; ///< the scenario's thread: the scenario is over, stop the simulation
    mutable std::mutex completion_mutex_;
    mutable std::condition_variable completion_changed_;
    std::uint64_t completion_generation_ = 0; ///< guarded by completion_mutex_
    std::mutex error_mutex_;
    std::string thread_error_; ///< guarded by error_mutex_
};

} // namespace vehicle_sil::runner::detail
