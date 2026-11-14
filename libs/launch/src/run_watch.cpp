// SPDX-License-Identifier: MIT
#include "run_watch.hpp"

#include "run_words.hpp"

#include "vehicle_sil/core/simulation_watch.hpp"
#include "vehicle_sil/launch/events.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch::detail {

namespace {

namespace orch = SilKit::Services::Orchestration;

} // namespace

std::string StandStill::look(std::int64_t now_ms, bool paused, std::optional<std::chrono::nanoseconds> overdue,
                             std::chrono::steady_clock::time_point wall)
{
    const auto stood_ms = [&](std::chrono::steady_clock::duration d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };
    if (over_)
    {
        return {};
    }
    if (paused || now_ms != last_ms_)
    {
        std::string line;
        if (said_ > 0 && !paused)
        {
            line = "virtual time advances again: it stood still for " + std::to_string(stood_ms(wall - since_) / 1000) +
                   " s at t=" + std::to_string(last_ms_) + " ms";
        }
        last_ms_ = now_ms;
        since_ = wall;
        said_ = 0;
        return line;
    }
    const std::int64_t stood = overdue ? stood_ms(*overdue) : stood_ms(wall - since_);
    const std::size_t before = said_;
    while (said_ < std::size(kSayAfterMs) && stood >= kSayAfterMs[said_])
    {
        ++said_;
    }
    if (said_ == before)
    {
        return {};
    }
    return "virtual time has not advanced for " + std::to_string(stood / 1000) + " s (at t=" + std::to_string(now_ms) +
           " ms, not paused): a participant may have lost its network connection (on another computer), stand still "
           "in a debugger, or hang in a step; the run waits on (a run with --timeout-ms ends when that has passed)";
}

std::string StandStill::stopping(std::int64_t now_ms, std::chrono::steady_clock::time_point wall)
{
    std::string line;
    if (!over_ && said_ > 0 && now_ms != last_ms_)
    {
        line = look(now_ms, false, std::nullopt, wall);
    }
    over_ = true; // says no more
    return line;
}

void RunWatch::note_timing(SimulationController& controller)
{
    const TimeState s = controller.time_state();
    RunTiming& t = outcome_.timing;
    t.speed = s.speed;
    t.sim_ms = sim_ms(controller, s);
    t.paused_ms = std::chrono::duration_cast<std::chrono::milliseconds>(s.paused_for).count();
    t.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(s.running_for).count() + t.paused_ms;
    t.paced_steps = s.paced_steps;
    t.late_steps = s.late_steps;
    t.max_late_ms = ceil_ms(s.max_lateness);
    t.slipped_ms = ceil_ms(s.slipped);
    t.late_ms = ceil_ms(s.current_lateness);
}

int RunWatch::watch(SimulationController& controller)
{
    WatchOptions watch;
    watch.poll = ctx_.options.poll;
    // A scenario that is over stops the simulation in order, as an interrupt does (which
    // comes first if both are there).
    // A required participant that has failed comes before the scenario, whichever is seen
    // first: a write to an ECU that has just gone fails its step, and the cause is the ECU.
    bool forScenario = false;
    bool processFailed = false;
    watch.interrupted = [this, &forScenario, &watch, &controller] {
        if (ctx_.interrupts() > 0)
        {
            return true;
        }
        if (!scenario_.over() || watch.failed() || FailureCheck::left_participant(controller))
        {
            return false; // the checks after this one decide a failure
        }
        forScenario = true;
        return true;
    };
    watch.failed = [this, &controller, &processFailed] {
        if (processFailed)
        {
            return true; // seen in this round already
        }
        // A required participant whose process failed fails the run at once; one that exited
        // with 0 may have stopped the simulation (the controller sees that a moment later).
        // Once the simulation stops, participants leave on purpose (a coordinated participant
        // ends only after the controller has stopped): their exit codes are judged after it.
        // (The time gate stands still in Running: a process that ends while the simulation is
        // paused fails the run as well.)
        const bool running = controller.runtime().state() == orch::ParticipantState::Running;
        // The events of what changed (here: the one look per poll the watch gives).
        notes_.note_joined(controller, outcome_);
        notes_.note_time(controller, TimeNote::Poll, outcome_);
        const TimeState time = controller.time_state();
        if (!running)
        {
            // It moved again just before it stopped (a simulation that ends soon after).
            if (const std::string line = stand_still_.stopping(sim_ms(controller, time), std::chrono::steady_clock::now());
                !line.empty())
            {
                out_.say(line);
            }
            // Stopping: the ends of the processes are looked at after the simulation's end
            // (RunParticipants::end_children()), so that they come after it in the output.
            return false;
        }
        if (const std::string line = stand_still_.look(
                sim_ms(controller, time), time.paused,
                time.speed ? std::optional<std::chrono::nanoseconds>(time.current_lateness) : std::nullopt,
                std::chrono::steady_clock::now());
            !line.empty())
        {
            out_.say(line);
        }
        std::vector<bool> known;
        for (const auto& run : outcome_.participants)
        {
            known.push_back(run.exit.has_value());
        }
        static_cast<void>(participants_.look_at_children(false));
        for (std::size_t i = 0; i < known.size(); ++i)
        {
            const ParticipantRun& run = outcome_.participants[i];
            if (!known[i] && run.required && failed_exit(run))
            {
                outcome_.failed_participant = run.name;
                processFailed = true;
                return true;
            }
        }
        return false;
    };
    if (ctx_.options.timeout)
    {
        // From the start, or from the hold before the first step (it counts as standing still).
        const std::int64_t held = start_.hold_began_ns();
        if (const std::int64_t heldMs = start_.hold_ms(); heldMs >= 0)
        {
            outcome_.held_before_start_ms = heldMs;
        }
        // With a participant started by hand: from when the launcher began to wait.
        watch.deadline = start_.manual_wait_began().value_or(
                             held >= 0 ? std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(held))
                                       : std::chrono::steady_clock::now()) +
                         *ctx_.options.timeout;
        outcome_.timed_out_while_holding = start_.hold_timed_out();
    }
    watch.before_abort = [this](const WatchResult& why) {
        if (why.end == WatchEnd::TimedOut)
        {
            out_.say("the simulation did not end within --timeout-ms (" + std::to_string(ctx_.options.timeout->count()) +
                " ms): aborting it");
        }
        else
        {
            const std::string who = why.failure ? why.failure->participant : outcome_.failed_participant;
            notes_.note_failed(outcome_, who, why.failure ? "left_simulation" : "process_ended");
            out_.say("participant '" + who + "' failed" + (why.failure ? ": it " + why.failure->reason : std::string{}) +
                "; aborting the simulation");
        }
    };
    WatchTarget target;
    target.wait_until_finished = [&controller](std::chrono::milliseconds timeout) { return controller.wait_until_finished(timeout); };
    target.left_participant = [&controller] { return FailureCheck::left_participant(controller); };
    target.stop = [&controller, &forScenario](std::string_view reason) {
        controller.stop(forScenario ? std::string_view("the scenario is over") : reason);
    };
    target.abort = [&controller] { controller.abort(); };
    const WatchResult watched = watch_simulation(target, watch);
    if (const std::string line = stand_still_.stopping(sim_ms(controller, controller.time_state()), std::chrono::steady_clock::now());
        !line.empty())
    {
        out_.say(line);
    }
    scenario_.finish(controller, watched, forScenario, outcome_);
    note_timing(controller);
    notes_.note_time(controller, TimeNote::Last, outcome_);
    // No time command from here on: the simulation is over.
    ctx_.commands.detach();
    // An interrupt from here on comes while the launcher waits for the participants to end.
    const int interrupts_at_end = ctx_.interrupts();
    switch (watched.end)
    {
    case WatchEnd::Ended:
        if (watched.state == orch::ParticipantState::Shutdown)
        {
            outcome_.simulation = ctx_.config.stop_at_ms && controller.wall_time_to_stop_at() ? SimulationEnd::ReachedStopAt
                                                                                           : SimulationEnd::Ended;
        }
        else
        {
            outcome_.simulation = SimulationEnd::Ended;
            outcome_.controller_state = watched.state ? orch::to_string(*watched.state) : std::string("unknown");
        }
        out_.say("the simulation ended");
        break;
    case WatchEnd::TimedOut:
        outcome_.simulation = SimulationEnd::Aborted;
        outcome_.timed_out = true;
        out_.say("the simulation was aborted: it did not end within --timeout-ms");
        break;
    case WatchEnd::Failed:
        outcome_.simulation = SimulationEnd::Aborted;
        if (watched.failure && outcome_.failed_participant.empty())
        {
            outcome_.failed_participant = watched.failure->participant;
            outcome_.failure_reason = watched.failure->reason;
        }
        out_.say("the simulation was aborted: participant '" + outcome_.failed_participant + "' failed");
        break;
    case WatchEnd::Interrupted:
        if (forScenario)
        {
            outcome_.simulation = SimulationEnd::Ended;
            outcome_.stopped_by_scenario = true;
            if (watched.state != orch::ParticipantState::Shutdown)
            {
                outcome_.controller_state = watched.state ? orch::to_string(*watched.state) : std::string("unknown");
            }
            out_.say("the scenario is over: the simulation was stopped");
            break;
        }
        outcome_.simulation = SimulationEnd::Interrupted;
        outcome_.interrupted_by = ctx_.interrupt_name();
        out_.say("interrupted (" + outcome_.interrupted_by + "): the simulation was stopped");
        break;
    }
    return interrupts_at_end;
}

} // namespace vehicle_sil::launch::detail
