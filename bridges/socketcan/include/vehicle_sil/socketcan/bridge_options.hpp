// SPDX-License-Identifier: MIT
//
// Command line of vehicle_sil_socketcan_bridge.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <string_view>

namespace vehicle_sil::socketcan {

/// The longest --start-timeout-ms: the longest that vehicle_sil_launch gives.
inline constexpr std::uint64_t kMaxStartTimeoutMs = 1'000'000'000'000;
/// How long the bridge waits for its participant to run in SIL Kit without --start-timeout-ms.
inline constexpr std::chrono::milliseconds kDefaultStartTimeout{15000};
/// The range of --stats-every-ms.
inline constexpr std::uint64_t kMinStatisticsIntervalMs = 100;
inline constexpr std::uint64_t kMaxStatisticsIntervalMs = 3'600'000;

struct BridgeOptions
{
    bool help = false;
    bool list_channels = false;
    /// --json (with --list-channels only): write the list as JSON.
    bool json = false;
    /// --describe: write the bridge's description and exit, without SocketCAN or SIL Kit.
    bool describe = false;

    std::string registry_uri;
    /// --name, or --participant (the same option).
    std::string participant = "SocketCanBridge";
    std::chrono::milliseconds start_timeout = kDefaultStartTimeout;
    /// --stats-every-ms: print the statistics line while running, at this interval where it changed.
    std::optional<std::chrono::milliseconds> statistics_interval;
    std::string network = "J1939_CAN1";

    /// --interface: the SocketCAN interface ("can0", "vcan0").
    std::string interface_name;
    /// --bitrate: the bit rate the interface is expected to run at. The bridge never sets it.
    std::optional<std::uint32_t> bitrate;
    /// --tx-timeout-ms: drop frames while the bus took none for this long. Unset: they wait.
    std::optional<std::chrono::milliseconds> tx_timeout;
};

class BridgeUsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Parses the arguments (without the program name). Throws BridgeUsageError.
[[nodiscard]] BridgeOptions parse_bridge_options(const std::vector<std::string_view>& args);

[[nodiscard]] std::string bridge_usage();

} // namespace vehicle_sil::socketcan
