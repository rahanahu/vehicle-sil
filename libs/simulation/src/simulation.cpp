// SPDX-License-Identifier: MIT
#include "vehicle_sil/simulation/simulation.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/local/local_bench.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <silkit/services/orchestration/string_utils.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace vehicle_sil {

namespace orch = SilKit::Services::Orchestration;

namespace {

constexpr const char* kWho = "vehicle_sil::Simulation: ";
constexpr const char* kControllerName = "SimulationController";
/// How often a run looks for a failed participant while it waits for the end.
constexpr std::chrono::milliseconds kPoll{50};
/// How long a participant that left the simulation without an error may take to end it (its
/// stop reaches the controller a moment after its state does).
constexpr std::chrono::seconds kLeaveGrace{5};
/// How long the participants of this process may take to end once the simulation ended.
constexpr std::chrono::seconds kEndWait{10};

std::string join(const std::vector<std::string>& parts)
{
    std::string out;
    for (const auto& p : parts)
    {
        out += (out.empty() ? "" : "; ") + p;
    }
    return out;
}

std::string state_text(const std::optional<orch::ParticipantState>& state)
{
    return state ? orch::to_string(*state) : std::string("unknown");
}

} // namespace

struct Simulation::LocalRun
{
    LocalBench can;
};

struct Simulation::SilKitRun
{
    // Destroyed bottom-up: the peers (the one added last first, see the destructor), the
    // controller, the registry.
    std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry;
    SilKitOptions options;
    std::string uri;
    std::unique_ptr<SimulationController> controller;
    std::vector<std::string> names; ///< of the ECUs, in the order added
    std::vector<std::unique_ptr<Peer>> peers;
    SimTime now{};
    bool ran = false;

    SilKitRun() = default;
    SilKitRun(const SilKitRun&) = delete;
    SilKitRun& operator=(const SilKitRun&) = delete;
    ~SilKitRun()
    {
        while (!peers.empty())
        {
            peers.pop_back(); // Peer::~Peer() stops it
        }
        controller.reset();
    }

    [[nodiscard]] bool name_is_taken(const std::string& name) const
    {
        return std::find(names.begin(), names.end(), name) != names.end()
               || std::find(options.other_participants.begin(), options.other_participants.end(), name)
                      != options.other_participants.end()
               || name == kControllerName;
    }

    [[nodiscard]] bool any_peer_failed() const
    {
        return std::any_of(peers.begin(), peers.end(), [](const auto& p) { return p->failed(); });
    }

    [[nodiscard]] std::vector<Peer*> peer_pointers() const
    {
        std::vector<Peer*> out;
        out.reserve(peers.size());
        for (const auto& p : peers)
        {
            out.push_back(p.get());
        }
        return out;
    }

    void stop_all(std::string_view reason) noexcept
    {
        for (auto& p : peers)
        {
            p->stop(reason);
        }
        if (controller)
        {
            controller->stop(reason);
        }
    }

    void run(std::optional<SimDuration> stop_at);

private:
    /// Waits for the end of the simulation of this process's controller; on a failure, aborts
    /// it. Returns why participants other than the ECUs (or the controller) failed.
    std::vector<std::string> watch_with_controller(bool& failure);
    /// Waits until every ECU ended, or one failed.
    void watch_without_controller(std::vector<std::optional<orch::ParticipantState>>& states, bool& failure);
};

void Simulation::SilKitRun::run(std::optional<SimDuration> stop_at)
{
    const std::vector<Peer*> list = peer_pointers();
    if (stop_at)
    {
        // The steps at or after the end do nothing, however late the controller's stop
        // reaches a participant.
        for (auto* p : list)
        {
            detail::set_virtual_time_end(*p, SimTime{} + *stop_at);
        }
    }
    if (options.controller)
    {
        SimulationControllerOptions c;
        c.participant_name = kControllerName;
        c.registry_uri = uri;
        c.required_participants = names;
        c.required_participants.insert(c.required_participants.end(), options.other_participants.begin(),
                                       options.other_participants.end());
        c.stop_at = stop_at;
        controller = std::make_unique<SimulationController>(std::move(c));
    }
    try
    {
        if (controller)
        {
            start_together(*controller, list, options.start_timeout);
        }
        else
        {
            start_together(list, options.start_timeout);
        }
    }
    catch (...)
    {
        stop_all("vehicle_sil::Simulation: the start failed");
        throw;
    }

    bool failure = false;
    std::vector<std::string> others;
    std::vector<std::optional<orch::ParticipantState>> states(peers.size());
    if (controller)
    {
        others = watch_with_controller(failure);
        if (!failure)
        {
            // The simulation ended: every participant is on its way out.
            const auto deadline = std::chrono::steady_clock::now() + kEndWait;
            for (std::size_t i = 0; i < peers.size(); ++i)
            {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                states[i] = peers[i]->wait(std::max(left, std::chrono::milliseconds::zero()));
            }
        }
    }
    else
    {
        watch_without_controller(states, failure);
    }
    // Without a controller, stopping a participant of a failed run ends the simulation for
    // the others too (the Coordinated stop).
    stop_all("vehicle_sil::Simulation: the run ended");

    std::vector<std::string> failures;
    for (std::size_t i = 0; i < peers.size(); ++i)
    {
        if (peers[i]->failed())
        {
            failures.push_back(names[i] + ": " + peers[i]->error());
        }
    }
    failures.insert(failures.end(), others.begin(), others.end());
    if (failures.empty() && !failure)
    {
        for (std::size_t i = 0; i < peers.size(); ++i)
        {
            if (states[i] != orch::ParticipantState::Shutdown)
            {
                failures.push_back(names[i] + ": did not end the simulation normally (state " + state_text(states[i]) + ")");
            }
        }
    }
    if (failure && failures.empty())
    {
        failures.emplace_back("the simulation failed");
    }
    if (!failures.empty())
    {
        throw SimulationError(join(failures));
    }
}

std::vector<std::string> Simulation::SilKitRun::watch_with_controller(bool& failure)
{
    std::vector<std::string> others;
    std::optional<std::chrono::steady_clock::time_point> left_at;
    std::optional<orch::ParticipantState> end;
    // SIL Kit does not end the simulation when a participant fails: virtual time only stops.
    while (!(end = controller->wait_until_finished(kPoll)))
    {
        if (any_peer_failed())
        {
            failure = true;
            break;
        }
        const auto name = controller->failed_participant();
        if (!name)
        {
            left_at.reset();
            continue;
        }
        const auto state = controller->runtime().peer_status(*name).state;
        const auto seen = std::chrono::steady_clock::now();
        if (!left_at)
        {
            left_at = seen;
        }
        // In Error: failed (or its connection was lost). Otherwise it stopped, which ends the
        // simulation a moment later, unless it did not stop the simulation with it.
        if (state == orch::ParticipantState::Error || seen - *left_at >= kLeaveGrace)
        {
            failure = true;
            const auto own = std::find(names.begin(), names.end(), *name);
            if (own == names.end() || !peers[static_cast<std::size_t>(own - names.begin())]->failed())
            {
                others.push_back(*name + ": left the simulation (state " + state_text(state) + ")");
            }
            break;
        }
    }
    if (failure)
    {
        controller->abort();
    }
    else if (end != orch::ParticipantState::Shutdown)
    {
        failure = true;
        others.push_back(std::string(kControllerName) + ": ended in state " + state_text(end));
    }
    return others;
}

void Simulation::SilKitRun::watch_without_controller(std::vector<std::optional<orch::ParticipantState>>& states, bool& failure)
{
    std::vector<bool> ended(peers.size(), false);
    for (;;)
    {
        if (any_peer_failed())
        {
            failure = true;
            return;
        }
        const auto next = std::find(ended.begin(), ended.end(), false);
        if (next == ended.end())
        {
            return;
        }
        const auto i = static_cast<std::size_t>(next - ended.begin());
        states[i] = peers[i]->wait(kPoll);
        ended[i] = states[i].has_value();
    }
}

Simulation::Simulation() : local_(std::make_unique<LocalRun>()) {}

Simulation::Simulation(SilKitOptions options) : silkit_(std::make_unique<SilKitRun>())
{
    if (!options.controller && !options.other_participants.empty())
    {
        throw std::invalid_argument(std::string(kWho) + "other_participants need a controller of this process");
    }
    for (std::size_t i = 0; i < options.other_participants.size(); ++i)
    {
        const auto& name = options.other_participants[i];
        if (name.empty() || name == kControllerName
            || std::find(options.other_participants.begin(), options.other_participants.begin() + static_cast<std::ptrdiff_t>(i), name)
                   != options.other_participants.begin() + static_cast<std::ptrdiff_t>(i))
        {
            throw std::invalid_argument(std::string(kWho) + "other_participants: an empty, repeated or reserved name: " + name);
        }
    }
    silkit_->options = std::move(options);
    silkit_->uri = silkit_->options.registry_uri;
    if (silkit_->uri.empty())
    {
        silkit_->registry = SilKit::Vendor::Vector::CreateSilKitRegistry(SilKit::Config::ParticipantConfigurationFromString("{}"));
        silkit_->uri = silkit_->registry->StartListening("silkit://127.0.0.1:0");
    }
}

Simulation::~Simulation() = default;

void Simulation::require_usable() const
{
    if (abandoned_ == nullptr)
    {
        return;
    }
    std::string name;
    if (local_)
    {
        for (const auto& n : local_->can.ecu_names())
        {
            if (&local_->can.ecu(n) == abandoned_)
            {
                name = n;
            }
        }
    }
    else
    {
        for (std::size_t i = 0; i < silkit_->peers.size(); ++i)
        {
            if (static_cast<const Ecu*>(silkit_->peers[i].get()) == abandoned_)
            {
                name = silkit_->names[i];
            }
        }
    }
    throw std::logic_error(std::string(kWho) + "the application of the ECU " + name +
                           " threw from its constructor in add(); this Simulation cannot be used any more");
}

void Simulation::require_ecus(const char* what) const
{
    if (local_ ? local_->can.ecu_names().empty() : silkit_->peers.empty())
    {
        throw std::logic_error(std::string(kWho) + what + " without an ECU (add one with add_ecu() or add())");
    }
}

Ecu& Simulation::add_ecu(std::string name, SimDuration step)
{
    require_usable();
    if (local_)
    {
        // The same names as on SIL Kit, so that the same code runs on both.
        if (name == kControllerName)
        {
            throw std::invalid_argument(std::string(kWho) + "the name " + name + " is the controller's");
        }
        return local_->can.add_ecu(std::move(name), step);
    }
    auto& s = *silkit_;
    if (s.ran)
    {
        throw std::logic_error(std::string(kWho) + "add_ecu() after a run");
    }
    if (name.empty())
    {
        throw std::invalid_argument(std::string(kWho) + "empty name");
    }
    if (s.name_is_taken(name))
    {
        throw std::invalid_argument(std::string(kWho) + "the name " + name + " is used");
    }
    if (step <= SimDuration::zero())
    {
        throw std::invalid_argument(std::string(kWho) + "the step must be > 0");
    }
    PeerOptions o;
    o.participant = name;
    o.registry_uri = s.uri;
    o.lifecycle = LifecycleMode::Coordinated;
    o.virtual_time = VirtualTimeOptions{step, std::nullopt, std::nullopt};
    s.names.reserve(s.names.size() + 1);
    s.peers.push_back(std::make_unique<Peer>(std::move(o)));
    s.names.push_back(std::move(name));
    return *s.peers.back();
}

void Simulation::run_for(SimDuration duration)
{
    require_usable();
    if (duration <= SimDuration::zero())
    {
        throw std::invalid_argument(std::string(kWho) + "the duration must be > 0");
    }
    require_ecus("run_for()");
    if (local_)
    {
        try
        {
            local_->can.run_for(duration);
        }
        catch (...)
        {
            // A host that failed stays failed: a later run_for() reports it again. Every ECU of
            // the LocalBench, also those added through local().
            std::vector<std::string> failures;
            for (const auto& name : local_->can.ecu_names())
            {
                if (const auto& host = local_->can.host(name); host.failed())
                {
                    failures.push_back(name + ": " + host.error());
                }
            }
            if (failures.empty())
            {
                throw;
            }
            throw SimulationError(join(failures));
        }
        return;
    }
    auto& s = *silkit_;
    if (!s.options.controller)
    {
        throw std::logic_error(std::string(kWho) + "run_for() needs SilKitOptions::controller (use run())");
    }
    if (s.ran)
    {
        throw std::logic_error(std::string(kWho) + "a SIL Kit simulation runs once");
    }
    s.ran = true;
    s.run(duration);
    s.now = SimTime{} + duration;
}

void Simulation::run()
{
    require_usable();
    if (local_)
    {
        throw std::logic_error(std::string(kWho) + "run() is for SIL Kit (use run_for())");
    }
    auto& s = *silkit_;
    if (s.ran)
    {
        throw std::logic_error(std::string(kWho) + "a SIL Kit simulation runs once");
    }
    require_ecus("run()");
    s.ran = true;
    s.run(std::nullopt);
}

SimTime Simulation::now() const noexcept { return local_ ? local_->can.now() : silkit_->now; }

bool Simulation::is_local() const noexcept { return local_ != nullptr; }

LocalBench& Simulation::local()
{
    if (!local_)
    {
        throw std::logic_error(std::string(kWho) + "local() is for a simulation on local buses");
    }
    return local_->can;
}

Peer& Simulation::peer(std::string_view ecu)
{
    if (!silkit_)
    {
        throw std::logic_error(std::string(kWho) + "peer() is for a simulation on SIL Kit");
    }
    for (std::size_t i = 0; i < silkit_->names.size(); ++i)
    {
        if (silkit_->names[i] == ecu)
        {
            return *silkit_->peers[i];
        }
    }
    throw std::out_of_range(std::string(kWho) + "no ECU named " + std::string(ecu));
}

const std::string& Simulation::registry_uri() const
{
    if (!silkit_)
    {
        throw std::logic_error(std::string(kWho) + "registry_uri() is for a simulation on SIL Kit");
    }
    return silkit_->uri;
}

} // namespace vehicle_sil
