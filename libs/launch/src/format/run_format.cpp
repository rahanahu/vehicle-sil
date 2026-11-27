// SPDX-License-Identifier: MIT
//
// The run's result for people (format_run_text()) and as JSON (format_run_json()), and the names
// of its values. Pure functions of the RunReport: no state, no thread, no lock.
#include "vehicle_sil/launch/run.hpp"

#include "check_format.hpp"
#include "run_format.hpp"
#include "vehicle_sil/runner/detail/run_words.hpp"

#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using config::detail::kNanosPerMilli;
using config::detail::millis_text;

/// The cell of the participants' table.
std::string ending_cell(const runner::ParticipantRun& p)
{
    const auto status = [&p] {
        if (!p.exit)
        {
            return std::string{};
        }
        return p.exit->signal ? "signal " + process::signal_name(*p.exit->signal)
                              : "exit code " + runner::detail::exit_code_text(p.exit->code);
    };
    switch (p.ending)
    {
    case runner::Ending::NotStarted:
        if (p.enabled && p.manual)
        {
            return p.manual_joined ? "started by you (manual)" : "not joined (manual)";
        }
        return p.enabled ? "not started" : "not started (disabled)";
    case runner::Ending::ExitedBeforeStart:
    case runner::Ending::Exited:
        return status();
    case runner::Ending::Stopped:
        return "stopped (" + std::string(runner::stop_request_name()) + "), " + status();
    case runner::Ending::Killed:
        return "killed";
    }
    return "-";
}

const char* ending_name(runner::Ending ending)
{
    switch (ending)
    {
    case runner::Ending::NotStarted: return "not_started";
    case runner::Ending::ExitedBeforeStart:
    case runner::Ending::Exited: return "exited";
    case runner::Ending::Stopped: return "stopped";
    case runner::Ending::Killed: return "killed";
    }
    return "not_started";
}

} // namespace

namespace {

const char* simulation_name(runner::SimulationEnd end)
{
    switch (end)
    {
    case runner::SimulationEnd::NotStarted: return "not_started";
    case runner::SimulationEnd::ReachedStopAt: return "reached_stop_at";
    case runner::SimulationEnd::Ended: return "ended";
    case runner::SimulationEnd::Aborted: return "aborted";
    case runner::SimulationEnd::Interrupted: return "interrupted";
    }
    return "not_started";
}

const char* result_name(RunResult result)
{
    switch (result)
    {
    case RunResult::Passed: return "passed";
    case RunResult::Stopped: return "stopped";
    case RunResult::Incomplete: return "incomplete";
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

std::string detail::timing_text(const config::RunTiming& timing)
{
    if (!timing.speed && timing.paced_steps == 0)
    {
        return {};
    }
    const auto actual = timing.actual_speed();
    std::string out = "time: speed " + config::speed_text(timing.speed) + ", ran at " + (actual ? config::speed_text(actual) : std::string("-")) +
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
    std::string out = detail::check_text_body(report.check, detail::scenario_of(report.check));
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
    const runner::RunOutcome& run = *report.run;
    std::string state;
    switch (run.end.simulation)
    {
    case runner::SimulationEnd::NotStarted:
        state = !run.setup.error.empty()      ? "the simulation could not be set up"
                : !run.end.interrupted_by.empty() ? run.end.stop_text() + " before the simulation started"
                                              : "the simulation did not start";
        break;
    case runner::SimulationEnd::ReachedStopAt:
        state = "the simulation ran to stop_at_ms (" + std::to_string(run.end.stop_at_ms.value_or(0)) + " ms) and ended";
        break;
    case runner::SimulationEnd::Ended:
        state = !run.end.controller_state.empty() ? "the simulation ended in state " + run.end.controller_state
                : !run.scenario.not_observed.empty()
                    ? "the simulation was stopped before its first step: the scenario was not begun"
                : run.end.stopped_by_scenario     ? "the simulation ran until the scenario was over (" +
                                                    std::to_string(run.timing.sim_ms) + " ms) and ended"
                                              : "the simulation ran until a participant ended it";
        break;
    case runner::SimulationEnd::Aborted:
        state = run.end.timed_out ? "the simulation was aborted: it did not end within --timeout-ms (" +
                                    std::to_string(run.end.timeout_ms.value_or(0)) + " ms)"
                : run.end.failed_participant.empty() ? "the simulation was aborted"
                                                 : "the simulation was aborted: participant '" + run.end.failed_participant + "' failed";
        break;
    case runner::SimulationEnd::Interrupted:
        state = run.end.stop_text() + ": the simulation was stopped";
        break;
    }
    out += "run: " + state + "\n";
    if (const std::string time = run.end.simulation != runner::SimulationEnd::NotStarted ? detail::timing_text(run.timing) : std::string{};
        !time.empty())
    {
        out += time + "\n";
    }
    out += "\n";
    if (run.scenario.result)
    {
        out += detail::scenario_text(*run.scenario.result) + "\n";
    }
    // --connect: the networks connected to a real bus, each through its bridge (the last rows below).
    for (const runner::RunOutside& o : run.outside)
    {
        out += "outside: " + o.network + " -> " + o.target + " (" + o.participant + ")";
        if (o.to_outside && o.from_outside)
        {
            out += ": " + std::to_string(*o.to_outside) + (*o.to_outside == 1 ? " frame out, " : " frames out, ") +
                   std::to_string(*o.from_outside) + " from devices";
        }
        out += "\n";
    }
    if (!run.outside.empty())
    {
        out += "\n";
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
    if (run.traffic)
    {
        out += detail::traffic_text(*run.traffic) + "\n";
    }
    out += detail::findings_text(report.check.file, report.findings);
    switch (result)
    {
    case RunResult::Passed: out += "passed: " + counts + "\n"; break;
    case RunResult::Stopped: out += "stopped: " + counts + "\n"; break;
    case RunResult::Incomplete: out += "incomplete: " + counts + "\n"; break;
    case RunResult::Interrupted: out += "interrupted: " + counts + "\n"; break;
    default:
        out += "failed: " + counts + (report.errors() == 0 && run.setup.error.empty() ? " (--strict)" : "") + "\n";
        break;
    }
    return out;
}

namespace {

/// A virtual time in milliseconds for people ("300", "2.5").
std::string sim_millis_text(SimTime t)
{
    return config::detail::millis_text(to_ns(t));
}

} // namespace

std::string detail::traffic_text(const runner::RunTraffic& traffic)
{
    if (traffic.per_network.empty())
    {
        return "traffic: no network in use\n";
    }
    const auto why = [](runner::TrafficNotCounted reason) -> std::string {
        switch (reason)
        {
        case runner::TrafficNotCounted::None: return {};
        case runner::TrafficNotCounted::NoControlLink: return "not counted: no control link";
        case runner::TrafficNotCounted::Restarted: return "not counted: restarted";
        case runner::TrafficNotCounted::RealTime: return "not counted: real time";
        case runner::TrafficNotCounted::NoCounters: return "not counted: no J1939 counters";
        case runner::TrafficNotCounted::Mapped: return "not counted: counters mapped by variables";
        case runner::TrafficNotCounted::NoSample: return "not counted: no sample before the end";
        }
        return {};
    };
    const auto unplaced = [](const runner::TrafficUnplaced& p) -> std::string {
        switch (p.reason)
        {
        case runner::TrafficUnplacedReason::NoControlLink: return "unplaced: no control link";
        case runner::TrafficUnplacedReason::NoCounters: return "unplaced: no J1939 counters";
        }
        return {};
    };
    const auto participants = [](std::size_t n) { return std::to_string(n) + (n == 1 ? " participant" : " participants"); };
    std::string line;
    std::vector<std::vector<std::string>> rows{{"network", "participant", "sent", "received"}};
    for (const runner::TrafficNetwork& network : traffic.per_network)
    {
        const std::size_t counted = network.counted();
        const std::size_t others = network.participants.size() - counted;
        const std::string may_be =
            network.unplaced.empty() ? std::string{} : ", " + std::to_string(network.unplaced.size()) + " unplaced";
        const std::string in_all = " (" + std::to_string(counted) + " counted" +
                                   (others > 0 ? ", " + std::to_string(others) + " not counted" : std::string{}) + may_be + ")";
        // "nothing sent" only when everyone who may be on it is counted and nobody received.
        const bool silent = network.senders() == 0 && others == 0 && network.unplaced.empty() && network.receivers() == 0;
        // Who sent and who received, not a share: a participant that only listens is as it should be.
        line += (line.empty() ? "" : "; ") + network.name + ": " +
                (network.participants.empty() ? "not counted (no participant known to be on it" + may_be + ")"
                 : counted == 0               ? "not counted (" + participants(others) + ")"
                 : silent                     ? "nothing sent" + in_all
                                              : participants(network.senders()) + " sent, " +
                                                    std::to_string(network.receivers()) + " received" + in_all);
        for (const runner::TrafficParticipant& p : network.participants)
        {
            std::vector<std::string> row{network.name, p.name, p.frames_sent ? std::to_string(*p.frames_sent) : "-",
                                         p.frames_received ? std::to_string(*p.frames_received) : "-"};
            if (!p.counted())
            {
                row.push_back(why(p.not_counted));
            }
            rows.push_back(std::move(row));
        }
        if (network.participants.empty())
        {
            rows.push_back({network.name, "-", "-", "-", "no participant known to be on it"});
        }
        // One that its counters place on another network gets a row here; one that is known on
        // no network gets one row after the networks.
        for (const runner::TrafficUnplaced& p : network.unplaced)
        {
            if (std::find(traffic.unplaced.begin(), traffic.unplaced.end(), p) == traffic.unplaced.end())
            {
                rows.push_back({network.name, p.name, "-", "-", unplaced(p)});
            }
        }
    }
    for (const runner::TrafficUnplaced& p : traffic.unplaced)
    {
        rows.push_back({"-", p.name, "-", "-", unplaced(p)});
    }
    // The numbers to the right.
    for (const std::size_t column : {std::size_t{2}, std::size_t{3}})
    {
        std::size_t width = 0;
        for (const auto& row : rows)
        {
            width = std::max(width, row[column].size());
        }
        for (std::size_t i = 1; i < rows.size(); ++i)
        {
            rows[i][column].insert(0, width - rows[i][column].size(), ' ');
        }
    }
    return "traffic: " + line + " (steps before " + std::to_string(traffic.before_ms) + " ms)\n\n" + detail::table_text(rows);
}

std::string detail::scenario_text(const scenario::ScenarioRun& scenario)
{
    const auto passed = static_cast<std::size_t>(std::count_if(
        scenario.steps.begin(), scenario.steps.end(), [](const scenario::ScenarioStepRun& step) { return step.status == scenario::StepStatus::Passed; }));
    std::string state;
    switch (scenario.status)
    {
    case scenario::ScenarioStatus::NotRun: state = "not run"; break;
    case scenario::ScenarioStatus::Passed: state = "passed"; break;
    case scenario::ScenarioStatus::Failed: state = "FAILED"; break;
    case scenario::ScenarioStatus::Incomplete: state = "incomplete"; break;
    }
    std::string out = "scenario " + scenario.file + (scenario.name.empty() ? "" : " (" + scenario.name + ")") + ": " + state +
                      ", " + std::to_string(passed) + " of " + std::to_string(scenario.steps.size()) + " steps passed" +
                      (scenario.skipped() > 0 ? ", " + std::to_string(scenario.skipped()) + " skipped" : std::string{}) +
                      (scenario.unchecked_expects() > 0
                           ? ", " + std::to_string(scenario.unchecked_expects()) +
                                 (scenario.unchecked_expects() == 1 ? " expect not checked" : " expects not checked")
                           : std::string{}) +
                      "\n\n";
    const bool hasFile = std::any_of(scenario.steps.begin(), scenario.steps.end(), [&](const scenario::ScenarioStepRun& s) {
        return !s.file.empty() && s.file != scenario.file;
    });
    const bool hasName = std::any_of(scenario.steps.begin(), scenario.steps.end(),
                                     [](const scenario::ScenarioStepRun& s) { return !s.name.empty(); });
    std::vector<std::string> header{hasFile ? "where" : "line"};
    if (hasName)
    {
        header.push_back("name");
    }
    header.push_back("step");
    header.push_back("result");
    header.push_back("at");
    std::vector<std::vector<std::string>> rows{std::move(header)};
    for (const scenario::ScenarioStepRun& step : scenario.steps)
    {
        std::vector<std::string> row;
        row.push_back(hasFile ? ((step.file.empty() ? scenario.file : step.file) + ":" + std::to_string(step.line))
                              : std::to_string(step.line));
        if (hasName)
        {
            row.push_back(step.name.empty() ? std::string("-") : step.name);
        }
        row.push_back(step.text);
        row.push_back(scenario::step_result_text(step.status));
        row.push_back(step.at ? sim_millis_text(*step.at) + " ms" : "-");
        rows.push_back(std::move(row));
    }
    out += detail::table_text(rows);
    // Why, for the steps that were skipped and the expects that were not checked: the table has no
    // room for it.
    const auto add_call_stack = [&](const std::vector<scenario::SourceFrame>& stack) {
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        {
            out += "    called from: " +
                   (it->file.empty() ? "line " + std::to_string(it->line) : it->file + ":" + std::to_string(it->line));
            std::string meta;
            if (!it->name.empty())
            {
                meta += "step '" + it->name + "'";
            }
            for (const scenario::SourceParam& p : it->params)
            {
                if (!meta.empty())
                {
                    meta += ", ";
                }
                meta += p.name + " = " + (p.quoted ? "\"" + p.text + "\"" : p.text);
            }
            if (!meta.empty())
            {
                out += " (" + meta + ")";
            }
            out += "\n";
        }
    };
    for (const scenario::ScenarioStepRun& step : scenario.steps)
    {
        const std::string where = step.file.empty() ? "line " + std::to_string(step.line) : step.file + ":" + std::to_string(step.line);
        if (step.status == scenario::StepStatus::Skipped)
        {
            out += where + " skipped: " + step.message + "\n";
            add_call_stack(step.call_stack);
        }
        else if (!step.expect_skipped.empty())
        {
            out += where + " expect not checked: " + step.expect_skipped + "\n";
            add_call_stack(step.call_stack);
        }
    }
    return out;
}

std::string format_run_json(const RunReport& report)
{
    vehicle_sil::json::JsonWriter w;
    detail::write_run_json(w, {}, report);
    std::string out = w.take();
    out += '\n';
    return out;
}

std::string format_result_event(const RunReport& report, bool launcher_ends, const ExecutionStatus* status)
{
    vehicle_sil::json::JsonWriter w(true);
    w.begin_object();
    w.boolean("launcher_ends", launcher_ends);
    if (status != nullptr)
    {
        w.begin_object("status");
        detail::write_status_members(w, *status);
        w.end_object();
    }
    detail::write_run_json(w, "result", report);
    w.end_object();
    return w.take();
}

void detail::write_run_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const RunReport& report)
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
    if (report.run)
    {
        w.string("end", runner::end_reason_name(runner::end_reason(*report.run)));
    }
    else
    {
        w.null("end");
    }
    w.boolean("ok", report.ok());
    w.string("message", report.message());
    w.boolean("strict", report.strict);
    w.integer("errors", static_cast<std::int64_t>(report.errors()));
    w.integer("warnings", static_cast<std::int64_t>(report.warnings()));
    w.integer("notes", static_cast<std::int64_t>(report.notes()));
    if (report.run)
    {
        w.string("simulation", simulation_name(report.run->end.simulation));
    }
    else
    {
        w.null("simulation");
    }
    if (report.run && !report.run->end.interrupted_by.empty())
    {
        w.string("interrupted_by", report.run->end.interrupted_by);
    }
    else
    {
        w.null("interrupted_by");
    }
    if (report.run && !report.run->setup.registry_uri.empty())
    {
        w.string("registry_uri", report.run->setup.registry_uri);
    }
    else
    {
        w.null("registry_uri");
    }
    if (report.run)
    {
        const config::RunTiming& t = report.run->timing;
        if (report.run->end.stop_at_ms) w.integer("stop_at_ms", *report.run->end.stop_at_ms);
        else w.null("stop_at_ms");
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
            return ns ? std::optional<double>(static_cast<double>(*ns) / static_cast<double>(config::detail::kNanosPerMilli)) : std::nullopt;
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
            if (p.for_network.empty())
            {
                w.null("for_network");
            }
            else
            {
                w.string("for_network", p.for_network);
            }
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
            w.string("state", runner::control_link_name(p.control));
            if (p.control == runner::ControlLink::Connected)
            {
                w.integer("variables", static_cast<std::int64_t>(p.control_variables));
            }
            else
            {
                w.null("variables");
            }
            if (p.control == runner::ControlLink::VersionMismatch)
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
    w.begin_array("outside");
    if (report.run)
    {
        for (const runner::RunOutside& o : report.run->outside)
        {
            w.begin_object();
            w.string("network", o.network);
            w.string("target", o.target);
            w.string("participant", o.participant);
            const auto count = [&w](const char* name, const std::optional<std::uint64_t>& value) {
                if (value)
                {
                    w.integer(name, static_cast<std::int64_t>(*value));
                }
                else
                {
                    w.null(name);
                }
            };
            count("to_outside", o.to_outside);
            count("from_outside", o.from_outside);
            // Every count of the bridge's last statistics line, with its own keys.
            w.begin_object("counts");
            for (const auto& [name, value] : o.link.counts)
            {
                w.integer(name, static_cast<std::int64_t>(value));
            }
            w.end_object();
            w.end_object();
        }
    }
    w.end_array();
    if (report.run && report.run->traffic)
    {
        const runner::RunTraffic& traffic = *report.run->traffic;
        w.begin_object("traffic");
        w.integer("before_ms", traffic.before_ms);
        const auto unplaced = [&w](const std::vector<runner::TrafficUnplaced>& all) {
            w.begin_array("unplaced");
            for (const runner::TrafficUnplaced& p : all)
            {
                w.begin_object();
                w.string("participant", p.name);
                w.string("reason", runner::traffic_unplaced_reason_name(p.reason));
                w.end_object();
            }
            w.end_array();
        };
        unplaced(traffic.unplaced);
        w.begin_array("networks");
        for (const runner::TrafficNetwork& network : traffic.per_network)
        {
            w.begin_object();
            w.string("name", network.name);
            w.string("kind", network.kind);
            w.integer("counted", static_cast<std::int64_t>(network.counted()));
            w.integer("senders", static_cast<std::int64_t>(network.senders()));
            w.integer("receivers", static_cast<std::int64_t>(network.receivers()));
            if (const std::optional<bool> delivered = network.delivered())
            {
                w.boolean("delivered", *delivered);
            }
            else
            {
                w.null("delivered");
            }
            unplaced(network.unplaced);
            w.begin_array("participants");
            for (const runner::TrafficParticipant& p : network.participants)
            {
                const auto number = [&w](std::string_view member, const std::optional<std::int64_t>& value) {
                    if (value)
                    {
                        w.integer(member, *value);
                    }
                    else
                    {
                        w.null(member);
                    }
                };
                w.begin_object();
                w.string("name", p.name);
                w.boolean("counted", p.counted());
                if (p.counted())
                {
                    w.null("not_counted");
                }
                else
                {
                    w.string("not_counted", runner::traffic_not_counted_name(p.not_counted));
                }
                if (p.virtual_time)
                {
                    w.boolean("virtual_time", *p.virtual_time);
                }
                else
                {
                    w.null("virtual_time");
                }
                w.real("at_ms", p.at ? std::optional<double>(static_cast<double>(to_ns(*p.at)) / static_cast<double>(config::detail::kNanosPerMilli))
                                     : std::nullopt);
                number("frames_sent", p.frames_sent);
                number("frames_received", p.frames_received);
                w.end_object();
            }
            w.end_array();
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    else
    {
        w.null("traffic");
    }
    detail::write_findings_json(w, report.check.file, report.findings);
    detail::write_check_json(w, "check", report.check);
    if (report.run && report.run->scenario.result)
    {
        // Only with --scenario: a result without one is as it was before scenarios.
        detail::write_scenario_json(w, "scenario", *report.run->scenario.result, report.run->scenario.timeline);
    }
    w.end_object();
}

void detail::write_scenario_step_members(vehicle_sil::json::JsonWriter& w, const scenario::ScenarioStepRun& step)
{
    const auto time = [&w](std::string_view key, const std::optional<SimTime>& t) {
        w.real(key, t ? std::optional<double>(static_cast<double>(to_ns(*t)) / static_cast<double>(config::detail::kNanosPerMilli)) : std::nullopt);
    };
    w.integer("index", static_cast<std::int64_t>(step.index));
    if (!step.file.empty())
    {
        w.string("file", step.file);
    }
    w.integer("line", static_cast<std::int64_t>(step.line));
    w.string("kind", scenario::step_kind_name(step.kind));
    if (!step.name.empty())
    {
        w.string("name", step.name);
    }
    w.string("text", step.text);
    w.string("status", scenario::step_status_name(step.status));
    time("start_ms", step.start);
    time("at_ms", step.at);
    time("end_ms", step.end);
    if (step.status == scenario::StepStatus::Failed || step.status == scenario::StepStatus::Skipped)
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
    if (!step.call_stack.empty())
    {
        w.begin_array("call_stack");
        for (const scenario::SourceFrame& frame : step.call_stack)
        {
            w.begin_object();
            w.string("file", frame.file);
            w.integer("line", static_cast<std::int64_t>(frame.line));
            if (frame.name.empty())
            {
                w.null("name");
            }
            else
            {
                w.string("name", frame.name);
            }
            w.begin_object("params");
            for (const scenario::SourceParam& param : frame.params)
            {
                detail::write_param_scalar(w, param.name, param.text, param.quoted);
            }
            w.end_object();
            w.end_object();
        }
        w.end_array();
    }
}

void detail::write_scenario_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const scenario::ScenarioRun& scenario,
                                 const std::string& timeline)
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
    w.string("status", scenario::scenario_status_name(scenario.status));
    w.real("over_at_ms", scenario.over_at ? std::optional<double>(static_cast<double>(to_ns(*scenario.over_at)) /
                                                                  static_cast<double>(config::detail::kNanosPerMilli))
                                          : std::nullopt);
    if (scenario.end_ms)
    {
        w.integer("end_ms", *scenario.end_ms);
    }
    else
    {
        w.null("end_ms");
    }
    if (scenario.skipped() > 0)
    {
        // Only then: a result without skipped steps is as it was before them.
        w.integer("skipped_steps", static_cast<std::int64_t>(scenario.skipped()));
    }
    if (scenario.unchecked_expects() > 0)
    {
        // Only then, as skipped_steps.
        w.integer("unchecked_expects", static_cast<std::int64_t>(scenario.unchecked_expects()));
    }
    w.begin_array("steps");
    for (const scenario::ScenarioStepRun& step : scenario.steps)
    {
        w.begin_object();
        write_scenario_step_members(w, step);
        w.end_object();
    }
    w.end_array();
    // Empty for a scenario without a dither (ScenarioRun::has_dither).
    write_scenario_lines_json(w, scenario.inputs, scenario.lines);
    if (timeline.empty())
    {
        w.null("timeline"); // without --timeline
    }
    else
    {
        w.string("timeline", timeline);
    }
    w.end_object();
}

std::string format_internal_error_json(bool run, const std::string& message)
{
    Checked check;
    check.findings.push_back(config::Finding{config::Severity::Error, "internal", message, 0, "", ""});
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
