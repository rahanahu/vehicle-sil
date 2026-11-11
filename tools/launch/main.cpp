// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch: puts ECU executables made by different people into one simulation.
//   check <file>   compares the simulation configuration with what each ecu_main executable
//                  reports about itself (--describe) and lists every error and warning.
//   run <file>     checks the same way; if the check passes, starts the registry, the controller
//                  and every participant, watches the simulation to its end, ends every
//                  participant and reports how each ended.
// The file format, the rules and the output: docs/simulation-config.md.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
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
#else
#include <unistd.h>
#endif

namespace {

using namespace vehicle_sil;

constexpr const char* kUsage =
    "usage: vehicle_sil_launch check [--json] [--strict] [--describe-timeout-ms <ms>] [--disable <name>]...\n"
    "                                [--enable <name>]... [--] <file>\n"
    "       vehicle_sil_launch run [--json] [--strict] [--log-dir <dir>] [--timeout-ms <ms>] [--describe-timeout-ms <ms>]\n"
    "                              [--disable <name>]... [--enable <name>]... [--] <file>\n"
    "  check <file>                compare the simulation configuration <file> (YAML or JSON) with what each\n"
    "                              ecu_main executable in it reports (--describe); list the participants,\n"
    "                              the networks and every error and warning\n"
    "  run <file>                  check <file>; if it passes, run the simulation: the registry and the\n"
    "                              controller in this process, each participant as a process of its own\n"
    "                              (its output on stderr, each line after [<name>]); then end every\n"
    "                              participant and report how each ended. Ctrl+C stops it in order\n"
    "  --json                      write the result as JSON\n"
    "  --strict                    fail on warnings too\n"
    "  --log-dir <dir>             run: also write each participant's output to <dir>/<name>.log\n"
    "  --timeout-ms <ms>           run: abort the simulation if it has not ended <ms> after its start (wall clock)\n"
    "  --describe-timeout-ms <ms>  how long one executable may take for --describe (default 10000)\n"
    "  --disable <name>            leave the participant <name> out (as enabled: false in <file>); repeatable\n"
    "  --enable <name>             take the participant <name> in (as enabled: true in <file>); repeatable\n"
    "  --help, -h                  this text\n"
    "  --                          what follows is the file, also if it starts with '-'\n"
    "exit codes: 0 passed, 1 errors (with --strict: or warnings; run: also a failed simulation), 3 the file\n"
    "            cannot be read (run: or the registry cannot be set up), 4 usage, 130 run was interrupted\n"
    "the file format, the rules and the run: docs/simulation-config.md\n";

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

#ifdef _WIN32
/// Writes UTF-8 `text` to the console `handle` as UTF-16 (whatever its code page). false if
/// `handle` is not a console.
bool write_console(HANDLE handle, std::string_view text)
{
    DWORD mode = 0;
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode))
    {
        return false;
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size > 0)
    {
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        DWORD written = 0;
        WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
    }
    return true;
}
#endif

/// Writes `text` with "\n" line ends on every system (also on Windows, for the report for people
/// as for the JSON: the same bytes everywhere; a Windows console shows it as UTF-8). false if it
/// could not be written (a closed standard output, a full disk).
bool write_stdout(const std::string& text)
{
#ifdef _WIN32
    std::fflush(stdout);
    if (write_console(GetStdHandle(STD_OUTPUT_HANDLE), text))
    {
        return true;
    }
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const bool written = std::fwrite(text.data(), 1, text.size(), stdout) == text.size();
    return std::fflush(stdout) == 0 && written;
}

/// Writes one line of UTF-8 text to stderr: on a Windows console as UTF-16 (whatever its code
/// page), elsewhere as the bytes with "\n".
void write_stderr_line(std::string_view line)
{
#ifdef _WIN32
    if (write_console(GetStdHandle(STD_ERROR_HANDLE), std::string(line) + "\n"))
    {
        return;
    }
    std::fflush(stderr);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// --- interrupts (run) ---

std::atomic<int> g_interrupts{0};
std::atomic<int> g_first_interrupt{0};

#ifdef _WIN32
BOOL WINAPI on_console_event(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT)
    {
        return FALSE;
    }
    int none = 0;
    g_first_interrupt.compare_exchange_strong(none, static_cast<int>(event) + 1);
    g_interrupts.fetch_add(event == CTRL_CLOSE_EVENT ? 2 : 1);
    if (event == CTRL_CLOSE_EVENT)
    {
        // Windows ends the process when this returns: give the run time to end the participants
        // (the job objects end them anyway when the process is gone).
        Sleep(4000);
    }
    return TRUE;
}

std::string interrupt_name()
{
    switch (g_first_interrupt.load() - 1)
    {
    case CTRL_C_EVENT: return "Ctrl+C";
    case CTRL_BREAK_EVENT: return "Ctrl+Break";
    case CTRL_CLOSE_EVENT: return "console closed";
    default: return "interrupt";
    }
}

void watch_interrupts() { SetConsoleCtrlHandler(on_console_event, TRUE); }
#else
static_assert(std::atomic<int>::is_always_lock_free, "the signal handler needs a lock-free counter");

extern "C" void on_signal(int signal)
{
    int none = 0;
    g_first_interrupt.compare_exchange_strong(none, signal);
    g_interrupts.fetch_add(1);
}

std::string interrupt_name() { return process::signal_name(g_first_interrupt.load()); }

void watch_interrupts()
{
    struct sigaction action{};
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART; // a write of the result is not cut short by a second Ctrl+C
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP})
    {
        // A signal ignored by whoever started the launcher (nohup: SIGHUP) stays ignored.
        struct sigaction inherited{};
        if (sigaction(sig, nullptr, &inherited) == 0 && inherited.sa_handler == SIG_IGN)
        {
            continue;
        }
        sigaction(sig, &action, nullptr);
    }
    // A reader of stderr that went away (a pipe into head) must not end the launcher before it
    // has ended the participants.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignore, nullptr);
}
#endif

// --- output ---

int write_or_fail(const std::string& text, int code)
{
    if (!write_stdout(text))
    {
        std::fputs("error: cannot write the result to the standard output (is it closed, or the disk full?)\n", stderr);
        return cli::kExitSetupFailed;
    }
    return code;
}

/// A wrong command line: each error and the usage on stderr; with --json also the JSON (result
/// "usage") on stdout, so that a program that reads it gets an answer. `file`: the configuration
/// file if the arguments name one, else "" (null in the JSON).
int usage_error(const std::vector<launch::Finding>& errors, bool json, bool run, const std::string& file)
{
    for (const auto& error : errors)
    {
        std::fprintf(stderr, "error: %s\n", error.message.c_str());
    }
    std::fputs(kUsage, stderr);
    if (json)
    {
        launch::CheckReport check;
        check.file = file;
        check.usage = true;
        check.findings = errors;
        if (run)
        {
            launch::RunReport report;
            report.check = std::move(check);
            report.usage = true;
            return write_or_fail(launch::format_run_json(report), cli::kExitUsage);
        }
        return write_or_fail(launch::format_json(check), cli::kExitUsage);
    }
    return cli::kExitUsage;
}

int usage_error(const std::string& message, bool json, bool run, const std::string& file)
{
    return usage_error({launch::Finding{launch::Severity::Error, "usage", message, 0, "", ""}}, json, run, file);
}

/// The arguments that are no option or an option's value (as cli::Args reads them), for a usage
/// error that comes before cli::Args has read them.
std::vector<std::string> positional_arguments(const std::vector<std::string>& arguments)
{
    std::vector<std::string> out;
    bool optionsEnded = false;
    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        const std::string& a = arguments[i];
        if (optionsEnded || a.empty() || a[0] != '-')
        {
            out.push_back(a);
        }
        else if (a == "--")
        {
            optionsEnded = true;
        }
        else if (a == "--describe-timeout-ms" || a == "--log-dir" || a == "--timeout-ms" || a == "--disable" ||
                 a == "--enable")
        {
            ++i;
        }
    }
    return out;
}

/// The file and the options both commands share.
struct Common
{
    std::string file;
    launch::CheckOptions options;
};

Common common(const cli::Args& args, const std::string& command)
{
    const auto& positionals = args.positionals();
    if (positionals.size() != 2)
    {
        throw cli::UsageError(positionals.size() < 2
                                  ? command + " needs the configuration file: vehicle_sil_launch " + command + " <file>"
                                  : command + " takes one file, not " + std::to_string(positionals.size() - 1));
    }
    Common c;
    c.options.describe_timeout = args.millis("describe-timeout-ms", c.options.describe_timeout);
    if (c.options.describe_timeout.count() == 0)
    {
        throw cli::UsageError("--describe-timeout-ms must be at least 1");
    }
    c.options.strict = args.flag("strict");
    c.options.overrides.disable = args.all("disable");
    c.options.overrides.enable = args.all("enable");
    c.file = positionals[1];
    return c;
}

int check(const cli::Args& args)
{
    if (args.has("log-dir") || args.has("timeout-ms"))
    {
        throw cli::UsageError(std::string(args.has("log-dir") ? "--log-dir" : "--timeout-ms") +
                              " is for run: check starts no participant");
    }
    const Common c = common(args, "check");
    const launch::CheckReport report = launch::check_file(process::path_from_utf8(c.file), c.file, c.options);
    if (report.usage)
    {
        return usage_error(report.findings, args.flag("json"), false, c.file);
    }
    const int code = !report.config ? cli::kExitSetupFailed : report.passed() ? cli::kExitOk : cli::kExitFailed;
    return write_or_fail(args.flag("json") ? launch::format_json(report) : launch::format_text(report), code);
}

int run(const cli::Args& args)
{
    const Common c = common(args, "run");
    if (args.has("log-dir") && args.str("log-dir", "").empty())
    {
        throw cli::UsageError("--log-dir needs a directory");
    }
    watch_interrupts();
    launch::RunOptions options;
    options.check = c.options;
    if (args.has("log-dir"))
    {
        options.log_dir = process::path_from_utf8(args.str("log-dir", ""));
    }
    if (args.has("timeout-ms"))
    {
        options.timeout = args.millis("timeout-ms", std::chrono::milliseconds(0));
        if (options.timeout->count() == 0)
        {
            throw cli::UsageError("--timeout-ms must be at least 1");
        }
    }
    options.write_line = write_stderr_line;
    options.interrupts = &g_interrupts;
    options.interrupt_name = interrupt_name;
    const launch::RunReport report = launch::run_file(process::path_from_utf8(c.file), c.file, options);
    if (report.check.usage)
    {
        return usage_error(report.check.findings, args.flag("json"), true, c.file);
    }
    return write_or_fail(args.flag("json") ? launch::format_run_json(report) : launch::format_run_text(report),
                         report.exit_code());
}

} // namespace

int main(int argc, char** argv)
{
#ifndef _WIN32
    // SIGCHLD ignored by whoever started the launcher (SIG_IGN is inherited) would make the system
    // reap the participants itself, and their exit codes would be lost.
    std::signal(SIGCHLD, SIG_DFL);
#endif
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
    // The command and the file, for a usage error's JSON.
    const std::vector<std::string> positionals = positional_arguments(arguments);
    const bool isRun = !positionals.empty() && positionals.front() == "run";
    const std::string file = positionals.size() == 2 ? positionals[1] : std::string{};
    try
    {
        const cli::Args args(arguments, {"describe-timeout-ms", "log-dir", "timeout-ms"}, {"json", "strict"},
                             {"disable", "enable"});
        if (args.positionals().empty())
        {
            return usage_error("no command: give check <file> or run <file>", json, false, file);
        }
        const std::string& command = args.positionals().front();
        if (command != "check" && !isRun)
        {
            return usage_error("unknown command '" + command + "': the commands are check and run", json, false, file);
        }
        return isRun ? run(args) : check(args);
    }
    catch (const cli::UsageError& e)
    {
        return usage_error(e.what(), json, isRun, file);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (json)
        {
            (void)write_or_fail(launch::format_internal_error_json(isRun, e.what()), cli::kExitSetupFailed);
        }
        return cli::kExitSetupFailed;
    }
}
