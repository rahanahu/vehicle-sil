// SPDX-License-Identifier: MIT
//
// The controls of a run: what can be asked of it from outside while it lasts (stop it, move its
// time) and where it stands (its phase), in one object that the run borrows (RunControls). Used by
// the run (run.hpp: RunLent::controls). Internal.
#pragma once

#include "vehicle_sil/runner/stop_requests.hpp"
#include "vehicle_sil/runner/time_commands.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::runner {

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

/// Whether a run keeps up with the speed set for it.
enum class Pacing
{
    NotPaced, ///< nothing to keep up with: as fast as possible, standing still, or not running
    OnTime,   ///< it runs at the speed set
    Late,     ///< it is behind real time now, or was within the last seconds (PacingWatch of the run)
};
/// "not-paced", "on-time", "late".
[[nodiscard]] const char* pacing_name(Pacing pacing);

/// Where a run stands and what it takes now: the discrete facts only (no time that moves, no
/// number that grows), as the event "state" writes them (docs/simulation-events.md). The one
/// place that says so: a reader does not work it out from the other events.
struct RunState
{
    /// Counts the states that RunControls::publish() has told: 1, 2, ... A reader that sees a gap
    /// has skipped states (only the latest waits to be written), which loses nothing.
    std::uint64_t rev = 0;
    Phase phase = Phase::Checking;
    /// TimeFacts::standing.
    bool paused = false;
    /// Where it stands still (TimeFacts::standing_at_ms): empty while `paused` is false, and while
    /// the simulation has not come to a stand yet.
    std::optional<std::int64_t> paused_at_ms{};
    /// The run waits in the hold before its first step.
    bool holding = false;
    /// The speed set (TimeFacts::speed): the outer empty, not known yet; the inner empty, max.
    std::optional<std::optional<double>> speed{};
    Pacing pacing = Pacing::NotPaced;
    /// The stop requests so far: 0, 1 (the run stops in order) or 2 (it does not wait for the
    /// participants; more requests than two say nothing more).
    int stop_requests = 0;
    /// What asked first (RunControls::set_stop_name(); the result's interrupted_by), or "".
    std::string stopped_by{};
    /// The run plays a scenario.
    bool scenario = false;
    /// What the run takes now (runner::allowed()), each as from the entry that has the operation:
    /// pause, resume, step, run_until, speed and stop as from standard input; write as from a
    /// control server (standard input has none). In a run without a control server
    /// (RunControls::set_control_server()) nothing can send a write: refused, with
    /// reason::kNoControlServer.
    struct Operations
    {
        Allowed pause, resume, step, speed, run_until, stop, write;
        [[nodiscard]] bool operator==(const Operations&) const = default;
    } allowed{};
    /// What each bridge of --connect stands at (docs/simulation-events.md, "outside" of the state):
    /// facts that change in steps, in the order of the bridges in the plan. Empty without --connect.
    /// Set by RunControls::set_outside(); a number that keeps growing is not here (the event "outside").
    struct Outside
    {
        std::string network{};
        std::string target{};      ///< as --connect gave it
        std::string participant{}; ///< the bridge's name (bridge.<network>)
        /// "up", "down" or "unknown" as the bridge's link line says; empty: it has not said.
        std::string link{};
        /// "active", "passive", "bus-off" or "unknown"; empty: the bridge has no bus state (Ethernet) or has not said.
        std::string bus{};
        /// The bridge sent frames to the bus and nothing came from a device there for
        /// kOutsideNothingReceivedAfter (detail::sent_without_answer(): the note connect-nothing-received).
        bool nothing_received = false;
        /// "ok", "interrupted" (the link is down or the bus is off) or "degraded" (the rest);
        /// interrupted wins when both hold.
        std::string condition = "ok";
        /// Why it is not "ok", in the fixed order: link-down, bus-off, bus-errors, frames-dropped,
        /// loop-suspected, device-shared. Empty when ok.
        std::vector<std::string> reasons{};
        [[nodiscard]] bool operator==(const Outside&) const = default;
    };
    std::vector<Outside> outside{};

    [[nodiscard]] bool operator==(const RunState&) const = default;
};

/// The controls of one run: the stop requests (borrowed), the time commands and the phase.
/// Whoever asks something of the run from outside (the reader of standard input, the threads of a
/// control server) and the run itself meet here; nothing here knows the events or SIL Kit.
///
/// Lifetime: one object serves one run. Its time commands refuse everything once the run has
/// detached them, and nothing attaches them again, so a second run needs controls of its own; the
/// stop requests are the part that several runs share. The caller of run_file() makes it before
/// the run and keeps it for as long as any of its threads may use it (vehicle_sil_launch run:
/// never destroyed, as the reader of standard input uses it until the process ends). Without
/// RunLent::controls the run makes its own for that run.
///
/// The state (RunState) is put together here from its owners: the phase and the pacing (told by
/// the run), the stop requests, and the time commands' facts. publish() tells Observers::on_state
/// when it differs from the one told last. It is called by whatever changed something: the time
/// commands after each command, attach, detach and the hold (TimeCommands::OnChange), set_phase(),
/// and the run's watch on every look (which is how a stop request of a signal handler, which
/// must not call anything here, and the time gate's standing still or going on get into it).
///
/// Threads and locks: stops() and time() return the same objects for the whole lifetime, and each
/// keeps itself (StopRequests: atomics; TimeCommands: its mutex). phase() may be read from any
/// thread. set_phase() and set_pacing() are the run's alone, one call at a time. This object has
/// no mutex of its own: the state told last and its `rev` are touched only under TimeCommands'
/// mutex (in its OnChange and in the `f` of its look()), and `on_state` is called there, so the
/// states are told one at a time, each after the command event of the command that made it. The
/// order of the locks stays: TimeCommands' mutex, then the event stream's or the time gate's.
/// set_outside() takes TimeCommands' mutex too, and its caller (RunOutput) holds its own mutex then:
/// RunOutput's mutex, then TimeCommands' (see run.cpp, "The locks"). Nothing under TimeCommands'
/// mutex calls RunOutput, so the reverse order never happens.
/// set_phase() calls `on_phase` with no lock, then publish(). `on_command` is called under
/// TimeCommands' mutex (TimeCommands::OnCommand).
///
/// What takes TimeCommands' mutex here (allowed(), publish(), state(), set_phase()) must not be
/// called from what runs under it: the observers `on_command` and `on_state`, the `accepted` of
/// TimeCommands::apply(), the `f` of TimeCommands::inspect() and look(), on_detach. Such a call
/// would wait for itself. Nor from a signal handler.
class RunControls
{
public:
    /// What is told of the controls, fixed when they are made. Either may be empty.
    struct Observers
    {
        /// Each time command as decided, before it is done: TimeCommands::OnCommand (the writer of
        /// the command events).
        TimeCommands::OnCommand on_command{};
        /// Each phase the run enters, on the run's thread, after phase() has it (the writer of the
        /// phase events). Must not call set_phase().
        std::function<void(Phase phase)> on_phase{};
        /// Each state that differs from the one told last (the writer of the state events), under
        /// TimeCommands' mutex: short, must not wait, must not call this object or its time
        /// commands.
        std::function<void(const RunState& state)> on_state{};
    };

    /// `stops`: the stop requests, the caller's; they must outlive this object. `start_paused`:
    /// run --start-paused (TimeCommands). Without `observers`: none. `scenario`: the run plays a
    /// scenario (the rules: ControlSituation::scenario). Both are the run's (RunIntent::start_paused,
    /// RunIntent::scenario): run_file() refuses controls lent for a run whose request says otherwise.
    RunControls(StopRequests& stops, bool start_paused, Observers observers, bool scenario);
    /// Controls without observers, for a run that plays no scenario.
    explicit RunControls(StopRequests& stops, bool start_paused = false);
    RunControls(const RunControls&) = delete;
    RunControls& operator=(const RunControls&) = delete;

    /// What this object was made with: run --start-paused, and whether the run plays a scenario.
    [[nodiscard]] bool start_paused() const noexcept { return start_paused_; }
    [[nodiscard]] bool scenario() const noexcept { return scenario_; }
    /// The stop requests this object was made with.
    [[nodiscard]] StopRequests& stops() noexcept { return stops_; }
    [[nodiscard]] const StopRequests& stops() const noexcept { return stops_; }
    /// The time commands of the run.
    [[nodiscard]] TimeCommands& time() noexcept { return time_; }

    /// Whether Observers::on_command / on_phase was given (run_file() needs both to be given when
    /// it also has an EventStream: the run does not write those events by itself then).
    [[nodiscard]] bool has_command_observer() const noexcept { return has_command_observer_; }
    [[nodiscard]] bool has_phase_observer() const noexcept { return static_cast<bool>(on_phase_); }
    [[nodiscard]] bool has_state_observer() const noexcept { return static_cast<bool>(on_state_); }
    /// Whether `operation` from `entry` would be taken now, and the range of its argument: the
    /// answer of the rules (control_rules.hpp) that the time commands decide each command with
    /// (TimeCommands::allowed()). Any thread, but not from what runs under TimeCommands' mutex
    /// (see above: it takes that mutex).
    [[nodiscard]] Allowed allowed(Operation operation, Entry entry) { return time_.allowed(operation, entry); }
    /// Whether `operation` from `entry` is refused for the whole run, and why
    /// (TimeCommands::always_refused()): a control server then refuses its clients' writes itself.
    [[nodiscard]] std::optional<Decision> always_refused(Entry entry, Operation operation) const
    {
        return time_.always_refused(entry, operation);
    }
    /// Whether this run is connected outside the simulation and keeps to real time
    /// (TimeCommands::set_time_lock()): once, right after the configuration has been read. Until
    /// then the time commands wait undecided; this decides them, each with its command event.
    void set_time_lock(const config::TimeLock& lock) { time_.set_time_lock(lock); }

    /// The phase the run entered last; Phase::Checking before it has said any.
    [[nodiscard]] Phase phase() const noexcept { return phase_.load(); }
    /// The run enters `phase`: phase() has it from now on, then Observers::on_phase is called,
    /// then publish().
    void set_phase(Phase phase);

    /// The run ends before it could give the time lock (its configuration could not be read, so
    /// nothing starts): as set_phase(Phase::Ended), with the time commands detached before the
    /// state is told. The commands that waited for the lock are refused there, each with its
    /// command event, and one state tells both the end and that no time command is taken any
    /// more. The run's alone, as set_phase().
    void end_without_time_lock();

    /// How the first stop request is called in the state (RunState::stopped_by): as the result's
    /// interrupted_by (RunObservers::interrupt_name). Without it: "interrupt". Called while the
    /// state is put together, under TimeCommands' mutex: short, no wait. Set before the run and
    /// before any other thread uses this object.
    void set_stop_name(std::function<std::string()> name) { stop_name_ = std::move(name); }
    /// Whether the run has a control server (RunHost::control), for the state: without
    /// one, RunState::allowed refuses what only a control server's client can send (write).
    /// Not given: none. It changes no rule and no command's answer. Set before the run and
    /// before any other thread uses this object.
    void set_control_server(bool present) noexcept { control_server_ = present; }
    /// The run's alone: whether it keeps up with its speed, as its watch saw it last. publish()
    /// tells it.
    void set_pacing(Pacing pacing) noexcept { pacing_.store(pacing); }
    /// The state of the bridges of --connect, as RunOutput has it now (RunState::outside): kept for
    /// state(), and publish() tells it if it differs from the last. Takes TimeCommands' mutex, so
    /// the caller may hold RunOutput's mutex but nothing that is taken under TimeCommands' mutex
    /// (not from what runs under it, see above). Any thread.
    void set_outside(std::vector<RunState::Outside> outside);
    /// Puts the state together and, if it differs from the one told last (but for `rev`), counts
    /// `rev` up and tells Observers::on_state. Any thread (not from what runs under TimeCommands'
    /// mutex, not from a signal handler). Cheap when nothing changed.
    void publish();
    /// The state as it is now, with the `rev` of the one told last (0: none yet). Any thread, as
    /// publish().
    [[nodiscard]] RunState state();

private:
    /// The state of `facts`, the phase, the pacing and the stop requests, without `rev`. Under
    /// TimeCommands' mutex.
    [[nodiscard]] RunState compose(const TimeFacts& facts) const;
    /// compose(), and on_state if it differs from told_. Under TimeCommands' mutex.
    void tell(const TimeFacts& facts);

    StopRequests& stops_;
    TimeCommands time_;
    std::atomic<Phase> phase_{Phase::Checking};
    std::atomic<Pacing> pacing_{Pacing::NotPaced};
    const std::function<void(Phase phase)> on_phase_;
    const std::function<void(const RunState& state)> on_state_;
    std::function<std::string()> stop_name_{};
    bool control_server_ = false;
    const bool start_paused_;
    const bool scenario_;
    const bool has_command_observer_;
    /// The state told last (rev 0: none). Under TimeCommands' mutex.
    RunState told_{};
    /// set_outside(). Under TimeCommands' mutex.
    std::vector<RunState::Outside> outside_{};
};

} // namespace vehicle_sil::runner
