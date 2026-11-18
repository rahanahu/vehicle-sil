// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: checks the configuration (check.hpp), then starts the registry and the
// controller in this process and every participant as a child process, watches the simulation
// until it ends, ends every child and writes the result for people and as JSON. What the run
// saw (RunOutcome) is judged by a pure function (assess_run()). Internal; the behaviour and the
// output: docs/simulation-run.md.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/run_controls.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/scenario_run.hpp"
#include "vehicle_sil/launch/stop_requests.hpp"
#include "vehicle_sil/launch/time_commands.hpp"
#include "vehicle_sil/launch/time_plan.hpp"
#include "vehicle_sil/launch/timeline.hpp"
#include "vehicle_sil/launch/traffic.hpp"
#include "vehicle_sil/process/process.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil {
class SimulationController;
namespace control {
class Hub;
} // namespace control
} // namespace vehicle_sil

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

/// How the launcher asks a participant to stop, in messages: "SIGTERM" (POSIX) or "Ctrl+Break"
/// (Windows).
[[nodiscard]] std::string_view stop_request_name();

/// How far the simulation got.
enum class SimulationEnd
{
    NotStarted,    ///< it never ran
    ReachedStopAt, ///< it ran to stop_at_ms and shut down
    Ended,         ///< it ran and ended otherwise (a participant stopped it, or the launcher when the scenario was over)
    Aborted,       ///< it ran and a participant failed it: the launcher aborted it
    Interrupted,   ///< it ran and the launcher was interrupted: it stopped it
};

/// How and when a participant's process ended.
enum class Ending
{
    NotStarted,        ///< it could not be started (or the run ended before its turn, or it is disabled)
    ExitedBeforeStart, ///< it ended by itself before the simulation started
    Exited,            ///< it ended by itself while the simulation ran, or after it ended
    Stopped,           ///< it ended after the launcher asked it to stop
    Killed,            ///< the launcher ended it (with the processes it started)
};

/// Where a participant's control agent stood when the simulation started: whether the launcher's
/// hub can read and write its variables in this run (vehicle_sil/control_silkit/hub_transport.hpp).
enum class ControlLink
{
    None,            ///< nothing to connect: no variables (or describe: false, or not started), and no hello
    Connected,       ///< its agent said hello and receives the launcher's commands: its variables are the hub's
    NoHello,         ///< it describes variables and offers them (--describe's "control"), but its agent did not say hello before the start
    VersionMismatch, ///< it offers them in another version of the messages (ParticipantRun::control_version), by --describe or by its hello
    NotOffered,      ///< it describes variables and does not offer them (--no-control, an executable of an earlier vehicle-sil)
};

/// The name of a ControlLink in the result and in the events ("none", "connected", "no_hello",
/// "version_mismatch", "not_offered").
[[nodiscard]] const char* control_link_name(ControlLink link);

/// What happened to one participant (in the order of the configuration).
struct ParticipantRun
{
    std::string name;
    bool required = true;
    bool describe = true;
    /// false: disabled (enabled: false, or --disable); it was not started (Ending::NotStarted).
    bool enabled = true;
    /// true: someone starts it (manual), not the launcher; it has no process here (Ending::NotStarted).
    bool manual = false;
    /// manual: it joined the simulation (the controller saw its lifecycle).
    bool manual_joined = false;
    /// The bridge that run started for --connect: the network it connects (the network of
    /// RunMember::outside); "" for a participant of the file. If it ends before the simulation is over,
    /// the run fails (connect-failed before the start, connect-lost after it).
    std::string for_network{};
    /// for_network: its last line that says why it failed (one that starts with "error:" or "FAIL"
    /// after its tag; detail::LastLine::failure()), once its output is read to the end; "" before,
    /// and if it wrote none (it was killed, say).
    std::string last_line{};
    /// for_network: its process had ended by itself when the watch found the simulation over (not
    /// aborted, not interrupted), before the launcher asked it to stop: the bus was not connected
    /// for the last moments of the run (connect-lost).
    bool ended_with_run = false;
    std::size_t line = 0; ///< its entry in the configuration file (a bridge's: its network's)
    /// Why it could not be started (Ending::NotStarted), or "".
    std::string start_error{};
    Ending ending = Ending::NotStarted;
    /// ExitedBeforeStart, Exited and Stopped: how. Empty for Killed and NotStarted.
    std::optional<process::ExitStatus> exit{};
    /// The start waited in vain for it to join (it had not connected to the registry).
    bool missing_at_start = false;
    /// It was still running stop_timeout_ms after the simulation ended, so the launcher stopped it
    /// (not when an interrupt cut the wait short).
    bool stopped_after_end = false;
    /// The launcher could not ask it to stop (Windows: no console) and ended it at once.
    bool stop_request_failed = false;
    /// Processes it started were still running after it had ended; the launcher ended them.
    bool left_processes = false;
    /// Its log file (--log-dir) as shown, or "".
    std::string log{};
    ControlLink control = ControlLink::None;
    /// Connected: how many variables its agent offers (with those the ECU's run adds itself).
    std::size_t control_variables = 0;
    /// Connected: the mappings of its entry's variables that do not fit what its agent announced
    /// (control::PeerInfo::map_problems); they were not applied.
    std::vector<std::string> variable_map_problems{};
    /// NoHello: its hello did arrive, but its agent did not answer that it receives commands.
    bool control_hello_seen = false;
    /// VersionMismatch: the version of the messages its agent speaks.
    int control_version = 0;
    /// VersionMismatch: --describe said so (the check has the finding).
    bool control_version_described = false;
    /// Messages of its agent's steps that reached the launcher after the launcher's controller
    /// had begun a later step (HubTransportStats::late_messages): 0 for an agent that publishes
    /// within its steps.
    std::uint64_t control_late_messages = 0;
};

/// What the launcher read of the state of a bridge of --connect while it ran and what it made of it
/// (RunOutside::link; detail::OutsideState): the last statistics line's counts, how long the link
/// or the bus was down, and why the launcher stopped the run for it. One member of RunOutside, so
/// that the fields of what is read from the bridge stay together.
struct OutsideLink
{
    /// Every key=N of the last statistics line, with the bridge's own keys; empty: none was read.
    std::map<std::string, std::uint64_t> counts{};
    /// "link=down" of the link line: how many times, and how long in all (a down that lasted to the
    /// end counts to the end).
    std::uint32_t down_count = 0;
    std::chrono::milliseconds down_total{};
    /// "bus=bus-off", the same.
    std::uint32_t bus_off_count = 0;
    std::chrono::milliseconds bus_off_total{};
    /// How long "bus=passive" lasted in all.
    std::chrono::milliseconds passive_total{};
    /// Why the launcher stopped the run while the bridge itself went on ("link down for 30 s"), or
    /// empty (OutsideState::lost()).
    std::string lost{};
    /// "device=" of the link line: the device the bridge opened, as the bridge names it (the same
    /// for the same device whatever --connect said), or empty.
    std::string device{};
    /// Other vehicle-sil bridges on the device that are not this run's (detail::foreign_sharing()):
    /// the most there were at once (0: none was seen), the process ids seen (Linux; the bridge
    /// reports them), and when the first was seen, in milliseconds from the start of the run.
    std::uint32_t shared_most = 0;
    std::vector<long> shared_peers{};
    std::optional<std::chrono::milliseconds> shared_first{};
    /// Whether the launcher stopped the run for it, and when: before it started, or after the
    /// sharing had lasted kOutsideSharedAfter (--strict only).
    enum class SharedStop { None, AtStart, AfterDelay };
    SharedStop shared_stop = SharedStop::None;
};

/// A network that the run connected to a real bus (--connect; the result's "outside").
struct RunOutside
{
    std::string network;
    std::string target;      ///< as --connect gave it
    std::string participant; ///< the bridge's name (bridge.<network>)
    /// The frames that the bridge sent to the bus, and those that came from a device there (not
    /// from this computer), from the last statistics line of the bridge that the launcher could
    /// read (detail::read_outside_counts()); empty: none could be read.
    std::optional<std::uint64_t> to_outside{};
    std::optional<std::uint64_t> from_outside{};
    /// The bridge's tool (bridge_tool(), NetworkOutside::tool): which counts its line has. Not in
    /// the outputs.
    std::string tool{};
    /// What was read of its link line and its counts, and made of them (see OutsideLink).
    OutsideLink link{};
};

/// Whether `run` is a participant that stops the run when it ends: a bridge that run started for
/// --connect (ParticipantRun::for_network).
namespace detail {
[[nodiscard]] inline bool stops_run(const ParticipantRun& run) { return !run.for_network.empty(); }
} // namespace detail

/// What the run could not set up before it started anything: the registry or the controller.
struct OutcomeSetup
{
    /// The registry or the controller could not be set up (nothing was started): why.
    std::string error{};
    /// Where the registry of the run listens, as its participants get it (with the port it took
    /// for port 0), or "" if none was started.
    std::string registry_uri{};
};

/// How the start went: the wait for the participants, the start of the simulation, and the
/// limits it kept to.
struct OutcomeStart
{
    /// The required participants did not all join within timeout_ms.
    bool timed_out = false;
    /// They all joined, but the controller could not start the simulation: why.
    std::string error{};
    /// RunObservers::before_start threw: the simulation had started, and the launcher aborted it. Why.
    std::string before_start_error{};
    /// How long the start waited at most for the required participants (the configuration's
    /// start_timeout_ms; not the run's --timeout, which is OutcomeEnd::timeout_ms).
    std::int64_t timeout_ms = 0;
    /// How long the start waited at most for the hello of the control agents.
    std::int64_t control_hello_timeout_ms = 0;
    /// With --timeout-ms: how long the run stood still before its first step (the hold of
    /// --start-paused with a control server) until a command let it go on, if it did; --timeout-ms
    /// counts from the beginning of the hold.
    std::optional<std::int64_t> held_before_start_ms{};
    /// OutcomeEnd::timed_out before the simulation started: a run that waits for a participant
    /// someone starts by hand, whose --timeout-ms counts from when the launcher began to wait for
    /// the participants.
    bool timed_out_before_start = false;
    /// timed_out_before_start: what the start still waited for ("Late (manual) has not joined", ...).
    std::string timed_out_waiting_for{};
    /// --timeout-ms counted from when the launcher began to wait for the participants (a run that
    /// waits for a participant someone starts by hand), not from the start.
    bool timeout_from_wait = false;
};

/// How the simulation ended and why: the part that tells the run's end (what ended it, who
/// failed, which limit passed).
struct OutcomeEnd
{
    SimulationEnd simulation = SimulationEnd::NotStarted;
    /// The simulation ended in this state instead of shutting down normally (SIL Kit's name), or "".
    std::string controller_state{};
    /// What asked first to stop the run ("SIGINT", "Ctrl+C", "stop command", "end of input", ...),
    /// or "" if nothing did.
    std::string interrupted_by{};
    /// The kind of what asked first (stop_kind() of the first stop request's source); set with
    /// interrupted_by. It decides between EndReason::Stopped and EndReason::Interrupted.
    std::optional<StopKind> interrupted_kind{};
    /// What asked, in words for people, by its kind: "by a stop request" (an operator), "because
    /// its input closed" or "because its output closed" (the supervisor went away), else "by a
    /// signal". What exactly asked is interrupted_by, for programs.
    [[nodiscard]] std::string stop_phrase() const
    {
        return interrupted_kind == StopKind::Operator ? "by a stop request"
               : interrupted_kind != StopKind::Supervisor ? "by a signal"
               : interrupted_by == stop_source_name(stop_source::kEndOfInput) ? "because its input closed"
                                                                              : "because its output closed";
    }
    /// How the launcher's lines and findings name it: "stopped by a stop request" for an operator,
    /// "interrupted (SIGINT)" for anything else.
    [[nodiscard]] std::string stop_text() const
    {
        return interrupted_kind == StopKind::Operator ? "stopped " + stop_phrase() : "interrupted (" + interrupted_by + ")";
    }
    /// The interrupt came after the simulation had ended, while the launcher waited for the
    /// participants to end.
    bool interrupted_while_ending = false;
    /// The simulation did not end within --timeout-ms of wall-clock time: the launcher aborted it.
    bool timed_out = false;
    /// --timeout-ms, if given.
    std::optional<std::int64_t> timeout_ms{};
    /// timed_out: --timeout-ms passed while the run stood still before the first step (the hold of
    /// --start-paused with a control server); the simulation was aborted right after it started.
    bool timed_out_while_holding = false;
    /// The participant whose failure ended the run (it ended, or the controller saw it leave), or "".
    std::string failed_participant{};
    /// Why the controller saw it fail (FailureCheck, without the name), or "" if the launcher saw
    /// its process end.
    std::string failure_reason{};
    std::int64_t stop_timeout_ms = 0;
    std::optional<std::int64_t> stop_at_ms{};
    /// The scenario was over (passed or failed) and the launcher stopped the simulation for it,
    /// at ScenarioRun::over_at (SimulationEnd::Ended).
    bool stopped_by_scenario = false;
};

/// What the launcher's end of the control agents dropped in the run (HubTransportStats): hellos it
/// did not take, messages it could not decode, and messages that fit no agent it knows or no schema.
struct OutcomeControl
{
    std::uint64_t refused_hellos = 0;
    std::uint64_t undecodable = 0;
    std::uint64_t unknown = 0;
};

/// run --scenario and --timeline: what became of the scenario and of its timeline file.
struct OutcomeScenario
{
    /// What became of the scenario (ScenarioStatus::NotRun if the simulation did not start).
    std::optional<ScenarioRun> result{};
    /// The scenario's thread failed unexpectedly: why (the scenario is Incomplete).
    std::string error{};
    /// The timeline file as the user named it (RunHost::timeline), once it was opened; ""
    /// without one.
    std::string timeline{};
    /// What the scenario's engine noted for the timeline; null if the scenario did not begin.
    std::shared_ptr<const TimelineRecord> timeline_record{};
    /// The timeline file could be opened, but not written when the run was over: why (finding
    /// "timeline-write"; the scenario's result stands). (A record that dropped points at its
    /// limits is the note "timeline-limit".)
    std::string timeline_error{};
};

/// How the launcher's own SIL Kit connection closed after the participants had ended.
struct OutcomeClose
{
    /// Set if the launcher's own SIL Kit connection (the hub's transport, the controller, the
    /// registry) had not closed when the launcher stopped waiting for it, after the participants
    /// had ended: how long it waited, in milliseconds (the limit: the controller's shutdown
    /// timeout and stop_timeout_ms, at least 5000 ms for the latter). A thread of the run is left
    /// closing it; it keeps the three and the run's hub, and uses nothing else of the run or of
    /// what the caller gave the run (its request, its observers): both may be destroyed. The result is complete.
    /// The caller ends the process without running the destructors of static objects (std::_Exit();
    /// Windows: TerminateProcess()) when it has written the result: the thread is inside SIL Kit.
    std::optional<std::int64_t> not_closed_ms{};
    /// not_closed_ms: one more interrupt ended the wait before its limit (the person asked for the
    /// end at once: no finding). false: the limit passed (finding "connection-not-closed").
    bool interrupted = false;
};

/// What a run saw, in parts: what was set up, how it started, how it ended, what the control
/// agents dropped, what became of the scenario, and how the connection closed; then what is not
/// part of one of them.
struct RunOutcome
{
    OutcomeSetup setup{};
    OutcomeStart start{};
    OutcomeEnd end{};
    OutcomeControl control{};
    OutcomeScenario scenario{};
    OutcomeClose close{};
    RunTiming timing{};
    /// In the order of the configuration, then the bridges that run started for --connect.
    std::vector<ParticipantRun> participants{};
    /// --connect: the networks connected to a real bus, in the order of the file's networks.
    std::vector<RunOutside> outside{};
    /// The first time command of the simulation that changed how its time goes (a pause, a step, a
    /// run_until, a speed other than 1), if any: TimeCommands::first_time_change() once the
    /// simulation is over. The notes outside-time-changed (a run that is connected outside, which
    /// accepts such a command only with --time-policy free) and unsynchronized-time-changed (a run
    /// with real_time_peers).
    std::optional<TimeChange> time_changed{};
    /// CheckReport::time_lock of the run: whether it kept to real time, and what connected it
    /// outside the simulation.
    TimeLock time_lock{};
    /// The participants of the file that ran in real time (detail::real_time_peers()).
    std::vector<std::string> real_time_peers{};
    /// What went over the networks; empty if the simulation did not start.
    std::optional<RunTraffic> traffic{};
};

/// Judges a run: the errors (they fail it), warnings and notes, each about the participant (with
/// its line) or the simulation, in sort_findings() order.
[[nodiscard]] std::vector<Finding> assess_run(const RunOutcome& outcome);

/// Why a run ended, whatever was found in it.
enum class EndReason
{
    Completed,   ///< it reached an end that was set: stop_at_ms, a participant ended it, the scenario was over
    Stopped,     ///< an operator stopped it (the stop command, the control server's Stop)
    Interrupted, ///< a signal or a console event, or the program that ran the launcher went away
    Failed,      ///< it could not start or go on: a participant or a bridge failed, a time limit passed
    SetupFailed, ///< the registry or the controller could not be set up
};

/// Why `outcome` ended. Pure. The order: a set-up that failed; then what asked first to stop
/// (an operator: Stopped; anything else: Interrupted), whatever came after it; then how far the
/// simulation got. A run whose scenario was over is Completed.
[[nodiscard]] EndReason end_reason(const RunOutcome& outcome);

/// The participants that the launcher ended by force in a run that an operator stopped, without
/// having waited for them in full: the stop request was repeated, or came while the launcher
/// waited for them to end. They did not end when asked to (not those the launcher could not ask),
/// and are no error of such a run (the operator chose not to wait); the note "ended-by-force" and
/// the result's message name them. Empty for any other run.
[[nodiscard]] std::vector<std::string> ended_by_force(const RunOutcome& outcome);

/// "completed", "stopped", "interrupted", "failed" or "setup_failed": the member "end" of the result.
[[nodiscard]] const char* end_reason_name(EndReason reason) noexcept;

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

/// Where the run's control server listens (ControlEndpoint::start()).
struct ControlServerInfo
{
    /// host:port with the port it actually listens on (also for port 0).
    std::string address{};
    bool tls = false;
    /// Every call needs a token of the server's credentials.
    bool credentials = false;
};

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

/// How long a bridge of --connect may report the link or the bus down before the launcher stops the
/// run (RunLimits::outside_lost_after).
inline constexpr std::chrono::milliseconds kOutsideLostAfter{30000};
/// With --strict: how long a bridge of --connect may share its device with a bridge that is not of
/// this run before the launcher stops the run (OutsideState::shared_for()). A bridge that another
/// run started later and that ends before its ECUs start must not take this run down with it.
inline constexpr std::chrono::milliseconds kOutsideSharedAfter{3000};
/// With --strict: the longest the launcher waits, after the bridges run, for the first link line of
/// each, which says whether the device is shared and comes before the bridge starts its lifecycle.
/// A bridge that has not said by then is taken as one that shares nothing.
inline constexpr std::chrono::milliseconds kOutsideFirstLineWait{2000};

/// What a run tells while it lasts, and the one thing it lets its caller do: the caller's
/// functions, which the run calls back. A run keeps its own copy.
struct RunObservers
{
    /// Writes one line (without its end) for the person watching: the participants' output as
    /// "[<name>] <line>", the launcher's own as "[vehicle_sil_launch] <text>". Called from several
    /// threads, one call at a time. Not called when `events` is set.
    std::function<void(std::string_view line)> write_line{};
    /// run --events: what happens, as events (events.hpp) instead of write_line(): the phases, the
    /// check, the participants, the lines, the run's findings, the virtual time. run_file() writes
    /// every event but hello and the result (the caller writes them). No call waits for the
    /// reader of the events. May be null. Borrowed: it outlives run_file(). run_file() calls the
    /// stream itself (the check, the phases, the state); the parts of the run are given it as
    /// the interface they know (RunEvents, run_events.hpp) and do not read this member.
    EventStream* events = nullptr;
    /// The name of the first interrupt ("SIGINT"), for the result and, without RunLent::controls,
    /// for the state events (RunControls::set_stop_name()). Called from any thread that changes the
    /// run's state (the run's own, a control server's, the hold's), under the time commands' mutex,
    /// and by run_file() for the result: safe from several threads, short, no wait, no call into
    /// the run or its controls.
    std::function<std::string()> interrupt_name{};
    /// Called in the window before the start (SimulationController::set_start_window()), on a
    /// thread of the controller, after the wait for the agents' hellos: every participant waits
    /// before its first step, and a write that an agent accepts now (control::Hub::write(),
    /// wait_queued()) takes effect in its step at 0. The simulation starts when it returns. If it
    /// throws, the launcher aborts the simulation as soon as it has started
    /// (OutcomeStart::before_start_error). Not called if the start was given up before the window.
    /// `hub`: the run's hub (never null), which the ECUs' control agents connect to over SIL Kit,
    /// on the controller's participant. The run makes it. A caller that keeps the pointer may use
    /// the hub from any thread while the simulation runs and after run_file() has returned (it
    /// then holds what the agents said last).
    std::function<void(const std::shared_ptr<control::Hub>& hub)> before_start{};
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
    /// take no time command any more. With RunObservers::events, the caller gives the controls the
    /// observers that write the command and phase events (the run writes neither by itself then).
    /// Null: the run makes its own for this run, on `stops`; no one outside reaches its time
    /// commands, and the run writes its phases to RunObservers::events. With events the controls
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
};

/// The waits of a run that no user sets: only tests make them shorter.
struct RunLimits
{
    /// How often the launcher looks at the participants and the simulation.
    std::chrono::milliseconds poll{20};
    /// How long a bridge of --connect may report the link or the bus down (link=down, bus=bus-off)
    /// before the launcher stops the run as for a bridge that ended (OutsideState::lost()).
    std::chrono::milliseconds outside_lost_after = kOutsideLostAfter;
    /// How long the start waits for the hello of each required ECU that describes variables and
    /// offers them in the launcher's version of the messages (it is there before the wait begins,
    /// by the order in which SIL Kit sets the participants up: the limit is a last line).
    std::chrono::milliseconds control_hello_timeout{5000};
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

/// Where the registry listens if the start waits for a participant someone starts by hand and the
/// file has no registry_uri: a fixed port, so that the command to start it is the same in every
/// run (the port SIL Kit's own registry takes by default).
inline constexpr std::string_view kManualRegistryUri = "silkit://127.0.0.1:8500";

namespace detail {
/// The check that run_file() runs with: a copy of `check` (that passed) with one participant at the
/// end for each network that --connect connects (CheckReport::outside, in its order): the bridge
/// bridge.<network> (role: bridge, required: false, its network's line), with a
/// ParticipantResult that is NotChecked and has the bridge's executable. Without --connect the copy
/// is the same as `check`.
[[nodiscard]] CheckReport with_bridges(const CheckReport& check);

/// The --start-timeout-ms the launcher gives an ecu_main executable: longer than the launcher's own
/// wait for the start, so that the launcher, not the ECU, decides that the start failed and names
/// who did not join; the ECU's limit only ends it if the launcher is gone. kMaxMillis if the start
/// waits for a participant someone starts by hand (waits_for_manual()) or, with
/// `stands_still_before_start`, stands still before the first step until a command (--start-paused
/// with a control server): either may take any time, and the ECU is not Running until it is over.
/// The launcher's own limits (before and after the wait) still end a start that does not come.
[[nodiscard]] std::int64_t ecu_start_timeout_ms(const SimulationConfig& config, bool stands_still_before_start);

/// The arguments that run starts the participant with, after its executable; the command to start
/// it by hand (manual) and the events show the same ones. An ecu_main executable (describe:
/// true): its command's arguments, then --name, --registry-uri `registry_uri` and
/// --start-timeout-ms (ecu_start_timeout_ms() with `stands_still_before_start`), then
/// map_network_arguments() and j1939_node_arguments() of its entry. Any other: its command's
/// arguments with the placeholders replaced, then the same two (a --map-network or --j1939-node
/// after what the file wrote, never inside it).
[[nodiscard]] std::vector<std::string> participant_arguments(const SimulationConfig& config, const ParticipantConfig& participant,
                                                             const std::string& registry_uri,
                                                             bool stands_still_before_start);

/// One line for this system's shell that starts `executable` (as given) with `arguments`: on
/// Windows for cmd.exe, quoted as the C runtimes read a command line back (an argument in double
/// quotes if it has a space, a tab, a quote or one of &|<>^,;, for cmd.exe and PowerShell; %...% is
/// not protected), elsewhere as
/// POSIX sh reads it (an argument in single quotes unless it has only letters, digits and
/// "%+,-./:=@_").
[[nodiscard]] std::string shell_command_line(const std::string& executable, const std::vector<std::string>& arguments);

/// The findings of assess_run(`outcome`) that are settled while the participants are still being
/// ended: whatever happens to them from here on, assess_run() of the final outcome has each of
/// these unchanged. (Left out: a participant that failed while its process still runs, whose
/// finding says how the process ended.) What run --events writes before the participants end.
[[nodiscard]] std::vector<Finding> settled_findings(const RunOutcome& outcome);

/// The line of the text output about the time ("time: ..."), or "" if no speed was set and no
/// step was paced.
[[nodiscard]] std::string timing_text(const RunTiming& timing);

/// How a wait of the launcher ended.
enum class Waited
{
    Done,
    TimedOut,
    Interrupted,
};
/// Waits up to `timeout` until `done()`, looking every `poll`; stops waiting once `interrupted()`.
/// What has become done by the time the interrupt is seen counts as done: an interrupt that comes
/// just as the awaited thing happens does not change the result.
template <class Done, class Interrupted>
[[nodiscard]] Waited wait_for(std::chrono::milliseconds timeout, std::chrono::milliseconds poll, Done done,
                              Interrupted interrupted)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done())
    {
        if (interrupted())
        {
            return done() ? Waited::Done : Waited::Interrupted;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return Waited::TimedOut;
        }
        std::this_thread::sleep_for(poll);
    }
    return Waited::Done;
}

/// What the launcher learnt when it asked a running participant to stop.
enum class StopRequest
{
    Sent,        ///< the request went out: wait for the participant
    HadEnded,    ///< it had ended just before (recorded as Ending::Exited)
    CannotSend,  ///< it runs, but the request cannot be sent (Windows: no console): end it now
};
/// Records in `run` what `sent` (Process::request_stop()) and `status` (Process::poll() after it)
/// say.
StopRequest record_stop_request(ParticipantRun& run, bool sent, const std::optional<process::ExitStatus>& status);

/// The registry's URI as the participants on this computer connect to it: a registry that listens
/// on every address ("silkit://0.0.0.0:<port>", "silkit://[::]:<port>") is reached at the loopback
/// address; any other URI as it is.
[[nodiscard]] std::string uri_for_participants(std::string uri);
/// Tells, while the simulation runs, that its virtual time stands still although it is not paused:
/// a participant lost its network connection (nobody notices that by itself for minutes), stands
/// still in a debugger, or hangs in a step. look() once per look of the run's watch.
class StandStill
{
public:
    /// When it has stood still this long (wall-clock time), look() says so; each once.
    static constexpr std::int64_t kSayAfterMs[] = {10000, 60000, 600000};

    /// `now_ms`: the controller's virtual time; `paused`: it stands still on purpose (a pause, a
    /// step); `overdue`: at a set speed, how long the next step is past its deadline (it stands
    /// still between deadlines on purpose), empty as fast as possible; `wall`: the steady clock.
    /// The line to write, or "" (also once it moves again after a line: then it says so).
    [[nodiscard]] std::string look(std::int64_t now_ms, bool paused, std::optional<std::chrono::nanoseconds> overdue,
                                   std::chrono::steady_clock::time_point wall);
    /// The simulation is no longer running (it stops): only the line that it moved again, if it
    /// did after a line; from then on, nothing.
    [[nodiscard]] std::string stopping(std::int64_t now_ms, std::chrono::steady_clock::time_point wall);

private:
    std::int64_t last_ms_ = -1;
    std::chrono::steady_clock::time_point since_{};
    std::size_t said_ = 0;
    bool over_ = false; ///< stopping() was called: nothing more
};

/// Tells which participants outside the file wait for the simulation's start (a mistyped --name):
/// one in Error, or in ServicesCreated for kHold (an autonomous participant passes that state at
/// once on its way to Running). look() for each such participant at each look of the start's wait.
class StrangerWait
{
public:
    enum class State
    {
        ServicesCreated,
        Error,
        Other, ///< any other state, or none known
    };
    static constexpr std::chrono::milliseconds kHold{2000};

    /// Whether `name`, in `state` at `now`, waits for the start.
    [[nodiscard]] bool look(const std::string& name, State state, std::chrono::steady_clock::time_point now);

private:
    std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> since_;
};

/// "a.b.c.d" of the IPv4 address `bytes` (4 bytes, network order); "" for one that other computers
/// do not reach (0.0.0.0/8, loopback 127/8, link-local 169.254/16).
[[nodiscard]] std::string reachable_ipv4_text(const std::uint8_t* bytes);
/// "[...]" of the IPv6 address `bytes` (16 bytes); "" for one that other computers do not reach
/// (::, ::1, link-local fe80::/10, multicast, an IPv4 address written as IPv6).
[[nodiscard]] std::string reachable_ipv6_text(const std::uint8_t* bytes);
/// The names of the network interfaces with a default route, from the text of /proc/net/route and
/// /proc/net/ipv6_route (Linux; either may be empty): each once, in the order found.
[[nodiscard]] std::vector<std::string> default_route_interfaces(std::string_view ipv4_routes, std::string_view ipv6_routes);

/// An address of this computer that other computers may reach.
struct HostAddress
{
    std::string host;           ///< a.b.c.d, or [...] for IPv6
    bool default_route = false; ///< its interface has the default route (most likely the one to give)
};

/// This computer's addresses that other computers may reach: of the network interfaces that are up
/// and running (Windows: operational), without loopback and link-local addresses, each once;
/// those of an interface with a default route first (a virtual one of WSL, Docker or a VM rarely
/// has it), IPv4 before IPv6, otherwise in the system's order. Empty if there are none or they
/// cannot be read. `ipv4`, `ipv6`: which of them.
[[nodiscard]] std::vector<HostAddress> host_addresses(bool ipv4, bool ipv6);
/// `addresses` in the order host_addresses() gives: those with the default route first, IPv4 before
/// IPv6, otherwise as they are.
[[nodiscard]] std::vector<HostAddress> in_order(std::vector<HostAddress> addresses);
/// Whether a registry that listens on [::] here takes IPv4 connections too: Linux unless
/// net.ipv6.bindv6only is 1; not on Windows (IPV6_V6ONLY is set there by default, and SIL Kit keeps it).
[[nodiscard]] bool ipv6_any_takes_ipv4();
/// The line run writes after "registry at" when other computers may reach the registry: where a
/// participant on another computer joins. `uri` is where the registry listens; one on every address
/// ("silkit://0.0.0.0:<port>", "silkit://[::]:<port>") is reached at one of `addresses`
/// (host_addresses()), or, without any, at an address of this computer the line tells how to
/// find; any other URI as it is. `ipv6_only`: a registry on [::] takes no IPv4 (the line says
/// how to take it, with the port the file has: `written_port`, or the port of `uri` if empty).
[[nodiscard]] std::string other_computers_line(const std::string& uri, const std::vector<HostAddress>& addresses,
                                               bool ipv6_only = false, const std::string& written_port = {});
/// What run adds, in parentheses, to "the registry cannot listen at <listen>: <SIL Kit's message>":
/// the likely cause and what to write instead. SIL Kit's message names no cause when the system
/// refuses the address or the port (SIL Kit v5.0.8 only logs it), so the cause is found from
/// `listen`: an IPv4 address that is none of `addresses` (host_addresses()), 0.0.0.0 or loopback
/// is not this computer's; with port 0 no other program can have the port, so only the host can be
/// wrong; with a fixed port of this computer, another registry may have it. `written`: `listen` is
/// the file's registry_uri; `fixed`: it is the port of a run with a participant started by hand.
/// "" when there is nothing to add.
[[nodiscard]] std::string registry_listen_hint(const std::string& listen, bool written, bool fixed,
                                               const std::vector<HostAddress>& addresses);
/// What a bridge of --connect has counted: the frames it sent to the real bus, and those that came
/// from a device there (RunOutside::to_outside, from_outside).
struct OutsideCounts
{
    std::uint64_t to_outside = 0;
    std::uint64_t from_outside = 0;

    friend bool operator==(const OutsideCounts&, const OutsideCounts&) = default;
};

/// The counts in one line of the bridge `tool` (bridge_tool()), or empty. The line is a statistics
/// line of the bridge ("stats key=N key=N ...", after the tag the bridge puts first, such as
/// "[eth-bridge] "; a "\r" at its end does not count) with the keys of `tool`: silkit_to_ext and
/// ext_from_devices (the same for both bridges). Empty for any other line, for a line with a key twice, with a
/// value that is not a decimal number up to 2^63 - 1 or with a part of another form, for one without
/// either key (a bridge of another version), and for any other tool.
[[nodiscard]] std::optional<OutsideCounts> read_outside_counts(std::string_view tool, std::string_view line);

/// Whether `line` is the link line of a bridge ("link key=value ...", after the tag the bridge puts
/// first; see "Link line" in docs/bridge-contract.md). Run does not show these lines: they are for the
/// launcher to read, like the statistics lines.
[[nodiscard]] bool is_link_line(std::string_view line);

/// Every key=N of a statistics line of the bridge `tool`: what read_outside_counts() accepts of the
/// line (the same refusals), with all its keys instead of two. Empty for any other line.
[[nodiscard]] std::optional<std::map<std::string, std::uint64_t>> read_outside_stats(std::string_view tool,
                                                                                    std::string_view line);

/// The state of the link and of the bus that a link line says ("link link=down", "link bus=bus-off").
enum class OutsideLinkValue { Unknown, Up, Down };
enum class OutsideBusValue { Unknown, Active, Passive, BusOff };
/// The keys of a link line that this launcher knows; a key it does not know or a value it does not
/// know is left out: empty.
struct OutsideLinkLine
{
    std::optional<OutsideLinkValue> link{};
    std::optional<OutsideBusValue> bus{};
    /// device=<hash>, marked=0|1, shared=<n> and peers=<pid>,<pid> (Linux): the other
    /// vehicle-sil bridges on the same device (docs/bridge-contract.md, "Device in use by another bridge").
    /// `peers` is read with `shared` and is empty when the line has none.
    std::optional<std::string> device{};
    std::optional<bool> marked{};
    std::optional<std::uint32_t> shared{};
    std::vector<long> peers{};
};
/// The known keys of a link line, or empty for a line that is none (is_link_line()) or whose parts
/// are not "key=value". A part of another form or a repeated key is skipped.
[[nodiscard]] std::optional<OutsideLinkLine> read_outside_link_line(std::string_view line);

/// What the counts of a bridge say about the frames it lost. The one table of the keys of both
/// bridges (names they have in common are listed once); they are the bridges' own names and change
/// with them. Dropped: the queue was full, a send failed, a receive overran, a frame was refused. The others (own_tx_dropped, filtered_*,
/// mac_unguarded, oversize, malformed, ext_checksum_incomplete) are not losses and are only carried.
inline constexpr std::array<std::string_view, 9> kOutsideDroppedKeys{
    "ext_overruns", "ext_queue_full", "ext_tx_failures", "silkit_tx_failures", "overruns",
    "ext_tx_dropped", "ext_tx_unsent", "silkit_rejected", "ext_rejected"};
/// The keys of the two counts that read_outside_counts() reads (both bridges): the frames sent to the
/// bus, and those that came from a device there.
inline constexpr std::string_view kOutsideToKey = "silkit_to_ext";
inline constexpr std::string_view kOutsideFromKey = "ext_from_devices";
/// The bridge sent frames to the bus and none came from a device there: the condition of the note
/// connect-nothing-received (at the end of the run) and of `nothing_received` of the state (while it
/// runs, once kOutsideNothingReceivedAfter has passed since the first frame was sent). The one place
/// that says it; empty counts (none could be read) are not it.
[[nodiscard]] inline bool sent_without_answer(std::optional<std::uint64_t> to_outside, std::optional<std::uint64_t> from_outside)
{
    return to_outside && from_outside && *to_outside != 0 && *from_outside == 0;
}
/// How long after the first frame to the bus nothing from a device makes `nothing_received` true.
inline constexpr std::chrono::milliseconds kOutsideNothingReceivedAfter{10000};
/// How long a count of lost frames or of stopped loops that grew keeps the condition "degraded"
/// (reasons frames-dropped, loop-suspected), and a bus that went passive is not meant: that one
/// lasts as long as the bus is passive.
inline constexpr std::chrono::milliseconds kOutsideDegradedHold{5000};
/// The count of frames the bridge stopped as a loop (Ethernet).
inline constexpr std::string_view kOutsideLoopKey = "loop_suspected";
/// The count of error frames the CAN controller saw.
inline constexpr std::string_view kOutsideBusErrorsKey = "error_frames";
/// The keys of `counts` in kOutsideDroppedKeys whose count is not 0, with the counts, in
/// the order of the table.
[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> dropped_counts(const std::map<std::string, std::uint64_t>& counts);

/// Whether `line` is the warning that a bridge writes when its device is used by another vehicle-sil
/// bridge ("warning: <device> is also used by another vehicle-sil bridge (process N): ..."). Run
/// does not show it: the launcher tells it as the finding outside-device-shared, once the bridges
/// of this run are subtracted. One function for the form of the line (the bridges' own text,
/// bridges/common/src/device_mark.cpp).
[[nodiscard]] bool is_shared_warning(std::string_view line);

/// What a bridge's link line says of its device (the sharing keys), kept by OutsideState.
struct OutsideSharing
{
    bool seen = false; ///< a link line was read
    std::string device{};
    bool marked = false;
    std::uint32_t shared = 0;
    std::vector<long> peers{};
};
/// What the launcher knows of a bridge of its own, by the participant's index (the bridge's own
/// participant and every other index of the run): whether its process runs now, and its process id
/// (0: not known).
struct OutsideOwn
{
    bool alive = false;
    long pid = 0;
};
/// The bridges on the device of bridge `i` that are not this run's.
struct OutsideForeign
{
    std::uint32_t count = 0;
    std::vector<long> pids{}; ///< their process ids if the bridge reports them (Linux), else empty
};
/// Bridge `index`'s `shared`, less the bridges of this run. With `peers` (Linux), the ids of the
/// bridges this run started are taken from them (even those that have ended since: their mark may
/// still be in a report). Without (Windows), the number of the run's other bridges that are alive,
/// have placed a mark (marked) and report the same `device`. Nothing if a bridge of the run has not
/// reported yet and the count would need it. Pure; `all` and `own` have one entry per participant.
[[nodiscard]] OutsideForeign foreign_sharing(std::size_t index, const std::vector<OutsideSharing>& all,
                                             const std::vector<OutsideOwn>& own);
/// "another vehicle-sil bridge (process 4321)", "2 other vehicle-sil bridges (processes 4321, 4400)"
/// (Windows: no ids, "another vehicle-sil bridge").
[[nodiscard]] std::string shared_text(const OutsideForeign& foreign);

/// What one bridge's link line and counts add up to. Pure like OutsideNote: time is an argument, no
/// clock, no thread, no lock (RunOutput's mutex guards it).
class OutsideState
{
public:
    /// A link line read at `now`. true: link or bus changed (what people are told).
    bool on_link(const OutsideLinkLine& line, std::chrono::steady_clock::time_point now);
    /// The counts of a statistics line read at `now` (the last ones are kept). A sum of lost frames
    /// (kOutsideDroppedKeys) or a count of loops (kOutsideLoopKey) that is larger than before is
    /// noted with `now`, for the condition.
    void on_counts(std::map<std::string, std::uint64_t> counts, std::chrono::steady_clock::time_point now);
    /// What the bridge stands at `now`, for RunState::outside (the names are the caller's): the link,
    /// the bus, `nothing_received`, and the condition with its reasons (docs/simulation-events.md).
    /// interrupted: the link is down or the bus is off. degraded: the bus is passive, frames were
    /// lost or loops stopped within kOutsideDegradedHold, or a bridge outside this run shares the
    /// device (on_foreign()). Both: interrupted, with the reasons of both.
    [[nodiscard]] RunState::Outside facts(std::chrono::steady_clock::time_point now) const;
    /// The link as it is now: "up", "down", "unknown", or empty before any line.
    [[nodiscard]] std::string link_text() const;
    /// The bus as it is now, as link_text().
    [[nodiscard]] std::string bus_text() const;
    /// Why the run must stop at `now`: the link or the bus has been down for `limit` or more
    /// ("link down for 30 s", "bus off for 30 s"), else empty. The link comes first.
    [[nodiscard]] std::string lost(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit) const;
    /// The sums at `now` (a down or bus-off that is still on counts to `now`), with `lost` empty.
    [[nodiscard]] OutsideLink summary(std::chrono::steady_clock::time_point now) const;
    /// A link line has been read.
    [[nodiscard]] bool link_line_seen() const { return sharing_.seen; }
    /// What the link lines said of the device (foreign_sharing() takes it from every bridge).
    [[nodiscard]] const OutsideSharing& sharing() const { return sharing_; }
    /// The bridges outside this run on the device, as foreign_sharing() has them at `now` (`since_start`
    /// into the run). true: more than ever before, which people are told once.
    bool on_foreign(const OutsideForeign& foreign, std::chrono::steady_clock::time_point now,
                    std::chrono::milliseconds since_start);
    /// The device has been shared with a bridge outside this run for `limit` or more.
    [[nodiscard]] bool shared_for(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit) const;
    /// The run is stopped (or not started) for it; summary() says so.
    void stop_shared(OutsideLink::SharedStop how) { shared_stop_ = how; }

private:
    struct Span
    {
        std::uint32_t count = 0;
        std::chrono::steady_clock::duration total{};
        std::optional<std::chrono::steady_clock::time_point> since{}; ///< when the one that is on began
        void begin(std::chrono::steady_clock::time_point now);
        void end(std::chrono::steady_clock::time_point now);
        [[nodiscard]] std::chrono::steady_clock::duration total_at(std::chrono::steady_clock::time_point now) const;
    };
    std::optional<OutsideLinkValue> link_{};
    std::optional<OutsideBusValue> bus_{};
    Span down_;
    Span bus_off_;
    Span passive_;
    std::map<std::string, std::uint64_t> counts_;
    OutsideSharing sharing_;
    Span foreign_;
    std::uint64_t dropped_ = 0; ///< the sum of the lost-frame counts of the last statistics line
    std::optional<std::chrono::steady_clock::time_point> dropped_at_{};
    std::uint64_t loops_ = 0;
    std::optional<std::chrono::steady_clock::time_point> loops_at_{};
    std::optional<std::chrono::steady_clock::time_point> sent_since_{}; ///< when frames to the bus were first counted
    std::uint32_t foreign_most_ = 0;
    std::vector<long> foreign_pids_;
    std::optional<std::chrono::milliseconds> foreign_first_{};
    OutsideLink::SharedStop shared_stop_ = OutsideLink::SharedStop::None;
};

/// When run --events writes the event "outside" of one bridge: when its counts have changed
/// (on_counts()), at most once in kEvery; counts held back by that go out once kEvery has passed
/// (due(), which the run asks while it watches the simulation) and at the end (flush()), so the last
/// counts are always written. Time is an argument: no clock, no thread, no lock (the caller's).
class OutsideNote
{
public:
    static constexpr std::chrono::milliseconds kEvery{1000};

    /// Counts read at `now`: what to write now, or empty (the same as written last, or too soon).
    [[nodiscard]] std::optional<OutsideCounts> on_counts(OutsideCounts counts, std::chrono::steady_clock::time_point now);
    /// The counts held back, if kEvery has passed at `now` since the last ones written; else empty.
    [[nodiscard]] std::optional<OutsideCounts> due(std::chrono::steady_clock::time_point now);
    /// The counts held back, whatever the time; else empty.
    [[nodiscard]] std::optional<OutsideCounts> flush();
    /// The counts read last, or empty if none were.
    [[nodiscard]] std::optional<OutsideCounts> last() const { return last_; }

private:
    std::optional<OutsideCounts> last_;
    std::optional<OutsideCounts> written_;
    std::chrono::steady_clock::time_point written_at_{};
};

/// One line of a participant's output as the launcher shows it: control characters (C0 but tabs,
/// DEL, C1) and bytes that are not UTF-8 as '?'.
[[nodiscard]] std::string printable(std::string_view line);
/// The file name (without ".log") of a participant's log: its name with every byte but ASCII
/// letters, digits, '.', '-' and '_' as '_', and a Windows device name (NUL, CON, COM1, ...; also
/// before a '.') with '_' after it.
[[nodiscard]] std::string log_name(const std::string& name);
} // namespace detail

} // namespace vehicle_sil::launch
