// SPDX-License-Identifier: MIT
//
// The pcap link's parts that do not depend on the operating system: they use the table of
// pcap_api.hpp only, so every platform builds and tests them.
#pragma once

#include "pcap_api.hpp"

#include "vehicle_sil/eth_link/pcap_link.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::eth_link::detail {

/// One device of the library's list.
struct PcapDevice
{
    std::string name;        ///< the pcap name (Windows: "\Device\NPF_{...}"; Linux: the interface's name)
    std::string description; ///< the library's text, or empty
    std::string connection;  ///< the OS's name of the network connection (Windows), or empty
    std::uint32_t flags = 0; ///< kPcapIf*
};

/// The devices the library offers, with the OS's connection names. Throws LinkError.
[[nodiscard]] std::vector<PcapDevice> pcap_devices(const PcapLibrary& library);

/// The name --list-interfaces shows for a device and messages use: the connection name if the
/// device has one, else the pcap name.
[[nodiscard]] const std::string& display_name(const PcapDevice& device) noexcept;

/// The index of the device --interface `wanted` means: the device with that pcap name; else the
/// devices with that connection name or that identifier ("{...}" of the pcap name, with or
/// without braces, in any case of the letters). Throws LinkError if there is none, or more than one.
[[nodiscard]] std::size_t choose_pcap_device(std::span<const PcapDevice> devices, std::string_view wanted, PcapFlavor flavor);

/// The text of --list-interfaces. `version`: what the library says it is.
[[nodiscard]] std::string format_pcap_devices(std::span<const PcapDevice> devices, PcapFlavor flavor, std::string_view version);

/// The link on a loaded library.
[[nodiscard]] std::unique_ptr<ExternalLink> make_pcap_link(std::shared_ptr<PcapLibrary> library, PcapLinkOptions options);

} // namespace vehicle_sil::eth_link::detail
