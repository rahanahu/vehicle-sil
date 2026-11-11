// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch: puts ECU executables made by different people into one simulation.
//   check <file>   compares the simulation configuration with what each ecu_main executable
//                  reports about itself (--describe) and lists every error and warning.
// The file format, the rules and the output: docs/simulation-config.md.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using namespace vehicle_sil;

constexpr const char* kUsage =
    "usage: vehicle_sil_launch check [--json] [--strict] [--describe-timeout-ms <ms>] [--] <file>\n"
    "  check <file>                compare the simulation configuration <file> (YAML or JSON) with what each\n"
    "                              ecu_main executable in it reports (--describe); list the participants,\n"
    "                              the networks and every error and warning\n"
    "  --json                      write the result as JSON\n"
    "  --strict                    fail on warnings too\n"
    "  --describe-timeout-ms <ms>  how long one executable may take for --describe (default 10000)\n"
    "  --help, -h                  this text\n"
    "  --                          what follows is the file, also if it starts with '-'\n"
    "exit codes: 0 passed, 1 errors (with --strict: or warnings), 3 the file cannot be read, 4 usage\n"
    "the file format and the rules: docs/simulation-config.md\n";

/// The command line as UTF-8: on Windows from the wide command line (argv is in the code page).
std::vector<std::string> utf8_arguments(int argc, char** argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::vector<std::string> out;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; wide != nullptr && i < count; ++i)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        }
        out.push_back(std::move(text));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

/// Writes `text` with "\n" line ends on every system (also on Windows, for the report for people
/// as for the JSON: the same bytes everywhere). false if it could not be written (a closed
/// standard output, a full disk).
bool write_stdout(const std::string& text)
{
#ifdef _WIN32
    std::fflush(stdout);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const bool written = std::fwrite(text.data(), 1, text.size(), stdout) == text.size();
    return std::fflush(stdout) == 0 && written;
}

/// Writes `report` (as JSON or for people) and returns `code`, or 3 if it could not be written.
int write_report(const launch::CheckReport& report, bool json, int code)
{
    if (!write_stdout(json ? launch::format_json(report) : launch::format_text(report)))
    {
        std::fputs("error: cannot write the result to the standard output (is it closed, or the disk full?)\n", stderr);
        return cli::kExitSetupFailed;
    }
    return code;
}

/// A wrong command line: the message and the usage on stderr; with --json also the JSON (result
/// "usage") on stdout, so that a program that reads it gets an answer.
int usage_error(const std::string& message, bool json)
{
    std::fprintf(stderr, "error: %s\n%s", message.c_str(), kUsage);
    if (json)
    {
        launch::CheckReport report;
        report.usage = true;
        report.findings.push_back(launch::Finding{launch::Severity::Error, "usage", message, 0, "", ""});
        return write_report(report, true, cli::kExitUsage);
    }
    return cli::kExitUsage;
}

int check(const cli::Args& args)
{
    const auto& positionals = args.positionals();
    if (positionals.size() != 2)
    {
        throw cli::UsageError(positionals.size() < 2 ? "check needs the configuration file: vehicle_sil_launch check <file>"
                                                     : "check takes one file, not " + std::to_string(positionals.size() - 1));
    }
    launch::CheckOptions options;
    options.describe_timeout = args.millis("describe-timeout-ms", options.describe_timeout);
    if (options.describe_timeout.count() == 0)
    {
        throw cli::UsageError("--describe-timeout-ms must be at least 1");
    }
    options.strict = args.flag("strict");
    const std::string& file = positionals[1];
    const launch::CheckReport report = launch::check_file(process::path_from_utf8(file), file, options);
    const int code = !report.config ? cli::kExitSetupFailed : report.passed() ? cli::kExitOk : cli::kExitFailed;
    return write_report(report, args.flag("json"), code);
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = utf8_arguments(argc, argv);
    bool json = false;
    for (const auto& argument : arguments)
    {
        if (argument == "--")
        {
            break; // file names follow
        }
        if (argument == "--help" || argument == "-h")
        {
            return write_stdout(kUsage) ? cli::kExitOk : cli::kExitSetupFailed;
        }
        json = json || argument == "--json";
    }
    try
    {
        const cli::Args args(arguments, {"describe-timeout-ms"}, {"json", "strict"});
        if (args.positionals().empty())
        {
            return usage_error("no command: give check <file>", json);
        }
        if (args.positionals().front() != "check")
        {
            return usage_error("unknown command '" + args.positionals().front() + "': the command is check", json);
        }
        return check(args);
    }
    catch (const cli::UsageError& e)
    {
        return usage_error(e.what(), json);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (json)
        {
            launch::CheckReport report;
            report.findings.push_back(launch::Finding{launch::Severity::Error, "internal", e.what(), 0, "", ""});
            (void)write_report(report, true, cli::kExitSetupFailed);
        }
        return cli::kExitSetupFailed;
    }
}
