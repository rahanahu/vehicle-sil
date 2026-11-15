// SPDX-License-Identifier: MIT
//
// --connect: the request, the form of a target, the bridge of each kind of network, its
// arguments, and where the launcher looks for it (connect.hpp). The check of the requests against
// a configuration is in check_connect.cpp.
#include "vehicle_sil/launch/connect.hpp"

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
                    [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; }))
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

std::optional<std::filesystem::path> find_tool(std::string_view name, const std::filesystem::path& launcher_dir)
{
    if (!launcher_dir.empty())
    {
        std::filesystem::path beside = resolve_executable(name, launcher_dir);
        if (detail::is_runnable_file(beside))
        {
            return beside;
        }
    }
    return detail::find_on_path(name);
}

} // namespace vehicle_sil::launch
