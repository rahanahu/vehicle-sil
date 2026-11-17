// SPDX-License-Identifier: MIT
//
// What vehicle_sil_launch finds out from the computer it runs on: where an executable or a bridge
// is, what a file has, what an executable prints (--describe, a bridge's list), what an environment
// variable has. Each function looks once and returns what it found as a value. The rules that turn
// those values into findings (config.hpp, check.hpp, connect.hpp, scenario.hpp) do not look
// themselves and do not include this header; check_file(), list_connections() and run_tests() call
// both. Internal.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"

#include <chrono>
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

/// Reads the scenario file `path` into `text`: "" if it was read, else what is wrong with it (the
/// message of the scenario's finding "file").
[[nodiscard]] std::string read_scenario_file(const std::filesystem::path& path, std::string& text);

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

/// Looks at what is at `path` (resolve_executable()) and gives executable_finding() of it: an error
/// if it is not an executable file of `participant`.
[[nodiscard]] std::optional<Finding> check_executable(const ParticipantConfig& participant, const std::filesystem::path& path);

namespace detail {
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

} // namespace vehicle_sil::launch
