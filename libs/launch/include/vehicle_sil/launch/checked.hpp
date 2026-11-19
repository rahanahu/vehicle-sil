// SPDX-License-Identifier: MIT
//
// check_file(): the check of one file as a use case. What it returns is the configuration's check
// (CheckReport, check.hpp) and, with --scenario, the scenario that was read with it
// (scenario_check.hpp). The outputs of both together are format_text() and format_json() of
// Checked; docs/simulation-check.md. Internal.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/scenario/scenario_check.hpp"

#include <functional>
#include <optional>
#include <string>

namespace vehicle_sil::launch {

struct RunRequest; // request.hpp

/// The version of the JSON that format_json() writes (its "vehicle_sil_check" key).
inline constexpr int kCheckFormat = 1;

/// What check_file() returns: the check of the configuration, with the scenario of --scenario.
/// The findings of the scenario are in CheckReport::findings (with Finding::file set), so that the
/// counts, the exit code and the order stay one: `scenario` only says what was read. A run, a test
/// and the outputs read it from here; the checks of the configuration take the CheckReport part.
/// Copying it into a CheckReport drops the scenario: take a result of check_file() as a Checked.
struct Checked : config::CheckReport
{
    /// --scenario: the scenario file that was read and checked.
    std::optional<scenario::ScenarioCheck> scenario{};
};

namespace detail {
/// The scenario of a check, or null without --scenario: what the outputs' pieces take
/// (check_format.hpp).
[[nodiscard]] inline const scenario::ScenarioCheck* scenario_of(const Checked& checked)
{
    return checked.scenario ? &*checked.scenario : nullptr;
}
} // namespace detail

/// The report for people: the participants and the networks as tables, then each error and
/// warning as "<severity>: <file>:<line>: <message> [<code>]" (with the detail indented below), then a
/// summary line; with --scenario also the scenario's line, its parameters and its lines. LF line
/// ends; the same report gives the same text.
[[nodiscard]] std::string format_text(const Checked& checked);

/// The report as JSON (format kCheckFormat): fixed keys in a fixed order, the layout of to_json();
/// with --scenario also the member "scenario". docs/simulation-check.md has the keys.
[[nodiscard]] std::string format_json(const Checked& checked);

/// The process exit code of `check` (the table of the exit codes is in one place, exit_codes.hpp):
/// 4 the command line was wrong, 3 the file was not read, 1 it did not pass, 0 it passed.
[[nodiscard]] int check_exit_code(const config::CheckReport& report);

/// How a check is made, beside what is asked for (RunRequest): what its caller is, not what the
/// user wants.
struct CheckOptions
{
    /// The request's parameters are for many scenarios (ScenarioContext::params_for_many): one
    /// that its scenario does not have is no error.
    bool params_for_many = false;
    /// Only read the file: no executable is looked for or run, and no scenario is read. The
    /// report has the file's findings, its variants and the overrides' usage errors.
    bool file_only = false;
    /// test: without the time lock every run goes as fast as possible, whatever the file's speed
    /// (which then does not count for time_lock_findings()); RunIntent::speed is that default
    /// then, not a speed the user asked for, and the check does not judge it.
    bool fastest_by_default = false;
};

/// Checks the file of `request` (RunHost::config, shown as given): reads it, checks the
/// executables, runs each --describe in turn (not in parallel), compares. A disabled participant's
/// executable is neither looked for nor run. With RunIntent::scenario, the scenario file is read
/// and checked against the participants' descriptions as well (if the configuration could be
/// read). Never throws for a problem of a file or of an executable: they are findings. The
/// scenario of a request file (RunRequest::origin has "scenario") is read from the directory of the
/// configuration file, and shown as the path from the working directory; any other from the
/// working directory, as it is named.
///
/// Of `request` it reads the file, the intent (the speed and --start-paused for
/// time_lock_findings(): the speed unless `options.fastest_by_default`), the --describe limit,
/// where the tools are, and what a check alone has: the configuration's text from standard input
/// (RunHost::config_from_stdin; then RunHost::config only says where it is), the saved descriptions
/// (RunHost::descriptions: then it starts no process at all: a participant without a saved output
/// is ParticipantStatus::NotDescribed, and --connect is checked for its form and names only) and
/// the file to save them to (RunHost::write_descriptions; an error descriptions-write if it cannot
/// be written, after the participants were described), and the file to compare the commands with
/// (RunHost::compare_commands: CheckReport::commands, read and compared once the configuration is
/// read, before anything is looked for). What only a run has (the logs, the
/// timeline, the time limit, the control server, the commands of standard input) it does not read.
///
/// `on_time_lock`, if given, is called once with Checked::time_lock as soon as the
/// configuration has been read (with --variant, --disable and --enable applied), before any
/// executable is looked for or run; not at all if the configuration could not be read. A run
/// gives its controls the lock there (run_file()), so that they know it before anything starts.
[[nodiscard]] Checked check_file(const RunRequest& request, const CheckOptions& options = {},
                                   const std::function<void(const config::TimeLock& lock)>& on_time_lock = {});

} // namespace vehicle_sil::launch
