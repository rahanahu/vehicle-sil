// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/os_links.hpp"

#include "text_width.hpp"

#include <algorithm>

namespace vehicle_sil::eth_link {

std::string format_interface_list(const std::vector<InterfaceInfo>& interfaces)
{
    std::size_t width = 0;
    for (const auto& entry : interfaces)
    {
        width = std::max(width, detail::display_width(entry.name));
    }
    std::string text;
    for (const auto& entry : interfaces)
    {
        text += "  --interface " + entry.name + std::string(width - detail::display_width(entry.name) + 2, ' ');
        text += interface_detail(entry) + "\n";
    }
    return text;
}

std::string interface_detail(const InterfaceInfo& entry)
{
    return (entry.ethernet ? std::string("Ethernet") : "not Ethernet (link type " + std::to_string(entry.link_type) + ")") +
           (entry.up ? ", up" : ", down");
}

} // namespace vehicle_sil::eth_link
