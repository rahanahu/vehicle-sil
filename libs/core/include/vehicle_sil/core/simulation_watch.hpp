// SPDX-License-Identifier: MIT
//
// Watches a running simulation from its controller until it ends: the loop that every tool with
// a SimulationController shares (vehicle_sil::Simulation, vehicle_sil_sim_controller,
// vehicle_sil_launch) (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/failure_check.hpp"

#include <silkit/services/orchestration/OrchestrationDatatypes.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vehicle_sil {

/// What watch_simulation() needs of a controller (SimulationController has all of it; tests
/// give their own).
struct WatchTarget
{
    /// SimulationController::wait_until_finished().
    std::function<std::optional<SilKit::Services::Orchestration::ParticipantState>(std::chrono::milliseconds)> wait_until_finished;
    /// FailureCheck::left_participant().
    std::function<std::optional<LeftParticipant>()> left_participant;
    /// SimulationController::stop(): ends the simulation in order, and waits for it.
    std::function<void(std::string_view reason)> stop;
    /// SimulationController::abort().
    std::function<void()> abort;
};

struct WatchResult;

struct WatchOptions
{
    /// How long one wait for the end lasts before the checks run again.
    std::chrono::milliseconds poll{50};
    /// Checked after each wait: true stops the simulation in order (WatchEnd::Interrupted).
    std::function<bool()> interrupted{};
    /// Checked after each wait: true if the caller has seen a failure of its own (one of its
    /// participants failed, a process ended): the simulation is aborted (WatchEnd::Failed, without
    /// WatchResult::failure).
    std::function<bool()> failed{};
    /// Wall clock: the simulation is aborted if it has not ended by then (WatchEnd::TimedOut).
    std::optional<std::chrono::steady_clock::time_point> deadline{};
    /// FailureCheck's grace for a participant that left in a state other than Error.
    std::chrono::steady_clock::duration leave_grace = FailureCheck::kDefaultGrace;
    /// After the stop of an interrupted simulation: how long to wait for its end.
    std::chrono::milliseconds stop_wait{5000};
    /// The clock of the failure check and the deadline; empty: std::chrono::steady_clock::now.
    std::function<std::chrono::steady_clock::time_point()> clock{};
    /// The reason given to WatchTarget::stop() when interrupted (other participants may log it).
    std::string stop_reason = "interrupted";
    /// Called before the simulation is aborted (Failed, TimedOut), with what watch_simulation()
    /// will return: to report why while the participants are still there.
    std::function<void(const WatchResult&)> before_abort{};
};

/// How watch_simulation() returned.
enum class WatchEnd
{
    Ended,       ///< the controller's lifecycle ended (WatchResult::state: in which state)
    Failed,      ///< a participant failed (FailureCheck, or WatchOptions::failed); aborted
    Interrupted, ///< WatchOptions::interrupted; stopped in order
    TimedOut,    ///< WatchOptions::deadline passed; aborted
};

struct WatchResult
{
    WatchEnd end = WatchEnd::Ended;
    /// The controller's final state: always for Ended, for Interrupted if the simulation ended
    /// within WatchOptions::stop_wait.
    std::optional<SilKit::Services::Orchestration::ParticipantState> state{};
    /// Failed by FailureCheck: who and why. Empty when WatchOptions::failed said so.
    std::optional<ParticipantFailure> failure{};
};

/// Waits until the running simulation ends. After each wait of WatchOptions::poll it checks, in
/// this order: interrupted (then stops the simulation in order and waits up to stop_wait), the
/// caller's failed, FailureCheck (a required participant left: in Error at once, otherwise after
/// leave_grace; then aborts), the deadline (then aborts). Call it once the simulation runs
/// (SimulationController::start() returned).
[[nodiscard]] WatchResult watch_simulation(const WatchTarget& target, const WatchOptions& options = {});

class SimulationController;

/// watch_simulation() of `controller`.
[[nodiscard]] WatchResult watch_simulation(SimulationController& controller, const WatchOptions& options = {});

} // namespace vehicle_sil
