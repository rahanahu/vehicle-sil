// SPDX-License-Identifier: MIT
//
// The code that the launch_run scenarios share: running the launcher, comparing
// its output and events, the participants started by hand, a free port.
#pragma once

#include "launcher_support.hpp"
#include "spawn_sleeper.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <tlhelp32.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;
using vehicle_sil::process::Captured;
using vehicle_sil::process::shown_path;

struct Options
{
    fs::path launch;
    fs::path test_ecu;
    fs::path helper;
    fs::path example_dir; ///< the example's directory in the build tree (simulation.yaml, engine/, dashboard/)
    fs::path docs; ///< the directory docs/ (each example is in the one document that has its marker line)
    fs::path work_dir;
    std::string scenario;
    // For the scenarios connect_* only (empty for the others): the directory with
    // vehicle_sil_launch and the two bridges on fake drivers under their names (CMake's
    // connect_tools), the Ethernet ECU of the bridge's tests, and the fake pcap libraries.
    fs::path connect_tools{};
    fs::path bridge_ecu{};
    fs::path fake_pcap_dir{};
    /// sil-kit-registry, for the scenarios that start a bridge outside the run (connect_device_shared*).
    fs::path registry{};
};

/// The scenario cannot run on this machine: main() exits with 77 (CTest's SKIP_RETURN_CODE).
struct Skip : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/// Where and how the launcher's lines on `err` say to start `name` by hand: (the directory, the
/// command line, the two lines); empty if they are not there yet.
struct HandCommand
{
    std::string directory;
    std::string line;
    std::string text;
};

void require(bool condition, const std::string& what);

bool contains(const std::string& text, const std::string& part);

std::string read_text(const fs::path& path);

std::string lf(std::string text);

void write_file(const fs::path& path, const std::string& text);

/// A YAML scalar in single quotes (a path with '\' or spaces as it is).
std::string yaml_quote(const std::string& text);

/// `text` with every value of a "vehicle_sil" key (a version) replaced by "<version>".
std::string without_versions(std::string text);

/// `text` with the values that depend on how fast the machine is as the docs show them: every
/// "wall_ms" and "actual_speed" (in the JSON and in the events) as <wall_ms> and <actual_speed>,
/// and the port of a "registry_uri" at 127.0.0.1 as <port>.
std::string without_timing(std::string text);

/// The Markdown file directly in the directory `docs` that has the line `marker` (exactly one has it).
fs::path marked_file(const fs::path& docs, const std::string& marker);

/// The fenced block after the line `marker` in the Markdown file, or in the one document of the
/// directory that has the line, without the fences.
std::string block_after(const fs::path& path, const std::string& marker);

void expect_text(const std::string& actual, const std::string& expected, const std::string& what);

/// Runs vehicle_sil_launch with `args` in `dir` to its end. The time limit is only a last line
/// against a hang.
Captured launch(const Options& o, const std::vector<std::string>& args, const fs::path& dir);

void expect_code(const Captured& run, int code, const std::string& what);

/// A stop request can be sent: always on POSIX; on Windows only with a console that the
/// processes share.
bool can_request_stop();

/// The part of the run's text from "run:" on.
std::string run_part(const std::string& text);

/// The example's directory as a copy in the work directory: simulation.yaml and the ECUs of engine/
/// and dashboard/ (hard links to the built files, copies where the file system has no links; on
/// Windows the DLLs next to them too). The files that the docs show with mistakes in them are
/// written here, where their relative paths find the ECUs as in the example, so that nothing that
/// fails on purpose lies next to the example that works (build/<preset>/examples/ecu_main/).
fs::path example_copy(const Options& o);

/// A scenario's directory with sim.yaml and the behaviour files of the test ECU.
fs::path scenario_dir(const Options& o, const std::string& config, const std::vector<std::pair<std::string, std::string>>& behaviours = {});

/// Waits until the test ECU `name` (hang-after-end) hangs after the run: from then on its own
/// handler answers a stop request. Before, ecu_main may still handle it, and the request is spent
/// on a run that has already stopped.
void wait_hanging(const fs::path& dir, const std::string& name);

std::string ecu(const Options& o);

std::string helper(const Options& o);

/// Ends the processes whose IDs are in `pid_files` if they still run (after a failed check), and
/// says whether every one had ended within `timeout`.
bool all_gone(const fs::path& dir, const std::vector<std::string>& pid_files, std::chrono::milliseconds timeout);

/// The lines of `text` (each ends with "\n").
std::vector<std::string> lines_of(const std::string& text);

/// The string value of `key` in an event line (the first), or "" if it has none.
std::string value_of(const std::string& line, const std::string& key);

/// The members of an event line after event, seq and t_ms ("" if it has none), without the braces.
std::string members_of(const std::string& line);

/// Checks the frame of the event line `i` (an object that starts with event, seq and t_ms; seq
/// 0, 1, 2, ...; hello first and only first).
void check_frame(const std::string& line, std::size_t i);

/// Checks every event line (check_frame(); result last and once; the phases never go back; a
/// participant's lines come after its "started"; every finding is in the result as it was
/// written, and none twice) and returns the lines.
std::vector<std::string> check_events(const std::vector<std::string>& lines);

std::vector<std::string> check_events(const std::string& text);

/// The lines that contain all of `parts`.
std::vector<std::string> events_with(const std::vector<std::string>& lines, const std::vector<std::string>& parts);

std::string code_text(const std::optional<vehicle_sil::process::ExitStatus>& status);

/// The object "scenario" of the run's pretty JSON (the last one at the top level), as text.
std::string scenario_json(const std::string& json);

/// Two test ECUs with the variables for scenarios, and `keys` at the top of the configuration.
fs::path scenario_ecus(const Options& o, const std::string& keys, const std::string& scenario, const std::string& body = "");

HandCommand hand_command(const std::string& err, const std::string& name);

/// A TCP port on 127.0.0.1 that is free now, for a registry_uri of a test. The launcher binds it
/// only later, so it is taken below the ports that the system hands out by itself: a port from
/// there (what binding port 0 gives) may go, in the meantime, to anything that binds port 0 or
/// connects (a participant of a test that runs at the same time listens at port 0, so does a
/// registry without registry_uri), and the run then cannot set up its registry. Below that range
/// only a program that names its port takes one; it is picked at random so that two copies of a
/// test (two build trees on one computer) rarely meet.
int free_port();

/// A directory of its own for a part of the scenario manual, with sim.yaml and behaviour files.
fs::path hand_dir(const Options& o, const std::string& part, const std::string& config,
                  const std::vector<std::pair<std::string, std::string>>& behaviours);

/// Each line of the run's standard output, for a message.
std::string out_of(const std::vector<std::string>& lines);

/// Runs the launcher with `args` in `dir`, waits for "simulation running" on its stderr, then
/// calls `act` with it.
template <class Act>
std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::string> run_until_running(
    const Options& o, const fs::path& dir, Act act, const std::vector<std::string>& args = {"run", "sim.yaml"})
{
    std::mutex mutex;
    std::condition_variable changed;
    std::string out;
    std::string err;
    bool running = false;
    vehicle_sil::process::ProcessOptions options;
    options.working_directory = dir;
    options.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
        const std::lock_guard lock(mutex);
        (stream == vehicle_sil::process::Stream::Output ? out : err) += std::string(line) + "\n";
        running = running || line == "[vehicle_sil_launch] simulation running";
        changed.notify_all();
    };
    vehicle_sil::process::Process launcher(o.launch, args, std::move(options));
    {
        std::unique_lock lock(mutex);
        // Only a last line against a hang: the start takes a second or two.
        require(changed.wait_for(lock, scaled_timeout(120s), [&] { return running || launcher.poll().has_value(); }) && running,
                "the simulation did not start:\n" + err);
    }
    act(launcher);
    const auto status = launcher.wait_for(scaled_timeout(120s));
    launcher.finish_output(10s);
    const std::lock_guard lock(mutex);
    log("stderr of the launcher:\n" + err);
    return {status, out};
}

/// vehicle_sil_launch run with a pipe as its standard input and its events read as they come.
class Live
{
public:
    /// `detached` (Windows): without a console, through the helper's --run-detached.
    Live(const Options& o, const fs::path& dir, const std::vector<std::string>& args, bool detached = false)
    {
        vehicle_sil::process::ProcessOptions options;
        options.working_directory = dir;
        options.input = true;
        // An event is one line, however long (the result of a run with many variables or steps).
        options.max_line = std::size_t{1} << 20;
        options.on_line = [this](vehicle_sil::process::Stream stream, std::string_view line) {
            std::unique_lock lock(mutex_);
            if (stream == vehicle_sil::process::Stream::Output)
            {
                out_.emplace_back(line);
            }
            else
            {
                err_ += std::string(line) + "\n";
            }
            changed_.notify_all();
            // hold(): the reader of the launcher's output stops here, and its pipes fill.
            changed_.wait(lock, [this] { return !held_; });
        };
        std::vector<std::string> all = args;
        if (detached)
        {
            all.insert(all.begin(), {"--run-detached", shown_path(o.launch)});
        }
        process_ = std::make_unique<vehicle_sil::process::Process>(detached ? o.helper : o.launch, all, std::move(options));
    }

    ~Live()
    {
        release();
        process_.reset(); // ends the launcher and its reader, which uses the members above
    }

    vehicle_sil::process::Process& process() { return *process_; }

    /// Stops reading the launcher's output (after the line that is being read) until release().
    void hold()
    {
        const std::lock_guard lock(mutex_);
        held_ = true;
    }

    void release()
    {
        const std::lock_guard lock(mutex_);
        held_ = false;
        changed_.notify_all();
    }

    /// Waits for an event line that contains every one of `parts` (up to 120 s, scaled); returns it.
    std::string wait_for(const std::vector<std::string>& parts)
    {
        std::unique_lock lock(mutex_);
        std::string found;
        std::size_t next = 0;
        // Only a last line against a hang: the start takes a second or two.
        const bool seen = changed_.wait_for(lock, scaled_timeout(120s), [&] {
            for (; next < out_.size() && found.empty(); ++next)
            {
                if (std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(out_[next], p); }))
                {
                    found = out_[next];
                }
            }
            return !found.empty() || process_->poll().has_value();
        });
        require(seen && !found.empty(), "no event with " + parts.front() + " in:\n" + text_locked() + "--- stderr ---\n" + err_);
        return found;
    }

    /// Waits until the latest "state" says paused (up to 120 s, scaled). For a pause of a simulation
    /// that goes. After a step or a run_until use wait_standing(): the state before the command's
    /// says paused too.
    void wait_paused()
    {
        std::unique_lock lock(mutex_);
        const auto paused = [&] {
            for (std::size_t i = out_.size(); i-- > 0;)
            {
                if (contains(out_[i], "\"event\":\"state\""))
                {
                    return contains(out_[i], "\"paused\":true,");
                }
            }
            return false;
        };
        const bool seen = changed_.wait_for(lock, scaled_timeout(120s), [&] { return paused() || process_->poll().has_value(); });
        require(seen && paused(), "the latest state does not say paused:\n" + text_locked());
    }

    /// Waits until the simulation stands still at `ms`: a "state" says paused with that time
    /// (`paused_at_ms`), and the "time" event of that time has come (the look that finds it standing
    /// writes it; before or after the state). Returns the time event.
    std::string wait_standing(std::int64_t ms)
    {
        static_cast<void>(wait_for({"\"event\":\"state\"", "\"paused\":true,\"paused_at_ms\":" + std::to_string(ms) + ","}));
        return wait_for({"\"event\":\"time\"", "\"sim_ms\":" + std::to_string(ms) + ","});
    }

    /// Waits for an event line with every one of `parts` that comes after one with every one of
    /// `first` (up to 120 s, scaled); returns it.
    std::string wait_for_after(const std::vector<std::string>& first, const std::vector<std::string>& parts)
    {
        std::unique_lock lock(mutex_);
        const auto has = [&](std::size_t i, const std::vector<std::string>& all) {
            return std::all_of(all.begin(), all.end(), [&](const std::string& p) { return contains(out_[i], p); });
        };
        std::string found;
        std::size_t next = 0;
        bool begun = false;
        const bool seen = changed_.wait_for(lock, scaled_timeout(120s), [&] {
            for (; next < out_.size() && found.empty(); ++next)
            {
                if (begun && has(next, parts))
                {
                    found = out_[next];
                }
                begun = begun || has(next, first);
            }
            return !found.empty() || process_->poll().has_value();
        });
        require(seen && !found.empty(), "no event with " + parts.front() + " after one with " + first.front() + " in:\n" + text_locked());
        return found;
    }

    /// Waits until `n` event lines contain every one of `parts` (up to 120 s, scaled).
    void wait_for_count(const std::vector<std::string>& parts, std::size_t n)
    {
        std::unique_lock lock(mutex_);
        std::size_t next = 0;
        std::size_t count = 0;
        const bool seen = changed_.wait_for(lock, scaled_timeout(120s), [&] {
            for (; next < out_.size(); ++next)
            {
                count += std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(out_[next], p); }) ? 1 : 0;
            }
            return count >= n || process_->poll().has_value();
        });
        require(seen && count >= n, std::to_string(n) + " events with " + parts.front() + " did not come:\n" + text_locked());
    }

    void write(const std::string& text) { require(process_->write_input(text), "cannot write to the launcher"); }

    /// Waits until its standard error makes `done` true, or it has ended (up to 120 s, scaled); returns
    /// the standard error.
    template <class Done>
    std::string wait_for_error(Done done)
    {
        std::unique_lock lock(mutex_);
        // Only a last line against a hang. It looks every 100 ms whether the launcher has ended:
        // that comes with no line that would wake it.
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(120s);
        while (!done(err_) && !process_->poll().has_value() && std::chrono::steady_clock::now() < deadline)
        {
            static_cast<void>(changed_.wait_for(lock, 100ms));
        }
        return err_;
    }

    /// Waits for its end (up to `timeout`, scaled) and reads the rest of its output; its status and
    /// its lines, unchecked.
    std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::vector<std::string>> finish_unchecked(
        std::chrono::milliseconds timeout = 120s)
    {
        release();
        const auto status = process_->wait_for(scaled_timeout(timeout));
        process_->finish_output(10s);
        const std::lock_guard lock(mutex_);
        log("stderr of the launcher:\n" + err_);
        return {status, out_};
    }

    /// Waits for its end; its status and its events (checked: framed, in order).
    std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::vector<std::string>> finish()
    {
        const auto [status, lines] = finish_unchecked();
        return {status, check_events(lines)};
    }

    std::string err()
    {
        const std::lock_guard lock(mutex_);
        return err_;
    }

    std::vector<std::string> lines()
    {
        const std::lock_guard lock(mutex_);
        return out_;
    }

private:
    /// The last event lines (the caller holds mutex_): enough to see where it stopped.
    std::string text_locked() const
    {
        std::string text;
        for (std::size_t i = out_.size() > 40 ? out_.size() - 40 : 0; i < out_.size(); ++i)
        {
            text += out_[i].substr(0, 400) + "\n";
        }
        return text;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> out_;
    std::string err_;
    bool held_ = false;
    std::unique_ptr<vehicle_sil::process::Process> process_;
};

/// A participant started from the line the launcher wrote, as a person who copies it would: in
/// its directory; on POSIX through sh, on Windows split as the C runtimes split a command line.
class ByHand
{
public:
    ByHand(const std::string& line, const std::string& directory)
    {
        vehicle_sil::process::ProcessOptions options;
        options.working_directory = vehicle_sil::process::path_from_utf8(directory);
        options.on_line = [this](vehicle_sil::process::Stream, std::string_view text) {
            const std::lock_guard lock(mutex_);
            output_ += std::string(text) + "\n";
        };
        log("starting by hand in " + directory + ": " + line);
#ifdef _WIN32
        const int size = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), size);
        int count = 0;
        wchar_t** words = CommandLineToArgvW(wide.c_str(), &count);
        require(words != nullptr && count > 0, "the command line cannot be split: " + line);
        std::vector<std::string> args;
        for (int i = 0; i < count; ++i)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, words[i], -1, nullptr, 0, nullptr, nullptr);
            std::string word(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, words[i], -1, word.data(), n, nullptr, nullptr);
            args.push_back(std::move(word));
        }
        LocalFree(words);
        const fs::path executable = vehicle_sil::process::path_from_utf8(args.front());
        args.erase(args.begin());
        process_ = std::make_unique<vehicle_sil::process::Process>(executable, args, std::move(options));
#else
        process_ = std::make_unique<vehicle_sil::process::Process>("/bin/sh", std::vector<std::string>{"-c", line},
                                                                   std::move(options));
#endif
    }
    ByHand(const ByHand&) = delete;
    ByHand& operator=(const ByHand&) = delete;
    ~ByHand() { process_.reset(); } // ends it if it still runs

    /// Stops every thread of it where it is, as a debugger's pause does (POSIX SIGSTOP to its process
    /// group, Windows each of its threads suspended); thaw() lets it go on.
    void freeze() { suspend(true); }
    void thaw() { suspend(false); }

    /// Waits for its end (it ends by itself): its status.
    std::optional<vehicle_sil::process::ExitStatus> wait(std::chrono::milliseconds timeout)
    {
        const auto status = process_->wait_for(scaled_timeout(timeout));
        process_->finish_output(10s);
        const std::lock_guard lock(mutex_);
        log("output of the participant started by hand:\n" + output_);
        return status;
    }

    /// For a failure's message: whether it still runs (or how it ended) and what it has written so far.
    std::string report(const std::string& who)
    {
        const auto status = process_->wait_for(0ms);
        const std::lock_guard lock(mutex_);
        return "--- " + who + " (started by hand): " +
               (status ? "ended with exit code " + std::to_string(status->code) +
                             (status->signal ? " (" + vehicle_sil::process::signal_name(*status->signal) + ")" : "")
                       : std::string("still runs")) +
               "; its output:\n" + output_;
    }

private:
    void suspend(bool stop)
    {
        const long pid = process_->id();
#ifdef _WIN32
        // Started without a shell: the process is the ECU.
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        require(snapshot != INVALID_HANDLE_VALUE, "cannot list the threads");
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != static_cast<DWORD>(pid))
            {
                continue;
            }
            if (const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID); thread != nullptr)
            {
                static_cast<void>(stop ? SuspendThread(thread) : ResumeThread(thread));
                CloseHandle(thread);
            }
        }
        CloseHandle(snapshot);
#else
        // sh and the ECU it started: the process group the process library made.
        require(::kill(-static_cast<pid_t>(pid), stop ? SIGSTOP : SIGCONT) == 0, "cannot stop or continue the process");
#endif
        log(std::string(stop ? "froze" : "thawed") + " the participant started by hand");
    }

    std::mutex mutex_;
    std::string output_;
    std::unique_ptr<vehicle_sil::process::Process> process_;
};

// The scenarios, by the file they are in; main() picks the one --scenario names.

// launch_run_processes.cpp
void connect_roundtrip(const Options& o);
void connect_no_adapter(const Options& o);
void connect_nothing_received(const Options& o);
void connect_bus_off(const Options& o);
void connect_bus_off_lost(const Options& o);
void connect_lost_kill(const Options& o);
void connect_lost_exit0(const Options& o);
void connect_dies_while_waiting(const Options& o);
void connect_dies_while_held(const Options& o);
void connect_request(const Options& o);
void connect_speed(const Options& o);
void connect_time_changed(const Options& o);
void connect_time_locked(const Options& o);
void connect_device_shared(const Options& o);
void connect_device_shared_later(const Options& o);
void connect_device_shared_own(const Options& o);
void docs(const Options& o);
void check(const Options& o);
void crash(const Options& o);
void step(const Options& o);
void optional(const Options& o);
void not_joined(const Options& o);
void quitter(const Options& o);
void hang(const Options& o);
void grandchild(const Options& o);
void path(const Options& o);
void interrupt(const Options& o);
void interrupt_twice(const Options& o);
void ended_early(const Options& o);
void interrupt_ending(const Options& o);
void timeout(const Options& o);
void no_console(const Options& o);
void killed(const Options& o);
void usage(const Options& o);
void control(const Options& o);
void disabled(const Options& o);

// launch_run_events.cpp
void events(const Options& o);
void events_usage(const Options& o);
void stdin_stop(const Options& o);
void stdin_eof(const Options& o);
void stdin_twice(const Options& o);
void stdin_ending(const Options& o);
void output_closed(const Options& o);
void stop_once(const Options& o);
void events_stalled(const Options& o);
void events_slow(const Options& o);
void events_findings(const Options& o);
void events_stderr(const Options& o);
void events_nonblocking(const Options& o);
void events_output(const Options& o);
void stdin_killed(const Options& o);
void no_console_stdin(const Options& o);

// launch_run_time.cpp
void realtime(const Options& o);
void on_time(const Options& o);
void late(const Options& o);
void stall(const Options& o);
void stand_still(const Options& o);
void speed_change(const Options& o);
void step_width(const Options& o);
void pause_step(const Options& o);
void run_until_stdin(const Options& o);
void state_on_arrival(const Options& o);
void paused_failures(const Options& o);
void time_usage(const Options& o);

// launch_run_scenario.cpp
void scenario(const Options& o);
void scenario_same(const Options& o);
void scenario_each_write(const Options& o);
void scenario_wave(const Options& o);
void scenario_dither(const Options& o);
void live(const Options& o);
void scenario_disconnect(const Options& o);
void ethernet(const Options& o);
void scenario_expect_beside(const Options& o);
void scenario_failures(const Options& o);

// launch_run_scenario_time.cpp
void scenario_time_commands_same(const Options& o);
void scenario_paused_timeout(const Options& o);

// launch_run_variants.cpp
void variable_map(const Options& o);
void variants(const Options& o);
void test_command(const Options& o);
void test_interrupt(const Options& o);

// launch_run_timeline.cpp
void timeline(const Options& o);

// launch_run_availability.cpp
void availability(const Options& o);

// launch_run_manual.cpp
void network(const Options& o);
void manual_late(const Options& o);
void manual_fails(const Options& o);
void manual_stop(const Options& o);
void manual_frozen(const Options& o);
void manual_thawed(const Options& o);
void manual_again(const Options& o);
void manual_timeout(const Options& o);
void busy_port(const Options& o);
void manual_stranger(const Options& o);
void manual(const Options& o);

// launch_run_networks.cpp
void traffic(const Options& o);
void network_names(const Options& o);
void j1939_nodes(const Options& o);
