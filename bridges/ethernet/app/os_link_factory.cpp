// SPDX-License-Identifier: MIT
//
// The external links of this operating system (vehicle_sil/eth_link/os_links.hpp, pcap_link.hpp).
#include "link_factory.hpp"
#include "pcap_library.hpp"

#include "vehicle_sil/eth_link/os_links.hpp"
#include "vehicle_sil/eth_link/pcap_link.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"
#include "vehicle_sil/ethernet_bridge/interface_list.hpp"

#include "vehicle_sil/cli/console.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace vehicle_sil::ethernet_bridge {

namespace {

/// Npcap installed for administrators only holds the bridge at Windows' elevation prompt (UAC) until
/// it is answered, and says nothing itself: one line before Npcap is opened says why the bridge
/// waits. `stream`: stdout while running (a line like the others), stderr for --list-interfaces
/// (stdout holds the list). Not for a library the tests name (pcap_library() not empty).
void tell_if_npcap_asks_for_elevation(Backend backend, std::FILE* stream)
{
    if (backend == Backend::Npcap && pcap_library().empty() && eth_link::npcap_asks_for_elevation())
    {
        cli::print(stream, "[eth-bridge] Npcap was installed for administrators only: Windows asks to allow it (an elevation "
                           "prompt, UAC), and the bridge waits for the answer\n");
    }
}

} // namespace

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
        tell_if_npcap_asks_for_elevation(*options.backend, stdout);
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
    // The marks of the bridges that use an interface, looked at without placing one.
    std::vector<bridge_runtime::DevicePeers> inUse;
    const auto look = [&inUse](const std::string& device) {
        inUse.push_back(bridge_runtime::look_for_marks(bridge_runtime::kEthernetDevice, device_mark_identity(device)));
    };
    if (backend == Backend::Npcap || backend == Backend::Pcap)
    {
        tell_if_npcap_asks_for_elevation(backend, stderr);
        const auto list = eth_link::pcap_interfaces(pcap_library());
        for (const auto& device : list.devices)
        {
            look(device.name);
        }
        return options.json ? pcap_interfaces_json(backend, list, inUse) : format_pcap_list(list, inUse);
    }
    const auto interfaces = eth_link::list_interfaces();
    for (const auto& entry : interfaces)
    {
        look(entry.name);
    }
    return options.json ? os_interfaces_json(backend, interfaces, inUse) : format_os_interface_list(interfaces, inUse);
}

} // namespace vehicle_sil::ethernet_bridge
