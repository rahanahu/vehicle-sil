// SPDX-License-Identifier: MIT
//
// What the bridge executable decides at build time: which pcap library it loads for --backend
// npcap and --backend pcap, which backend it uses without --backend, and under which identity it
// marks its device.
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

/// The identity under which the bridge marks the device it opened (bridge_runtime::DeviceMark) and
/// looks for the marks on a listed one, given the device's own (eth_link::ExternalLink::
/// device_identity(), or the name in the list): that identity as it is (pcap_library.cpp). The
/// executables of the process tests make it differ per process unless the test names a scope
/// (tests/ethernet_bridge/fake_device_scope.hpp), so that tests that run at the same time on the
/// same fake device do not find each other.
[[nodiscard]] std::string device_mark_identity(const std::string& device);

} // namespace vehicle_sil::ethernet_bridge
