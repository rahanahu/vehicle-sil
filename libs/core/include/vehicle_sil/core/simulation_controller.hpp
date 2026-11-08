// SPDX-License-Identifier: MIT
//
// System controller for a coordinated, virtual-time simulation (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/participant_runtime.hpp"
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
    /// long the simulation took to advance by stop_at. Empty until stop_at has been reached.
    [[nodiscard]] std::optional<std::chrono::nanoseconds> wall_time_to_stop_at() const;

    [[nodiscard]] ParticipantRuntime& runtime() noexcept { return *runtime_; }

private:
    SimulationControllerOptions options_;
    std::unique_ptr<ParticipantRuntime> runtime_;
    SilKit::Experimental::Services::Orchestration::ISystemController* system_controller_ = nullptr;
    std::atomic<std::int64_t> wall_start_ns_{-1};
    std::atomic<std::int64_t> wall_stop_ns_{-1};
};

} // namespace vehicle_sil
