// SPDX-License-Identifier: MIT
//
// TimeCommands (time_commands.hpp): the time commands of a run (pause, resume, step, run_until,
// speed), from before the simulation is set up until it is over.
//
// State and lock: everything is in TimeCommands::State, guarded by its mutex. Any thread applies
// a command (the thread that reads the commands, a control server's); the run's main thread
// attaches and detaches the controller; the start window's thread holds before the first step.
// Taken while that mutex is held: whatever on_command and on_change take (the event stream's, for
// the "command" and "state" events), the controller's gate's (pause, resume, run_until, set_speed,
// time_state) and whatever on_detach takes.
// hold_before_start() calls on_hold and give_up without it.
//
// The time lock: until set_time_lock() (or detach()) the commands but a stop wait in State::waiting
// and nothing of them is decided; then they are decided in the order they came, under the mutex,
// before any command that comes after (answer_waiting()). So no command is decided without the
// lock, and the order of the command events stays the order in which the commands took effect.
#include "vehicle_sil/launch/time_commands.hpp"

#include "vehicle_sil/launch/config.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::launch {

struct TimeCommands::State
{
    std::mutex mutex;
    /// hold_before_start() and wait_paused(): a command released the hold, or detach().
    std::condition_variable cv;
    /// Called in apply() with each command as decided (the writer of the command events).
    OnCommand on_command{};
    /// A pause was asked for (or the run starts paused) and no resume since.
    bool paused = false;
    /// The last speed command before attach() (the inner empty: "max").
    std::optional<std::optional<double>> speed{};
    SimulationController* controller = nullptr;
    std::int64_t step_ms = 1;
    bool detached = false;
    /// The run plays a scenario (fixed).
    bool scenario = false;
    /// set_time_lock() was called: the two below are the run's for good.
    bool lock_given = false;
    bool time_locked = false;
    bool time_free = false;
    /// The commands that came before the lock was given (and before detach()), in their order:
    /// decided by answer_waiting(). At most kMaxWaitingCommands.
    struct Waiting
    {
        Command command;
        std::string source;
        std::function<void()> accepted;
    };
    std::deque<Waiting> waiting{};
    /// In hold_before_start(), and no resume, step or run_until yet.
    bool holding = false;
    /// Since the last pause (or the start, paused): a step, run_until or pause at a later time
    /// has let the simulation go on from 0, so there is nothing to hold before the first step.
    bool released = false;
    /// The speed the gate had when it was looked at last (facts_of()); kept for after detach().
    std::optional<std::optional<double>> gate_speed{};
    /// set_on_change().
    OnChange on_change{};
    /// Called in detach() with the controller, before it goes (set_on_detach()).
    std::function<void(SimulationController&)> on_detach{};
    /// The first accepted command that changed how the time goes (first_time_change()).
    std::optional<TimeChange> first_change{};
};

namespace {

/// Where `s` stands, for the rules. Under its mutex.
template <typename S> ControlSituation situation_of(const S& s)
{
    const ControlStage stage = s.detached ? ControlStage::Detached
                               : s.controller == nullptr ? ControlStage::SettingUp
                                                         : ControlStage::Attached;
    return ControlSituation{stage, s.paused, s.scenario, s.time_locked, s.time_free};
}

/// The facts of `s` now. Under its mutex; reads the gate.
template <typename S> TimeFacts facts_of(S& s)
{
    TimeFacts f;
    f.situation = situation_of(s);
    f.holding = s.holding;
    if (s.controller != nullptr)
    {
        const TimeState t = s.controller->time_state();
        // As the time event's `paused` (noted_paused()): it stands still, or does with its next step.
        f.standing = s.holding || t.paused || (t.pause_target && *t.pause_target <= t.now);
        s.gate_speed = t.speed;
    }
    else
    {
        f.standing = !s.detached && s.paused;
    }
    f.speed = s.gate_speed ? s.gate_speed : s.speed;
    return f;
}

/// Tells on_change, if there is one. Under the mutex of `s`.
template <typename S> void tell(S& s)
{
    if (s.on_change)
    {
        s.on_change(facts_of(s));
    }
}

/// `ms` rounded up to a whole number of `step_ms`.
std::int64_t whole_steps(std::int64_t ms, std::int64_t step_ms)
{
    return (ms + step_ms - 1) / step_ms * step_ms;
}

} // namespace

TimeCommands::TimeCommands(bool start_paused, OnCommand on_command, bool scenario) : state_(std::make_unique<State>())
{
    state_->paused = start_paused;
    state_->on_command = std::move(on_command);
    state_->scenario = scenario;
}

TimeCommands::~TimeCommands() = default;

Command TimeCommands::apply(const Command& command, std::string_view source) { return apply(command, source, {}); }

namespace {

/// Decides `command` of `source` for where `s` stands, writes its event and does it (apply()
/// says what that is). Under the mutex of `s`; the caller tells on_change after it.
template <typename S>
Command decide_and_do(S& s, const Command& command, std::string_view source, const std::function<void()>& accepted)
{
    Command out = command;
    /// An accepted step: where it ends (the command event's "at_ms").
    std::optional<SimTime> step_to;
    const std::string name = command.name.value_or("");
    // A pause at a time, or run_until: they need the controller's step to round to.
    const bool at_time = command.at_ms.has_value() && (name == "pause" || name == "run_until");
    const std::optional<Entry> entry = entry_of(source);
    Decision decision =
        entry ? decide(situation_of(s), *entry, command_operation(name), name, &command)
              : Decision{false, reason::kUnknownSource,
                         "the command came from '" + std::string(source) +
                             "', which is no entry of a run's commands (stdin, control_server): it is refused"};
    if (!decision.ok)
    {
        out.accepted = false;
        out.message = std::move(decision.message);
        out.reason = std::move(decision.reason);
    }
    else if (name == "step")
    {
        // Whole steps of the controller: the simulation can stand still only at one of them.
        out.ms = whole_steps(command.ms.value_or(1), s.step_ms);
        if (SimulationController* c = s.controller)
        {
            // From where it stands, or will stand after the steps asked for before. Read here,
            // before the command's event, so that the event says where this step ends. The gate
            // may go on between this look and run_until() below, but never past `step_to`: a step
            // is taken only once a pause was asked for, so the gate stands still, or does at its
            // next step or at its target, and each of those is at or before `step_to`.
            const TimeState t = c->time_state();
            const SimTime from = t.pause_target && *t.pause_target > t.now ? *t.pause_target : t.now;
            const SimDuration d = std::chrono::milliseconds(*out.ms);
            step_to = d >= SimTime::max() - from ? SimTime::max() : from + d;
            out.at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(step_to->time_since_epoch()).count();
        }
    }
    else if (at_time)
    {
        out.at_ms = whole_steps(*command.at_ms, s.step_ms);
    }
    if (s.on_command)
    {
        s.on_command(out, source);
    }
    if (!out.accepted)
    {
        return out;
    }
    if (accepted)
    {
        accepted();
    }
    SimulationController* c = s.controller;
    // A stop is the caller's (it asks the run to stop): only its event and its refusal are here,
    // in the order of the other commands.
    if (name == "stop")
    {
        return out;
    }
    // Before it takes effect: where the simulation stood when it came.
    if (!s.first_change && (name == "pause" || name == "step" || name == "run_until" ||
                            (name == "speed" && command.speed != std::optional<double>(1.0))))
    {
        const std::int64_t at =
            c != nullptr ? std::chrono::duration_cast<std::chrono::milliseconds>(c->time_state().now.time_since_epoch()).count()
                         : 0;
        s.first_change = TimeChange{name, std::string(source), at, name == "step" ? out.ms : std::nullopt, out.at_ms,
                                    name == "speed" ? command.speed : std::nullopt};
    }
    // What lets the simulation go on from 0: the hold ends with it, or (before the hold) there
    // is none. A pause (not at a later time) asks for it again.
    const bool goes_on = name == "resume" || name == "step" || name == "run_until" ||
                         (name == "pause" && out.at_ms && *out.at_ms > 0);
    if (goes_on)
    {
        s.released = true;
    }
    else if (name == "pause")
    {
        s.released = false;
    }
    if (s.holding && goes_on)
    {
        s.holding = false;
        s.cv.notify_all();
    }
    if (name == "pause")
    {
        s.paused = true;
        if (c != nullptr)
        {
            out.at_ms ? c->pause_at(SimTime{} + std::chrono::milliseconds(*out.at_ms)) : c->pause();
        }
    }
    else if (name == "run_until")
    {
        // It stands still there: a step from there is a step while paused.
        s.paused = true;
        if (c != nullptr)
        {
            c->run_until(SimTime{} + std::chrono::milliseconds(*out.at_ms));
        }
    }
    else if (name == "resume")
    {
        s.paused = false;
        if (c != nullptr)
        {
            c->resume();
        }
    }
    else if (name == "speed")
    {
        if (c != nullptr)
        {
            c->set_speed(command.speed);
        }
        else
        {
            s.speed = command.speed;
        }
    }
    else if (name == "step" && c != nullptr && step_to)
    {
        c->run_until(*step_to);
    }
    return out;
}

/// Decides the commands that waited, in their order, each as apply() would now: its event, what
/// it does, then on_change. Under the mutex of `s`.
template <typename S> void answer_waiting(S& s)
{
    auto waiting = std::move(s.waiting);
    s.waiting.clear();
    for (const auto& w : waiting)
    {
        static_cast<void>(decide_and_do(s, w.command, w.source, w.accepted));
        tell(s);
    }
}

} // namespace

Command TimeCommands::apply(const Command& command, std::string_view source, const std::function<void()>& accepted)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    // However this returns: what the command changed (or a stop request the caller made in
    // `accepted`) is told after the command's event.
    struct Tell
    {
        State& s;
        ~Tell() { tell(s); }
    } const told{s};
    // Not known yet whether the time is locked: nothing but a stop is decided (a stop is taken
    // whatever the lock is, and must not wait behind the others).
    if (!s.lock_given && !s.detached && command.name != "stop")
    {
        Command out = command;
        out.accepted = false;
        if (s.waiting.size() >= kMaxWaitingCommands)
        {
            out.reason = reason::kNotSetUp;
            out.message = "the run has not read its configuration yet and keeps " + std::to_string(kMaxWaitingCommands) +
                          " commands to answer once it has: " + command.name.value_or("") +
                          " is refused; send it again after the run's check event (the configuration is read then)";
            if (s.on_command)
            {
                s.on_command(out, source);
            }
            return out;
        }
        s.waiting.push_back({command, std::string(source), accepted});
        out.pending = true;
        return out;
    }
    return decide_and_do(s, command, source, accepted);
}

void TimeCommands::attach(SimulationController& controller, std::int64_t step_ms, std::optional<double> speed)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    if (!s.lock_given)
    {
        // No command may reach a controller before the lock is known: the run gives it first.
        throw std::logic_error("TimeCommands::attach(): set_time_lock() has not been called; give the run's time lock "
                               "(TimeLock{} for a run that is not connected outside) before the controller is attached");
    }
    s.controller = &controller;
    s.step_ms = std::max<std::int64_t>(step_ms, 1);
    controller.set_speed(s.speed ? *s.speed : speed);
    if (s.paused)
    {
        controller.pause_at(SimTime{});
    }
    tell(s);
}

void TimeCommands::set_on_change(OnChange on_change)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    s.on_change = std::move(on_change);
}

void TimeCommands::look(const std::function<void(const TimeFacts& facts)>& f)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    f(facts_of(s));
}

void TimeCommands::set_on_detach(std::function<void(SimulationController& controller)> f)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    s.on_detach = std::move(f);
}

void TimeCommands::detach()
{
    State& s = *state_;
    {
        const std::lock_guard lock(s.mutex);
        if (s.controller != nullptr)
        {
            static_cast<void>(facts_of(s)); // the gate's speed, for the facts from now on
            if (s.on_detach)
            {
                s.on_detach(*s.controller);
            }
        }
        const bool was_detached = s.detached;
        s.controller = nullptr;
        s.detached = true;
        s.holding = false;
        if (!was_detached)
        {
            tell(s);
        }
        // The lock never came (the configuration could not be read): the simulation does not
        // start, and the commands that waited are told so.
        answer_waiting(s);
    }
    s.cv.notify_all();
}

TimeCommands::Hold TimeCommands::hold_before_start(const std::function<bool()>& give_up, std::chrono::milliseconds poll,
                                                   const std::function<void()>& on_hold)
{
    State& s = *state_;
    {
        const std::lock_guard lock(s.mutex);
        if (s.detached)
        {
            return Hold::GaveUp;
        }
        if (!s.paused || s.released)
        {
            return Hold::None; // a command before the window let it go on: nothing to hold
        }
        s.holding = true;
        tell(s);
    }
    // However this ends (also by an exception), the run no longer holds.
    struct EndHold
    {
        State& s;
        ~EndHold()
        {
            const std::lock_guard lock(s.mutex);
            s.holding = false;
            tell(s);
        }
    } const end{s};
    if (on_hold)
    {
        on_hold();
    }
    while (true)
    {
        {
            std::unique_lock lock(s.mutex);
            if (s.cv.wait_for(lock, poll, [&s] { return !s.holding; }))
            {
                return s.detached ? Hold::GaveUp : Hold::Released;
            }
        }
        // Without the lock: it may take other locks (the window's, the runtime's).
        if (give_up && give_up())
        {
            const std::lock_guard lock(s.mutex);
            if (!s.holding)
            {
                return s.detached ? Hold::GaveUp : Hold::Released; // a command came just now
            }
            return Hold::GaveUp;
        }
    }
}

std::optional<TimeChange> TimeCommands::first_time_change()
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    return s.first_change;
}

Allowed TimeCommands::allowed(Operation operation, Entry entry)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    return launch::allowed(situation_of(s), entry, operation);
}

std::optional<Decision> TimeCommands::always_refused(Entry entry) const
{
    // Fixed since the constructor: no lock.
    return launch::always_refused(state_->scenario, entry);
}

void TimeCommands::set_time_lock(const TimeLock& lock)
{
    State& s = *state_;
    const std::lock_guard guard(s.mutex);
    if (s.lock_given)
    {
        return; // fixed for the run: commands have been decided with the first one
    }
    s.lock_given = true;
    s.time_locked = lock.locked;
    s.time_free = !lock.locked && !lock.because().empty();
    // The state first (what is taken from now on, if the lock changes it), then the commands
    // that waited for it.
    if (s.time_locked || s.time_free)
    {
        tell(s);
    }
    answer_waiting(s);
}

TimeCommands::Stage TimeCommands::inspect(const std::function<void(SimulationController& controller, bool holding)>& f)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    if (s.detached)
    {
        return Stage::Detached;
    }
    if (s.controller == nullptr)
    {
        return Stage::SettingUp;
    }
    f(*s.controller, s.holding);
    return Stage::Attached;
}

std::optional<SimTime> TimeCommands::wait_paused(std::chrono::milliseconds timeout)
{
    State& s = *state_;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(s.mutex);
    while (true)
    {
        if (s.detached)
        {
            return std::nullopt;
        }
        if (s.holding)
        {
            return SimTime{};
        }
        if (SimulationController* c = s.controller)
        {
            // As SimulationController::wait_until_paused(), without waiting under the gate's lock.
            if (c->runtime().started() && has_finished(PeerStatus{c->runtime().state()}))
            {
                return std::nullopt;
            }
            const TimeState t = c->time_state();
            if (t.paused)
            {
                return t.now;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return std::nullopt;
        }
        // Short slices: the gate does not signal this condition variable.
        s.cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(10)));
    }
}

} // namespace vehicle_sil::launch
