// SPDX-License-Identifier: MIT
//
// The machine interface of vehicle_sil_launch: the events of `run --events` (one JSON object per
// line), the commands of `run --stdin-commands` (one JSON object per line on standard input) and
// `version --json`. Internal; the format: docs/simulation-events.md.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/process/process.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

/// The version of the events (hello's "vehicle_sil_events").
inline constexpr int kEventsFormat = 1;
/// The version of `version --json` (its "vehicle_sil_launch").
inline constexpr int kVersionFormat = 1;
/// What this launcher can do, by name, for a reader that asks "is it there" instead of comparing
/// versions (`version --json`'s "features"). Names are only added.
inline constexpr std::array<std::string_view, 10> kFeatures{"events",        "stdin-commands", "check-variables", "speed",
                                                            "start-paused",  "time-commands",  "control-links",   "scenario",
                                                            "manual-participants", "scenario-wave"};

/// `version`: "vehicle_sil_launch <version>\n".
[[nodiscard]] std::string format_version_text();
/// The feature of a vehicle_sil_launch built with the control server (run --control): only such
/// a build lists it (format_version_json()'s `more`).
inline constexpr std::string_view kControlServerFeature = "control-server";
/// The feature of such a build that has the control server during a scenario too (run --scenario
/// with --control: its clients read, and cannot write or move the time). After
/// kControlServerFeature in `more`.
inline constexpr std::string_view kScenarioControlFeature = "scenario-control";

/// `version --json`: the formats this launcher writes and reads, kFeatures, then `more` (what
/// this build adds).
[[nodiscard]] std::string format_version_json(const std::vector<std::string_view>& more = {});

/// The stages of a run, in this order (a run may skip some, never goes back).
enum class Phase
{
    Checking, ///< the check (--describe of each executable)
    Starting, ///< the registry, the controller, the participants' processes
    Waiting,  ///< for the required participants to join
    Running,  ///< the simulation runs
    Stopping, ///< the simulation has ended (or did not start): the participants are being ended
    Ended,    ///< every participant has ended; the result follows
};
[[nodiscard]] const char* phase_name(Phase phase);

/// One line of standard input, read as a command.
struct Command
{
    /// The value of "command" if the line is a JSON object whose "command" is a string.
    std::optional<std::string> name{};
    bool accepted = false;
    /// Why it was not accepted (one line), or "".
    std::string message{};
    /// step: its "ms" as read; once applied (TimeCommands), how far the simulation goes (the
    /// command event's "ms").
    std::optional<std::int64_t> ms{};
    /// speed: its "value" (empty: "max").
    std::optional<double> speed{};
    /// Only from a control server: pause at, and run_until, this virtual time; once applied
    /// (TimeCommands), where the simulation will stand (the command event's "at_ms"), a whole
    /// number of the controller's steps.
    std::optional<std::int64_t> at_ms{};

    [[nodiscard]] bool is_stop() const { return accepted && name == "stop"; }
    /// pause, resume, step, run_until or speed (TimeCommands applies it).
    [[nodiscard]] bool is_time() const { return accepted && name && *name != "stop"; }
};

/// The longest command line read; a longer one is not accepted.
inline constexpr std::size_t kMaxCommandLine = 64 * 1024;

/// Reads `line` (without its line end; a "\r" at its end is ignored) as a command: {"command":
/// "stop"}, {"command":"pause"}, {"command":"resume"}, {"command":"step","ms":<1 to kMaxMillis>},
/// {"command":"speed","value":<a speed, parse_speed()>} (other keys are ignored). Only the form
/// is read: whether the command can be done now is TimeCommands' part. Empty for a line of white
/// space only (it is skipped).
[[nodiscard]] std::optional<Command> parse_command(std::string_view line);

/// Cuts the bytes of standard input into lines and reads each as a command (parse_command()). A
/// line longer than kMaxCommandLine is one command that is not accepted, and the rest of it up to
/// its line end is skipped. Not thread safe: one reader.
class CommandInput
{
public:
    explicit CommandInput(std::function<void(const Command&)> on_command) : on_command_(std::move(on_command)) {}
    /// What was read.
    void append(std::string_view bytes);
    /// The end of input: a last line without a line end is read too.
    void finish();

private:
    void line(std::string_view text);

    std::function<void(const Command&)> on_command_;
    std::string pending_;
    bool skipping_ = false; ///< in a line that was too long, until its end
};

/// The event "time".
struct TimeEvent
{
    std::int64_t sim_ms = 0;
    /// The speed set now; empty: as fast as possible ("max").
    std::optional<double> speed{};
    bool paused = false;
    /// Virtual time / wall-clock time over about the last second while it ran; empty (null) if
    /// that cannot be told (too short, or it stood still).
    std::optional<double> actual_speed{};
    /// The pacing numbers so far, as if the run ended now (RunTiming has their meaning): they
    /// include a step that has not begun although its real time has come.
    std::uint64_t late_steps = 0;
    std::int64_t max_late_ms = 0;
    std::int64_t slipped_ms = 0;
    /// How late the step that has not begun yet is by now; 0 if none is overdue.
    std::int64_t late_ms = 0;
};

/// A participant's state in the event "participant".
enum class ParticipantState
{
    Disabled,      ///< left out (enabled: false, --disable): not started
    NotStarted,    ///< its process could not be started ("message" says why)
    Started,       ///< its process runs ("pid", "command", "log")
    Manual,        ///< someone starts it (manual): the launcher does not ("command", "working_directory", "command_line")
    Joined,        ///< it joined the simulation (the controller saw its lifecycle)
    Failed,        ///< the launcher decided that it failed, and ends the run ("reason", "message")
    Ended,         ///< its process ended ("exit_code", "signal")
    StopRequested, ///< the launcher asked it to stop (SIGTERM, Ctrl+Break)
    Killed,        ///< the launcher ended it with the processes it started
};
[[nodiscard]] const char* participant_state_name(ParticipantState state);

/// What one call of EventStream's `write` did.
struct EventWrite
{
    /// The bytes written from the beginning of what was given. 0 without `failed`: nothing could
    /// be written now (an output that does not block is full); the call has waited a moment.
    std::size_t bytes = 0;
    /// The output cannot be written any more (closed, an error): `error` says why, in a few words.
    bool failed = false;
    std::string error{};
};

struct EventStreamOptions
{
    /// A "heartbeat" event is written when no line has been written for this long, from the
    /// first line to the result. 0: none.
    std::chrono::milliseconds heartbeat{1000};
    /// The reader is given up (the stream closes) when a line is being written and `write` has
    /// taken nothing for this long. The time starts when a write begins to wait, that is when the
    /// output is full, not when the reader stops reading; a write that waits passes on a piece of
    /// up to 4096 bytes as a whole, so a reader must take about that much within this time to be
    /// seen reading (one byte, if the output does not block). result() waits at most this long
    /// for the result to be written, also for a reader that reads.
    std::chrono::milliseconds stall_timeout{30000};
    /// "log" events wait in the queue up to these limits; more are dropped and counted.
    std::size_t max_log_lines = 4096;
    std::size_t max_log_bytes = std::size_t{1} << 20;
    /// The other events are never dropped: the stream closes when more than this many wait.
    std::size_t max_other_events = 4096;
    /// The time for "t_ms" (empty: a steady clock from the stream's construction).
    std::function<std::int64_t()> now_ms{};
};

/// The events of `run --events`: one JSON object per line, each with "event", "seq" and "t_ms".
///
/// The threads. Any thread makes events (main; the reader of each participant's output; the reader
/// of standard input): it puts a line into a queue and returns. One thread, the writer, takes the
/// lines from the queue and writes them with `write`; it is the only thread that ever waits for
/// the reader of the events. A third, the watch, sees a reader that does not read, and tells the
/// caller (`on_closed`) when the stream closes. Both belong to the stream and are detached.
///
/// Invariants.
/// - A thread that makes an event waits for nothing but the queue's mutex, which no thread holds
///   while it writes, waits or calls back. A reader that is slow, has stopped or has gone never
///   holds up the run, the participants (their output is read all the same) or a stop request.
/// - The lines are written in the order of the calls. "seq" is given when a line is written: 0, 1,
///   2, ... without a gap in what is written. "t_ms" is the time of the call (wall-clock
///   milliseconds since the stream was made); it never goes back from line to line.
/// - Only "log" events are ever dropped, and only while more than max_log_lines or max_log_bytes
///   of them wait. A "log_dropped" event stands where they would have been ("count", and "sources":
///   how many of which source), with the numbers known when it is written.
/// - Any other event is written, or the stream closes. It closes (once, for good) when `write`
///   fails; when a line is being written and `write` has taken nothing for stall_timeout; when
///   more than max_other_events other events wait; when result() gives up. A closed
///   stream takes no more events and writes nothing more (a line may have been cut). `on_closed`
///   is called once, on the watch's thread, with the reason; not if the result had been given
///   (the run is over then: result() returns false instead).
/// - A "heartbeat" event is written when nothing else has been written for
///   EventStreamOptions::heartbeat, after the first line and before the result: the reader sees
///   that the launcher lives, and the launcher sees within that time that its reader has gone.
/// - The result is the last line: after result() nothing is accepted, and nothing follows it.
///
/// What may wait, and for how long. A maker of events: no wait. The writer: in `write`, as long as
/// the reader lets it (nothing waits for the writer without a limit; when the stream closes it is
/// left behind). The watch: in `on_closed`. result(): until the result is written or the stream
/// closes, at most stall_timeout (a reader that reads so slowly that what waits takes longer does
/// not get the result), less if `give_up` says so. The destructor: at most a few seconds
/// for the two threads to end (they share the stream's state, so an output that still blocks is
/// left to them; `write` and `on_closed` must stay callable until they return).
///
/// The end. Nothing joins the two threads. The writer ends when it has written the result or the
/// stream has closed, the watch with it; the destructor only tells them to end. vehicle_sil_launch
/// never destroys its stream: its process ends when main returns, whatever the two threads wait
/// for.
///
/// The reader, case by case. It reads: every event, in order. It is slow: lines wait in the queue;
/// "log" events over the limits are dropped and counted. It has stopped: the same, and after
/// stall_timeout the stream closes. It has gone: `write` fails (at the latest with the next
/// heartbeat), and the stream closes. The systems differ only in `write` (tools/launch/main.cpp):
/// a POSIX write() to a full pipe blocks, or fails with EAGAIN if the pipe has O_NONBLOCK (then
/// `write` waits a moment for room and says that it wrote nothing); a Windows WriteFile() to a full
/// pipe blocks, or writes 0 bytes if the pipe is in no-wait mode (the same answer), and a write to
/// a console blocks while the console holds its output; a closed pipe fails the write on both
/// (SIGPIPE is ignored).
class EventStream
{
public:
    /// Writes the beginning of `bytes` to the output (as much as it takes); may block while the
    /// reader does not read. Called by the writer thread only.
    using Write = std::function<EventWrite(std::string_view bytes)>;
    /// The stream has closed before its result: `reason` in a few words.
    using OnClosed = std::function<void(const std::string& reason)>;

    EventStream(Write write, OnClosed on_closed, EventStreamOptions options = {});
    ~EventStream();

    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    /// The first event. `file`: the configuration file as given, or "" (null) if unknown.
    void hello(long pid, const std::string& file, const std::vector<std::string>& arguments);
    void phase(Phase phase);
    /// The check's result, as `check --json` writes it.
    void check(const CheckReport& report);
    void registry(const std::string& uri);
    /// "control_server": the run's control server listens at `address` (gRPC, the actual port),
    /// with TLS or not, with credentials (every call needs a token) or not.
    void control_server(const std::string& address, bool tls, bool credentials);
    /// Disabled, Joined, StopRequested, Killed.
    void participant(const std::string& name, ParticipantState state);
    void participant_not_started(const std::string& name, const std::string& message);
    /// Failed. `reason`: "left_simulation" (it left the simulation while its process runs),
    /// "process_ended" (its process ended as a failure while the simulation ran) or
    /// "ended_before_start" (its process ended before the simulation started). `message`: its
    /// finding's message if that is settled, or "" (no "message").
    void participant_failed(const std::string& name, std::string_view reason, const std::string& message);
    /// `command`: the executable as started and its arguments; `log`: its log file, or "" (null).
    void participant_started(const std::string& name, long pid, const std::vector<std::string>& command,
                             const std::string& log);
    void participant_ended(const std::string& name, const process::ExitStatus& status);
    /// Manual: `command` the executable and its arguments, `working_directory` where to start it
    /// (both as shown in the outputs), `command_line` the same as one line for this system's shell,
    /// `environment` the variables to set for it (name, value; "environment" only if there are any).
    void participant_manual(const std::string& name, const std::vector<std::string>& command,
                            const std::string& working_directory, const std::string& command_line,
                            const std::vector<std::pair<std::string, std::string>>& environment = {});
    /// "control_link": where a started participant's control agent stands when the simulation
    /// has started: `state` is control_link_name(); "variables" with "connected" (how many its
    /// agent offers), "version" with "version_mismatch" (the version of the messages it speaks).
    void control_link(const std::string& name, std::string_view state, std::optional<std::size_t> variables,
                      std::optional<int> version);
    /// "scenario_step" (run --scenario): a step of the scenario began ("status": "running") or was
    /// decided ("passed", "failed"), with the members of the result's "scenario"."steps".
    void scenario_step(const ScenarioStepRun& step);
    /// One line of a participant's output (`source`: its name) or of the launcher
    /// ("vehicle_sil_launch"), as the text output shows it but without "[<source>] ". The only
    /// event that may be dropped (see above).
    void log(const std::string& source, const std::string& line);
    /// One of the run's findings, with the fields of the results' "findings".
    void finding(const std::string& file, const Finding& finding);
    /// The simulation's virtual time as the controller sees it, and how it goes.
    void time(const TimeEvent& time);
    /// A command from standard input (`source` "stdin"), its end ("eof"), or a call of a control
    /// client ("control_server").
    void command(const Command& command, std::string_view source);
    /// The last event: the run's result as `run --json` writes it. Waits until it has been written
    /// (true) or the stream has closed (false): at most stall_timeout, whether the reader reads
    /// or not. While nothing could be written for half a second, `give_up` is asked every few
    /// milliseconds, and true closes the stream (`give_up` takes the result from no reader that
    /// reads).
    bool result(const RunReport& report, const std::function<bool()>& give_up = {});

    /// Waits up to `timeout` until every event given so far has been written; false if the time
    /// is over or the stream has closed.
    bool wait_written(std::chrono::milliseconds timeout);
    /// How many events but "log" wait to be written now (takes only the queue's mutex). The run
    /// makes no "time" event that says only that the time has moved while some wait: a reader that
    /// falls behind is not given up because of them (max_other_events).
    [[nodiscard]] std::size_t waiting_events() const;
    [[nodiscard]] bool closed() const;
    /// Why it closed, or "".
    [[nodiscard]] std::string close_reason() const;

private:
    enum class Kind
    {
        Log,
        Other,
        Result,
    };
    /// Queues {"event":<event>,"seq":..,"t_ms":..,<the members of `object`>}; `object` is a compact
    /// JSON object. false if it was not taken (closed, after the result, a dropped "log").
    bool emit(std::string_view event, std::string object, Kind kind = Kind::Other, const std::string& source = {});

    struct State;
    static void write_lines(const std::shared_ptr<State>& state);
    static void watch(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
};

} // namespace vehicle_sil::launch
