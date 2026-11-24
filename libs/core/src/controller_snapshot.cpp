// SPDX-License-Identifier: MIT
//
// Snapshot extraction from SimulationController (vehicle_sil::core).
#include "vehicle_sil/core/simulation_controller.hpp"

#include "vehicle_sil/core/failure_check.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>

namespace vehicle_sil {

TimeView time_view_of(SimulationController& controller, bool holding, bool ended)
{
    TimeView v;
    v.available = true;
    const TimeState s = controller.time_state();
    v.speed = s.speed;
    if (ended)
    {
        // Final view as the controller detaches: where it ended, as the result's sim_ms counts it,
        // and no pause target remains.
        v.now = std::max(s.now, controller.runtime().now());
        v.ended = true;
        return v;
    }
    const auto state = controller.runtime().state();
    const bool finished = controller.runtime().started() && has_finished(PeerStatus{state});
    if (holding && !finished)
    {
        // Before the first step: it stands still at 0, and has not started.
        v.paused = true;
        return v;
    }
    v.now = holding ? SimTime{} : s.now;
    v.pause_target = s.pause_target;
    // One answer from two looks: a simulation that ended between them stands still nowhere (as
    // time_state() says of one that had ended before).
    v.paused = s.paused && !finished;
    v.running = !finished && state == SilKit::Services::Orchestration::ParticipantState::Running;
    v.ended = finished;
    if (finished)
    {
        // Where it ended, as the result's sim_ms counts it, and no pause target remains.
        v.now = std::max(v.now, controller.runtime().now());
        v.pause_target.reset();
    }
    return v;
}

std::vector<std::pair<std::string, std::string>> simulation_participants_of(SimulationController& controller)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& name : controller.required_participants())
    {
        const auto status = controller.runtime().peer_status(name);
        out.emplace_back(name, status.state ? SilKit::Services::Orchestration::to_string(*status.state) : std::string());
    }
    return out;
}

std::optional<LeftParticipant> left_participant_of(SimulationController& controller)
{
    return FailureCheck::left_participant(controller);
}

void refresh_peer_statuses(SimulationController& controller)
{
    controller.runtime().refresh_peer_statuses();
}

ControllerSnapshot controller_snapshot_of(SimulationController& controller, bool holding, bool ended)
{
    ControllerSnapshot snapshot;
    snapshot.time = time_view_of(controller, holding, ended);
    snapshot.participants = simulation_participants_of(controller);
    snapshot.left_participant = left_participant_of(controller);
    return snapshot;
}

} // namespace vehicle_sil
