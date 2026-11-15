// SPDX-License-Identifier: MIT
//
// What an ECU executable made with ecu_main() (vehicle_sil/simulation/ecu_main.hpp) and the
// launcher that starts it (vehicle_sil_launch) agree on: the arguments the launcher gives, the
// longest duration in milliseconds, the exit codes. Both build against this header (no SIL Kit).
// The meaning of each: docs/ecu-executable.md.
#pragma once

#include <cstdint>
#include <string_view>

namespace vehicle_sil::ecu_executable {

/// The longest duration in milliseconds (about 31 years): the largest --run-for-ms and
/// --start-timeout-ms of an ECU executable, and every duration of the launcher's configuration.
inline constexpr std::int64_t kMaxMillis = 1'000'000'000'000;

/// The arguments that the launcher gives an ECU executable itself, or that would clash with them;
/// a command of the configuration must not have them. An ECU executable takes each of them.
inline constexpr std::string_view kLauncherArguments[] = {"--describe",    "--name",       "--registry-uri",
                                                          "--local",       "--run-for-ms", "--start-timeout-ms",
                                                          "--map-network", "--j1939-node", "--help",
                                                          "-h"};

/// The argument that renames the networks of an ECU executable for one run (with --registry-uri,
/// any number of times): `--map-network <simulation's name>=<executable's name>`, split at the
/// first '='. The network that the executable's code names <executable's name> is the SIL Kit
/// network <simulation's name>. An executable that takes it says so in its --describe:
/// kFeatureMapNetwork in "features".
inline constexpr std::string_view kMapNetworkArgument = "--map-network";
inline constexpr std::string_view kFeatureMapNetwork = "map-network";

/// The argument that gives a J1939 node of an ECU executable other values than its code for one
/// run (with --registry-uri, any number of times): `--j1939-node <simulation's network
/// name>=<setting>:<value>[,...]`, split at the first '='; the settings are from, address,
/// identity_number, function_instance and ecu_instance (vehicle_sil/ecu/j1939_node_argument.hpp
/// writes and reads the value). An executable that takes it says so in its --describe:
/// kFeatureJ1939Node in "features".
inline constexpr std::string_view kJ1939NodeArgument = "--j1939-node";
inline constexpr std::string_view kFeatureJ1939Node = "j1939-node";

/// The exit codes of an ECU executable (the same as those of the other tools of vehicle-sil).
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;      ///< the ECU failed in the run
inline constexpr int kExitSetupFailed = 3; ///< the set-up failed (the ECU's, joining, the start)
inline constexpr int kExitUsage = 4;       ///< a bad command line
/// Interrupted by a signal (SIGINT, SIGTERM; Windows Ctrl+C, Ctrl+Break): 128 + SIGINT, as shells
/// report a program that Ctrl+C ended.
inline constexpr int kExitInterrupted = 130;

} // namespace vehicle_sil::ecu_executable
