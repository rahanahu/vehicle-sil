// SPDX-License-Identifier: MIT
//
// Runs ECUs on local virtual CAN buses or as SIL Kit participants of this process, in virtual
// time (vehicle_sil::simulation). The header includes no SIL Kit header.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/ecu/ecu.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil {

class Peer;
namespace testkit {
class LocalCan;
} // namespace testkit

/// Runs the ECUs as SIL Kit participants of this process, in virtual time.
struct SilKitOptions
{
    /// The registry to connect to. Empty: a registry of this process (nothing else to start);
    /// Simulation::registry_uri() tells where it listens.
    std::string registry_uri{};
    /// true: this process starts and ends the simulation (a SimulationController, named
    /// "SimulationController", whose required participants are the ECUs added here and
    /// `other_participants`). false: a controller elsewhere does (vehicle_sil_sim_controller,
    /// the Control Server, or one the caller runs).
    bool controller = true;
    /// Coordinated participants of other processes (or made by the caller) that must be present
    /// before the simulation starts. Only with `controller` (std::invalid_argument otherwise).
    std::vector<std::string> other_participants{};
    /// How long the start may take (the registry, the required participants).
    std::chrono::milliseconds start_timeout{std::chrono::seconds(30)};
};

/// A run failed. what(): "<ecu>: <why>" of each ECU (or other participant) that failed,
/// separated by "; ".
class SimulationError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Where ECUs run together, in virtual time: on virtual CAN buses in this process
/// (Simulation()), or as SIL Kit participants of this process (Simulation(SilKitOptions)). The
/// ECUs are written against vehicle_sil::Ecu, so the same code runs on both; only the
/// constructor differs.
///
///     vehicle_sil::Simulation sim;                                // local virtual buses
///     // vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{}); // SIL Kit, own registry
///     sim.add<Engine>("Engine", 10ms);                             // Engine(Ecu&) owned by the ECU
///     auto& cab = sim.add<Cab>("Cab", 5ms);
///     sim.run_for(3s);                                             // SimulationError on failure
///
/// Local: a testkit::LocalCan; every ECU is a host on it (LocalCan::add_ecu()), stepped in the
/// calling thread. run_for() may be called again and continues. The ECUs' networks are on named
/// buses, so local() takes the bus's name too (LocalCan::inject(network, time, frame)).
///
/// SIL Kit: every ECU is a Peer, a Coordinated participant in virtual time named like the ECU
/// with the ECU's step. A run starts them together (with this process's SimulationController,
/// or one elsewhere) and returns only after every participant of this process has stopped, so
/// what the steps wrote can be read afterwards. A simulation runs once: SIL Kit cannot hold
/// all participants at a time boundary without the boundary step still running, so a second
/// run_for() throws. A failed participant does not end a SIL Kit simulation by itself (virtual
/// time only stops); the run watches for it, ends the simulation and throws SimulationError.
///
/// Ownership: the Simulation owns the ECUs (and what they own); references it returns are valid
/// as long as the Simulation. The destructor stops everything, destroys the ECUs (the one added
/// last first), then the controller, then the registry of this process.
///
/// Threading: call the Simulation's functions from one thread. Local: the steps run in that
/// thread, inside run_for(). SIL Kit: the steps run on SIL Kit's threads during a run (as with
/// Peer); read what they record after the run returned.
class Simulation
{
public:
    /// On virtual CAN buses in this process, stepped in the calling thread (testkit::LocalCan).
    Simulation();
    /// On SIL Kit. Starts the registry of this process if options.registry_uri is empty
    /// (SilKit::SilKitError on failure); connects nothing else yet (add_ecu() connects each
    /// participant). std::invalid_argument for other_participants without controller, or for an
    /// empty or repeated name in it.
    explicit Simulation(SilKitOptions options);
    /// Stops everything; never throws.
    ~Simulation();
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    /// An ECU stepped every `step` (> 0). On SIL Kit: a Coordinated, virtual-time participant
    /// named `name` (connects to the registry: SilKit::SilKitError on failure).
    /// std::invalid_argument for an empty or a used name (also "SimulationController", on both,
    /// and one of other_participants) or step <= 0; std::logic_error once a run has started.
    Ecu& add_ecu(std::string name, SimDuration step);
    /// add_ecu(), then constructs an App{ecu, args...} that the ECU owns (Ecu::emplace()). The
    /// App's constructor sets the ECU up (j1939(), every(), ...). If it throws, the exception
    /// propagates and this Simulation cannot be used any more: what the constructor set up stays
    /// on the ECU and may refer to the destroyed App, so add_ecu(), add(), run_for() and run()
    /// then throw std::logic_error (naming the ECU).
    template <class App, class... Args>
    App& add(std::string name, SimDuration step, Args&&... args)
    {
        Ecu& ecu = add_ecu(std::move(name), step);
        try
        {
            return ecu.emplace<App>(ecu, std::forward<Args>(args)...);
        }
        catch (...)
        {
            abandon(ecu);
            throw;
        }
    }

    /// Runs `duration` (> 0, std::invalid_argument) of virtual time: every step that starts
    /// before the end. std::logic_error without an ECU. Local: may be called again and
    /// continues. SIL Kit: once (std::logic_error after a run, and without
    /// SilKitOptions::controller); no ECU runs a step at or after the end, whenever the
    /// controller's stop reaches it.
    /// Throws SimulationError if a step of an ECU failed (local: also every later run_for(); an
    /// ECU added through local() counts too). On
    /// SIL Kit it returns (or throws) only after every participant of this process has stopped,
    /// so what the steps wrote can be read. Exceptions of the start (SIL Kit: the required
    /// participants did not come within start_timeout, std::runtime_error) propagate after
    /// everything was stopped.
    void run_for(SimDuration duration);
    /// SIL Kit only (std::logic_error on local, after a run, and without an ECU): runs until the
    /// simulation ends, whoever ends it (a participant's request_stop(), the controller
    /// elsewhere). Once.
    /// With SilKitOptions::controller, a participant that leaves the simulation in Error is a
    /// failure; one that leaves otherwise must end the simulation within a few seconds, or it is
    /// one too. Without it, only a failed ECU of this process is (the other participants' are
    /// the controller's to see): the run then stops this process's participants, which ends
    /// their part of the simulation. Throws SimulationError like run_for().
    void run();
    /// Where the runs got to (local: LocalCan::now(); SIL Kit: the duration of a finished
    /// run_for(), else 0).
    [[nodiscard]] SimTime now() const noexcept;

    [[nodiscard]] bool is_local() const noexcept;
    /// Local only (std::logic_error otherwise): the buses, for fault injection, raw frames and
    /// taps (include vehicle_sil/testkit/local_can.hpp).
    [[nodiscard]] testkit::LocalCan& local();
    /// SIL Kit only (std::logic_error otherwise): the participant of an ECU, for what only a
    /// Peer has (LIN, taps, control agents, the SIL Kit port; include vehicle_sil/peer/peer.hpp).
    /// std::out_of_range for an unknown name.
    [[nodiscard]] Peer& peer(std::string_view ecu);
    /// SIL Kit only (std::logic_error otherwise): the registry's URI (the one given, or where the
    /// registry of this process listens).
    [[nodiscard]] const std::string& registry_uri() const;

private:
    struct LocalRun;
    struct SilKitRun;
    /// add() failed for `ecu`: the Simulation cannot be used any more.
    void abandon(const Ecu& ecu) noexcept { abandoned_ = &ecu; }
    /// std::logic_error after abandon().
    void require_usable() const;
    /// std::logic_error without an ECU.
    void require_ecus(const char* what) const;

    std::unique_ptr<LocalRun> local_;
    std::unique_ptr<SilKitRun> silkit_;
    const Ecu* abandoned_ = nullptr;
};

} // namespace vehicle_sil
