// SPDX-License-Identifier: MIT
//
// The time commands of a run (pause, resume, step, run_until, speed) from standard input and a
// control server, on their way to the simulation's time gate (TimeCommands). Used by the run
// through its controls (run_controls.hpp). Internal; the commands: docs/simulation-events.md.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/runner/command.hpp"
#include "vehicle_sil/runner/control_rules.hpp"
#include "vehicle_sil/config/time_plan.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vehicle_sil {
class SimulationController;
} // namespace vehicle_sil

namespace vehicle_sil::launch {

/// A time command that changed how the simulation's time goes: a pause (also at a time), a step,
/// a run_until, or a speed other than 1 (also max). A resume, a speed of 1 and run --start-paused
/// itself are none.
struct TimeChange
{
    std::string command;     ///< "pause", "step", "run_until" or "speed"
    std::string source;      ///< as apply() was given it ("stdin", "control_server")
    std::int64_t sim_ms = 0; ///< the virtual time when it was accepted (0 before the start)
    /// step: how far (whole steps of the controller) and where it was to stand still after it;
    /// pause at a time, run_until: where it was to stand still. As the command event says.
    std::optional<std::int64_t> ms{};
    std::optional<std::int64_t> at_ms{};
    std::optional<double> speed{}; ///< speed: its value (empty: max)
};

/// How many commands wait at most for the time lock (TimeCommands::set_time_lock()); one more is
/// refused at once.
inline constexpr std::size_t kMaxWaitingCommands = 256;

/// What the time commands know of where the run stands, at one moment: the part of the run's
/// state (RunState of run_controls.hpp) that changes under their mutex.
struct TimeFacts
{
    /// What the rules look at (allowed() answers from it).
    ControlSituation situation{};
    /// The run waits in the hold before its first step.
    bool holding = false;
    /// The simulation stands still: attached, the controller's time gate does (or is at the time
    /// it is to stand still at), or the run holds; before that,
    /// whether it will stand still at 0 when it starts (--start-paused, a pause); false once
    /// detached.
    bool standing = false;
    /// The virtual time (ms) at which it stands still: while attached and the gate stands still
    /// (the time of the step it stands before), or the run holds (0). Empty while `standing` is
    /// false, and while it is true before the simulation has come to a stand (before the
    /// controller exists; between a pause at a time that has passed and the next step, where the
    /// gate stops).
    std::optional<std::int64_t> standing_at_ms{};
    /// The speed set: of the controller's gate while attached, the last one known after that;
    /// before, the last speed command's. The outer empty: not known yet (no speed command, and
    /// the run has not read its configuration's speed into the controller). The inner empty: max.
    std::optional<std::optional<double>> speed{};
};

/// The time commands (pause, resume, step, run_until, speed) on their way to the simulation's time
/// gate, from every source: the reader of standard input (`run --stdin-commands`) and the threads
/// of a control server (`run --control`). Thread safe. The commands of all sources go through one
/// mutex: each is decided, written as a command event and done before the next, so the order of
/// the command events is the order in which they took effect. apply() never waits for the run or
/// the reader of the events: only for this object's mutex and the gate's (neither is held while
/// something waits).
///
/// Whether a command is taken is the rules' answer (control_rules.hpp: decide()) for where this
/// object stands; allowed() gives the same answer without doing anything.
///
/// The time lock comes first. Whether the run's time is locked (it is connected outside the
/// simulation: TimeLock) is known only once the run has read its configuration, and a command
/// that came before must not be decided without it. So until set_time_lock() every command but a
/// stop waits, undecided, in the order it came (at most kMaxWaitingCommands; one more is refused
/// at once with reason::kNotSetUp): apply() returns it with Command::pending and writes no event.
/// set_time_lock() then decides the commands that waited, in that order, each with its command
/// event, before any command that comes after: the order of the events is still the order in
/// which the commands took effect, only later than they came. If the lock never comes (the
/// configuration could not be read, so nothing starts), detach() decides them: they are refused
/// as every command is once detached (reason::kNotRunning). A stop never waits. A TimeCommands
/// that nothing gives the lock answers no command: the run does (run_file()), and a test gives
/// TimeLock{} for a run that is not connected outside.
///
/// Before the run attaches its controller (the check, the start), what the commands ask for is
/// kept and given to the controller when it is attached (attach() needs the lock to be given). While attached, each command goes to the
/// gate at once. Once detached (the simulation is over, or did not start), the rules refuse the
/// commands (but a stop from standard input, which is a stop request whenever it comes).
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
    /// What apply() calls with each command as decided and its source, under this object's mutex,
    /// before the command is done: the writer of the command events (EventStream::command()). Must
    /// not wait and must not call this object.
    using OnCommand = std::function<void(const Command& command, std::string_view source)>;

    /// `start_paused`: run --start-paused (a step is accepted before anything has been paused).
    /// `on_command`: where apply() writes the command events (may be empty). `scenario`: the run
    /// plays a scenario (ControlSituation::scenario).
    explicit TimeCommands(bool start_paused = false, OnCommand on_command = {}, bool scenario = false);
    ~TimeCommands();
    TimeCommands(const TimeCommands&) = delete;
    TimeCommands& operator=(const TimeCommands&) = delete;

    /// Decides whether the time command (Command::is_time()) can be done now (decide(), with
    /// entry_of(source) and the command's arguments); writes its command
    /// event (`source`) with that answer (and, for step, how far the simulation goes: "ms" rounded
    /// up to whole steps of the controller, and where it will stand still after it: "at_ms", counted
    /// from where it stands or, while it still goes, from the time the commands before sent it to;
    /// for run_until and pause with at_ms, where it will stand
    /// still: "at_ms" rounded up the same way), then does it, then tells OnChange. The event comes
    /// before what the command changes. Returns the command as written; one that was refused has
    /// the code of the row that refused it (Command::reason) and its message. What is refused is
    /// the table's alone to say (the rows of control_rules.cpp). A "stop" is only decided and
    /// written here; the caller asks the run to stop.
    ///
    /// Before set_time_lock() (and detach()), a command other than a stop is not decided here: it
    /// is returned with Command::pending (not accepted, no reason), and its event, its effect and
    /// OnCommand come when the lock is given (see above). The command events are the answer then.
    ///
    /// `source` must be the entry_name() of an entry ("stdin", "control_server"): a command of
    /// any other source is refused (reason::kUnknownSource) and written with that source, so that
    /// a misspelt or new source is never taken by the rules of another entry.
    Command apply(const Command& command, std::string_view source);
    /// As apply(command, source), and if the command is accepted, `accepted` is called before
    /// apply() returns, under this object's mutex (short, must not wait): before any later
    /// command and before detach(). For a command that waits for the time lock, a copy of
    /// `accepted` is kept and called when the command is decided and accepted (on the thread that
    /// gives the lock): what it refers to must live until then. A control server's stop asks the run to stop there, so that
    /// a stop that is accepted reaches the run before the simulation is over.
    Command apply(const Command& command, std::string_view source, const std::function<void()>& accepted);

    /// Whether `operation` from `entry` would be taken now, and the range of its argument
    /// (launch::allowed()): what apply() decides for a command of that operation whose arguments
    /// are in range, at this moment (another thread's command may change it right after). Before
    /// set_time_lock(), when apply() decides nothing yet: the answer without a lock, which the
    /// lock may change. Changes nothing and writes nothing. Takes this object's mutex: not to be called from what runs
    /// under it (OnCommand, OnChange, `accepted`, the `f` of inspect() and look(), on_detach), which
    /// would wait for itself.
    [[nodiscard]] Allowed allowed(Operation operation, Entry entry);

    /// What is called, under this object's mutex, whenever the facts may have changed: at the end
    /// of apply() (after the command is done, also for one that was refused), attach(), detach(),
    /// set_time_lock() (if the run is connected outside, and after each command it or detach()
    /// decides), and when a hold begins or ends. It must be short, must not wait and must not
    /// call this object; it may take the event stream's mutex (as OnCommand). Set once,
    /// before any other thread uses this object (RunControls sets it in its constructor).
    using OnChange = std::function<void(const TimeFacts& facts)>;
    void set_on_change(OnChange on_change);
    /// Calls `f` once with the facts as they are now, under this object's mutex (the rules for
    /// `f`: as OnChange). Reads the controller's time gate while attached.
    void look(const std::function<void(const TimeFacts& facts)>& f);
    /// launch::always_refused() for this run: whether `operation` from `entry` is refused for the
    /// whole run. The same answer whenever it is asked.
    [[nodiscard]] std::optional<Decision> always_refused(Entry entry, Operation operation) const;
    /// Whether this run is connected outside the simulation, and whether that keeps it to real time
    /// (ControlSituation::time_locked for `lock.locked`; time_free for a lock that names something
    /// and is not locked). For the run: once, right after it has read the configuration and
    /// before anything of it starts (run_file(): from the check, before --describe). Tells
    /// OnChange if the run is connected outside (the lock changes what is taken), then decides the commands that waited for it, in their order (see above). The
    /// first call is the run's for good: a later one changes nothing.
    void set_time_lock(const config::TimeLock& lock);

    /// The run's part. attach(): from now on the commands go to `controller` (its time gate,
    /// made with `step_ms`); std::logic_error before set_time_lock(). First it is given the speed
    /// (`speed`, or the last speed command) and the pause (start paused, or the last pause or
    /// resume command, and the steps since). detach(): before the controller goes; the commands
    /// are refused from then on (those that still wait for the time lock too, each with its
    /// event), a hold ends and wait_paused() returns.
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

    /// Where the run stands, for a control server: SettingUp (before attach()), Attached,
    /// Detached.
    using Stage = ControlStage;
    /// While attached, calls `f` with the controller and whether the run holds before its first
    /// step, under this object's mutex (`f` must be short and must not wait). Returns the stage.
    Stage inspect(const std::function<void(SimulationController& controller, bool holding)>& f);

    /// Waits up to `timeout` until the simulation stands still: the time it stands at (0 in the
    /// hold), or empty on the timeout, once detached and once the controller's lifecycle has ended
    /// (as SimulationController::wait_until_paused()).
    std::optional<SimTime> wait_paused(std::chrono::milliseconds timeout);

    /// The first accepted command that changed how the time goes (TimeChange), or empty. apply()
    /// notes it under this object's mutex; the run reads it after detach(), when no command is
    /// accepted any more (the notes outside-time-changed and unsynchronized-time-changed).
    std::optional<TimeChange> first_time_change();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vehicle_sil::launch
