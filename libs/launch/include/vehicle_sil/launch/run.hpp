// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: checks the configuration (check.hpp), then starts the registry and the
// controller in this process and every participant as a child process, watches the simulation
// until it ends, ends every child and writes the result for people and as JSON. What the run
// saw (RunOutcome) is judged by a pure function (assess_run()). Internal; the behaviour and the
// output: docs/simulation-config.md.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/scenario_run.hpp"
#include "vehicle_sil/process/process.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
/// report a program that Ctrl+C ended.
inline constexpr int kExitInterrupted = 130;

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
    std::size_t line = 0; ///< its entry in the configuration file
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

/// How the simulation's time went (the result's "timing"). The pacing numbers are the time gate's
/// (TimeControlOptions in vehicle_sil/core/simulation_controller.hpp has the definitions): they
/// run from the first step of the launcher's controller to the last one, and include a step that
/// had not begun when the run was ended although its real time had come.
struct RunTiming
{
    /// The speed at the end; empty: as fast as possible.
    std::optional<double> speed{};
    /// The step of the launcher's controller (detail::plan_time_step()), and whether the file's
    /// time_step_ms gave it.
    std::int64_t time_step_ms = 0;
    bool time_step_written = false;
    /// The smallest step of the ECUs in nanoseconds; empty if a participant's step is not known
    /// (describe: false), or none has one.
    std::optional<std::int64_t> ecu_step_ns{};
    /// How long after the real time its virtual time stands for an ECU step may run although
    /// nothing is late, because the controller's step is coarser than the ECUs' (nanoseconds; 0:
    /// every ECU step starts at a controller step); empty if a participant's step is not known.
    std::optional<std::int64_t> step_delay_ns{};
    /// The time_step_ms with which step_delay_ns is 0.
    std::int64_t fine_step_ms = 0;
    /// The check found coarse-time-step already (the file has speed and time_step_ms, and the run
    /// keeps that speed): assess_run() does not report it again.
    bool coarse_step_checked = false;
    std::int64_t late_tolerance_ms = 0;
    /// The virtual time the simulation reached (the controller's last step).
    std::int64_t sim_ms = 0;
    /// From the beginning of the controller's first step to the beginning of its last one (to the
    /// end of the run if the next step was overdue then, or the speed was max): without the
    /// start-up, the shutdown and the time until a failure was noticed after the simulation ended.
    std::int64_t wall_ms = 0;
    /// Of wall_ms, the time it stood still (paused).
    std::int64_t paused_ms = 0;
    /// The controller steps that were measured against real time (a speed was set), and those
    /// that began more than the tolerance late.
    std::uint64_t paced_steps = 0;
    std::uint64_t late_steps = 0;
    /// The largest lateness of a paced step, in milliseconds (rounded up).
    std::int64_t max_late_ms = 0;
    /// The sum of the lateness of the late steps (rounded up): the simulation does not catch up
    /// after a late step, so this is how far behind the speed asked for it came in all.
    std::int64_t slipped_ms = 0;
    /// How late the step was that had not begun when the run ended (rounded up); 0 if none was
    /// overdue.
    std::int64_t late_ms = 0;

    /// sim_ms / (wall_ms - paused_ms) to three decimals; empty if it did not run for a
    /// millisecond.
    [[nodiscard]] std::optional<double> actual_speed() const;
};

/// What a run saw.
struct RunOutcome
{
    /// The registry or the controller could not be set up (nothing was started): why.
    std::string setup_error{};
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
    std::vector<ParticipantRun> participants{};
    /// run --scenario: what became of the scenario (ScenarioStatus::NotRun if the simulation did
    /// not start).
    std::optional<ScenarioRun> scenario{};
    /// The scenario was over (passed or failed) and the launcher stopped the simulation for it,
    /// at ScenarioRun::over_at (SimulationEnd::Ended).
    bool stopped_by_scenario = false;
    /// The scenario's thread failed unexpectedly: why (the scenario is Incomplete).
    std::string scenario_error{};
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

/// The JSON of `vehicle_sil_launch <command> --json` when the launcher itself failed unexpectedly
/// (`message`): the form of the command's result (`run`: "vehicle_sil_run"; else the check's),
/// result "unreadable", one finding with the code "internal".
[[nodiscard]] std::string format_internal_error_json(bool run, const std::string& message);

/// The time commands (pause, resume, step, run_until, speed) on their way to the simulation's time
/// gate, from every source: the reader of standard input (`run --stdin-commands`) and the threads
/// of a control server (`run --control`). Thread safe. The commands of all sources go through one
/// mutex: each is decided, written as a command event and done before the next, so the order of
/// the command events is the order in which they took effect. apply() never waits for the run or
/// the reader of the events: only for this object's mutex and the gate's (neither is held while
/// something waits).
///
/// Before the run attaches its controller (the check, the start), what the commands ask for is
/// kept and given to the controller when it is attached. While attached, each command goes to the
/// gate at once. Once detached (the simulation is over, or did not start), commands are refused.
///
/// The hold (run --start-paused with a control server): in the start window, before anyone's
/// step at 0, the run waits in hold_before_start() until a resume, step or run_until command is
/// accepted; until then the simulation stands still at 0 before its first step, so what a client
/// writes then takes effect in the steps at 0. The commands accepted before the window count:
/// the run holds only if it is to stand still at 0 when the window opens (start paused, or a
/// pause since, and no resume, step, run_until or pause at a later time after that). A step or
/// run_until before the window is done from 0 without a hold, as if it had come in the hold.
class TimeCommands
{
public:
    /// `start_paused`: run --start-paused (a step is accepted before anything has been paused).
    /// `events`: where apply() writes the command events (may be null).
    explicit TimeCommands(bool start_paused = false, EventStream* events = nullptr);
    ~TimeCommands();
    TimeCommands(const TimeCommands&) = delete;
    TimeCommands& operator=(const TimeCommands&) = delete;

    /// Decides whether the time command (Command::is_time()) can be done now; writes its command
    /// event (`source`) with that answer (and, for step, how far the simulation goes: "ms" rounded
    /// up to whole steps of the controller; for run_until and pause with at_ms, where it will stand
    /// still: "at_ms" rounded up the same way), then does it. The event comes before what the
    /// command changes. Returns the command as written. Refused: a step while not paused, a step,
    /// run_until or pause with at_ms before the controller is attached, any command once detached,
    /// a speed out of the range of parse_speed(), a step or at_ms out of 1 to kMaxMillis (at_ms
    /// from 0). A "stop" (a control server's) is only decided and written here (refused once
    /// detached); the caller asks the run to stop.
    Command apply(const Command& command, std::string_view source);
    /// As apply(command, source), and if the command is accepted, `accepted` is called before
    /// apply() returns, under this object's mutex (short, must not wait): before any later
    /// command and before detach(). A control server's stop asks the run to stop there, so that
    /// a stop that is accepted reaches the run before the simulation is over.
    Command apply(const Command& command, std::string_view source, const std::function<void()>& accepted);

    /// The run's part. attach(): from now on the commands go to `controller` (its time gate,
    /// made with `step_ms`); first it is given the speed (`speed`, or the last speed command) and
    /// the pause (start paused, or the last pause or resume command, and the steps since). detach():
    /// before the controller goes; the commands are refused from then on, a hold ends and
    /// wait_paused() returns.
    void attach(SimulationController& controller, std::int64_t step_ms, std::optional<double> speed);
    void detach();
    /// `f` is called in detach() with the controller (if one is attached), under this object's
    /// mutex, before it goes: the last look at it (short, must not wait). Set before the run.
    void set_on_detach(std::function<void(SimulationController& controller)> f);

    /// How hold_before_start() ended.
    enum class Hold
    {
        None,     ///< nothing was held: the commands before it let the simulation go on (see above)
        Released, ///< a resume, step or run_until command was accepted
        GaveUp,   ///< `give_up()` was true, or detach() was called
    };
    /// The hold, on the thread of the start window: waits until a resume, step or run_until
    /// command has been accepted, `give_up()` is true or detach() is called. `on_hold` is called
    /// once when the hold begins (not for Hold::None), `give_up` every `poll`; both without this
    /// object's mutex.
    Hold hold_before_start(const std::function<bool()>& give_up, std::chrono::milliseconds poll,
                           const std::function<void()>& on_hold = {});

    /// Where the run stands, for a control server.
    enum class Stage
    {
        SettingUp, ///< before attach(): the check, the start of the registry and the controller
        Attached,  ///< the controller takes the commands (also before its start and in the hold)
        Detached,  ///< the simulation is over, or did not start
    };
    /// While attached, calls `f` with the controller and whether the run holds before its first
    /// step, under this object's mutex (`f` must be short and must not wait). Returns the stage.
    Stage inspect(const std::function<void(SimulationController& controller, bool holding)>& f);

    /// Waits up to `timeout` until the simulation stands still: the time it stands at (0 in the
    /// hold), or empty on the timeout, once detached and once the controller's lifecycle has ended
    /// (as SimulationController::wait_until_paused()).
    std::optional<SimTime> wait_paused(std::chrono::milliseconds timeout);

private:
    struct State;
    std::unique_ptr<State> state_;
};

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
    /// --speed: replaces the configuration's speed (the inner empty: "max").
    std::optional<std::optional<double>> speed{};
    /// --start-paused: the simulation stands still at 0 until a resume or step command.
    bool start_paused = false;
    /// Where the time commands go (--stdin-commands); null: the run has none of its own.
    TimeCommands* time_commands = nullptr;
    /// A file of each participant's output in this directory (created if needed); empty: none.
    std::filesystem::path log_dir{};
    /// Writes one line (without its end) for the person watching: the participants' output as
    /// "[<name>] <line>", the launcher's own as "[vehicle_sil_launch] <text>". Called from several
    /// threads, one call at a time. Not called when `events` is set.
    std::function<void(std::string_view line)> write_line{};
    /// run --events: what happens, as events (events.hpp) instead of write_line(): the phases, the
    /// check, the participants, the lines, the run's findings, the virtual time. run_file() writes
    /// every event but hello and the result (the caller writes them). No call waits for the
    /// reader of the events. May be null.
    EventStream* events = nullptr;
    /// How often the launcher has been interrupted (by the caller's signal handler); 1 stops the
    /// simulation in order, 2 or more skip the waits for the participants to end. May be null.
    const std::atomic<int>* interrupts = nullptr;
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
    /// stay usable while run_file() runs (its time operations go to `time_commands`, which must be
    /// set) and outlive the hub's use by run_file(); the caller ends it after run_file().
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
/// The --start-timeout-ms the launcher gives an ecu_main executable: longer than the launcher's own
/// wait for the start, so that the launcher, not the ECU, decides that the start failed and names
/// who did not join; the ECU's limit only ends it if the launcher is gone. kMaxMillis if the start
/// waits for a participant someone starts by hand (waits_for_manual()): that may take any time.
[[nodiscard]] std::int64_t ecu_start_timeout_ms(const SimulationConfig& config);

/// One line for this system's shell that starts `executable` (as given) with `arguments`: on
/// Windows for cmd.exe, quoted as the C runtimes read a command line back (an argument in double
/// quotes if it has a space, a tab, a quote or one of &|<>^; %...% is not protected), elsewhere as
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

/// The step of the launcher's controller for a run at `speed` (empty: as fast as possible; `fine`:
/// as with a speed, whatever the speed is: a run with a scenario, whose results must not depend on
/// the speed), and
/// what it means for the ECUs' steps: RunTiming's time_step_ms, time_step_written, ecu_step_ns,
/// step_delay_ns and fine_step_ms (the rest is left as it is in the result).
/// - time_step_ms as written, if it is.
/// - Without a speed: launch::time_step_ms() (kDefaultTimeStepMs or what of it divides stop_at_ms).
/// - With a speed: the greatest common divisor of kDefaultTimeStepMs, stop_at_ms and the steps of
///   the enabled ECUs as `check` has them, so that every ECU step starts at a controller step (the
///   gate keeps real time only there); 1 ms if a participant's step is not known (describe: false)
///   or the divisor is no whole number of milliseconds.
[[nodiscard]] RunTiming plan_time_step(const SimulationConfig& config, const CheckReport& check,
                                       const std::optional<double>& speed, bool fine = false);

/// The warning coarse-time-step for `timing` (plan_time_step() with speed set as the run has it),
/// if it applies: a speed counts (timing.speed, or steps were paced) and an ECU step starts
/// between two controller steps. assess_run() reports it after a run; check_file() before one, if
/// the file has speed and time_step_ms.
[[nodiscard]] std::optional<Finding> coarse_time_step(const RunTiming& timing);

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
/// One line of a participant's output as the launcher shows it: control characters (C0 but tabs,
/// DEL, C1) and bytes that are not UTF-8 as '?'.
[[nodiscard]] std::string printable(std::string_view line);
/// The file name (without ".log") of a participant's log: its name with every byte but ASCII
/// letters, digits, '.', '-' and '_' as '_', and a Windows device name (NUL, CON, COM1, ...; also
/// before a '.') with '_' after it.
[[nodiscard]] std::string log_name(const std::string& name);
} // namespace detail

} // namespace vehicle_sil::launch
