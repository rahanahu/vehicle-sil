// SPDX-License-Identifier: MIT
//
// System controller for a coordinated, virtual-time simulation (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/real_time_pacer.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <silkit/services/orchestration/OrchestrationDatatypes.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SilKit::Experimental::Services::Orchestration {
class ISystemController;
} // namespace SilKit::Experimental::Services::Orchestration

namespace vehicle_sil {

/// Pausing, stepping and speed control of a coordinated virtual-time simulation (the time gate).
///
/// Pacing and lateness. With a speed, the gate ties virtual time to the wall clock, so that
/// something that runs in real time and does not wait (a device) can be attached. The numbers of
/// TimeState say how well that held; they are defined as follows.
///
/// - Base: a virtual time and the wall-clock time it stands for. It is taken at the first
///   controller step (not when the speed is set before the start), on resume, and when pacing
///   starts; a change of speed keeps the wall-clock time of the next step.
/// - Deadline of a controller step: the wall-clock time its start stands for,
///   base wall + (start - base virtual) / speed. No step begins before its deadline (the gate holds
///   the step before it back until then).
/// - A step begins when the gate has let the step before it go (from then on the participants
///   may run their steps that start up to its start) and the controller has learnt that every
///   participant has finished its steps that start before it. SIL Kit tells it when the
///   participant's thread is free: a participant that goes from one step straight into the next
///   is seen to have finished the first only after the second. A step is therefore never measured
///   as beginning earlier than the participants could begin theirs, but possibly later, by up to
///   the time one step of a participant takes; over a run, `slipped` is exact all the same (the
///   time the run took beyond the time at the speed).
/// - Lateness of a step: when it began, minus its deadline (never below 0). The gate's own late
///   wake-up is part of it: a device cannot tell it from a slow participant (the gate waits with a
///   precise timer instead; TimeState::max_gate_lateness shows what is left of it).
/// - A late step: lateness > late_tolerance. Lateness within the tolerance is made up for by
///   itself: the deadlines stay, and a step whose deadline has passed is not held back.
/// - Slip: a late step is not caught up with. Its start becomes the new base, so the steps after
///   it keep the speed, and all virtual time from then on comes later by its lateness, which is
///   added to TimeState::slipped. (Catching up would run the missed steps at once: periodic
///   messages would leave in a burst, which is further from real time than coming late.) One slow
///   step is therefore one late step, however long it took.
/// - What is counted: every step that began while a base was set, from the first one after the
///   base to the last one, including the step at stop_at. A step that has not begun although its
///   deadline has passed is counted with the lateness it has so far, as if the run ended now:
///   time_state() shows a stall while it lasts, and a run that ends in one (a stop, an abort, a
///   failed participant) keeps it. The numbers stop changing when a stop is requested or the
///   controller's lifecycle leaves Running. Time stood still (paused) is not counted: resume takes
///   a new base.
/// - Granularity: the gate aligns the starts of the controller's steps only. A participant's
///   step that starts between two of them at T runs when the gate lets the controller step before
///   it go, which is the deadline of the first controller step at or after T: up to
///   step - gcd(step, the participant's step) later than T, never earlier. A participant whose
///   step is a multiple of the controller's is not affected; use the greatest common divisor of
///   the participants' steps as `step` when a device is attached.
/// - Limit of the measurement: the controller learns that a participant's step has ended when
///   SIL Kit tells it, and a participant that goes straight into its next step tells it only
///   after that one. The lateness of a step can so be measured larger than it was, by up to the
///   real time one step of that participant takes, never smaller. A lateness measured above the
///   tolerance is a late step like any other: the base moves, and the slip is then real. With a
///   participant whose steps overrun, choose late_tolerance as what the device can wait plus
///   the real time one of its steps takes.
struct TimeControlOptions
{
    /// Step of the controller in the time synchronisation. The simulation can stand still only
    /// at multiples of it, and every step adds a round of synchronisation. Must be > 0.
    SimDuration step{std::chrono::milliseconds(10)};
    /// Stand still at T = 0 (after every participant's step at 0) until resume() or run_until().
    bool start_paused = false;
    /// Initial speed (simulation seconds per wall-clock second); empty: as fast as possible.
    std::optional<double> speed{};
    /// Pacing: a controller step that begins more than this after its deadline is late and makes
    /// the simulation slip (see above). Wall-clock time, whatever the speed and the step: small
    /// against the timeouts of a device, large against the jitter of the operating system's
    /// timers. Must not be negative.
    std::chrono::nanoseconds late_tolerance{std::chrono::milliseconds(10)};
};

/// Where the time gate stands, and how its pacing went (TimeControlOptions has the definitions).
/// See SimulationController::pause().
struct TimeState
{
    /// Start time of the controller's step that runs or ran last; SimTime{} before the first.
    SimTime now{};
    /// The simulation stands still at `now`: every synchronised participant has run (or is
    /// running) its steps that start at or before `now`, and runs none that starts after it.
    bool paused = false;
    /// Set by pause_at() or run_until(): the simulation stands still at the first controller
    /// step at or after it.
    std::optional<SimTime> pause_target{};
    std::optional<double> speed{};
    /// The controller steps that were measured against a deadline (a speed and a base were set),
    /// including one that is overdue now.
    std::uint64_t paced_steps = 0;
    /// The paced steps whose lateness was more than TimeControlOptions::late_tolerance: the number
    /// of slips.
    std::uint64_t late_steps = 0;
    /// The largest lateness of a paced step (also of one within the tolerance).
    std::chrono::nanoseconds max_lateness{0};
    /// The sum of the lateness of the late steps: how much later than at the speed asked for the
    /// simulation has come, in all (but for what is within the tolerance now).
    std::chrono::nanoseconds slipped{0};
    /// The lateness so far of the step that has not begun although its deadline has passed; 0 if
    /// there is none. It is counted in the four numbers above already.
    std::chrono::nanoseconds current_lateness{0};
    /// The largest time by which the gate itself let a paced step go after the step's deadline
    /// (the wake-up of its thread). It is part of the lateness of the step after; it tells the
    /// gate's timer from slow participants.
    std::chrono::nanoseconds max_gate_lateness{0};
    /// Wall-clock time the simulation has stood still (`paused`) in all, up to now.
    std::chrono::nanoseconds paused_for{0};
    /// Wall-clock time the simulation has run for: from the beginning of the first controller step
    /// to the beginning of the one at `now`, without the time it stood still; and on up to now
    /// while the next step is overdue (as fast as possible: whenever the gate holds nothing).
    /// It leaves out the start-up and the shutdown: now / running_for is the speed it ran at.
    std::chrono::nanoseconds running_for{0};
};

struct SimulationControllerOptions
{
    std::string participant_name = "SimulationController";
    std::string registry_uri = "silkit://localhost:8500";
    std::string participant_configuration = "{}";
    /// The participants (other than the controller) that must be present before the
    /// simulation starts. They must use LifecycleMode::Coordinated.
    std::vector<std::string> required_participants;
    /// If set, the controller joins the time synchronisation with a single step of this size
    /// and stops the simulation when virtual time reaches it. At that moment every other
    /// synchronised participant has finished all of its steps before `stop_at`. Must be > 0.
    std::optional<SimDuration> stop_at;
    /// If set, the controller joins the time synchronisation with TimeControlOptions::step and
    /// can pause, step and pace the simulation (pause(), run_until(), set_speed()). With stop_at
    /// as well, the controller stops at its first step at or after stop_at (instead of a step
    /// at stop_at itself).
    std::optional<TimeControlOptions> time_control;
    std::chrono::milliseconds shutdown_timeout{5000};
};

/// What a start window function is given (SimulationController::set_start_window()).
class StartWindow
{
public:
    /// When SimulationController::start() gives up: return before it.
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return deadline_; }
    /// true once going on is pointless: a stop was requested, the controller's lifecycle has
    /// ended, or start() has given up. Look at it in every wait and return soon once it is true:
    /// start() waits for the function.
    [[nodiscard]] bool cancelled() const;

private:
    friend class SimulationController;
    StartWindow(std::chrono::steady_clock::time_point deadline, std::function<bool()> cancelled) :
      deadline_(deadline), cancelled_(std::move(cancelled))
    {
    }
    std::chrono::steady_clock::time_point deadline_;
    std::function<bool()> cancelled_;
};

/// Starts and stops a coordinated SIL Kit simulation.
///
/// The controller is itself a coordinated participant: it sets the workflow configuration
/// (the required participants plus itself) once every required participant has connected,
/// which makes all of them start together at virtual time 0. Stopping the controller's
/// lifecycle (stop(), or reaching stop_at) stops every coordinated participant.
///
/// A required participant that fails (a step handler exception, a crash or a lost connection)
/// goes to ParticipantState::Error and possibly on to Shutdown. SIL Kit does not end the
/// simulation then: virtual time simply stops advancing for everyone. Poll
/// failed_participant(), or FailureCheck (failure_check.hpp) for the policy the tools share, and
/// abort() in that case (vehicle_sil_sim_controller does).
///
/// Only one system controller may run per simulation.
class SimulationController
{
public:
    /// Connects to the registry. Throws SilKit::SilKitError on failure, std::invalid_argument
    /// for invalid options.
    explicit SimulationController(SimulationControllerOptions options);
    ~SimulationController();

    SimulationController(const SimulationController&) = delete;
    SimulationController& operator=(const SimulationController&) = delete;
    SimulationController(SimulationController&&) = delete;
    SimulationController& operator=(SimulationController&&) = delete;

    /// Waits until every required participant has connected, publishes the workflow
    /// configuration and waits until the simulation is Running.
    /// Throws std::runtime_error on timeout (naming every required participant that has not
    /// connected). `give_up`: as in ParticipantRuntime::start(), asked while it waits for the
    /// participants to connect and for Running (also during the start window, whose function is
    /// then told to give up and waited for as on a timeout). `timeout` kNoStartLimit: no limit
    /// but `give_up` (the window's StartWindow::deadline() is then the time_point that never
    /// comes: do not add to it or wait until it).
    void start(std::chrono::milliseconds timeout, const std::function<std::string()>& give_up = {});

    /// Sets a function for the window before the start: start() runs it, on a thread of the
    /// controller's (not SIL Kit's, not the caller's), when every required participant and the
    /// controller wait before their first step with their publish/subscribe services connected
    /// (ParticipantRuntime::set_communication_ready_handler_async()), and the simulation starts
    /// when it has returned. What it sends and receives through services of runtime() made before
    /// start() is exchanged with every participant before anyone's step at 0: a write that a
    /// participant accepts there takes effect in its first step.
    ///
    /// The window is part of the start: start()'s timeout covers it (StartWindow::deadline()), the
    /// start() of every participant waits for it, and nothing interrupts the function but itself
    /// (StartWindow::cancelled()). If it throws, the simulation starts all the same and start()
    /// throws std::runtime_error with its message afterwards. It is not called if the window never
    /// opens before start() gives up (a required participant did not get that far); a window that
    /// opens afterwards is completed without it, so that the participants do not stay before their
    /// first step and a stop still ends them in order. start() waits for the function before it
    /// returns or throws (after its timeout: until the function has seen cancelled()), so the
    /// function never runs once start() is over.
    /// Once, before start() (std::logic_error; std::invalid_argument for an empty function).
    void set_start_window(std::function<void(const StartWindow& window)> window);

    /// The required participants that have not connected yet (no lifecycle state seen), in the
    /// order of SimulationControllerOptions::required_participants. Once it is empty, start()
    /// only publishes the workflow configuration and waits for Running. Thread safe.
    [[nodiscard]] std::vector<std::string> missing_participants() const;

    /// Waits until the controller's lifecycle has ended. @returns its final state or nullopt.
    std::optional<SilKit::Services::Orchestration::ParticipantState> wait_until_finished(std::chrono::milliseconds timeout);

    /// Stops the simulation (all coordinated participants) and waits for the shutdown.
    void stop(std::string_view reason) noexcept;

    /// Aborts the simulation, for example when a participant hangs or failed. Never throws.
    void abort() noexcept;

    /// While the controller is Running and has not been asked to stop: the first required
    /// participant that has left the simulation (Error, or a stopping or terminal state), if any.
    [[nodiscard]] std::optional<std::string> failed_participant() const;

    /// Wall-clock time between the controller's step at T = 0 and its step at stop_at, i.e. how
    /// long the simulation took to advance by stop_at (with time_control: including the time it
    /// stood still and was paced; TimeState::running_for leaves the pauses out). Empty until
    /// stop_at has been reached.
    [[nodiscard]] std::optional<std::chrono::nanoseconds> wall_time_to_stop_at() const;

    /// With time_control, the time gate owns the runtime's StepHold: do not call
    /// set_step_hold() or release_step() through it.
    [[nodiscard]] ParticipantRuntime& runtime() noexcept { return *runtime_; }
    [[nodiscard]] const std::vector<std::string>& required_participants() const noexcept
    {
        return options_.required_participants;
    }

    // Time gate (only with SimulationControllerOptions::time_control; otherwise each function
    // throws std::logic_error). The controller holds back the completion of one of its steps;
    // SIL Kit then lets no participant begin a step that starts after it (ParticipantRuntime::
    // set_step_hold()). The gate only decides where the simulation stands still and how fast it
    // goes: every participant runs the same steps with the same data as without it.
    // Only participants that take part in the time synchronisation are paused; participants in
    // real time (without virtual time) keep running; and the participants' steps between two
    // controller steps run in a burst, at the deadline of the later one (the gate paces per
    // controller step; TimeControlOptions, "Granularity"). The functions are
    // thread safe and do not wait, so they may also be called from a step of another
    // participant of the same process; wait_until_paused() waits and must not be (the
    // simulation cannot reach the pause while that step runs).

    /// Stands still at the controller step that is being held for pacing, or else at its next
    /// step.
    void pause();
    /// Stands still at the first controller step at or after `t` (at the next one if `t` has
    /// passed). Replaces an earlier target.
    void pause_at(SimTime t);
    /// Runs on (clears the pause target as well).
    void resume();
    /// Runs until the first controller step at or after `t`, then stands still there. If the
    /// simulation stands still at or after `t`, it stays there.
    void run_until(SimTime t);
    /// run_until(now + d), with `now` of time_state() read under the same lock. d > 0
    /// (std::invalid_argument).
    void step(SimDuration d);
    /// Simulation seconds per wall-clock second within [RealTimePacer::kMinFactor, kMaxFactor]
    /// (std::invalid_argument), or empty for as fast as possible. Like
    /// VirtualTimeOptions::real_time_factor, it only holds steps back: it cannot make the
    /// slowest participant faster. Setting the same speed again has no effect; a step held for
    /// pacing keeps the part of its wait that has passed; between two steps the next one keeps its
    /// deadline (and the lateness it has), and the new speed counts from there. Pacing starts
    /// anew on resume.
    void set_speed(std::optional<double> speed);
    /// `paused` is false once the lifecycle has ended. The pacing numbers are as if the run ended
    /// now (they include a step that is overdue), and stay as they were when a stop was requested
    /// or the lifecycle left Running.
    [[nodiscard]] TimeState time_state() const;
    /// Waits until the simulation stands still, or the lifecycle ended, or the timeout expires.
    /// @returns the time at which it stands still, or nullopt (also once the lifecycle ended).
    std::optional<SimTime> wait_until_paused(std::chrono::milliseconds timeout);

private:
    struct Gate;
    struct Window;
    [[nodiscard]] Gate& gate() const;
    /// The thread of the start window: waits for the window, runs the function, completes.
    void run_window(std::chrono::steady_clock::time_point deadline);
    /// Tells the window's function to give up and waits for it (not for the thread).
    void cancel_window() noexcept;
    /// Ends the window's thread (the controller goes).
    void end_window() noexcept;
    /// The StepHold of the runtime (SIL Kit thread).
    bool hold_step(SimTime now, SimDuration step);
    void run_gate();
    /// The pacing numbers stop here (a stop was requested, or the lifecycle left Running).
    void end_gate() noexcept;

    SimulationControllerOptions options_;
    /// Only with time_control. Before runtime_, so that it outlives it: the runtime's handlers
    /// (the step, the hold, the stop observer) use it.
    std::unique_ptr<Gate> gate_;
    /// Only with set_start_window(). Before runtime_ as well: the runtime's handler uses it.
    std::unique_ptr<Window> window_;
    std::unique_ptr<ParticipantRuntime> runtime_;
    SilKit::Experimental::Services::Orchestration::ISystemController* system_controller_ = nullptr;
    std::atomic<std::int64_t> wall_start_ns_{-1};
    std::atomic<std::int64_t> wall_stop_ns_{-1};
};

} // namespace vehicle_sil
