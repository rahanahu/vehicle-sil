// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: checks the configuration (checked.hpp), then starts the registry and the
// controller in this process and every participant as a child process, watches the simulation
// until it ends, ends every child and writes the result for people and as JSON. What the run
// saw (RunOutcome, run_outcome.hpp) is judged by a pure function (assess_run()). Internal; the
// behaviour and the output: docs/simulation-run.md.
#pragma once

#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run_controls.hpp"
#include "vehicle_sil/launch/run_events.hpp"
#include "vehicle_sil/launch/run_outcome.hpp"
#include "vehicle_sil/launch/run_plan.hpp"
#include "vehicle_sil/launch/stop_requests.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::control {
class Hub;
} // namespace vehicle_sil::control

namespace vehicle_sil::launch {

class EventStream;
struct Command;

/// The version of the JSON that format_run_json() writes (its "vehicle_sil_run" key).
inline constexpr int kRunFormat = 2;

/// The exit code of a run that was interrupted (a signal, a console event, or the program that ran
/// the launcher went away: its input ended or its output closed): 128 + SIGINT, as shells report
/// a program that Ctrl+C ended (the same as an ECU executable's). Not of a run that a command
/// stopped: that is 0, or kExitIncomplete.
inline constexpr int kExitInterrupted = ecu_executable::kExitInterrupted;

/// The exit code of a run whose scenario a command stopped before its end, when nothing else was
/// wrong: the scenario has no verdict.
inline constexpr int kExitIncomplete = 5;

enum class RunResult
{
    Passed,       ///< the run reached its end without errors (or, --strict, warnings)
    Stopped,      ///< an operator stopped the run, and nothing was wrong (no scenario, or it had passed)
    Incomplete,   ///< an operator stopped the run before its scenario's end, and nothing else was wrong
    Failed,       ///< the run found errors (or, --strict, warnings), or could not start or go on
    Interrupted,  ///< a signal, or the program that ran the launcher went away
    CheckFailed,  ///< the check found errors (or, --strict, warnings): nothing was started
    Unreadable,   ///< the configuration cannot be read: nothing was started
    SetupFailed,  ///< the registry or the controller could not be set up: nothing was started
    Usage,        ///< the command line was wrong
};

/// The result of `vehicle_sil_launch run`.
struct RunReport
{
    Checked check{};
    /// Empty if nothing was started because of the check (or the command line).
    std::optional<RunOutcome> run{};
    /// assess_run() of `run`.
    std::vector<Finding> findings{};
    bool strict = false;
    bool usage = false;
    /// run --control: the run's control server, ended (no call of a client is open, none comes
    /// any more; its audit log is complete), for the caller to let go: empty it, or let the report
    /// go, after the result is written. Its destruction may take the shutdown of gRPC itself,
    /// which must not keep the result back. Nothing else is to be done with it. Empty: the run
    /// made no server.
    std::shared_ptr<void> ended_control_server{};

    /// The check's and the run's together.
    [[nodiscard]] std::size_t errors() const;
    [[nodiscard]] std::size_t warnings() const;
    [[nodiscard]] std::size_t notes() const;
    [[nodiscard]] RunResult result() const;
    /// 0 passed or stopped; 1 failed or the check failed; 3 unreadable or the set-up failed;
    /// 4 usage; kExitIncomplete; kExitInterrupted.
    [[nodiscard]] int exit_code() const;
    /// The result counts as a success: exit_code() is 0.
    [[nodiscard]] bool ok() const;
    /// One sentence for people that says what the result is (English, one line, never empty);
    /// for a failure, with the first error.
    [[nodiscard]] std::string message() const;
};

/// The report for people: the check's tables and findings, then the run (how far the simulation
/// got, a table of the participants and how each ended, the run's findings), then one line with
/// the result and the counts of both. LF line ends; no time, duration or process ID in it.
[[nodiscard]] std::string format_run_text(const RunReport& report);

/// The report as JSON (format kRunFormat), with the check's JSON as "check".
[[nodiscard]] std::string format_run_json(const RunReport& report);

/// The members of the event "result" (EventStream::result()): the same object as the member
/// "result" of a compact JSON object.
[[nodiscard]] std::string format_result_event(const RunReport& report);

/// The JSON of `vehicle_sil_launch <command> --json` when the launcher itself failed unexpectedly
/// (`message`): the form of the command's result (`run`: "vehicle_sil_run"; else the check's),
/// result "unreadable", one finding with the code "internal".
[[nodiscard]] std::string format_internal_error_json(bool run, const std::string& message);


/// run --control: the control server of one run, as the run sees it. run_file() makes the one
/// that the request asks for (RunHost::control), starts it and ends it, and gives it back for the
/// caller to let go after the result (RunReport::ended_control_server). A caller may lend its own
/// instead (RunLent::control_endpoint).
class ControlEndpoint
{
public:
    virtual ~ControlEndpoint() = default;
    /// Starts listening on `hub`, the run's (never null), and keeps it for as long as the server
    /// uses it (as RunObservers::before_start says of the hub); says where it listens. Throws
    /// std::exception if it cannot listen.
    virtual ControlServerInfo start(std::shared_ptr<control::Hub> hub) = 0;
    /// Ends every call (the waits and watches return); idempotent.
    virtual void end() = 0;
};


/// What a run borrows of its caller: all of it outlives run_file().
///
/// run --control (RunHost::control): run_file() makes the control server of the run before it
/// checks anything, from what the request names (the address, the files of TLS, the credentials,
/// the audit log). A request that is wrong there is a usage error: CheckReport::usage with one
/// finding, and nothing is checked, told or started. The server's time operations go to the time
/// commands of `controls`, which must be set (std::invalid_argument otherwise), and a client's
/// Stop to their stop requests. run_file() starts the server on the run's hub
/// (ControlEndpoint::start()) once, on the main thread, when the check has passed and the run's
/// files are open, before the registry starts; the run then writes the event "control_server" and
/// the line "control server at <address>". If the start throws, nothing is started
/// (OutcomeSetup::error). With RunIntent::start_paused, the run holds before the first step
/// (TimeCommands::hold_before_start()). When run_file() returns or throws, the server has ended
/// (every call of a client has returned, and its audit log is complete); the report holds what is
/// left of it (RunReport::ended_control_server).
struct RunLent
{
    /// The run's controls (run_controls.hpp), the caller's: the stop requests the run reads (as
    /// `stops` below says), the time commands it attaches its controller to (--stdin-commands, a
    /// control server) and the phase it sets. For this one run: controls that a run has used
    /// take no time command any more. With `events`, the caller gives the controls the
    /// observers that write the command and phase events (the run writes neither by itself then).
    /// Null: the run makes its own for this run, on `stops`; no one outside reaches its time
    /// commands, and the run writes its phases to `events`. With `events` the controls
    /// must have both observers, `stops` must be null or the controls' own, and the controls must
    /// be made with the request's RunIntent::start_paused and whether it has a scenario
    /// (RunControls::start_paused(), scenario()): run_file() throws std::invalid_argument otherwise.
    RunControls* controls = nullptr;
    /// Without `controls`: the requests to stop (the caller's: its signal handler and whatever
    /// else asks). The run reads their count: 1 stops the simulation in order, 2 or more skip the
    /// waits for the participants to end. May be null. Not read with `controls`, which has its own
    /// (RunControls::stops()).
    StopRequests* stops = nullptr;
    /// A control server of the caller's own, in place of the one that run_file() makes for
    /// RunHost::control (which is not read then); null: none. The launcher's tests stand in for the
    /// server with it. run_file() starts it as it starts its own; it must stay usable while
    /// run_file() runs, and the caller ends it after run_file().
    ControlEndpoint* control_endpoint = nullptr;
    /// run --events: what happens, as events (events.hpp) instead of RunObservers::write_line():
    /// the phases, the check, the participants, the lines, the run's findings, the virtual time.
    /// run_file() writes every event but hello and the result (the caller writes them). No call
    /// waits for the reader of the events. May be null. run_file() calls the stream itself (the
    /// check, the phases, the state); the parts of the run are given it as the interface they know
    /// (RunEvents, run_events.hpp).
    EventStream* events = nullptr;
};


/// Runs what `request` asks for: check_file() of its file; if it passed, the simulation; with
/// RunIntent::scenario, the scenario in it (docs/scenario.md; the run then ends when the scenario
/// is over). Never throws for a problem of the file, an executable or a participant: they are
/// findings. Must be called from the thread that lives longest (the main thread): on Linux, the
/// participants end when it ends (process::Process).
///
/// Of `request` the run reads what the check reads, and of the rest: RunIntent::speed (replaces
/// the configuration's speed; the inner empty: "max"; with the time lock the run goes at speed 1
/// whatever it says, planned_speed(), and its check refuses another speed), RunIntent::start_paused
/// (the simulation stands still at 0 until a resume or step command; with the time lock the check
/// refuses it), RunHost::log_dir (created if needed), RunHost::timeline (with a scenario: the
/// timeline file of the run, timeline.hpp; its directory is created if needed; the file is opened
/// before anything starts, OutcomeSetup::error if that fails, and written when the run is over,
/// whatever became of the scenario) and RunHost::timeout (then the run is aborted:
/// OutcomeEnd::timed_out). Whether a control server and the commands of standard input were asked
/// for (RunHost::control, stdin_commands) is the caller's to act on: the run has the server it is
/// lent (`lent`). The request is read before anything starts and not kept: nothing reads it after
/// run_file() has returned, also not a closing thread that was left (OutcomeClose::not_closed_ms).
///
/// `check`: of it the run reads `params_for_many` and `fastest_by_default` (a test's runs); a run
/// never reads the file only.
[[nodiscard]] RunReport run_file(const RunRequest& request, const RunObservers& observers = {}, const RunLent& lent = {},
                                 const RunLimits& limits = {}, const CheckOptions& check = {});


} // namespace vehicle_sil::launch
