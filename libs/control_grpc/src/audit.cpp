// SPDX-License-Identifier: MIT
#include "audit.hpp"

namespace vehicle_sil::control_grpc::detail {

std::string clean(std::string text)
{
    for (auto& c : text)
    {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
        {
            c = '?';
        }
    }
    return text;
}

std::string cut_utf8(std::string text, std::size_t max)
{
    if (text.size() > max)
    {
        auto cut = max;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        {
            --cut;
        }
        text.resize(cut);
    }
    return text;
}

} // namespace vehicle_sil::control_grpc::detail
