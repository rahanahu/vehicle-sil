// SPDX-License-Identifier: MIT
//
// What a run's outcome means: the step of the launcher's controller (plan_time_step()), the
// findings (assess_run()), the result and the exit code. Pure functions of the configuration and
// the RunOutcome: no state, no thread, no lock.
#include "vehicle_sil/launch/run.hpp"

#include "run_words.hpp"

#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using detail::ended_by_itself;
using detail::exit_text;
using detail::failed_exit;
using detail::kNanosPerMilli;
using detail::millis_text;

std::string prefix_of(const ParticipantRun& p) { return "participant '" + p.name + "': "; }

/// "its process exited with code 2", or what is known without its status.
std::string process_text(const ParticipantRun& p)
{
    return p.exit ? "its process " + exit_text(*p.exit) : std::string("its process ended");
}

std::string lines_hint(const ParticipantRun& p)
{
    return p.manual ? "you started it (manual), so what it wrote is where you started it"
                    : "the lines [" + p.name + "] above show what it wrote";
}

std::size_t count(const std::vector<Finding>& findings, Severity severity)
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [severity](const Finding& f) { return f.severity == severity; }));
}

} // namespace

// A finding that is added here and that the end of the participants can still change
// (RunParticipants::end_children(): exit, ending, stopped_after_end, stop_request_failed, left_processes,
// interrupted_by, interrupted_while_ending) must be left out by detail::settled_findings() until it cannot.
std::vector<Finding> assess_run(const RunOutcome& outcome)
{
    std::vector<Finding> out;
    const auto add = [&out](Severity severity, std::string code, std::string message, const ParticipantRun* p) {
        out.push_back(Finding{severity, std::move(code), std::move(message), p != nullptr ? p->line : 0,
                              p != nullptr ? p->name : std::string{}, ""});
    };
    if (!outcome.setup_error.empty())
    {
        add(Severity::Error, "setup", "cannot set up the simulation: " + outcome.setup_error, nullptr);
    }
    const bool ran = outcome.simulation != SimulationEnd::NotStarted;
    for (const ParticipantRun& p : outcome.participants)
    {
        const std::string pre = prefix_of(p);
        const Severity weight = p.required ? Severity::Error : Severity::Warning;
        if (!p.start_error.empty())
        {
            add(weight, "start", pre + p.start_error + ": check the command in the configuration file", &p);
            continue;
        }
        const bool blamed = !outcome.failed_participant.empty() && p.name == outcome.failed_participant;
        // An optional participant may end before or after the start (it does not hold the start
        // up): only its exit code counts, so that the same run gives the same result.
        if (p.ending == Ending::ExitedBeforeStart && p.required)
        {
            add(Severity::Error, "ended-before-start",
                pre + "it ended before the simulation started: " + process_text(p) +
                    ", so the simulation could not start; " + lines_hint(p),
                &p);
        }
        else if (blamed)
        {
            std::string what;
            if (p.exit && ended_by_itself(p))
            {
                what = "its process " + exit_text(*p.exit);
            }
            else if (!outcome.failure_reason.empty())
            {
                what = "it " + outcome.failure_reason;
            }
            else
            {
                what = "it left the simulation";
            }
            add(Severity::Error, "participant-failed",
                pre + "it failed while the simulation was running, and the launcher aborted the simulation: " + what +
                    "; " + lines_hint(p),
                &p);
        }
        else if (p.missing_at_start)
        {
            add(Severity::Error, "not-joined",
                pre + "it did not join the simulation within " + std::to_string(outcome.start_timeout_ms) +
                    " ms (start_timeout_ms), although its process was running: " +
                    (p.describe ? "it did not connect to the registry; " + lines_hint(p)
                                : "it must connect to the registry given with {registry_uri} as the participant '" +
                                      p.name + "' with a coordinated lifecycle (or set required: false)") +
                    "; if it is only slow, raise start_timeout_ms",
                &p);
        }
        else if (failed_exit(p))
        {
            add(weight, "exit-code", pre + "its process " + exit_text(*p.exit) + "; " + lines_hint(p), &p);
        }
        // Only a participant the launcher had to stop or end: one that ended by itself just
        // before the request did end.
        if (p.stopped_after_end && ran && (p.ending == Ending::Stopped || p.ending == Ending::Killed))
        {
            std::string how;
            if (p.stop_request_failed)
            {
                how = "could not ask it to stop (no console to send " + std::string(stop_request_name()) +
                      " through) and ended it with the processes it started";
            }
            else if (p.ending == Ending::Stopped)
            {
                how = "asked it to stop (" + std::string(stop_request_name()) + "), and " + process_text(p);
            }
            else
            {
                how = "asked it to stop (" + std::string(stop_request_name()) + "), and as it did not end within " +
                      std::to_string(outcome.stop_timeout_ms) + " ms either, ended it with the processes it started";
            }
            add(weight, "did-not-end",
                pre + "it was still running " + std::to_string(outcome.stop_timeout_ms) +
                    " ms (stop_timeout_ms) after the simulation ended: the launcher " + how +
                    "; a participant should end when the simulation ends (" + lines_hint(p) + ")",
                &p);
        }
        if (p.control == ControlLink::NoHello)
        {
            add(Severity::Warning, "control-link-no-hello",
                pre + "it offers its variables (--describe), but " +
                    (p.control_hello_seen ? "its control agent said hello and then did not answer that it receives the "
                                            "launcher's commands within "
                                          : "its control agent did not say hello within ") +
                    std::to_string(outcome.control_hello_timeout_ms) +
                    " ms before the simulation started, so its variables cannot be read or written in this run. " +
                    (p.control_hello_seen
                         ? "Its transport does not answer the launcher's probe: build the ECU and vehicle_sil_launch from "
                           "the same version of vehicle-sil"
                         : "Either the executable did not make the agent's transport that --describe announces (an "
                           "application with a transport of its own gives --no-control), or the launcher did not take its "
                           "hello (the finding control-link-dropped says so, if it is there)") +
                    "; " + lines_hint(p),
                &p);
        }
        // Said by the check already if --describe announced the version.
        else if (p.control == ControlLink::VersionMismatch && !p.control_version_described)
        {
            add(Severity::Warning, "control-link-version",
                pre + "its control agent speaks version " + std::to_string(p.control_version) +
                    " of the control messages and this launcher version " + std::to_string(control::wire::kVersion) +
                    ", so its variables cannot be read or written in this run: build the ECU and vehicle_sil_launch from "
                    "the same version of vehicle-sil",
                &p);
        }
        if (!p.variable_map_problems.empty())
        {
            std::string problems;
            for (const std::string& problem : p.variable_map_problems)
            {
                problems += (problems.empty() ? "" : "; ") + problem;
            }
            add(Severity::Error, "variable-map-not-applied",
                pre + "variables: " + std::to_string(p.variable_map_problems.size()) +
                    " of its mappings do not fit the variables its control agent announced and were not applied, so "
                    "those variables were seen under the executable's names and values in this run and writes to the names "
                    "of these mappings were refused: " + problems +
                    ". Correct variables in the configuration (vehicle_sil_launch check compares them with what an "
                    "ecu_main executable describes)",
                &p);
        }
        if (p.control_late_messages > 0)
        {
            add(Severity::Warning, "control-link-late-values",
                pre + std::to_string(p.control_late_messages) +
                    " messages of its control agent's steps reached the launcher after the launcher's controller had "
                    "begun a later step: the values and results that the launcher had at a pause were not complete. Its "
                    "agent must publish within its steps (an executable built with ecu_main() does; a transport of its "
                    "own must send from control::Agent::set_step_sink(), not from another thread)",
                &p);
        }
        if (p.left_processes)
        {
            add(Severity::Warning, "left-processes",
                pre + "processes it started were still running after it had ended: the launcher ended them; end them "
                      "in the participant before it exits",
                &p);
        }
    }
    if (!outcome.start_error.empty())
    {
        add(Severity::Error, "simulation-start", "the simulation did not start: " + outcome.start_error, nullptr);
    }
    if (outcome.connection_not_closed_ms && !outcome.connection_close_interrupted)
    {
        // Not when one more interrupt cut the wait: then the launcher did what it was asked to.
        add(Severity::Warning, "connection-not-closed",
            "the launcher's own connection to the simulation (SIL Kit) had not closed " +
                std::to_string(*outcome.connection_not_closed_ms) +
                " ms after the participants had ended: the launcher ended without closing it. The result is complete. "
                "This is not expected of any configuration: it is a defect of the launcher or of SIL Kit. Report it "
                "with this result, the launcher's output and what the participants were doing (was one stopped in a "
                "debugger?)",
            nullptr);
    }
    if (outcome.control_refused_hellos + outcome.control_undecodable + outcome.control_unknown > 0)
    {
        add(Severity::Warning, "control-link-dropped",
            "the launcher dropped messages of control agents: " + std::to_string(outcome.control_refused_hellos) +
                " hellos it did not take, " + std::to_string(outcome.control_undecodable) + " messages it could not read and " +
                std::to_string(outcome.control_unknown) +
                " messages (or samples) that fit no agent it knows. Hellos are not taken beyond " +
                std::to_string(control_silkit::HubTransportOptions{}.max_links) +
                " participants with control agents, or with the session number of another participant; a message cannot be "
                "read if it is of another version of vehicle-sil (the participant's control-link-version says so) or not a "
                "control message at all (another program publishes on the topics vehicle_sil.control.*); a message fits "
                "no agent if its agent's hello was not taken, is of another version or has not arrived yet (a "
                "participant that joined while the simulation ran), and a sample if it does not match the variables its "
                "agent announced. The variables of the participants concerned may be missing or stale",
            nullptr);
    }
    if (!outcome.before_start_error.empty())
    {
        add(Severity::Error, "before-start",
            "what was to be done before the simulation started failed, and the launcher aborted the simulation at once: " +
                outcome.before_start_error,
            nullptr);
    }
    if (!outcome.controller_state.empty())
    {
        add(Severity::Error, "simulation",
            "the simulation ended in state " + outcome.controller_state +
                " instead of shutting down normally: the lines above show what the participants wrote",
            nullptr);
    }
    if (outcome.timed_out && outcome.timed_out_before_start)
    {
        add(Severity::Error, "timeout",
            "the simulation did not start within " + std::to_string(outcome.timeout_ms.value_or(0)) +
                " ms of wall-clock time (--timeout-ms, which counts from when the launcher began to wait for the "
                "participants, as the run waits for a participant you start yourself), so the launcher gave up: " +
                outcome.timed_out_waiting_for +
                "; start the participants started by hand (manual) in time, or leave --timeout-ms out to wait for them",
            nullptr);
    }
    else if (outcome.timed_out)
    {
        add(Severity::Error, "timeout",
            "the simulation did not end within " + std::to_string(outcome.timeout_ms.value_or(0)) +
                " ms of wall-clock time (--timeout-ms), so the launcher aborted it: " +
                (outcome.timed_out_while_holding
                     ? std::string("it stood still before its first step all that time, waiting for a resume, step or "
                                   "run_until command (--start-paused with a control server): no client let it go on")
                     : std::string("a participant may hang in a step (the lines above show what the participants "
                                   "wrote); if it is only slow, raise --timeout-ms") +
                           (outcome.timeout_from_wait
                                ? ". Those " + std::to_string(outcome.timeout_ms.value_or(0)) +
                                      " ms count from when the launcher began to wait for the participants, as the run "
                                      "waits for a participant you start yourself (manual)"
                            : outcome.held_before_start_ms
                                ? ". Those " + std::to_string(outcome.timeout_ms.value_or(0)) +
                                      " ms count from when the run began to stand still before its first step "
                                      "(--start-paused with a control server), and " +
                                      std::to_string(*outcome.held_before_start_ms) +
                                      " ms of them passed before a client let it go on: allow for that time in --timeout-ms"
                                : std::string{})),
            nullptr);
    }
    else if (outcome.simulation == SimulationEnd::Aborted && outcome.failed_participant.empty() &&
             outcome.before_start_error.empty())
    {
        add(Severity::Error, "simulation", "the simulation was aborted: the lines above show what the participants wrote",
            nullptr);
    }
    if (!outcome.interrupted_by.empty())
    {
        add(Severity::Note, "interrupted",
            "interrupted (" + outcome.interrupted_by + "): the launcher " +
                (outcome.interrupted_while_ending ? std::string("stopped the participants instead of waiting for them to end")
                 : ran                            ? std::string("stopped the simulation and the participants")
                                                  : std::string("stopped the participants before the simulation started")),
            nullptr);
    }
    if (const RunTiming& t = outcome.timing; t.late_steps > 0)
    {
        const auto actual = t.actual_speed();
        add(Severity::Warning, "behind-real-time",
            "the simulation fell behind real time: " + std::to_string(t.late_steps) + " of " + std::to_string(t.paced_steps) +
                " paced steps of the launcher's controller (" + std::to_string(t.time_step_ms) + " ms each) began more than " +
                std::to_string(t.late_tolerance_ms) + " ms (late_tolerance_ms) after the real time they stand for, the latest by " +
                std::to_string(t.max_late_ms) + " ms" +
                (t.late_ms > 0 ? "; when the run ended, the next step was " + std::to_string(t.late_ms) +
                                     " ms overdue (the simulation was not moving)"
                               : std::string{}) +
                ". The launcher does not catch up after a late step (the ECUs' periodic messages would leave in a burst): it "
                "goes on at the speed from there, so the simulation came " +
                std::to_string(t.slipped_ms) + " ms behind in all" +
                (actual ? " and ran at " + speed_text(actual) + " times real time (pauses not counted)" : std::string{}) +
                "; a device attached in real time got the ECUs' messages late by up to that. Lower the speed (speed, run "
                "--speed), or find what is slow: an ECU whose step takes long needs a lighter step (a larger step of that "
                "ECU does not help), and many ECUs with small steps cost a round of synchronisation per step (larger ECU "
                "steps and a larger time_step_ms help then). The launcher cannot tell which participant is slow: leave ECUs "
                "out one at a time (--disable <name>) and compare",
            nullptr);
    }
    if (!outcome.timing.coarse_step_checked)
    {
        if (auto f = detail::coarse_time_step(outcome.timing))
        {
            out.push_back(std::move(*f));
        }
    }
    if (outcome.scenario)
    {
        const ScenarioRun& scenario = *outcome.scenario;
        const auto add_scenario = [&](std::string code, std::string message, std::size_t line) {
            out.push_back(Finding{Severity::Error, std::move(code), std::move(message), line, "", "", "", scenario.file});
        };
        for (const ScenarioStepRun& step : scenario.steps)
        {
            if (step.status == StepStatus::Failed)
            {
                add_scenario(step.code, step.text + ": " + step.message, step.line);
            }
        }
        if (scenario.status == ScenarioStatus::Incomplete)
        {
            const auto reached = std::find_if(scenario.steps.rbegin(), scenario.steps.rend(), [](const ScenarioStepRun& step) {
                return step.status != StepStatus::NotRun;
            });
            const auto passed = static_cast<std::size_t>(std::count_if(
                scenario.steps.begin(), scenario.steps.end(), [](const ScenarioStepRun& step) { return step.status == StepStatus::Passed; }));
            add_scenario("scenario-incomplete",
                         "the scenario did not get to its end: the simulation ended at " + std::to_string(outcome.timing.sim_ms) +
                             " ms when " + std::to_string(passed) + " of " + std::to_string(scenario.steps.size()) +
                             " steps had passed" +
                             (reached != scenario.steps.rend() && reached->status == StepStatus::Unfinished
                                  ? " and the step at line " + std::to_string(reached->line) + " (" + reached->text + ") was not decided yet"
                                  : std::string{}) +
                             (outcome.scenario_error.empty()
                                  ? "; the other findings and the lines above say why the simulation ended"
                                  : "; the launcher's scenario thread failed (" + outcome.scenario_error +
                                        "): this is an error of vehicle_sil_launch"),
                         reached != scenario.steps.rend() ? reached->line : 0);
        }
    }
    if (outcome.simulation == SimulationEnd::Ended && outcome.stop_at_ms && outcome.controller_state.empty() &&
        !outcome.stopped_by_scenario)
    {
        add(Severity::Note, "ended-before-stop-at",
            "the simulation ended before stop_at_ms (" + std::to_string(*outcome.stop_at_ms) +
                " ms): a participant stopped it; if that is not expected, the lines above show which one",
            nullptr);
    }
    sort_findings(out);
    return out;
}

std::vector<Finding> detail::settled_findings(const RunOutcome& outcome)
{
    std::vector<Finding> out;
    for (Finding& f : assess_run(outcome))
    {
        // What a participant that failed in the simulation did is said with how its process ended,
        // once it has: until then the finding would still change.
        if (f.code == "participant-failed")
        {
            const auto p = std::find_if(outcome.participants.begin(), outcome.participants.end(),
                                        [&f](const ParticipantRun& run) { return run.name == f.participant; });
            if (p == outcome.participants.end() || !(p->exit && ended_by_itself(*p)))
            {
                continue;
            }
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<double> RunTiming::actual_speed() const
{
    const std::int64_t ran = wall_ms - paused_ms;
    if (ran < 1)
    {
        return std::nullopt;
    }
    return std::round(static_cast<double>(sim_ms) / static_cast<double>(ran) * 1000.0) / 1000.0;
}

RunTiming detail::plan_time_step(const SimulationConfig& config, const CheckReport& check, const std::optional<double>& speed,
                                 bool fineStep)
{
    RunTiming t;
    // The synchronised ECUs' steps (an ECU without one runs in real time: the gate does not move it).
    bool known = true;
    std::vector<std::int64_t> steps;
    for (std::size_t i = 0; i < config.participants.size(); ++i)
    {
        if (!config.participants[i].enabled)
        {
            continue;
        }
        if (i < check.participants.size() && check.participants[i].status == ParticipantStatus::Described &&
            check.participants[i].ecu)
        {
            if (const auto& step = check.participants[i].ecu->description.step)
            {
                steps.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(*step).count());
            }
        }
        else
        {
            known = false;
        }
    }
    SimulationConfig unwritten = config;
    unwritten.time_step_ms.reset();
    const std::int64_t coarsest = time_step_ms(unwritten);
    // The step at which every ECU step starts at a controller step.
    std::int64_t fine = coarsest * kNanosPerMilli;
    for (const std::int64_t step : steps)
    {
        fine = std::gcd(fine, step);
    }
    t.fine_step_ms = known && fine % kNanosPerMilli == 0 ? fine / kNanosPerMilli : 1;
    t.time_step_written = config.time_step_ms.has_value();
    t.time_step_ms = t.time_step_written ? *config.time_step_ms : speed || fineStep ? t.fine_step_ms : coarsest;
    if (known)
    {
        // An ECU step at T runs when the controller's first step at or after T is due.
        const std::int64_t gate = t.time_step_ms * kNanosPerMilli;
        std::int64_t delay = 0;
        for (const std::int64_t step : steps)
        {
            delay = std::max(delay, gate - std::gcd(gate, step));
        }
        t.step_delay_ns = delay;
        if (!steps.empty())
        {
            t.ecu_step_ns = *std::min_element(steps.begin(), steps.end());
        }
    }
    return t;
}

std::optional<Finding> detail::coarse_time_step(const RunTiming& t)
{
    // Only where a speed counts: a run as fast as possible keeps no real time anyway.
    if (!((t.speed || t.paced_steps > 0) && t.step_delay_ns.value_or(0) > 0 && t.fine_step_ms > 0 &&
          t.fine_step_ms < t.time_step_ms))
    {
        return std::nullopt;
    }
    const std::string delay = millis_text(*t.step_delay_ns);
    return Finding{Severity::Warning, "coarse-time-step",
            "the launcher's controller steps every " + std::to_string(t.time_step_ms) + " ms (" +
                (t.time_step_written ? "time_step_ms" : "the default of a run that starts without a speed") + "), but " +
                (t.ecu_step_ns && *t.ecu_step_ns < t.time_step_ms * kNanosPerMilli
                     ? "the ECUs' steps are as small as " + millis_text(*t.ecu_step_ns) + " ms"
                     : std::string("not every ECU step starts at one of its steps")) +
                ": with a speed, the launcher keeps real time only at the controller's steps. An ECU step that starts "
                "between two of them runs at the later one, up to " +
                delay + " ms after the real time it stands for (never before it), together with the other steps in "
                        "between; a device attached in real time gets the messages of those steps up to " +
                delay + " ms late and in a burst. Write time_step_ms: " + std::to_string(t.fine_step_ms) +
                (t.time_step_written ? " (or leave time_step_ms out: with a speed, the default is that)"
                                     : " (or give the speed from the start, with speed or run --speed: the default is then that)"), 0, "", ""};
}

std::size_t RunReport::errors() const { return check.errors() + count(findings, Severity::Error); }
std::size_t RunReport::warnings() const { return check.warnings() + count(findings, Severity::Warning); }
std::size_t RunReport::notes() const { return check.notes() + count(findings, Severity::Note); }

RunResult RunReport::result() const
{
    if (usage || check.usage)
    {
        return RunResult::Usage;
    }
    if (!check.config)
    {
        return RunResult::Unreadable;
    }
    if (!run)
    {
        return RunResult::CheckFailed;
    }
    if (!run->interrupted_by.empty())
    {
        return RunResult::Interrupted;
    }
    if (!run->setup_error.empty())
    {
        return RunResult::SetupFailed;
    }
    const bool ended = run->simulation == SimulationEnd::ReachedStopAt || run->simulation == SimulationEnd::Ended;
    if (!ended || errors() > 0 || (strict && warnings() > 0))
    {
        return RunResult::Failed;
    }
    return RunResult::Passed;
}

int RunReport::exit_code() const
{
    switch (result())
    {
    case RunResult::Passed: return 0;
    case RunResult::Failed:
    case RunResult::CheckFailed: return 1;
    case RunResult::Unreadable:
    case RunResult::SetupFailed: return 3;
    case RunResult::Usage: return 4;
    case RunResult::Interrupted: return kExitInterrupted;
    }
    return 1;
}

} // namespace vehicle_sil::launch
