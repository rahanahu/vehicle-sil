// SPDX-License-Identifier: MIT
//
// The start of a run's simulation: the wait for the participants to join, the window before the
// first step (the control agents' hellos, what is written before the start, the scenario's
// beginning, the hold of --start-paused), and the limits in time of all that. Internal.
#pragma once

#include "run_context.hpp"
#include "run_control.hpp"
#include "run_notes.hpp"
#include "run_output.hpp"
#include "run_participants.hpp"
#include "run_recorder.hpp"
#include "run_scenario.hpp"
#include "vehicle_sil/runner/detail/run_start_limit.hpp"

#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::runner::detail {

/// What the main thread (RunStart) and the start window's thread (RunStartWindow) share while
/// the simulation starts. Nothing else of the run is touched by both.
///
/// Locks: the limit's mutex (StartLimit) and `found_mutex` are independent, and nothing is taken
/// while either is held. Neither is taken while TimeCommands' mutex is held
/// (TimeCommands::hold_before_start() calls on_hold and give_up without it).
struct StartShared
{
    /// The limit of the start (the give-up of SimulationController::start()). Written by the main
    /// thread and the window's thread, read by both.
    StartLimit limit;

    /// What the start window found. Written by the window's thread, read by the main thread after
    /// SimulationController::start() has returned. Guarded by `found_mutex`.
    std::mutex found_mutex;
    std::optional<ControlStart> control_start;
    std::string before_start_error; ///< RunObservers::before_start failed with this

    /// When the launcher began to wait for the participants (steady-clock nanoseconds), in a run
    /// that waits for one started by hand and has --timeout-ms; -1 otherwise. The main thread
    /// writes it before the controller starts; both read it.
    std::atomic<std::int64_t> manual_wait_began_ns{-1};
    /// When the hold began (steady-clock nanoseconds), or -1 if there was none. The window's
    /// thread writes these three; the main thread reads them once the simulation runs.
    std::atomic<std::int64_t> hold_began_ns{-1};
    std::atomic<std::int64_t> hold_ms{-1}; ///< how long the hold before the first step lasted (-1: none)
    /// --timeout-ms ended the hold.
    std::atomic<bool> hold_timed_out{false};

    /// manual_wait_began_ns as a time; empty for -1.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> manual_wait_began() const
    {
        const std::int64_t ns = manual_wait_began_ns.load();
        if (ns < 0)
        {
            return std::nullopt;
        }
        return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ns));
    }
};

/// The window before the start, on the controller's thread (SimulationController's start
/// window): every participant waits before its first step.
///
/// Threads and locks: run() is the window's thread's, and all this class does. Of the run it
/// touches the StartShared (under its locks), and what keeps itself: the hub and its transport,
/// the RunScenario's begin(), the RunOutput, the TimeCommands. It never sees the RunOutcome. The
/// main thread waits in SimulationController::start() meanwhile; if that gives up, it waits for
/// run() to return (StartWindow::cancelled() tells it to).
class RunStartWindow
{
public:
    RunStartWindow(const RunContext& ctx, RunOutput& out, RunControl& control, RunScenario& scenario,
                   RunRecorder& recorder, StartShared& shared) :
      ctx_(ctx), out_(out), control_(control), scenario_(scenario), recorder_(recorder), shared_(shared)
    {
    }

    /// Waits for the agents' hellos, then lets the caller write (RunObservers::before_start), begins
    /// the scenario, and holds before the first step (--start-paused with a control server).
    void run(control_silkit::HubTransport& transport, SimulationController& controller, const StartWindow& window);

private:
    const RunContext& ctx_;
    RunOutput& out_;
    RunControl& control_;
    RunScenario& scenario_;
    RunRecorder& recorder_;
    StartShared& shared_;
};

/// The start of the simulation, on the main thread.
///
/// Threads and locks: wait_for_start() and everything it calls is the main thread's (also
/// start_given_up(): SimulationController::start() calls its give_up on the caller's thread), as
/// are the RunOutcome and the members below but `shared_`. window() is called on the
/// controller's thread and only hands on to the RunStartWindow.
class RunStart
{
public:
    RunStart(const RunContext& ctx, RunOutput& out, RunControl& control, RunNotes& notes, RunScenario& scenario,
             RunRecorder& recorder, RunParticipants& participants, RunOutcome& outcome);

    /// The controller's start window (the controller's thread).
    void window(control_silkit::HubTransport& transport, SimulationController& controller, const StartWindow& window)
    {
        window_.run(transport, controller, window);
    }

    /// After the bridges of --connect have been started, before anything else: waits until each
    /// that was started runs in SIL Kit (Running, as the controller sees it), up to start_timeout_ms.
    /// false if one ended, left (Error or Shutdown), did not get there in time, or the run was
    /// interrupted: the outcome says why, and nothing else is to be started. With --strict, also
    /// false if a bridge shares its device with a bridge that is not of this run (the finding
    /// outside-device-shared). true at once without bridges.
    [[nodiscard]] bool wait_for_bridges(SimulationController& controller);

    /// Waits until every required participant has joined, then starts the simulation. false if
    /// it did not start (the outcome says why). `transport`: for the control links after the start.
    [[nodiscard]] bool wait_for_start(SimulationController& controller, control_silkit::HubTransport& transport);

    /// The simulation ran, and was aborted because what was to be done before it failed: the
    /// participants are ended as after a run.
    [[nodiscard]] bool before_start_failed() const { return before_start_failed_; }

    // For --timeout-ms while the simulation runs (thread safe: atomics of the StartShared).
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> manual_wait_began() const
    {
        return shared_.manual_wait_began();
    }
    [[nodiscard]] std::int64_t hold_began_ns() const { return shared_.hold_began_ns.load(); }
    [[nodiscard]] std::int64_t hold_ms() const { return shared_.hold_ms.load(); }
    [[nodiscard]] bool hold_timed_out() const { return shared_.hold_timed_out.load(); }

private:
    /// Waits (up to kOutsideFirstLineWait) for the first link line of each of `bridges`, then looks at
    /// whether one shares its device with a bridge that is not this run's, and says so. With `strict`,
    /// true: one does, the run is not to start (the outcome's bridge says so, and so does a line).
    /// Also true if the run was interrupted meanwhile. Without `strict` it only says it.
    [[nodiscard]] bool stopped_for_shared_device(const std::vector<std::size_t>& bridges, bool strict);
    /// The give_up of the start: a required participant has left, or the start's limit has passed
    /// (the participants that hold it up named).
    [[nodiscard]] std::string start_given_up(SimulationController& controller);
    /// The required participants that hold the start up, with their states: those that have not
    /// got to ReadyToRun (the others wait for them), or else all that do not run yet.
    [[nodiscard]] std::string not_ready(SimulationController& controller) const;
    /// Every enabled required participant started by hand is ready for its first step (ReadyToRun
    /// or further).
    [[nodiscard]] bool manual_ready(SimulationController& controller) const;
    /// After the start: where each participant's control agent stands (RunControl::note_links()),
    /// from what the window found and, for the agents it did not wait for, the links now.
    void note_control(control_silkit::HubTransport& transport);
    /// --timeout-ms passed before the simulation started (a run that waits for a participant started
    /// by hand): `waiting_for` says for what. Returns false (it did not start).
    bool start_timed_out(const std::string& waiting_for);
    /// A stop request came before the simulation started: notes who asked in the outcome and says
    /// so. The caller returns false.
    void interrupted_before_start();
    /// Says once of each participant that joined the simulation under a name this run does not
    /// have and waits for the start (a mistyped --name of a participant started by hand;
    /// StrangerWait): the run waits on without it. One outside the file that runs on its own
    /// (autonomous) is not named.
    void tell_strangers(SimulationController& controller);

    const RunContext& ctx_;
    RunOutput& out_;
    RunControl& control_;
    RunNotes& notes_;
    RunParticipants& participants_;
    RunOutcome& outcome_; ///< the main thread's
    StartShared shared_;
    RunStartWindow window_;
    /// --timeout-ms passed during the start (start_given_up()); what it waited for.
    std::string manual_timed_out_;
    std::vector<std::string> strangers_told_;
    StrangerWait stranger_wait_;
    bool before_start_failed_ = false;
};

} // namespace vehicle_sil::runner::detail
