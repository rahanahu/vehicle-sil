// SPDX-License-Identifier: MIT
//
// What vehicle_sil_launch finds out from the computer it runs on: where an executable or a bridge
// is, what a file has, what an executable prints (--describe, a bridge's list), what an environment
// variable has. Each function looks once and returns what it found as a value. The rules that turn
// those values into findings (config.hpp, check.hpp, connect.hpp, scenario.hpp) do not include
// this header; check_file(), list_connections() and run_tests() call both. One look is still on the
// rules' side: bridge_working_directory() (connect.cpp) makes a relative directory absolute.
// Where check_file() takes the participants' --describe outputs from is one value (DescribeSource):
// a run of each, or the outputs that an earlier check saved (saved_descriptions.cpp), which starts
// no process. Internal.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/config/config.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// One run of `executable` with `arguments` (process::run_captured(): at most 1 MiB kept of each
/// output) in `directory` (empty: the launcher's), killed after `timeout`; shown in messages as
/// `shown` followed by the arguments. A participant's --describe, a bridge's --describe or list.
[[nodiscard]] DescribeRun run_tool(const std::filesystem::path& executable, const std::string& shown,
                                   const std::vector<std::string>& arguments, std::chrono::milliseconds timeout,
                                   const std::filesystem::path& directory);

/// Reads the configuration file `path` into `text`: "" if it was read, else what is wrong with it
/// (the message of the finding "file": it does not exist, is a directory or too large, ...).
[[nodiscard]] std::string read_config_file(const std::filesystem::path& path, std::string& text);

/// Reads the text of a configuration from standard input (check --text-from-stdin) into `text`, up
/// to its end, as bytes (on Windows too: "\r\n" stays): "" if it was read, else what is wrong (the
/// message of the finding "file": more than a configuration file may have, or it cannot be read).
[[nodiscard]] std::string read_config_stdin(std::string& text);

/// Reads the scenario file `path` into `text`: "" if it was read, else what is wrong with it (the
/// message of the scenario's finding "file").
[[nodiscard]] std::string read_scenario_file(const std::filesystem::path& path, std::string& text);

/// Reads the request file `path` (--request) into `text`: "" if it was read, else what is wrong
/// with it (it does not exist, is a directory or too large, ...). The same limit as a
/// configuration file's.
[[nodiscard]] std::string read_request_file_text(const std::filesystem::path& path, std::string& text);
/// The value of the environment variable `name` of vehicle_sil_launch itself; empty: not set.
[[nodiscard]] std::optional<std::string> environment_value(std::string_view name);

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

/// What the file system has at `path` (resolve_executable()), looked at once: whether something is
/// there, its type and its permissions. executable_finding() (config.hpp) judges it.
[[nodiscard]] std::filesystem::file_status executable_status(const std::filesystem::path& path);

namespace detail {
/// The command as messages show it, on one line (DescribeRun::command_text): an argument that is
/// empty or has white space, quotes or control characters in quotes, the control characters as
/// \n, \t, \r or \xNN.
[[nodiscard]] std::string command_text(const std::vector<std::string>& words);
/// `path` is a regular file that can be run (POSIX: one with an execute permission).
[[nodiscard]] bool is_runnable_file(const std::filesystem::path& path);
/// The first directory of PATH that has `name` as a runnable file (resolve_executable(): on
/// Windows also with ".exe"). Relative entries of PATH are skipped: they would depend on the
/// directory the launcher runs in. Shared by find_executable() and find_tool().
[[nodiscard]] std::optional<std::filesystem::path> find_on_path(std::string_view name);
} // namespace detail

/// Where the launcher finds the tool `name` (bridge_tool()): in `launcher_dir` (the directory of
/// vehicle_sil_launch; skipped if empty), then in the absolute directories of PATH
/// (detail::find_on_path()); on Windows also with ".exe". Never in the working directory or the
/// directory of the configuration file: a folder received from someone does not get to run a
/// program of its own. Empty if it is in none of them.
[[nodiscard]] std::optional<std::filesystem::path> find_tool(std::string_view name,
                                                             const std::filesystem::path& launcher_dir);

/// The ConnectTools of this computer: find_tool() in `tool_directory`, and run_tool() of the tool
/// found, shown by its name without the directory and the extension, in bridge_working_directory(),
/// with `timeout`.
[[nodiscard]] ConnectTools bridge_tools(const std::filesystem::path& tool_directory, std::chrono::milliseconds timeout);

/// The ScenarioContext::read_file of the scenario file `scenario`: a path is from the directory
/// the file is in (a UTF-8 path, also on Windows). A file larger than 16 MiB, a directory and a
/// file that cannot be opened are problems.
[[nodiscard]] std::function<std::optional<std::string>(const std::string&, std::string&)>
files_beside(const std::filesystem::path& scenario);

// --- the participants' --describe: run, or saved ---

/// Where check_file() takes a participant's --describe output from: given the participant, its
/// executable (find_executable()) and the arguments after command[0] (with --describe --name
/// <name>), the run of it, or nothing if there is none (ParticipantStatus::NotDescribed). The
/// check asks it nothing else of the executables.
using DescribeSource =
    std::function<std::optional<DescribeRun>(const ParticipantConfig& participant, const std::filesystem::path& executable,
                                             const std::vector<std::string>& arguments)>;

/// The DescribeSource that runs each executable (run_tool()) in `directory`, killed after `timeout`.
[[nodiscard]] DescribeSource describe_by_running(std::chrono::milliseconds timeout, const std::filesystem::path& directory);

/// What the file system has at an executable, to tell later whether it is the one that described
/// itself: its absolute path (symbolic links resolved where they exist), size and time of its last
/// change.
struct ExecutableStamp
{
    std::string path; ///< UTF-8
    std::uintmax_t size = 0;
    /// The time of the last change in the file system's own unit; compared, never shown.
    std::int64_t modified = 0;

    friend bool operator==(const ExecutableStamp&, const ExecutableStamp&) = default;
};

/// The stamp of `executable`; empty if the file system cannot tell its size or time.
[[nodiscard]] std::optional<ExecutableStamp> executable_stamp(const std::filesystem::path& executable);

/// One participant's --describe output as check --write-descriptions saved it.
struct SavedDescription
{
    std::string name;
    /// Its command as the configuration wrote it (ParticipantConfig::command: placeholders not expanded).
    std::vector<std::string> command{};
    ExecutableStamp executable{};
    /// What --describe wrote on standard output.
    std::string output{};
};

/// A file of saved descriptions, as read.
struct SavedDescriptions
{
    std::vector<SavedDescription> entries{};
    /// The file is there but cannot be used: why (it cannot be read, it is no such file, another
    /// vehicle-sil wrote it). Empty: it was read, or it does not exist (then there are no entries).
    std::string problem{};
};

/// Reads the file that write_saved_descriptions() wrote. Its form is this launcher's own: a file of
/// another vehicle-sil version is not used (SavedDescriptions::problem).
[[nodiscard]] SavedDescriptions read_saved_descriptions(const std::filesystem::path& file);

/// Writes `entries` to `file` in place of what it had: first to a new file beside it, then renamed
/// over it, so that a check that ends half way leaves the file as it was. "" if it was written, else
/// what went wrong.
[[nodiscard]] std::string write_saved_descriptions(const std::filesystem::path& file,
                                                   const std::vector<SavedDescription>& entries);

/// The DescribeSource that starts no process: the output of the entry of `saved` with the
/// participant's name and command whose executable has the same stamp now (executable_stamp());
/// nothing if there is none.
[[nodiscard]] DescribeSource describe_from_saved(std::vector<SavedDescription> saved);

} // namespace vehicle_sil::launch
