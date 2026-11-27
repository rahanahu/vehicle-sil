// SPDX-License-Identifier: MIT
//
// The simulation configuration file of vehicle_sil_launch (YAML or JSON): which participants
// to start, the networks they share and when to stop. Reading it checks only the text (keys,
// types, values, names, the commands' arguments); check.hpp compares it with the executables.
// Internal; the file format is in docs/simulation-config.md.
#pragma once

#include "vehicle_sil/control/variable_map.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/ecu/j1939_node_argument.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::config {

/// The version of the configuration format (its "vehicle_sil_simulation" key).
inline constexpr int kSimulationFormat = 2;

/// The longest duration in milliseconds the configuration takes (about 31 years). It is also the
/// largest --start-timeout-ms (and --run-for-ms) that ecu_main accepts (docs/ecu-executable.md):
/// both take it from vehicle_sil/ecu/executable.hpp.
inline constexpr std::int64_t kMaxMillis = ecu_executable::kMaxMillis;

/// The participant name of the controller that vehicle_sil_launch runs; no participant may use it.
inline constexpr std::string_view kControllerName = "SimulationController";

/// The arguments that vehicle_sil_launch gives an ecu_main executable itself; its command must
/// not have them (also not as "--name=..."). docs/ecu-executable.md has their meaning.
/// --map-network and --j1939-node are the ones the file asks for (network_names, j1939_nodes);
/// with describe: false, where a command may have the others, they must not be in the command
/// beside the key that gives them. The list is
/// ecu_executable::kLauncherArguments (vehicle_sil/ecu/executable.hpp), which ecu_main's tests
/// hold against its arguments.
inline constexpr const auto& kLauncherArguments = ecu_executable::kLauncherArguments;

/// The placeholders of a command with describe: false, in the arguments after the executable (also
/// inside a longer argument). Braces are special only as "{word}" (word: letters, digits, '_'):
/// "{{word}}" is the literal "{word}"; any other brace is text.
inline constexpr std::string_view kPlaceholders[] = {"{registry_uri}", "{name}"};

/// How much a finding weighs: an error fails the check, a warning fails it with --strict, a note
/// never does (it says what the check could not look at).
enum class Severity
{
    Error,
    Warning,
    Note,
};

/// One problem that vehicle_sil_launch found. The message says what, with which value, and how
/// to fix it; it does not repeat the file name or the line (the output adds them).
struct Finding
{
    Severity severity = Severity::Error;
    /// A short fixed name of the rule, e.g. "unknown-key", "step-mismatch" (the list:
    /// docs/simulation-config.md).
    std::string code;
    std::string message;
    /// The line in the configuration file (from 1); 0 for none.
    std::size_t line = 0;
    std::string participant; ///< the participant it is about, or ""
    std::string network;     ///< the network it is about, or ""
    /// More text below the message (e.g. the end of an executable's standard error), or "".
    std::string detail{};
    /// The file it is about if that is not the configuration file (a scenario file, as the user
    /// named it), or "": the outputs show it instead, and `line` is a line of it.
    std::string file{};

    friend bool operator==(const Finding&, const Finding&) = default;
};

/// A network's "external" key: how the network behaves when it is connected outside (to a real
/// bus). It holds what is the same on every PC; where the network is connected is not in the file.
/// Only the keys of the network's kind are read (can: bitrate; ethernet: ethertypes, all_frames).
struct NetworkExternal
{
    std::size_t line = 0;
    /// can: the bit rate to set on the channel; empty: the channel's setting is kept.
    std::optional<std::uint32_t> bitrate{};
    /// ethernet: the EtherTypes passed between the network and the outside, as written (none twice).
    std::vector<std::uint16_t> ethertypes{};
    /// ethernet: true passes every frame (never together with ethertypes).
    bool all_frames = false;

    friend bool operator==(const NetworkExternal&, const NetworkExternal&) = default;
};

struct NetworkConfig
{
    std::string name;
    NetworkKind kind = NetworkKind::Can;
    std::size_t line = 0;
    /// false if its kind is missing or wrong (reported when reading): it is not checked.
    bool valid = true;
    /// Its "external" key; empty if it has none, or if the key is wrong (reported when reading).
    std::optional<NetworkExternal> external{};
};

/// What a participant is for (its "role" key). The check reads it; running does not: a participant
/// of any role is started and joins the same way, and it is not in --describe's JSON nor on the wire.
enum class ParticipantRole
{
    Ecu,    ///< an ECU of the vehicle (the default)
    Test,   ///< a participant for testing (a spy, a tool that talks to the ECUs); not counted as an ECU
    Bridge, ///< carries one or more networks to real buses (and from them)
};

/// The roles in the order the messages list them, and their names in the file and in check --json.
inline constexpr ParticipantRole kParticipantRoles[] = {ParticipantRole::Ecu, ParticipantRole::Test,
                                                       ParticipantRole::Bridge};
[[nodiscard]] const char* to_string(ParticipantRole role);
/// The role to_string() names `name` (exactly, lower case), or empty for another text.
[[nodiscard]] std::optional<ParticipantRole> participant_role_named(std::string_view name) noexcept;

/// One line of a participant's "network_names": the simulation's network `name` (a name of
/// "networks") is the network that the executable's code names `from`. Everywhere in this
/// direction: the file writes `name: from`, run gives the executable `--map-network name=from`.
struct NetworkName
{
    std::string name;
    std::string from;
    std::size_t line = 0;
    /// false for a line whose two names are the same: it stands for no line (a variant writes it
    /// over a line of the common part to take that away). Not given to the executable, not checked.
    [[nodiscard]] bool renames() const { return name != from; }

    friend bool operator==(const NetworkName&, const NetworkName&) = default;
};

/// One line of a participant's "j1939_nodes": other values for one J1939 node of its executable
/// (ecu_executable::J1939NodeOverride: `network` is the simulation's name, `node.from` the address
/// in the executable's code), and the line it is on. An entry that changes nothing
/// (node.changes() is false: network and from only) is not given to the executable and not
/// matched; a variant writes it over a common entry to take that away.
struct J1939NodeEntry : ecu_executable::J1939NodeOverride
{
    std::size_t line = 0;

    friend bool operator==(const J1939NodeEntry&, const J1939NodeEntry&) = default;
};

struct ParticipantConfig
{
    /// The participant name (on SIL Kit); an ecu_main executable gets it with --name.
    std::string name;
    /// The executable (a path; relative: from the directory of the configuration file) and its
    /// arguments.
    std::vector<std::string> command;
    /// true: an ecu_main executable; vehicle_sil_launch asks it for its description
    /// (--describe) and gives it --name and --registry-uri. false: any executable, run with the
    /// arguments as written, the placeholders replaced.
    bool describe = true;
    /// false: the simulation may start without it.
    bool required = true;
    /// false: it is not part of the simulation (its "enabled" key, or --disable): not checked, not
    /// started, not waited for. Its entry is read and checked like any other.
    bool enabled = true;
    /// true: it is not enabled because the chosen variant says so ("enabled: false" in the
    /// variant), and the command line did not take it in again (--enable). A scenario skips the
    /// steps that use its variables; one that is left out in any other way is an error there.
    bool left_out_by_variant = false;
    /// true: someone starts it (from a debugger, say), not vehicle_sil_launch run (its "manual"
    /// key, or --manual): run prints the command to start it with and waits for it to join. It is
    /// checked as any other.
    bool manual = false;
    /// What it is for (its "role" key; ParticipantRole::Ecu if it has none or an error there).
    ParticipantRole role = ParticipantRole::Ecu;
    /// true: the entry has a role key (even role: ecu). A participant with describe: false and no
    /// role may be a bridge to the check, unless another participant has role: bridge.
    bool role_written = false;
    /// The step the executable must report, in milliseconds; empty: any.
    std::optional<std::int64_t> step_ms{};
    /// Its variable map (its "variables" key): the names and values under which the simulation
    /// sees the executable's variables (control::VariableMapping). Only the mappings that are one
    /// of the forms (the others are reported when reading); whether they fit the executable's
    /// variables is the check's to say (seen_variables()).
    control::VariableMap variables{};
    std::vector<std::size_t> variable_lines{}; ///< the line of each mapping of `variables`
    /// Its "network_names" as written (also the lines that rename nothing), in the file's order;
    /// with a variant, the variant's lines laid over them. Only lines of the right form (the
    /// others are reported when reading). What they do: renamed_networks().
    std::vector<NetworkName> network_names{};
    /// Its "j1939_nodes" as written (also the entries that change nothing), in the file's order;
    /// with a variant, the variant's entries laid over them. Only entries of the right form (the
    /// others are reported when reading). What run gives: j1939_node_arguments(); what the check
    /// makes of them: seen_networks().
    std::vector<J1939NodeEntry> j1939_nodes{};
    std::size_t line = 0;         ///< the line of the participant's entry
    std::size_t command_line = 0; ///< the line of its command (its entry's line if it has none)
    std::size_t step_line = 0;    ///< the line of step_ms (its entry's line if it has none)
    /// false if the entry cannot be described or compared (no name, no command, a duplicate or
    /// reserved name, a launcher argument or an unknown placeholder in the command, a describe
    /// that is not true or false, an error in network_names or j1939_nodes; reported when
    /// reading). An unknown
    /// key or a bad step_ms or required does not make it invalid.
    bool valid = true;
};

/// The lines of the participant's network_names that rename a network (NetworkName::renames()), in
/// their order: what run gives the executable, and what the check applies to its description.
[[nodiscard]] std::vector<NetworkName> renamed_networks(const ParticipantConfig& participant);

/// What run adds to the participant's arguments for its network_names: "--map-network",
/// "<simulation's name>=<executable's name>" for each of renamed_networks(); empty without one.
[[nodiscard]] std::vector<std::string> map_network_arguments(const ParticipantConfig& participant);

/// What run adds to the participant's arguments for its j1939_nodes (after map_network_arguments()):
/// "--j1939-node", ecu_executable::format_j1939_node_argument() of each entry that changes
/// something, in the file's order; empty without one.
[[nodiscard]] std::vector<std::string> j1939_node_arguments(const ParticipantConfig& participant);

/// The settings of `node` as a configuration file writes them, for messages: "from: 0x00,
/// address: 0x01, identity_number: 2" (the settings that are given, in the order of
/// j1939::kNodeOverrideSettings; an address as j1939::node_override_address_text()).
[[nodiscard]] std::string j1939_node_settings_yaml(const j1939::NodeOverride& node);
/// One entry of j1939_nodes as a configuration file writes it, for messages:
/// "{ network: VehicleCAN, from: 0x00, address: 0x01 }".
[[nodiscard]] std::string j1939_node_yaml(const ecu_executable::J1939NodeOverride& entry);

/// Which word of the features of an executable's --describe a setting of j1939_nodes needs. One
/// row for each setting: a setting added later gets its own word ("j1939-node.<setting>"), so
/// that an executable built before it is told apart from one that takes it.
struct J1939NodeSettingFeature
{
    std::string_view setting; ///< as in j1939_nodes and --j1939-node: "from", "address", ...
    std::string_view feature; ///< "j1939-node"
};
inline constexpr J1939NodeSettingFeature kJ1939NodeSettingFeatures[] = {
    {"from", ecu_executable::kFeatureJ1939Node},
    {"address", ecu_executable::kFeatureJ1939Node},
    {"identity_number", ecu_executable::kFeatureJ1939Node},
    {"function_instance", ecu_executable::kFeatureJ1939Node},
    {"ecu_instance", ecu_executable::kFeatureJ1939Node},
};

/// A word of the features that the participant's j1939_nodes need, with the first setting and
/// line that need it.
struct J1939NodeFeatureNeed
{
    std::string feature;
    std::string setting;
    std::size_t line = 0;

    friend bool operator==(const J1939NodeFeatureNeed&, const J1939NodeFeatureNeed&) = default;
};

/// The words of the features that the entries of the participant's j1939_nodes that change
/// something need (`table`: setting -> word), each once, in the order of the entries and of the
/// settings in an entry.
[[nodiscard]] std::vector<J1939NodeFeatureNeed>
required_j1939_node_features(const ParticipantConfig& participant,
                             std::span<const J1939NodeSettingFeature> table = kJ1939NodeSettingFeatures);

/// Where a parameter's value comes from; a later one replaces an earlier one of the same name.
enum class ParamSource
{
    Scenario,        ///< the scenario's "params" (its default)
    Configuration,   ///< the configuration's "params"
    Variant,         ///< the chosen variant's "params"
    Case,            ///< the chosen case's "params"
    RequestFile,     ///< the "params" of the request file (--request); --param replaces it by name
    CommandLine,     ///< --param <name>=<value>
    TemplateDefault, ///< a template's "parameters.<name>.default"
    TemplateCall,    ///< the arguments of a `- template:` call (params:)
};
/// "scenario", "configuration", "variant", "case", "request_file", "command_line", "template_default", "template_call" (the outputs' "from").
[[nodiscard]] const char* param_source_name(ParamSource source);
/// For a message: "the scenario's params", "the configuration's params", "the variant's params", "the case's params", "--param",
/// "the request file's params", "the template's default", "the template call's params".
[[nodiscard]] const char* param_source_text(ParamSource source);

/// Which values of a request came from a request file (--request) and not from the command line: a
/// message about one of them names the key and the file, not an option the user did not write.
struct RequestOrigin
{
    /// The request file as the user named it; empty: there is none.
    std::string file{};
    /// The keys of the file whose value the request has ("speed", "start_paused", "time_policy",
    /// "scenario", ...): not one that an argument replaced.
    std::vector<std::string> fields{};
    /// The networks whose target the request has from the file's "connect".
    std::vector<std::string> connected{};
    /// The participants the request has from the file's "disable", "enable" and "manual" only (the
    /// command line does not name them there), each as "<key> <name>": "manual Engine".
    std::vector<std::string> participants{};

    [[nodiscard]] bool has(std::string_view field) const
    {
        return std::find(fields.begin(), fields.end(), field) != fields.end();
    }
    /// How a message names `field`: `argument` ("--speed 5") if the command line gave it, `key`
    /// and the file ("speed 5 in bench.json") if the request file did.
    [[nodiscard]] std::string named(std::string_view field, std::string_view argument, std::string_view key) const
    {
        return has(field) ? std::string(key) + " in " + file : std::string(argument);
    }

    friend bool operator==(const RequestOrigin&, const RequestOrigin&) = default;
};

/// A parameter of the scenarios: a name ("idle_rpm": letters, digits and '_', not a digit first)
/// and the value a scenario's "${idle_rpm}" stands for, as it was written.
struct ParamValue
{
    std::string name;
    std::string text;    ///< the value as written, without its quotes
    bool quoted = false; ///< written in quotes: a text, also if it reads as a number
    ParamSource source = ParamSource::Configuration;
    std::size_t line = 0;  ///< in its file (0: the command line)
    bool from_cli = false; ///< true when parsed from CLI --param (shell strips quotes)

    friend bool operator==(const ParamValue&, const ParamValue&) = default;
};

/// true if `name` can be a parameter's name.
[[nodiscard]] bool is_param_name(std::string_view name);

/// What a variant changes of one participant of the common part. The keys a variant may write
/// are these (kVariantParticipantKeys in config.cpp): a new one gets a member here, a line in that
/// list, in the reader and in apply_variant().
struct VariantParticipant
{
    std::string name;
    std::size_t line = 0;
    /// "command": the executable and its arguments instead of the common ones.
    std::optional<std::vector<std::string>> command{};
    std::size_t command_line = 0;
    /// "variables": laid over the common variable map, mapping by mapping (by the name the
    /// simulation uses).
    control::VariableMap variables{};
    std::vector<std::size_t> variable_lines{};
    /// "network_names": laid over the common ones, line by line (by the simulation's name). A
    /// line whose two names are the same takes the common line away.
    std::vector<NetworkName> network_names{};
    /// "j1939_nodes": laid over the common ones, entry by entry (by network and from, also when
    /// both have no from): an entry takes the place of the common one whole. An entry that changes
    /// nothing takes the common one away.
    std::vector<J1939NodeEntry> j1939_nodes{};
    /// "enabled".
    std::optional<bool> enabled{};
    /// false if its command, its network_names or its j1939_nodes (with the common ones) has an
    /// error (reported when reading): the participant cannot be described or started in this
    /// variant.
    bool valid = true;
};

/// One variant of the simulation ("variants"): the common part with these changes.
struct VariantConfig
{
    std::string name;
    std::size_t line = 0;
    std::vector<VariantParticipant> participants{};
    std::vector<ParamValue> params{}; ///< over the common "params", by name
};

struct SimulationConfig
{
    /// Where the registry of the simulation listens ("silkit://<host>:<port>"); empty: a free port
    /// on 127.0.0.1.
    std::optional<std::string> registry_uri{};
    /// The step of the launcher's controller in the time synchronisation (milliseconds of virtual
    /// time) as written; empty: calculate it for each run.
    std::optional<std::int64_t> time_step_ms{};
    std::vector<NetworkConfig> networks{};
    std::vector<ParticipantConfig> participants{};
    std::size_t participants_line = 0; ///< the line of participants (0 if it is missing)
    /// The variants the file defines ("variants"), in the file's order; empty: it has none.
    std::vector<VariantConfig> variants{};
    /// The variant that was applied to `participants` and `params` (--variant), or "".
    std::string variant{};
    /// The scenarios' parameters ("params"); with a variant, its params laid over them.
    std::vector<ParamValue> params{};
};

/// The coarsest automatic controller step, in milliseconds.
inline constexpr std::int64_t kDefaultTimeStepMs = 10;
/// The default late_tolerance_ms: small against the timeouts of a device that is attached in real
/// time, large against the jitter of an operating system's timers. It does not follow the step.
inline constexpr std::int64_t kDefaultLateToleranceMs = 10;

/// The range of a speed (as SimulationController takes it).
inline constexpr double kMinSpeed = 1e-6;
inline constexpr double kMaxSpeed = 1e6;
/// Reads a speed as the request file and `run --speed` write it: "max" (`speed` is set
/// empty: as fast as possible) or a decimal number from kMinSpeed to kMaxSpeed ("1", "0.5",
/// "2.5e-1"). false if `text` is neither.
[[nodiscard]] bool parse_speed(std::string_view text, std::optional<double>& speed);
/// A speed for people and the JSON's text: "max", or the number in its shortest form.
[[nodiscard]] std::string speed_text(const std::optional<double>& speed);

/// The command line's choices about the file: the variant (--variant), and the changes to the
/// participants' "enabled" and "manual" keys: the names of the participants to leave out
/// (--disable), to take in (--enable) and to start by hand (--manual). They come after the variant.
struct EnabledOverrides
{
    std::vector<std::string> disable{};
    std::vector<std::string> enable{};
    std::vector<std::string> manual{};
    /// --variant: the variant to apply. A file that defines variants needs one (none is the
    /// default); a file without variants takes none.
    std::optional<std::string> variant{};
    /// Which of the names a request file gave: a usage error about one of them names the key and
    /// the file ("manual Engine in bench.json"), not an option.
    RequestOrigin origin{};
};

/// true if the start waits for a participant that someone starts by hand (enabled, required and
/// manual): the start then has no time limit for it (docs/simulation-config.md).
[[nodiscard]] bool waits_for_manual(const SimulationConfig& config);

/// true if other computers may reach the registry of `config`: registry_uri is written and not a
/// loopback address (is_loopback_registry_uri() of vehicle_sil/core/registry_uri.hpp).
[[nodiscard]] bool registry_on_network(const SimulationConfig& config);

/// The value of VEHICLE_SIL_LISTEN_ON_NETWORK (kListenOnNetworkVariable) that run gives the
/// participants it starts, whatever its own environment has: "1" if registry_on_network(), else "0".
[[nodiscard]] std::string_view listen_variable_value(const SimulationConfig& config);

/// The note "listen-variable-ignored" if vehicle_sil_launch's own environment has
/// VEHICLE_SIL_LISTEN_ON_NETWORK (`inherited`; null: not set) with another value than
/// listen_variable_value(): run does not pass it on.
[[nodiscard]] std::optional<Finding> inherited_listen_variable(const SimulationConfig& config, const char* inherited);

/// The top-level keys of a configuration that say which commands run, and how they reach the
/// registry: what compare_commands() (check.hpp) compares.
inline constexpr std::string_view kCommandParts[] = {"registry_uri", "participants", "variants"};

/// What read_config() found.
struct ConfigRead
{
    /// Empty if the text cannot be read as a configuration at all (a syntax error, not a mapping,
    /// an unknown format version); `findings` then says why.
    std::optional<SimulationConfig> config;
    /// Every error and warning of the text, in the order of the file.
    std::vector<Finding> findings;
    /// The overrides' usage errors (code "usage"), every one: a name given to both --disable and
    /// --enable (always); a name that the file does not have (only if the text has no error: an
    /// error in the text may be why the name is not there, so it is corrected first; meanwhile the
    /// overrides of the names the file has are applied). If there is one, no override is applied.
    /// The variant's come first, and alone: no --variant for a file that defines variants
    /// and a --variant that is none of the file's variants (always; the message lists them), a
    /// --variant for a file without variants (only if the text has no error).
    std::vector<Finding> override_errors{};
    /// The keys of kCommandParts whose text has an error (in `findings`; also a key written twice),
    /// each once, in the order they were read: what they say is then not known for sure, and
    /// compare_commands() does not compare the text.
    std::vector<std::string> command_parts_with_errors{};
};

/// The overrides' usage errors that do not need the file: each name given to both --disable and
/// --enable, or to both --disable and --manual.
[[nodiscard]] std::vector<Finding> override_conflicts(const EnabledOverrides& overrides);

/// Reads a configuration (YAML, or JSON if it starts with '{'; a UTF-8 byte order mark is
/// skipped) and checks what the text alone can tell: the format version, unknown and missing
/// keys, the types and ranges of the values, duplicate and reserved names, launcher arguments
/// and placeholders in the commands. Reports every problem at once. No file system access.
/// Every variant is read and checked, also the ones that are not chosen. `overrides` choose the
/// variant, which is laid over the common part (ConfigRead::config is the simulation of that
/// variant), and then change the participants' "enabled", before the rules that depend on it (a
/// required participant among the enabled ones).
[[nodiscard]] ConfigRead read_config(std::string_view text, const EnabledOverrides& overrides = {});

/// An error if `path` (resolve_executable() in the launcher's launch/probe.hpp), where the file system has `status`, is
/// not an executable file of `participant`: missing (with a hint for PATH and for arguments in one
/// text), a directory, a batch file (.bat, .cmd), or (POSIX) without an execute permission.
[[nodiscard]] std::optional<Finding> executable_finding(const ParticipantConfig& participant,
                                                        const std::filesystem::path& path,
                                                        const std::filesystem::file_status& status);

/// The arguments with the placeholders replaced and "{{word}}" as "{word}"; any other "{word}" and
/// any other brace stay as they are.
[[nodiscard]] std::vector<std::string> expand_placeholders(const std::vector<std::string>& arguments,
                                                           std::string_view registry_uri, std::string_view name);

namespace detail {
/// Lays `variant` over the common part `config` as --variant does (read_config()): every key a
/// variant may write (kVariantParticipantKeys in config.cpp), and SimulationConfig::variant.
void apply_variant(SimulationConfig& config, const VariantConfig& variant);
/// The number of single-character edits between `a` and `b` (for "did you mean").
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b);
/// The candidate closest to `word` if it is close enough to be a typing error, or "".
[[nodiscard]] std::string closest(std::string_view word, const std::vector<std::string>& candidates);
} // namespace detail

} // namespace vehicle_sil::config
