// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch: puts ECU executables made by different people into one simulation.
//   check <file>   compares the simulation configuration with what each ecu_main executable
//                  reports about itself (--describe) and lists every error and warning.
//   run <file>     checks the same way; if the check passes, starts the registry, the controller
//                  and every participant, watches the simulation to its end, ends every
//                  participant and reports how each ended; with --events as events (one JSON
//                  object per line), with --stdin-commands stopped by commands on stdin.
//   version        its version (--json: also the formats it writes and what it can do).
// The file format, the rules, the output and the events: docs/simulation-config.md.
//
// The threads of run: main (the check, the run, the launcher's events, the result); one reader per
// participant (its lines); with --stdin-commands, the reader of stdin (the command events, stop
// requests); with --events, the writer of the events (the only thread that waits for their
// reader) and their watch (a reader that does not read; says on stderr that the stream closed).
// They meet in the event stream (a queue: no thread that makes an event waits; events.hpp says
// what holds) and in the stop requests (atomics, as the signal handler). Only main is ever
// waited for: the others are detached and may still wait when the process ends.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace {

using namespace vehicle_sil;

constexpr const char* kUsage =
    "usage: vehicle_sil_launch check [--json] [--strict] [--describe-timeout-ms <ms>] [--disable <name>]...\n"
    "                                [--enable <name>]... [--] <file>\n"
    "       vehicle_sil_launch run [--json | --events [--events-stall-ms <ms>]] [--stdin-commands] [--strict]\n"
    "                              [--log-dir <dir>] [--timeout-ms <ms>] [--describe-timeout-ms <ms>]\n"
    "                              [--disable <name>]... [--enable <name>]... [--] <file>\n"
    "       vehicle_sil_launch version [--json]\n"
    "  check <file>                compare the simulation configuration <file> (YAML or JSON) with what each\n"
    "                              ecu_main executable in it reports (--describe); list the participants,\n"
    "                              the networks and every error and warning\n"
    "  run <file>                  check <file>; if it passes, run the simulation: the registry and the\n"
    "                              controller in this process, each participant as a process of its own\n"
    "                              (its output on stderr, each line after [<name>]); then end every\n"
    "                              participant and report how each ended. Ctrl+C stops it in order\n"
    "  version                     the version of vehicle_sil_launch (--json: also its formats and features)\n"
    "  --json                      write the result as JSON\n"
    "  --events                    run: write what happens as events on stdout, one JSON object per line, the\n"
    "                              result last (nothing on stderr but the launcher's own failures)\n"
    "  --events-stall-ms <ms>      run --events: give up a reader of the events that takes nothing for <ms>, and wait\n"
    "                              at most <ms> for it to take the result (1000 or more; default 30000)\n"
    "  --stdin-commands            run: read commands from stdin, one JSON object per line: {\"command\":\"stop\"}\n"
    "                              stops in order (twice: without waiting); the end of stdin stops as well\n"
    "  --strict                    fail on warnings too\n"
    "  --log-dir <dir>             run: also write each participant's output to <dir>/<name>.log\n"
    "  --timeout-ms <ms>           run: abort the simulation if it has not ended <ms> after its start (wall clock)\n"
    "  --describe-timeout-ms <ms>  how long one executable may take for --describe (default 10000)\n"
    "  --disable <name>            leave the participant <name> out (as enabled: false in <file>); repeatable\n"
    "  --enable <name>             take the participant <name> in (as enabled: true in <file>); repeatable\n"
    "  --help, -h                  this text\n"
    "  --version                   as version\n"
    "  --                          what follows is the file, also if it starts with '-'\n"
    "exit codes: 0 passed, 1 errors (with --strict: or warnings; run: also a failed simulation), 3 the file\n"
    "            cannot be read (run: or the registry cannot be set up), 4 usage, 130 run was interrupted (or stopped)\n"
    "the file format, the rules, the run and the events: docs/simulation-config.md\n";

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


/// What the threads of run share that must outlive main() (the reader of standard input and the
/// threads of the events are never joined: they may still wait when the process ends). Made
/// once, never destroyed.
struct Shared
{
    /// One line at a time on stderr. A thread other than main may hold it for as long as stderr
    /// takes (a reader that reads neither the events nor stderr): main never waits for it
    /// without a limit.
    std::timed_mutex stderr_mutex;
    /// The result has been written: the other threads write nothing more (the events stop by
    /// themselves after their result).
    std::atomic<bool> finished{false};
    launch::EventStream* events = nullptr;
};

Shared& shared()
{
    static Shared* const instance = new Shared;
    return *instance;
}

/// Writes one line of UTF-8 text to stderr: on a Windows console as UTF-16 (whatever its code
/// page), elsewhere as the bytes with "\n". The caller holds stderr_mutex.
void write_stderr_line_locked(std::string_view line)
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

/// How long main waits for stderr when another thread is writing to it.
constexpr std::chrono::seconds kStderrWait{2};

/// One line on stderr from a thread other than main, without the C library's stream: a write
/// that waits (stderr is not read: a full pipe) then holds no lock of the stream, for which
/// exit() on the main thread could wait when it puts the streams in order. The caller holds
/// stderr_mutex.
void write_stderr_line_directly(std::string_view line)
{
    std::string data(line);
    data += '\n';
#ifdef _WIN32
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (write_console(err, data) || err == nullptr || err == INVALID_HANDLE_VALUE)
    {
        return;
    }
    for (std::size_t done = 0; done < data.size();)
    {
        DWORD written = 0;
        if (!WriteFile(err, data.data() + done, static_cast<DWORD>(data.size() - done), &written, nullptr))
        {
            return;
        }
        if (written == 0)
        {
            Sleep(20);
        }
        done += written;
    }
#else
    for (std::size_t done = 0; done < data.size();)
    {
        const ssize_t written = ::write(STDERR_FILENO, data.data() + done, data.size() - done);
        if (written >= 0)
        {
            done += static_cast<std::size_t>(written);
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            pollfd wait{STDERR_FILENO, POLLOUT, 0};
            static_cast<void>(::poll(&wait, 1, 100));
        }
        else if (errno != EINTR)
        {
            return;
        }
    }
#endif
}

/// write_stderr_line_locked(), one line at a time. From main: the line is left out if another
/// thread has been in a write to stderr for kStderrWait (stderr is not read).
void write_stderr_line(std::string_view line)
{
    const std::unique_lock lock(shared().stderr_mutex, kStderrWait);
    if (lock.owns_lock())
    {
        write_stderr_line_locked(line);
    }
}

/// A line on stderr from a thread other than main(): nothing once the result has been written
/// (main() may be ending the process). May wait for as long as stderr takes; main waits neither
/// for stderr_mutex without a limit nor, when the process ends, for anything this holds.
void note_from_thread(std::string_view line)
{
    const std::lock_guard lock(shared().stderr_mutex);
    if (!shared().finished.load())
    {
        write_stderr_line_directly(line);
    }
}

/// The other threads write nothing more (the result has been written). Waits for a line that is
/// being written, but not without a limit.
void finish_threads()
{
    shared().finished.store(true);
    const std::unique_lock lock(shared().stderr_mutex, kStderrWait);
}

/// A line on stderr from main() when stderr may not be read any more (the reader of the events
/// has stopped reading, and stderr may go to the same place): written by a thread of its own, for
/// which main waits kStderrWait at most.
void write_stderr_line_without_waiting(std::string line)
{
    struct Done
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
    };
    const auto done = std::make_shared<Done>();
    std::thread([done, line = std::move(line)] {
        {
            const std::lock_guard lock(shared().stderr_mutex);
            write_stderr_line_directly(line);
        }
        const std::lock_guard lock(done->mutex);
        done->done = true;
        done->changed.notify_all();
    }).detach();
    std::unique_lock lock(done->mutex);
    done->changed.wait_for(lock, kStderrWait, [&done] { return done->done; });
}

// --- interrupts and stop requests (run) ---

std::atomic<int> g_interrupts{0};
/// What asked first: a signal (POSIX), a console event + 1 (Windows), or one of these.
std::atomic<int> g_first_interrupt{0};
constexpr int kStopCommand = -1;  // {"command":"stop"} on standard input
constexpr int kEndOfInput = -2;   // standard input ended (run --stdin-commands)
constexpr int kOutputClosed = -3; // the reader of the events has gone (run --events)

std::string stop_source_name(int source)
{
    switch (source)
    {
    case kStopCommand: return "stop command";
    case kEndOfInput: return "end of input";
    case kOutputClosed: return "output closed";
    default: return {};
    }
}

/// A stop request as one Ctrl+C: the first stops the run in order, the second skips the waits.
void request_stop(int source)
{
    int none = 0;
    g_first_interrupt.compare_exchange_strong(none, source);
    g_interrupts.fetch_add(1);
}

/// A stop request only if nothing has asked for a stop yet (the end of input, a closed output:
/// never a second request). `before` runs between the decision and the request (it writes the
/// event that says why, before the run reacts). false if a stop had been requested.
template <class Before>
bool request_first_stop(int source, Before before)
{
    int none = 0;
    if (!g_first_interrupt.compare_exchange_strong(none, source))
    {
        return false;
    }
    before();
    // A signal in between counts as the first request, not as a second one.
    int zero = 0;
    g_interrupts.compare_exchange_strong(zero, 1);
    return true;
}

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
    const int first = g_first_interrupt.load();
    if (first < 0)
    {
        return stop_source_name(first);
    }
    switch (first - 1)
    {
    case CTRL_C_EVENT: return "Ctrl+C";
    case CTRL_BREAK_EVENT: return "Ctrl+Break";
    case CTRL_CLOSE_EVENT: return "console closed";
    default: return "interrupt";
    }
}

void watch_interrupts() { SetConsoleCtrlHandler(on_console_event, TRUE); }

/// A broken pipe is an error of the write, not a signal, on Windows.
void ignore_broken_pipes() {}
#else
static_assert(std::atomic<int>::is_always_lock_free, "the signal handler needs a lock-free counter");

extern "C" void on_signal(int signal)
{
    int none = 0;
    g_first_interrupt.compare_exchange_strong(none, signal);
    g_interrupts.fetch_add(1);
}

std::string interrupt_name()
{
    const int first = g_first_interrupt.load();
    return first < 0 ? stop_source_name(first) : process::signal_name(first);
}

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
}

/// A reader of stdout or stderr that went away (a pipe into head, a GUI that died) must not end
/// the launcher before it has ended the participants: the write fails instead.
void ignore_broken_pipes()
{
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, nullptr);
}
#endif

// --- the events (run --events) ---

#ifdef _WIN32
HANDLE g_events_handle = INVALID_HANDLE_VALUE;
#else
int g_events_fd = -1;
#endif

/// Takes the standard output for the events alone: the events go to a copy of it, and the
/// process's standard output becomes its standard error, so that nothing else (a library that
/// prints) can write into the lines of the events. false if there is no standard output.
bool take_stdout_for_events()
{
#ifdef _WIN32
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &g_events_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        g_events_handle = INVALID_HANDLE_VALUE;
        return false;
    }
    std::fflush(stdout);
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    if (_fileno(stdout) >= 0 && _fileno(stderr) >= 0)
    {
        _dup2(_fileno(stderr), _fileno(stdout));
    }
    return true;
#else
    std::fflush(stdout);
    g_events_fd = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3); // -1 if stdout is closed
    if (g_events_fd < 0)
    {
        return false;
    }
    if (::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
    {
        const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null >= 0)
        {
            ::dup2(null, STDOUT_FILENO);
            ::close(null);
        }
    }
    return true;
#endif
}

/// Writes the beginning of `bytes` to the events' output, for the writer thread of the events
/// (launch::EventStream::Write). It blocks while the reader does not read (the stream's watch
/// sees that); an output that does not block and is full is not a closed one: it waits a moment
/// and writes nothing.
launch::EventWrite write_events(std::string_view bytes)
{
    launch::EventWrite out;
#ifdef _WIN32
    DWORD mode = 0;
    if (GetConsoleMode(g_events_handle, &mode))
    {
        // All of it: a console takes UTF-16, and the pieces end between characters.
        static_cast<void>(write_console(g_events_handle, bytes));
        out.bytes = bytes.size();
        return out;
    }
    DWORD written = 0;
    if (!WriteFile(g_events_handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
    {
        const DWORD error = GetLastError();
        out.failed = true;
        out.error = error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
                        ? "the standard output is closed: has their reader gone?"
                        : error == ERROR_OPERATION_ABORTED ? "the write was stopped"
                                                           : "the standard output cannot be written: Windows error " + std::to_string(error);
        return out;
    }
    if (written == 0)
    {
        Sleep(20); // a pipe in no-wait mode that is full
    }
    out.bytes = written;
    return out;
#else
    const ssize_t written = ::write(g_events_fd, bytes.data(), bytes.size());
    if (written >= 0)
    {
        out.bytes = static_cast<std::size_t>(written);
        return out;
    }
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK)
    {
        // O_NONBLOCK is set on the output (by whoever shares it): until there is room.
        pollfd wait{g_events_fd, POLLOUT, 0};
        static_cast<void>(::poll(&wait, 1, 100));
        return out;
    }
    if (error == EINTR)
    {
        return out;
    }
    out.failed = true;
    out.error = error == EPIPE ? std::string("the standard output is closed: has their reader gone?")
                               : std::string("the standard output cannot be written: ") + std::strerror(error);
    return out;
#endif
}

/// The stream of the events has closed before the result (launch::EventStream::OnClosed, on the
/// stream's watch thread): a stop request (unless one came before), and the one line on stderr
/// that says why.
void on_events_closed(const std::string& reason)
{
    const bool stops = request_first_stop(kOutputClosed, [] {});
#ifdef _WIN32
    // A write that waits for the reader ends (so that nothing of the process waits in it).
    CancelIoEx(g_events_handle, nullptr);
#endif
    note_from_thread("vehicle_sil_launch: cannot write the events (" + reason + "): " +
                     (stops ? "stopping the run" : "no more events are written"));
}

/// --events-stall-ms: at least this (a reader may stop for a moment), and what fits the clocks.
constexpr std::int64_t kMinEventsStallMs = 1000;
constexpr std::int64_t kMaxEventsStallMs = 86'400'000;

/// The value of --events-stall-ms in `arguments` if it is one the option takes; else the default
/// (the events start before the command line is read as a whole, which then says what is wrong).
std::chrono::milliseconds events_stall_timeout(const std::vector<std::string>& arguments)
{
    for (std::size_t i = 0; i + 1 < arguments.size() && arguments[i] != "--"; ++i)
    {
        if (arguments[i] == "--events-stall-ms")
        {
            const std::string& text = arguments[i + 1];
            char* end = nullptr;
            errno = 0;
            const long long ms = std::strtoll(text.c_str(), &end, 10);
            if (!text.empty() && *end == '\0' && errno == 0 && ms >= kMinEventsStallMs && ms <= kMaxEventsStallMs)
            {
                return std::chrono::milliseconds(ms);
            }
            break;
        }
    }
    return launch::EventStreamOptions{}.stall_timeout;
}

/// Starts the events: hello with the arguments as given. Null if there is no standard output.
launch::EventStream* start_events(const std::string& file, const std::vector<std::string>& arguments)
{
    if (!take_stdout_for_events())
    {
        return nullptr;
    }
    launch::EventStreamOptions options;
    options.stall_timeout = events_stall_timeout(arguments);
    shared().events = new launch::EventStream(write_events, on_events_closed, options);
#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    shared().events->hello(pid, file, arguments);
    return shared().events;
}

// --- the commands on standard input (run --stdin-commands) ---

/// Reads standard input to its end on a thread of its own: each command as an event (or, without
/// --events, a line on stderr for one that was not accepted); a stop as one Ctrl+C. Its end is a
/// stop unless one was requested before.
void start_reading_commands()
{
    std::thread([] {
        launch::EventStream* events = shared().events;
        launch::CommandInput input([events](const launch::Command& command) {
            // The event first: it comes before what the stop does (it does not wait for its reader).
            if (events != nullptr)
            {
                events->command(command, "stdin");
            }
            else if (!command.accepted)
            {
                note_from_thread("vehicle_sil_launch: a line of the standard input was ignored: " + command.message);
            }
            if (command.is_stop())
            {
                request_stop(kStopCommand);
            }
        });
        char buffer[4096];
        while (true)
        {
#ifdef _WIN32
            const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            DWORD got = 0;
            SetLastError(ERROR_SUCCESS);
            if (in == nullptr || in == INVALID_HANDLE_VALUE || !ReadFile(in, buffer, sizeof(buffer), &got, nullptr))
            {
                break;
            }
            if (got == 0)
            {
                // A console's read that Ctrl+C or Ctrl+Break interrupted is no end of input (the
                // console event is the stop request).
                DWORD mode = 0;
                if (GetLastError() == ERROR_OPERATION_ABORTED && GetConsoleMode(in, &mode))
                {
                    Sleep(20);
                    continue;
                }
                break;
            }
#else
            const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (got < 0 && errno == EINTR)
            {
                continue;
            }
            if (got <= 0)
            {
                break;
            }
#endif
            input.append(std::string_view(buffer, static_cast<std::size_t>(got)));
        }
        input.finish();
        static_cast<void>(request_first_stop(kEndOfInput, [events] {
            if (events != nullptr)
            {
                launch::Command stop;
                stop.name = "stop";
                stop.accepted = true;
                events->command(stop, "eof");
            }
        }));
    }).detach();
}

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

/// The last event (run --events): `report` as the result. Waits for it to be written, but not
/// for a reader that does not read: until the stream's limit, or less after a second stop request
/// or one that comes while it waits (as the waits for the participants). The exit code of the run,
/// or 3 if the result could not be written (unless the run was stopped: then that is what
/// happened).
int finish_events(launch::EventStream& events, const launch::RunReport& report, int code)
{
    const int requests = g_interrupts.load();
    const bool written = events.result(report, [requests] { return g_interrupts.load() > std::min(requests, 1); });
    finish_threads();
    if (!written && report.result() != launch::RunResult::Interrupted)
    {
        // stderr may go where the events go: not read either.
        write_stderr_line_without_waiting("error: cannot write the result to the standard output (" + events.close_reason() + ")");
        return cli::kExitSetupFailed;
    }
    return code;
}

/// A wrong command line: each error and the usage on stderr; with --json also the JSON (result
/// "usage") on stdout, with --events the result event, so that a program that reads it gets an
/// answer. `file`: the configuration file if the arguments name one, else "" (null in the JSON).
int usage_error(const std::vector<launch::Finding>& errors, bool json, bool run, const std::string& file,
                launch::EventStream* events)
{
    for (const auto& error : errors)
    {
        write_stderr_line("error: " + error.message);
    }
    {
        const std::unique_lock lock(shared().stderr_mutex, kStderrWait);
        if (lock.owns_lock())
        {
            std::fputs(kUsage, stderr);
            std::fflush(stderr);
        }
    }
    if (json || events != nullptr)
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
            return events != nullptr ? finish_events(*events, report, cli::kExitUsage)
                                     : write_or_fail(launch::format_run_json(report), cli::kExitUsage);
        }
        return write_or_fail(launch::format_json(check), cli::kExitUsage);
    }
    return cli::kExitUsage;
}

int usage_error(const std::string& message, bool json, bool run, const std::string& file, launch::EventStream* events)
{
    return usage_error({launch::Finding{launch::Severity::Error, "usage", message, 0, "", ""}}, json, run, file, events);
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
        else if (a == "--describe-timeout-ms" || a == "--log-dir" || a == "--timeout-ms" || a == "--events-stall-ms" ||
                 a == "--disable" || a == "--enable")
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
    for (const char* runOnly : {"log-dir", "timeout-ms", "events-stall-ms", "events", "stdin-commands"})
    {
        if (args.has(runOnly) || args.flag(runOnly))
        {
            throw cli::UsageError("--" + std::string(runOnly) + " is for run: check starts no participant");
        }
    }
    const Common c = common(args, "check");
    const launch::CheckReport report = launch::check_file(process::path_from_utf8(c.file), c.file, c.options);
    if (report.usage)
    {
        return usage_error(report.findings, args.flag("json"), false, c.file, nullptr);
    }
    const int code = !report.config ? cli::kExitSetupFailed : report.passed() ? cli::kExitOk : cli::kExitFailed;
    return write_or_fail(args.flag("json") ? launch::format_json(report) : launch::format_text(report), code);
}

int run(const cli::Args& args, launch::EventStream* events)
{
    const Common c = common(args, "run");
    if (args.has("log-dir") && args.str("log-dir", "").empty())
    {
        throw cli::UsageError("--log-dir needs a directory");
    }
    if (args.flag("json") && args.flag("events"))
    {
        throw cli::UsageError("--json and --events cannot be used together: with --events, the last event is the result");
    }
    if (args.has("events-stall-ms"))
    {
        if (!args.flag("events"))
        {
            throw cli::UsageError("--events-stall-ms is for --events: without it no events are written");
        }
        const auto ms = static_cast<std::int64_t>(args.millis("events-stall-ms", std::chrono::milliseconds(0)).count());
        if (ms < kMinEventsStallMs || ms > kMaxEventsStallMs)
        {
            throw cli::UsageError("--events-stall-ms must be from " + std::to_string(kMinEventsStallMs) + " to " +
                                  std::to_string(kMaxEventsStallMs));
        }
    }
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
    watch_interrupts();
    if (events != nullptr)
    {
        options.events = events;
    }
    else
    {
        options.write_line = write_stderr_line;
    }
    if (args.flag("stdin-commands"))
    {
        start_reading_commands();
    }
    options.interrupts = &g_interrupts;
    options.interrupt_name = interrupt_name;
    const launch::RunReport report = launch::run_file(process::path_from_utf8(c.file), c.file, options);
    if (report.check.usage)
    {
        return usage_error(report.check.findings, args.flag("json"), true, c.file, events);
    }
    if (events != nullptr)
    {
        return finish_events(*events, report, report.exit_code());
    }
    return write_or_fail(args.flag("json") ? launch::format_run_json(report) : launch::format_run_text(report),
                         report.exit_code());
}

/// `version [--json]`: nothing else on its command line.
int version(const std::vector<std::string>& arguments)
{
    bool json = false;
    bool command = false;
    for (const auto& argument : arguments)
    {
        if (argument == "version" && !command)
        {
            command = true;
            continue;
        }
        if (argument == "--json" && !json)
        {
            json = true;
            continue;
        }
        write_stderr_line("error: version takes only --json, not " + argument);
        std::fputs(kUsage, stderr);
        return cli::kExitUsage;
    }
    return write_stdout(json ? launch::format_version_json() : launch::format_version_text()) ? cli::kExitOk
                                                                                              : cli::kExitSetupFailed;
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
    bool events = false;
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
        if (argument == "--version")
        {
            return write_stdout(launch::format_version_text()) ? cli::kExitOk : cli::kExitSetupFailed;
        }
        json = json || argument == "--json";
        events = events || argument == "--events";
    }
    // The command and the file, for a usage error's JSON.
    const std::vector<std::string> positionals = positional_arguments(arguments);
    const bool isRun = !positionals.empty() && positionals.front() == "run";
    const std::string file = positionals.size() == 2 ? positionals[1] : std::string{};
    if (!positionals.empty() && positionals.front() == "version")
    {
        return version(arguments);
    }
    launch::EventStream* stream = nullptr;
    if (isRun)
    {
        ignore_broken_pipes();
        if (events)
        {
            // hello first, whatever is wrong with the rest of the command line.
            stream = start_events(file, arguments);
            if (stream == nullptr)
            {
                return usage_error("--events needs a standard output to write the events to, and there is none (it is "
                                   "closed): start vehicle_sil_launch with its standard output as a pipe or a file",
                                   false, true, file, nullptr);
            }
        }
    }
    int code = cli::kExitSetupFailed;
    try
    {
        const cli::Args args(arguments, {"describe-timeout-ms", "log-dir", "timeout-ms", "events-stall-ms"},
                             {"json", "strict", "events", "stdin-commands"}, {"disable", "enable"});
        if (args.positionals().empty())
        {
            code = usage_error("no command: give check <file>, run <file> or version", json, false, file, stream);
        }
        else if (args.positionals().front() != "check" && !isRun)
        {
            code = usage_error("unknown command '" + args.positionals().front() +
                                   "': the commands are check, run and version",
                               json, false, file, stream);
        }
        else
        {
            code = isRun ? run(args, stream) : check(args);
        }
    }
    catch (const cli::UsageError& e)
    {
        code = usage_error(e.what(), json, isRun, file, stream);
    }
    catch (const std::exception& e)
    {
        write_stderr_line(std::string("error: ") + e.what());
        if (stream != nullptr)
        {
            launch::RunReport report;
            report.findings.push_back(launch::Finding{launch::Severity::Error, "internal", e.what(), 0, "", ""});
            static_cast<void>(finish_events(*stream, report, cli::kExitSetupFailed));
        }
        else if (json)
        {
            static_cast<void>(write_or_fail(launch::format_internal_error_json(isRun, e.what()), cli::kExitSetupFailed));
        }
        code = cli::kExitSetupFailed;
    }
    // The reader of stdin and the threads of the events may still wait: they write nothing from here on.
    finish_threads();
    return code;
}
