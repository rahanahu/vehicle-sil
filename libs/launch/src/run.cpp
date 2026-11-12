// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/run.hpp"

#include "check_format.hpp"

#include "vehicle_sil/launch/events.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <silkit/config/IParticipantConfiguration.hpp>
#include <silkit/services/orchestration/string_utils.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <numeric>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace vehicle_sil::launch {

namespace {

constexpr std::string_view kLauncherName = "vehicle_sil_launch";
constexpr std::string_view kLauncherTag = "[vehicle_sil_launch] ";

std::string prefix_of(const ParticipantRun& p) { return "participant '" + p.name + "': "; }

std::string hex_code(int code)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(code));
    return text;
}

/// "exited with code 7", "was ended by signal SIGSEGV".
std::string exit_text(const process::ExitStatus& exit)
{
    if (exit.signal)
    {
        return "was ended by signal " + process::signal_name(*exit.signal);
    }
    // A Windows NTSTATUS (0xC0000005 an access violation, 0xC000013A Ctrl+C) reads better in hex.
    return "exited with code " + (exit.code < 0 ? hex_code(exit.code) : std::to_string(exit.code));
}

/// The cell of the participants' table.
std::string ending_cell(const ParticipantRun& p)
{
    const auto status = [&p] {
        if (!p.exit)
        {
            return std::string{};
        }
        return p.exit->signal ? "signal " + process::signal_name(*p.exit->signal)
                              : "exit code " + (p.exit->code < 0 ? hex_code(p.exit->code) : std::to_string(p.exit->code));
    };
    switch (p.ending)
    {
    case Ending::NotStarted:
        return p.enabled ? "not started" : "not started (disabled)";
    case Ending::ExitedBeforeStart:
    case Ending::Exited:
        return status();
    case Ending::Stopped:
        return "stopped (" + std::string(stop_request_name()) + "), " + status();
    case Ending::Killed:
        return "killed";
    }
    return "-";
}

bool ended_by_itself(const ParticipantRun& p)
{
    return p.ending == Ending::Exited || p.ending == Ending::ExitedBeforeStart;
}

bool failed_exit(const ParticipantRun& p)
{
    return p.exit && ended_by_itself(p) && (p.exit->code != 0 || p.exit->signal);
}

/// "its process exited with code 2", or what is known without its status.
std::string process_text(const ParticipantRun& p)
{
    return p.exit ? "its process " + exit_text(*p.exit) : std::string("its process ended");
}

std::string lines_hint(const ParticipantRun& p) { return "the lines [" + p.name + "] above show what it wrote"; }

const char* ending_name(Ending ending)
{
    switch (ending)
    {
    case Ending::NotStarted: return "not_started";
    case Ending::ExitedBeforeStart:
    case Ending::Exited: return "exited";
    case Ending::Stopped: return "stopped";
    case Ending::Killed: return "killed";
    }
    return "not_started";
}

const char* simulation_name(SimulationEnd end)
{
    switch (end)
    {
    case SimulationEnd::NotStarted: return "not_started";
    case SimulationEnd::ReachedStopAt: return "reached_stop_at";
    case SimulationEnd::Ended: return "ended";
    case SimulationEnd::Aborted: return "aborted";
    case SimulationEnd::Interrupted: return "interrupted";
    }
    return "not_started";
}

const char* result_name(RunResult result)
{
    switch (result)
    {
    case RunResult::Passed: return "passed";
    case RunResult::Failed: return "failed";
    case RunResult::Interrupted: return "interrupted";
    case RunResult::CheckFailed: return "check_failed";
    case RunResult::Unreadable: return "unreadable";
    case RunResult::SetupFailed: return "setup_failed";
    case RunResult::Usage: return "usage";
    }
    return "failed";
}

constexpr std::int64_t kNanosPerMilli = 1'000'000;

/// Nanoseconds as milliseconds in the shortest form ("10", "2.5").
std::string millis_text(std::int64_t ns)
{
    return speed_text(static_cast<double>(ns) / static_cast<double>(kNanosPerMilli));
}

std::size_t count(const std::vector<Finding>& findings, Severity severity)
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [severity](const Finding& f) { return f.severity == severity; }));
}

} // namespace

std::string_view stop_request_name()
{
#ifdef _WIN32
    return "Ctrl+Break";
#else
    return "SIGTERM";
#endif
}

// A finding that is added here and that the end of the participants can still change (end_children():
// exit, ending, stopped_after_end, stop_request_failed, left_processes, interrupted_by,
// interrupted_while_ending) must be left out by detail::settled_findings() until it cannot.
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
    if (!outcome.controller_state.empty())
    {
        add(Severity::Error, "simulation",
            "the simulation ended in state " + outcome.controller_state +
                " instead of shutting down normally: the lines above show what the participants wrote",
            nullptr);
    }
    if (outcome.timed_out)
    {
        add(Severity::Error, "timeout",
            "the simulation did not end within " + std::to_string(outcome.timeout_ms.value_or(0)) +
                " ms of wall-clock time (--timeout-ms), so the launcher aborted it: a participant may hang in a step "
                "(the lines above show what the participants wrote); if it is only slow, raise --timeout-ms",
            nullptr);
    }
    else if (outcome.simulation == SimulationEnd::Aborted && outcome.failed_participant.empty())
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
    // Only where a speed counts: a run as fast as possible keeps no real time anyway.
    if (const RunTiming& t = outcome.timing; (t.speed || t.paced_steps > 0) && t.step_delay_ns.value_or(0) > 0 &&
                                             t.fine_step_ms > 0 && t.fine_step_ms < t.time_step_ms)
    {
        const std::string delay = millis_text(*t.step_delay_ns);
        add(Severity::Warning, "coarse-time-step",
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
                                     : " (or give the speed from the start, with speed or run --speed: the default is then that)"),
            nullptr);
    }
    if (outcome.simulation == SimulationEnd::Ended && outcome.stop_at_ms && outcome.controller_state.empty())
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

std::string detail::timing_text(const RunTiming& timing)
{
    if (!timing.speed && timing.paced_steps == 0)
    {
        return {};
    }
    const auto actual = timing.actual_speed();
    std::string out = "time: speed " + speed_text(timing.speed) + ", ran at " + (actual ? speed_text(actual) : std::string("-")) +
                      " times real time; " + std::to_string(timing.late_steps) + " of " +
                      std::to_string(timing.paced_steps) + " paced steps more than " +
                      std::to_string(timing.late_tolerance_ms) + " ms late (the latest by " +
                      std::to_string(timing.max_late_ms) + " ms), " + std::to_string(timing.slipped_ms) + " ms behind in all";
    if (timing.paused_ms > 0)
    {
        out += "; paused for " + std::to_string(timing.paused_ms) + " ms (not counted)";
    }
    return out;
}

RunTiming detail::plan_time_step(const SimulationConfig& config, const CheckReport& check, const std::optional<double>& speed)
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
    t.time_step_ms = t.time_step_written ? *config.time_step_ms : speed ? t.fine_step_ms : coarsest;
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

std::string format_run_text(const RunReport& report)
{
    const RunResult result = report.result();
    if (result == RunResult::Unreadable || result == RunResult::Usage)
    {
        return format_text(report.check);
    }
    std::string out = detail::check_text_body(report.check);
    const std::string counts = detail::count_summary(report.errors(), report.warnings(), report.notes());
    if (!report.run)
    {
        out += "nothing was started: the check found " +
               std::string(report.check.errors() > 0 ? "errors" : "warnings, and --strict fails on them") +
               " (vehicle_sil_launch check " + report.check.file + " shows the same)\n\nfailed: " + counts + "\n";
        return out;
    }
    const RunOutcome& run = *report.run;
    std::string state;
    switch (run.simulation)
    {
    case SimulationEnd::NotStarted:
        state = !run.setup_error.empty()      ? "the simulation could not be set up"
                : !run.interrupted_by.empty() ? "interrupted (" + run.interrupted_by + ") before the simulation started"
                                              : "the simulation did not start";
        break;
    case SimulationEnd::ReachedStopAt:
        state = "the simulation ran to stop_at_ms (" + std::to_string(run.stop_at_ms.value_or(0)) + " ms) and ended";
        break;
    case SimulationEnd::Ended:
        state = run.controller_state.empty() ? "the simulation ran until a participant ended it"
                                             : "the simulation ended in state " + run.controller_state;
        break;
    case SimulationEnd::Aborted:
        state = run.timed_out ? "the simulation was aborted: it did not end within --timeout-ms (" +
                                    std::to_string(run.timeout_ms.value_or(0)) + " ms)"
                : run.failed_participant.empty() ? "the simulation was aborted"
                                                 : "the simulation was aborted: participant '" + run.failed_participant + "' failed";
        break;
    case SimulationEnd::Interrupted:
        state = "interrupted (" + run.interrupted_by + "): the simulation was stopped";
        break;
    }
    out += "run: " + state + "\n";
    if (const std::string time = run.simulation != SimulationEnd::NotStarted ? detail::timing_text(run.timing) : std::string{};
        !time.empty())
    {
        out += time + "\n";
    }
    out += "\n";
    if (!run.participants.empty())
    {
        std::vector<std::vector<std::string>> rows{{"participant", "required", "ended"}};
        for (const auto& p : run.participants)
        {
            rows.push_back({p.name, !p.enabled ? "-" : p.required ? "yes" : "no", ending_cell(p)});
        }
        out += detail::table_text(rows) + "\n";
    }
    out += detail::findings_text(report.check.file, report.findings);
    switch (result)
    {
    case RunResult::Passed: out += "passed: " + counts + "\n"; break;
    case RunResult::Interrupted: out += "interrupted: " + counts + "\n"; break;
    default:
        out += "failed: " + counts + (report.errors() == 0 && run.setup_error.empty() ? " (--strict)" : "") + "\n";
        break;
    }
    return out;
}

std::string format_run_json(const RunReport& report)
{
    vehicle_sil::detail::JsonWriter w;
    detail::write_run_json(w, {}, report);
    std::string out = w.take();
    out += '\n';
    return out;
}

void detail::write_run_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const RunReport& report)
{
    w.begin_object(key);
    w.integer("vehicle_sil_run", kRunFormat);
    w.string("vehicle_sil", version());
    if (report.check.file.empty())
    {
        w.null("file");
    }
    else
    {
        w.string("file", report.check.file);
    }
    w.string("result", result_name(report.result()));
    w.boolean("strict", report.strict);
    w.integer("errors", static_cast<std::int64_t>(report.errors()));
    w.integer("warnings", static_cast<std::int64_t>(report.warnings()));
    w.integer("notes", static_cast<std::int64_t>(report.notes()));
    if (report.run)
    {
        w.string("simulation", simulation_name(report.run->simulation));
    }
    else
    {
        w.null("simulation");
    }
    if (report.run && !report.run->interrupted_by.empty())
    {
        w.string("interrupted_by", report.run->interrupted_by);
    }
    else
    {
        w.null("interrupted_by");
    }
    if (report.run)
    {
        const RunTiming& t = report.run->timing;
        w.begin_object("timing");
        if (t.speed)
        {
            w.real("speed", t.speed);
        }
        else
        {
            w.string("speed", "max");
        }
        const auto millis = [](const std::optional<std::int64_t>& ns) {
            return ns ? std::optional<double>(static_cast<double>(*ns) / static_cast<double>(kNanosPerMilli)) : std::nullopt;
        };
        w.integer("time_step_ms", t.time_step_ms);
        w.real("ecu_step_ms", millis(t.ecu_step_ns));
        w.real("step_delay_ms", millis(t.step_delay_ns));
        w.integer("late_tolerance_ms", t.late_tolerance_ms);
        w.integer("sim_ms", t.sim_ms);
        w.integer("wall_ms", t.wall_ms);
        w.real("actual_speed", t.actual_speed());
        w.integer("paced_steps", static_cast<std::int64_t>(t.paced_steps));
        w.integer("late_steps", static_cast<std::int64_t>(t.late_steps));
        w.integer("max_late_ms", t.max_late_ms);
        w.integer("slipped_ms", t.slipped_ms);
        w.integer("late_ms", t.late_ms);
        w.integer("paused_ms", t.paused_ms);
        w.end_object();
    }
    else
    {
        w.null("timing");
    }
    w.begin_array("participants");
    if (report.run)
    {
        for (const auto& p : report.run->participants)
        {
            w.begin_object();
            w.string("name", p.name);
            w.boolean("required", p.required);
            w.boolean("enabled", p.enabled);
            w.string("ended", ending_name(p.ending));
            if (p.exit && !p.exit->signal)
            {
                w.integer("exit_code", p.exit->code);
            }
            else
            {
                w.null("exit_code");
            }
            if (p.exit && p.exit->signal)
            {
                w.string("signal", process::signal_name(*p.exit->signal));
            }
            else
            {
                w.null("signal");
            }
            w.boolean("left_processes", p.left_processes);
            if (p.log.empty())
            {
                w.null("log");
            }
            else
            {
                w.string("log", p.log);
            }
            w.end_object();
        }
    }
    w.end_array();
    detail::write_findings_json(w, report.check.file, report.findings);
    detail::write_check_json(w, "check", report.check);
    w.end_object();
}

std::string format_internal_error_json(bool run, const std::string& message)
{
    CheckReport check;
    check.findings.push_back(Finding{Severity::Error, "internal", message, 0, "", ""});
    if (!run)
    {
        return format_json(check);
    }
    // The run's own findings: where a reader of the run's JSON looks first.
    RunReport report;
    report.findings = std::move(check.findings);
    return format_run_json(report);
}

namespace {

namespace orch = SilKit::Services::Orchestration;

} // namespace

std::string detail::log_name(const std::string& name)
{
    std::string out;
    for (const char c : name)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                           c == '-' || c == '_';
        out += plain ? c : '_';
    }
    // Windows reads these names (also before an extension) as devices: the same rule everywhere,
    // so that one configuration gives the same files.
    static constexpr std::string_view kDevices[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                                    "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string stem = out.substr(0, out.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (std::find(std::begin(kDevices), std::end(kDevices), stem) != std::end(kDevices))
    {
        out.insert(stem.size(), "_");
    }
    return out;
}

std::int64_t detail::ecu_start_timeout_ms(const SimulationConfig& config)
{
    // The launcher waits start_timeout_ms for the start and then stops the participants within
    // stop_timeout_ms; the ECU must not give up first (it would end, and the launcher would name it
    // instead of who did not join). 10 s more for a slow machine; never more than ecu_main accepts.
    return std::min(config.start_timeout_ms + config.stop_timeout_ms + 10000, kMaxMillis);
}

detail::StopRequest detail::record_stop_request(ParticipantRun& run, bool sent, const std::optional<process::ExitStatus>& status)
{
    if (sent)
    {
        return StopRequest::Sent;
    }
    if (status)
    {
        run.exit = status;
        run.ending = Ending::Exited;
        run.stopped_after_end = false;
        return StopRequest::HadEnded;
    }
    run.stop_request_failed = true;
    return StopRequest::CannotSend;
}

std::string detail::uri_for_participants(std::string uri)
{
    for (const auto& [any, loopback] : {std::pair<std::string, std::string>{"silkit://0.0.0.0:", "silkit://127.0.0.1:"},
                                        std::pair<std::string, std::string>{"silkit://[::]:", "silkit://[::1]:"}})
    {
        if (uri.rfind(any, 0) == 0)
        {
            return loopback + uri.substr(any.size());
        }
    }
    return uri;
}

// A child's output must not move the cursor or end the line early on the launcher's terminal.
std::string detail::printable(std::string_view line)
{
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size();)
    {
        const auto byte = static_cast<unsigned char>(line[i]);
        if (byte < 0x80)
        {
            out += (byte < 0x20 && byte != '\t') || byte == 0x7F ? '?' : static_cast<char>(byte);
            ++i;
            continue;
        }
        // A UTF-8 sequence: its length, the smallest value of that length, the value.
        const std::size_t length = byte >= 0xF5 ? 0 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC2 ? 2 : 0;
        std::uint32_t value = length == 4 ? byte & 0x07u : length == 3 ? byte & 0x0Fu : byte & 0x1Fu;
        bool valid = length != 0 && i + length <= line.size();
        for (std::size_t k = 1; valid && k < length; ++k)
        {
            const auto next = static_cast<unsigned char>(line[i + k]);
            valid = (next & 0xC0) == 0x80;
            value = (value << 6) | (next & 0x3Fu);
        }
        const std::uint32_t smallest = length == 4 ? 0x10000 : length == 3 ? 0x800 : 0x80;
        valid = valid && value >= smallest && value <= 0x10FFFF && (value < 0xD800 || value > 0xDFFF);
        if (!valid)
        {
            out += '?'; // not UTF-8: one '?' per byte that starts no character
            ++i;
            continue;
        }
        // The C1 control characters (U+0080 to U+009F) steer a terminal too.
        if (value <= 0x9F)
        {
            out += '?';
        }
        else
        {
            out.append(line.substr(i, length));
        }
        i += length;
    }
    return out;
}

struct TimeCommands::State
{
    std::mutex mutex;
    EventStream* events = nullptr;
    /// A pause was asked for (or the run starts paused) and no resume since.
    bool paused = false;
    /// The last speed command before attach() (the inner empty: "max").
    std::optional<std::optional<double>> speed{};
    SimulationController* controller = nullptr;
    std::int64_t step_ms = 1;
    bool detached = false;
};

TimeCommands::TimeCommands(bool start_paused, EventStream* events) : state_(std::make_unique<State>())
{
    state_->paused = start_paused;
    state_->events = events;
}

TimeCommands::~TimeCommands() = default;

Command TimeCommands::apply(const Command& command, std::string_view source)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    Command out = command;
    const std::string name = command.name.value_or("");
    const auto refuse = [&out](std::string why) {
        out.accepted = false;
        out.message = std::move(why);
    };
    if (s.detached)
    {
        refuse("the simulation is not running (it is over, or it did not start): " + name +
               " works from the start of the run until the simulation ends");
    }
    else if (name == "step" && s.controller == nullptr)
    {
        refuse("the simulation has not been set up yet: step works once it stands still (a time event with paused "
               "true), with --start-paused from the start");
    }
    else if (name == "step" && !s.paused)
    {
        refuse("step works while the simulation is paused: send {\"command\":\"pause\"} first (or run with "
               "--start-paused)");
    }
    else if (name == "step")
    {
        // Whole steps of the controller: the simulation can stand still only at one of them.
        out.ms = (command.ms.value_or(1) + s.step_ms - 1) / s.step_ms * s.step_ms;
    }
    if (s.events != nullptr)
    {
        s.events->command(out, source);
    }
    if (!out.accepted)
    {
        return out;
    }
    SimulationController* c = s.controller;
    if (name == "pause")
    {
        s.paused = true;
        if (c != nullptr)
        {
            c->pause();
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

void TimeCommands::detach()
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    s.controller = nullptr;
    s.detached = true;
}

namespace {

using detail::log_name;
using detail::printable;
using detail::uri_for_participants;

/// `path` without its "." parts ("dir/./engine" as "dir/engine"); ".." stays (it may cross a
/// symbolic link).
std::filesystem::path without_dots(const std::filesystem::path& path)
{
    std::filesystem::path out;
    for (const auto& part : path)
    {
        if (part != ".")
        {
            out /= part;
        }
    }
    return out;
}

std::string command_text(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
{
    std::string out = process::shown_path(executable);
    for (const auto& argument : arguments)
    {
        out += ' ';
        const bool plain = !argument.empty() && argument.find_first_of(" \t\"'") == std::string::npos;
        out += plain ? argument : "'" + argument + "'";
    }
    return out;
}

/// run --events: the virtual time is written at most this often while it moves.
constexpr std::chrono::milliseconds kTimeInterval{100};

class Runner
{
public:
    Runner(const SimulationConfig& config, const CheckReport& check, std::filesystem::path base, const RunOptions& options) :
      config_(config), check_(check), base_(std::move(base)), options_(options)
    {
        outcome_.start_timeout_ms = config.start_timeout_ms;
        outcome_.stop_timeout_ms = config.stop_timeout_ms;
        outcome_.stop_at_ms = config.stop_at_ms;
        const std::optional<double> speed = options.speed ? *options.speed : config.speed;
        outcome_.timing = detail::plan_time_step(config, check, speed);
        outcome_.timing.speed = speed;
        outcome_.timing.late_tolerance_ms = late_tolerance_ms(config);
        if (options.time_commands == nullptr)
        {
            own_commands_ = std::make_unique<TimeCommands>(options.start_paused);
        }
        if (options.timeout)
        {
            outcome_.timeout_ms = options.timeout->count();
        }
        for (const auto& p : config.participants)
        {
            ParticipantRun run;
            run.name = p.name;
            run.required = p.required;
            run.describe = p.describe;
            run.enabled = p.enabled;
            run.line = p.line;
            outcome_.participants.push_back(std::move(run));
        }
        children_.resize(config.participants.size());
        logs_.resize(config.participants.size());
        joined_.resize(config.participants.size(), false);
    }

    RunOutcome run()
    {
        // Before the guard below, so that they go after it.
        std::unique_ptr<SilKit::Vendor::Vector::ISilKitRegistry> registry;
        std::unique_ptr<SimulationController> controller;
        // Whatever happens, the time commands end with the run (they are refused from then on),
        // and before the controller they are given to goes: the thread that reads the commands
        // may apply one at any time.
        struct Detach
        {
            TimeCommands& commands;
            ~Detach() { commands.detach(); }
        } const detach{commands()};
        if (interrupts() > 0)
        {
            outcome_.interrupted_by = interrupt_name();
            return outcome_;
        }
        event([](EventStream& e) { e.phase(Phase::Starting); });
        if (!open_logs())
        {
            return outcome_;
        }
        std::string uri;
        try
        {
            registry = SilKit::Vendor::Vector::CreateSilKitRegistry(SilKit::Config::ParticipantConfigurationFromString("{}"));
            const std::string listen = config_.registry_uri.value_or("silkit://127.0.0.1:0");
            try
            {
                uri = uri_for_participants(registry->StartListening(listen));
            }
            catch (const std::exception& e)
            {
                throw std::runtime_error("the registry cannot listen at " + listen + ": " + one_line(e.what()) +
                                         (config_.registry_uri ? " (does another registry run there? choose another port in "
                                                                 "registry_uri, or leave it out for a free port)"
                                                               : ""));
            }
            event([&uri](EventStream& e) { e.registry(uri); });
            say("registry at " + uri);
            SimulationControllerOptions c;
            c.participant_name = std::string(kControllerName);
            c.registry_uri = uri;
            for (const auto& p : config_.participants)
            {
                if (p.required && p.enabled)
                {
                    c.required_participants.push_back(p.name);
                }
            }
            if (config_.stop_at_ms)
            {
                c.stop_at = std::chrono::milliseconds(*config_.stop_at_ms);
            }
            // The time gate, always (also as fast as possible): one way for every run. The step
            // divides stop_at_ms (detail::plan_time_step()), so the simulation stops at stop_at_ms
            // itself.
            TimeControlOptions time;
            time.step = std::chrono::milliseconds(outcome_.timing.time_step_ms);
            time.late_tolerance = std::chrono::milliseconds(outcome_.timing.late_tolerance_ms);
            c.time_control = time;
            controller = std::make_unique<SimulationController>(std::move(c));
            commands().attach(*controller, outcome_.timing.time_step_ms, outcome_.timing.speed);
        }
        catch (const std::exception& e)
        {
            outcome_.setup_error = one_line(e.what());
            return outcome_;
        }

        bool ran = false;
        if (start_children(uri))
        {
            event([](EventStream& e) { e.phase(Phase::Waiting); });
            if (wait_for_start(*controller))
            {
                ran = true;
                watch(*controller);
            }
        }
        // What is known for good when the simulation is over (why it ended), before the
        // participants end.
        publish_findings(false);
        event([](EventStream& e) { e.phase(Phase::Stopping); });
        end_children(ran);
        commands().detach();
        controller.reset();
        registry.reset();
        return outcome_;
    }

    /// run --events: the run's findings that have not been written yet. Before the participants
    /// have ended (`final` false) only those that their end cannot change (settled_findings()):
    /// what is written as an event is in the result as it was written.
    void publish_findings(bool final)
    {
        if (options_.events == nullptr)
        {
            return;
        }
        for (const Finding& f : final ? assess_run(outcome_) : detail::settled_findings(outcome_))
        {
            if (std::find(published_.begin(), published_.end(), f) == published_.end())
            {
                options_.events->finding(check_.file, f);
                published_.push_back(f);
            }
        }
    }

private:
    [[nodiscard]] TimeCommands& commands() { return own_commands_ ? *own_commands_ : *options_.time_commands; }
    [[nodiscard]] int interrupts() const { return options_.interrupts != nullptr ? options_.interrupts->load() : 0; }
    [[nodiscard]] std::string interrupt_name() const
    {
        return options_.interrupt_name ? options_.interrupt_name() : std::string("interrupt");
    }

    static std::string one_line(std::string text)
    {
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        return text;
    }

    void write(const std::string& line)
    {
        if (options_.write_line)
        {
            options_.write_line(line);
        }
    }

    void say(const std::string& text)
    {
        const std::lock_guard lock(output_);
        if (options_.events != nullptr)
        {
            options_.events->log(std::string(kLauncherName), text);
        }
        else
        {
            write(std::string(kLauncherTag) + text);
        }
    }

    /// Writes an event (run --events) with what `emit` writes.
    template <class Emit>
    void event(Emit emit)
    {
        if (options_.events != nullptr)
        {
            emit(*options_.events);
        }
    }

    bool open_logs()
    {
        if (options_.log_dir.empty())
        {
            return true;
        }
        std::error_code ec;
        std::filesystem::create_directories(options_.log_dir, ec);
        // Compared without case: on Windows and macOS "Engine.log" and "engine.log" are one file.
        std::set<std::string> used;
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        for (std::size_t i = 0; i < config_.participants.size(); ++i)
        {
            if (!config_.participants[i].enabled)
            {
                continue; // not started: no log
            }
            std::string name = log_name(config_.participants[i].name);
            for (int n = 2; !used.insert(lower(name)).second; ++n)
            {
                name = log_name(config_.participants[i].name) + "-" + std::to_string(n);
            }
            const auto file = options_.log_dir / process::path_from_utf8(name + ".log");
            outcome_.participants[i].log = process::shown_path(file);
            logs_[i] = std::make_unique<std::ofstream>(file, std::ios::binary | std::ios::trunc);
            if (!*logs_[i])
            {
                outcome_.setup_error = "cannot write the log " + process::shown_path(file) +
                                       ": check that --log-dir names a directory you can write to";
                return false;
            }
        }
        return true;
    }

    void on_line(std::size_t index, std::string_view line)
    {
        const std::lock_guard lock(output_);
        if (options_.events != nullptr)
        {
            options_.events->log(config_.participants[index].name, printable(line));
        }
        else
        {
            write("[" + config_.participants[index].name + "] " + printable(line));
        }
        if (logs_[index])
        {
            *logs_[index] << line << '\n' << std::flush;
        }
    }

    /// Starts every participant in the file's order. false if a required one could not be started.
    bool start_children(const std::string& uri)
    {
        const auto workDir = without_dots(std::filesystem::absolute(base_));
        for (std::size_t i = 0; i < config_.participants.size(); ++i)
        {
            const ParticipantConfig& p = config_.participants[i];
            ParticipantRun& run = outcome_.participants[i];
            if (!p.enabled)
            {
                say("not starting " + p.name + ": disabled");
                continue;
            }
            const std::vector<std::string> rest(p.command.begin() + 1, p.command.end());
            std::vector<std::string> arguments;
            if (p.describe)
            {
                arguments = expand_placeholders(rest, "", "");
                arguments.insert(arguments.end(), {"--name", p.name, "--registry-uri", uri, "--start-timeout-ms",
                                                   std::to_string(detail::ecu_start_timeout_ms(config_))});
            }
            else
            {
                arguments = expand_placeholders(rest, uri, p.name);
            }
            const std::filesystem::path executable = without_dots(std::filesystem::absolute(check_.participants[i].executable));
            say("starting " + p.name + ": " + command_text(executable, arguments));
            process::ProcessOptions options;
            options.working_directory = workDir;
            options.on_line = [this, i](process::Stream, std::string_view line) { on_line(i, line); };
            try
            {
                // Its lines wait (on_line() takes output_) until its "started" event is out.
                const std::lock_guard lock(output_);
                children_[i] = std::make_unique<process::Process>(executable, arguments, std::move(options));
                event([&](EventStream& e) {
                    std::vector<std::string> command{process::shown_path(executable)};
                    command.insert(command.end(), arguments.begin(), arguments.end());
                    e.participant_started(p.name, children_[i]->id(), command, run.log);
                });
            }
            catch (const std::exception& e)
            {
                run.start_error = one_line(e.what());
                event([&](EventStream& stream) { stream.participant_not_started(p.name, run.start_error); });
                say(p.name + " could not be started: " + run.start_error);
                if (p.required)
                {
                    return false;
                }
            }
        }
        return true;
    }

    /// Notes the participants whose process has ended since the last look; returns the first
    /// required one among them (or -1).
    int look_at_children(bool before_start)
    {
        int ended = -1;
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            ParticipantRun& run = outcome_.participants[i];
            if (!children_[i] || run.exit)
            {
                continue;
            }
            if (auto status = children_[i]->poll())
            {
                run.exit = status;
                run.ending = before_start ? Ending::ExitedBeforeStart : Ending::Exited;
                say(run.name + " " + exit_text(*status));
                event([&](EventStream& e) { e.participant_ended(run.name, *status); });
                if (run.required && ended < 0)
                {
                    ended = static_cast<int>(i);
                }
            }
        }
        return ended;
    }

    /// Waits until every required participant has joined, then starts the simulation. false if
    /// it did not start (outcome_ says why).
    bool wait_for_start(SimulationController& controller)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.start_timeout_ms);
        while (true)
        {
            if (interrupts() > 0)
            {
                outcome_.interrupted_by = interrupt_name();
                say("interrupted (" + outcome_.interrupted_by + ") before the simulation started");
                return false;
            }
            // Who has not joined is read before the ends of the processes: at the deadline, who
            // did not come is named even if a process ends in the same round.
            const std::vector<std::string> missing = controller.missing_participants();
            const bool late = !missing.empty() && std::chrono::steady_clock::now() >= deadline;
            note_joined(controller);
            const int ended = look_at_children(true);
            if (ended >= 0 && !late)
            {
                outcome_.failed_participant = outcome_.participants[static_cast<std::size_t>(ended)].name;
                note_failed(outcome_.failed_participant, "ended_before_start");
                return false;
            }
            if (missing.empty())
            {
                break;
            }
            if (late)
            {
                outcome_.start_timed_out = true;
                std::string names;
                for (auto& run : outcome_.participants)
                {
                    if (std::find(missing.begin(), missing.end(), run.name) != missing.end())
                    {
                        run.missing_at_start = true;
                        names += (names.empty() ? "" : ", ") + run.name;
                    }
                }
                say("the simulation did not start: not joined within " + std::to_string(config_.start_timeout_ms) +
                    " ms: " + names);
                return false;
            }
            std::this_thread::sleep_for(options_.poll);
        }
        try
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            controller.start(std::max(left, std::chrono::milliseconds(1000)));
        }
        catch (const std::exception& e)
        {
            outcome_.start_error = one_line(e.what());
            say("the simulation did not start: " + outcome_.start_error);
            return false;
        }
        event([](EventStream& e) { e.phase(Phase::Running); });
        say("simulation running");
        // Every participant starts at virtual time 0.
        note_time(controller, TimeNote::Start);
        return true;
    }

    /// run --events: the launcher has decided that `name` failed and ends the run because of it
    /// (`reason`: one word that stays). Written at once, as an event that is never dropped; with
    /// the message of its finding if that is settled already (the finding itself is written when
    /// it is settled: publish_findings()).
    void note_failed(const std::string& name, std::string_view reason)
    {
        if (options_.events == nullptr)
        {
            return;
        }
        std::string message;
        for (const Finding& f : detail::settled_findings(outcome_))
        {
            if (f.participant == name && (f.code == "participant-failed" || f.code == "ended-before-start"))
            {
                message = f.message;
            }
        }
        options_.events->participant_failed(name, reason, message);
    }

    /// run --events: "joined" for each started participant whose lifecycle the controller has seen
    /// since the last look (also an optional one; not after its process has ended).
    void note_joined(SimulationController& controller)
    {
        if (options_.events == nullptr)
        {
            return;
        }
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            ParticipantRun& run = outcome_.participants[i];
            if (!children_[i] || joined_[i] || run.exit || run.ending == Ending::Killed)
            {
                continue;
            }
            if (controller.runtime().peer_status(run.name).state)
            {
                joined_[i] = true;
                options_.events->participant(run.name, ParticipantState::Joined);
            }
        }
    }

    enum class TimeNote
    {
        Start, ///< the simulation runs: virtual time 0
        Poll,  ///< a look of the watch
        Last,  ///< the simulation is over
    };

    /// run --events: the controller's virtual time and how it goes. At the start 0; then when it
    /// stands still or runs on, or the speed changes, at once; when it has moved, at most every
    /// kTimeInterval; at the end, if anything changed.
    void note_time(SimulationController& controller, TimeNote note)
    {
        if (options_.events == nullptr)
        {
            return;
        }
        const TimeState s = controller.time_state();
        const auto wall = std::chrono::steady_clock::now();
        const std::int64_t now = sim_ms(controller, s);
        // Standing still is decided once the gate stops at the time it is at (start paused); an
        // ended simulation stands still nowhere (time_state()).
        const bool paused = s.paused || (note != TimeNote::Last && s.pause_target && *s.pause_target <= s.now);
        const std::int64_t ms =
            note == TimeNote::Start
                ? 0
                : std::max(last_time_ms_, now);
        // The actual speed over about the last second while it ran at one speed.
        if (paused || paused != last_paused_ || s.speed != last_speed_)
        {
            samples_.clear();
        }
        if (!paused)
        {
            samples_.emplace_back(wall, ms);
            while (samples_.size() > 2 && wall - samples_[1].first >= std::chrono::seconds(1))
            {
                samples_.pop_front();
            }
        }
        std::optional<double> actual;
        if (samples_.size() >= 2 && samples_.back().first - samples_.front().first >= std::chrono::milliseconds(200))
        {
            const double seconds = std::chrono::duration<double>(samples_.back().first - samples_.front().first).count();
            actual = std::round(static_cast<double>(samples_.back().second - samples_.front().second) / 1000.0 / seconds * 1000.0) / 1000.0;
        }
        // Standing still at another time (a step has ended) is news as well.
        const bool changed = note == TimeNote::Start || paused != last_paused_ || s.speed != last_speed_ ||
                             (paused && ms != last_time_ms_);
        // A simulation that does not move although its real time has come (a stall) is news too,
        // while it lasts: the time stays, the lateness grows.
        const std::int64_t late = ceil_ms(s.current_lateness);
        const bool stalled = late > outcome_.timing.late_tolerance_ms && late != last_late_ms_;
        // Only that it moved, or is late: not while other events wait for a slow reader (they
        // would pile up).
        const bool moved = (ms > last_time_ms_ || stalled || s.late_steps != last_late_steps_) &&
                           (note == TimeNote::Last ||
                            (wall - last_time_wall_ >= kTimeInterval && options_.events->waiting_events() == 0));
        if (!changed && !moved)
        {
            return;
        }
        TimeEvent e;
        e.sim_ms = ms;
        e.speed = s.speed;
        e.paused = paused;
        e.actual_speed = actual;
        e.late_steps = s.late_steps;
        e.max_late_ms = ceil_ms(s.max_lateness);
        e.slipped_ms = ceil_ms(s.slipped);
        e.late_ms = late;
        options_.events->time(e);
        last_late_ms_ = late;
        last_late_steps_ = s.late_steps;
        last_time_ms_ = ms;
        last_time_wall_ = wall;
        last_paused_ = paused;
        last_speed_ = s.speed;
    }

    /// The controller's virtual time: the start of its step that runs or ran last (the gate's
    /// time, or the runtime's if that has gone on since the gate's numbers stopped).
    static std::int64_t sim_ms(SimulationController& controller, const TimeState& s)
    {
        const SimTime t = std::max(s.now, controller.runtime().now());
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    }

    static std::int64_t ceil_ms(std::chrono::nanoseconds d)
    {
        return std::chrono::ceil<std::chrono::milliseconds>(d).count();
    }

    /// The simulation is over: how its time went. The gate's numbers stopped when the run ended
    /// (a stop, an abort, stop_at_ms), so the shutdown is not in them.
    void note_timing(SimulationController& controller)
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

    void watch(SimulationController& controller)
    {
        WatchOptions watch;
        watch.poll = options_.poll;
        watch.interrupted = [this] { return interrupts() > 0; };
        watch.failed = [this, &controller] {
            // A required participant whose process failed fails the run at once; one that exited
            // with 0 may have stopped the simulation (the controller sees that a moment later).
            // Once the simulation stops, participants leave on purpose (a coordinated participant
            // ends only after the controller has stopped): their exit codes are judged after it.
            // (The time gate stands still in Running: a process that ends while the simulation is
            // paused fails the run as well.)
            const bool running = controller.runtime().state() == orch::ParticipantState::Running;
            // The events of what changed (here: the one look per poll the watch gives).
            note_joined(controller);
            note_time(controller, TimeNote::Poll);
            if (!running)
            {
                // Stopping: the ends of the processes are looked at after the simulation's end
                // (end_children()), so that they come after it in the output.
                return false;
            }
            std::vector<bool> known;
            for (const auto& run : outcome_.participants)
            {
                known.push_back(run.exit.has_value());
            }
            static_cast<void>(look_at_children(false));
            for (std::size_t i = 0; i < known.size(); ++i)
            {
                const ParticipantRun& run = outcome_.participants[i];
                if (!known[i] && run.required && failed_exit(run))
                {
                    outcome_.failed_participant = run.name;
                    return true;
                }
            }
            return false;
        };
        if (options_.timeout)
        {
            watch.deadline = std::chrono::steady_clock::now() + *options_.timeout;
        }
        watch.before_abort = [this](const WatchResult& why) {
            if (why.end == WatchEnd::TimedOut)
            {
                say("the simulation did not end within --timeout-ms (" + std::to_string(options_.timeout->count()) +
                    " ms): aborting it");
            }
            else
            {
                const std::string who = why.failure ? why.failure->participant : outcome_.failed_participant;
                note_failed(who, why.failure ? "left_simulation" : "process_ended");
                say("participant '" + who + "' failed" + (why.failure ? ": it " + why.failure->reason : std::string{}) +
                    "; aborting the simulation");
            }
        };
        const WatchResult watched = watch_simulation(controller, watch);
        note_timing(controller);
        note_time(controller, TimeNote::Last);
        // No time command from here on: the simulation is over.
        commands().detach();
        // An interrupt from here on comes while the launcher waits for the participants to end.
        interrupts_at_end_ = interrupts();
        switch (watched.end)
        {
        case WatchEnd::Ended:
            if (watched.state == orch::ParticipantState::Shutdown)
            {
                outcome_.simulation = config_.stop_at_ms && controller.wall_time_to_stop_at() ? SimulationEnd::ReachedStopAt
                                                                                               : SimulationEnd::Ended;
            }
            else
            {
                outcome_.simulation = SimulationEnd::Ended;
                outcome_.controller_state = watched.state ? orch::to_string(*watched.state) : std::string("unknown");
            }
            say("the simulation ended");
            break;
        case WatchEnd::TimedOut:
            outcome_.simulation = SimulationEnd::Aborted;
            outcome_.timed_out = true;
            say("the simulation was aborted: it did not end within --timeout-ms");
            break;
        case WatchEnd::Failed:
            outcome_.simulation = SimulationEnd::Aborted;
            if (watched.failure && outcome_.failed_participant.empty())
            {
                outcome_.failed_participant = watched.failure->participant;
                outcome_.failure_reason = watched.failure->reason;
            }
            say("the simulation was aborted: participant '" + outcome_.failed_participant + "' failed");
            break;
        case WatchEnd::Interrupted:
            outcome_.simulation = SimulationEnd::Interrupted;
            outcome_.interrupted_by = interrupt_name();
            say("interrupted (" + outcome_.interrupted_by + "): the simulation was stopped");
            break;
        }
    }

    [[nodiscard]] bool any_running(bool required_only)
    {
        static_cast<void>(look_at_children(false));
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            const ParticipantRun& run = outcome_.participants[i];
            if (children_[i] && !run.exit && run.ending != Ending::Killed && (!required_only || run.required))
            {
                return true;
            }
        }
        return false;
    }

    using Waited = detail::Waited;

    /// Waits up to `timeout` until `done()`; stops waiting once more than `interrupts_allowed`
    /// interrupts have come (detail::wait_for()).
    template <class Done>
    Waited wait_until(std::chrono::milliseconds timeout, int interrupts_allowed, Done done)
    {
        return detail::wait_for(timeout, std::chrono::milliseconds(5), done,
                                [this, interrupts_allowed] { return interrupts() > interrupts_allowed; });
    }

    /// Ends every participant: after a simulation, first gives the required ones stop_timeout_ms
    /// to end by themselves; then asks the rest to stop, waits stop_timeout_ms, and ends what is
    /// left with the processes it started.
    void end_children(bool ran)
    {
        const std::chrono::milliseconds stopTimeout(config_.stop_timeout_ms);
        // An interrupt while the launcher waits for the participants to end by themselves stops
        // the waiting (a first one too: the simulation has ended already); a second interrupt in
        // all skips the wait after the stop requests as well.
        bool waitedInFull = false;
        if (ran)
        {
            const Waited waited = wait_until(stopTimeout, interrupts_at_end_, [this] { return !any_running(true); });
            waitedInFull = waited == Waited::TimedOut;
            if (waited == Waited::Interrupted && outcome_.interrupted_by.empty())
            {
                outcome_.interrupted_by = interrupt_name();
                outcome_.interrupted_while_ending = true;
                say("interrupted (" + outcome_.interrupted_by + "): no more waiting for the participants to end");
            }
        }
        std::vector<std::size_t> asked;
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            ParticipantRun& run = outcome_.participants[i];
            if (!children_[i] || run.exit)
            {
                continue;
            }
            run.stopped_after_end = waitedInFull && run.required;
            const bool sent = children_[i]->request_stop();
            switch (detail::record_stop_request(run, sent, children_[i]->poll()))
            {
            case detail::StopRequest::Sent:
                say("asking " + run.name + " to stop (" + std::string(stop_request_name()) + ")");
                event([&run](EventStream& e) { e.participant(run.name, ParticipantState::StopRequested); });
                asked.push_back(i);
                break;
            case detail::StopRequest::HadEnded:
                say(run.name + " " + exit_text(*run.exit));
                event([&run](EventStream& e) { e.participant_ended(run.name, *run.exit); });
                break;
            case detail::StopRequest::CannotSend:
                say("cannot ask " + run.name + " to stop: ending it");
                children_[i]->kill_tree();
                run.ending = Ending::Killed;
                event([&run](EventStream& e) { e.participant(run.name, ParticipantState::Killed); });
                break;
            }
        }
        if (!asked.empty())
        {
            // The first process of each and what it started: a wrapper (sh -c) may end before the
            // program it started has cleaned up.
            static_cast<void>(wait_until(stopTimeout, 1, [this, &asked] {
                return std::all_of(asked.begin(), asked.end(), [this](std::size_t i) {
                    return children_[i]->poll().has_value() && !children_[i]->others_running();
                });
            }));
        }
        for (const std::size_t i : asked)
        {
            ParticipantRun& run = outcome_.participants[i];
            if (auto status = children_[i]->poll())
            {
                run.exit = status;
                run.ending = Ending::Stopped;
                event([&run](EventStream& e) { e.participant_ended(run.name, *run.exit); });
            }
            else
            {
                say("ending " + run.name + " (it did not stop)");
                children_[i]->kill_tree();
                run.ending = Ending::Killed;
                event([&run](EventStream& e) { e.participant(run.name, ParticipantState::Killed); });
            }
        }
        for (std::size_t i = 0; i < children_.size(); ++i)
        {
            if (!children_[i])
            {
                continue;
            }
            ParticipantRun& run = outcome_.participants[i];
            if (run.ending != Ending::Killed && children_[i]->others_running())
            {
                run.left_processes = true;
                say(run.name + " left processes running: ending them");
                children_[i]->kill_tree();
            }
        }
        for (auto& child : children_)
        {
            if (child)
            {
                static_cast<void>(child->finish_output(std::chrono::milliseconds(2000)));
            }
        }
    }

    const SimulationConfig& config_;
    const CheckReport& check_;
    std::filesystem::path base_;
    const RunOptions& options_;
    RunOutcome outcome_;
    int interrupts_at_end_ = 0; ///< interrupts() when the simulation had ended
    std::vector<std::unique_ptr<std::ofstream>> logs_;
    // run --events (main thread only): who has joined, the last virtual time written, the
    // findings written.
    std::vector<bool> joined_;
    std::int64_t last_time_ms_ = -1;
    std::chrono::steady_clock::time_point last_time_wall_{};
    bool last_paused_ = false;
    std::int64_t last_late_ms_ = 0;
    std::uint64_t last_late_steps_ = 0;
    std::optional<double> last_speed_{};
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::int64_t>> samples_; ///< (wall, sim_ms)
    std::vector<Finding> published_;
    std::unique_ptr<TimeCommands> own_commands_; ///< without RunOptions::time_commands
    std::mutex output_;
    // Last, so that it goes first: a Process ends its reader thread, which uses output_ and logs_,
    // also when run() leaves with an exception.
    std::vector<std::unique_ptr<process::Process>> children_;
};

} // namespace

RunReport run_file(const std::filesystem::path& path, const std::string& shown_as, const RunOptions& options)
{
    const auto event = [&options](auto emit) {
        if (options.events != nullptr)
        {
            emit(*options.events);
        }
    };
    RunReport report;
    report.strict = options.check.strict;
    event([](EventStream& e) { e.phase(Phase::Checking); });
    report.check = check_file(path, shown_as, options.check);
    event([&report](EventStream& e) { e.check(report.check); });
    if (!report.check.config || !report.check.passed())
    {
        event([](EventStream& e) { e.phase(Phase::Ended); });
        return report;
    }
    // The participants that will not start, before anything starts, in the file's order.
    event([&report](EventStream& e) {
        for (const auto& p : report.check.config->participants)
        {
            if (!p.enabled)
            {
                e.participant(p.name, ParticipantState::Disabled);
            }
        }
    });
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    Runner runner(*report.check.config, report.check, base, options);
    report.run = runner.run();
    report.findings = assess_run(*report.run);
    runner.publish_findings(true);
    event([](EventStream& e) { e.phase(Phase::Ended); });
    return report;
}

} // namespace vehicle_sil::launch
