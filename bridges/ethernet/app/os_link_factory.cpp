// SPDX-License-Identifier: MIT
//
// The external links of this operating system (vehicle_sil/eth_link/os_links.hpp, pcap_link.hpp).
#include "link_factory.hpp"
#include "pcap_library.hpp"

#include "vehicle_sil/eth_link/os_links.hpp"
#include "vehicle_sil/eth_link/pcap_link.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"
#include "vehicle_sil/ethernet_bridge/interface_list.hpp"

#include <stdexcept>

namespace vehicle_sil::ethernet_bridge {

std::unique_ptr<eth_link::ExternalLink> make_external_link(const BridgeOptions& options)
{
    if (const auto why = backend_unavailable(options.backend, host_os()))
    {
        throw BackendUnavailableError(*why);
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
        throw BackendUnavailableError(*why);
    }
    if (!options.backend)
    {
        throw std::logic_error("no backend: the executable gives parse_bridge_options() the default of this OS");
    }
    const Backend backend = *options.backend;
    if (backend == Backend::Npcap || backend == Backend::Pcap)
    {
        const auto list = eth_link::pcap_interfaces(pcap_library());
        return options.json ? pcap_interfaces_json(backend, list) : eth_link::format_pcap_interface_list(list);
    }
    const auto interfaces = eth_link::list_interfaces();
    return options.json ? os_interfaces_json(backend, interfaces) : format_os_interface_list(interfaces);
}

} // namespace vehicle_sil::ethernet_bridge
