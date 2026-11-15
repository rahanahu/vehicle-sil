// SPDX-License-Identifier: MIT
//
// Command line of vehicle_sil_vector_xl_bridge. Platform independent so it can be tested anywhere.
#pragma once

#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::vector_xl {

/// The longest --start-timeout-ms: the longest that vehicle_sil_launch gives (kMaxMillis of
/// vehicle_sil/ecu/executable.hpp).
inline constexpr std::uint64_t kMaxStartTimeoutMs = 1'000'000'000'000;
/// How long the bridge waits for its participant to run in SIL Kit without --start-timeout-ms.
inline constexpr std::chrono::milliseconds kDefaultStartTimeout{15000};

struct BridgeOptions
{
    bool help = false;
    bool list_channels = false;
    /// --describe: write the bridge's description and exit, without the XL driver or SIL Kit;
    /// --registry-uri is not needed.
    bool describe = false;

    std::string registry_uri;
    /// --name, or --participant (the same option).
    std::string participant = "VectorXlBridge";
    /// --start-timeout-ms: how long ParticipantRuntime::start() waits for Running.
    std::chrono::milliseconds start_timeout = kDefaultStartTimeout;
    std::string network = "J1939_CAN1";

    XlTarget target = XlTarget::Virtual;
    unsigned channel = 0; ///< 1-based, see selectable_channels()
    /// Bit rate to configure. Without it the channel keeps its current configuration.
    std::optional<std::uint32_t> bitrate;
    /// XL receive queue size in events; the XL driver requires a power of two.
    std::uint32_t rx_queue_size = 16384;
    /// Drop frames that could not reach the bus for this long (XlCanPortOptions::transmit_timeout).
    /// Without it they wait and go out once the bus works again.
    std::optional<std::chrono::milliseconds> tx_timeout;
    /// Ask the driver for the controller's error state (XlCanPortOptions::bus_state_interval).
    bool bus_monitor = true;
    /// Restart the channel after a lasting bus off (XlCanPortOptions::bus_off_restart); 0: never.
    /// Unset: the port's default. Used only with init access.
    std::optional<std::chrono::milliseconds> bus_off_restart;
};

class BridgeUsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Parses the arguments (without the program name). Throws BridgeUsageError.
[[nodiscard]] BridgeOptions parse_bridge_options(const std::vector<std::string_view>& args);

[[nodiscard]] std::string bridge_usage();

} // namespace vehicle_sil::vector_xl
