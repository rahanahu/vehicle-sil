// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch connections: where each network of a configuration file can be connected on
// this computer. For each can and ethernet network the launcher asks the network's bridge for its
// list as JSON (--list-channels --json, --list-interfaces --json; the bridge found as --connect
// finds it, find_tool()) and passes the list on; for a bridge it cannot ask, it makes a list in
// the same form with the error. It connects to nothing: no SIL Kit, no participant's --describe.
// Internal; the command and its outputs: docs/simulation-run.md.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/request.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
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

/// The arguments with which the bridge of `backend` for a network of `kind` lists its targets as
/// JSON: --list-interfaces --backend <backend> --json (ethernet), --list-channels --json (can);
/// none for LIN.
[[nodiscard]] std::vector<std::string> list_arguments(NetworkKind kind, std::string_view backend);

/// One target of a list ("targets[]").
struct ListedTarget
{
    std::string value;  ///< what --connect <network>=<value> takes
    std::string label;  ///< the name to show
    std::string detail; ///< what else the bridge says about it
    bool usable = true;
    std::string reason{}; ///< why it cannot be used ("" if usable)
    /// ethernet: the pcap name the bridge reports ("ethernet"."pcap_name"), or "".
    std::string pcap_name{};
    /// The launcher made it usable: false (refuse_targets()): the outputs say so in the JSON too.
    bool refused = false;

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

/// The deepest nesting of objects and arrays that read_target_list() takes.
inline constexpr std::size_t kMaxListDepth = 64;

/// Reads a list that a bridge wrote. Throws DescriptionError, saying what is wrong, for anything
/// that is not a list of version kTargetsFormat, or nested deeper than kMaxListDepth.
[[nodiscard]] TargetList read_target_list(std::string_view json);

/// A list that the launcher makes for `tool`: no targets, and the error `code` with `message`.
[[nodiscard]] TargetList failed_list(std::string_view tool, std::string_view code, std::string message);

/// The list of `tool` (bridge_tool(kind, backend)) from one run of it with list_arguments(): what it
/// wrote (also a list with an error, with which a bridge ends with exit code 3); or else a failed
/// list with the code tool-cannot-start (it could not be started, or Windows did not start it
/// for a DLL), tool-timeout (it did not end within the run's time limit) or tool-bad-output (it
/// wrote no list that the launcher reads; the message has its line that says why).
[[nodiscard]] TargetList interpret_list(NetworkKind kind, std::string_view tool, const config::DescribeRun& run);

/// Why `--connect <network>=<value>` cannot connect `network` (of `kind`) at `target`, or empty:
/// the network's name has '=' or starts with "--" (the command line cannot name it), or
/// target_problem() of its value (on `os`).
[[nodiscard]] std::optional<std::string> refused_target(std::string_view network, NetworkKind kind,
                                                        const ListedTarget& target, config::Os os = config::kThisOs);

/// Makes each usable target of `list` that refused_target() refuses unusable, with that reason
/// (ListedTarget::refused): so the text and the JSON say the same, and a usable target's value is
/// what --connect takes.
void refuse_targets(std::string_view network, NetworkKind kind, TargetList& list, config::Os os = config::kThisOs);

/// Gives each target of the list of `backend` the value that --connect takes: the name the bridge
/// listed, with "<backend>:" before it unless that is what --connect reads without it (the
/// default backend, and a name that does not read as "none" or as another backend's prefix).
void prefix_targets(NetworkKind kind, std::string_view backend, TargetList& list, config::Os os = config::kThisOs);

/// One network of the file and where it can be connected.
struct NetworkConnections
{
    std::string name;
    /// Empty if the file's kind of it has an error (the check reports it): nothing is listed.
    std::optional<NetworkKind> kind{};
    std::string tool{};                  ///< bridge_tool() of the backend; "" if there is none here
    std::filesystem::path tool_path{};   ///< where find_tool() found it; empty: not found, or no tool
    std::optional<TargetList> list{};    ///< empty if there is no bridge here, or the kind is unknown
    /// Why there is no bridge for it on this computer (no_bridge_here()), or "".
    std::string no_bridge{};
    /// The backend the list is of (the "<backend>:" of its targets, where they need it); "" for LIN.
    std::string backend{};
};

/// Whether the target that a request file gives for one network can be used on this computer
/// (--request): the target as written, and for one that cannot, the code and the reason. The
/// codes: target-not-listed (the bridge's list of the backend does not have it, or no list of the
/// backend is made here), target-unusable (it is listed with usable: false), list-failed (the
/// list could not be made), target-bad (it is no target for this kind of network on this OS, or
/// the network has no bridge).
struct RequestedTarget
{
    std::string network;
    std::string target;
    bool usable = false;
    std::string code{};   ///< "" if usable
    std::string reason{}; ///< what is wrong and what to do; "" if usable

    friend bool operator==(const RequestedTarget&, const RequestedTarget&) = default;
};

/// The request file of `connections --request` and what it asks for.
struct ConnectionsRequest
{
    std::string file; ///< as the user named it
    /// One entry for each network that the file connects (not none) and that is listed, in the
    /// file's order. Empty with --network for a network the file does not connect.
    std::vector<RequestedTarget> connect{};
};

/// What `vehicle_sil_launch connections` found.
struct ConnectionsReport
{
    std::string file; ///< as the user named it
    /// The file was read as a configuration.
    bool readable = false;
    /// The command line was wrong (findings say why; code "usage").
    bool usage = false;
    std::vector<NetworkConnections> per_network{}; ///< in the file's order; with --network, that one
    /// With --request: the request file's targets and whether each can be used here.
    std::optional<ConnectionsRequest> request{};
    /// Why the file could not be read, the usage errors, or the errors of a file that was read
    /// (as check reports them; the networks listed are those it could read).
    std::vector<config::Finding> findings{};

    /// A bridge that is there could not make its list (any error but tool-missing), a network's
    /// kind could not be read, or the file has an error.
    [[nodiscard]] bool incomplete() const;
    /// A target of the request file cannot be used here.
    [[nodiscard]] bool request_unusable() const;
    /// 4 usage, 3 the file cannot be read, 1 incomplete() or request_unusable(), else 0.
    [[nodiscard]] int exit_code() const;
};

/// The networks of `config` (with `network`, that one only: it must be one of them) and their
/// lists, one entry for each backend that the OS lists for the kind (listed_backends() of
/// `tools.os`; an entry without a list for LIN): each bridge is looked for and asked once for a
/// backend (`tools`: find, describe for the run of the list), also for two networks of its kind;
/// a bridge that is not found has the failed list tool-missing. Each list goes through
/// prefix_targets() and refuse_targets(). With `only`, just the networks named in it (no bridge is
/// asked for the others).
[[nodiscard]] std::vector<NetworkConnections> connections_of(const config::SimulationConfig& config,
                                                             const std::optional<std::string>& network,
                                                             const config::ConnectTools& tools,
                                                             const std::vector<std::string>* only = nullptr);

/// Whether each of `requests` (a target that is not none, for a network that `listed` has) can be
/// used: its backend and name are compared with the listed targets of that backend, each read by
/// read_connect_target() on `os`, so `packet:enp3s0` and `enp3s0` are one target. A request for a
/// network that `listed` does not have is left out. Pure.
[[nodiscard]] std::vector<RequestedTarget> check_requested(const std::vector<NetworkConnections>& listed,
                                                           std::span<const config::ConnectRequest> requests, config::Os os = config::kThisOs);

/// The usage error of --network `network` if `config` has no such network, or empty.
[[nodiscard]] std::optional<config::Finding> unknown_network(const config::SimulationConfig& config, std::string_view network);

/// What `connections` gets from the command line.
struct ConnectionsOptions
{
    std::optional<std::string> network{};      ///< --network
    /// --request: the file's "connect" says which networks are listed and checked; its other keys
    /// are read and not used. Empty: no request.
    std::optional<RequestFile> request{};
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
/// " $ ` % ! nor the quotation marks that PowerShell takes for " (U+201C, U+201D, U+201E), no
/// control character, and no \ before one of " $ ` \ or at its end; else empty (no one form works
/// in all of them).
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
