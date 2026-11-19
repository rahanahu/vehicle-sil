// SPDX-License-Identifier: MIT
//
// What a run saw (RunOutcome), as a value in parts, and the pure functions that judge it
// (assess_run(), end_reason()). The run's result and its outputs: run.hpp. Internal; the
// behaviour and the output: docs/simulation-run.md.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/runner/stop_requests.hpp"
#include "vehicle_sil/runner/time_commands.hpp"
#include "vehicle_sil/config/time_plan.hpp"
#include "vehicle_sil/scenario/timeline.hpp"
#include "vehicle_sil/runner/traffic.hpp"
#include "vehicle_sil/process/process.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::runner {

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
    std::optional<scenario::ScenarioRun> result{};
    /// The scenario's thread failed unexpectedly: why (the scenario is Incomplete).
    std::string error{};
    /// The timeline file as the user named it (RunHost::timeline), once it was opened; ""
    /// without one.
    std::string timeline{};
    /// What the scenario's engine noted for the timeline; null if the scenario did not begin.
    std::shared_ptr<const scenario::TimelineRecord> timeline_record{};
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
    config::RunTiming timing{};
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
    config::TimeLock time_lock{};
    /// The participants of the file that ran in real time (detail::real_time_peers()).
    std::vector<std::string> real_time_peers{};
    /// What went over the networks; empty if the simulation did not start.
    std::optional<RunTraffic> traffic{};
};

/// Judges a run: the errors (they fail it), warnings and notes, each about the participant (with
/// its line) or the simulation, in sort_findings() order.
[[nodiscard]] std::vector<config::Finding> assess_run(const RunOutcome& outcome);

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

namespace detail {
/// The findings of assess_run(`outcome`) that are settled while the participants are still being
/// ended: whatever happens to them from here on, assess_run() of the final outcome has each of
/// these unchanged. (Left out: a participant that failed while its process still runs, whose
/// finding says how the process ended.) What run --events writes before the participants end.
[[nodiscard]] std::vector<config::Finding> settled_findings(const RunOutcome& outcome);
} // namespace detail

} // namespace vehicle_sil::runner
