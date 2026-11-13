// SPDX-License-Identifier: MIT
//
// The words that more than one reader or output of the check puts into its messages: lists, the
// kinds and uses of network as the messages list them, a step, an address. Pure functions.
// Internal.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/j1939/types.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace vehicle_sil::launch::detail {

/// "A, B, C".
inline std::string join(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out;
}

/// "A and B", "A, B and C" (`last` "or": "A, B or C").
inline std::string and_list(const std::vector<std::string>& items, const char* last = " and ")
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        out += (i == 0 ? "" : i + 1 == items.size() ? last : ", ") + items[i];
    }
    return out;
}

/// "A or B", "A, B or C".
inline std::string or_list(const std::vector<std::string>& items) { return and_list(items, " or "); }

/// The names of the uses of a network (kNetworkUses): "raw", "j1939", ...
inline std::vector<std::string> network_use_names()
{
    std::vector<std::string> out;
    for (const NetworkUse use : kNetworkUses)
    {
        out.push_back(to_string(use));
    }
    return out;
}

/// "10 ms", "500 us" or "250 ns".
inline std::string step_text(SimDuration step)
{
    const auto ns = step.count();
    if (ns % 1'000'000 == 0)
    {
        return std::to_string(ns / 1'000'000) + " ms";
    }
    if (ns % 1'000 == 0)
    {
        return std::to_string(ns / 1'000) + " us";
    }
    return std::to_string(ns) + " ns";
}

/// "0x80".
inline std::string hex2(unsigned value)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%02X", value & 0xFFu);
    return buffer;
}

} // namespace vehicle_sil::launch::detail
