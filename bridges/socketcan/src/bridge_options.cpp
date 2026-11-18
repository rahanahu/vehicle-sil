// SPDX-License-Identifier: MIT
#include "vehicle_sil/socketcan/bridge_options.hpp"

#include <charconv>

namespace vehicle_sil::socketcan {
namespace {

std::uint64_t parse_unsigned(std::string_view key, std::string_view text, std::uint64_t max)
{
    std::uint64_t value = 0;
    const auto* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, 10);
    if (text.empty() || ec != std::errc{} || ptr != end || value > max)
    {
        throw BridgeUsageError("invalid value for --" + std::string(key) + ": " + std::string(text));
    }
    return value;
}

} // namespace

std::string bridge_usage()
{
    return "usage:\n"
           "  vehicle_sil_socketcan_bridge --list-channels [--json]\n"
           "  vehicle_sil_socketcan_bridge --registry-uri <silkit://host:port> --network <name>\n"
           "                                 --interface <name> [options]\n"
           "  vehicle_sil_socketcan_bridge --describe --network <name> --interface <name> [options]\n"
           "\n"
           "Forwards classic CAN frames in both directions between a SIL Kit CAN network and a SocketCAN\n"
           "interface of this computer (a CAN adapter such as can0, or a virtual one such as vcan0) until\n"
           "Ctrl+C. Linux only. It needs no privileges and sets nothing on the interface.\n"
           "\n"
           "  --list-channels          print the CAN interfaces of this computer and exit\n"
           "  --json                   with --list-channels: write the list as JSON to stdout (for programs)\n"
           "  --describe               write what the bridge is (its name and network) as JSON to stdout,\n"
           "                           as an ecu_main executable does for vehicle_sil_launch, and exit\n"
           "                           without opening the interface or joining SIL Kit\n"
           "  --registry-uri <uri>     SIL Kit registry\n"
           "  --name <name>            SIL Kit participant name (SocketCanBridge)\n"
           "  --participant <name>     the same as --name\n"
           "  --start-timeout-ms <ms>  how long to wait for the participant to run in SIL Kit (15000)\n"
           "  --stats-every-ms <ms>    also print the statistics line while running, at this interval\n"
           "                           where it changed, 100..3600000 (without it: once, when stopping)\n"
           "  --network <name>         SIL Kit CAN network (no default)\n"
           "  --interface <name>       the SocketCAN interface, see --list-channels\n"
           "  --bitrate <bit/s>        the bit rate the interface must run at, 15000..1000000. The bridge does\n"
           "                           not set it (that needs root: sudo ip link set can0 type can bitrate N);\n"
           "                           it refuses to start if the interface runs at another rate. A vcan\n"
           "                           interface has no bit rate: the option is noted and ignored\n"
           "  --tx-timeout-ms <ms>     drop frames while the bus took none for this long, 100..60000 (no ACK,\n"
           "                           bus off, cable pulled); without it they wait and go out late\n"
           "  --help                   this text\n";
}

BridgeOptions parse_bridge_options(const std::vector<std::string_view>& args)
{
    BridgeOptions options;
    std::string_view named; // "name" or "participant", whichever was given

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h")
        {
            options.help = true;
            return options;
        }
        if (arg == "--list-channels")
        {
            options.list_channels = true;
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
        if (arg.size() < 3 || arg.substr(0, 2) != "--")
        {
            throw BridgeUsageError("unexpected argument: " + std::string(arg));
        }
        const std::string_view key = arg.substr(2);
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
            const auto ms = parse_unsigned(key, value, kMaxStartTimeoutMs);
            if (ms == 0)
            {
                throw BridgeUsageError("--start-timeout-ms must be between 1 and " + std::to_string(kMaxStartTimeoutMs));
            }
            options.start_timeout = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(ms));
        }
        else if (key == "stats-every-ms")
        {
            std::uint64_t ms = 0;
            const auto* const end = value.data() + value.size();
            const auto [ptr, ec] = std::from_chars(value.data(), end, ms, 10);
            if (value.empty() || ec != std::errc{} || ptr != end || ms < kMinStatisticsIntervalMs || ms > kMaxStatisticsIntervalMs)
            {
                throw BridgeUsageError("--stats-every-ms must be between " + std::to_string(kMinStatisticsIntervalMs) + " and " +
                                       std::to_string(kMaxStatisticsIntervalMs) + ", got: " + std::string(value));
            }
            options.statistics_interval = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(ms));
        }
        else if (key == "network")
        {
            options.network = value;
        }
        else if (key == "interface")
        {
            options.interface_name = value;
        }
        else if (key == "bitrate")
        {
            const auto bitrate = parse_unsigned(key, value, 1'000'000);
            if (bitrate < 15'000)
            {
                throw BridgeUsageError("--bitrate must be between 15000 and 1000000 bit/s");
            }
            options.bitrate = static_cast<std::uint32_t>(bitrate);
        }
        else if (key == "tx-timeout-ms")
        {
            // Shorter, a healthy but busy bus would count as stuck (the same bounds as the XL bridge).
            const auto ms = parse_unsigned(key, value, 60'000);
            if (ms < 100)
            {
                throw BridgeUsageError("--tx-timeout-ms must be between 100 and 60000");
            }
            options.tx_timeout = std::chrono::milliseconds(ms);
        }
        else
        {
            throw BridgeUsageError("unknown option: " + std::string(arg));
        }
    }

    if (options.json && !options.list_channels)
    {
        throw BridgeUsageError("--json is for --list-channels: it writes the list as JSON");
    }
    if (options.list_channels)
    {
        if (options.describe)
        {
            throw BridgeUsageError("--describe and --list-channels exclude each other: give one of them");
        }
        return options;
    }
    if (options.registry_uri.empty() && !options.describe)
    {
        throw BridgeUsageError("missing --registry-uri");
    }
    if (options.interface_name.empty())
    {
        throw BridgeUsageError("missing --interface (see --list-channels), for example --interface can0");
    }
    if (options.interface_name.size() > 15)
    {
        throw BridgeUsageError("--interface '" + options.interface_name + "' is longer than 15 characters, the longest name Linux gives an interface");
    }
    if (options.participant.empty())
    {
        throw BridgeUsageError("--" + std::string(named) + " must not be empty");
    }
    if (options.network.empty())
    {
        throw BridgeUsageError("missing --network: name the SIL Kit CAN network to bridge (there is no default, so "
                               "that the bridge never joins a network by mistake)");
    }
    return options;
}

} // namespace vehicle_sil::socketcan
