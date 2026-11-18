// SPDX-License-Identifier: MIT
#include "vehicle_sil/bridge_runtime/link_line.hpp"

namespace vehicle_sil::bridge_runtime {

std::string format_link_line(std::string_view key, std::string_view value)
{
    std::string line = "link ";
    line += key;
    line += '=';
    line += value;
    return line;
}

} // namespace vehicle_sil::bridge_runtime
