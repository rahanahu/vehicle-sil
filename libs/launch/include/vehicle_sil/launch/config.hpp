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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// The version of the configuration format (its "vehicle_sil_simulation" key).
inline constexpr int kSimulationFormat = 1;

/// The longest duration in milliseconds the configuration takes (about 31 years). It is also the
/// largest --start-timeout-ms (and --run-for-ms) that ecu_main accepts (docs/ecu-executable.md):
/// both take it from vehicle_sil/ecu/executable.hpp.
inline constexpr std::int64_t kMaxMillis = ecu_executable::kMaxMillis;

/// The participant name of the controller that vehicle_sil_launch runs; no participant may use it.
inline constexpr std::string_view kControllerName = "SimulationController";

/// The arguments that vehicle_sil_launch gives an ecu_main executable itself; its command must
/// not have them (also not as "--name=..."). docs/ecu-executable.md has their meaning. The list is
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

struct NetworkConfig
{
    std::string name;
    NetworkKind kind = NetworkKind::Can;
    std::size_t line = 0;
    /// false if its kind is missing or wrong (reported when reading): it is not checked.
    bool valid = true;
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
    /// true: someone starts it (from a debugger, say), not vehicle_sil_launch run (its "manual"
    /// key, or --manual): run prints the command to start it with and waits for it to join. It is
    /// checked as any other.
    bool manual = false;
    /// The step the executable must report, in milliseconds; empty: any.
    std::optional<std::int64_t> step_ms{};
    /// Its variable map (its "variables" key): the names and values under which the simulation
    /// sees the executable's variables (control::VariableMapping). Only the mappings that are one
    /// of the forms (the others are reported when reading); whether they fit the executable's
    /// variables is the check's to say (seen_variables()).
    control::VariableMap variables{};
    std::vector<std::size_t> variable_lines{}; ///< the line of each mapping of `variables`
    std::size_t line = 0;         ///< the line of the participant's entry
    std::size_t command_line = 0; ///< the line of its command (its entry's line if it has none)
    std::size_t step_line = 0;    ///< the line of step_ms (its entry's line if it has none)
    /// false if the entry cannot be described or compared (no name, no command, a duplicate or
    /// reserved name, a launcher argument or an unknown placeholder in the command, a describe
    /// that is not true or false; reported when reading). An unknown key or a bad step_ms or
    /// required does not make it invalid.
    bool valid = true;
};

/// Where a parameter's value comes from; a later one replaces an earlier one of the same name.
enum class ParamSource
{
    Scenario,      ///< the scenario's "params" (its default)
    Configuration, ///< the configuration's "params"
    Variant,       ///< the chosen variant's "params"
    CommandLine,   ///< --param <name>=<value>
};
/// "scenario", "configuration", "variant", "command_line" (the outputs' "from").
[[nodiscard]] const char* param_source_name(ParamSource source);
/// For a message: "the scenario's params", "the configuration's params", "the variant's params", "--param".
[[nodiscard]] const char* param_source_text(ParamSource source);

/// A parameter of the scenarios: a name ("idle_rpm": letters, digits and '_', not a digit first)
/// and the value a scenario's "${idle_rpm}" stands for, as it was written.
struct ParamValue
{
    std::string name;
    std::string text;    ///< the value as written, without its quotes
    bool quoted = false; ///< written in quotes: a text, also if it reads as a number
    ParamSource source = ParamSource::Configuration;
    std::size_t line = 0; ///< in its file (0: the command line)

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
    /// "enabled".
    std::optional<bool> enabled{};
    /// false if its command has an error (reported when reading): the participant cannot be
    /// described or started in this variant.
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
    /// When to stop, in milliseconds of virtual time; empty: when someone stops it.
    std::optional<std::int64_t> stop_at_ms{};
    /// How long to wait for the required participants to join, in milliseconds (wall clock).
    std::int64_t start_timeout_ms = 30000;
    /// How long vehicle_sil_launch run waits, at each step of ending the participants, for them to
    /// end, in milliseconds (wall clock).
    std::int64_t stop_timeout_ms = 10000;
    /// Where the registry of the simulation listens ("silkit://<host>:<port>"); empty: a free port
    /// on 127.0.0.1.
    std::optional<std::string> registry_uri{};
    /// The step of the launcher's controller in the time synchronisation (milliseconds of virtual
    /// time) as written; empty: the default (time_step_ms()).
    std::optional<std::int64_t> time_step_ms{};
    /// Simulation seconds per wall-clock second; empty: as fast as possible ("max", or not written).
    std::optional<double> speed{};
    /// A controller step that begins more than this (wall-clock milliseconds) after the wall-clock
    /// time its start stands for is late; empty: kDefaultLateToleranceMs (late_tolerance_ms()).
    std::optional<std::int64_t> late_tolerance_ms{};
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

/// The default step of the launcher's controller, in milliseconds: the step of a run as fast as
/// possible, and the coarsest step of a run with a speed (see time_step_ms()).
inline constexpr std::int64_t kDefaultTimeStepMs = 10;
/// The default late_tolerance_ms: small against the timeouts of a device that is attached in real
/// time, large against the jitter of an operating system's timers. It does not follow the step.
inline constexpr std::int64_t kDefaultLateToleranceMs = 10;

/// The step of the launcher's controller in milliseconds as the file alone gives it: time_step_ms
/// if written; else kDefaultTimeStepMs, or, if that does not divide stop_at_ms, the greatest
/// common divisor of both (the controller can stop the simulation only at one of its steps). A run
/// with a speed makes the default finer if the ECUs' steps ask for it (run.hpp,
/// detail::plan_time_step()).
[[nodiscard]] std::int64_t time_step_ms(const SimulationConfig& config);
/// late_tolerance_ms if written, else kDefaultLateToleranceMs.
[[nodiscard]] std::int64_t late_tolerance_ms(const SimulationConfig& config);

/// The range of a speed (as SimulationController takes it).
inline constexpr double kMinSpeed = 1e-6;
inline constexpr double kMaxSpeed = 1e6;
/// Reads a speed as the configuration's "speed" and `run --speed` write it: "max" (`speed` is set
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
};

/// true if the start waits for a participant that someone starts by hand (enabled, required and
/// manual): the start then has no time limit for it (docs/simulation-config.md).
[[nodiscard]] bool waits_for_manual(const SimulationConfig& config);

/// true if other computers may reach the registry of `config`: registry_uri is written and not a
/// loopback address (is_loopback_registry_uri() of vehicle_sil/core/participant_runtime.hpp).
[[nodiscard]] bool registry_on_network(const SimulationConfig& config);

/// The value of VEHICLE_SIL_LISTEN_ON_NETWORK (kListenOnNetworkVariable) that run gives the
/// participants it starts, whatever its own environment has: "1" if registry_on_network(), else "0".
[[nodiscard]] std::string_view listen_variable_value(const SimulationConfig& config);

/// The note "listen-variable-ignored" if vehicle_sil_launch's own environment has
/// VEHICLE_SIL_LISTEN_ON_NETWORK (`inherited`; null: not set) with another value than
/// listen_variable_value(): run does not pass it on.
[[nodiscard]] std::optional<Finding> inherited_listen_variable(const SimulationConfig& config, const char* inherited);

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
    /// (always; the message lists them), a --variant the file does not have or for a file without
    /// variants (only if the text has no error).
    std::vector<Finding> override_errors{};
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

/// Where a participant's executable is: command[0], relative to `base_dir` unless absolute
/// (UTF-8 in, a path that works with non-ASCII characters on Windows out; not normalized, so that
/// ".." keeps its meaning across symbolic links). On Windows a path that does not exist and does
/// not end in ".exe" gets ".exe" if that file exists. PATH is not searched (find_executable()).
[[nodiscard]] std::filesystem::path resolve_executable(std::string_view executable, const std::filesystem::path& base_dir);

/// Where command[0] of a participant is.
struct FoundExecutable
{
    std::filesystem::path path;
    /// It was not next to the configuration file but on PATH (`path` is then where).
    bool on_path = false;
};

/// resolve_executable(); if no executable file is there and `executable` is a bare name (no '/' or
/// '\\'), the first directory of PATH that has it as an executable file (on Windows also with
/// ".exe"). Without a match: resolve_executable()'s path.
[[nodiscard]] FoundExecutable find_executable(std::string_view executable, const std::filesystem::path& base_dir);

/// An error if `path` (resolve_executable()) is not an executable file of `participant`:
/// missing (with a hint for PATH and for arguments in one text), a directory, a batch file
/// (.bat, .cmd), or (POSIX) without an execute permission.
[[nodiscard]] std::optional<Finding> check_executable(const ParticipantConfig& participant, const std::filesystem::path& path);

/// The arguments with the placeholders replaced and "{{word}}" as "{word}"; any other "{word}" and
/// any other brace stay as they are.
[[nodiscard]] std::vector<std::string> expand_placeholders(const std::vector<std::string>& arguments,
                                                           std::string_view registry_uri, std::string_view name);

namespace detail {
/// The number of single-character edits between `a` and `b` (for "did you mean").
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b);
/// The candidate closest to `word` if it is close enough to be a typing error, or "".
[[nodiscard]] std::string closest(std::string_view word, const std::vector<std::string>& candidates);
} // namespace detail

} // namespace vehicle_sil::launch
