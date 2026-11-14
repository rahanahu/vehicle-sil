// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/os_links.hpp"

#include <algorithm>

namespace vehicle_sil::eth_link {

std::string format_interface_list(const std::vector<InterfaceInfo>& interfaces)
{
    std::size_t width = 0;
    for (const auto& entry : interfaces)
    {
        width = std::max(width, entry.name.size());
    }
    std::string text;
    for (const auto& entry : interfaces)
    {
        text += "  --interface " + entry.name + std::string(width - entry.name.size() + 2, ' ');
        text += entry.ethernet ? "Ethernet" : "not Ethernet (link type " + std::to_string(entry.link_type) + ")";
        text += entry.up ? ", up\n" : ", down\n";
    }
    return text;
}

} // namespace vehicle_sil::eth_link
