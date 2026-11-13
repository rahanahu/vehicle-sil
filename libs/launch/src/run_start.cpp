// SPDX-License-Identifier: MIT
#include "run_start.hpp"

#include "vehicle_sil/launch/events.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

namespace vehicle_sil::launch::detail {

namespace {

namespace orch = SilKit::Services::Orchestration;

/// The first enabled required participant that has left the simulation before it started (a
/// process that ended is seen as Error: "Connection Lost"), as "participant '<name>' left
/// ... (state <state>)"; "" if none. Thread safe (the controller's runtime's peer statuses).
[[nodiscard]] std::string gone_before_start(const RunContext& ctx, SimulationController& controller)
{
    for (const std::string& name : controller.required_participants())
    {
        const PeerStatus status = controller.runtime().peer_status(name);
        if (has_finished(status))
        {
            const bool manual = std::any_of(ctx.config.participants.begin(), ctx.config.participants.end(),
                                            [&name](const ParticipantConfig& p) { return p.manual && p.name == name; });
            return "participant '" + name + "' left the simulation before it started (state " +
                   std::string(orch::to_string(*status.state)) + ")" +
                   (manual ? "; you started it yourself (manual), and it cannot join this run again: to start it "
                             "again, start vehicle_sil_launch again as well"
                           : "");
        }
    }
    return {};
}

} // namespace

void RunStartWindow::run(control_silkit::HubTransport& transport, SimulationController& controller, const StartWindow& window)
{
    const auto cancelled = [this, &window] { return window.cancelled() || ctx_.interrupts() > 0; };
    // Not beyond the start's limit, and with room for the start to go on after it.
    const auto deadline = std::min(std::chrono::steady_clock::now() + ctx_.options.control_hello_timeout,
                                   shared_.limit.at() - std::chrono::milliseconds(500));
    control_silkit::HelloReport report = transport.wait_for_hellos(control_.expected_agents(), deadline, cancelled);
    {
        // The links as they are now, before any step: the simulation may be over by the time
        // the main thread looks (a short one at full speed), and an agent that has ended by
        // then was connected at the start all the same.
        const std::lock_guard lock(shared_.found_mutex);
        shared_.control_start = ControlStart{std::move(report), transport.links()};
    }
    if (ctx_.options.before_start && !cancelled())
    {
        // The simulation starts whatever happens here: a failure is kept, and the run ends
        // the simulation for it once it has started (wait_for_start()).
        std::string error;
        try
        {
            ctx_.options.before_start(control_.hub());
        }
        catch (const std::exception& e)
        {
            error = *e.what() != '\0' ? e.what() : "exception without message";
        }
        catch (...)
        {
            error = "unknown exception";
        }
        const std::lock_guard lock(shared_.found_mutex);
        shared_.before_start_error = std::move(error);
    }
    if (ctx_.scenario != nullptr && !cancelled())
    {
        scenario_.begin(transport, cancelled);
    }
    if (ctx_.holds() && !cancelled())
    {
        // Every participant waits before its step at 0, with its subscriptions connected:
        // what a control client writes now (and is queued) takes effect in the steps at 0.
        std::chrono::steady_clock::time_point began{};
        const auto on_hold = [&] {
            // The start has no limit while it holds; --timeout-ms counts from here.
            began = std::chrono::steady_clock::now();
            shared_.hold_began_ns.store(began.time_since_epoch().count());
            shared_.limit.begin_hold();
            out_.say("standing still before the first step until a resume, step or run_until command (--start-paused "
                "with a control server)");
        };
        const auto give_up = [&] {
            // With a participant started by hand, --timeout-ms counts from the wait for the participants.
            const auto from = shared_.manual_wait_began().value_or(began);
            if (ctx_.options.timeout && std::chrono::steady_clock::now() >= from + *ctx_.options.timeout)
            {
                shared_.hold_timed_out.store(true);
                return true;
            }
            return cancelled() || !gone_before_start(ctx_, controller).empty();
        };
        if (ctx_.commands.hold_before_start(give_up, std::chrono::milliseconds(20), on_hold) != TimeCommands::Hold::None)
        {
            shared_.hold_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count());
            // From here on, the participants have start_timeout_ms to get to their first step
            // (with a participant started by hand: from when those are ready, as before the hold).
            shared_.limit.end_hold(std::chrono::steady_clock::now(), std::chrono::milliseconds(ctx_.config.start_timeout_ms),
                                   waits_for_manual(ctx_.config));
        }
    }
}

RunStart::RunStart(const RunContext& ctx, RunOutput& out, RunControl& control, RunNotes& notes, RunScenario& scenario,
                   RunParticipants& participants, RunOutcome& outcome) :
  ctx_(ctx), out_(out), control_(control), notes_(notes), participants_(participants), outcome_(outcome),
  window_(ctx, out, control, scenario, shared_)
{
}

std::string RunStart::start_given_up(SimulationController& controller)
{
    // With a participant started by hand, a stop request ends the start too: without a limit,
    // it would wait for ever for one that stands still in a debugger. (Without one, the start
    // has its limit and a stop takes effect once it is over, as it always did.)
    if (waits_for_manual(ctx_.config) && ctx_.interrupts() > 0)
    {
        return "interrupted (" + ctx_.interrupt_name() + ")";
    }
    if (const auto from = shared_.manual_wait_began(); from && ctx_.options.timeout && std::chrono::steady_clock::now() >= *from + *ctx_.options.timeout)
    {
        manual_timed_out_ = "not at their first step: " + not_ready(controller);
        return "--timeout-ms";
    }
    if (std::string gone = gone_before_start(ctx_, controller); !gone.empty())
    {
        return gone;
    }
    // With a participant started by hand, the limit begins once every such participant is
    // ready for its first step (not while the run stands still before it).
    const bool manualReady = waits_for_manual(ctx_.config) && manual_ready(controller);
    const StartLimit::Now limit = shared_.limit.take(std::chrono::steady_clock::now(), manualReady);
    if (std::chrono::steady_clock::now() < limit.at)
    {
        return {};
    }
    return "the participants did not all get to their first step within " + std::to_string(limit.length.count()) +
           " ms" + (limit.after_hold ? " after the run stopped standing still before it" : "") +
           (waits_for_manual(ctx_.config) ? " after the participants you start yourself (manual) were ready" : "") +
           " (start_timeout_ms; a participant may hang while it starts): not ready: " + not_ready(controller);
}

std::string RunStart::not_ready(SimulationController& controller) const
{
    std::string waiting;
    std::string all;
    for (const std::string& name : controller.required_participants())
    {
        const PeerStatus status = controller.runtime().peer_status(name);
        const auto state = status.state.value_or(orch::ParticipantState::Invalid);
        if (state == orch::ParticipantState::Running)
        {
            continue;
        }
        const std::string item = name + " (" + std::string(orch::to_string(state)) + ")";
        all += (all.empty() ? "" : ", ") + item;
        if (state != orch::ParticipantState::ReadyToRun)
        {
            waiting += (waiting.empty() ? "" : ", ") + item;
        }
    }
    return waiting.empty() ? (all.empty() ? std::string("the launcher's controller") : all) : waiting;
}

bool RunStart::manual_ready(SimulationController& controller) const
{
    return std::all_of(ctx_.config.participants.begin(), ctx_.config.participants.end(), [&controller](const ParticipantConfig& p) {
        if (!p.enabled || !p.required || !p.manual)
        {
            return true;
        }
        const auto state = controller.runtime().peer_status(p.name).state;
        return state && (*state == orch::ParticipantState::ReadyToRun || *state == orch::ParticipantState::Running ||
                         *state == orch::ParticipantState::Paused);
    });
}

void RunStart::note_control()
{
    std::optional<ControlStart> start;
    {
        const std::lock_guard lock(shared_.found_mutex);
        start = shared_.control_start;
    }
    control_.note_links(outcome_, start, [this](std::size_t i) { return participants_.in_run(i); });
}

bool RunStart::wait_for_start(SimulationController& controller)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ctx_.config.start_timeout_ms);
    // The participants someone starts by hand have no time limit: they may take any time.
    const auto manual = [this](const std::string& name) {
        return std::any_of(ctx_.config.participants.begin(), ctx_.config.participants.end(),
                           [&name](const ParticipantConfig& p) { return p.manual && p.name == name; });
    };
    bool saidWaiting = false;
    // A run that waits for a participant started by hand: --timeout-ms counts from here (an
    // automatic run with manual: true in its file must end too).
    if (waits_for_manual(ctx_.config) && ctx_.options.timeout)
    {
        shared_.manual_wait_began_ns.store(std::chrono::steady_clock::now().time_since_epoch().count());
        outcome_.timeout_from_wait = true;
    }
    while (true)
    {
        if (ctx_.interrupts() > 0)
        {
            outcome_.interrupted_by = ctx_.interrupt_name();
            out_.say("interrupted (" + outcome_.interrupted_by + ") before the simulation started");
            return false;
        }
        if (const auto from = shared_.manual_wait_began(); from && std::chrono::steady_clock::now() >= *from + *ctx_.options.timeout)
        {
            std::string names;
            const std::vector<std::string> left = controller.missing_participants();
            for (const auto& name : left)
            {
                names += (names.empty() ? "" : ", ") + name + (manual(name) ? " (manual)" : "");
            }
            return start_timed_out(names + (left.size() == 1 ? " has not joined" : " have not joined"));
        }
        // Who has not joined is read before the ends of the processes: at the deadline, who
        // did not come is named even if a process ends in the same round.
        const std::vector<std::string> missing = controller.missing_participants();
        const bool onlyManual = std::all_of(missing.begin(), missing.end(), manual);
        const bool late = !onlyManual && std::chrono::steady_clock::now() >= deadline;
        notes_.note_joined(controller, outcome_);
        const int ended = participants_.look_at_children(true);
        if (ended >= 0 && !late)
        {
            outcome_.failed_participant = outcome_.participants[static_cast<std::size_t>(ended)].name;
            notes_.note_failed(outcome_, outcome_.failed_participant, "ended_before_start");
            return false;
        }
        if (missing.empty())
        {
            break;
        }
        if (onlyManual && !saidWaiting)
        {
            saidWaiting = true;
            std::string names;
            for (const auto& name : missing)
            {
                names += (names.empty() ? "" : ", ") + name;
            }
            out_.say("waiting for " + names + " to join (manual): start " + (missing.size() == 1 ? "it" : "them") +
                " with the command above; Ctrl+C stops the run");
        }
        if (late)
        {
            outcome_.start_timed_out = true;
            std::string names;
            for (auto& run : outcome_.participants)
            {
                if (std::find(missing.begin(), missing.end(), run.name) != missing.end() && !run.manual)
                {
                    run.missing_at_start = true;
                    names += (names.empty() ? "" : ", ") + run.name;
                }
            }
            out_.say("the simulation did not start: not joined within " + std::to_string(ctx_.config.start_timeout_ms) +
                " ms: " + names);
            return false;
        }
        std::this_thread::sleep_for(ctx_.options.poll);
    }
    try
    {
        // The start's limits are the run's own (StartLimit): until the hold, what is left of
        // start_timeout_ms (with room for the wait for the agents' hellos); in the hold, none
        // (it ends with a command, a stop, --timeout-ms or a participant that leaves); after
        // it, start_timeout_ms anew. A required participant that leaves ends the start at
        // once: the others would wait for it until the limit.
        // With a participant started by hand, none until every such participant is ready for its
        // first step (it may stand in a debugger before), then start_timeout_ms (start_given_up()).
        if (waits_for_manual(ctx_.config))
        {
            shared_.limit.set_once_manual_ready(std::chrono::milliseconds(ctx_.config.start_timeout_ms));
        }
        else
        {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const auto limit = std::max(left, ctx_.options.control_hello_timeout + std::chrono::milliseconds(1000));
            shared_.limit.set(std::chrono::steady_clock::now() + limit, limit);
        }
        controller.start(kNoStartLimit, [this, &controller] { return start_given_up(controller); });
    }
    catch (const std::exception& e)
    {
        // A stop request and --timeout-ms end the start of a run with a participant started by
        // hand as they end the wait above.
        if (waits_for_manual(ctx_.config) && ctx_.interrupts() > 0)
        {
            outcome_.interrupted_by = ctx_.interrupt_name();
            out_.say("interrupted (" + outcome_.interrupted_by + ") before the simulation started");
            return false;
        }
        if (!manual_timed_out_.empty())
        {
            return start_timed_out(manual_timed_out_);
        }
        if (const int gone = participants_.gone_process(!gone_before_start(ctx_, controller).empty()); gone >= 0)
        {
            // A participant's process ended before the simulation started: as in the wait above.
            outcome_.failed_participant = outcome_.participants[static_cast<std::size_t>(gone)].name;
            notes_.note_failed(outcome_, outcome_.failed_participant, "ended_before_start");
            return false;
        }
        outcome_.start_error = one_line(e.what());
        out_.say("the simulation did not start: " + outcome_.start_error);
        return false;
    }
    note_control();
    {
        const std::lock_guard lock(shared_.found_mutex);
        outcome_.before_start_error = shared_.before_start_error;
    }
    if (!outcome_.before_start_error.empty())
    {
        // It runs (every participant got to its first step), but not as it was to: ended here.
        out_.say("the simulation started, but what was to be done before it failed (" + outcome_.before_start_error +
            "): aborting it");
        controller.abort();
        outcome_.simulation = SimulationEnd::Aborted;
        before_start_failed_ = true;
        return false;
    }
    out_.event([](EventStream& e) { e.phase(Phase::Running); });
    out_.say("simulation running");
    // Every participant starts at virtual time 0.
    notes_.note_time(controller, TimeNote::Start, outcome_);
    return true;
}

bool RunStart::start_timed_out(const std::string& waiting_for)
{
    outcome_.timed_out = true;
    outcome_.timed_out_before_start = true;
    outcome_.timed_out_waiting_for = waiting_for;
    out_.say("the simulation did not start within --timeout-ms (" + std::to_string(ctx_.options.timeout->count()) +
        " ms): " + waiting_for);
    return false;
}

} // namespace vehicle_sil::launch::detail
