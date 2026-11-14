// SPDX-License-Identifier: MIT
//
// Command line of the Ethernet bridge. Platform independent so it can be tested anywhere.
#pragma once

#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::ethernet_bridge {

/// How the bridge reaches the external interface.
enum class Backend
{
    Tap,    ///< a Linux TAP device (stays in this computer)
    Packet, ///< a Linux network interface through a packet socket
    Npcap,  ///< a Windows network adapter through Npcap
    Pcap,   ///< a Linux network interface through libpcap
};

/// "tap", "packet", "npcap", "pcap".
[[nodiscard]] std::string_view to_string(Backend backend) noexcept;
[[nodiscard]] std::optional<Backend> parse_backend(std::string_view text) noexcept;
/// Whether frames sent through `backend` may reach a real network adapter (all but tap).
[[nodiscard]] bool reaches_real_adapters(Backend backend) noexcept;

/// The smallest --max-frame-bytes: SIL Kit delivers no shorter frame (it pads to 60 bytes).
inline constexpr std::size_t kMinMaxFrameBytes = 60;
inline constexpr std::size_t kMaxMaxFrameBytes = 65535;
inline constexpr std::size_t kMinMacTableSize = 16;
inline constexpr std::size_t kMaxMacTableSize = 65536;

struct BridgeOptions
{
    bool help = false;
    bool list_interfaces = false;

    std::string registry_uri;
    std::string participant = "EthernetBridge";
    /// The SIL Kit Ethernet network. No default: the bridge never joins a network by mistake.
    std::string network;

    std::optional<Backend> backend;
    std::string interface_name;
    /// --all-frames: no EtherType or MAC restriction, also on a real adapter.
    bool all_frames = false;
    bool promiscuous = false;
    /// Create the TAP device for the bridge's lifetime (tap only).
    bool tap_create = false;

    /// The filter (--ethertype, --src-mac, --vlan-tpid, --max-frame-bytes) and the MAC rule
    /// (--no-mac-guard, --mac-table-size).
    EthernetBridgeOptions bridge;
};

/// what() says what is wrong and how to fix it.
class BridgeUsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// Parses the arguments (without the program name). Throws BridgeUsageError.
[[nodiscard]] BridgeOptions parse_bridge_options(const std::vector<std::string_view>& args);

[[nodiscard]] std::string bridge_usage();

} // namespace vehicle_sil::ethernet_bridge
