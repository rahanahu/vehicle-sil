// SPDX-License-Identifier: MIT
//
// Snapshot value types extracted from a SimulationController without SIL Kit (vehicle_sil::sim_base).
// Header only and independent of SIL Kit, so layers that observe a simulation's time and participants
// (runner public headers, launch, control_grpc public headers) need neither vehicle_sil::core nor SIL Kit.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil {

/// Default grace period for FailureCheck when a required participant leaves in a state other than Error.
inline constexpr std::chrono::milliseconds kDefaultLeaveGrace{5000};

/// Where the simulation time stands, as reported outside the controller.
struct TimeView
{
    bool available = false; ///< false: no time control is attached
    SimTime now{};
    bool paused = false;
    std::optional<SimTime> pause_target{};
    std::optional<double> speed{};
    bool running = false;
    bool ended = false; ///< the simulation ran and is over (it will not stand still again)

    bool operator==(const TimeView&) const = default;
};

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

/// A complete snapshot of a SimulationController's observable time, required participants and
/// failure state at one moment.
struct ControllerSnapshot
{
    TimeView time{};
    std::vector<std::pair<std::string, std::string>> participants{};
    std::optional<LeftParticipant> left_participant{};
};

} // namespace vehicle_sil
