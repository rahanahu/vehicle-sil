// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/os_links.hpp"

#include "linux_link.hpp"

#include <net/if.h>
#include <net/if_arp.h>

namespace vehicle_sil::eth_link {

bool has_os_links() noexcept
{
    return true;
}

std::vector<InterfaceInfo> list_interfaces()
{
    std::vector<InterfaceInfo> interfaces;
    for (auto& name : detail::interface_names())
    {
        const auto state = detail::interface_state(name);
        if (!state)
        {
            continue; // removed since the names were read
        }
        InterfaceInfo info;
        info.name = std::move(name);
        info.link_type = state->link_type;
        info.ethernet = state->link_type == ARPHRD_ETHER;
        info.up = (state->flags & IFF_UP) != 0;
        interfaces.push_back(std::move(info));
    }
    return interfaces;
}

} // namespace vehicle_sil::eth_link
