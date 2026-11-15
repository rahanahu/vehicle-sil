// SPDX-License-Identifier: MIT
//
// What a bridge executable writes for --describe: the JSON that an ecu_main executable writes for
// --describe (format 1 of "vehicle_sil_ecu", docs/ecu-executable.md), so that vehicle_sil_launch
// checks and starts a bridge like an ECU (describe: true in the configuration file).
#pragma once

#include <string>

namespace vehicle_sil::bridge_runtime {

/// The kind of the SIL Kit network that a bridge carries to an interface outside SIL Kit.
enum class BridgedNetworkKind
{
    Can,      ///< "kind": "can", "use": "raw" (classic CAN frames, as Ecu::can())
    Ethernet, ///< "kind": "ethernet", "use": "ethernet", "mac": null (the bridge has no address of its own)
};

struct BridgeDescription
{
    /// The participant name (--name).
    std::string name;
    /// The SIL Kit network that the bridge carries (--network).
    std::string network;
    BridgedNetworkKind kind = BridgedNetworkKind::Can;
};

/// `description` as the JSON of an ecu_main executable's --describe, ending with a newline: the
/// participant name, "step_ns": null (a bridge runs in real time, outside the virtual time), the
/// one network, no variables, "control": null (no control agent) and no "features" (a bridge takes
/// neither --map-network nor --j1939-node). The same writer and layout as vehicle_sil::to_json().
[[nodiscard]] std::string describe_json(const BridgeDescription& description);

} // namespace vehicle_sil::bridge_runtime
