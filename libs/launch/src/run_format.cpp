// SPDX-License-Identifier: MIT
//
// The run's result for people (format_run_text()) and as JSON (format_run_json()), and the names
// of its values. Pure functions of the RunReport: no state, no thread, no lock.
#include "vehicle_sil/launch/run.hpp"

#include "check_format.hpp"
#include "run_words.hpp"

#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using detail::hex_code;
using detail::kNanosPerMilli;
using detail::millis_text;

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
        if (p.enabled && p.manual)
        {
            return p.manual_joined ? "started by you (manual)" : "not joined (manual)";
        }
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

} // namespace

const char* control_link_name(ControlLink link)
{
    switch (link)
    {
    case ControlLink::None:
        return "none";
    case ControlLink::Connected:
        return "connected";
    case ControlLink::NoHello:
        return "no_hello";
    case ControlLink::VersionMismatch:
        return "version_mismatch";
    case ControlLink::NotOffered:
        return "not_offered";
    }
    return "none";
}

namespace {

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

} // namespace

std::string_view stop_request_name()
{
#ifdef _WIN32
    return "Ctrl+Break";
#else
    return "SIGTERM";
#endif
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
               " (vehicle_sil_launch check " +
               (report.check.config && !report.check.config->variant.empty() ? "--variant " + report.check.config->variant + " " : std::string{}) +
               report.check.file + " shows the same)\n\nfailed: " + counts + "\n";
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
        state = !run.controller_state.empty() ? "the simulation ended in state " + run.controller_state
                : run.stopped_by_scenario     ? "the simulation ran until the scenario was over (" +
                                                    std::to_string(run.timing.sim_ms) + " ms) and ended"
                                              : "the simulation ran until a participant ended it";
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
    if (run.scenario)
    {
        out += detail::scenario_text(*run.scenario) + "\n";
    }
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

namespace {

/// A virtual time in milliseconds for people ("300", "2.5").
std::string sim_millis_text(SimTime t)
{
    return millis_text(to_ns(t));
}

const char* step_result_text(StepStatus status)
{
    switch (status)
    {
    case StepStatus::NotRun: return "not run";
    case StepStatus::Running: return "running";
    case StepStatus::Passed: return "ok";
    case StepStatus::Failed: return "FAILED";
    case StepStatus::Unfinished: return "unfinished";
    case StepStatus::Skipped: return "skipped";
    }
    return "not run";
}

} // namespace

std::string detail::scenario_step_text(const ScenarioStepRun& step)
{
    return "line " + std::to_string(step.line) + ": " + step.text + ": " + step_result_text(step.status) +
           (step.at ? " at " + sim_millis_text(*step.at) + " ms" : std::string{}) +
           (step.status == StepStatus::Failed || step.status == StepStatus::Skipped ? " (" + step.message + ")" : std::string{});
}

std::string detail::scenario_text(const ScenarioRun& scenario)
{
    const auto passed = static_cast<std::size_t>(std::count_if(
        scenario.steps.begin(), scenario.steps.end(), [](const ScenarioStepRun& step) { return step.status == StepStatus::Passed; }));
    std::string state;
    switch (scenario.status)
    {
    case ScenarioStatus::NotRun: state = "not run"; break;
    case ScenarioStatus::Passed: state = "passed"; break;
    case ScenarioStatus::Failed: state = "FAILED"; break;
    case ScenarioStatus::Incomplete: state = "incomplete"; break;
    }
    std::string out = "scenario " + scenario.file + (scenario.name.empty() ? "" : " (" + scenario.name + ")") + ": " + state +
                      ", " + std::to_string(passed) + " of " + std::to_string(scenario.steps.size()) + " steps passed" +
                      (scenario.skipped() > 0 ? ", " + std::to_string(scenario.skipped()) + " skipped" : std::string{}) + "\n\n";
    std::vector<std::vector<std::string>> rows{{"line", "step", "result", "at"}};
    for (const ScenarioStepRun& step : scenario.steps)
    {
        rows.push_back({std::to_string(step.line), step.text, step_result_text(step.status),
                        step.at ? sim_millis_text(*step.at) + " ms" : "-"});
    }
    out += detail::table_text(rows);
    // Why, for the steps that were skipped and the expects that were not checked: the table has no
    // room for it.
    for (const ScenarioStepRun& step : scenario.steps)
    {
        if (step.status == StepStatus::Skipped)
        {
            out += "line " + std::to_string(step.line) + " skipped: " + step.message + "\n";
        }
        else if (!step.expect_skipped.empty())
        {
            out += "line " + std::to_string(step.line) + " expect not checked: " + step.expect_skipped + "\n";
        }
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
            w.boolean("manual", p.manual);
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
            w.begin_object("control_link");
            w.string("state", control_link_name(p.control));
            if (p.control == ControlLink::Connected)
            {
                w.integer("variables", static_cast<std::int64_t>(p.control_variables));
            }
            else
            {
                w.null("variables");
            }
            if (p.control == ControlLink::VersionMismatch)
            {
                w.integer("version", p.control_version);
            }
            else
            {
                w.null("version");
            }
            w.integer("late_messages", static_cast<std::int64_t>(p.control_late_messages));
            w.end_object();
            w.end_object();
        }
    }
    w.end_array();
    detail::write_findings_json(w, report.check.file, report.findings);
    detail::write_check_json(w, "check", report.check);
    if (report.run && report.run->scenario)
    {
        // Only with --scenario: a result without one is as it was before scenarios.
        detail::write_scenario_json(w, "scenario", *report.run->scenario);
    }
    w.end_object();
}

void detail::write_scenario_step_members(vehicle_sil::detail::JsonWriter& w, const ScenarioStepRun& step)
{
    const auto time = [&w](std::string_view key, const std::optional<SimTime>& t) {
        w.real(key, t ? std::optional<double>(static_cast<double>(to_ns(*t)) / static_cast<double>(kNanosPerMilli)) : std::nullopt);
    };
    w.integer("index", static_cast<std::int64_t>(step.index));
    w.integer("line", static_cast<std::int64_t>(step.line));
    w.string("kind", step_kind_name(step.kind));
    w.string("text", step.text);
    w.string("status", step_status_name(step.status));
    time("start_ms", step.start);
    time("at_ms", step.at);
    time("end_ms", step.end);
    if (step.status == StepStatus::Failed || step.status == StepStatus::Skipped)
    {
        w.string("code", step.code);
        w.string("message", step.message);
    }
    else
    {
        w.null("code");
        w.null("message");
    }
    if (!step.expect_skipped.empty())
    {
        // Only when there is one: the steps of a scenario without it are as they were.
        w.string("expect_skipped", step.expect_skipped);
    }
}

void detail::write_scenario_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const ScenarioRun& scenario)
{
    w.begin_object(key);
    w.string("file", scenario.file);
    if (scenario.name.empty())
    {
        w.null("name");
    }
    else
    {
        w.string("name", scenario.name);
    }
    w.string("status", scenario_status_name(scenario.status));
    w.real("over_at_ms", scenario.over_at ? std::optional<double>(static_cast<double>(to_ns(*scenario.over_at)) /
                                                                  static_cast<double>(kNanosPerMilli))
                                          : std::nullopt);
    if (scenario.skipped() > 0)
    {
        // Only then: a result without skipped steps is as it was before them.
        w.integer("skipped_steps", static_cast<std::int64_t>(scenario.skipped()));
    }
    w.begin_array("steps");
    for (const ScenarioStepRun& step : scenario.steps)
    {
        w.begin_object();
        write_scenario_step_members(w, step);
        w.end_object();
    }
    w.end_array();
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

} // namespace vehicle_sil::launch
