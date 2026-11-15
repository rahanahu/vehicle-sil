// SPDX-License-Identifier: MIT
//
// The time commands of a run (pause, resume, step, run_until, speed) from standard input and a
// control server, on their way to the simulation's time gate (TimeCommands). Used by the run
// (run.hpp: RunOptions::time_commands). Internal; the commands: docs/simulation-events.md.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace vehicle_sil {
class SimulationController;
} // namespace vehicle_sil

namespace vehicle_sil::launch {

class EventStream;
struct Command;

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

} // namespace vehicle_sil::launch
