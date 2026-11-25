// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/os_links.hpp"

#include "vehicle_sil/text/display_width.hpp"

#include <algorithm>

namespace vehicle_sil::eth_link {

std::string format_interface_list(const std::vector<InterfaceInfo>& interfaces, std::span<const std::string> notes)
{
    std::size_t width = 0;
    for (const auto& entry : interfaces)
    {
        width = std::max(width, vehicle_sil::text::display_width(entry.name));
    }
    std::string text;
    for (std::size_t i = 0; i < interfaces.size(); ++i)
    {
        const auto& entry = interfaces[i];
        text += "  --interface " + entry.name + std::string(width - vehicle_sil::text::display_width(entry.name) + 2, ' ');
        text += interface_detail(entry);
        if (i < notes.size() && !notes[i].empty())
        {
            text += "; " + notes[i];
        }
        text += "\n";
    }
    return text;
}

std::string interface_detail(const InterfaceInfo& entry)
{
    std::string text = entry.ethernet ? std::string("Ethernet") : "not Ethernet (link type " + std::to_string(entry.link_type) + ")";
    text += entry.up ? ", up" : ", down";
    if (entry.up && entry.carrier)
    {
        text += *entry.carrier ? ", connected" : ", disconnected";
    }
    return text;
}

} // namespace vehicle_sil::eth_link
