// SPDX-License-Identifier: MIT
//
// TimeCommands (run.hpp): the time commands of a run (pause, resume, step, run_until, speed), from
// before the simulation is set up until it is over.
//
// State and lock: everything is in TimeCommands::State, guarded by its mutex. Any thread applies
// a command (the thread that reads the commands, a control server's); the run's main thread
// attaches and detaches the controller; the start window's thread holds before the first step.
// Taken while that mutex is held: the event stream's (the "command" event), the controller's
// gate's (pause, resume, run_until, set_speed, time_state) and whatever on_detach takes.
// hold_before_start() calls on_hold and give_up without it.
#include "vehicle_sil/launch/run.hpp"

#include "vehicle_sil/launch/events.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace vehicle_sil::launch {

struct TimeCommands::State
{
    std::mutex mutex;
    /// hold_before_start() and wait_paused(): a command released the hold, or detach().
    std::condition_variable cv;
    EventStream* events = nullptr;
    /// A pause was asked for (or the run starts paused) and no resume since.
    bool paused = false;
    /// The last speed command before attach() (the inner empty: "max").
    std::optional<std::optional<double>> speed{};
    SimulationController* controller = nullptr;
    std::int64_t step_ms = 1;
    bool detached = false;
    /// In hold_before_start(), and no resume, step or run_until yet.
    bool holding = false;
    /// Since the last pause (or the start, paused): a step, run_until or pause at a later time
    /// has let the simulation go on from 0, so there is nothing to hold before the first step.
    bool released = false;
    /// Called in detach() with the controller, before it goes (set_on_detach()).
    std::function<void(SimulationController&)> on_detach{};
};

namespace {

/// `ms` rounded up to a whole number of `step_ms`.
std::int64_t whole_steps(std::int64_t ms, std::int64_t step_ms)
{
    return (ms + step_ms - 1) / step_ms * step_ms;
}

} // namespace

TimeCommands::TimeCommands(bool start_paused, EventStream* events) : state_(std::make_unique<State>())
{
    state_->paused = start_paused;
    state_->events = events;
}

TimeCommands::~TimeCommands() = default;

Command TimeCommands::apply(const Command& command, std::string_view source) { return apply(command, source, {}); }

Command TimeCommands::apply(const Command& command, std::string_view source, const std::function<void()>& accepted)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    Command out = command;
    const std::string name = command.name.value_or("");
    const auto refuse = [&out](std::string why) {
        out.accepted = false;
        out.message = std::move(why);
    };
    // A pause at a time, or run_until: they need the controller's step to round to.
    const bool at_time = command.at_ms.has_value() && (name == "pause" || name == "run_until");
    if (s.detached)
    {
        refuse("the simulation is not running (it is over, or it did not start): " + name +
               " works from the start of the run until the simulation ends");
    }
    else if (name != "pause" && name != "resume" && name != "step" && name != "run_until" && name != "speed" &&
             name != "stop")
    {
        refuse("'" + name + "' is no time command: the time commands are pause, resume, step, run_until and speed");
    }
    else if (name == "speed" && command.speed && !(*command.speed >= kMinSpeed && *command.speed <= kMaxSpeed))
    {
        refuse("the speed must be from 0.000001 to 1000000 simulation seconds per wall-clock second, or max");
    }
    else if (name == "step" && !(command.ms.value_or(1) >= 1 && command.ms.value_or(1) <= kMaxMillis))
    {
        refuse("step needs a time from 1 to " + std::to_string(kMaxMillis) + " ms");
    }
    else if (name == "run_until" && !command.at_ms)
    {
        refuse("run_until needs the time to stand still at");
    }
    else if (at_time && !(*command.at_ms >= 0 && *command.at_ms <= kMaxMillis))
    {
        refuse(name + " at a time needs a time from 0 to " + std::to_string(kMaxMillis) + " ms");
    }
    else if ((name == "step" || at_time) && s.controller == nullptr)
    {
        refuse("the simulation has not been set up yet: " + name +
               (name == "step" ? std::string(" works once it stands still (a time event with paused true), with "
                                             "--start-paused from the start")
                               : std::string(" at a time works once the participants have been started")));
    }
    else if (name == "step" && !s.paused)
    {
        refuse("step works while the simulation is paused: send {\"command\":\"pause\"} first (or run with "
               "--start-paused)");
    }
    else if (name == "step")
    {
        // Whole steps of the controller: the simulation can stand still only at one of them.
        out.ms = whole_steps(command.ms.value_or(1), s.step_ms);
    }
    else if (at_time)
    {
        out.at_ms = whole_steps(*command.at_ms, s.step_ms);
    }
    if (s.events != nullptr)
    {
        s.events->command(out, source);
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
    else if (name == "step" && c != nullptr)
    {
        // From where it stands, or will stand after the steps asked for before.
        const TimeState t = c->time_state();
        const SimTime from = t.pause_target && *t.pause_target > t.now ? *t.pause_target : t.now;
        const SimDuration d = std::chrono::milliseconds(*out.ms);
        c->run_until(d >= SimTime::max() - from ? SimTime::max() : from + d);
    }
    return out;
}

void TimeCommands::attach(SimulationController& controller, std::int64_t step_ms, std::optional<double> speed)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    s.controller = &controller;
    s.step_ms = std::max<std::int64_t>(step_ms, 1);
    controller.set_speed(s.speed ? *s.speed : speed);
    if (s.paused)
    {
        controller.pause_at(SimTime{});
    }
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
        if (s.controller != nullptr && s.on_detach)
        {
            s.on_detach(*s.controller);
        }
        s.controller = nullptr;
        s.detached = true;
        s.holding = false;
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
    }
    // However this ends (also by an exception), the run no longer holds.
    struct EndHold
    {
        State& s;
        ~EndHold()
        {
            const std::lock_guard lock(s.mutex);
            s.holding = false;
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
