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

namespace {

constexpr std::string_view kEthernetTool = "vehicle_sil_ethernet_bridge";
constexpr std::string_view kXlTool = "vehicle_sil_vector_xl_bridge";
constexpr std::string_view kSocketCanTool = "vehicle_sil_socketcan_bridge";

// Every backend of every kind (all OSes): the one place that says which bridge connects what.
// ethernet: npcap (Windows), packet and tap (Linux), all in vehicle_sil_ethernet_bridge. can: xl
// (vehicle_sil_vector_xl_bridge, named on any OS, listed on Windows) and socketcan
// (vehicle_sil_socketcan_bridge, Linux). LIN has none.
constexpr ConnectBackend kBackends[] = {
    {NetworkKind::Ethernet, "npcap", kEthernetTool, false, true, false, true},
    {NetworkKind::Ethernet, "packet", kEthernetTool, true, false, true, false},
    {NetworkKind::Ethernet, "tap", kEthernetTool, true, false, true, false},
    {NetworkKind::Can, "xl", kXlTool, true, true, false, true},
    {NetworkKind::Can, "socketcan", kSocketCanTool, true, false, true, false},
};

bool on_os(const ConnectBackend& b, Os os) { return os == Os::Linux ? b.on_linux : b.on_windows; }

std::string_view os_name(Os os) { return os == Os::Linux ? "Linux" : "Windows"; }

std::string_view default_backend(NetworkKind kind, std::string_view selector, Os os)
{
    if (kind == NetworkKind::Ethernet)
    {
        return os == Os::Linux ? "packet" : "npcap";
    }
    if (kind == NetworkKind::Can)
    {
        return can_target(selector) || os == Os::Windows ? "xl" : "socketcan";
    }
    return {};
}

} // namespace

const ConnectBackend* find_backend(NetworkKind kind, std::string_view name)
{
    for (const ConnectBackend& b : kBackends)
    {
        if (b.kind == kind && b.name == name)
        {
            return &b;
        }
    }
    return nullptr;
}

std::vector<const ConnectBackend*> backends_on(NetworkKind kind, Os os)
{
    std::vector<const ConnectBackend*> out;
    for (const ConnectBackend& b : kBackends)
    {
        if (b.kind == kind && on_os(b, os))
        {
            out.push_back(&b);
        }
    }
    return out;
}

std::vector<const ConnectBackend*> listed_backends(NetworkKind kind, Os os)
{
    std::vector<const ConnectBackend*> out;
    for (const ConnectBackend& b : kBackends)
    {
        if (b.kind == kind && (os == Os::Linux ? b.listed_on_linux : b.listed_on_windows))
        {
            out.push_back(&b);
        }
    }
    return out;
}

ConnectTarget read_connect_target(NetworkKind kind, std::string_view target, Os os)
{
    ConnectTarget out;
    if (target == kNoTarget)
    {
        out.none = true;
        return out;
    }
    out.selector = target;
    if (const std::size_t colon = target.find(':'); colon != std::string_view::npos)
    {
        if (const ConnectBackend* backend = find_backend(kind, target.substr(0, colon)))
        {
            out.prefixed = true;
            out.backend = backend->name;
            out.selector = target.substr(colon + 1);
            return out;
        }
    }
    out.backend = default_backend(kind, out.selector, os);
    return out;
}

std::string_view bridge_tool(NetworkKind kind, std::string_view backend)
{
    const ConnectBackend* row = find_backend(kind, backend);
    return row ? row->tool : std::string_view{};
}

bool is_bridge_tool(std::string_view tool)
{
    return std::any_of(std::begin(kBackends), std::end(kBackends), [tool](const ConnectBackend& b) { return b.tool == tool; });
}

bool is_can_bridge_tool(std::string_view tool)
{
    return std::any_of(std::begin(kBackends), std::end(kBackends),
                       [tool](const ConnectBackend& b) { return b.kind == NetworkKind::Can && b.tool == tool; });
}

std::optional<std::string> backend_problem(NetworkKind kind, std::string_view target, Os os)
{
    const ConnectTarget read = read_connect_target(kind, target, os);
    if (!read.prefixed || on_os(*find_backend(kind, read.backend), os))
    {
        return std::nullopt;
    }
    std::string names;
    for (const ConnectBackend* b : backends_on(kind, os))
    {
        names += (names.empty() ? "" : ", ") + std::string(b->name);
    }
    return "the backend '" + std::string(read.backend) + "' is not for " + std::string(os_name(os)) + ": the " +
           std::string(to_string(kind)) + " backends here are " + names + " (give the target without the prefix to take the default, " +
           std::string(default_backend(kind, "", os)) + "); vehicle_sil_launch connections <file> lists the targets of each";
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

std::optional<std::string> no_bridge_here(NetworkKind kind, std::string_view backend, Os os)
{
    if (kind == NetworkKind::Lin)
    {
        return std::string("there is no bridge for LIN");
    }
    if (kind == NetworkKind::Can && backend == "xl" && os != Os::Windows)
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

std::optional<std::string> target_problem(NetworkKind kind, std::string_view target, Os os)
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
    if (auto problem = backend_problem(kind, target, os))
    {
        return problem;
    }
    const ConnectTarget read = read_connect_target(kind, target, os);
    if (read.selector.empty())
    {
        return "the target has nothing after '" + std::string(read.backend) + ":': give the " +
               (kind == NetworkKind::Can ? "channel or interface" : "interface") + " name";
    }
    if (read.selector.front() == '-')
    {
        return "the target starts with '-', which the bridge would take for an option";
    }
    if (kind == NetworkKind::Can && read.backend == "xl" && !can_target(read.selector))
    {
        return "a Vector XL target is virtual:<n> or physical:<n>, with the channel number <n> from 1";
    }
    return std::nullopt;
}

std::vector<std::string> bridge_arguments(const NetworkConfig& network, std::string_view target,
                                          const std::vector<eth::MacAddress>& macs, Os os)
{
    std::vector<std::string> out{"--network", network.name};
    const NetworkExternal external = network.external.value_or(NetworkExternal{});
    const ConnectTarget read = read_connect_target(network.kind, target, os);
    if (network.kind == NetworkKind::Ethernet)
    {
        out.insert(out.end(), {"--backend", std::string(read.backend), "--interface", std::string(read.selector)});
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
        if (read.backend == "socketcan")
        {
            out.insert(out.end(), {"--interface", std::string(read.selector)});
        }
        else
        {
            const std::optional<CanTarget> can = can_target(read.selector);
            out.insert(out.end(), {"--xl-target", std::string(can ? can->xl_target : ""), "--xl-channel",
                                   std::string(can ? can->channel : "")});
        }
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

std::string connect_device_key(NetworkKind kind, std::string_view target, Os os)
{
    if (kind == NetworkKind::Lin || target_problem(kind, target, os))
    {
        return {};
    }
    if (target == kNoTarget)
    {
        return {};
    }
    const ConnectTarget read = read_connect_target(kind, target, os);
    std::string key(read.selector);
    if (kind == NetworkKind::Can && read.backend == "xl")
    {
        const std::optional<CanTarget> can = can_target(read.selector);
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
    if (os == Os::Windows)
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

std::string tool_missing_fix(std::string_view tool)
{
    const char* how = tool == kXlTool        ? "build it with the Vector XL Driver Library (docs/bridges.md)"
                      : tool == kSocketCanTool ? "build it on Linux (docs/socketcan-bridge.md)"
                                               : "build it (docs/ethernet-bridge.md)";
    return std::string(how) + " and put it next to vehicle_sil_launch, or its directory on PATH";
}

} // namespace vehicle_sil::launch
