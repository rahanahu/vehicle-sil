// SPDX-License-Identifier: MIT
//
// --connect: the request, the form of a target, the bridge of each kind of network, its
// arguments, and where the launcher looks for it (connect.hpp). The check of the requests against
// a configuration is in check_connect.cpp.
#include "vehicle_sil/launch/connect.hpp"

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>

namespace vehicle_sil::launch {

namespace {

/// A CAN target split: the XL target ("virtual" or "physical") and the channel number.
struct CanTarget
{
    std::string_view xl_target;
    std::string_view channel;
};

/// `target` as virtual:<n> or physical:<n> (n: decimal digits, from 1, at most 9 of them), or empty.
std::optional<CanTarget> can_target(std::string_view target)
{
    const std::size_t colon = target.find(':');
    if (colon == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string_view kind = target.substr(0, colon);
    const std::string_view number = target.substr(colon + 1);
    if ((kind != "virtual" && kind != "physical") || number.empty() || number.size() > 9 ||
        !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    std::from_chars(number.data(), number.data() + number.size(), value);
    if (value == 0)
    {
        return std::nullopt;
    }
    return CanTarget{kind, number};
}

std::string hex4(std::uint16_t value)
{
    char text[8];
    std::snprintf(text, sizeof(text), "0x%04x", static_cast<unsigned>(value));
    return text;
}

} // namespace

std::optional<ConnectRequest> parse_connect(std::string_view text)
{
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0)
    {
        return std::nullopt;
    }
    return ConnectRequest{std::string(text.substr(0, equals)), std::string(text.substr(equals + 1))};
}

std::string_view bridge_tool(NetworkKind kind)
{
    switch (kind)
    {
    case NetworkKind::Ethernet:
        return "vehicle_sil_ethernet_bridge";
    case NetworkKind::Can:
        return "vehicle_sil_vector_xl_bridge";
    case NetworkKind::Lin:
        break;
    }
    return "";
}

std::size_t detail::unicode_control_at(std::string_view text) noexcept
{
    const auto at = [&text](std::size_t i) { return i < text.size() ? static_cast<unsigned char>(text[i]) : 0u; };
    if (at(0) == 0xC2 && at(1) >= 0x80 && at(1) <= 0x9F)
    {
        return 2;
    }
    if (at(0) == 0xE2 && ((at(1) == 0x80 && at(2) >= 0xAA && at(2) <= 0xAE) || (at(1) == 0x81 && at(2) >= 0xA6 && at(2) <= 0xA9)))
    {
        return 3;
    }
    return 0;
}

bool detail::has_unicode_control(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (unicode_control_at(text.substr(i)) != 0)
        {
            return true;
        }
    }
    return false;
}

std::optional<std::string> no_bridge_here(NetworkKind kind, bool can_bridge)
{
    if (kind == NetworkKind::Lin)
    {
        return std::string("there is no bridge for LIN");
    }
    if (kind == NetworkKind::Can && !can_bridge)
    {
        return std::string("the Vector XL bridge is for Windows");
    }
    return std::nullopt;
}

std::filesystem::path bridge_working_directory(const std::filesystem::path& tool)
{
    return std::filesystem::absolute(tool).parent_path();
}

std::string bridge_participant(std::string_view network) { return "bridge." + std::string(network); }

std::optional<std::string> target_problem(NetworkKind kind, std::string_view target)
{
    if (target == kNoTarget)
    {
        return std::nullopt;
    }
    if (target.empty())
    {
        return "the target is empty";
    }
    if (target.size() > kMaxTargetBytes)
    {
        return "the target is longer than " + std::to_string(kMaxTargetBytes) + " bytes";
    }
    if (!vehicle_sil::detail::is_utf8(target))
    {
        return "the target is not UTF-8 text";
    }
    if (std::any_of(target.begin(), target.end(),
                    [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; }) ||
        detail::has_unicode_control(target))
    {
        return "the target has a control character";
    }
    if (target.front() == '-')
    {
        return "the target starts with '-', which the bridge would take for an option";
    }
    if (kind == NetworkKind::Can && !can_target(target))
    {
        return "a CAN target is virtual:<n> or physical:<n>, with the channel number <n> from 1";
    }
    return std::nullopt;
}

std::vector<std::string> bridge_arguments(const NetworkConfig& network, std::string_view target,
                                          const std::vector<eth::MacAddress>& macs)
{
    std::vector<std::string> out{"--network", network.name};
    const NetworkExternal external = network.external.value_or(NetworkExternal{});
    if (network.kind == NetworkKind::Ethernet)
    {
        out.insert(out.end(), {"--interface", std::string(target)});
        if (external.all_frames)
        {
            out.emplace_back("--all-frames");
        }
        else if (!external.ethertypes.empty())
        {
            std::string list;
            for (const std::uint16_t type : external.ethertypes)
            {
                list += (list.empty() ? "" : ",") + hex4(type);
            }
            out.insert(out.end(), {"--ethertype", list});
        }
        else
        {
            std::vector<eth::MacAddress> once;
            std::string list;
            for (const eth::MacAddress& mac : macs)
            {
                if (std::find(once.begin(), once.end(), mac) == once.end())
                {
                    once.push_back(mac);
                    list += (list.empty() ? "" : ",") + mac.to_string();
                }
            }
            if (!list.empty())
            {
                out.insert(out.end(), {"--src-mac", list});
            }
        }
    }
    else if (network.kind == NetworkKind::Can)
    {
        const std::optional<CanTarget> can = can_target(target);
        out.insert(out.end(), {"--xl-target", std::string(can ? can->xl_target : ""), "--xl-channel",
                               std::string(can ? can->channel : "")});
        if (external.bitrate)
        {
            out.insert(out.end(), {"--bitrate", std::to_string(*external.bitrate)});
        }
    }
    return out;
}

std::vector<std::string> bridge_run_arguments(const NetworkOutside& outside, std::string_view registry_uri,
                                              std::int64_t start_timeout_ms)
{
    std::vector<std::string> out = outside.arguments.value_or(std::vector<std::string>{});
    out.insert(out.end(), {"--name", outside.participant, "--registry-uri", std::string(registry_uri), "--start-timeout-ms",
                           std::to_string(start_timeout_ms), "--stats-every-ms", std::to_string(kBridgeStatisticsEveryMs)});
    return out;
}

std::string connect_device_key(NetworkKind kind, std::string_view target, bool ignore_case)
{
    if (kind == NetworkKind::Lin || target_problem(kind, target))
    {
        return {};
    }
    if (target == kNoTarget)
    {
        return {};
    }
    if (kind == NetworkKind::Can)
    {
        const std::optional<CanTarget> can = can_target(target);
        if (!can)
        {
            return {};
        }
        if (can->xl_target == "virtual")
        {
            return "virtual";
        }
        std::uint32_t value = 0;
        std::from_chars(can->channel.data(), can->channel.data() + can->channel.size(), value);
        return "physical:" + std::to_string(value);
    }
    std::string key(target);
    if (ignore_case)
    {
        for (char& c : key)
        {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    }
    return key;
}

bool networks_are_joined(const std::vector<std::vector<std::string>>& participant_networks, const std::string& a,
                         const std::string& b)
{
    std::vector<std::string> reached{a};
    for (bool grown = true; grown;)
    {
        grown = false;
        for (const std::vector<std::string>& networks : participant_networks)
        {
            const bool touches = std::any_of(networks.begin(), networks.end(), [&](const std::string& n) {
                return std::find(reached.begin(), reached.end(), n) != reached.end();
            });
            if (!touches)
            {
                continue;
            }
            for (const std::string& n : networks)
            {
                if (std::find(reached.begin(), reached.end(), n) == reached.end())
                {
                    reached.push_back(n);
                    grown = true;
                }
            }
        }
    }
    return std::find(reached.begin(), reached.end(), b) != reached.end();
}

std::string tool_not_found(const std::filesystem::path& launcher_dir)
{
    return launcher_dir.empty() ? std::string("not on PATH")
                                : "neither next to vehicle_sil_launch (" + process::shown_path(launcher_dir) + ") nor on PATH";
}

std::string tool_missing_fix(NetworkKind kind)
{
    return std::string(kind == NetworkKind::Can ? "build it with the Vector XL Driver Library (docs/bridges.md)"
                                                : "build it (docs/ethernet-bridge.md)") +
           " and put it next to vehicle_sil_launch, or its directory on PATH";
}

} // namespace vehicle_sil::launch
