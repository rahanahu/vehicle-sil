// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run: checks the configuration (check.hpp), then starts the registry and the
// controller in this process and every participant as a child process, watches the simulation
// until it ends, ends every child and writes the result for people and as JSON. What the run
// saw (RunOutcome) is judged by a pure function (assess_run()). Internal; the behaviour and the
// output: docs/simulation-config.md.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/process/process.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
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
    Ended,         ///< it ran and ended otherwise (a participant stopped it)
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

/// What happened to one participant (in the order of the configuration).
struct ParticipantRun
{
    std::string name;
    bool required = true;
    bool describe = true;
    /// false: disabled (enabled: false, or --disable); it was not started (Ending::NotStarted).
    bool enabled = true;
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
    /// The participant whose failure ended the run (it ended, or the controller saw it leave), or "".
    std::string failed_participant{};
    /// Why the controller saw it fail (FailureCheck, without the name), or "" if the launcher saw
    /// its process end.
    std::string failure_reason{};
    std::int64_t start_timeout_ms = 0;
    std::int64_t stop_timeout_ms = 0;
    std::optional<std::int64_t> stop_at_ms{};
    RunTiming timing{};
    std::vector<ParticipantRun> participants{};
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

/// The time commands of `run --stdin-commands` (pause, resume, step, speed) on their way from the
/// thread that reads them to the simulation's time gate. Thread safe. apply() never waits for the
/// run or the reader of the events: only for this object's mutex and the gate's (neither is held
/// while something waits).
///
/// Before the run attaches its controller (the check, the start), what the commands ask for is
/// kept and given to the controller when it is attached. While attached, each command goes to the
/// gate at once. Once detached (the simulation is over, or did not start), commands are refused.
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
    /// up to whole steps of the controller), then does it. The event comes before what the command
    /// changes. Returns the command as written.
    Command apply(const Command& command, std::string_view source);

    /// The run's part. attach(): from now on the commands go to `controller` (its time gate,
    /// made with `step_ms`); first it is given the speed (`speed`, or the last speed command) and
    /// the pause (start paused, or the last pause or resume command, and the steps since). detach():
    /// before the controller goes; the commands are refused from then on.
    void attach(SimulationController& controller, std::int64_t step_ms, std::optional<double> speed);
    void detach();

private:
    struct State;
    std::unique_ptr<State> state_;
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
};

/// Runs the file `path` (shown as `shown_as`): check_file(); if it passed, the simulation. Never
/// throws for a problem of the file, an executable or a participant: they are findings. Must be
/// called from the thread that lives longest (the main thread): on Linux, the participants end
/// when it ends (process::Process).
[[nodiscard]] RunReport run_file(const std::filesystem::path& path, const std::string& shown_as, const RunOptions& options);

namespace detail {
/// The --start-timeout-ms the launcher gives an ecu_main executable: longer than the launcher's own
/// wait for the start, so that the launcher, not the ECU, decides that the start failed and names
/// who did not join; the ECU's limit only ends it if the launcher is gone.
[[nodiscard]] std::int64_t ecu_start_timeout_ms(const SimulationConfig& config);

/// The findings of assess_run(`outcome`) that are settled while the participants are still being
/// ended: whatever happens to them from here on, assess_run() of the final outcome has each of
/// these unchanged. (Left out: a participant that failed while its process still runs, whose
/// finding says how the process ended.) What run --events writes before the participants end.
[[nodiscard]] std::vector<Finding> settled_findings(const RunOutcome& outcome);

/// The line of the text output about the time ("time: ..."), or "" if no speed was set and no
/// step was paced.
[[nodiscard]] std::string timing_text(const RunTiming& timing);

/// The step of the launcher's controller for a run at `speed` (empty: as fast as possible), and
/// what it means for the ECUs' steps: RunTiming's time_step_ms, time_step_written, ecu_step_ns,
/// step_delay_ns and fine_step_ms (the rest is left as it is in the result).
/// - time_step_ms as written, if it is.
/// - Without a speed: launch::time_step_ms() (kDefaultTimeStepMs or what of it divides stop_at_ms).
/// - With a speed: the greatest common divisor of kDefaultTimeStepMs, stop_at_ms and the steps of
///   the enabled ECUs as `check` has them, so that every ECU step starts at a controller step (the
///   gate keeps real time only there); 1 ms if a participant's step is not known (describe: false)
///   or the divisor is no whole number of milliseconds.
[[nodiscard]] RunTiming plan_time_step(const SimulationConfig& config, const CheckReport& check,
                                       const std::optional<double>& speed);

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
