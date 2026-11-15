// SPDX-License-Identifier: MIT
//
// What --list-interfaces prints: the text for people, and with --json the JSON that programs read
// (vehicle_sil_launch and the GUI offer the interfaces from it). Both come from the same data:
// eth_link::list_interfaces() for packet and tap, eth_link::pcap_interfaces() for npcap and pcap.
// Platform independent so it can be tested anywhere.
#pragma once

#include "vehicle_sil/eth_link/os_links.hpp"
#include "vehicle_sil/eth_link/pcap_link.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::ethernet_bridge {

/// The version of the JSON form ("vehicle_sil_targets"), the same for every bridge that lists
/// where it can connect.
inline constexpr int kTargetsFormat = 1;

/// The text of --list-interfaces for the interfaces of this computer (packet, tap): a heading,
/// then eth_link::format_interface_list().
[[nodiscard]] std::string format_os_interface_list(const std::vector<eth_link::InterfaceInfo>& interfaces);

/// --list-interfaces --json for packet and tap: one target per interface, "value" its name;
/// one that is not Ethernet is not usable. Ends with a newline.
[[nodiscard]] std::string os_interfaces_json(Backend backend, const std::vector<eth_link::InterfaceInfo>& interfaces);

/// --list-interfaces --json for npcap and pcap: one target per device of `list`, "value"
/// eth_link::pcap_interface_argument(); a loopback device, and "any" of libpcap, are not usable.
/// "source" is the library's version, "notes" the list's notes. Ends with a newline.
[[nodiscard]] std::string pcap_interfaces_json(Backend backend, const eth_link::PcapInterfaceList& list);

/// The "code" of the JSON for a list that could not be made: "library-missing" and
/// "permission-denied" (eth_link::LinkError of that kind), "backend-unavailable"
/// (BackendUnavailableError), else "failed".
[[nodiscard]] std::string_view interface_list_error_code(const std::exception& error) noexcept;

/// --list-interfaces --json when the list could not be made: no targets, and "error" with
/// interface_list_error_code() and the message (what()). Ends with a newline.
[[nodiscard]] std::string interface_list_error_json(std::optional<Backend> backend, const std::exception& error);

} // namespace vehicle_sil::ethernet_bridge
