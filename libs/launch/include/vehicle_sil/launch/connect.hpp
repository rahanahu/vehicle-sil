// SPDX-License-Identifier: MIT
//
// Connecting a network of the simulation outside, to a real bus (--connect <network>=<target>):
// the request on the command line, the form of a target, the bridge tool of each kind of network,
// the arguments the launcher gives it and the words about where the launcher looks for it. Pure
// functions (the search itself is find_tool() in probe.hpp). Internal; the command line: docs/simulation-config.md, the findings:
// docs/simulation-check.md.
#pragma once

#include "vehicle_sil/launch/config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// One --connect <network>=<target>.
struct ConnectRequest
{
    std::string network;
    std::string target;

    friend bool operator==(const ConnectRequest&, const ConnectRequest&) = default;
};

/// Reads "<network>=<target>", split at the first '=' (a network name with '=' cannot be given;
/// the target may have one). Empty if there is no '=' or nothing before it; the target may be
/// empty (target_problem() says so).
[[nodiscard]] std::optional<ConnectRequest> parse_connect(std::string_view text);

/// The target that connects nothing: the network runs on SIL Kit only, as without --connect.
inline constexpr std::string_view kNoTarget = "none";

/// The longest target, in bytes.
inline constexpr std::size_t kMaxTargetBytes = 256;

/// The executable that connects a network of `kind` outside: vehicle_sil_ethernet_bridge,
/// vehicle_sil_vector_xl_bridge, or "" for LIN (there is no bridge for it).
[[nodiscard]] std::string_view bridge_tool(NetworkKind kind);

/// true where the bridge of a can network (vehicle_sil_vector_xl_bridge, on the Vector XL Driver
/// Library) runs: Windows only.
#ifdef _WIN32
inline constexpr bool kCanBridgeOnThisOs = true;
#else
inline constexpr bool kCanBridgeOnThisOs = false;
#endif

/// Why a network of `kind` has no bridge on this computer (`can_bridge`: kCanBridgeOnThisOs), or
/// empty: "there is no bridge for LIN", "the Vector XL bridge is for Windows".
[[nodiscard]] std::optional<std::string> no_bridge_here(NetworkKind kind, bool can_bridge = kCanBridgeOnThisOs);

/// The working directory of a bridge that the launcher starts (its --describe, its list, its run):
/// the directory of its executable `tool`. Never the directory of the configuration file or the
/// launcher's working directory: a DLL put into a folder received from someone is not loaded.
[[nodiscard]] std::filesystem::path bridge_working_directory(const std::filesystem::path& tool);

/// The participant name of the bridge of `network`: "bridge.<network>".
[[nodiscard]] std::string bridge_participant(std::string_view network);

/// What is wrong with `target` for a network of `kind`, or empty: it is empty, longer than
/// kMaxTargetBytes, not UTF-8, has a control character (C0, DEL, C1, or one of Unicode's that
/// change the direction of text: U+202A to U+202E, U+2066 to U+2069) or starts with '-'; for CAN
/// it is not virtual:<n> or physical:<n> with n from 1. kNoTarget is never wrong.
[[nodiscard]] std::optional<std::string> target_problem(NetworkKind kind, std::string_view target);

/// The arguments of the bridge of `network` at `target` (target_problem() empty), without --name
/// and what a run adds. ethernet: --network, --interface, and the frames that pass: --all-frames
/// or --ethertype (external) or else --src-mac with `macs` (in their order, each once; none if
/// `macs` is empty). can: --network, --xl-target, --xl-channel and --bitrate (external).
[[nodiscard]] std::vector<std::string> bridge_arguments(const NetworkConfig& network, std::string_view target,
                                                        const std::vector<eth::MacAddress>& macs);

/// true where adapter names are compared without regard to case (Windows).
#ifdef _WIN32
inline constexpr bool kAdapterNamesIgnoreCase = true;
#else
inline constexpr bool kAdapterNamesIgnoreCase = false;
#endif

/// The device that `target` names for a network of `kind`, as a string to compare, or empty if it
/// names none (kNoTarget, or a target that target_problem() refuses). Two networks whose targets
/// have the same key use the same device. Ethernet: the interface name (lower case if
/// `ignore_case`). CAN: "physical:<n>" per channel number; every "virtual:<n>" is "virtual" (the
/// Vector Virtual CAN channels are one bus). Strings only: a connection name and the pcap name
/// of the same adapter are not told apart.
[[nodiscard]] std::string connect_device_key(NetworkKind kind, std::string_view target,
                                             bool ignore_case = kAdapterNamesIgnoreCase);

/// true if the networks `a` and `b` are in one connected group when each entry of
/// `participant_networks` (the networks one participant is on) joins its networks: they are on
/// one participant, or on a chain of participants (a-G1-c-G2-b).
[[nodiscard]] bool networks_are_joined(const std::vector<std::vector<std::string>>& participant_networks,
                                       const std::string& a, const std::string& b);

namespace detail {
/// The length in bytes of the Unicode control character at the start of UTF-8 `text`: a C1
/// control (U+0080 to U+009F: 2) or a control of the direction of text (U+202A to U+202E, U+2066
/// to U+2069: 3); 0 for anything else. A terminal does not show them as they are.
[[nodiscard]] std::size_t unicode_control_at(std::string_view text) noexcept;
/// true if `text` has a character of unicode_control_at().
[[nodiscard]] bool has_unicode_control(std::string_view text) noexcept;
} // namespace detail

/// Where find_tool() looked and found nothing, for a message: "neither next to
/// vehicle_sil_launch (<launcher_dir>) nor on PATH", or "not on PATH" if `launcher_dir` is empty.
[[nodiscard]] std::string tool_not_found(const std::filesystem::path& launcher_dir);

/// What to do about a bridge of a network of `kind` that find_tool() does not find: build it
/// (where the docs say how) and put it where the launcher looks.
[[nodiscard]] std::string tool_missing_fix(NetworkKind kind);

} // namespace vehicle_sil::launch
