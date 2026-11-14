// SPDX-License-Identifier: MIT
//
// The external links of this operating system (vehicle_sil/eth_link/os_links.hpp).
#include "link_factory.hpp"

#include "vehicle_sil/eth_link/os_links.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"

#include <stdexcept>

namespace vehicle_sil::ethernet_bridge {

std::unique_ptr<eth_link::ExternalLink> make_external_link(const BridgeOptions& options)
{
    if (const auto why = backend_unavailable(options.backend, host_os()))
    {
        throw std::runtime_error(*why);
    }
    if (options.backend == Backend::Tap)
    {
        return eth_link::make_tap_link({options.interface_name, options.tap_create});
    }
    return eth_link::make_packet_link({options.interface_name, options.promiscuous});
}

std::string interface_list(const BridgeOptions& options)
{
    if (const auto why = backend_unavailable(options.backend, host_os()))
    {
        throw std::runtime_error(*why);
    }
    return "interfaces of this computer (--backend packet takes the Ethernet ones; --backend tap takes a TAP device):\n" +
           eth_link::format_interface_list(eth_link::list_interfaces());
}

} // namespace vehicle_sil::ethernet_bridge
