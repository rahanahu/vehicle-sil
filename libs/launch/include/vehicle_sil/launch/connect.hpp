// SPDX-License-Identifier: MIT
//
// Connecting a network of the simulation outside, to a real bus (--connect <network>=<target>):
// the request on the command line, the form of a target, the bridge tool of each kind of network,
// the arguments the launcher gives it and where the launcher looks for it. Pure functions but
// find_tool(). Internal; the command line: docs/simulation-config.md, the findings:
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

/// The participant name of the bridge of `network`: "bridge.<network>".
[[nodiscard]] std::string bridge_participant(std::string_view network);

/// What is wrong with `target` for a network of `kind`, or empty: it is empty, longer than
/// kMaxTargetBytes, not UTF-8, has a control character or starts with '-'; for CAN it is not
/// virtual:<n> or physical:<n> with n from 1. kNoTarget is never wrong.
[[nodiscard]] std::optional<std::string> target_problem(NetworkKind kind, std::string_view target);

/// The arguments of the bridge of `network` at `target` (target_problem() empty), without --name
/// and what a run adds. ethernet: --network, --interface, and the frames that pass: --all-frames
/// or --ethertype (external) or else --src-mac with `macs` (in their order, each once; none if
/// `macs` is empty). can: --network, --xl-target, --xl-channel and --bitrate (external).
[[nodiscard]] std::vector<std::string> bridge_arguments(const NetworkConfig& network, std::string_view target,
                                                        const std::vector<eth::MacAddress>& macs);

/// Where the launcher finds the tool `name` (bridge_tool()): in `launcher_dir` (the directory of
/// vehicle_sil_launch; skipped if empty), then in the absolute directories of PATH
/// (detail::find_on_path()); on Windows also with ".exe". Never in the working directory or the
/// directory of the configuration file: a folder received from someone does not get to run a
/// program of its own. Empty if it is in none of them.
[[nodiscard]] std::optional<std::filesystem::path> find_tool(std::string_view name,
                                                             const std::filesystem::path& launcher_dir);

} // namespace vehicle_sil::launch
