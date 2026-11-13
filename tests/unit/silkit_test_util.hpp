// SPDX-License-Identifier: MIT
//
// In-process SIL Kit helpers shared by the tests that run real SIL Kit participants.
// Requires linking vehicle_sil::core.
#pragma once

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/services/orchestration/ISystemMonitor.hpp>
#include <silkit/services/orchestration/string_utils.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::unit {

/// A SIL Kit registry listening on a free local port for the lifetime of the object.
struct InProcessRegistry
{
    std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry =
        SilKit::Vendor::Vector::CreateSilKitRegistry(
            SilKit::Config::ParticipantConfigurationFromString(std::string(vehicle_sil::default_registry_configuration())));
    std::string uri = registry->StartListening("silkit://127.0.0.1:0");
};

/// The states one participant goes through, in order, as another participant sees them (a plain
/// SIL Kit participant outside the simulation, with a system monitor).
class StateLog
{
public:
    using State = SilKit::Services::Orchestration::ParticipantState;

    StateLog(const std::string& uri, std::string watched, const std::string& own_name = "Watcher") :
      watched_(std::move(watched))
    {
        participant_ = SilKit::CreateParticipant(
            SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(uri)), own_name, uri);
        participant_->CreateSystemMonitor()->AddParticipantStatusHandler(
            [this](const SilKit::Services::Orchestration::ParticipantStatus& status) {
                if (status.participantName == watched_)
                {
                    const std::lock_guard lock(mutex_);
                    states_.push_back(status.state);
                    entered_.push_back(status.enterTime);
                }
            });
    }

    [[nodiscard]] std::vector<State> states() const
    {
        const std::lock_guard lock(mutex_);
        return states_;
    }

    [[nodiscard]] bool reached(State state) const
    {
        const auto s = states();
        return std::find(s.begin(), s.end(), state) != s.end();
    }

    /// When the watched participant entered `state` for the first time, by its own clock.
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> entered(State state) const
    {
        const std::lock_guard lock(mutex_);
        const auto it = std::find(states_.begin(), states_.end(), state);
        if (it == states_.end())
        {
            return std::nullopt;
        }
        return entered_[static_cast<std::size_t>(it - states_.begin())];
    }

    /// The state that followed Running, if any.
    [[nodiscard]] std::optional<State> after_running() const
    {
        const auto s = states();
        auto it = std::find(s.begin(), s.end(), State::Running);
        if (it == s.end() || ++it == s.end())
        {
            return std::nullopt;
        }
        return *it;
    }

    [[nodiscard]] std::string text() const
    {
        std::string out;
        for (const auto state : states())
        {
            out += (out.empty() ? "" : " ") + SilKit::Services::Orchestration::to_string(state);
        }
        return out;
    }

private:
    std::string watched_;
    mutable std::mutex mutex_;
    std::vector<State> states_;
    std::vector<std::chrono::system_clock::time_point> entered_; // as states_
    std::unique_ptr<SilKit::IParticipant> participant_; // last: destroyed first, with the handler
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

/// Keeps this thread busy for `busy` of wall time, as a model that computes would: a slow
/// participant of a test. Not a sleep: a sleep lasts a whole timer tick on Windows (about 15.6 ms)
/// however short it is asked to be, which made such a participant ten times slower there.
inline void busy_for(std::chrono::microseconds busy)
{
    const auto end = std::chrono::steady_clock::now() + busy;
    while (std::chrono::steady_clock::now() < end)
    {
    }
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
