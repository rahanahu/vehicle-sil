// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch check: compares the configuration with what each ecu_main executable
// reports about itself (--describe) and writes the result for people and as JSON. The rules
// are pure functions (no process, no file); check_file() runs them on a file and the
// executables. Internal; the rules and the output: docs/simulation-config.md.
#pragma once

#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/scenario.hpp"

#include <chrono>
#include <cstdint>
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

/// Compares the configuration with what the participants reported (`results` in the order of
/// config.participants): names, steps, networks and their kinds, J1939 addresses and NAMEs (on a
/// CAN network), LIN masters (on a LIN network), unused networks and networks with one
/// participant, participants that were not checked or are disabled, vehicle-sil versions other
/// than `launcher_version`. The rules of a network are those of its kind only: a description
/// with J1939 nodes on a use other than j1939 is not checked for them (read_description()
/// refuses it). A disabled
/// participant (ParticipantConfig::enabled, whatever its result) is not on any network and holds
/// back no rule: the rules are about the simulation without it. Findings in a fixed order:
/// participants in the file's order, then networks in the file's order.
[[nodiscard]] std::vector<Finding> check_participants(const SimulationConfig& config,
                                                      const std::vector<ParticipantResult>& results,
                                                      std::string_view launcher_version);

/// The scenario that was checked with the configuration (--scenario).
struct ScenarioCheck
{
    std::string file; ///< as the user named it
    /// Its "name" and the number of its steps, as far as it could be read.
    std::string name{};
    std::size_t steps = 0;
    /// Set if the configuration could be read and the scenario has no error: it can run.
    std::optional<Scenario> scenario{};
};

/// The result of checking one file.
struct CheckReport
{
    std::string file; ///< the file as the user named it (in messages)
    /// Empty if the file could not be read as a configuration.
    std::optional<SimulationConfig> config{};
    std::vector<ParticipantResult> participants{}; ///< in the order of config->participants
    std::vector<Finding> findings{};               ///< in the order of sort_findings()
    /// --scenario: the scenario file; its findings are in `findings`, with Finding::file set.
    std::optional<ScenarioCheck> scenario{};
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

/// Puts the findings in the report's order: the errors, the warnings, the notes, each by file (the
/// configuration's first) and line (those without a line first), otherwise as found.
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
    /// --scenario: a scenario file to check against the configuration (`scenario_shown`: as the
    /// user named it); empty: none.
    std::filesystem::path scenario{};
    std::string scenario_shown{};
};

/// Checks the file `path` (shown as `shown_as`): reads it, checks the executables, runs each
/// --describe in turn (not in parallel), compares. A disabled participant's executable is neither
/// looked for nor run. With CheckOptions::scenario, the scenario file is read and checked against
/// the participants' descriptions as well (if the configuration could be read). Never throws for a
/// problem of a file or of an executable: they are findings.
[[nodiscard]] CheckReport check_file(const std::filesystem::path& path, const std::string& shown_as,
                                     const CheckOptions& options);

} // namespace vehicle_sil::launch
