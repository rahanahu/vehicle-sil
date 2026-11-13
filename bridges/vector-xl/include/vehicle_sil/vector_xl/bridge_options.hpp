// SPDX-License-Identifier: MIT
//
// Command line of vehicle_sil_vector_xl_bridge. Platform independent so it can be tested anywhere.
#pragma once

#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::vector_xl {

struct BridgeOptions
{
    bool help = false;
    bool list_channels = false;

    std::string registry_uri;
    std::string participant = "VectorXlBridge";
    std::string network = "J1939_CAN1";

    XlTarget target = XlTarget::Virtual;
    unsigned channel = 0; ///< 1-based, see selectable_channels()
    /// Bit rate to configure. Without it the channel keeps its current configuration.
    std::optional<std::uint32_t> bitrate;
    /// XL receive queue size in events; the XL driver requires a power of two.
    std::uint32_t rx_queue_size = 16384;
    /// Ask the driver for the controller's error state (XlCanPortOptions::bus_state_interval).
    bool bus_monitor = true;
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
