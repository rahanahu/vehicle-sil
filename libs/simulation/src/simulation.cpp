// SPDX-License-Identifier: MIT
#include "vehicle_sil/simulation/simulation.hpp"

#include "simulation_internal.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"
#include "vehicle_sil/local/local_bench.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <silkit/services/orchestration/string_utils.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace vehicle_sil {

namespace orch = SilKit::Services::Orchestration;

namespace {

constexpr const char* kWho = "vehicle_sil::Simulation: ";
constexpr const char* kControllerName = "SimulationController";
/// How often a run looks for a failed participant while it waits for the end.
constexpr std::chrono::milliseconds kPoll{50};
/// How long the participants of this process may take to end once the simulation ended.
constexpr std::chrono::seconds kEndWait{10};
/// After a failure under a controller elsewhere: how long the ECUs of this process wait for
/// that controller to abort the simulation before they stop (see
/// Simulation::SilKitRun::wait_for_the_abort()). The tools of this project look every 50 ms and
/// abort when a participant is in Error. Tests shorten it (detail::set_abort_wait()).
std::atomic<std::chrono::milliseconds::rep> abort_wait_ms{5000};
/// How often wait_for_the_abort() looks at the states.
constexpr std::chrono::milliseconds kAbortPoll{10};

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

    /// The ECUs that failed first, then the others, each group in the order they were added:
    /// what a failed run does to the rest of the simulation must not depend on that order. A
    /// failed ECU goes from Error to Shutdown; only then is the stop of another ECU, in
    /// Coordinated mode, an ordinary stop of the whole simulation (while an ECU is in Error it
    /// would stop that ECU alone). So a run that nobody aborted leaves no other process behind
    /// in a simulation that is in Error. Without a failure the order is the one of add().
    void stop_all(std::string_view reason) noexcept
    {
        std::vector<bool> stopFirst(peers.size(), false);
        for (std::size_t i = 0; i < peers.size(); ++i)
        {
            stopFirst[i] = peers[i]->failed();
        }
        for (const bool first : {true, false})
        {
            for (std::size_t i = 0; i < peers.size(); ++i)
            {
                if (stopFirst[i] == first)
                {
                    peers[i]->stop(reason);
                }
            }
        }
        if (controller)
        {
            controller->stop(reason);
        }
    }

    /// The network check of the ECU `ecu` (its peer `self`, see detail::set_network_check()):
    /// std::invalid_argument if another ECU of this process has a network named `network` of the
    /// other kind. The participants of other processes are not seen.
    void require_network_kind(const Peer& self, const std::string& ecu, const std::string& network,
                              NetworkKind kind, const std::string& call) const
    {
        for (std::size_t i = 0; i < peers.size(); ++i)
        {
            if (peers[i].get() == &self)
            {
                continue;
            }
            const auto other = detail::network_kind(*peers[i], network);
            if (other && *other != kind)
            {
                const bool lin = *other == NetworkKind::Lin;
                throw std::invalid_argument(std::string(kWho) + call + " of the ECU \"" + ecu + "\": \"" + network +
                                            "\" is a " + (lin ? "LIN" : "CAN") + " network of the ECU \"" + names[i] +
                                            "\" (made by " + (lin ? "lin_master() or lin_slave()" : "j1939() or can()") +
                                            "); a " + (lin ? "CAN" : "LIN") + " network needs a name of its own");
            }
        }
    }

    void run(std::optional<SimDuration> stop_at);

private:
    /// Waits for the end of the simulation of this process's controller; on a failure, aborts
    /// it. Returns why participants other than the ECUs (or the controller) failed.
    std::vector<std::string> watch_with_controller(bool& failure);
    /// Waits until every ECU ended, or one failed.
    void watch_without_controller(std::vector<std::optional<orch::ParticipantState>>& states, bool& failure);
    /// After a failure under a controller elsewhere: waits (up to the abort wait) until no ECU
    /// of this process is Running, Paused or in Error any more, which the controller's abort
    /// does. Nobody is stopped meanwhile.
    void wait_for_the_abort();
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
        if (failure)
        {
            wait_for_the_abort();
        }
    }
    // Without a controller, stopping an ECU that did not fail ends the simulation for the others
    // too (the Coordinated stop); after a failure, wait_for_the_abort() has given the controller
    // elsewhere its chance to see the failure first. If it did not abort, the failed ECUs are
    // stopped first (stop_all()), and the stop of the others then ends the simulation in order.
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
    WatchOptions watch;
    watch.poll = kPoll;
    watch.failed = [this] { return any_peer_failed(); };
    // Before the abort, as the participants were then: whether the one that left is an ECU of
    // this process that failed (reported with its own error) or another participant.
    watch.before_abort = [this, &others](const WatchResult& why) {
        if (const auto& failed = why.failure)
        {
            const auto own = std::find(names.begin(), names.end(), failed->participant);
            if (own == names.end() || !peers[static_cast<std::size_t>(own - names.begin())]->failed())
            {
                others.push_back(failed->participant + ": " + failed->reason);
            }
        }
    };
    const WatchResult watched = watch_simulation(*controller, watch);
    if (watched.end == WatchEnd::Failed)
    {
        failure = true;
    }
    else if (watched.state != orch::ParticipantState::Shutdown)
    {
        failure = true;
        others.push_back(std::string(kControllerName) + ": ended in state " + state_text(watched.state));
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

void Simulation::SilKitRun::wait_for_the_abort()
{
    // The controller elsewhere sees the failure as a participant in Error. So the failed ECUs
    // stay in Error and nobody is stopped until that controller has aborted the simulation (no
    // ECU of this process is Running, Paused or in Error any more), also when the failed ECU is
    // the only one of this process:
    // - Stopping a failed ECU takes it from Error to Shutdown at once; a controller that looks
    //   from time to time then sees a participant that left, not one that failed.
    // - Stopping an ECU that did not fail is an ordinary stop: once no ECU is in Error any more
    //   it ends the whole simulation (Coordinated), and the controller sees no failure at all.
    // A stop that someone asks for meanwhile (Peer::request_stop(), a signal in ecu_main()) is
    // not held back: it takes the ECU out of these states, which ends the wait.
    const auto in_the_simulation = [](const std::unique_ptr<Peer>& p) {
        const auto state = p->runtime().state();
        return state == orch::ParticipantState::Running || state == orch::ParticipantState::Paused
               || state == orch::ParticipantState::Error;
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(abort_wait_ms.load(std::memory_order_relaxed));
    while (std::any_of(peers.begin(), peers.end(), in_the_simulation) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kAbortPoll);
    }
}

namespace detail {

void set_abort_wait(std::chrono::milliseconds wait) noexcept
{
    abort_wait_ms.store(wait.count(), std::memory_order_relaxed);
}

} // namespace detail

Simulation::Simulation() : local_(std::make_unique<LocalRun>()) {}

Simulation::Simulation(SilKitOptions silkit) : Simulation(std::optional<SilKitOptions>(std::move(silkit))) {}

Simulation::Simulation(std::optional<SilKitOptions> silkit)
{
    if (!silkit)
    {
        local_ = std::make_unique<LocalRun>();
        return;
    }
    SilKitOptions options = std::move(*silkit);
    silkit_ = std::make_unique<SilKitRun>();
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
        silkit_->registry = SilKit::Vendor::Vector::CreateSilKitRegistry(
            SilKit::Config::ParticipantConfigurationFromString(std::string(default_participant_configuration())));
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
    auto peer = std::make_unique<Peer>(std::move(o));
    // As on the local buses: a name is a CAN or a LIN network for all the ECUs of this process.
    detail::set_network_check(*peer, [run = &s, self = peer.get(), ecu = name](const std::string& network,
                                                                               NetworkKind kind,
                                                                               const std::string& call) {
        run->require_network_kind(*self, ecu, network, kind, call);
    });
    s.peers.push_back(std::move(peer));
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

Ecu& Simulation::ecu(std::string_view name)
{
    const auto names = local_ ? local_->can.ecu_names() : silkit_->names;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (names[i] == name)
        {
            return local_ ? local_->can.ecu(name) : static_cast<Ecu&>(*silkit_->peers[i]);
        }
    }
    std::string list;
    for (const auto& n : names)
    {
        list += (list.empty() ? "\"" : ", \"") + n + "\"";
    }
    throw std::out_of_range(std::string(kWho) + "ecu(\"" + std::string(name) + "\"): no ECU has that name. ECUs: "
                            + (list.empty() ? std::string("none (add one with add_ecu() or add())") : list));
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
