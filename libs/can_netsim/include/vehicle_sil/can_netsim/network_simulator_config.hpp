// SPDX-License-Identifier: MIT
//
// The network simulator configuration file, in the format of the Vector SIL Kit Network
// Simulator, so that one file serves both simulators (vehicle_sil::can_netsim).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::can {

/// A configuration file that cannot be used: not YAML or JSON, a missing or malformed required
/// entry, or an unsupported schema version.
class NetworkSimulatorConfigError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// One entry of SimulatedNetworks.
struct SimulatedNetworkConfig
{
    std::string name; ///< The network name the participants' controllers use.
    std::string type; ///< "CAN", "LIN", "Ethernet" or "FlexRay", as written in the file.
};

/// The network simulator configuration.
///
/// The format is the Vector SIL Kit Network Simulator's (SchemaVersion 1, YAML or JSON):
///
///     SchemaVersion: 1
///     Description: optional text
///     SimulationStepSizeNanoseconds: 1000000   # optional, default 1 ms
///     SimulatedNetworks:
///       - Name: CAN1
///         Type: CAN
///     Switches: [...]                          # Ethernet only
///
/// vehicle-sil's simulator simulates the CAN networks only. Networks of other types, the
/// Ethernet switches, and keys it does not know are accepted and reported in `warnings`, so a
/// file written for Vector's simulator can be used unchanged. Settings that only vehicle-sil's
/// simulator has are not in this file (command-line options), for the same reason.
struct NetworkSimulatorConfig
{
    int schema_version = 1;
    std::string description;
    SimDuration step_size{std::chrono::milliseconds(1)};
    std::vector<SimulatedNetworkConfig> networks;
    /// Parts of the file this simulator does not use (one line each).
    std::vector<std::string> warnings;

    /// Names of the networks of type CAN, in file order.
    [[nodiscard]] std::vector<std::string> can_networks() const;
};

/// Parses a configuration in YAML or JSON (JSON is detected by a leading '{'). `source` names
/// the text in error messages. Throws NetworkSimulatorConfigError.
[[nodiscard]] NetworkSimulatorConfig parse_network_simulator_config(std::string_view text,
                                                                    std::string_view source = "configuration");

/// Reads and parses the file at `path`. Throws NetworkSimulatorConfigError (also if the file
/// cannot be read).
[[nodiscard]] NetworkSimulatorConfig load_network_simulator_config(const std::string& path);

} // namespace vehicle_sil::can
