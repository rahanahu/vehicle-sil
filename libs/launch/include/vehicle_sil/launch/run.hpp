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
#include "vehicle_sil/launch/run_controls.hpp"
#include "vehicle_sil/launch/scenario_run.hpp"
#include "vehicle_sil/launch/stop_requests.hpp"
#include "vehicle_sil/launch/time_commands.hpp"
#include "vehicle_sil/launch/time_plan.hpp"
#include "vehicle_sil/launch/timeline.hpp"
#include "vehicle_sil/launch/traffic.hpp"
#include "vehicle_sil/process/process.hpp"

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
inline constexpr int kRunFormat = 1;

/// The exit code of a run that was interrupted (Ctrl+C, SIGINT, SIGTERM): 128 + SIGINT, as shells
/// report a program that Ctrl+C ended (the same as an ECU executable's).
inline constexpr int kExitInterrupted = ecu_executable::kExitInterrupted;

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
    /// The bridge that run started for --connect: the network it connects (ParticipantConfig::
    /// for_network); "" for a participant of the file. If it ends before the simulation is over,
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
};

/// Whether `run` is a participant that stops the run when it ends: a bridge that run started for
/// --connect (ParticipantRun::for_network).
namespace detail {
[[nodiscard]] inline bool stops_run(const ParticipantRun& run) { return !run.for_network.empty(); }
} // namespace detail

/// What a run saw.
struct RunOutcome
{
    /// The registry or the controller could not be set up (nothing was started): why.
    std::string setup_error{};
    /// Where the registry of the run listens, as its participants get it (with the port it took
    /// for port 0), or "" if none was started.
    std::string registry_uri{};
    SimulationEnd simulation = SimulationEnd::NotStarted;
    /// The required participants did not all join within start_timeout_ms.
    bool start_timed_out = false;
    /// They all joined, but the controller could not start the simulation: why.
    std::string start_error{};
    /// The simulation ended in this state instead of shutting down normally (SIL Kit's name), or "".
    std::string controller_state{};
    /// The interrupt that ended the run ("SIGINT", "SIGTERM", "Ctrl+C", ...), or "".
    std::string interrupted_by{};
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
    /// With --timeout-ms: how long the run stood still before its first step (the hold of
    /// --start-paused with a control server) until a command let it go on, if it did; --timeout-ms
    /// counts from the beginning of the hold.
    std::optional<std::int64_t> held_before_start_ms{};
    /// timed_out before the simulation started: a run that waits for a participant someone starts
    /// by hand, whose --timeout-ms counts from when the launcher began to wait for the participants.
    bool timed_out_before_start = false;
    /// timed_out_before_start: what the start still waited for ("Late (manual) has not joined", ...).
    std::string timed_out_waiting_for{};
    /// --timeout-ms counted from when the launcher began to wait for the participants (a run that
    /// waits for a participant someone starts by hand), not from the start.
    bool timeout_from_wait = false;
    /// What the launcher's end of the control agents dropped in the run (HubTransportStats):
    /// hellos it did not take, messages it could not decode, and messages that fit no agent it
    /// knows or no schema.
    std::uint64_t control_refused_hellos = 0;
    std::uint64_t control_undecodable = 0;
    std::uint64_t control_unknown = 0;
    /// RunOptions::before_start threw: the simulation had started, and the launcher aborted it. Why.
    std::string before_start_error{};
    /// The participant whose failure ended the run (it ended, or the controller saw it leave), or "".
    std::string failed_participant{};
    /// Why the controller saw it fail (FailureCheck, without the name), or "" if the launcher saw
    /// its process end.
    std::string failure_reason{};
    std::int64_t start_timeout_ms = 0;
    std::int64_t stop_timeout_ms = 0;
    /// How long the start waited at most for the hello of the control agents.
    std::int64_t control_hello_timeout_ms = 0;
    std::optional<std::int64_t> stop_at_ms{};
    RunTiming timing{};
    /// In the order of the configuration, then the bridges that run started for --connect.
    std::vector<ParticipantRun> participants{};
    /// --connect: the networks connected to a real bus, in the order of the file's networks.
    std::vector<RunOutside> outside{};
    /// The first time command of the simulation that changed how its time goes (a pause, a step, a
    /// run_until, a speed other than 1), if any: TimeCommands::first_time_change() once the
    /// simulation is over. With --connect, the warning connect-time-changed.
    std::optional<TimeChange> time_changed{};
    /// What went over the networks; empty if the simulation did not start.
    std::optional<RunTraffic> traffic{};
    /// run --scenario: what became of the scenario (ScenarioStatus::NotRun if the simulation did
    /// not start).
    std::optional<ScenarioRun> scenario{};
    /// The scenario was over (passed or failed) and the launcher stopped the simulation for it,
    /// at ScenarioRun::over_at (SimulationEnd::Ended).
    bool stopped_by_scenario = false;
    /// The scenario's thread failed unexpectedly: why (the scenario is Incomplete).
    std::string scenario_error{};
    /// run --timeline: the timeline file as the user named it (RunOptions::timeline_shown), once
    /// it was opened; "" without one.
    std::string timeline{};
    /// What the scenario's engine noted for the timeline; null if the scenario did not begin.
    std::shared_ptr<const TimelineRecord> timeline_record{};
    /// The timeline file could be opened, but not written when the run was over: why (finding
    /// "timeline-write"; the scenario's result stands). (A record that dropped points at its
    /// limits is the note "timeline-limit".)
    std::string timeline_error{};
    /// Set if the launcher's own SIL Kit connection (the hub's transport, the controller, the
    /// registry) had not closed when the launcher stopped waiting for it, after the participants
    /// had ended: how long it waited, in milliseconds (the limit: the controller's shutdown
    /// timeout and stop_timeout_ms, at least 5000 ms for the latter). A thread of the run is left
    /// closing it, with the parts of the run it may use (they are never destroyed). The result is
    /// complete all the same. The caller ends the process without destroying what it gave the run
    /// (RunOptions) and without running the destructors of static objects (std::_Exit(); Windows:
    /// TerminateProcess()) when it has written the result.
    std::optional<std::int64_t> connection_not_closed_ms{};
    /// connection_not_closed_ms: one more interrupt ended the wait before its limit (the person
    /// asked for the end at once: no finding). false: the limit passed (finding
    /// "connection-not-closed").
    bool connection_close_interrupted = false;
};

/// Judges a run: the errors (they fail it), warnings and notes, each about the participant (with
/// its line) or the simulation, in sort_findings() order.
[[nodiscard]] std::vector<Finding> assess_run(const RunOutcome& outcome);

enum class RunResult
{
    Passed,       ///< the simulation ended normally and every required participant exited with 0
    Failed,       ///< the run found errors (or, --strict, warnings)
    Interrupted,  ///< the launcher was interrupted
    CheckFailed,  ///< the check found errors (or, --strict, warnings): nothing was started
    Unreadable,   ///< the configuration cannot be read: nothing was started
    SetupFailed,  ///< the registry or the controller could not be set up: nothing was started
    Usage,        ///< the command line was wrong
};

/// The result of `vehicle_sil_launch run`.
struct RunReport
{
    CheckReport check{};
    /// Empty if nothing was started because of the check (or the command line).
    std::optional<RunOutcome> run{};
    /// assess_run() of `run`.
    std::vector<Finding> findings{};
    bool strict = false;
    bool usage = false;

    /// The check's and the run's together.
    [[nodiscard]] std::size_t errors() const;
    [[nodiscard]] std::size_t warnings() const;
    [[nodiscard]] std::size_t notes() const;
    [[nodiscard]] RunResult result() const;
    /// 0 passed; 1 failed or the check failed; 3 unreadable or the set-up failed; 4 usage;
    /// kExitInterrupted.
    [[nodiscard]] int exit_code() const;
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

/// Where the run's control server listens (RunOptions::serve_control).
struct ControlServerInfo
{
    /// host:port with the port it actually listens on (also for port 0).
    std::string address{};
    bool tls = false;
    /// Every call needs a token of the server's credentials.
    bool credentials = false;
};

struct RunOptions
{
    CheckOptions check{};
    /// --speed: replaces the configuration's speed (the inner empty: "max"). With the time lock
    /// (CheckReport::time_lock) the run goes at speed 1 whatever this says (planned_speed()), and
    /// its check refuses another speed: run_file() gives the check this speed and `start_paused`
    /// (CheckOptions::run_speed, CheckOptions::start_paused), so they are written here only.
    std::optional<std::optional<double>> speed{};
    /// --start-paused: the simulation stands still at 0 until a resume or step command. With the
    /// time lock the check refuses it.
    bool start_paused = false;
    /// The run's controls (run_controls.hpp), the caller's: the stop requests the run reads (as
    /// `stops` below says), the time commands it attaches its controller to (--stdin-commands, a
    /// control server) and the phase it sets. For this one run: controls that a run has used
    /// take no time command any more. With `events`, the caller gives the controls the observers
    /// that write the command and phase events (the run writes neither by itself then). Null: the
    /// run makes its own for this run, on `stops`; no one outside reaches its time commands, and
    /// the run writes its phases to `events`. With `events` the controls must have both observers, and
    /// `stops` must be null or the controls' own: run_file() throws std::invalid_argument otherwise.
    RunControls* controls = nullptr;
    /// A file of each participant's output in this directory (created if needed); empty: none.
    std::filesystem::path log_dir{};
    /// run --timeline, with a scenario: the timeline file of the run (timeline.hpp; its directory
    /// is created if needed), and its name as the user gave it; empty: none. The file is opened
    /// before anything starts (RunOutcome::setup_error if that fails) and written when the run
    /// is over, whatever became of the scenario.
    std::filesystem::path timeline{};
    std::string timeline_shown{};
    /// Writes one line (without its end) for the person watching: the participants' output as
    /// "[<name>] <line>", the launcher's own as "[vehicle_sil_launch] <text>". Called from several
    /// threads, one call at a time. Not called when `events` is set.
    std::function<void(std::string_view line)> write_line{};
    /// run --events: what happens, as events (events.hpp) instead of write_line(): the phases, the
    /// check, the participants, the lines, the run's findings, the virtual time. run_file() writes
    /// every event but hello and the result (the caller writes them). No call waits for the
    /// reader of the events. May be null.
    EventStream* events = nullptr;
    /// Without `controls`: the requests to stop (the caller's: its signal handler and whatever
    /// else asks). The run reads their count: 1 stops the simulation in order, 2 or more skip the
    /// waits for the participants to end. May be null. Not read with `controls`, which has its own
    /// (RunControls::stops()).
    StopRequests* stops = nullptr;
    /// The name of the first interrupt ("SIGINT"), for the result.
    std::function<std::string()> interrupt_name{};
    /// How often the launcher looks at the participants and the simulation.
    std::chrono::milliseconds poll{20};
    /// The longest the simulation may run, from its start to its end (wall clock); then it is
    /// aborted (RunOutcome::timed_out). Empty: no limit.
    std::optional<std::chrono::milliseconds> timeout{};
    /// The hub that the ECUs' control agents connect to (over SIL Kit, on the controller's
    /// participant), from the start window to the end of the run; it must outlive run_file().
    /// Null: the run has a hub of its own. With a hub of the caller, other threads read and write
    /// the ECUs' variables through it while the simulation runs.
    control::Hub* hub = nullptr;
    /// Called in the window before the start (SimulationController::set_start_window()), on a
    /// thread of the controller, after the wait for the agents' hellos: every participant waits
    /// before its first step, and a write that an agent accepts now (control::Hub::write(),
    /// wait_queued()) takes effect in its step at 0. The simulation starts when it returns. If it
    /// throws, the launcher aborts the simulation as soon as it has started
    /// (RunOutcome::before_start_error). Not called if the start was given up before the window.
    std::function<void(control::Hub& hub)> before_start{};
    /// How long the start waits for the hello of each required ECU that describes variables and
    /// offers them in the launcher's version of the messages (it is there before the wait begins,
    /// by the order in which SIL Kit sets the participants up: the limit is a last line).
    std::chrono::milliseconds control_hello_timeout{5000};
    /// run --control: starts the control server on the run's hub (the argument) and says where it
    /// listens. Called once, on the main thread, when the check has passed and before the registry
    /// starts; the run writes the event "control_server" and the line "control server at
    /// <address>". If it throws, nothing is started (RunOutcome::setup_error). The server must
    /// stay usable while run_file() runs (its time operations go to the time commands of
    /// `controls`, which must be set) and outlive the hub's use by run_file(); the caller ends it
    /// after run_file().
    /// With start_paused, the run holds before the first step (TimeCommands::hold_before_start()).
    std::function<ControlServerInfo(control::Hub& hub)> serve_control{};
};

/// Runs the file `path` (shown as `shown_as`): check_file(); if it passed, the simulation; with
/// CheckOptions::scenario (in `options.check`), the scenario in it (docs/scenario.md; the run then
/// ends when the scenario is over). Never
/// throws for a problem of the file, an executable or a participant: they are findings. Must be
/// called from the thread that lives longest (the main thread): on Linux, the participants end
/// when it ends (process::Process).
[[nodiscard]] RunReport run_file(const std::filesystem::path& path, const std::string& shown_as, const RunOptions& options);

/// Where the registry listens if the start waits for a participant someone starts by hand and the
/// file has no registry_uri: a fixed port, so that the command to start it is the same in every
/// run (the port SIL Kit's own registry takes by default).
inline constexpr std::string_view kManualRegistryUri = "silkit://127.0.0.1:8500";

namespace detail {
/// The check that run_file() runs with: a copy of `check` (that passed) with one participant at the
/// end for each network that --connect connects (CheckReport::outside, in its order): the bridge
/// bridge.<network> (role: bridge, required: false, its network's line, for_network set), with a
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
/// ext_from_devices of vehicle_sil_ethernet_bridge, silkit_to_xl and xl_to_silkit of
/// vehicle_sil_vector_xl_bridge. Empty for any other line, for a line with a key twice, with a
/// value that is not a decimal number up to 2^63 - 1 or with a part of another form, for one without
/// either key (a bridge of another version), and for any other tool.
[[nodiscard]] std::optional<OutsideCounts> read_outside_counts(std::string_view tool, std::string_view line);

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
