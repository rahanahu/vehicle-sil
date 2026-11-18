// SPDX-License-Identifier: MIT
//
// The words that more than one reader or output of the check puts into its messages: lists, the
// kinds and uses of network as the messages list them, a step, an address, a time in
// milliseconds. Pure functions. Internal.
#pragma once

#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/j1939/types.hpp"
#include "vehicle_sil/config/config.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
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

/// The exit status with which Windows ends a process before main() when a DLL that the executable
/// imports is missing (STATUS_DLL_NOT_FOUND) or lacks a function it imports
/// (STATUS_ENTRYPOINT_NOT_FOUND). No other system gives these exit codes, so they are checked
/// everywhere.
inline constexpr std::uint32_t kStatusDllNotFound = 0xC0000135u;
inline constexpr std::uint32_t kStatusEntryPointNotFound = 0xC0000139u;

/// "0xC0000135".
inline std::string status_hex(std::uint32_t code)
{
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(code));
    return hex;
}

/// How an executable that Windows did not start because of a DLL ended (`exit_code` is
/// kStatusDllNotFound or kStatusEntryPointNotFound): "could not start (exit status 0xC0000135):
/// Windows did not find a DLL that it needs"; "" for another exit code. The executable's standard
/// error is empty then: nothing says which DLL.
inline std::string dll_start_failure(int exit_code)
{
    const auto code = static_cast<std::uint32_t>(exit_code);
    if (code != kStatusDllNotFound && code != kStatusEntryPointNotFound)
    {
        return {};
    }
    return "could not start (exit status " + status_hex(code) +
           (code == kStatusDllNotFound ? "): Windows did not find a DLL that it needs"
                                       : "): a DLL that it needs is of another version (a function in it is missing)");
}

/// What to do after dll_start_failure().
inline constexpr const char* kDllFix =
    "put that DLL, of the version the executable was built with, next to the executable or in a folder on PATH";

/// What to do after dll_start_failure() of the bridge of a network of `kind`: the Vector XL bridge
/// needs the DLL of the Vector XL Driver Library first.
inline std::string bridge_dll_fix(NetworkKind kind)
{
    return kind == NetworkKind::Can ? std::string("vehicle_sil_vector_xl_bridge needs vxlapi64.dll of the Vector XL Driver "
                                                  "Library: install the library, or put vxlapi64.dll next to the bridge; for "
                                                  "another DLL, ") +
                                          kDllFix
                                    : std::string(kDllFix);
}

/// What to do about an ECU whose control agent speaks `version` of the control messages, not
/// control::wire::kVersion: which side is the older one, and what to build or use again
/// (control-link-version of the check and of the run).
inline std::string control_version_fix(std::int64_t version)
{
    return version < control::wire::kVersion
               ? "the ECU is of an older vehicle-sil; build it again with the vehicle-sil of this vehicle_sil_launch"
               : "the ECU is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a newer one";
}

/// A line of a bridge's output without the tag that the bridge puts first: "[" and a word without a
/// space, then "] " ("[eth-bridge] ", "[bridge] "). The line as it is if it has none.
inline std::string_view without_tag(std::string_view line)
{
    if (line.size() > 2 && line.front() == '[')
    {
        if (const auto close = line.find("] ");
            close != std::string_view::npos && close > 1 && line.substr(1, close - 1).find(' ') == std::string_view::npos)
        {
            line.remove_prefix(close + 2);
        }
    }
    return line;
}

/// The line of a tool's output that says why it failed, from its lines one at a time. A line that
/// says so starts with "error:" (an argument the bridges refuse; they write the usage hint after it)
/// or "FAIL" (a bridge that cannot set up, or fails while it runs; it writes what it did since, its
/// statistics and "stopped" after it), after the tag a bridge puts first ("[eth-bridge] ",
/// "[bridge] "). Without "\r", and cut after 300 bytes. check uses it on a bridge's --describe
/// (text()), run on the lines of the bridges it starts (failure()).
class LastLine
{
public:
    void add(std::string_view line)
    {
        std::string text(line);
        std::erase(text, '\r');
        const std::string_view body = without_tag(text);
        if (body.rfind("error:", 0) == 0 || body.rfind("FAIL", 0) == 0)
        {
            failure_ = text;
        }
        if (!text.empty())
        {
            last_ = std::move(text);
        }
    }

    /// The last line that says why it failed, or else the last line that is not empty.
    [[nodiscard]] std::string text() const { return cut(failure_.empty() ? last_ : failure_); }

    /// The last line that says why it failed, or "" (a tool that was killed, or ended without
    /// saying why: its last line would be about something else).
    [[nodiscard]] std::string failure() const { return cut(failure_); }

private:
    static std::string cut(const std::string& line) { return line.size() > 300 ? line.substr(0, 300) + "..." : line; }

    std::string last_;
    std::string failure_;
};

inline constexpr std::int64_t kNanosPerMilli = 1'000'000;

/// Nanoseconds as milliseconds in the shortest form ("10", "2.5").
inline std::string millis_text(std::int64_t ns)
{
    return speed_text(static_cast<double>(ns) / static_cast<double>(kNanosPerMilli));
}

} // namespace vehicle_sil::launch::detail
