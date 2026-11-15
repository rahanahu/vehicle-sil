// SPDX-License-Identifier: MIT
//
// The names of the variables vehicle-sil registers to monitor a participant: a Peer's monitoring
// outputs (PeerOptions::monitor), and the prefixes kept for them, which an ECU's application
// cannot use (kReservedPrefixes, refused by Ecu::input() and the like). The Peer that registers
// them and the launcher that reads them (the traffic of a run) both build against this header
// (no SIL Kit). The outputs and their meaning: "Peer monitoring" in docs/internals/control.md.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vehicle_sil::monitor_names {

/// The prefix of a Peer's own outputs ("peer.steps" ...).
inline constexpr std::string_view kPeerPrefix = "peer.";
inline constexpr std::string_view kPeerSteps = "peer.steps";
inline constexpr std::string_view kPeerSpeed = "peer.speed";
inline constexpr std::string_view kPeerAppTimeMaxUs = "peer.app_time_max_us";

/// The prefix of the outputs of a J1939 network: "j1939.<network>.<field>", with the network's
/// name in the simulation.
inline constexpr std::string_view kJ1939Prefix = "j1939.";

/// The fields of a J1939 network ("j1939.<network>.<field>").
inline constexpr std::string_view kBusState = "bus_state";
inline constexpr std::string_view kFramesReceived = "frames_received";
inline constexpr std::string_view kFramesSent = "frames_sent";
inline constexpr std::string_view kSendFailures = "send_failures";
inline constexpr std::string_view kSendQueueFull = "send_queue_full";
inline constexpr std::string_view kReceiveOverflows = "receive_overflows";
inline constexpr std::string_view kHeldDropped = "held_dropped";
inline constexpr std::string_view kAddressViolations = "address_violations";
inline constexpr std::string_view kTransportAbortsSent = "transport_aborts_sent";
inline constexpr std::string_view kTransportAbortsReceived = "transport_aborts_received";

/// The fields of a node of a J1939 network ("j1939.<network>.node<i>.<field>", i from 0).
inline constexpr std::string_view kNode = "node";
inline constexpr std::string_view kNodeAddress = "address";
inline constexpr std::string_view kNodeAddressState = "address_state";

/// "j1939.<network>.": the start of every output of the network `network`.
inline std::string j1939_prefix(std::string_view network)
{
    std::string out(kJ1939Prefix);
    out += network;
    out += '.';
    return out;
}

/// "j1939.<network>.node<index>": the start of the outputs of a node (".address" ... follow).
inline std::string j1939_node_prefix(std::string_view network, std::size_t index)
{
    return j1939_prefix(network) + std::string(kNode) + std::to_string(index);
}

/// The <network> of "j1939.<network>.<field>" (not empty), or empty if `name` is not of that form.
inline std::string_view j1939_network(std::string_view name, std::string_view field)
{
    if (name.size() <= kJ1939Prefix.size() + 1 + field.size() || !name.starts_with(kJ1939Prefix) ||
        !name.ends_with(field) || name[name.size() - field.size() - 1] != '.')
    {
        return {};
    }
    return name.substr(kJ1939Prefix.size(), name.size() - kJ1939Prefix.size() - 1 - field.size());
}

/// The prefixes kept for the monitoring outputs of the buses that have none yet.
inline constexpr std::string_view kCanPrefix = "can.";
inline constexpr std::string_view kLinPrefix = "lin.";
inline constexpr std::string_view kEthPrefix = "eth.";

/// A prefix of monitoring outputs and what has its outputs there (for the error message).
struct ReservedPrefix
{
    std::string_view prefix;
    std::string_view outputs; ///< "the monitoring outputs of <outputs>"
};

/// The prefixes an ECU's own variable must not start with: the monitoring outputs are named
/// with them, on every implementation and also when monitoring is off. "ecu." (the variables
/// every ECU has) is not a monitoring prefix and is refused on its own (Ecu::input()). "spy." is
/// not reserved: vehicle_sil::spy::Spy declares its variables as an application does.
inline constexpr ReservedPrefix kReservedPrefixes[] = {
    {kPeerPrefix, "a Peer (PeerOptions::monitor)"},
    {kJ1939Prefix, "a Peer's J1939 networks (\"j1939.<network>.frames_sent\" and the like; PeerOptions::monitor)"},
    {kCanPrefix, "raw CAN channels"},
    {kLinPrefix, "LIN nodes"},
    {kEthPrefix, "Ethernet channels"},
};

/// The entry of kReservedPrefixes that `name` starts with, or nullptr.
inline const ReservedPrefix* reserved_prefix(std::string_view name) noexcept
{
    for (const auto& reserved : kReservedPrefixes)
    {
        if (name.starts_with(reserved.prefix))
        {
            return &reserved;
        }
    }
    return nullptr;
}

} // namespace vehicle_sil::monitor_names
