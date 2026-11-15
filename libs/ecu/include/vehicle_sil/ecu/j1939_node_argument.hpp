// SPDX-License-Identifier: MIT
//
// The value of the argument --j1939-node (vehicle_sil/ecu/executable.hpp) of an ECU executable:
// other values for a J1939 node than its code gives, on a network named as the simulation names
// it. Written by whoever starts the executable (the launcher) and read by the executable
// (ecu_main()) with this one pair of functions, so that both mean the same; also the type that a
// program takes these values with (PeerOptions::j1939_nodes, SilKitOptions::j1939_nodes).
// No SIL Kit.
#pragma once

#include "vehicle_sil/j1939/node_override.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vehicle_sil::ecu_executable {

/// Other values for one J1939 node of an ECU: `network` is the name of the node's network in the
/// simulation (the name after --map-network or PeerOptions::network_names, not the name in the
/// ECU's code where the two differ), `node` says which node (`from`: the address in the ECU's
/// code; none: the only node of the ECU on that network) and what it gets.
struct J1939NodeOverride
{
    std::string network;
    j1939::NodeOverride node{};

    [[nodiscard]] friend bool operator==(const J1939NodeOverride&, const J1939NodeOverride&) = default;
};

/// The value of --j1939-node for `entry`: "VehicleCAN=from:0x00,address:0x01,identity_number:2"
/// (the network's name, '=', j1939::format_node_override()). parse_j1939_node_argument() reads it
/// back to the same entry, for a name without '=' that does not start with '-' and an entry that
/// changes something.
[[nodiscard]] std::string format_j1939_node_argument(const J1939NodeOverride& entry);

/// What parse_j1939_node_argument() made of a value.
struct J1939NodeArgument
{
    /// The entry, if the value is right.
    std::optional<J1939NodeOverride> entry{};
    /// Otherwise what is wrong and how to write it, as one sentence for a usage error
    /// ("--j1939-node ... "); it quotes the value.
    std::string error{};
};

/// Reads one value of --j1939-node: split at the first '=' into the network's name in the
/// simulation and the settings (j1939::parse_node_override(): `<setting>:<value>` separated by
/// commas; a number in decimal or as 0x.. in hexadecimal). An error for a value that is not UTF-8,
/// has no '=', an empty name or one that starts with '-', for wrong settings and for an entry that
/// changes nothing (`from` only).
[[nodiscard]] J1939NodeArgument parse_j1939_node_argument(std::string_view text);

/// The usage error for --j1939-node as the last argument, without its value.
[[nodiscard]] std::string j1939_node_argument_missing();

/// Checks the entries of one ECU against each other. Empty if they can all stand; otherwise what
/// is wrong, as one sentence for a usage error: two entries for the same network with the same
/// `from`, or an entry without `from` (it is for the only node of the ECU on its network) and
/// another one for the same network.
[[nodiscard]] std::string j1939_node_arguments_error(std::span<const J1939NodeOverride> entries);

} // namespace vehicle_sil::ecu_executable
