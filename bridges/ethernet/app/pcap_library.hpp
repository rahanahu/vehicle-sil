// SPDX-License-Identifier: MIT
//
// What the bridge executable decides at build time: which pcap library it loads for --backend
// npcap and --backend pcap, and which backend it uses without --backend.
#pragma once

#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"

#include <optional>
#include <string>

namespace vehicle_sil::ethernet_bridge {

/// Empty: the installed library (eth_link::PcapLinkOptions::library says where that is).
///
/// The executable links pcap_library.cpp, where this is empty and nothing can change it: no
/// option, no environment variable, no file. A program that loaded the library from a place a
/// user or a folder of files could name would load whatever was put there. The process tests
/// link their own definition instead (tests/ethernet_bridge/fake_pcap_library.cpp), which names
/// the fake library; that file is not part of the executable that is installed.
[[nodiscard]] std::string pcap_library();

/// The backend without --backend, given to parse_bridge_options(): npcap on Windows, packet on
/// Linux, none elsewhere (pcap_library.cpp). The executable on the fake pcap library uses its
/// pcap backend (npcap on Windows, pcap on Linux); the one on fake links packet.
[[nodiscard]] std::optional<Backend> default_backend() noexcept;

} // namespace vehicle_sil::ethernet_bridge
