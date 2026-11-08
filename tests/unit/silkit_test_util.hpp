// SPDX-License-Identifier: MIT
//
// In-process SIL Kit helpers shared by the tests that run real SIL Kit participants.
// Requires linking vehicle_sil::core.
#pragma once

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::unit {

/// A SIL Kit registry listening on a free local port for the lifetime of the object.
struct InProcessRegistry
{
    std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry =
        SilKit::Vendor::Vector::CreateSilKitRegistry(SilKit::Config::ParticipantConfigurationFromString("{}"));
    std::string uri = registry->StartListening("silkit://127.0.0.1:0");
};

/// Options of a participant whose lifecycle a SimulationController coordinates.
[[nodiscard]] inline ParticipantRuntimeOptions coordinated_options(std::string name, std::string uri,
                                                                   VirtualTimeOptions virtualTime)
{
    ParticipantRuntimeOptions options;
    options.participant_name = std::move(name);
    options.registry_uri = std::move(uri);
    options.lifecycle_mode = LifecycleMode::Coordinated;
    options.virtual_time = virtualTime;
    return options;
}

/// Starts every runtime from its own thread, the way separate processes would, then the
/// controller, and waits until every start() has returned.
inline void start_together(SimulationController& controller, const std::vector<ParticipantRuntime*>& runtimes,
                           std::chrono::milliseconds timeout)
{
    std::vector<std::thread> starters;
    starters.reserve(runtimes.size());
    for (auto* runtime : runtimes)
    {
        starters.emplace_back([runtime, timeout] { runtime->start(timeout); });
    }
    controller.start(timeout);
    for (auto& starter : starters)
    {
        starter.join();
    }
}

} // namespace vehicle_sil::unit
