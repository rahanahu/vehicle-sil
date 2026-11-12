// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch check: compares the configuration with what each ecu_main executable
// reports about itself (--describe) and writes the result for people and as JSON. The rules
// are pure functions (no process, no file); check_file() runs them on a file and the
// executables. Internal; the rules and the output: docs/simulation-config.md.
#pragma once

#include "vehicle_sil/launch/config.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
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

/// Reads --describe's JSON (format kEcuDescriptionFormat; unknown keys are ignored). Throws
/// DescriptionError, saying what is wrong, for anything else; a message that starts with
/// "format " is an unknown format version.
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

/// Compares the configuration with what the participants reported (`results` in the order of
/// config.participants): names, steps, networks and their kinds, J1939 addresses and NAMEs,
/// LIN masters, unused networks and networks with one participant, participants that were not
/// checked or are disabled, vehicle-sil versions other than `launcher_version`. A disabled
/// participant (ParticipantConfig::enabled, whatever its result) is not on any network and holds
/// back no rule: the rules are about the simulation without it. Findings in a fixed order:
/// participants in the file's order, then networks in the file's order.
[[nodiscard]] std::vector<Finding> check_participants(const SimulationConfig& config,
                                                      const std::vector<ParticipantResult>& results,
                                                      std::string_view launcher_version);

/// The result of checking one file.
struct CheckReport
{
    std::string file; ///< the file as the user named it (in messages)
    /// Empty if the file could not be read as a configuration.
    std::optional<SimulationConfig> config{};
    std::vector<ParticipantResult> participants{}; ///< in the order of config->participants
    std::vector<Finding> findings{};               ///< in the order of sort_findings()
    /// --strict: a warning fails the check too (a note never does).
    bool strict = false;
    /// The command line was wrong (format_json(): result "usage"; the findings say why; `file` stays
    /// as given). check_file() sets it for overrides that do not fit (override_conflicts(),
    /// ConfigRead::override_errors); `config` is then empty.
    bool usage = false;

    [[nodiscard]] std::size_t errors() const;
    [[nodiscard]] std::size_t warnings() const;
    [[nodiscard]] std::size_t notes() const;
    /// true if the file was read and has no error (with `strict`: and no warning).
    [[nodiscard]] bool passed() const;
};

/// Puts the findings in the report's order: the errors, the warnings, the notes, each by line
/// (those without a line first), otherwise as found.
void sort_findings(std::vector<Finding>& findings);

/// The report for people: the participants and the networks as tables, then each error and
/// warning as "<severity>: <file>:<line>: <message>" (with the detail indented below), then a
/// summary line. LF line ends; the same report gives the same text.
[[nodiscard]] std::string format_text(const CheckReport& report);

/// The report as JSON (format kCheckFormat): fixed keys in a fixed order, the layout of
/// to_json(). docs/simulation-config.md has the keys.
[[nodiscard]] std::string format_json(const CheckReport& report);

struct CheckOptions
{
    /// How long one --describe may take.
    std::chrono::milliseconds describe_timeout{10000};
    /// CheckReport::strict.
    bool strict = false;
    /// --disable and --enable.
    EnabledOverrides overrides{};
};

/// Checks the file `path` (shown as `shown_as`): reads it, checks the executables, runs each
/// --describe in turn (not in parallel), compares. A disabled participant's executable is neither
/// looked for nor run. Never throws for a problem of the file or of an executable: they are
/// findings.
[[nodiscard]] CheckReport check_file(const std::filesystem::path& path, const std::string& shown_as,
                                     const CheckOptions& options);

} // namespace vehicle_sil::launch
