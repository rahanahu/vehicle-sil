// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch check: compares the configuration with what each ecu_main executable
// reports about itself (--describe) and writes the result for people and as JSON. The rules
// are pure functions (no process, no file); check_file() runs them on a file and the
// executables. Internal; the rules and the output: docs/simulation-check.md.
#pragma once

#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/connect.hpp"
#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/launch/time_plan.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// The version of the JSON that format_json() writes (its "vehicle_sil_check" key).
inline constexpr int kCheckFormat = 1;

/// What an executable reported with --describe (docs/ecu-executable.md).
struct ReportedEcu
{
    std::string vehicle_sil_version; ///< "vehicle_sil": the vehicle-sil it was built with
    EcuDescription description;
    /// The description had the key "variables" (`description.variables`, maybe empty); false: it
    /// did not say (the check's JSON writes null).
    bool variables_reported = false;

    friend bool operator==(const ReportedEcu&, const ReportedEcu&) = default;
};

/// --describe's output is not a description this launcher can read.
class DescriptionError : public std::runtime_error
{
public:
    /// `message` is one line; `context` (more lines: where in the output, or "") goes below it.
    explicit DescriptionError(const std::string& message, std::string context = {}) :
      std::runtime_error(message), context_(std::move(context))
    {
    }
    [[nodiscard]] const std::string& context() const noexcept { return context_; }

private:
    std::string context_;
};

/// The description is in another format version (what() is "format <n>").
class DescriptionFormatError : public DescriptionError
{
public:
    DescriptionFormatError(std::int64_t format, std::string vehicle_sil_version) :
      DescriptionError("format " + std::to_string(format)), format_(format),
      vehicle_sil_version_(std::move(vehicle_sil_version))
    {
    }
    [[nodiscard]] std::int64_t format() const noexcept { return format_; }
    /// "vehicle_sil" of the description if it has that key as a string, else "".
    [[nodiscard]] const std::string& vehicle_sil_version() const noexcept { return vehicle_sil_version_; }

private:
    std::int64_t format_;
    std::string vehicle_sil_version_;
};

/// A key that has a fixed set of values ("kind", "use", "type", "causality") has one this
/// launcher does not know. A later vehicle-sil may add values without a new format version, so
/// this means an executable of a newer vehicle-sil, not broken output. what() is as for other
/// errors ("networks[0].use is \"ethernet\", not raw, j1939, lin_master or lin_slave").
class DescriptionValueError : public DescriptionError
{
public:
    DescriptionValueError(const std::string& message, std::string key, std::string value, std::vector<std::string> known,
                          std::string vehicle_sil_version) :
      DescriptionError(message), key_(std::move(key)), value_(std::move(value)), known_(std::move(known)),
      vehicle_sil_version_(std::move(vehicle_sil_version))
    {
    }
    /// Where the value is: "networks[0].use", "variables[2].type".
    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    /// The values this launcher knows for the key.
    [[nodiscard]] const std::vector<std::string>& known() const noexcept { return known_; }
    /// "vehicle_sil" of the description, or "" if it has none.
    [[nodiscard]] const std::string& vehicle_sil_version() const noexcept { return vehicle_sil_version_; }

private:
    std::string key_;
    std::string value_;
    std::vector<std::string> known_;
    std::string vehicle_sil_version_;
};

/// Reads --describe's JSON (format kEcuDescriptionFormat; unknown keys are ignored). Throws
/// DescriptionError, saying what is wrong, for anything else: DescriptionFormatError for another
/// format version, DescriptionValueError for a value it does not know of a key with a fixed set
/// of values.
[[nodiscard]] ReportedEcu read_description(std::string_view json);

/// One run of `<command> --describe --name <name>` (process::run_captured()).
struct DescribeRun
{
    std::string command_text; ///< the command as shown in messages
    /// Set if the process could not be started: the reason.
    std::optional<std::string> start_error{};
    std::optional<int> exit_code{}; ///< empty: it did not end in time and was killed
    std::optional<int> signal{};    ///< POSIX: the signal that ended it (process::Captured::signal)
    bool output_truncated = false;  ///< standard output was longer than the limit (its beginning kept)
    bool error_truncated = false;   ///< standard error was longer than the limit (its end kept)
    /// The system refused to start it (not the executable's fault): process::StartError::Cause.
    bool start_refused_by_system = false;
    std::string standard_output{};
    std::string standard_error{};
    std::chrono::milliseconds timeout{};
};

/// What a participant's --describe gave: its description, or the error that says why not.
struct DescribeOutcome
{
    std::optional<ReportedEcu> ecu;
    std::optional<Finding> error;
};

/// Interprets one run of --describe (exit code, time limit, JSON, format version).
[[nodiscard]] DescribeOutcome interpret_describe(const ParticipantConfig& participant, const DescribeRun& run);

enum class ParticipantStatus
{
    Described,  ///< its executable described itself (ParticipantResult::ecu)
    NotChecked, ///< describe: false
    Failed,     ///< its executable is missing, or --describe failed
    Skipped,    ///< its entry in the file has an error (and it is enabled)
    Disabled,   ///< enabled: false (or --disable), also with an error in its entry: not checked, not started
};

struct ParticipantResult
{
    ParticipantStatus status = ParticipantStatus::Skipped;
    std::optional<ReportedEcu> ecu{};
    /// Its executable (find_executable()) if it is one (Described, NotChecked, and Failed by
    /// --describe); empty otherwise.
    std::filesystem::path executable{};
};

/// What one entry of a participant's j1939_nodes that changes something does on its network
/// (j1939::apply_node_overrides(), on the nodes as the executable described them).
struct J1939NodeMatch
{
    std::size_t entry = 0; ///< the index of the entry in ParticipantConfig::j1939_nodes
    std::size_t line = 0;  ///< its line
    j1939::NodeOverrideResult result{};
};

/// A network of a participant as the simulation sees it: what its executable described, under
/// the name its entry gives it, with the values its entry's j1939_nodes give its J1939 nodes.
struct SeenNetwork
{
    /// As described, but `name` is the simulation's name (a key of the entry's network_names if
    /// one renames it, else the executable's name), and `nodes` have the values after j1939_nodes.
    NetworkDescription network;
    /// The name in the executable's code (--describe's).
    std::string executable_name;
    /// The line of the network_names entry that renames it; 0: none does.
    std::size_t line = 0;
    /// The J1939 nodes as the executable described them (the values in its code), in the order of
    /// `network.nodes`.
    std::vector<J1939NodeDescription> coded_nodes{};
    /// For each of `network.nodes`: the line of the j1939_nodes entry that gave it its values; 0: none did.
    std::vector<std::size_t> node_lines{};
    /// One for each entry of j1939_nodes for this network that changes something, in the entries'
    /// order (an entry that takes no node keeps the values as they are; the rules say why).
    std::vector<J1939NodeMatch> j1939_matches{};
    [[nodiscard]] bool renamed() const { return executable_name != network.name; }
};

/// The participant's networks as the simulation sees them: those its executable described, in
/// that order, each under its name in the simulation (renamed_networks() of its entry applied; a
/// line that names no network of the executable changes nothing), and on a J1939 network, the
/// entries of its j1939_nodes that change something applied to the nodes
/// (j1939::apply_node_overrides(): each matched against the addresses in the executable's code).
/// The one place that applies network_names and j1939_nodes to a description: the check's rules,
/// its table and the "networks" of its JSON read the networks of a description only through it.
/// (run gives the same lines to the executable: map_network_arguments(), j1939_node_arguments().)
[[nodiscard]] std::vector<SeenNetwork> seen_networks(const ParticipantConfig& participant, const ReportedEcu& ecu);

/// The indexes (in ParticipantConfig::j1939_nodes) of the entries that change something and name a
/// network on which the executable has no J1939 network (`seen`: seen_networks()), in their order.
[[nodiscard]] std::vector<std::size_t> unmatched_j1939_entries(const ParticipantConfig& participant,
                                                               const std::vector<SeenNetwork>& seen);

/// The error "j1939-node-unsupported" of the participant, if its j1939_nodes need a word of the
/// features that its executable's --describe does not have (`table`: required_j1939_node_features()).
[[nodiscard]] std::optional<Finding>
j1939_nodes_unsupported(const ParticipantConfig& participant, const ReportedEcu& ecu, std::string_view launcher_version,
                        std::span<const J1939NodeSettingFeature> table = kJ1939NodeSettingFeatures);

/// Compares the configuration with what the participants reported (`results` in the order of
/// config.participants): names, steps, networks and their kinds, J1939 addresses and NAMEs (on a
/// CAN network), LIN masters (on a LIN network), MAC addresses (on an Ethernet network), unused
/// networks and networks with one
/// participant, participants that were not checked or are disabled, vehicle-sil versions other
/// than `launcher_version`. A network is under its name in the simulation (seen_networks()) in
/// every rule, and the entry's network_names are compared with the description (an executable
/// that does not take --map-network, a name the executable does not have, two of its networks
/// under one name). The J1939 nodes have the values of the entry's j1939_nodes in every rule
/// (seen_networks()); an entry that takes no node is an error, and keeps that participant's nodes
/// on that network out of the rules of the addresses and NAMEs. The rules of a network are those of its kind only: a description
/// with J1939 nodes on a use other than j1939 is not checked for them (read_description()
/// refuses it). A disabled
/// participant (ParticipantConfig::enabled, whatever its result) is not on any network and holds
/// back no rule: the rules are about the simulation without it. `outside`: the networks that
/// --connect connects to a real bus (NetworkOutside::network); on each, the bridge counts as a
/// participant once an ECU is on it, a test participant with J1939 nodes is warned about as on a
/// network with a bridge, and an enabled role: bridge participant is a loop (connect-loop, or
/// connect-maybe-loop with describe: false). Findings in a fixed order: participants in the file's
/// order, then networks in the file's order.
[[nodiscard]] std::vector<Finding> check_participants(const SimulationConfig& config,
                                                      const std::vector<ParticipantResult>& results,
                                                      std::string_view launcher_version,
                                                      std::span<const std::string> outside = {});

/// A network that --connect connects to a real bus, as the check made it out.
struct NetworkOutside
{
    std::string network;
    std::string tool;                ///< bridge_tool()
    std::filesystem::path tool_path; ///< find_tool(); empty: not found (or not looked for)
    std::string target;              ///< as given
    std::string participant;         ///< bridge_participant()
    /// bridge_arguments(): what a run gives the bridge. Empty if an error of the check is about
    /// connecting this network (a connect-* error), or if the frames that pass are not known (a
    /// participant has no description).
    std::optional<std::vector<std::string>> arguments{};

    friend bool operator==(const NetworkOutside&, const NetworkOutside&) = default;
};

/// How often a bridge that run starts writes its statistics line while it runs, if the counts have
/// changed (its --stats-every-ms): run reads the counts from it (detail::read_outside_counts()).
inline constexpr std::int64_t kBridgeStatisticsEveryMs = 1000;

/// The arguments run starts the bridge of `outside` with, after its executable: its arguments
/// (as the check showed them in its command, untouched: an adapter's name may have "{name}" in
/// it), then --name, --registry-uri `registry_uri`, --start-timeout-ms `start_timeout_ms` and
/// --stats-every-ms kBridgeStatisticsEveryMs.
[[nodiscard]] std::vector<std::string> bridge_run_arguments(const NetworkOutside& outside, std::string_view registry_uri,
                                                            std::int64_t start_timeout_ms);

/// What check_connections() uses to look for a bridge and to ask it --describe, and
/// connections_of() to ask it for its list.
struct ConnectTools
{
    /// Where the tool of that name is (find_tool()), or empty.
    std::function<std::optional<std::filesystem::path>(std::string_view)> find;
    /// The first place `find` looks (CheckOptions::tool_directory), for the message when it finds nothing.
    std::filesystem::path launcher_dir{};
    /// One run of the tool with the arguments (bridge_arguments() and --describe --name <bridge>;
    /// list_arguments()), in bridge_working_directory().
    std::function<DescribeRun(const std::filesystem::path&, const std::vector<std::string>&)> describe;
    /// Whether the bridge of a can network runs on this OS (no_bridge_here()). Where it does not, a
    /// bridge that `find` finds anyway (a build for tests) is still used.
    bool can_bridge = kCanBridgeOnThisOs;
};

/// The findings of the --connect requests and the networks they connect (in the order of the
/// file's networks; a request with kNoTarget connects nothing). In this order for each request:
/// connect-network (no such network), connect-kind (LIN), connect-target (its form),
/// connect-name-taken (a participant of the file has the bridge's name), connect-filter (an
/// Ethernet network without external.ethertypes or all_frames whose ECUs' MAC addresses are not
/// all known; the default filter passes the frames of the ECUs only, not of participants with
/// role: test), connect-tool-missing, connect-tool-failed (its --describe failed, or it described
/// another name or network); the bridge's --describe runs only if nothing else is wrong with the
/// request and every enabled participant was described. (What a connection outside means for the
/// time is not judged here: time_lock_findings() in time_plan.hpp.) Without `tools` (only the file is read)
/// neither the tools nor the participants' descriptions are looked at: only the requests' form and
/// names. NetworkOutside::arguments is empty for a network with an error here; check_file() also
/// empties it for one with an error of check_participants() (connect-loop).
struct ConnectCheck
{
    std::vector<Finding> findings;
    std::vector<NetworkOutside> outside;
};
[[nodiscard]] ConnectCheck check_connections(const SimulationConfig& config, const std::vector<ParticipantResult>& results,
                                             const std::vector<ConnectRequest>& requests, const ConnectTools* tools);

/// The scenario that was checked with the configuration (--scenario).
struct ScenarioCheck
{
    std::string file; ///< as the user named it
    /// Its "name" and the number of its steps, as far as it could be read.
    std::string name{};
    std::size_t steps = 0;
    /// Set if the configuration could be read and the scenario has no error: it can run.
    std::optional<Scenario> scenario{};
    /// ScenarioRead::params, only, skipped and unchecked_expects.
    std::vector<ParamValue> params{};
    std::optional<std::vector<std::string>> only{};
    std::size_t skipped = 0;
    std::size_t unchecked_expects = 0;
    /// ScenarioRead::not_for_variant: its "only" does not list the chosen variant (one of the
    /// errors says so). vehicle_sil_launch test skips such a scenario instead of failing.
    bool not_for_variant = false;
};

/// The result of checking one file.
struct CheckReport
{
    std::string file; ///< the file as the user named it (in messages)
    /// Empty if the file could not be read as a configuration.
    std::optional<SimulationConfig> config{};
    /// The names of the variants the file defines, also when `usage` (no --variant, or an unknown
    /// one: the outputs then list what can be chosen); `config->variant` is the chosen one.
    std::vector<std::string> variants{};
    std::vector<ParticipantResult> participants{}; ///< in the order of config->participants
    std::vector<Finding> findings{};               ///< in the order of sort_findings()
    /// --scenario: the scenario file; its findings are in `findings`, with Finding::file set.
    std::optional<ScenarioCheck> scenario{};
    /// --connect: the networks connected to a real bus (check_connections()); empty without it.
    std::vector<NetworkOutside> outside{};
    /// Whether a run keeps real time because it is connected outside the simulation (time_lock()
    /// of `config` and CheckOptions::connect, time_policy); nothing without `config`.
    TimeLock time_lock{};
    /// --strict: a warning fails the check too (a note never does).
    bool strict = false;
    /// The command line was wrong (format_json(): result "usage"; the findings say why; `file` stays
    /// as given). check_file() sets it for overrides that do not fit (override_conflicts(),
    /// ConfigRead::override_errors, and the scenario's ScenarioRead::usage_errors); `config` is
    /// then empty.
    bool usage = false;

    [[nodiscard]] std::size_t errors() const;
    [[nodiscard]] std::size_t warnings() const;
    [[nodiscard]] std::size_t notes() const;
    /// true if the file was read and has no error (with `strict`: and no warning).
    [[nodiscard]] bool passed() const;
};

/// Puts the findings in the report's order: the errors, the warnings, the notes, each by file (the
/// configuration's first) and line (those without a line first), otherwise as found.
/// The participant's variables as the simulation sees them: what its executable described,
/// through the variable map of its entry (ParticipantConfig::variables). The run's hub makes the
/// same of what the executable's agent announces. problems(): the mappings that do not fit.
[[nodiscard]] control::MappedSchema seen_variables(const ParticipantConfig& participant, const ReportedEcu& ecu);

void sort_findings(std::vector<Finding>& findings);

/// The catalog of a checked configuration, for read_scenario() (CheckReport::config must be set):
/// each participant as a scenario can use it, from its entry and what its executable described.
[[nodiscard]] ScenarioCatalog scenario_catalog(const CheckReport& check);

/// The report for people: the participants and the networks as tables, then each error and
/// warning as "<severity>: <file>:<line>: <message> [<code>]" (with the detail indented below), then a
/// summary line. LF line ends; the same report gives the same text.
[[nodiscard]] std::string format_text(const CheckReport& report);

/// The report as JSON (format kCheckFormat): fixed keys in a fixed order, the layout of
/// to_json(). docs/simulation-check.md has the keys.
[[nodiscard]] std::string format_json(const CheckReport& report);

struct CheckOptions
{
    /// How long one --describe may take.
    std::chrono::milliseconds describe_timeout{10000};
    /// CheckReport::strict.
    bool strict = false;
    /// --variant, --disable, --enable and --manual.
    EnabledOverrides overrides{};
    /// --param <name>=<value>, each once (ParamSource::CommandLine): over the scenario's and the
    /// configuration's parameters. Only with `scenario`.
    std::vector<ParamValue> params{};
    /// --scenario: a scenario file to check against the configuration (`scenario_shown`: as the
    /// user named it); empty: none.
    std::filesystem::path scenario{};
    std::string scenario_shown{};
    /// `params` are for many scenarios (ScenarioContext::params_for_many): one that `scenario`
    /// does not have is no error.
    bool params_for_many = false;
    /// Only read the file: no executable is looked for or run, and no scenario is read. The
    /// report has the file's findings, its variants and the overrides' usage errors.
    bool file_only = false;
    /// --connect <network>=<target>, each network once (CheckReport::outside).
    std::vector<ConnectRequest> connect{};
    /// Where the launcher is: the first place to look for a bridge (find_tool()); empty: PATH only.
    std::filesystem::path tool_directory{};
    /// --time-policy: what a run that is connected outside the simulation does about time
    /// (CheckReport::time_lock).
    TimePolicy time_policy = TimePolicy::Realtime;
    /// run: its --speed (RunOptions::speed), for time_lock_findings(); the outer optional empty
    /// without it (check: always), and the file's speed counts.
    std::optional<std::optional<double>> run_speed{};
    /// test: without the time lock every run goes as fast as possible, whatever the file's speed
    /// (which then does not count for time_lock_findings()).
    bool fastest_by_default = false;
    /// run: --start-paused, for time_lock_findings().
    bool start_paused = false;
};

/// Checks the file `path` (shown as `shown_as`): reads it, checks the executables, runs each
/// --describe in turn (not in parallel), compares. A disabled participant's executable is neither
/// looked for nor run. With CheckOptions::scenario, the scenario file is read and checked against
/// the participants' descriptions as well (if the configuration could be read). Never throws for a
/// problem of a file or of an executable: they are findings.
[[nodiscard]] CheckReport check_file(const std::filesystem::path& path, const std::string& shown_as,
                                     const CheckOptions& options);

} // namespace vehicle_sil::launch
