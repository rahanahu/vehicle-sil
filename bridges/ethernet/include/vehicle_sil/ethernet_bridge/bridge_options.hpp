// SPDX-License-Identifier: MIT
//
// Command line of the Ethernet bridge. Platform independent so it can be tested anywhere.
#pragma once

#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <chrono>
#include <cstdint>
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
inline constexpr std::size_t kMaxMacTableSize = eth_link::MacSideTable::kMaxCapacity;
/// The longest --start-timeout-ms: the longest that vehicle_sil_launch gives (kMaxMillis of
/// vehicle_sil/ecu/executable.hpp).
inline constexpr std::uint64_t kMaxStartTimeoutMs = 1'000'000'000'000;
/// How long the bridge waits for its participant to run in SIL Kit without --start-timeout-ms.
inline constexpr std::chrono::milliseconds kDefaultStartTimeout{15000};
/// The range of --stats-every-ms.
inline constexpr std::uint64_t kMinStatisticsIntervalMs = 100;
inline constexpr std::uint64_t kMaxStatisticsIntervalMs = 3'600'000;

struct BridgeOptions
{
    bool help = false;
    bool list_interfaces = false;
    /// --json (with --list-interfaces only): write the list as JSON (interface_list_json()).
    bool json = false;
    /// --describe: write the bridge's description (describe_json()) and exit; nothing is opened,
    /// and --registry-uri is not needed.
    bool describe = false;

    std::string registry_uri;
    /// --name, or --participant (the same option).
    std::string participant = "EthernetBridge";
    /// --start-timeout-ms: how long ParticipantRuntime::start() waits for Running.
    std::chrono::milliseconds start_timeout = kDefaultStartTimeout;
    /// --stats-every-ms: print the statistics line while running, at this interval where it
    /// changed. Unset: only once, when the bridge stops.
    std::optional<std::chrono::milliseconds> statistics_interval;
    /// The SIL Kit Ethernet network. No default: the bridge never joins a network by mistake.
    std::string network;

    /// --backend, else the default the executable gives parse_bridge_options(); nullopt only
    /// where neither is (no backend works on this operating system).
    std::optional<Backend> backend;
    std::string interface_name;
    /// --all-frames: no EtherType or MAC restriction, also on a real adapter.
    bool all_frames = false;
    /// --promiscuous: On, --no-promiscuous: Off, neither: Auto (promiscuous mode where the
    /// adapter accepts it). Only for the backends that reach real adapters.
    eth_link::Promiscuous promiscuous = eth_link::Promiscuous::Auto;
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
/// `default_backend`: the backend without --backend. The executable decides it (packet on Linux,
/// npcap on Windows, none elsewhere), so the parsing knows neither the OS nor the build.
[[nodiscard]] BridgeOptions parse_bridge_options(const std::vector<std::string_view>& args,
                                                 std::optional<Backend> default_backend);

[[nodiscard]] std::string bridge_usage();

} // namespace vehicle_sil::ethernet_bridge
