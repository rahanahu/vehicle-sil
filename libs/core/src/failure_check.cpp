// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/failure_check.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <stdexcept>

namespace vehicle_sil {

namespace {

std::string duration_text(std::chrono::steady_clock::duration d)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    return ms % 1000 == 0 ? std::to_string(ms / 1000) + " s" : std::to_string(ms) + " ms";
}

} // namespace

FailureCheck::FailureCheck(std::chrono::steady_clock::duration grace) : grace_(grace)
{
    if (grace < std::chrono::steady_clock::duration::zero())
    {
        throw std::invalid_argument("vehicle_sil::FailureCheck: the grace must not be negative");
    }
}

std::optional<ParticipantFailure> FailureCheck::check(SimulationController& controller, std::chrono::steady_clock::time_point now)
{
    return check(left_participant(controller), now);
}

std::optional<ParticipantFailure> FailureCheck::check(const std::optional<LeftParticipant>& left, std::chrono::steady_clock::time_point now)
{
    if (!left)
    {
        left_since_.reset();
        return std::nullopt;
    }
    if (!left_since_)
    {
        left_since_ = now;
    }
    if (left->error)
    {
        return ParticipantFailure{left->name, left->state,
                                  "left the simulation (state " + left->state
                                      + "): it failed or lost its connection; see its log for the cause"};
    }
    if (now - *left_since_ >= grace_)
    {
        return ParticipantFailure{left->name, left->state,
                                  "left the simulation (state " + left->state + ") and the simulation did not end within "
                                      + duration_text(grace_) + "; see its log for why it stopped"};
    }
    return std::nullopt;
}

std::optional<LeftParticipant> FailureCheck::left_participant(SimulationController& controller)
{
    const auto name = controller.failed_participant();
    if (!name)
    {
        return std::nullopt;
    }
    const auto state = controller.runtime().peer_status(*name).state;
    LeftParticipant out;
    out.name = *name;
    out.state = state ? SilKit::Services::Orchestration::to_string(*state) : std::string("unknown");
    out.error = state == SilKit::Services::Orchestration::ParticipantState::Error;
    return out;
}

} // namespace vehicle_sil
