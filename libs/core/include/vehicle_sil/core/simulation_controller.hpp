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
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SilKit::Experimental::Services::Orchestration {
class ISystemController;
} // namespace SilKit::Experimental::Services::Orchestration

namespace vehicle_sil {

/// Pausing, stepping and speed control of a coordinated virtual-time simulation (the time gate).
struct TimeControlOptions
{
    /// Step of the controller in the time synchronisation. The simulation can stand still only
    /// at multiples of it, and every step adds a round of synchronisation. Must be > 0.
    SimDuration step{std::chrono::milliseconds(10)};
    /// Stand still at T = 0 (after every participant's step at 0) until resume() or run_until().
    bool start_paused = false;
    /// Initial speed (simulation seconds per wall-clock second); empty: as fast as possible.
    std::optional<double> speed{};
};

/// Where the time gate stands. See SimulationController::pause().
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
/// failed_participant() and abort() in that case (vehicle_sil_sim_controller does).
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
    /// Throws std::runtime_error on timeout.
    void start(std::chrono::milliseconds timeout);

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
    /// stood still and was paced). Empty until stop_at has been reached.
    [[nodiscard]] std::optional<std::chrono::nanoseconds> wall_time_to_stop_at() const;

    /// With time_control, the time gate owns the runtime's StepHold: do not call
    /// set_step_hold() or release_step() through it.
    [[nodiscard]] ParticipantRuntime& runtime() noexcept { return *runtime_; }

    // Time gate (only with SimulationControllerOptions::time_control; otherwise each function
    // throws std::logic_error). The controller holds back the completion of one of its steps;
    // SIL Kit then lets no participant begin a step that starts after it (ParticipantRuntime::
    // set_step_hold()). The gate only decides where the simulation stands still and how fast it
    // goes: every participant runs the same steps with the same data as without it.
    // Only participants that take part in the time synchronisation are paused; participants in
    // real time (without virtual time) keep running; and the participants' steps between two
    // controller steps run in a burst (the gate paces per controller step). The functions are
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
    /// pacing keeps the part of its wait that has passed. Pacing starts anew on resume.
    void set_speed(std::optional<double> speed);
    /// `paused` is false once the lifecycle has ended.
    [[nodiscard]] TimeState time_state() const;
    /// Waits until the simulation stands still, or the lifecycle ended, or the timeout expires.
    /// @returns the time at which it stands still, or nullopt (also once the lifecycle ended).
    std::optional<SimTime> wait_until_paused(std::chrono::milliseconds timeout);

private:
    struct Gate;
    [[nodiscard]] Gate& gate() const;
    /// The StepHold of the runtime (SIL Kit thread).
    bool hold_step(SimTime now, SimDuration step);
    void run_gate();

    SimulationControllerOptions options_;
    std::unique_ptr<ParticipantRuntime> runtime_;
    SilKit::Experimental::Services::Orchestration::ISystemController* system_controller_ = nullptr;
    std::atomic<std::int64_t> wall_start_ns_{-1};
    std::atomic<std::int64_t> wall_stop_ns_{-1};
    std::unique_ptr<Gate> gate_; ///< only with time_control
};

} // namespace vehicle_sil
