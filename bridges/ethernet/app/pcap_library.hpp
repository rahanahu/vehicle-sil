// SPDX-License-Identifier: MIT
//
// Which pcap library the bridge executable loads for --backend npcap and --backend pcap.
#pragma once

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

} // namespace vehicle_sil::ethernet_bridge
