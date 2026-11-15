// SPDX-License-Identifier: MIT
#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"

#include <charconv>
#include <cstdint>

namespace vehicle_sil::ethernet_bridge {
namespace {

std::vector<std::string_view> split_commas(std::string_view text)
{
    std::vector<std::string_view> items;
    std::size_t begin = 0;
    for (;;)
    {
        const auto comma = text.find(',', begin);
        items.push_back(text.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (comma == std::string_view::npos)
        {
            return items;
        }
        begin = comma + 1;
    }
}

/// "0x88b5" (1 to 4 hex digits after 0x, either case).
std::optional<std::uint16_t> parse_hex16(std::string_view text)
{
    if (text.size() < 3 || text.size() > 6 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
    {
        return std::nullopt;
    }
    std::uint16_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data() + 2, end, value, 16);
    if (ec != std::errc{} || ptr != end)
    {
        return std::nullopt;
    }
    return value;
}

std::vector<std::uint16_t> parse_hex_list(std::string_view option, std::string_view value, std::string_view what,
                                          std::string_view example)
{
    std::vector<std::uint16_t> values;
    for (const auto item : split_commas(value))
    {
        const auto parsed = parse_hex16(item);
        if (!parsed)
        {
            throw BridgeUsageError("invalid value for --" + std::string(option) + ": '" + std::string(value) + "': give " +
                                   std::string(what) + " as hex numbers with 0x, separated by commas, e.g. " +
                                   std::string(example));
        }
        values.push_back(*parsed);
    }
    return values;
}

std::vector<eth_link::MacAddress> parse_mac_list(std::string_view value)
{
    std::vector<eth_link::MacAddress> macs;
    for (const auto item : split_commas(value))
    {
        const auto parsed = eth_link::parse_mac_address(item);
        if (!parsed)
        {
            throw BridgeUsageError("invalid value for --src-mac: '" + std::string(value) +
                                   "': give MAC addresses as six hex bytes, separated by commas, e.g. "
                                   "02:00:00:00:00:01,02:00:00:00:00:02");
        }
        if (parsed->is_group())
        {
            throw BridgeUsageError("--src-mac " + std::string(item) +
                                   " is a group address: give the individual MAC addresses of the SIL Kit side");
        }
        macs.push_back(*parsed);
    }
    return macs;
}

std::size_t parse_size(std::string_view option, std::string_view text, std::size_t min, std::size_t max)
{
    std::size_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, 10);
    if (text.empty() || ec != std::errc{} || ptr != end || value < min || value > max)
    {
        throw BridgeUsageError("--" + std::string(option) + " must be between " + std::to_string(min) + " and " +
                               std::to_string(max) + ", got: " + std::string(text));
    }
    return value;
}

std::chrono::milliseconds parse_start_timeout(std::string_view text)
{
    std::uint64_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, 10);
    if (text.empty() || ec != std::errc{} || ptr != end || value == 0 || value > kMaxStartTimeoutMs)
    {
        throw BridgeUsageError("--start-timeout-ms must be between 1 and " + std::to_string(kMaxStartTimeoutMs) +
                               ", got: " + std::string(text));
    }
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

std::chrono::milliseconds parse_statistics_interval(std::string_view text)
{
    std::uint64_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, 10);
    if (text.empty() || ec != std::errc{} || ptr != end || value < kMinStatisticsIntervalMs || value > kMaxStatisticsIntervalMs)
    {
        throw BridgeUsageError("--stats-every-ms must be between " + std::to_string(kMinStatisticsIntervalMs) + " and " +
                               std::to_string(kMaxStatisticsIntervalMs) + ", got: " + std::string(text));
    }
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

} // namespace

std::string_view to_string(Backend backend) noexcept
{
    switch (backend)
    {
    case Backend::Tap: return "tap";
    case Backend::Packet: return "packet";
    case Backend::Npcap: return "npcap";
    case Backend::Pcap: return "pcap";
    }
    return "unknown";
}

std::optional<Backend> parse_backend(std::string_view text) noexcept
{
    for (const auto backend : {Backend::Tap, Backend::Packet, Backend::Npcap, Backend::Pcap})
    {
        if (text == to_string(backend))
        {
            return backend;
        }
    }
    return std::nullopt;
}

bool reaches_real_adapters(Backend backend) noexcept
{
    return backend != Backend::Tap;
}

std::string bridge_usage()
{
    return "usage:\n"
           "  vehicle_sil_ethernet_bridge --list-interfaces [--backend <backend>] [--json]\n"
           "  vehicle_sil_ethernet_bridge --registry-uri <silkit://host:port> --network <name>\n"
           "                              [--backend <backend>] --interface <name> [options]\n"
           "  vehicle_sil_ethernet_bridge --describe --network <name> [--backend <backend>]\n"
           "                              --interface <name> [options]\n"
           "\n"
           "Forwards Ethernet frames in both directions between a SIL Kit Ethernet network and an\n"
           "Ethernet interface outside SIL Kit until Ctrl+C.\n"
           "\n"
           "  --list-interfaces        print the interfaces for the backend and exit: those of this computer\n"
           "                           for packet and tap, those the pcap library offers for npcap and pcap\n"
           "  --json                   with --list-interfaces: write the list as JSON to stdout (for programs)\n"
           "  --describe               write what the bridge is (its name and network) as JSON to stdout,\n"
           "                           as an ecu_main executable does for vehicle_sil_launch, and exit\n"
           "                           without opening the interface or joining SIL Kit\n"
           "  --registry-uri <uri>     SIL Kit registry\n"
           "  --name <name>            SIL Kit participant name (EthernetBridge)\n"
           "  --participant <name>     the same as --name\n"
           "  --start-timeout-ms <ms>  how long to wait for the participant to run in SIL Kit (15000)\n"
           "  --stats-every-ms <ms>    also print the statistics line while running, at this interval\n"
           "                           where it changed, 100..3600000 (without it: once, when stopping)\n"
           "  --network <name>         SIL Kit Ethernet network (no default)\n"
           "  --backend <backend>      tap: a TAP device (Linux); packet: a network interface (Linux);\n"
           "                           npcap: a network adapter through Npcap (Windows);\n"
           "                           pcap: a network interface through libpcap (Linux).\n"
           "                           Default: packet on Linux, npcap on Windows\n"
           "  --interface <name>       the interface to bridge, see --list-interfaces\n"
           "  --ethertype <hex>,...    pass only these EtherTypes (behind up to two VLAN tags), e.g. 0x88b5\n"
           "  --src-mac <mac>,...      pass only frames from these SIL Kit MAC addresses out, and only\n"
           "                           frames to them or to group addresses in\n"
           "  --all-frames             pass every frame. With packet, npcap and pcap, one of\n"
           "                           --ethertype, --src-mac and --all-frames is required\n"
           "  --vlan-tpid <hex>,...    the values that mark a VLAN tag (0x8100,0x88a8)\n"
           "  --max-frame-bytes <n>    drop longer frames, without FCS, 60..65535 (1522)\n"
           "  --promiscuous            receive frames to every address, or fail if the interface refuses\n"
           "                           (packet, npcap, pcap). Without it and --no-promiscuous: receive\n"
           "                           frames to every address where the interface accepts it, else\n"
           "                           go on without and say so\n"
           "  --no-promiscuous         receive only frames to the interface's own address and to group\n"
           "                           addresses (packet, npcap, pcap)\n"
           "  --tap-create             create the TAP device while the bridge runs (tap)\n"
           "  --no-mac-guard           do not drop frames by the source MAC rule against loops\n"
           "  --mac-table-size <n>     MAC addresses the rule remembers, 16..65536 (1024)\n"
           "  --help                   this text\n";
}

BridgeOptions parse_bridge_options(const std::vector<std::string_view>& args, std::optional<Backend> default_backend)
{
    BridgeOptions options;
    auto& filter = options.bridge.filter;
    std::string_view named; // "name" or "participant", whichever was given
    std::string_view promiscuousGiven; // "--promiscuous" or "--no-promiscuous", whichever was given

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h")
        {
            options.help = true;
            return options;
        }
        if (arg == "--list-interfaces")
        {
            options.list_interfaces = true;
            continue;
        }
        if (arg == "--describe")
        {
            options.describe = true;
            continue;
        }
        if (arg == "--json")
        {
            options.json = true;
            continue;
        }
        if (arg == "--all-frames")
        {
            options.all_frames = true;
            continue;
        }
        if (arg == "--promiscuous" || arg == "--no-promiscuous")
        {
            const auto mode = arg == "--promiscuous" ? eth_link::Promiscuous::On : eth_link::Promiscuous::Off;
            if (!promiscuousGiven.empty() && promiscuousGiven != arg)
            {
                throw BridgeUsageError("--promiscuous and --no-promiscuous contradict each other: give one of them, or "
                                       "neither (the bridge then uses promiscuous mode where the interface accepts it)");
            }
            promiscuousGiven = arg;
            options.promiscuous = mode;
            continue;
        }
        if (arg == "--tap-create")
        {
            options.tap_create = true;
            continue;
        }
        if (arg == "--no-mac-guard")
        {
            options.bridge.mac_guard = false;
            continue;
        }
        if (arg.size() < 3 || arg.substr(0, 2) != "--")
        {
            throw BridgeUsageError("unexpected argument: " + std::string(arg) + " (options start with --; see --help)");
        }
        const std::string_view key = arg.substr(2);
        const bool known = key == "registry-uri" || key == "participant" || key == "name" || key == "network" ||
                           key == "backend" || key == "interface" || key == "ethertype" || key == "src-mac" ||
                           key == "vlan-tpid" || key == "max-frame-bytes" || key == "mac-table-size" ||
                           key == "start-timeout-ms" || key == "stats-every-ms";
        if (!known)
        {
            throw BridgeUsageError("unknown option: " + std::string(arg) + " (see --help)");
        }
        if (i + 1 >= args.size())
        {
            throw BridgeUsageError("missing value for " + std::string(arg));
        }
        const std::string_view value = args[++i];

        if (key == "registry-uri")
        {
            options.registry_uri = value;
        }
        else if (key == "participant" || key == "name")
        {
            if (!named.empty() && named != key)
            {
                throw BridgeUsageError("--name and --participant are the same option: give one of them");
            }
            named = key;
            options.participant = value;
        }
        else if (key == "start-timeout-ms")
        {
            options.start_timeout = parse_start_timeout(value);
        }
        else if (key == "stats-every-ms")
        {
            options.statistics_interval = parse_statistics_interval(value);
        }
        else if (key == "network")
        {
            options.network = value;
        }
        else if (key == "backend")
        {
            options.backend = parse_backend(value);
            if (!options.backend)
            {
                throw BridgeUsageError("--backend must be tap, packet, npcap or pcap, got: " + std::string(value));
            }
        }
        else if (key == "interface")
        {
            options.interface_name = value;
        }
        else if (key == "ethertype")
        {
            const auto types = parse_hex_list(key, value, "EtherTypes", "0x88b5,0x0800");
            filter.ether_types.insert(filter.ether_types.end(), types.begin(), types.end());
        }
        else if (key == "src-mac")
        {
            const auto macs = parse_mac_list(value);
            filter.silkit_macs.insert(filter.silkit_macs.end(), macs.begin(), macs.end());
        }
        else if (key == "vlan-tpid")
        {
            filter.vlan_tpids = parse_hex_list(key, value, "tag identifiers", "0x8100,0x88a8");
        }
        else if (key == "max-frame-bytes")
        {
            filter.max_frame_bytes = parse_size(key, value, kMinMaxFrameBytes, kMaxMaxFrameBytes);
        }
        else // mac-table-size
        {
            options.bridge.mac_table_size = parse_size(key, value, kMinMacTableSize, kMaxMacTableSize);
        }
    }

    if (!options.backend)
    {
        options.backend = default_backend;
    }
    if (options.json && !options.list_interfaces)
    {
        throw BridgeUsageError("--json is for --list-interfaces: it writes the list as JSON");
    }
    if (options.list_interfaces)
    {
        if (options.describe)
        {
            throw BridgeUsageError("--describe and --list-interfaces exclude each other: give one of them");
        }
        return options;
    }
    if (options.registry_uri.empty() && !options.describe)
    {
        throw BridgeUsageError("missing --registry-uri: give the SIL Kit registry, e.g. --registry-uri silkit://127.0.0.1:8500");
    }
    if (options.network.empty())
    {
        throw BridgeUsageError("missing --network: name the SIL Kit Ethernet network to bridge (there is no default, so "
                               "that the bridge never joins a network by mistake)");
    }
    if (options.participant.empty())
    {
        throw BridgeUsageError("--" + std::string(named) + " must not be empty");
    }
    if (!options.backend)
    {
        throw BridgeUsageError("missing --backend: choose tap, packet, npcap or pcap");
    }
    if (options.interface_name.empty())
    {
        throw BridgeUsageError("missing --interface: name the interface to bridge (run --list-interfaces to see them)");
    }
    const bool restricted = !filter.ether_types.empty() || !filter.silkit_macs.empty();
    if (options.all_frames && restricted)
    {
        throw BridgeUsageError("--all-frames passes every frame, so it cannot be combined with --ethertype or "
                               "--src-mac: keep one or the other");
    }
    const auto backend = *options.backend;
    if (reaches_real_adapters(backend) && !options.all_frames && !restricted)
    {
        throw BridgeUsageError("--backend " + std::string(to_string(backend)) +
                               " sends to a real adapter: say what may pass with --ethertype, --src-mac or --all-frames.");
    }
    if (options.tap_create && backend != Backend::Tap)
    {
        throw BridgeUsageError("--tap-create needs --backend tap");
    }
    if (!promiscuousGiven.empty() && !reaches_real_adapters(backend))
    {
        throw BridgeUsageError(std::string(promiscuousGiven) +
                               " needs --backend packet, npcap or pcap: a TAP device gets every frame the OS sends to it "
                               "anyway");
    }
    return options;
}

} // namespace vehicle_sil::ethernet_bridge
