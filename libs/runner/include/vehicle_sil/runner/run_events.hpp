// SPDX-License-Identifier: MIT
//
// What a run tells of itself while it lasts: the interface the parts of a run call (RunEvents),
// and the values they pass. The run does not know how this is written: the events of
// `run --events` (EventStream, events.hpp) are one implementation. Also the caller's functions
// that the run calls back (RunObservers), and where a control server listens (ControlServerInfo).
// Internal.
#pragma once

#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/process/process.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::control {
class Hub;
} // namespace vehicle_sil::control

namespace vehicle_sil::runner {

/// The event "time": the virtual time and how it keeps up. Whether the simulation stands still and
/// the speed set are in the event "state" (RunState), not here.
struct TimeEvent
{
    std::int64_t sim_ms = 0;
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

/// A participant's state as the run tells it. The parts of a run pass Joined, StopRequested and
/// Killed to RunEvents::participant(), and each of NotStarted, Started, Manual, Failed and Ended to
/// a call of its own; run_file() tells Disabled before the run begins. How each is written:
/// participant_state_name() in events.hpp.
enum class ParticipantState
{
    Disabled,      ///< left out (enabled: false, --disable): not started
    NotStarted,    ///< its process could not be started
    Started,       ///< its process runs
    Manual,        ///< someone starts it (manual): the launcher does not
    Joined,        ///< it joined the simulation (the controller saw its lifecycle)
    Failed,        ///< the launcher decided that it failed, and ends the run
    Ended,         ///< its process ended
    StopRequested, ///< the launcher asked it to stop (SIGTERM, Ctrl+Break)
    Killed,        ///< the launcher ended it with the processes it started
};

/// What the parts of a run tell while it lasts (run_file(): RunLent::events, as the run sees it).
///
/// Threads: every thread of the run calls it (run.cpp lists them: the main thread, the start
/// window's, the scenario's, each participant's reader thread), some of them under RunOutput's
/// mutex. So an implementation keeps itself, and no member waits: not for a reader of what it
/// writes, and not for a lock of the run (it takes none of them and calls nothing of the run
/// back). The order of the calls is the order of what is told.
///
/// Lifetime: borrowed. It outlives the run and every thread of the run that may still call it.
class RunEvents
{
public:
    RunEvents() = default;
    RunEvents(const RunEvents&) = delete;
    RunEvents& operator=(const RunEvents&) = delete;
    virtual ~RunEvents() = default;

    /// The run's registry listens at `uri`.
    virtual void registry(const std::string& uri) = 0;
    /// The run's control server listens at `address` (gRPC, the actual port), with TLS or not,
    /// with credentials (every call needs a token) or not.
    virtual void control_server(const std::string& address, bool tls, bool credentials) = 0;
    /// Joined, StopRequested, Killed (run_file() also passes Disabled, through EventStream).
    virtual void participant(const std::string& name, ParticipantState state) = 0;
    virtual void participant_not_started(const std::string& name, const std::string& message) = 0;
    /// Failed. `reason`: "left_simulation" (it left the simulation while its process runs),
    /// "process_ended" (its process ended as a failure while the simulation ran) or
    /// "ended_before_start" (its process ended before the simulation started). `message`: its
    /// finding's message if that is settled, or "".
    virtual void participant_failed(const std::string& name, std::string_view reason, const std::string& message) = 0;
    /// `command`: the executable as started and its arguments; `log`: its log file, or "".
    virtual void participant_started(const std::string& name, long pid, const std::vector<std::string>& command,
                                     const std::string& log) = 0;
    virtual void participant_ended(const std::string& name, const process::ExitStatus& status) = 0;
    /// Manual: `command` the executable and its arguments, `working_directory` where to start it
    /// (both as shown in the outputs), `command_line` the same as one line for this system's shell,
    /// `environment` the variables to set for it (name, value).
    virtual void participant_manual(const std::string& name, const std::vector<std::string>& command,
                                    const std::string& working_directory, const std::string& command_line,
                                    const std::vector<std::pair<std::string, std::string>>& environment) = 0;
    /// Where a started participant's control agent stands when the simulation has started:
    /// `state` is control_link_name(); `variables` with "connected" (how many its agent offers),
    /// `version` with "version_mismatch" (the version of the messages it speaks).
    virtual void control_link(const std::string& name, std::string_view state, std::optional<std::size_t> variables,
                              std::optional<int> version) = 0;
    /// run --scenario: a step of the scenario began (StepStatus::Running) or was decided.
    virtual void scenario_step(const scenario::ScenarioStepRun& step) = 0;
    /// One line of a participant's output (`source`: its name) or of the launcher
    /// ("vehicle_sil_launch"), as the text output shows it but without "[<source>] ". The only
    /// thing told here that an implementation may drop.
    virtual void log(const std::string& source, const std::string& line) = 0;
    /// run --connect: what the bridge of `network` has counted so far (the result's
    /// outside[].to_outside and from_outside).
    virtual void outside(const std::string& network, std::uint64_t to_outside, std::uint64_t from_outside) = 0;
    /// One of the run's findings (`file`: the configuration file).
    virtual void finding(const std::string& file, const config::Finding& finding) = 0;
    /// The simulation's virtual time as the controller sees it, and how it goes.
    virtual void time(const TimeEvent& time) = 0;
    /// How many things told but "log" have not got out yet (short, no wait). The run does not
    /// tell that only the time has moved while some wait: a reader that falls behind is not
    /// given up because of them.
    [[nodiscard]] virtual std::size_t waiting_events() const = 0;
};

/// Where the run's control server listens (ControlEndpoint::start()).
struct ControlServerInfo
{
    /// host:port with the port it actually listens on (also for port 0).
    std::string address{};
    bool tls = false;
    /// Every call needs a token of the server's credentials.
    bool credentials = false;
};

/// What a run tells while it lasts, and the one thing it lets its caller do: the caller's
/// functions, which the run calls back. A run keeps its own copy.
struct RunObservers
{
    /// Writes one line (without its end) for the person watching: the participants' output as
    /// "[<name>] <line>", the launcher's own as "[vehicle_sil_launch] <text>". Called from several
    /// threads, one call at a time. Not called when the run tells its events instead (RunEvents;
    /// run_file(): RunLent::events).
    std::function<void(std::string_view line)> write_line{};
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
    /// Asked on the run's thread, every few milliseconds, while the run waits for its own
    /// connection to SIL Kit to close (after the participants have ended): true ends that wait
    /// before its limit (OutcomeClose::not_closed_ms, OutcomeClose::interrupted). `stops_before_close`
    /// is the stop request count when closing began. Empty: the wait ends when a stop request comes
    /// after it began (run, test). serve gives one that is true only when a new stop request comes
    /// while closing that brings the count to 2+ (Force) or the launcher ends, so that one late stop
    /// or an earlier Force does not leave its connection open. Short, no wait, no call into the run
    /// or its controls.
    std::function<bool(int stops_before_close)> close_cut{};
};

} // namespace vehicle_sil::runner
