// SPDX-License-Identifier: MIT
//
// The machine interface of vehicle_sil_launch: the events of `serve` and `serve --once` (one JSON
// object per line) and `version --json`; the commands of `run --stdin-commands` and of `serve` are
// read by command_input.hpp. Internal; the format: docs/simulation-events.md.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/run_sink.hpp"
#include "vehicle_sil/runner/command.hpp"
#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/runner/run_events.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
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

struct ExecutionStatus;
struct PrepareResult;

/// The version of the events (hello's "vehicle_sil_events").
inline constexpr int kEventsFormat = 4;
/// The version of `version --json` (its "vehicle_sil_launch").
inline constexpr int kVersionFormat = 1;
/// What this launcher can do, by name, for a reader that asks "is it there" instead of comparing
/// versions (`version --json`'s "features"). Names are only added.
inline constexpr std::array<std::string_view, 38> kFeatures{"events",
                                                            "stdin-commands",
                                                            "check-variables",
                                                            "speed",
                                                            "start-paused",
                                                            "time-commands",
                                                            "control-links",
                                                            "scenario",
                                                            "manual-participants",
                                                            "scenario-wave",
                                                            "scenario-motion-expect",
                                                            "scenario-points-file",
                                                            "timeline",
                                                            "timeline-lines",
                                                            "scenario-inputs",
                                                            "network-names",
                                                            "run-traffic",
                                                            "j1939-nodes",
                                                            "connect",
                                                            "connections",
                                                            "connect-counts",
                                                            "state",
                                                            "time-policy",
                                                            "stdin-run-until",
                                                            "request",
                                                            "scenario-samples",
                                                            "run-stop-at",
                                                            "serve",
                                                            "check-file-only",
                                                            "serve-prepare",
                                                            "control-metrics",
                                                            "scenario-end-ms",
                                                            "scenario-handover",
                                                            "scenario-cases",
                                                            "scenario-templates",
                                                            "scenario-variable-compare",
                                                            "external-registry",
                                                            "record-signals"};

/// `version`: "vehicle_sil_launch <version>\n".
[[nodiscard]] std::string format_version_text();
/// The feature of a vehicle_sil_launch built with the control server (run --control): only such
/// a build lists it (format_version_json()'s `more`).
inline constexpr std::string_view kControlServerFeature = "control-server";
/// The feature of such a build that has the control server during a scenario too (run --scenario
/// with --control: its clients read, and cannot write). After kControlServerFeature in `more`.
inline constexpr std::string_view kScenarioControlFeature = "scenario-control";
/// The feature of such a build whose control server takes the time operations during a scenario
/// (pause, resume, step, run_until, speed: as standard input does). After kScenarioControlFeature
/// in `more`.
inline constexpr std::string_view kScenarioTimeFeature = "scenario-time";

/// `version --json`: the formats this launcher writes and reads, kFeatures, then `more` (what
/// this build adds).
[[nodiscard]] std::string format_version_json(const std::vector<std::string_view>& more = {});

/// The name of a participant's state in the event "participant": "disabled", "not_started", ...
/// The members a state brings besides: not_started "message"; started "pid", "command", "log";
/// manual "command", "working_directory", "command_line", "environment"; failed "reason",
/// "message"; ended "exit_code", "signal".
[[nodiscard]] const char* participant_state_name(runner::ParticipantState state);

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

/// The events of `serve`: one JSON object per line, each with "event", "seq" and "t_ms".
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
/// - A "state" event is the latest state, not history: while one waits, a newer one takes its
///   place at the end of the queue, so a reader that reads slowly skips the states in between
///   (their "rev" shows the gap). At most one waits, and it does not count for max_other_events.
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
///
/// What a run itself tells is RunEventSink (run_sink.hpp), which extends RunEvents (run_events.hpp)
/// and which this implements: run_file() and the parts of a run call those members and do not
/// know this class. The other members are for whoever runs the launcher's commands (hello, idle,
/// refused, open_run, result).
///
/// serve: one stream for the life of the launcher, which runs one run after the other. Each run
/// has a window (open_run()): a stream object that shares this stream's queue, writer, watch,
/// "seq" and "t_ms", and puts "run":"<run ID>" first in each line it makes. The window's first
/// line is "run_start" and its last its result(), which closes the window but not the stream:
/// whatever is given to the window after that (a thread of the run that is late) is dropped, so
/// no line of a run follows its result. While a window is open, a "heartbeat" has its "run" too,
/// so every line from "run_start" to the result has it; "log_dropped" and "state" belong to the
/// window that made them (a waiting "state" is only replaced by one of the same window). Between
/// the windows the stream writes the launcher's own lines (status(), prepare(), refused(), heartbeats) without
/// "run", and is never sealed: only a result() of the stream itself (the launcher ends at its
/// start) is the last line. One window is open at a time (the caller's rule).
class EventStream : public RunEventSink
{
public:
    /// Writes the beginning of `bytes` to the output (as much as it takes); may block while the
    /// reader does not read. Called by the writer thread only.
    using Write = std::function<EventWrite(std::string_view bytes)>;
    /// The stream has closed before its result: `reason` in a few words.
    using OnClosed = std::function<void(const std::string& reason)>;

    EventStream(Write write, OnClosed on_closed, EventStreamOptions options = {});
    ~EventStream() override;

    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    /// The first event. `file`: the configuration file as given, or "" (null) if unknown.
    /// `launcher`: the launcher's instance (16 hexadecimal digits; each run ID begins with it).
    void hello(long pid, const std::string& file, const std::vector<std::string>& arguments, const std::string& launcher);
    /// serve: a command of standard input that the launcher refused itself (not a run's: no
    /// "run"): the members of command() and "asked_run", the run the command named (null if it
    /// named none).
    void refused(const runner::Command& command, std::string_view source, const std::optional<std::string>& asked_run);
    /// serve: reply to {"command":"prepare"} (no "run").
    void prepare(const PrepareResult& result, std::optional<std::int64_t> id);
    /// serve: spontaneous status transition or reply to {"command":"status"} (no "run").
    void status(const ExecutionStatus& status, std::optional<std::int64_t> id = std::nullopt);
    /// serve: opens the window of the run `run` (see above) and writes its "run_start" with "id",
    /// the run command's number (null without one). The window is open until its result() or
    /// until it is destroyed; it may outlive this object (it shares the stream's state).
    [[nodiscard]] std::unique_ptr<EventStream> open_run(const std::string& run, std::optional<std::int64_t> id);
    void phase(runner::Phase phase) override;
    /// The check's result, as `check --json` writes it.
    void check(const Checked& report) override;
    void registry(const std::string& uri) override;
    /// "control_server": the run's control server listens at `address` (gRPC, the actual port),
    /// with TLS or not, with credentials (every call needs a token) or not.
    void control_server(const std::string& address, bool tls, bool credentials) override;
    /// Disabled, Joined, StopRequested, Killed.
    void participant(const std::string& name, runner::ParticipantState state) override;
    void participant_not_started(const std::string& name, const std::string& message) override;
    /// Failed. `reason`: "left_simulation" (it left the simulation while its process runs),
    /// "process_ended" (its process ended as a failure while the simulation ran) or
    /// "ended_before_start" (its process ended before the simulation started). `message`: its
    /// finding's message if that is settled, or "" (no "message").
    void participant_failed(const std::string& name, std::string_view reason, const std::string& message) override;
    /// `command`: the executable as started and its arguments; `log`: its log file, or "" (null).
    void participant_started(const std::string& name, long pid, const std::vector<std::string>& command,
                             const std::string& log) override;
    void participant_ended(const std::string& name, const process::ExitStatus& status) override;
    /// Manual: `command` the executable and its arguments (empty when `command` is omitted),
    /// `working_directory` where to start it ("" -> JSON null when omitted or not found locally),
    /// `command_line` the same as one line for this system's shell ("" -> JSON null when omitted),
    /// `environment` the variables to set for it (name, value; "environment" only if there are any).
    void participant_manual(const std::string& name, const std::vector<std::string>& command,
                            const std::string& working_directory, const std::string& command_line,
                            const std::vector<std::pair<std::string, std::string>>& environment) override;
    /// "control_link": where a started participant's control agent stands when the simulation
    /// has started: `state` is control_link_name(); "variables" with "connected" (how many its
    /// agent offers), "version" with "version_mismatch" (the version of the messages it speaks).
    void control_link(const std::string& name, std::string_view state, std::optional<std::size_t> variables,
                      std::optional<int> version) override;
    /// "scenario_step" (run --scenario): a step of the scenario began ("status": "running") or was
    /// decided ("passed", "failed"), with the members of the result's "scenario"."steps".
    void scenario_step(const scenario::ScenarioStepRun& step) override;
    /// One line of a participant's output (`source`: its name) or of the launcher
    /// ("vehicle_sil_launch"), as the text output shows it but without "[<source>] ". The only
    /// event that may be dropped (see above).
    void log(const std::string& source, const std::string& line) override;
    /// "outside" (run --connect): what the bridge of `network` has counted so far (the result's
    /// outside[].to_outside and from_outside). Not dropped, unlike "log".
    void outside(const std::string& network, std::uint64_t to_outside, std::uint64_t from_outside) override;
    /// One of the run's findings, with the fields of the results' "findings".
    void finding(const std::string& file, const config::Finding& finding) override;
    /// The simulation's virtual time as the controller sees it, and how it goes.
    void time(const runner::TimeEvent& time) override;
    /// A command from standard input (`source` "stdin"), its end ("eof"), or a call of a control
    /// client ("control_server").
    void command(const runner::Command& command, std::string_view source) override;
    /// "state": where the run stands and what it takes now (RunControls::publish() calls this
    /// when that has changed). Unlike every other event it is no history: only the latest means
    /// anything, so a "state" that still waits to be written is replaced by this one, which goes
    /// to the end of the queue (its "seq" is higher than that of every event made before the
    /// call). It is never counted for max_other_events, and at most one waits.
    void state(const runner::RunState& state) override;
    /// The last event: the run's result as `run --json` writes it. `members`: the event's members
    /// as a compact JSON object, {"result":{...}} (format_result_event() of run.hpp writes it from
    /// the run's report). On a run's window (open_run()): the window's last line, which closes the
    /// window and, when `seal` is true (`launcher_ends: true`), also seals the stream so no line
    /// follows it. Waits until it has been written
    /// (true) or the stream has closed (false): at most stall_timeout, whether the reader reads
    /// or not. While nothing could be written for half a second, `give_up` is asked every few
    /// milliseconds, and true closes the stream (`give_up` takes the result from no reader that
    /// reads).
    bool result(std::string members, const std::function<bool()>& give_up = {}, bool seal = false);
    void enqueue_result(std::string members, bool seal = false) override;
    bool wait_result(const std::function<bool()>& give_up = {}, bool seal = false) override;

    /// Waits up to `timeout` until every event given so far has been written; false if the time
    /// is over or the stream has closed.
    bool wait_written(std::chrono::milliseconds timeout);
    /// If the stream has closed before its result (so `on_closed` runs on the watch thread), waits
    /// up to `timeout` for `on_closed` to finish; true if the stream is not closed, if `on_closed`
    /// need not run (sealed or empty), or once `on_closed` has returned; false if `timeout` ran out
    /// first. Must not be called from `on_closed` itself.
    bool wait_on_closed(std::chrono::milliseconds timeout);
    /// How many events but "log" wait to be written now (takes only the queue's mutex). The run
    /// makes no "time" event that says only that the time has moved while some wait: a reader that
    /// falls behind is not given up because of them (max_other_events).
    [[nodiscard]] std::size_t waiting_events() const override;
    [[nodiscard]] bool closed() const;
    /// Why it closed, or "".
    [[nodiscard]] std::string close_reason() const;

private:
    enum class Kind
    {
        Log,
        Other,
        State,
        Result,
        ResultFinal, ///< a window's result that also ends the stream (launcher_ends: true)
        Open,        ///< a window's "run_start": an Other that opens the window for the heartbeats
    };
    /// Queues {"event":<event>,"seq":..,"t_ms":..,<the members of `object`>}; `object` is a compact
    /// JSON object; a window puts its "run" first. false if it was not taken (closed, after the
    /// result, a window that is closed, a dropped "log"). `order`: set to the line's place in the
    /// queue (result() waits for it).
    bool emit(std::string_view event, std::string object, Kind kind = Kind::Other, const std::string& source = {},
              std::uint64_t* order = nullptr);

    struct State;
    struct Window;
    /// A run's window on `state` (open_run()).
    EventStream(std::shared_ptr<State> state, std::shared_ptr<Window> window);
    static void write_lines(const std::shared_ptr<State>& state);
    static void watch(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
    /// Set: this object is a run's window (open_run()), not the stream.
    std::shared_ptr<Window> window_;
    std::uint64_t result_order_ = 0;
    bool result_enqueued_ = false;
};

} // namespace vehicle_sil::launch
