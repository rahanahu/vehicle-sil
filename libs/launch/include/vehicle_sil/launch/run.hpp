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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

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

struct RunOptions
{
    CheckOptions check{};
    /// A file of each participant's output in this directory (created if needed); empty: none.
    std::filesystem::path log_dir{};
    /// Writes one line (without its end) for the person watching: the participants' output as
    /// "[<name>] <line>", the launcher's own as "[vehicle_sil_launch] <text>". Called from several
    /// threads, one call at a time.
    std::function<void(std::string_view line)> write_line{};
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
