// SPDX-License-Identifier: MIT
//
// What the launcher finds out from the computer (probe.hpp): the file system, the environment and
// the processes it runs for their output. Nothing here makes a finding: the rules judge what it
// found.
#include "vehicle_sil/launch/probe.hpp"

#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/paths.hpp"

#include "vehicle_sil/scenario/detail/scenario_csv.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace vehicle_sil::launch {

std::string detail::command_text(const std::vector<std::string>& words)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const auto& word : words)
    {
        bool quote = word.empty();
        std::string shown;
        for (const char c : word)
        {
            const auto byte = static_cast<unsigned char>(c);
            quote = quote || byte <= ' ' || c == '"' || c == '\'';
            if (c == '\n')
            {
                shown += "\\n";
            }
            else if (c == '\t')
            {
                shown += "\\t";
            }
            else if (c == '\r')
            {
                shown += "\\r";
            }
            else if (byte < ' ' || byte == 0x7F)
            {
                shown += std::string("\\x") + kHex[byte >> 4] + kHex[byte & 0xF];
            }
            else
            {
                shown += c;
            }
        }
        out += (out.empty() ? "" : " ") + (quote ? "'" + shown + "'" : shown);
    }
    return out;
}

config::DescribeRun run_tool(const std::filesystem::path& executable, const std::string& shown,
                     const std::vector<std::string>& arguments, std::chrono::milliseconds timeout,
                     const std::filesystem::path& directory)
{
    std::vector<std::string> words{shown};
    words.insert(words.end(), arguments.begin(), arguments.end());
    config::DescribeRun run;
    run.command_text = detail::command_text(words);
    run.timeout = timeout;
    try
    {
        process::Captured captured = process::run_captured(std::filesystem::absolute(executable), arguments,
                                                           process::CaptureOptions{timeout, 1u << 20, directory});
        run.exit_code = captured.exit_code;
        run.signal = captured.signal;
        run.output_truncated = captured.output_truncated;
        run.error_truncated = captured.error_truncated;
        run.standard_output = std::move(captured.standard_output);
        run.standard_error = std::move(captured.standard_error);
    }
    catch (const process::StartError& e)
    {
        run.start_error = e.what();
        run.start_refused_by_system = e.cause() == process::StartError::Cause::System;
    }
    catch (const std::exception& e)
    {
        run.start_error = e.what();
        run.start_refused_by_system = true;
    }
    return run;
}

namespace {
/// The largest configuration file read.
constexpr std::uintmax_t kMaxConfigBytes = std::uintmax_t{16} << 20;
} // namespace

std::string read_config_file(const std::filesystem::path& path, std::string& text)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return "the file does not exist: check the path";
    }
    if (std::filesystem::is_directory(path, ec))
    {
        return "this is a directory: name the configuration file in it";
    }
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return "this is not a regular file (a device or a pipe?): name the configuration file";
    }
    if (std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    {
        return "the file is larger than " + std::to_string(kMaxConfigBytes >> 20) +
               " MiB, too large for a configuration: name the configuration file";
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return "cannot open the file: check its permissions";
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return {};
}

std::string read_config_stdin(std::string& text)
{
#ifdef _WIN32
    // As bytes: the text mode would take the '\r' of "\r\n" away and end at a Ctrl+Z.
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
#endif
    text.clear();
    char buffer[1 << 16];
    for (;;)
    {
        const std::size_t got = std::fread(buffer, 1, sizeof buffer, stdin);
        text.append(buffer, got);
        if (text.size() > kMaxConfigBytes)
        {
            // Not read to its end: whatever writes it gets a broken pipe.
            text.clear();
            return "the text on standard input (--text-from-stdin) is larger than " +
                   std::to_string(kMaxConfigBytes >> 20) + " MiB, too large for a configuration";
        }
        if (got < sizeof buffer)
        {
            break;
        }
    }
    if (std::ferror(stdin) != 0)
    {
        text.clear();
        return "cannot read the standard input (--text-from-stdin): give the configuration's text on it, or leave "
               "--text-from-stdin out to read the file";
    }
    return {};
}

std::string read_request_file_text(const std::filesystem::path& path, std::string& text)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return "the file does not exist: check the path";
    }
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return "this is not a regular file: name the request file";
    }
    if (std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    {
        return "the file is larger than " + std::to_string(kMaxConfigBytes >> 20) + " MiB, too large for a request file";
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return "cannot open the file: check its permissions";
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return {};
}

std::string read_scenario_file(const std::filesystem::path& path, std::string& text)
{
    std::error_code ec;
    std::string problem;
    if (!std::filesystem::exists(path, ec))
    {
        problem = "the scenario file does not exist: check the path given with --scenario";
    }
    else if (!std::filesystem::is_regular_file(path, ec))
    {
        problem = "this is not a regular file: name the scenario file with --scenario";
    }
    else if (std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    {
        problem = "the file is larger than " + std::to_string(kMaxConfigBytes >> 20) + " MiB, too large for a scenario";
    }
    else if (std::ifstream in(path, std::ios::binary); !in)
    {
        problem = "cannot open the scenario file: check its permissions";
    }
    else
    {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return problem;
}

std::optional<std::string> environment_value(std::string_view name)
{
    const std::string variable(name);
    const char* value = std::getenv(variable.c_str());
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
}

#ifdef _WIN32
namespace {

/// As config.cpp's: ASCII letters in lower case.
std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace
#endif

std::filesystem::path resolve_executable(std::string_view executable, const std::filesystem::path& base_dir)
{
    std::filesystem::path path = process::path_from_utf8(executable);
    if (path.is_relative())
    {
        path = base_dir / path;
    }
#ifdef _WIN32
    std::error_code ec;
    if (lower(process::utf8_from_path(path.extension())) != ".exe" && !std::filesystem::exists(path, ec))
    {
        auto withExe = path;
        withExe += L".exe";
        if (std::filesystem::exists(withExe, ec))
        {
            return withExe;
        }
    }
#endif
    return path;
}

namespace detail {

bool is_runnable_file(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return false;
    }
#ifndef _WIN32
    const auto perms = std::filesystem::status(path, ec).permissions();
    return (perms & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                     std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
#else
    return true;
#endif
}

std::optional<std::filesystem::path> find_on_path(std::string_view name)
{
#ifdef _WIN32
    const wchar_t* variable = _wgetenv(L"PATH");
    const std::wstring list = variable != nullptr ? variable : L"";
    constexpr wchar_t kSeparator = L';';
#else
    const char* variable = std::getenv("PATH");
    const std::string list = variable != nullptr ? variable : "";
    constexpr char kSeparator = ':';
#endif
    std::size_t begin = 0;
    while (begin <= list.size())
    {
        const std::size_t end = std::min(list.find(kSeparator, begin), list.size());
        const std::filesystem::path directory(list.substr(begin, end - begin));
        begin = end + 1;
        if (directory.empty() || directory.is_relative())
        {
            continue; // a relative entry would depend on the directory the launcher runs in
        }
        std::filesystem::path candidate = resolve_executable(name, directory);
        if (is_runnable_file(candidate))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace detail

FoundExecutable find_executable(std::string_view executable, const std::filesystem::path& base_dir)
{
    FoundExecutable found{resolve_executable(executable, base_dir), false};
    // Next to the file only an executable file counts (a data file of the same name does not
    // hide the program on PATH).
    if (detail::is_runnable_file(found.path) || executable.empty() ||
        executable.find_first_of("/\\") != std::string_view::npos)
    {
        return found;
    }
    if (auto onPath = detail::find_on_path(executable))
    {
        return FoundExecutable{std::move(*onPath), true};
    }
    return found;
}

std::filesystem::file_status executable_status(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::status(path, ec);
}

std::optional<std::filesystem::path> find_tool(std::string_view name, const std::filesystem::path& launcher_dir)
{
    if (!launcher_dir.empty())
    {
        std::filesystem::path beside = resolve_executable(name, launcher_dir);
        if (detail::is_runnable_file(beside))
        {
            return beside;
        }
    }
    return detail::find_on_path(name);
}

config::ConnectTools bridge_tools(const std::filesystem::path& tool_directory, std::chrono::milliseconds timeout)
{
    config::ConnectTools tools;
    tools.launcher_dir = tool_directory;
    tools.find = [tool_directory](std::string_view name) { return find_tool(name, tool_directory); };
    // In the bridge's own directory, not the configuration file's (config::bridge_working_directory()).
    tools.describe = [timeout](const std::filesystem::path& tool, const std::vector<std::string>& arguments) {
        return run_tool(tool, process::utf8_from_path(tool.stem()), arguments, timeout, config::bridge_working_directory(tool));
    };
    return tools;
}

DescribeSource describe_by_running(std::chrono::milliseconds timeout, const std::filesystem::path& directory)
{
    return [timeout, directory](const config::ParticipantConfig& participant, const std::filesystem::path& executable,
                                const std::vector<std::string>& arguments) -> std::optional<config::DescribeRun> {
        return run_tool(executable, participant.command.front(), arguments, timeout, directory);
    };
}

std::function<std::optional<std::string>(const std::string&, std::string&)>
files_beside(const std::filesystem::path& scenario, std::string_view file_kind)
{
    return [directory = scenario.parent_path(),
            kind = std::string(file_kind)](const std::string& name, std::string& problem) -> std::optional<std::string> {
        const std::filesystem::path path = directory / process::path_from_utf8(name);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            problem = "does not exist (looked for " + process::utf8_from_path(path) +
                      "): the path is from the scenario file's directory; check it";
            return std::nullopt;
        }
        if (!std::filesystem::is_regular_file(path, ec))
        {
            problem = "is not a regular file: name " + kind;
            return std::nullopt;
        }
        if (std::filesystem::file_size(path, ec) > scenario::detail::kMaxCsvBytes)
        {
            if (kind == "the CSV file")
            {
                problem = "is larger than " + std::to_string(scenario::detail::kMaxCsvBytes >> 20) +
                          " MiB, too large for points: keep fewer points, or split it into steps of their own";
            }
            else
            {
                problem = "is larger than " + std::to_string(scenario::detail::kMaxCsvBytes >> 20) +
                          " MiB, too large to read: split it into smaller files";
            }
            return std::nullopt;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            problem = "cannot be opened: check its permissions";
            return std::nullopt;
        }
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
}

} // namespace vehicle_sil::launch
