// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch: puts ECU executables made by different people into one simulation.
//   check <file>   compares the simulation configuration with what each ecu_main executable
//                  reports about itself (--describe) and lists every error and warning.
//   run <file>     checks the same way; if the check passes, starts the registry, the controller
//                  and every participant, watches the simulation to its end, ends every
//                  participant and reports how each ended; with --events as events (one JSON
//                  object per line), with --stdin-commands stopped by commands on stdin.
//   test <file>    runs every scenario of a list with the file, once for each of its variants,
//                  one run after the other, and reports all of them as one table.
//   connections <file>  where each network of the file can be connected on this computer: the
//                  lists of the networks' bridges.
//   version        its version (--json: also the formats it writes and what it can do).
// The file format: docs/simulation-config.md; the rules and the output of check: docs/simulation-check.md;
// the run: docs/simulation-run.md; the events: docs/simulation-events.md.
//
// The threads of run: main (the check, the run, the launcher's events, the result); one reader per
// participant (its lines); with --stdin-commands, the reader of stdin (the command events, stop
// requests, the time commands: it hands each to the controller's time gate itself, through
// runner::TimeCommands, waiting only for that object's and the gate's mutexes); with --events, the
// writer of the events (the only thread that waits for their reader) and their watch (a reader
// that does not read; says on stderr that the stream closed). SIL Kit's threads run the
// controller's steps (the gate holds them). They meet in the event stream (a queue: no thread
// that makes an event waits; events.hpp says what holds), in TimeCommands and in the stop
// requests (atomics, as the signal handler). Only main is ever waited for: the others are
// detached and may still wait when the process ends. With --control (a build with the control
// server), gRPC's threads serve the clients: their time operations go through the same
// TimeCommands (source "control_server"), a Stop is a stop request like {"command":"stop"}, and
// their writes and reads go to the run's hub; the run (launch::run_file()) makes the server from
// the request and ends it after the simulation, before main writes the result; main lets go of
// what is left of it after the result.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/cli/utf8.hpp"
#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/command_input.hpp"
#include "vehicle_sil/launch/connections.hpp"
#include "vehicle_sil/launch/error_result.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/runner/stop_requests.hpp"
#include "vehicle_sil/launch/test.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/trace/trace.hpp"

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
#include <optional>
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
    "usage: vehicle_sil_launch check [--json] [--strict] [--variant <name>] [--scenario <file> [--param <name>=<value>]...]\n"
    "                                [--describe-timeout-ms <ms>] [--connect <network>=<target>]...\n"
    "                                [--time-policy realtime|free] [--request <file>] [--speed <speed>]\n"
    "                                [--stop-at-ms <ms>] [--late-tolerance-ms <ms>]\n"
    "                                [--text-from-stdin] [--descriptions <file>] [--write-descriptions <file>]\n"
    "                                [--compare-commands <file>]\n"
    "                                [--disable <name>]... [--enable <name>]... [--manual <name>]... [--] <file>\n"
    "       vehicle_sil_launch run [--json | --events [--events-stall-ms <ms>]] [--stdin-commands] [--start-paused]\n"
    "                              [--control <address> [--control-credentials <file>] [--control-tls-cert <file>\n"
    "                              --control-tls-key <file>] [--control-audit-log <file>]]\n"
    "                              [--variant <name>] [--scenario <file> [--param <name>=<value>]... [--timeline <file>]]\n"
    "                              [--speed <speed>] [--strict] [--log-dir <dir>]\n"
    "                              [--timeout-ms <ms>] [--stop-at-ms <ms>] [--late-tolerance-ms <ms>]\n"
    "                              [--describe-timeout-ms <ms>] [--connect <network>=<target>]...\n"
    "                              [--time-policy realtime|free] [--request <file>]\n"
    "                              [--disable <name>]... [--enable <name>]... [--manual <name>]... [--] <file>\n"
    "       vehicle_sil_launch test [--json] [--strict] [--fail-fast] [--variant <name>]... [--param <name>=<value>]...\n"
    "                               [--timeout-ms <ms>] [--describe-timeout-ms <ms>] [--time-policy realtime|free]\n"
    "                               [--stop-at-ms <ms>] [--late-tolerance-ms <ms>] [--request <file>]\n"
    "                               [--disable <name>]... [--enable <name>]... <file> --scenarios <directory or files>...\n"
    "       vehicle_sil_launch connections [--json] [--network <name>] [--request <file>] [--] <file>\n"
    "       vehicle_sil_launch version [--json]\n"
    "  check <file>                compare the simulation configuration <file> (YAML or JSON) with what each\n"
    "                              ecu_main executable in it reports (--describe); list the participants,\n"
    "                              the networks and every error and warning\n"
    "  run <file>                  check <file>; if it passes, run the simulation: the registry and the\n"
    "                              controller in this process, each participant as a process of its own\n"
    "                              (its output on stderr, each line after [<name>]); then end every\n"
    "                              participant and report how each ended. Ctrl+C stops it in order\n"
    "  test <file>                 run every scenario of --scenarios with <file>, once for each of its variants, one\n"
    "                              run after the other (each as run --speed max --scenario: its own registry and\n"
    "                              processes); then a table of all runs: passed, FAILED or skipped, and why. A\n"
    "                              failed run does not end the test. Ctrl+C stops the current run in order and\n"
    "                              leaves the rest out\n"
    "  connections <file>          where each network of <file> can be connected on this computer: asks the bridge\n"
    "                              of each can and ethernet network for its list and writes the targets as\n"
    "                              --connect lines to copy (connects to nothing). With --request <request file>: only\n"
    "                              the networks that the request file connects, and whether each target is there\n"
    "                              and usable here (exit code 1 if one is not)\n"
    "  version                     the version of vehicle_sil_launch (--json: also its formats and features)\n"
    "  --json                      write the result as JSON\n"
    "  --events                    run: write what happens as events on stdout, one JSON object per line, the\n"
    "                              result last (nothing on stderr but the launcher's own failures)\n"
    "  --events-stall-ms <ms>      run --events: give up a reader of the events that takes nothing for <ms>, and wait\n"
    "                              at most <ms> for it to take the result (1000 or more; default 30000)\n"
    "  --stdin-commands            run: read commands from stdin, one JSON object per line: {\"command\":\"stop\"}\n"
    "                              stops in order (twice: without waiting); the end of stdin stops as well;\n"
    "                              pause, resume, step (with \"ms\"), run_until (with \"at_ms\") and speed (with\n"
    "                              \"value\") control the time\n"
    "  --start-paused              run --stdin-commands or --control: stand still at 0 until a resume or step\n"
    "                              command (with --control: before the first step, so that a client's writes\n"
    "                              take effect at 0)\n"
    "  --control <address>         run: serve the Control API (gRPC) for clients: the ECUs' variables and the\n"
    "                              time; <port> (on 127.0.0.1; 0: any free port), <host>:<port> or unix:<path>;\n"
    "                              the event control_server and a line say where (only in a build with\n"
    "                              VEHICLE_SIL_BUILD_CONTROL_SERVER=ON)\n"
    "  --control-credentials <file> run --control: tokens and roles, one per line: <role> <name> <token>\n"
    "                              (every call then needs a token; never put a token on the command line)\n"
    "  --control-tls-cert <file>   run --control: PEM certificate chain of the server (with --control-tls-key: TLS)\n"
    "  --control-tls-key <file>    run --control: PEM private key of the server\n"
    "  --control-audit-log <file>  run --control: append who changed what\n"
    "  --scenario <file>           check: also check the scenario <file> against the participants' variables;\n"
    "                              run: then run it: write the ECUs' variables and compare their values at\n"
    "                              virtual times, stop the simulation when the scenario is over, and fail if a\n"
    "                              step failed (docs/scenario.md; not with --start-paused; with --control the\n"
    "                              clients read, move the time and stop, and cannot write)\n"
    "  --variant <name>            the variant of <file> to check or run (its variants: the common part with the\n"
    "                              variant's commands, variable maps, params and enabled laid over it); a file\n"
    "                              that defines variants needs it: none is the default. test: only this variant\n"
    "                              (repeatable); without it, every variant\n"
    "  --param <name>=<value>      with --scenario: the value of the scenario's parameter ${<name>}, over the\n"
    "                              scenario's, the file's and the variant's params; repeatable. test: for each\n"
    "                              scenario that has the parameter (at least one must)\n"
    "  --scenarios <dir or files>  test: the scenarios: a directory (the files directly in it that end in .yaml,\n"
    "                              .yml or .json, in the order of their names) or files, in the order given;\n"
    "                              several after one --scenarios (so <file> comes first, or after --); repeatable\n"
    "  --timeline <file>           run --scenario: write what the scenario wrote and saw over the virtual time\n"
    "                              (its steps, the written and the sampled values) to <file> as JSON\n"
    "                              (docs/simulation-run.md; schema/timeline.schema.json)\n"
    "  --fail-fast                 test: do not run what comes after the first run that fails\n"
    "  --speed <speed>             run: simulation seconds per wall-clock second (1 real time, 2 twice as fast)\n"
    "                              or max (as fast as possible); replaces speed in <file>\n"
    "  --strict                    fail on warnings too\n"
    "  --log-dir <dir>             run: also write each participant's output to <dir>/<name>.log\n"
    "  --stop-at-ms <ms>           stop at this exact virtual time (default: no automatic stop)\n"
    "  --late-tolerance-ms <ms>    wall-clock pacing tolerance (default 10)\n"
    "  --participant-start-timeout-ms <ms>  wait for required participants (default 30000)\n"
    "  --participant-stop-timeout-ms <ms>   wait per participant shutdown stage (default 10000)\n"
    "  --timeout-ms <ms>           run: abort the simulation if it has not ended <ms> after its start (wall clock;\n"
    "                              with --start-paused and --control, from when it stands still before the start);\n"
    "                              test: each run\n"
    "  --describe-timeout-ms <ms>  how long one executable may take for --describe (default 10000)\n"
    "  --text-from-stdin           check: read the configuration's text from stdin, not from <file>; <file> still\n"
    "                              says where it is (its relative paths are from its directory) and names it in\n"
    "                              the findings, and need not exist\n"
    "  --write-descriptions <file> check: write each participant's --describe output that the check read to <file>,\n"
    "                              for --descriptions (only the same vehicle_sil_launch reads it)\n"
    "  --descriptions <file>       check: start no process: take each participant's --describe output from <file>\n"
    "                              (check --write-descriptions) if its command and executable are as they were;\n"
    "                              any other participant is not described. --connect: only its form and names\n"
    "  --compare-commands <file>   check: say in \"commands\" of --json (same, changed or not_compared) whether\n"
    "                              the commands that run (each participant's command, describe, manual, enabled\n"
    "                              and, for describe: false, what run adds to its arguments, in every variant;\n"
    "                              the participants, the variants, registry_uri) are as in <file>; it only\n"
    "                              reads <file>\n"
    "  --connect <network>=<target> connect the network <network> of <file> to a real bus at <target>: an\n"
    "                              Ethernet adapter (or tap:<device>), a SocketCAN interface (can0, vcan0 or\n"
    "                              socketcan:<name>), or virtual:<n> or physical:<n> for a Vector XL channel\n"
    "                              (connections <file> lists those of this computer, as lines to copy); none:\n"
    "                              SIL Kit only. check finds the network's bridge (next to vehicle_sil_launch or\n"
    "                              on PATH) and asks it --describe;\n"
    "                              run starts it before the other participants (as bridge.<network>) and fails if\n"
    "                              it ends. Repeatable, each network once\n"
    "  --time-policy realtime|free the time of a run that is connected outside the simulation (by --connect, or by\n"
    "                              an enabled participant with role: bridge). realtime (the default): it goes at\n"
    "                              speed 1; another --speed and --start-paused are the\n"
    "                              error outside-real-time, and the commands that pause or step it or change its\n"
    "                              speed are refused while it runs. free: the speed and the time are yours to\n"
    "                              change as in any run; the bridge still carries frames both ways, so what is\n"
    "                              connected there sees the simulation pause or run ahead. No effect on other\n"
    "                              runs\n"
    "  --request <file>            check, run, connections: what to run, from the request file <file> (JSON:\n"
    "                              variant, params, scenario, disable, enable, manual, connect, time_policy, speed,\n"
    "                              start_paused, strict, stop_at_ms, late_tolerance_ms; schema/run-request.schema.json). An argument wins over the\n"
    "                              file, field by field: --connect and --param by name, --connect <network>=none\n"
    "                              takes the file's target away; the lists add up. scenario in <file> is from the\n"
    "                              directory of the configuration file. Only this file is read: none is looked for.\n"
    "                              connections reads only its connect; test reads only stop_at_ms and late_tolerance_ms\n"
    "  --network <name>            connections: only the network <name>\n"
    "  --disable <name>            leave the participant <name> out (as enabled: false in <file>); repeatable\n"
    "  --enable <name>             take the participant <name> in (as enabled: true in <file>); repeatable\n"
    "  --manual <name>             you start the participant <name> yourself (from a debugger, say): run does not\n"
    "                              start it, prints the command to start it with and waits for it to join, with\n"
    "                              no time limit (as manual: true in <file>); repeatable\n"
    "  --help, -h                  this text\n"
    "  --version                   as version\n"
    "  --                          what follows is the file, also if it starts with '-'\n"
    "exit codes: 0 passed (run: also stopped by a command), 1 errors (with --strict: or warnings; run: also a failed\n"
    "            simulation; test: a run failed or was left out; connections: a list could not be made, or a\n"
    "            target of --request cannot be used), 3 the file cannot be read (run: or the registry cannot be set\n"
    "            up), 4 usage, 5 run: a scenario stopped before its end, 130 run or test was\n"
    "            interrupted (a signal, or its input or output closed)\n"
    "the file format: docs/simulation-config.md; check: docs/simulation-check.md; run: docs/simulation-run.md;\n"
    "events and commands: docs/simulation-events.md; scenarios: docs/scenario.md\n";

/// Writes `text` with "\n" line ends on every system (also on Windows, for the report for people
/// as for the JSON: the same bytes everywhere; a Windows console shows it as UTF-8). false if it
/// could not be written (a closed standard output, a full disk).
bool write_stdout(const std::string& text)
{
#ifdef _WIN32
    std::fflush(stdout);
    if (cli::write_console(GetStdHandle(STD_OUTPUT_HANDLE), text))
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
    /// The controls of the run (run), on g_stops: the reader of stdin hands the time commands to
    /// the run through them, a control server its time operations.
    runner::RunControls* controls = nullptr;
    /// Without --events: the lines about refused commands of standard input that are not written
    /// yet (write_stdin_notes()). They are decided under the time commands' mutex, where nothing
    /// may wait for stderr, so they are kept here and written outside it.
    std::mutex stdin_notes_mutex;
    std::vector<std::string> stdin_notes;
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
    if (cli::write_console(GetStdHandle(STD_ERROR_HANDLE), std::string(line) + "\n"))
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
    if (cli::write_console(err, data) || err == nullptr || err == INVALID_HANDLE_VALUE)
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

/// Ends the process with `code` now, when the result has been written: without the destructors of
/// static objects and without unloading SIL Kit, which a thread of the run still uses
/// (OutcomeClose::not_closed_ms). Nothing else is left to flush: the result and every line on
/// stderr are flushed where they are written, the events by their own thread before the result
/// counts as written; only the profiling trace, which waits for the process's end, is written here.
[[noreturn]] void end_process_at_once(int code)
{
    finish_threads();
    // The profiling trace, if one is taken (docs/internals/trace.md): atexit does not run. Silent:
    // nothing may read stderr any more.
    static_cast<void>(vehicle_sil::trace::flush(vehicle_sil::trace::Errors::Silent));
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
    std::_Exit(code);
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

/// The stop requests of the process (run: those of the run's controls; test:
/// launch::RunLent::stops). The source of a request: a
/// signal (POSIX), a console event + 1 (Windows), or one of runner::stop_source.
constinit runner::StopRequests g_stops;
using runner::stop_source::kEndOfInput;
using runner::stop_source::kOutputClosed;
using runner::stop_source::kStopCommand;
using runner::stop_source_name;

#ifdef _WIN32
BOOL WINAPI on_console_event(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT)
    {
        return FALSE;
    }
    // Closing the console is not followed by a second request: it skips the waits at once.
    g_stops.request(static_cast<int>(event) + 1, event == CTRL_CLOSE_EVENT ? 2 : 1);
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
    const int first = g_stops.first();
    if (first < 0)
    {
        return runner::stop_source_name(first);
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
/// Only runner::StopRequests::request(): lock-free atomics, nothing else (async-signal-safe).
extern "C" void on_signal(int signal) { g_stops.request(signal); }

std::string interrupt_name()
{
    const int first = g_stops.first();
    return first < 0 ? runner::stop_source_name(first) : process::signal_name(first);
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
        static_cast<void>(cli::write_console(g_events_handle, bytes));
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
    const bool stops = g_stops.request_first(kOutputClosed, [] {});
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

/// At most this many lines wait in Shared::stdin_notes (more are left out: nobody reads stderr).
constexpr std::size_t kMaxStdinNotes = 1024;

std::string stdin_ignored_line(const runner::Command& command)
{
    return "vehicle_sil_launch: a line of the standard input was ignored: " + command.message;
}

/// Without --events: a line of standard input that was not accepted, as a line on stderr. From
/// the thread that reads standard input, outside every mutex of the run.
void note_stdin_ignored(const runner::Command& command)
{
    note_from_thread(stdin_ignored_line(command));
}

/// note_stdin_ignored() for where nothing may wait (under the time commands' mutex): the line is
/// kept for write_stdin_notes().
void keep_stdin_note(const runner::Command& command)
{
    const std::lock_guard lock(shared().stdin_notes_mutex);
    if (shared().stdin_notes.size() < kMaxStdinNotes)
    {
        shared().stdin_notes.push_back(stdin_ignored_line(command));
    }
}

/// Writes the lines keep_stdin_note() kept, outside the time commands' mutex: `from_main` as
/// write_stderr_line() (main does not wait for stderr without a limit), else as
/// note_from_thread(). The lines of one call keep their order.
void write_stdin_notes(bool from_main)
{
    std::vector<std::string> lines;
    {
        const std::lock_guard lock(shared().stdin_notes_mutex);
        lines.swap(shared().stdin_notes);
    }
    for (const std::string& line : lines)
    {
        from_main ? write_stderr_line(line) : note_from_thread(line);
    }
}

/// Reads standard input to its end on a thread of its own: each command as an event (or, without
/// --events, a line on stderr for one that was not accepted: a time command's from the controls'
/// observer, which also has the answer of one that waited for the run to read its configuration);
/// a stop as one Ctrl+C. Its end is a stop unless one was requested before.
void start_reading_commands()
{
    // Once: a second reader would take lines of the same standard input.
    static std::atomic<bool> started{false};
    if (started.exchange(true))
    {
        return;
    }
    std::thread([] {
        launch::EventStream* events = shared().events;
        // Never null: the reader starts from the phase observer of these controls, which are set
        // before the run begins.
        runner::RunControls* controls = shared().controls;
        launch::CommandInput input([events, controls](const runner::Command& command) {
            // The time commands and a stop: the run's rules decide whether one is taken (a time
            // command now, or once the run has read its configuration; a stop at once), as for a
            // control server's, and its event is written before it is done (neither waits for the
            // run or the reader of the events). What it answers goes to the controls' observer. An
            // accepted stop is one Ctrl+C, asked under the time commands' mutex, so the state that
            // follows the command's event says the request.
            if (command.is_time() || command.is_stop())
            {
                static_cast<void>(command.is_stop()
                                      ? controls->time().apply(command, "stdin", [] { g_stops.request(kStopCommand); })
                                      : controls->time().apply(command, "stdin"));
                if (events == nullptr)
                {
                    write_stdin_notes(false);
                }
                return;
            }
            // A line that was not accepted as a command: its event, or a line on stderr.
            if (events != nullptr)
            {
                events->command(command, "stdin");
            }
            else
            {
                note_stdin_ignored(command);
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
        static_cast<void>(g_stops.request_first(kEndOfInput, [events] {
            if (events != nullptr)
            {
                runner::Command stop;
                stop.name = "stop";
                stop.accepted = true;
                events->command(stop, "eof");
            }
        }));
        if (controls != nullptr)
        {
            controls->publish();
        }
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
    const int requests = g_stops.count();
    const bool written = events.result(launch::format_result_event(report),
                                       [requests] { return g_stops.count() > std::min(requests, 1); });
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
/// `variants`: the variants the file defines, if it was read (the JSON lists what --variant takes).
int usage_error(const std::vector<config::Finding>& errors, bool json, launch::CommandKind command, const std::string& file,
                launch::EventStream* events, const std::vector<std::string>& variants = {})
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
    if (events != nullptr)
    {
        return finish_events(*events, launch::usage_run_report(file, errors, variants), cli::kExitUsage);
    }
    return json ? write_or_fail(launch::format_usage_json(command, file, errors, variants), cli::kExitUsage) : cli::kExitUsage;
}

int usage_error(const std::string& message, bool json, launch::CommandKind command, const std::string& file, launch::EventStream* events)
{
    return usage_error({config::Finding{config::Severity::Error, "usage", message, 0, "", ""}}, json, command, file, events);
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
                 a == "--stop-at-ms" || a == "--late-tolerance-ms" || a == "--participant-start-timeout-ms" ||
                 a == "--participant-stop-timeout-ms" ||
                 a == "--disable" || a == "--enable" || a == "--manual" || a == "--speed" || a == "--scenario" || a == "--control" ||
                 a == "--variant" || a == "--param" || a == "--scenarios" || a == "--timeline" || a == "--connect" ||
                 a == "--network" || a == "--time-policy" || a == "--request" || a == "--descriptions" ||
                 a == "--write-descriptions" || a == "--compare-commands" || a == "--control-credentials" ||
                 a == "--control-tls-cert" || a == "--control-tls-key" || a == "--control-audit-log")
        {
            ++i;
        }
    }
    return out;
}

/// What is wrong with the file of --request: every error of it, for usage_error().
struct RequestFileError
{
    std::vector<config::Finding> findings;
};

/// --request <file>: the request file, which the arguments are laid over. Without the option
/// there is none, and a request is the arguments alone.
struct RequestArgument
{
    std::optional<launch::RequestFile> file{};
    /// --time-policy is given: it wins over the file's, also as realtime.
    bool time_policy_given = false;

    /// `arguments` with what the file adds (launch::with_request_file()).
    [[nodiscard]] launch::RunRequest over(const launch::RunRequest& arguments) const
    {
        if (!file)
        {
            return arguments;
        }
        launch::RunRequest request = launch::with_request_file(arguments, *file, time_policy_given);
        if (!request.intent.connect.empty())
        {
            request.host.tool_directory = process::current_executable().parent_path();
        }
        return request;
    }
};

/// Reads the file of --request, if the option is given. Only this file: none is looked for.
/// Throws RequestFileError with what is wrong with it.
RequestArgument read_request_argument(const cli::Args& args)
{
    RequestArgument out;
    if (!args.has("request"))
    {
        return out;
    }
    const std::string name = args.str("request", "");
    if (name.empty())
    {
        throw cli::UsageError("--request needs a request file: --request <file>.json");
    }
    std::string text;
    if (const std::string problem = launch::read_request_file_text(process::path_from_utf8(name), text); !problem.empty())
    {
        config::Finding finding{config::Severity::Error, "usage", "--request " + name + ": " + problem, 0, "", ""};
        finding.file = name;
        throw RequestFileError{{std::move(finding)}};
    }
    launch::RequestFileRead read = launch::read_request_file(text, name);
    if (!read.file)
    {
        throw RequestFileError{std::move(read.findings)};
    }
    out.file = std::move(read.file);
    out.time_policy_given = args.has("time-policy");
    return out;
}

/// Refuses the first rule of the request that its fields so far break (launch::validate_request()).
/// Called where the arguments are read in the order the user's mistakes are reported in: a rule
/// whose fields are not read yet does not fire. With a request file (`asked`) nothing is judged
/// here: the file gives fields too, so the rules wait until every argument is read
/// (whole_request()).
void require_valid(const launch::RunRequest& request, launch::RequestCommand command, const RequestArgument& asked = {})
{
    if (asked.file)
    {
        return;
    }
    const std::vector<config::Finding> findings = launch::validate_request(request, command);
    if (!findings.empty())
    {
        throw cli::UsageError(findings.front().message);
    }
}

/// The request of the command once every argument is read: `arguments` over the request file, if
/// there is one, judged as a whole (launch::validate_request(); the first rule broken is refused).
launch::RunRequest whole_request(const launch::RunRequest& arguments, launch::RequestCommand command,
                                 const RequestArgument& asked)
{
    if (!asked.file)
    {
        return arguments; // judged as the arguments were read (require_valid())
    }
    launch::RunRequest request = asked.over(arguments);
    const std::vector<config::Finding> findings = launch::validate_request(request, command);
    if (!findings.empty())
    {
        throw cli::UsageError(findings.front().message);
    }
    return request;
}

void read_speed(const cli::Args& args, launch::RunIntent& intent)
{
    if (args.has("speed"))
    {
        std::optional<double> speed;
        if (!config::parse_speed(args.str("speed", ""), speed))
        {
            throw cli::UsageError("--speed must be a number of simulation seconds per wall-clock second from 0.000001 to "
                                  "1000000 (1: real time, 2: twice as fast) or max (as fast as possible), not '" +
                                  args.str("speed", "") + "'");
        }
        intent.speed.emplace(speed);
    }
}

/// What check, run and test share: the configuration file and the arguments that say what to run.
launch::RunRequest read_request(const cli::Args& args, launch::RequestCommand command, const RequestArgument& asked = {})
{
    const bool isTest = command == launch::RequestCommand::Test;
    const std::string name = isTest ? "test" : command == launch::RequestCommand::Run ? "run" : "check";
    const auto& positionals = args.positionals();
    if (positionals.size() != 2)
    {
        if (isTest)
        {
            throw cli::UsageError(positionals.size() < 2
                                      ? "test needs the configuration file: vehicle_sil_launch test <file> --scenarios <directory "
                                        "or files> (the file comes before --scenarios, which takes every path after it; or "
                                        "last, after --)"
                                      : "test takes one configuration file, not " + std::to_string(positionals.size() - 1) +
                                            ": the scenarios come after --scenarios");
        }
        throw cli::UsageError(positionals.size() < 2
                                  ? name + " needs the configuration file: vehicle_sil_launch " + name + " <file>"
                                  : name + " takes one file, not " + std::to_string(positionals.size() - 1));
    }
    launch::RunRequest request;
    launch::RunIntent& intent = request.intent;
    launch::RunHost& host = request.host;
    host.describe_timeout = args.millis("describe-timeout-ms", host.describe_timeout);
    if (host.describe_timeout.count() == 0)
    {
        throw cli::UsageError("--describe-timeout-ms must be at least 1");
    }
    const auto ms = [&args](const char* option, std::int64_t otherwise, std::int64_t min) {
        const auto count = args.millis(option, std::chrono::milliseconds(otherwise)).count();
        if (count < min || count > config::kMaxMillis)
        {
            throw cli::UsageError("--" + std::string(option) + " must be between " + std::to_string(min) + " and " +
                                  std::to_string(config::kMaxMillis));
        }
        return static_cast<std::int64_t>(count);
    };
    if (args.has("stop-at-ms"))
    {
        intent.stop_at_ms = ms("stop-at-ms", 0, 1);
    }
    if (args.has("late-tolerance-ms"))
    {
        intent.late_tolerance_ms = ms("late-tolerance-ms", 0, 0);
    }
    host.start_timeout_ms = ms("participant-start-timeout-ms", host.start_timeout_ms, 1);
    host.stop_timeout_ms = ms("participant-stop-timeout-ms", host.stop_timeout_ms, 1);
    intent.strict = args.flag("strict");
    intent.disable = args.all("disable");
    intent.enable = args.all("enable");
    intent.manual = args.all("manual");
    if (args.has("variant") && !isTest) // test: each of its --variant, in test()
    {
        intent.variant = args.str("variant", "");
        if (intent.variant->empty())
        {
            throw cli::UsageError("--variant needs the name of a variant of the file");
        }
    }
    for (const std::string& given : args.all("param"))
    {
        // <name>=<value>; the value is the rest, also with a '=' in it, also empty.
        const auto equals = given.find('=');
        config::ParamValue param;
        param.name = given.substr(0, equals);
        param.source = config::ParamSource::CommandLine;
        if (equals == std::string::npos || !config::is_param_name(param.name))
        {
            throw cli::UsageError("--param " + given + ": write --param <name>=<value> (the name as the scenario has it in "
                                  "${<name>}: letters, digits and _), e.g. --param idle_rpm=650");
        }
        param.text = given.substr(equals + 1);
        for (const config::ParamValue& before : intent.params)
        {
            if (before.name == param.name)
            {
                throw cli::UsageError("--param " + param.name + " is given twice (" + before.text + " and " + param.text +
                                      "): give each parameter one value");
            }
        }
        intent.params.push_back(std::move(param));
    }
    // Given, not yet checked for a value: its error comes after the ones of --connect and --time-policy.
    if (args.has("scenario"))
    {
        intent.scenario = args.str("scenario", "");
    }
    require_valid(request, command, asked);
    for (const std::string& given : args.all("connect"))
    {
        std::optional<config::ConnectRequest> connect = config::parse_connect(given);
        if (!connect)
        {
            throw cli::UsageError("--connect " + given +
                                  ": write --connect <network>=<target>, the network as the file names it, e.g. --connect "
                                  "\"ETH_EXT=vEthernet (A)\" (none as the target: SIL Kit only)");
        }
        for (const config::ConnectRequest& before : intent.connect)
        {
            if (before.network == connect->network)
            {
                throw cli::UsageError("--connect " + connect->network + " is given twice (" + before.target + " and " +
                                      connect->target + "): connect each network once");
            }
        }
        intent.connect.push_back(std::move(*connect));
    }
    if (!intent.connect.empty())
    {
        host.tool_directory = process::current_executable().parent_path();
    }
    if (args.has("time-policy"))
    {
        const std::optional<config::TimePolicy> policy = config::time_policy_named(args.str("time-policy", ""));
        if (!policy)
        {
            throw cli::UsageError("--time-policy must be realtime (a run that is connected outside the simulation goes at "
                                  "speed 1; the default) or free (its speed and time can be changed), not '" +
                                  args.str("time-policy", "") + "'");
        }
        intent.time_policy = *policy;
    }
    if (intent.scenario)
    {
        if (intent.scenario->empty())
        {
            throw cli::UsageError("--scenario needs a scenario file");
        }
    }
    // What a check reads instead of the file and the executables (validate_request(): check only).
    host.config_from_stdin = args.flag("text-from-stdin");
    host.descriptions = args.str("descriptions", "");
    host.write_descriptions = args.str("write-descriptions", "");
    host.compare_commands = args.str("compare-commands", "");
    require_valid(request, command, asked);
    for (const std::string option : {"descriptions", "write-descriptions"})
    {
        if (args.has(option) && args.str(option, "").empty())
        {
            throw cli::UsageError("--" + option + " needs a file: --" + option + " <file>.json");
        }
    }
    if (args.has("compare-commands") && host.compare_commands.empty())
    {
        throw cli::UsageError("--compare-commands needs a file: --compare-commands <configuration file>");
    }
    host.config = positionals[1];
    return request;
}

int check(const cli::Args& args)
{
    for (const char* runOnly : {"log-dir", "timeout-ms", "events-stall-ms", "events", "stdin-commands", "start-paused",
                                "control", "control-credentials", "control-tls-cert", "control-tls-key", "control-audit-log",
                                "timeline"})
    {
        if (args.has(runOnly) || args.flag(runOnly))
        {
            throw cli::UsageError("--" + std::string(runOnly) + " is for run: check starts no participant");
        }
    }
    const RequestArgument asked = read_request_argument(args);
    launch::RunRequest request = read_request(args, launch::RequestCommand::Check, asked);
    read_speed(args, request.intent);
    request = whole_request(request, launch::RequestCommand::Check, asked);
    const std::string& file = request.host.config;
    const launch::Checked report = launch::check_file(request);
    if (report.usage)
    {
        return usage_error(report.findings, args.flag("json"), launch::CommandKind::Check, file, nullptr, report.variants);
    }
    return write_or_fail(args.flag("json") ? launch::format_json(report) : launch::format_text(report), launch::check_exit_code(report));
}

/// `connections [--json] [--network <name>] <file>`.
int connections(const cli::Args& args)
{
    if (args.has("variant"))
    {
        throw cli::UsageError("connections takes no --variant: every variant has the networks of the file, so leave it out");
    }
    const auto& positionals = args.positionals();
    if (positionals.size() != 2)
    {
        throw cli::UsageError(positionals.size() < 2
                                  ? "connections needs the configuration file: vehicle_sil_launch connections <file>"
                                  : "connections takes one file, not " + std::to_string(positionals.size() - 1));
    }
    launch::ConnectionsOptions options;
    if (args.has("network"))
    {
        options.network = args.str("network", "");
        if (options.network->empty())
        {
            throw cli::UsageError("--network needs the name of a network of the file");
        }
    }
    options.tool_directory = process::current_executable().parent_path();
    options.request = read_request_argument(args).file;
    const std::string& file = positionals[1];
    const launch::ConnectionsReport report = launch::list_connections(process::path_from_utf8(file), file, options);
    if (report.usage)
    {
        return usage_error(report.findings, args.flag("json"), launch::CommandKind::Connections, file, nullptr);
    }
    return write_or_fail(args.flag("json") ? launch::format_connections_json(report) : launch::format_connections_text(report),
                         report.exit_code());
}

int run(const cli::Args& args, launch::EventStream* events)
{
    const RequestArgument asked = read_request_argument(args);
    launch::RunRequest request = read_request(args, launch::RequestCommand::Run, asked);
    launch::RunHost& host = request.host;
    const std::string& file = host.config;
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
    if (args.has("log-dir"))
    {
        host.log_dir = process::path_from_utf8(args.str("log-dir", ""));
    }
    if (args.has("timeline"))
    {
        host.timeline = args.str("timeline", "");
        if (host.timeline.empty())
        {
            throw cli::UsageError("--timeline needs a file to write: --timeline <file>.json");
        }
        require_valid(request, launch::RequestCommand::Run, asked);
    }
    if (args.has("timeout-ms"))
    {
        host.timeout = args.millis("timeout-ms", std::chrono::milliseconds(0));
        if (host.timeout->count() == 0)
        {
            throw cli::UsageError("--timeout-ms must be at least 1");
        }
    }

    read_speed(args, request.intent);
    const bool control = args.has("control");
    for (const char* option : {"control-credentials", "control-tls-cert", "control-tls-key", "control-audit-log"})
    {
        if (args.has(option) && !control)
        {
            throw cli::UsageError("--" + std::string(option) + " is for --control: without it there is no control server");
        }
    }
#ifndef VEHICLE_SIL_LAUNCH_CONTROL_SERVER
    if (control)
    {
        throw cli::UsageError("--control needs a vehicle_sil_launch built with the control server: configure with "
                              "-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON (it needs gRPC) and build again; `vehicle_sil_launch "
                              "version --json` lists control-server in its features when it has it (docs/control-api.md)");
    }
#endif
    if (control)
    {
        // As given: the run reads and checks them (launch::run_file()), and a value that is wrong
        // comes back as a usage error of the run.
        launch::ControlHost& server = host.control.emplace();
        server.listen = args.str("control", "");
        server.credentials = args.str("control-credentials", "");
        server.tls_cert = args.str("control-tls-cert", "");
        server.tls_key = args.str("control-tls-key", "");
        server.audit_log = args.str("control-audit-log", "");
    }
    host.stdin_commands = args.flag("stdin-commands");
    request.intent.start_paused = args.flag("start-paused");
    require_valid(request, launch::RequestCommand::Run, asked);
    // From here on the request is the whole of it: the arguments over the file of --request,
    // judged now that the fields the rules need (--stdin-commands, --control) are read.
    request = whole_request(request, launch::RequestCommand::Run, asked);
    runner::RunObservers watching;
    launch::RunLent lent;
    watch_interrupts();
    if (events != nullptr)
    {
        lent.events = events;
    }
    else
    {
        // The answers of commands that waited for the configuration are kept (keep_stdin_note()):
        // they are written before the run's next line, and after the run. Like the line itself,
        // this does not wait for stderr without a limit.
        watching.write_line = [](std::string_view line) {
            write_stdin_notes(true);
            write_stderr_line(line);
        };
    }
    {
        // The controls of this run, on the stop requests of the process. Never destroyed: the
        // reader of stdin may use them until the process ends.
        runner::RunControls::Observers observers;
        // The reader of standard input starts when the run begins its check, the first thing a
        // run tells: before any --describe, so that a command given at once is read while the
        // check runs. A run that is refused before its check (--control with a value that is
        // wrong) reads no command, and its only event is the result.
        const bool reads_commands = host.stdin_commands;
        observers.on_phase = [events, reads_commands](runner::Phase phase) {
            if (phase == runner::Phase::Checking && reads_commands)
            {
                start_reading_commands();
            }
            if (events != nullptr)
            {
                events->phase(phase);
            }
        };
        if (events != nullptr)
        {
            observers.on_command = [events](const runner::Command& command, std::string_view source) {
                events->command(command, source);
            };
            observers.on_state = [events](const runner::RunState& state) { events->state(state); };
        }
        else
        {
            // No event says what became of a command: a refused one of standard input is a line,
            // written once the time commands' mutex is left (this is called under it).
            observers.on_command = [](const runner::Command& command, std::string_view source) {
                if (!command.accepted && source == "stdin")
                {
                    keep_stdin_note(command);
                }
            };
        }
        shared().controls = new runner::RunControls(g_stops, request.intent.start_paused, std::move(observers),
                                                    request.intent.scenario.has_value());
        // The state names the first stop request as the result does.
        shared().controls->set_stop_name(interrupt_name);
        // Without --control nothing can send a write: the state says so.
        shared().controls->set_control_server(control);
        lent.controls = shared().controls;
    }
    watching.interrupt_name = interrupt_name;
    // With --control, the run's control server has ended when this returns: every call of a client
    // has ended (a watch, a wait), none comes any more, and its audit log is complete. The report
    // holds what is left of it, which goes after the result is written: when this function
    // returns, or below, before the process is ended at once.
    launch::RunReport report = launch::run_file(request, watching, lent);
    if (events == nullptr)
    {
        write_stdin_notes(true); // before the result
    }
    if (report.check.usage)
    {
        return usage_error(report.check.findings, args.flag("json"), launch::CommandKind::Run, file, events, report.check.variants);
    }
    const int code = events != nullptr
                         ? finish_events(*events, report, report.exit_code())
                         : write_or_fail(args.flag("json") ? launch::format_run_json(report) : launch::format_run_text(report),
                                         report.exit_code());
    if (report.run && report.run->close.not_closed_ms)
    {
        report.ended_control_server.reset(); // the hub stays, with the thread that was left closing
        end_process_at_once(code);
    }
    return code;
}

/// "--scenarios a b c" as "--scenarios a --scenarios b --scenarios c" (test): --scenarios takes
/// every argument after it up to the next option or "--", so that a shell's list of files
/// (scenarios/*.yaml) can follow it.
std::vector<std::string> spread_scenarios(const std::vector<std::string>& arguments)
{
    std::vector<std::string> out;
    bool ended = false;   // after "--"
    bool listing = false; // after --scenarios and its first path
    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        const std::string& a = arguments[i];
        if (ended)
        {
            out.push_back(a);
        }
        else if (a == "--")
        {
            ended = true;
            out.push_back(a);
        }
        else if (a == "--scenarios")
        {
            out.push_back(a);
            listing = i + 1 < arguments.size() && arguments[i + 1].substr(0, 2) != "--";
            if (listing)
            {
                out.push_back(arguments[++i]);
            }
        }
        else if (a.substr(0, 2) == "--")
        {
            listing = false;
            out.push_back(a);
        }
        else
        {
            if (listing)
            {
                out.emplace_back("--scenarios");
            }
            out.push_back(a);
        }
    }
    return out;
}

int test(const cli::Args& args)
{
    if (args.has("scenario"))
    {
        throw cli::UsageError("--scenario is for check and run: test takes its scenarios with --scenarios <directory or files>");
    }
    if (args.has("connect"))
    {
        throw cli::UsageError("--connect is not for test: test is made for runs that go as fast as possible, one after the "
                              "other, and a real bus does not wait for the virtual time; for a test with a device, run one scenario: vehicle_sil_launch "
                              "run --scenario <scenario> --speed 1 --strict --connect <network>=<target> <file>");
    }
    const RequestArgument asked = read_request_argument(args);
    if (asked.file)
    {
        for (const auto& field : asked.file->fields)
        {
            if (field != "stop_at_ms" && field != "late_tolerance_ms")
            {
                throw cli::UsageError("test --request takes only stop_at_ms and late_tolerance_ms; it selects its own runs");
            }
        }
    }
    for (const char* runOnly : {"log-dir", "events-stall-ms", "events", "stdin-commands", "speed", "start-paused", "manual", "control",
                                "control-credentials", "control-tls-cert", "control-tls-key", "control-audit-log", "timeline"})
    {
        if (args.has(runOnly) || args.flag(runOnly))
        {
            throw cli::UsageError("--" + std::string(runOnly) +
                                  " is for run, not for test (which runs one simulation after the other, each as fast as "
                                  "possible, or at speed 1 if a role: bridge participant connects it outside the "
                                  "simulation): to use it, run one scenario with vehicle_sil_launch run --scenario <file>");
        }
    }
    launch::RunRequest request = whole_request(read_request(args, launch::RequestCommand::Test, asked),
                                               launch::RequestCommand::Test, asked);
    const std::string& file = request.host.config;
    const bool json = args.flag("json");
    launch::TestOptions options;
    options.variants = args.all("variant");
    for (const std::string& variant : options.variants)
    {
        if (variant.empty())
        {
            throw cli::UsageError("--variant needs the name of a variant of the file");
        }
    }
    options.fail_fast = args.flag("fail-fast");
    if (args.has("timeout-ms"))
    {
        request.host.timeout = args.millis("timeout-ms", std::chrono::milliseconds(0));
        if (request.host.timeout->count() == 0)
        {
            throw cli::UsageError("--timeout-ms must be at least 1");
        }
    }
    const std::vector<std::string> given = args.all("scenarios");
    if (given.empty())
    {
        throw cli::UsageError("test needs the scenarios to run: give --scenarios <directory> (every .yaml, .yml and .json "
                              "file directly in it) or --scenarios <file>... after the configuration file");
    }
    launch::ScenarioList list = launch::list_scenarios(given);
    if (!list.errors.empty())
    {
        return usage_error(list.errors, json, launch::CommandKind::Test, file, nullptr);
    }
    options.scenarios = std::move(list.scenarios);
    // As fast as possible, whatever the file says (at speed 1 with the time lock): a default, not
    // a speed the user asked for, which the check would refuse with the time lock.
    request.intent.speed.emplace(std::optional<double>{});
    options.fastest_by_default = true;
    options.request = request;
    watch_interrupts();
    options.observers.write_line = write_stderr_line;
    options.lent.stops = &g_stops;
    options.observers.interrupt_name = interrupt_name;
    if (vehicle_sil::trace::enabled())
    {
        // One file holds the runs of every scenario, their virtual times from 0 each, and the rings of
        // the threads of every run stay until the end (docs/internals/trace.md).
        write_stderr_line("vehicle-sil trace: test runs every scenario in this one process, so its trace mixes them and "
                          "scripts/trace-report.py refuses it. Trace one scenario with `run --scenario` instead.");
    }
    const launch::TestReport report = launch::test_file(options);
    if (report.usage)
    {
        return usage_error(report.findings, json, launch::CommandKind::Test, file, nullptr);
    }
    const int code = write_or_fail(json ? launch::format_test_json(report) : launch::format_test_text(report), report.exit_code());
    if (report.connection_not_closed)
    {
        end_process_at_once(code);
    }
    return code;
}

/// What this build adds to launch::kFeatures.
std::vector<std::string_view> build_features()
{
#ifdef VEHICLE_SIL_LAUNCH_CONTROL_SERVER
    return {launch::kControlServerFeature, launch::kScenarioControlFeature, launch::kScenarioTimeFeature};
#else
    return {};
#endif
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
    return write_stdout(json ? launch::format_version_json(build_features()) : launch::format_version_text())
               ? cli::kExitOk
               : cli::kExitSetupFailed;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // No message box of Windows for a participant whose DLL is missing: the children inherit the
    // mode, so such a participant ends at once (check reports it) instead of waiting for someone
    // to close the box until its time runs out.
    // (SetErrorMode() returns the mode before, which is kept.)
    SetErrorMode(SetErrorMode(SEM_FAILCRITICALERRORS) | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#else
    // SIGCHLD ignored by whoever started the launcher (SIG_IGN is inherited) would make the system
    // reap the participants itself, and their exit codes would be lost.
    std::signal(SIGCHLD, SIG_DFL);
#endif
    std::vector<std::string> arguments = cli::utf8_arguments(argc, argv);
    if (const auto first = positional_arguments(arguments); !first.empty() && first.front() == "test")
    {
        arguments = spread_scenarios(arguments);
    }
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
    const bool isTest = !positionals.empty() && positionals.front() == "test";
    const bool isConnections = !positionals.empty() && positionals.front() == "connections";
    const launch::CommandKind command = isRun ? launch::CommandKind::Run : isTest ? launch::CommandKind::Test : isConnections ? launch::CommandKind::Connections : launch::CommandKind::Check;
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
                                   false, launch::CommandKind::Run, file, nullptr);
            }
        }
    }
    if (isTest)
    {
        ignore_broken_pipes();
    }
    int code = cli::kExitSetupFailed;
    try
    {
        // test takes --variant more than once, and has options of its own.
        const cli::Args args =
            isConnections ? cli::Args(arguments, {"network", "variant", "request"}, {"json"})
            : isTest ? cli::Args(arguments,
                               {"describe-timeout-ms", "log-dir", "timeout-ms", "stop-at-ms", "late-tolerance-ms",
                                "participant-start-timeout-ms", "participant-stop-timeout-ms",
                                "events-stall-ms", "speed", "scenario", "control",
                                "control-credentials", "control-tls-cert", "control-tls-key", "control-audit-log", "timeline",
                                "time-policy", "request", "descriptions", "write-descriptions", "compare-commands"},
                               {"json", "strict", "events", "stdin-commands", "start-paused", "fail-fast", "text-from-stdin"},
                               {"disable", "enable", "manual", "param", "variant", "scenarios", "connect"})
                   : cli::Args(arguments,
                               {"describe-timeout-ms", "log-dir", "timeout-ms", "stop-at-ms", "late-tolerance-ms",
                                "participant-start-timeout-ms", "participant-stop-timeout-ms",
                                "events-stall-ms", "speed", "scenario", "control",
                                "control-credentials", "control-tls-cert", "control-tls-key", "control-audit-log", "variant",
                                "timeline", "time-policy", "request", "descriptions", "write-descriptions",
                                "compare-commands"},
                               {"json", "strict", "events", "stdin-commands", "start-paused", "text-from-stdin"},
                               {"disable", "enable", "manual", "param", "connect"});
        if (args.positionals().empty())
        {
            code = usage_error("no command: give check <file>, run <file>, test <file>, connections <file> or version", json,
                               command, file, stream);
        }
        else if (args.positionals().front() != "check" && !isRun && !isTest && !isConnections)
        {
            code = usage_error("unknown command '" + args.positionals().front() +
                                   "': the commands are check, run, test, connections and version",
                               json, command, file, stream);
        }
        else
        {
            code = isRun ? run(args, stream) : isTest ? test(args) : isConnections ? connections(args) : check(args);
        }
    }
    catch (const cli::UsageError& e)
    {
        code = usage_error(e.what(), json, command, file, stream);
    }
    catch (const RequestFileError& e)
    {
        code = usage_error(e.findings, json, command, file, stream);
    }
    catch (const std::exception& e)
    {
        write_stderr_line(std::string("error: ") + e.what());
        if (stream != nullptr)
        {
            static_cast<void>(finish_events(*stream, launch::internal_error_run_report(e.what()), cli::kExitSetupFailed));
        }
        else if (json)
        {
            static_cast<void>(write_or_fail(launch::format_internal_error_json(command, file, e.what()), cli::kExitSetupFailed));
        }
        code = cli::kExitSetupFailed;
    }
    // The reader of stdin and the threads of the events may still wait: they write nothing from here on.
    finish_threads();
    return code;
}
