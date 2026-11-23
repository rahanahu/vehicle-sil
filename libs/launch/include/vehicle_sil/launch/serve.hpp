// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch serve: one launcher process for one configuration file that runs one run after
// the other, each as `run` would (launch::run_file()), each with its own run ID, stop requests,
// controls and window of the events, and nothing of a run kept for the next. Internal; the
// commands, the events and the exit codes: docs/simulation-run.md and docs/simulation-events.md.
//
// The two lifetimes.
//   The launcher (the process): its instance (a random 64-bit number, 16 hexadecimal digits), the
//   one stream of the events, the reader of standard input, the launcher's end (LauncherEnd, which
//   a signal handler may touch) and the frame of the current run (Serve, under its mutex).
//   A run (from the run command that is taken to the "idle" after its result): its ID
//   ("<instance>-<n>", n from 1), its StopRequests, its RunControls, its window of the events
//   (EventStream::open_run()) and all that run_file() makes (the registry, the controller, the
//   participants, the control server, the scenario).
//
// The threads.
//   The reader of standard input calls Serve::command() and, at the end of the input,
//   Serve::end_of_input(). It installs a run (below) and hands the stop and time commands to the
//   current run, matching the run ID and applying the command under the frame's mutex, so the run
//   cannot change in between. It never runs a run.
//   The main thread calls Serve::run(): it waits for a run, runs it (only the main thread ever calls
//   run_file(): on Linux the participants end when the thread that started them ends) and writes
//   its result and the idle that follows.
//   The watch of the events calls Serve::output_closed() when the reader of the events has gone.
//   A signal handler (POSIX) or a console handler (Windows) calls LauncherEnd::request() only.
//
// Installing a run (Serve::command() for a run command, under the frame's mutex, at once): the run
// ID is given, the request is read (the request file, prepare_serve_run()), the run's window opens
// (its run_start is queued), its StopRequests and RunControls are made, the run becomes the current
// run, its StopRequests are published to the signal handler (LauncherEnd::publish()) and it is handed
// to the main thread. The main thread then reads the launcher's end again (seq_cst) before it runs
// the run, and asks the run's StopRequests to stop if the end has been asked for: a signal that came
// while the run was installed reaches it one way or the other, and no run starts without the stop
// request that is pending.
//
// Ending a run (Serve::run(), when the run function has returned): the run stops being the current
// run (under the mutex: a command for it is refused with run-changed from now on), its StopRequests
// are withdrawn from the signal handler (LauncherEnd::withdraw(), which waits until no handler is
// inside), its result goes through its window (which closes: nothing of the run follows its result),
// and then, unless the launcher ends, everything of the run is let go and "idle" is written.
//
// Contamination: a run whose own connection to SIL Kit did not close (OutcomeClose::not_closed_ms) or
// whose run function threw has left the process in a state that is not known. Its result says
// "launcher_ends":true and the launcher ends with kExitContaminated (at once: a thread may still be
// closing). Nothing is run again: the caller starts a new launcher for the next run.
#pragma once

#include "vehicle_sil/launch/command_input.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/runner/stop_requests.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

/// The exit code of serve when a run left the launcher in a state that is not known (its own
/// connection to SIL Kit did not close, or the run failed inside the launcher): the run's result
/// says so ("launcher_ends":true), and a new launcher takes the next run.
inline constexpr int kExitContaminated = 6;

/// The reasons of the commands that serve refuses itself (the event "command" without "run").
namespace serve_reason {
/// A command other than run without "run".
inline constexpr const char* kRunRequired = "run-required";
/// A command for a run that is not the current one (it has ended, or never was of this launcher).
inline constexpr const char* kRunChanged = "run-changed";
/// A run command while a run is under way (until its idle): nothing is queued.
inline constexpr const char* kBusy = "busy";
/// A run command after the launcher has been asked to end.
inline constexpr const char* kLauncherEnding = "launcher-ending";
} // namespace serve_reason

/// A new launcher instance: 16 lowercase hexadecimal digits of a random 64-bit number. A run ID is
/// "<instance>-<n>" (n from 1): an opaque name that only tells one run from another. It is no
/// credential (a Control API client's Ping tells it to anyone).
[[nodiscard]] std::string new_launcher_instance();

/// The end of the launcher that has been asked for, and the path from a signal handler to the
/// current run's stop requests. Made once for the process (constinit), never destroyed.
///
/// request(), mark(), requested(), source() and count() touch only lock-free atomics, so a signal
/// handler may call request(). request() asks the end and then, if a run's StopRequests are
/// published, asks them too (the same source and count). The publisher (publish()) stores the
/// pointer and then reads requested(); all of it is seq_cst, so of a handler that is inside while a
/// run is published, either the handler sees the run or the publisher's reader sees the end.
/// withdraw() takes the pointer back and waits until no handler is inside (an in/out counter): the
/// StopRequests may be destroyed when it returns.
class LauncherEnd
{
public:
    constexpr LauncherEnd() noexcept = default;

    /// A signal or a console event (`source`: its number, or its code + 1): the end, and the
    /// current run's stop requests `count` times (1 as one Ctrl+C, 2 to skip the waits).
    void request(int source, int count = 1) noexcept
    {
        mark(source, count);
        inside_.fetch_add(1);
        if (runner::StopRequests* const run = run_.load())
        {
            run->request(source, count);
        }
        inside_.fetch_sub(1);
    }
    /// The end only: the caller asks the run itself (the end of input, a closed output).
    void mark(int source, int count = 1) noexcept
    {
        int none = 0;
        first_.compare_exchange_strong(none, source);
        count_.fetch_add(count);
    }
    [[nodiscard]] bool requested() const noexcept { return count_.load() > 0; }
    /// The source of the first request; 0 if none.
    [[nodiscard]] int source() const noexcept { return first_.load(); }
    [[nodiscard]] int count() const noexcept { return count_.load(); }

    /// The current run's stop requests, for request(); null: none.
    void publish(runner::StopRequests* run) noexcept { run_.store(run); }
    /// No run's stop requests any more; returns when no request() is inside.
    void withdraw() noexcept;

private:
    static_assert(std::atomic<runner::StopRequests*>::is_always_lock_free, "a signal handler needs lock-free atomics");
    static_assert(std::atomic<int>::is_always_lock_free, "a signal handler needs lock-free atomics");

    std::atomic<int> count_{0};
    std::atomic<int> first_{0};
    std::atomic<runner::StopRequests*> run_{nullptr};
    std::atomic<int> inside_{0};
};

/// What a run of serve is asked to do, as read from its run command.
struct PreparedRun
{
    RunRequest request{};
    /// What is wrong with the run command (its request file, a rule of validate_request()): the
    /// run starts nothing, and its result is "usage". Empty: none.
    std::vector<config::Finding> usage{};
};

/// The request of the run `run_id` of serve: `base` (the launcher's: the configuration file, the
/// limits, the control server, the log directory, standard input's commands) with the run command's
/// request file laid over it (as run --request does, the request file's fields only), its timeline
/// file, the log directory `<base log_dir>/<run_id>` and the run ID of the control server (required:
/// a client must name the run). The rules of validate_request() for a run.
[[nodiscard]] PreparedRun prepare_serve_run(const RunRequest& base, const ServeCommand& command, const std::string& run_id);

/// How a run of serve is run: launch::run_file() unless a test gives another (it gets the run's
/// request, observers and controls and window exactly as run_file() would).
using ServeRunFunction = std::function<RunReport(const RunRequest& request, const runner::RunObservers& observers, const RunLent& lent)>;

struct ServeOptions
{
    /// The launcher's request, for prepare_serve_run(): RunHost (config, limits, control, log_dir as
    /// the base directory, stdin_commands true) and no intent.
    RunRequest base{};
    /// new_launcher_instance(), as hello says it.
    std::string instance{};
    /// Empty: run_file().
    ServeRunFunction run{};
    /// Empty: prepare_serve_run(). Called by the reader of standard input under the frame's mutex.
    std::function<PreparedRun(const RunRequest& base, const ServeCommand& command, const std::string& run_id)> prepare{};
    /// The name of a stop request's source as the result's interrupted_by says it (a signal's name,
    /// a console event's); empty: runner::stop_source_name(), "interrupt" for any other.
    std::function<std::string(int source)> source_name{};
    /// How long run() waits for the events to be written when the launcher ends while idle.
    std::chrono::milliseconds flush_limit{5000};
};

/// How the launcher ends (Serve::run()).
struct ServeEnd
{
    int code = 0;
    /// End the process at once, without destructors: a thread of the run may still be closing.
    bool at_once = false;
};

/// The frame of the current run and the loop that runs one run after the other (see above).
class Serve
{
public:
    /// `events` and `end` outlive this object.
    Serve(EventStream& events, LauncherEnd& end, ServeOptions options);
    ~Serve();
    Serve(const Serve&) = delete;
    Serve& operator=(const Serve&) = delete;

    /// A line of standard input (the reader's thread): a run command installs a run if the launcher
    /// is idle (else busy, or launcher-ending); any other command needs "run" and goes to that run's
    /// controls if it is the current run (else run-required, run-changed); a line that is no command
    /// is refused. Every refusal is the launcher's own event "command".
    void command(const ServeCommand& line);
    /// Standard input has ended: the launcher ends; the current run, if any, is stopped once
    /// (stop_source::kEndOfInput, with its "command" event of source "eof").
    void end_of_input();
    /// The reader of the events has gone (EventStream::OnClosed): as end_of_input(), source
    /// stop_source::kOutputClosed.
    void output_closed();
    /// The main thread: writes "idle" and runs every run handed to it until the launcher ends.
    /// 0 when standard input ended while no run was under way; kExitInterrupted when a signal, the
    /// end of input during a run or a closed output ended it; kExitContaminated (at_once) after a
    /// contaminated run that no end request came before.
    ServeEnd run();

private:
    struct Run;
    void refuse(const runner::Command& command, const char* reason, std::string message,
                const std::optional<std::string>& asked_run);
    void install(const ServeCommand& line); ///< under mutex_
    void supervisor_end(int source);
    void flush();

    EventStream& events_;
    LauncherEnd& end_;
    ServeOptions options_;

    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t seq_ = 0;           ///< the number of the last run installed
    bool busy_ = false;               ///< from a run's install to its idle
    bool ending_ = false;             ///< the launcher ends: no run is taken any more
    bool input_ended_in_run_ = false; ///< standard input ended while a run was the current one
    Run* current_ = nullptr;          ///< the run commands go to; null outside a run
    std::unique_ptr<Run> handed_;     ///< installed, not yet taken by run()
    std::unique_ptr<Run> contaminated_run_; ///< kept alive after contamination (avoiding raw release for sanitizers)
};

} // namespace vehicle_sil::launch
