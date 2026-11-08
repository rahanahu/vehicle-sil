// SPDX-License-Identifier: MIT
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/experimental/participant/ParticipantExtensions.hpp>
#include <silkit/experimental/services/orchestration/ISystemController.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil {
namespace {

namespace orch = SilKit::Services::Orchestration;

std::int64_t wall_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool has_any_state(const PeerStatus& status) noexcept
{
    return status.state.has_value();
}

} // namespace

SimulationController::SimulationController(SimulationControllerOptions options) : options_(std::move(options))
{
    if (options_.required_participants.empty())
    {
        throw std::invalid_argument("SimulationController needs at least one required participant");
    }
    if (std::find(options_.required_participants.begin(), options_.required_participants.end(),
                  options_.participant_name) != options_.required_participants.end())
    {
        throw std::invalid_argument("SimulationController must not list itself as a required participant");
    }
    if (options_.stop_at && *options_.stop_at <= SimDuration::zero())
    {
        throw std::invalid_argument("SimulationController stop_at must be > 0");
    }

    ParticipantRuntimeOptions runtimeOptions;
    runtimeOptions.participant_name = options_.participant_name;
    runtimeOptions.registry_uri = options_.registry_uri;
    runtimeOptions.participant_configuration = options_.participant_configuration;
    runtimeOptions.shutdown_timeout = options_.shutdown_timeout;
    runtimeOptions.lifecycle_mode = LifecycleMode::Coordinated;
    if (options_.stop_at)
    {
        // One step of size stop_at: steps at 0 and at stop_at only, so the controller never
        // holds back the other participants before stop_at.
        runtimeOptions.virtual_time = VirtualTimeOptions{*options_.stop_at, std::nullopt};
    }
    runtime_ = std::make_unique<ParticipantRuntime>(std::move(runtimeOptions));
    system_controller_ = SilKit::Experimental::Participant::CreateSystemController(&runtime_->participant());

    if (options_.stop_at)
    {
        runtime_->set_step_handler([this](SimTime now, SimDuration) {
            if (now == SimTime{})
            {
                wall_start_ns_.store(wall_ns(), std::memory_order_release);
            }
            if (now.time_since_epoch() >= *options_.stop_at && wall_stop_ns_.load(std::memory_order_acquire) < 0)
            {
                wall_stop_ns_.store(wall_ns(), std::memory_order_release);
                runtime_->request_stop("SimulationController: stop_at reached");
            }
        });
    }
}

SimulationController::~SimulationController()
{
    // Stop first: the step handler uses the members below, which are destroyed before runtime_.
    runtime_->stop("SimulationController destroyed");
}

void SimulationController::start(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&] {
        return std::max(std::chrono::milliseconds(0), std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          deadline - std::chrono::steady_clock::now()));
    };

    // A participant has connected once its first lifecycle status (ServicesCreated) is known.
    // The workflow configuration is published only then, as sil-kit-system-controller does,
    // so that every required participant receives it.
    for (const auto& name : options_.required_participants)
    {
        if (!runtime_->wait_for_peer(name, has_any_state, remaining()))
        {
            throw std::runtime_error("SimulationController: required participant '" + name + "' did not connect within " +
                                     std::to_string(timeout.count()) + " ms");
        }
    }

    std::vector<std::string> required = options_.required_participants;
    required.push_back(options_.participant_name);
    system_controller_->SetWorkflowConfiguration(orch::WorkflowConfiguration{required});

    runtime_->start(remaining());
}

std::optional<orch::ParticipantState> SimulationController::wait_until_finished(std::chrono::milliseconds timeout)
{
    return runtime_->wait_until_finished(timeout);
}

void SimulationController::stop(std::string_view reason) noexcept
{
    runtime_->stop(reason);
}

void SimulationController::abort() noexcept
{
    try
    {
        system_controller_->AbortSimulation();
    }
    catch (const std::exception&)
    {
        // Best effort: destroying the participant disconnects it anyway.
    }
}

std::optional<std::string> SimulationController::failed_participant() const
{
    // Once the controller stops the simulation, the others leave on purpose.
    if (runtime_->stop_requested() || runtime_->state() != orch::ParticipantState::Running)
    {
        return std::nullopt;
    }
    for (const auto& name : options_.required_participants)
    {
        // Error, or already stopped and shut down (a failed peer may get there quickly).
        if (has_finished(runtime_->peer_status(name)))
        {
            return name;
        }
    }
    return std::nullopt;
}

std::optional<std::chrono::nanoseconds> SimulationController::wall_time_to_stop_at() const
{
    const auto start = wall_start_ns_.load(std::memory_order_acquire);
    const auto stop = wall_stop_ns_.load(std::memory_order_acquire);
    if (start < 0 || stop < 0)
    {
        return std::nullopt;
    }
    return std::chrono::nanoseconds(stop - start);
}

} // namespace vehicle_sil
