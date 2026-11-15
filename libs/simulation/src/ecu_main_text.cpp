// SPDX-License-Identifier: MIT
#include "ecu_main_text.hpp"

#include <string>
#include <string_view>

namespace vehicle_sil::ecu_main_detail {

std::string hex_bytes(std::string_view text)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text)
    {
        const auto b = static_cast<unsigned char>(c);
        out += out.empty() ? "" : " ";
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

std::string in_quotes(std::string_view text) { return "\"" + std::string(text) + "\""; }

std::string one_line(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool brk = false;
    for (const char c : text)
    {
        if (c == '\n' || c == '\r')
        {
            brk = true;
            continue;
        }
        if (brk && !out.empty() && out.back() != ' ')
        {
            out += ' ';
        }
        brk = false;
        out += c;
    }
    return out;
}

} // namespace vehicle_sil::ecu_main_detail
