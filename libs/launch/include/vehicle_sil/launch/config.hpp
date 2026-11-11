// SPDX-License-Identifier: MIT
//
// The simulation configuration file of vehicle_sil_launch (YAML or JSON): which participants
// to start, the networks they share and when to stop. Reading it checks only the text (keys,
// types, values, names, the commands' arguments); check.hpp compares it with the executables.
// Internal; the file format is in docs/simulation-config.md.
#pragma once

#include "vehicle_sil/ecu/describe.hpp"

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

/// The participant name of the controller that vehicle_sil_launch runs; no participant may use it.
inline constexpr std::string_view kControllerName = "SimulationController";

/// The arguments that vehicle_sil_launch gives an ecu_main executable itself; its command must
/// not have them (also not as "--name=..."). docs/ecu-executable.md has their meaning.
inline constexpr std::string_view kLauncherArguments[] = {"--describe",   "--name",       "--registry-uri",
                                                          "--local",      "--run-for-ms", "--start-timeout-ms",
                                                          "--help",       "-h"};

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
    /// The step the executable must report, in milliseconds; empty: any.
    std::optional<std::int64_t> step_ms{};
    std::size_t line = 0;         ///< the line of the participant's entry
    std::size_t command_line = 0; ///< the line of its command (its entry's line if it has none)
    std::size_t step_line = 0;    ///< the line of step_ms (its entry's line if it has none)
    /// false if the entry cannot be described or compared (no name, no command, a duplicate or
    /// reserved name, a launcher argument or an unknown placeholder in the command, a describe
    /// that is not true or false; reported when reading). An unknown key or a bad step_ms or
    /// required does not make it invalid.
    bool valid = true;
};

struct SimulationConfig
{
    /// When to stop, in milliseconds of virtual time; empty: when someone stops it.
    std::optional<std::int64_t> stop_at_ms{};
    /// How long to wait for the required participants to join, in milliseconds (wall clock).
    std::int64_t start_timeout_ms = 30000;
    std::vector<NetworkConfig> networks{};
    std::vector<ParticipantConfig> participants{};
};

/// What read_config() found.
struct ConfigRead
{
    /// Empty if the text cannot be read as a configuration at all (a syntax error, not a mapping,
    /// an unknown format version); `findings` then says why.
    std::optional<SimulationConfig> config;
    /// Every error and warning of the text, in the order of the file.
    std::vector<Finding> findings;
};

/// Reads a configuration (YAML, or JSON if it starts with '{'; a UTF-8 byte order mark is
/// skipped) and checks what the text alone can tell: the format version, unknown and missing
/// keys, the types and ranges of the values, duplicate and reserved names, launcher arguments
/// and placeholders in the commands. Reports every problem at once. No file system access.
[[nodiscard]] ConfigRead read_config(std::string_view text);

/// Where a participant's executable is: command[0], relative to `base_dir` unless absolute
/// (UTF-8 in, a path that works with non-ASCII characters on Windows out; not normalized, so that
/// ".." keeps its meaning across symbolic links). On Windows a path that does not exist and does
/// not end in ".exe" gets ".exe" if that file exists. PATH is not searched.
[[nodiscard]] std::filesystem::path resolve_executable(std::string_view executable, const std::filesystem::path& base_dir);

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
