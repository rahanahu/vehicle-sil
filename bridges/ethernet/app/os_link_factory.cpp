// SPDX-License-Identifier: MIT
//
// The external links of this operating system (vehicle_sil/eth_link/os_links.hpp, pcap_link.hpp).
#include "link_factory.hpp"
#include "pcap_library.hpp"

#include "vehicle_sil/eth_link/os_links.hpp"
#include "vehicle_sil/eth_link/pcap_link.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"

#include <stdexcept>

namespace vehicle_sil::ethernet_bridge {

std::unique_ptr<eth_link::ExternalLink> make_external_link(const BridgeOptions& options)
{
    if (const auto why = backend_unavailable(options.backend, host_os()))
    {
        throw std::runtime_error(*why);
    }
    switch (*options.backend)
    {
    case Backend::Tap: return eth_link::make_tap_link({options.interface_name, options.tap_create});
    case Backend::Packet: return eth_link::make_packet_link({options.interface_name, options.promiscuous});
    case Backend::Npcap:
    case Backend::Pcap:
        // Loads the library: a computer without it is told so here, before SIL Kit is touched.
        return eth_link::make_pcap_link({options.interface_name, options.promiscuous, pcap_library()});
    }
    throw std::logic_error("unknown backend");
}

std::string interface_list(const BridgeOptions& options)
{
    if (const auto why = backend_unavailable(options.backend, host_os()))
    {
        throw std::runtime_error(*why);
    }
    // Without --backend: the interfaces of the OS where the bridge has links of the OS itself
    // (Linux), else what the pcap library offers (Windows).
    const bool pcap = options.backend ? *options.backend == Backend::Npcap || *options.backend == Backend::Pcap
                                      : !eth_link::has_os_links();
    if (pcap)
    {
        return eth_link::pcap_interface_list(pcap_library());
    }
    return "interfaces of this computer (--backend packet and --backend pcap take the Ethernet ones; --backend tap takes a TAP "
           "device):\n" +
           eth_link::format_interface_list(eth_link::list_interfaces());
}

} // namespace vehicle_sil::ethernet_bridge
