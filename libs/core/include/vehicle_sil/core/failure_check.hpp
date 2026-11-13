// SPDX-License-Identifier: MIT
//
// When a participant that left a running simulation has failed it (vehicle_sil::core).
#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace vehicle_sil {

class SimulationController;

/// A required participant seen to have left the running simulation
/// (SimulationController::failed_participant()).
struct LeftParticipant
{
    std::string name;
    /// Its SIL Kit lifecycle state as text ("unknown" if none was seen).
    std::string state;
    /// The state is Error: it failed (a step handler exception, a crash) or lost its connection.
    bool error = false;
};

/// A participant that failed the simulation.
struct ParticipantFailure
{
    std::string participant;
    std::string state;
    /// Why, without the name: "left the simulation (state ...) ...".
    std::string reason;
};

/// Decides when a participant that left a running simulation has failed it, the same way for
/// every tool that watches a simulation (vehicle_sil::Simulation, vehicle_sil_sim_controller,
/// the control server's SimulationMonitor).
///
/// A participant in Error fails the simulation at once. A participant that left in another state
/// (it stopped) usually ends the simulation a moment later: its stop reaches the controller after
/// its state does. It fails the simulation only if some participant has been seen gone for
/// `grace` without a break; a check that sees none starts the wait anew.
///
/// It has no thread and is not thread safe: one caller checks it, typically in a polling loop.
class FailureCheck
{
public:
    static constexpr std::chrono::milliseconds kDefaultGrace{5000};

    /// `grace` >= 0 (std::invalid_argument).
    explicit FailureCheck(std::chrono::steady_clock::duration grace = kDefaultGrace);

    /// Checks what `controller` sees now (left_participant()).
    [[nodiscard]] std::optional<ParticipantFailure> check(SimulationController& controller, std::chrono::steady_clock::time_point now);
    /// Checks an observation: the participant that has left, if any, at `now` (not earlier than
    /// the `now` of the previous call).
    [[nodiscard]] std::optional<ParticipantFailure> check(const std::optional<LeftParticipant>& left,
                                                          std::chrono::steady_clock::time_point now);

    /// The first required participant that has left the running simulation, with its state.
    [[nodiscard]] static std::optional<LeftParticipant> left_participant(SimulationController& controller);

    [[nodiscard]] std::chrono::steady_clock::duration grace() const noexcept { return grace_; }

private:
    std::chrono::steady_clock::duration grace_;
    std::optional<std::chrono::steady_clock::time_point> left_since_;
};

} // namespace vehicle_sil
