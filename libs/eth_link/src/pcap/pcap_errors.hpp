// SPDX-License-Identifier: MIT
//
// The LinkError of each way the pcap link fails to load its library or to open a device, and its
// warnings: what happened, then how to fix it. Text only (results and texts of the library come
// from the caller), so every platform builds and tests the messages of both libraries.
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"

#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace vehicle_sil::eth_link::detail {

/// Which pcap library: they share the functions and differ in how they are installed, how devices
/// are named and what a user does about a failure.
enum class PcapFlavor
{
    Libpcap, ///< Linux; --backend pcap
    Npcap,   ///< Windows; --backend npcap
};

/// "pcap", "npcap": the value of --backend.
[[nodiscard]] std::string_view backend_name(PcapFlavor flavor) noexcept;
/// "libpcap", "Npcap".
[[nodiscard]] std::string_view library_name(PcapFlavor flavor) noexcept;

// Results of the library's pcap_activate() the messages tell apart.
inline constexpr int kPcapErrorNoSuchDevice = -5;
inline constexpr int kPcapErrorPermissionDenied = -8;
inline constexpr int kPcapErrorInterfaceNotUp = -9;
inline constexpr int kPcapErrorPromiscuousPermissionDenied = -11;
inline constexpr int kPcapWarningPromiscuousNotSupported = 2;

/// The library could not be loaded. `tried`: the file or files; `why`: the loader's reason.
[[nodiscard]] LinkError pcap_library_missing(PcapFlavor flavor, std::string_view tried, std::string_view why);
/// The library was loaded from `path` and lacks the functions `names`. `version`: what it says it
/// is, or empty.
[[nodiscard]] LinkError pcap_functions_missing(PcapFlavor flavor, std::string_view path, std::string_view version,
                                               std::span<const std::string> names);
/// The library could not list the devices; `why` is its text.
[[nodiscard]] LinkError pcap_list_failed(PcapFlavor flavor, std::string_view why);
/// No device is called `wanted`. `names`: the devices the library offers, as --interface takes them.
[[nodiscard]] LinkError pcap_no_interface(PcapFlavor flavor, std::string_view wanted, std::span<const std::string> names);
/// More than one device is called `wanted`. `names`: their pcap names, which tell them apart.
[[nodiscard]] LinkError pcap_ambiguous_interface(PcapFlavor flavor, std::string_view wanted, std::span<const std::string> names);
/// The device `name` does not carry Ethernet frames.
[[nodiscard]] LinkError pcap_not_ethernet(PcapFlavor flavor, std::string_view name, int link_type);
/// Opening the device `name` failed in `step` (the library's function) with `status` and the
/// library's text `why` (may be empty). `executable`: this program's path, or empty.
[[nodiscard]] LinkError pcap_open_failed(PcapFlavor flavor, std::string_view name, std::string_view step, int status,
                                         std::string_view why, std::string_view executable);

/// The Warning for a device the library marks as wireless.
[[nodiscard]] std::string pcap_wireless_warning(std::string_view name);
/// The library's "packets are there" sign has never come up, and frames were found by the look
/// the link takes every `period` anyway.
[[nodiscard]] std::string pcap_no_wake_warning(PcapFlavor flavor, std::string_view name, std::chrono::milliseconds period);
/// The Warning for a device that was opened with a warning of the library (`status` > 0).
[[nodiscard]] std::string pcap_activate_warning(std::string_view name, int status, std::string_view why);

} // namespace vehicle_sil::eth_link::detail
