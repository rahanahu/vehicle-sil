// SPDX-License-Identifier: MIT
//
// Runs ECUs on local virtual CAN buses or as SIL Kit participants of this process, in virtual
// time (vehicle_sil::simulation). The header includes no SIL Kit header.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/ecu/ecu.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil {

class LocalBench;
class Peer;

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
    std::chrono::milliseconds start_timeout{std::chrono::seconds(30)};    /// SIL Kit participant configuration (YAML or JSON text) for every participant of this
    /// process: the ECUs and, with `controller`, the controller. "{}": vehicle-sil's defaults.
    /// As ParticipantRuntimeOptions::participant_configuration: the given one is used with
    /// vehicle-sil's defaults for what it leaves out, and one that sets ParticipantName (with
    /// more than one participant here, no name fits them all), another Middleware.RegistryUri
    /// or dynamic simulation steps is std::invalid_argument from add_ecu() or the start. For a
    /// file: participant_configuration_from_file() (vehicle_sil/core/participant_runtime.hpp).
    std::string participant_configuration = "{}";
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
/// To choose at run time, pass a std::optional<SilKitOptions>: empty for the local buses.
///
///     std::optional<vehicle_sil::SilKitOptions> silkit;
///     if (use_silkit) silkit.emplace();
///     vehicle_sil::Simulation sim(silkit);
///
/// Local: a LocalBench; every ECU is a host on it (LocalBench::add_ecu()), stepped in the
/// calling thread. run_for() may be called again and continues. local() reaches the ECUs' buses
/// by their names (LocalBench::inject(network, time, frame)); with one bus, also without a name.
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
    /// On virtual CAN and LIN buses in this process, stepped in the calling thread (LocalBench).
    Simulation();
    /// On SIL Kit if `silkit` has a value, else as Simulation(). On SIL Kit: starts the registry
    /// of this process if registry_uri is empty (SilKit::SilKitError on failure); connects
    /// nothing else yet (add_ecu() connects each participant). std::invalid_argument for
    /// other_participants without controller, or for an empty or repeated name in it.
    explicit Simulation(std::optional<SilKitOptions> silkit);
    /// On SIL Kit: Simulation(std::optional<SilKitOptions>(silkit)). It keeps a braced list
    /// without the type name meaning SilKitOptions (Simulation sim({.registry_uri = uri})), and
    /// makes Simulation sim{{}} ambiguous (a compile error) instead of quietly local.
    explicit Simulation(SilKitOptions silkit);
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
    /// the controller's to see): the ECUs of this process, the failed one too (it stays in
    /// Error), then wait up to 5 s for that controller to abort the simulation, so that it sees
    /// the failure; the run stops none of them meanwhile. If nobody aborts or stops it in that
    /// time, the run stops the failed ECUs first and then the others, which ends the simulation
    /// for the other participants too. Throws SimulationError like run_for().
    void run();
    /// Where the runs got to (local: LocalBench::now(); SIL Kit: the duration of a finished
    /// run_for(), else 0).
    [[nodiscard]] SimTime now() const noexcept;

    /// The ECU named `name` (added with add_ecu() or add(), on both; local: also one added
    /// through local()), e.g. for its control agent (Ecu::agent()) after add<App>().
    /// std::out_of_range for an unknown name (the message lists the ECUs).
    [[nodiscard]] Ecu& ecu(std::string_view name);
    [[nodiscard]] bool is_local() const noexcept;
    /// Local only (std::logic_error otherwise): the buses, for fault injection, raw frames and
    /// taps (include vehicle_sil/local/local_bench.hpp).
    [[nodiscard]] LocalBench& local();
    /// SIL Kit only (std::logic_error otherwise): the participant of an ECU, for what only a
    /// Peer has (taps, the SIL Kit ports, the runtime; include vehicle_sil/peer/peer.hpp). The
    /// ECU's variables and its control agent are on the Ecu (Ecu::input(), Ecu::agent()), on both.
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
