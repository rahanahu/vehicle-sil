// SPDX-License-Identifier: MIT
//
// The main() of an executable that is one ECU, with the same command line for every ECU
// (vehicle_sil::simulation). The header includes no SIL Kit header.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/ecu/ecu.hpp"

#include <functional>
#include <string>
#include <utility>

namespace vehicle_sil {

/// The ECU of an executable made with ecu_main().
struct EcuMainOptions
{
    /// The ECU's name: its SIL Kit participant name and the "name" of --describe. --name replaces
    /// it.
    std::string name{};
    /// The ECU's step (> 0).
    SimDuration step{};
};

/// The main() of an executable that is one ECU, set up by `set_up` (called once with the ECU,
/// before anything runs; it emplaces the application, Ecu::emplace()). The command line:
///
///     --describe              sets the ECU up on the local virtual buses without running or
///                             connecting anything, writes to_json(Ecu::describe()) to stdout, 0;
///                             from before the set-up to the end of the process, fd 1 points at
///                             stderr (what the application writes to stdout goes there, also from
///                             destructors and threads); the JSON goes to a copy of the original stdout
///     --registry-uri <uri>    joins the SIL Kit simulation at <uri> as a Coordinated participant
///                             in virtual time and runs until it ends; a controller elsewhere
///                             (vehicle_sil_sim_controller) starts and stops it
///                             (Simulation(SilKitOptions{.registry_uri, .controller = false}).run())
///     --start-timeout-ms <ms> with --registry-uri: how long to wait for the start (30000)
///     --local --run-for-ms <ms>  runs this ECU alone on the local virtual buses for <ms> of
///                             virtual time (a check of the ECU by itself)
///     --name <name>           the participant name instead of EcuMainOptions::name (ASCII or
///                             UTF-8); also for --describe. Only the name: the J1939 NAME, the
///                             addresses and the variables stay
///     --help, -h              the usage on stdout, 0 (where an option goes, not as a value)
///
/// One of --describe, --registry-uri and --local is needed: without one (also without any
/// argument) it writes the usage to stderr and returns 4 instead of starting something. Exit
/// codes, as the other tools: 0 done, 1 the ECU failed in the run (SimulationError), 3 the set-up
/// failed (the application's constructor threw, EcuMainOptions is wrong, the registry could not be
/// reached, the simulation did not start, stdout could not be written), 4 a bad command line (an
/// unknown argument, a missing or bad value, an argument given twice, two of the three modes),
/// 130 stopped by a signal (below).
/// With --registry-uri, SIGINT and SIGTERM (Windows: Ctrl+C, Ctrl+Break, closing the console)
/// stop the participant once the simulation runs (which stops the coordinated simulation in
/// order), and ecu_main() returns 130 after the run; before it runs, the process ends at once with
/// 130 (no destructor runs). A second signal ends it at once. The handlers are installed only
/// while ecu_main() runs the simulation (not for --describe and --local, not while the ECU is
/// destroyed). Every failure writes one line "error: <ECU name>: <why and what to do>" to stderr, a bad
/// command line also the usage. ecu_main() itself writes to stdout only the JSON and the usage.
/// --describe leaves fd 1 pointing at stderr when it returns: call it as the last thing of main().
int ecu_main(int argc, const char* const* argv, const EcuMainOptions& options, const std::function<void(Ecu&)>& set_up);

/// ecu_main() with an App{ecu, app_args...} that the ECU owns (Ecu::emplace()):
///
///     int main(int argc, char** argv)
///     {
///         return vehicle_sil::ecu_main<Engine>(argc, argv, {.name = "Engine", .step = 10ms});
///     }
template <class App, class... Args>
int ecu_main(int argc, char** argv, EcuMainOptions options, Args&&... app_args)
{
    return ecu_main(argc, argv, options,
                    [&](Ecu& ecu) { ecu.emplace<App>(ecu, std::forward<Args>(app_args)...); });
}

} // namespace vehicle_sil
