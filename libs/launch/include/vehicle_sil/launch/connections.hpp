// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch connections: where each network of a configuration file can be connected on
// this computer. For each can and ethernet network the launcher asks the network's bridge for its
// list as JSON (--list-channels --json, --list-interfaces --json; the bridge found as --connect
// finds it, find_tool()) and passes the list on; for a bridge it cannot ask, it makes a list in
// the same form with the error. It connects to nothing: no SIL Kit, no participant's --describe.
// Internal; the command and its outputs: docs/simulation-run.md.
#pragma once

#include "vehicle_sil/launch/check.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// The version of `connections --json` (its "vehicle_sil_connections").
inline constexpr int kConnectionsFormat = 1;
/// The version of the bridges' lists that the launcher reads (their "vehicle_sil_targets").
inline constexpr int kTargetsFormat = 1;

/// How long one bridge may take for its list.
inline constexpr std::chrono::milliseconds kListTimeout{30000};

/// The arguments with which a bridge of a network of `kind` lists its targets as JSON:
/// --list-interfaces --json (ethernet), --list-channels --json (can); none for LIN.
[[nodiscard]] std::vector<std::string> list_arguments(NetworkKind kind);

/// One target of a list ("targets[]").
struct ListedTarget
{
    std::string value;  ///< what --connect <network>=<value> takes
    std::string label;  ///< the name to show
    std::string detail; ///< what else the bridge says about it
    bool usable = true;
    std::string reason{}; ///< why it cannot be used ("" if usable)

    friend bool operator==(const ListedTarget&, const ListedTarget&) = default;
};

/// A list in the form of the bridges' lists ("vehicle_sil_targets": 1): what a bridge wrote, or
/// what the launcher made for a list it could not get (no targets, and the error).
struct TargetList
{
    std::string tool;
    std::optional<std::string> backend{};
    std::optional<std::string> source{};
    std::vector<ListedTarget> targets{};
    std::vector<std::string> notes{};
    /// The list could not be made: its "code" and "message" (`error_code` empty: no error).
    std::string error_code{};
    std::string error_message{};
    /// The list as JSON: the bridge's text as it wrote it, or the launcher's for a list it made.
    /// The outputs pass on every key of it.
    std::string json{};
};

/// Reads a list that a bridge wrote. Throws DescriptionError, saying what is wrong, for anything
/// that is not a list of version kTargetsFormat.
[[nodiscard]] TargetList read_target_list(std::string_view json);

/// A list that the launcher makes for `tool`: no targets, and the error `code` with `message`.
[[nodiscard]] TargetList failed_list(std::string_view tool, std::string_view code, std::string message);

/// The list of `tool` (bridge_tool(kind)) from one run of it with list_arguments(kind): what it
/// wrote (also a list with an error, with which a bridge ends with exit code 3); or else a failed
/// list with the code tool-cannot-start (it could not be started, or Windows did not start it
/// for a DLL), tool-timeout (it did not end within the run's time limit) or tool-bad-output (it
/// wrote no list that the launcher reads; the message has its line that says why).
[[nodiscard]] TargetList interpret_list(NetworkKind kind, std::string_view tool, const DescribeRun& run);

/// One network of the file and where it can be connected.
struct NetworkConnections
{
    std::string name;
    /// Empty if the file's kind of it has an error (the check reports it): nothing is listed.
    std::optional<NetworkKind> kind{};
    std::string tool{};                  ///< bridge_tool(); "" for LIN and an unknown kind
    std::filesystem::path tool_path{};   ///< where find_tool() found it; empty: not found, or no tool
    std::optional<TargetList> list{};    ///< empty for LIN and an unknown kind
};

/// What `vehicle_sil_launch connections` found.
struct ConnectionsReport
{
    std::string file; ///< as the user named it
    /// The file was read as a configuration (its other errors do not matter here).
    bool readable = false;
    /// The command line was wrong (findings say why; code "usage").
    bool usage = false;
    std::vector<NetworkConnections> per_network{}; ///< in the file's order; with --network, that one
    /// Why the file could not be read, or the usage errors.
    std::vector<Finding> findings{};

    /// The network whose kind could not be read, or whose list has an error.
    [[nodiscard]] bool incomplete() const;
    /// 4 usage, 3 the file cannot be read, 1 a list has an error (incomplete()), else 0.
    [[nodiscard]] int exit_code() const;
};

/// The networks of `config` (with `network`, that one only: it must be one of them) and their
/// lists: each bridge is looked for and asked once (`tools`: find, describe for the run of the
/// list), also for two networks of its kind; a bridge that is not found has the failed list
/// tool-missing.
[[nodiscard]] std::vector<NetworkConnections> connections_of(const SimulationConfig& config,
                                                             const std::optional<std::string>& network,
                                                             const ConnectTools& tools);

/// The usage error of --network `network` if `config` has no such network, or empty.
[[nodiscard]] std::optional<Finding> unknown_network(const SimulationConfig& config, std::string_view network);

/// What `connections` gets from the command line.
struct ConnectionsOptions
{
    std::optional<std::string> network{};      ///< --network
    std::filesystem::path tool_directory{};    ///< where vehicle_sil_launch is (find_tool())
    std::chrono::milliseconds list_timeout = kListTimeout;
};

/// Reads the file `path` (shown as `shown_as`) as a configuration, without a variant (every
/// variant has the same networks), and lists where each network can be connected
/// (connections_of(), with the bridges run as processes). Never throws for a problem of the file
/// or of a bridge: they are findings or failed lists.
[[nodiscard]] ConnectionsReport list_connections(const std::filesystem::path& path, const std::string& shown_as,
                                                 const ConnectionsOptions& options);

/// `word` as one argument that sh (bash, zsh), PowerShell and cmd all read as it is: unchanged if
/// it has only letters, digits and _ - . : / = + (ASCII); else in double quotes if it has none of
/// " $ ` % ! and no control character, and no \ before one of " $ ` \ or at its end; else empty
/// (no one form works in all of them).
[[nodiscard]] std::optional<std::string> quoted_for_shells(std::string_view word);

/// `word` quoted for sh: in single quotes, each ' as '\''.
[[nodiscard]] std::string quoted_for_sh(std::string_view word);

/// The report for people: a line for the file, then each network with its targets as
/// --connect lines to copy, the targets it cannot use and why, the bridge's notes, and why a list
/// could not be made. A file that cannot be read: its findings.
[[nodiscard]] std::string format_connections_text(const ConnectionsReport& report);

/// The report as JSON (format kConnectionsFormat): docs/simulation-run.md has the keys. Each
/// network's "list" is its TargetList::json with every key.
[[nodiscard]] std::string format_connections_json(const ConnectionsReport& report);

} // namespace vehicle_sil::launch
