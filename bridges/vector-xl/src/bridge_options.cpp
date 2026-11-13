// SPDX-License-Identifier: MIT
#include "vehicle_sil/vector_xl/bridge_options.hpp"

#include <charconv>
#include <limits>

namespace vehicle_sil::vector_xl {
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
           "  vehicle_sil_vector_xl_bridge --list-channels\n"
           "  vehicle_sil_vector_xl_bridge --registry-uri <silkit://host:port> --xl-target <virtual|physical>\n"
           "                               --xl-channel <n> [options]\n"
           "\n"
           "Forwards classic CAN frames in both directions between a SIL Kit CAN network and a\n"
           "Vector XL CAN channel until Ctrl+C.\n"
           "\n"
           "  --list-channels          print the CAN channels the XL driver offers and exit\n"
           "  --registry-uri <uri>     SIL Kit registry\n"
           "  --participant <name>     SIL Kit participant name (VectorXlBridge)\n"
           "  --network <name>         SIL Kit CAN network (J1939_CAN1)\n"
           "  --xl-target <target>     virtual: Vector Virtual CAN, physical: CAN hardware (CANcase, VN16xx)\n"
           "  --xl-channel <n>         1-based channel number within the target, see --list-channels\n"
           "  --bitrate <bit/s>        configure the XL channel bit rate, 15000..1000000 (needs init access);\n"
           "                           without it the channel keeps its configuration\n"
           "  --rx-queue-size <n>      XL receive queue size in events, power of two, 16..32768 (16384)\n"
           "  --tx-timeout-ms <ms>     drop frames while the bus took none for this long, 100..60000 (no ACK,\n"
           "                           bus off, cable pulled); without it they wait and go out late.\n"
           "                           Needs --bitrate (init access), not used without it\n"
           "  --bus-off-restart-ms <ms> after the controller was bus off for this long, take the channel\n"
           "                           off the bus and back on to leave it (1000; 0: never; 100..60000).\n"
           "                           Needs --bitrate (init access), not used without it\n"
           "  --no-bus-monitor         do not ask the XL driver for the bus state (a removed device goes\n"
           "                           unnoticed; 'XL bus:' lines come only from what the driver reports\n"
           "                           by itself)\n"
           "  --help                   this text\n";
}

BridgeOptions parse_bridge_options(const std::vector<std::string_view>& args)
{
    BridgeOptions options;
    bool haveTarget = false;
    bool haveChannel = false;

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
        if (arg == "--no-bus-monitor")
        {
            options.bus_monitor = false;
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
        else if (key == "participant")
        {
            options.participant = value;
        }
        else if (key == "network")
        {
            options.network = value;
        }
        else if (key == "xl-target")
        {
            const auto target = parse_xl_target(value);
            if (!target)
            {
                throw BridgeUsageError("--xl-target must be 'virtual' or 'physical', got: " + std::string(value));
            }
            options.target = *target;
            haveTarget = true;
        }
        else if (key == "xl-channel")
        {
            options.channel = static_cast<unsigned>(parse_unsigned(key, value, 64));
            if (options.channel == 0)
            {
                throw BridgeUsageError("--xl-channel is 1-based");
            }
            haveChannel = true;
        }
        else if (key == "bitrate")
        {
            const auto bitrate = parse_unsigned(key, value, 1'000'000);
            if (bitrate < 15'000) // xlCanSetChannelBitrate: 15000 ... 1000000 (XL manual)
            {
                throw BridgeUsageError("--bitrate must be between 15000 and 1000000 bit/s");
            }
            options.bitrate = static_cast<std::uint32_t>(bitrate);
        }
        else if (key == "rx-queue-size")
        {
            // The XL driver accepts 16..32768 events for a classic CAN port.
            const auto size = parse_unsigned(key, value, std::uint64_t{1} << 15);
            if (size < 16 || (size & (size - 1)) != 0)
            {
                throw BridgeUsageError("--rx-queue-size must be a power of two between 16 and 32768");
            }
            options.rx_queue_size = static_cast<std::uint32_t>(size);
        }
        else if (key == "bus-off-restart-ms")
        {
            const auto ms = parse_unsigned(key, value, 60'000);
            if (ms != 0 && ms < 100)
            {
                throw BridgeUsageError("--bus-off-restart-ms must be 0 or between 100 and 60000");
            }
            options.bus_off_restart = std::chrono::milliseconds(ms);
        }
        else if (key == "tx-timeout-ms")
        {
            // Shorter, a healthy but busy bus would count as stuck: at low bit rates one frame
            // takes over 10 ms, and a frame can wait behind several of higher priority.
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

    if (options.list_channels)
    {
        return options;
    }
    if (options.registry_uri.empty())
    {
        throw BridgeUsageError("missing --registry-uri");
    }
    if (!haveTarget)
    {
        throw BridgeUsageError("missing --xl-target (virtual or physical)");
    }
    if (!haveChannel)
    {
        throw BridgeUsageError("missing --xl-channel (see --list-channels)");
    }
    if (options.participant.empty() || options.network.empty())
    {
        throw BridgeUsageError("--participant and --network must not be empty");
    }
    return options;
}

} // namespace vehicle_sil::vector_xl
