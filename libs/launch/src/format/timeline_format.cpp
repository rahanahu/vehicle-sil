// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/timeline_format.hpp"

#include "check_format.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/ecu/describe_json.hpp"
#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/scenario/timeline.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace vehicle_sil::launch {

namespace {

using vehicle_sil::json::JsonWriter;

constexpr double kNanosPerMilli = 1'000'000.0;

void write_ms(JsonWriter& w, std::string_view key, const std::optional<SimTime>& t)
{
    w.real(key, t ? std::optional<double>(static_cast<double>(to_ns(*t)) / kNanosPerMilli) : std::nullopt);
}

void write_value(JsonWriter& w, std::string_view key, const std::optional<control::Value>& value)
{
    if (!value)
    {
        w.null(key);
    }
    else if (const auto* b = std::get_if<bool>(&*value))
    {
        w.boolean(key, *b);
    }
    else if (const auto* i = std::get_if<std::int64_t>(&*value))
    {
        w.integer(key, *i);
    }
    else
    {
        w.real(key, std::get<double>(*value));
    }
}

void write_text_or_null(JsonWriter& w, std::string_view key, const std::string& text)
{
    if (text.empty())
    {
        w.null(key);
    }
    else
    {
        w.string(key, text);
    }
}

const control::VariableInfo* described(const scenario::ScenarioCatalog* catalog, const scenario::Scenario& scenario, const scenario::ScenarioVariable& variable)
{
    if (catalog == nullptr)
    {
        return nullptr;
    }
    const std::string& participant = scenario.participants[variable.participant].name;
    for (const scenario::CatalogParticipant& p : *catalog)
    {
        if (p.name == participant)
        {
            const auto it = std::find_if(p.variables.begin(), p.variables.end(),
                                         [&variable](const control::VariableInfo& info) { return info.name == variable.name; });
            return it == p.variables.end() ? nullptr : &*it;
        }
    }
    return nullptr;
}

void write_variables(JsonWriter& w, const TimelineInput& input)
{
    const scenario::Scenario& scenario = *input.scenario;
    // Without a record (the scenario never began): from the steps, as the record does.
    const scenario::TimelineRecord fromSteps(scenario);
    const scenario::TimelineRecord& uses = input.record != nullptr ? *input.record : fromSteps;
    w.begin_array("variables");
    for (std::size_t i = 0; i < scenario.variables.size(); ++i)
    {
        const scenario::ScenarioVariable& v = scenario.variables[i];
        const control::VariableInfo* info = described(input.catalog, scenario, v);
        w.begin_object();
        w.string("name", v.text);
        w.string("participant", scenario.participants[v.participant].name);
        w.string("type", vehicle_sil::value_type_name(v.type));
        if (info != nullptr)
        {
            w.string("causality", vehicle_sil::causality_name(info->causality));
            w.string("unit", info->unit);
            w.real("min", info->min);
            w.real("max", info->max);
        }
        else
        {
            w.null("causality");
            w.string("unit", "");
            w.null("min");
            w.null("max");
        }
        w.begin_array("items");
        for (const control::EnumItem& item : v.items)
        {
            w.begin_object();
            w.string("name", item.name);
            w.integer("value", item.value);
            w.end_object();
        }
        w.end_array();
        const SimDuration step = scenario.participants[v.participant].step;
        w.real("ecu_step_ms", step > SimDuration::zero() ? std::optional<double>(static_cast<double>(step.count()) / kNanosPerMilli)
                                                         : std::nullopt);
        w.boolean("written", uses.is_written(i));
        w.boolean("watched", uses.is_watched(i));
        w.end_object();
    }
    w.end_array();
}

void write_watch(JsonWriter& w, const scenario::Scenario& scenario, const scenario::ScenarioStep& step, const scenario::ScenarioStepRun& run,
                 const std::optional<scenario::TimelineRecord::Watch>& watch)
{
    if (step.conditions.empty())
    {
        w.null("watch");
        return;
    }
    scenario::WatchMode mode = scenario::WatchMode::At;
    if (step.kind == scenario::StepKind::Ramp || step.kind == scenario::StepKind::Wave)
    {
        mode = scenario::WatchMode::During;
    }
    else if (step.within_ms)
    {
        mode = scenario::WatchMode::Once;
    }
    else if (step.hold_for_ms)
    {
        mode = scenario::WatchMode::Always;
    }
    w.begin_object("watch");
    w.string("mode", scenario::watch_mode_name(mode));
    // The times of a step that has its time; a step the scenario did not get to has none.
    const bool timed = watch && run.start;
    write_ms(w, "from_ms", timed ? std::optional<SimTime>(watch->from) : std::nullopt);
    write_ms(w, "to_ms", timed ? watch->until : std::nullopt);
    w.begin_array("conditions");
    for (const scenario::ScenarioCondition& condition : step.conditions)
    {
        w.begin_object();
        w.string("variable", scenario.variables[condition.variable].text);
        w.begin_array("compare");
        for (const scenario::ScenarioComparison& c : condition.all)
        {
            w.begin_object();
            w.string("op", scenario::compare_name(c.op));
            write_value(w, "value", c.value);
            if (c.op == scenario::Compare::Near)
            {
                w.real("tolerance", c.tolerance);
            }
            else
            {
                w.null("tolerance");
            }
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

void write_failed(JsonWriter& w, const scenario::Scenario& scenario, const scenario::ScenarioStepRun& run, const std::optional<scenario::TimelineFailure>& failure,
                  const scenario::TimelineRecord* record)
{
    if (run.status != scenario::StepStatus::Failed)
    {
        w.null("failed");
        return;
    }
    w.begin_object("failed");
    write_ms(w, "at_ms", run.at);
    w.string("code", run.code);
    w.begin_array("conditions");
    if (failure)
    {
        for (const scenario::TimelineFailedCondition& c : failure->conditions)
        {
            w.begin_object();
            w.integer("condition", static_cast<std::int64_t>(c.condition));
            write_value(w, "value", c.value);
            write_ms(w, "sample_ms", c.sample);
            w.end_object();
        }
    }
    w.end_array();
    // Every write of the request that failed, in the order sent, and which of them is the
    // reason (an index, as "condition" and "point" are; null if that is not known).
    w.begin_array("writes");
    if (failure)
    {
        for (const scenario::TimelineFailedWrite& write : failure->writes)
        {
            w.begin_object();
            w.string("variable", scenario.variables[write.variable].text);
            write_ms(w, "at_ms", write.at);
            write_value(w, "value", write.value);
            // Which point of the variable's written series it is; null if it has none (nothing
            // was sent).
            const auto point = record != nullptr && write.sent ? record->point_of(write.variable, *write.sent) : std::nullopt;
            if (point)
            {
                w.integer("point", static_cast<std::int64_t>(*point));
            }
            else
            {
                w.null("point");
            }
            if (write.applied)
            {
                w.boolean("applied", *write.applied);
            }
            else
            {
                w.null("applied");
            }
            w.end_object();
        }
    }
    w.end_array();
    if (failure && failure->write)
    {
        w.integer("write", static_cast<std::int64_t>(*failure->write));
    }
    else
    {
        w.null("write");
    }
    w.end_object();
}

void write_series(JsonWriter& w, const TimelineInput& input)
{
    w.begin_array("series");
    if (input.record != nullptr)
    {
        for (const scenario::TimelineRecord::Series& series : input.record->series())
        {
            w.begin_object();
            w.string("variable", input.scenario->variables[series.variable].text);
            w.string("source", series.sampled ? "sampled" : "written");
            w.string("encoding", series.sampled ? "changes" : "all");
            // Every key in every series: what does not apply to this one is null.
            w.begin_array_on_one_line("columns");
            w.string({}, "at_ms");
            w.string({}, "value");
            if (series.dithered)
            {
                // A dither was on the input: every point with what it was made of.
                w.string({}, "base");
            }
            w.end_array();
            if (series.sampled)
            {
                w.integer("samples", static_cast<std::int64_t>(series.sample_count));
                write_ms(w, "first_ms", series.first_sample);
                write_ms(w, "last_ms", series.last_sample);
                w.null("not_applied_from");
            }
            else
            {
                w.null("samples");
                w.null("first_ms");
                w.null("last_ms");
                // Sent and not known to be applied: the first write of the failed request that
                // was not applied (or has no result) and what was sent after it, and what was
                // written for a time after the step at which the scenario was over (no ECU got
                // to that time).
                const auto it = std::find_if(series.written.begin(), series.written.end(), [&](const scenario::TimelineRecord::Written& p) {
                    return p.refused || (input.run->over_at && p.at > *input.run->over_at);
                });
                if (it != series.written.end())
                {
                    w.integer("not_applied_from", it - series.written.begin());
                }
                else
                {
                    w.null("not_applied_from");
                }
            }
            w.begin_array("points");
            for (const scenario::TimelineRecord::Written& point : series.written)
            {
                w.begin_array_on_one_line({});
                write_ms(w, {}, point.at);
                write_value(w, {}, point.value);
                if (series.dithered)
                {
                    w.real({}, point.base);
                }
                w.end_array();
            }
            for (const scenario::TimelineRecord::Sampled& point : series.samples)
            {
                if (!point.repeats)
                {
                    w.begin_array_on_one_line({});
                    write_ms(w, {}, point.first);
                    write_value(w, {}, point.value);
                    w.end_array();
                }
            }
            w.end_array();
            w.end_object();
        }
    }
    w.end_array();
}

/// A dither as a line: what "inputs" declares (no step) or a dither step put, from the first
/// write that carried it. It has no end in the text and no base.
void write_dither_line(JsonWriter& w, const scenario::Scenario& scenario, const scenario::TimelineRecord::Line& line)
{
    const bool declared = line.step == scenario::TimelineRecord::kNoStep;
    const scenario::ScenarioOverlay& overlay = declared ? scenario.inputs[line.part] : scenario.steps[line.step].overlays[line.part];
    w.begin_object();
    w.string("variable", scenario.variables[line.variable].text);
    w.string("kind", "dither");
    if (!overlay.file.empty())
    {
        w.string("file", overlay.file);
    }
    w.integer("line", static_cast<std::int64_t>(overlay.line));
    if (declared)
    {
        w.null("step");
    }
    else
    {
        w.integer("step", static_cast<std::int64_t>(line.step + 1));
    }
    write_ms(w, "from_ms", line.from);
    w.null("to_ms");
    w.null("base");
    w.null("base_source");
    if (overlay.shape != nullptr)
    {
        w.begin_object("shape");
        w.string("type", overlay.described.type);
        for (const auto& [key, value] : overlay.described.values)
        {
            detail::write_shape_value(w, key, value);
        }
        w.end_object();
    }
    else
    {
        w.null("shape");
    }
    w.string("text", overlay.text);
    w.end_object();
}

/// The lines of the inputs: what each step put on the line of a variable it writes.
void write_lines(JsonWriter& w, const TimelineInput& input)
{
    const scenario::Scenario& scenario = *input.scenario;
    w.begin_array("lines");
    if (input.record != nullptr)
    {
        for (const scenario::TimelineRecord::Line& line : input.record->lines())
        {
            if (line.dither)
            {
                write_dither_line(w, scenario, line);
                continue;
            }
            const scenario::ScenarioStep& step = scenario.steps[line.step];
            const scenario::ScenarioMotion* motion = step.kind == scenario::StepKind::Set ? nullptr : &step.motions[line.part];
            w.begin_object();
            w.string("variable", scenario.variables[line.variable].text);
            w.string("kind", scenario::step_kind_name(step.kind));
            if (!step.file.empty())
            {
                w.string("file", step.file);
            }
            w.integer("line", static_cast<std::int64_t>(step.line));
            w.integer("step", static_cast<std::int64_t>(line.step + 1));
            write_ms(w, "from_ms", line.from);
            write_ms(w, "to_ms", motion != nullptr ? line.from + std::chrono::milliseconds(motion->length_ms) : line.from);
            write_value(w, "base", line.base);
            write_text_or_null(w, "base_source", line.base_source != nullptr ? line.base_source : "");
            if (motion != nullptr)
            {
                w.begin_object("shape");
                w.string("type", motion->described.type);
                for (const auto& [key, value] : motion->described.values)
                {
                    detail::write_shape_value(w, key, value);
                }
                w.end_object();
            }
            else
            {
                w.null("shape");
            }
            w.string("text", motion != nullptr ? motion->text : step.writes[line.part].text);
            w.end_object();
        }
    }
    w.end_array();
}

/// Of a step: where its lines are in "lines".
void write_step_lines(JsonWriter& w, const TimelineInput& input, std::size_t step)
{
    w.begin_array_on_one_line("lines");
    if (input.record != nullptr)
    {
        const auto& all = input.record->lines();
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (all[i].step == step)
            {
                w.integer({}, static_cast<std::int64_t>(i));
            }
        }
    }
    w.end_array();
}

void write_limits(JsonWriter& w, const TimelineInput& input)
{
    const scenario::TimelineLimits limits = input.record != nullptr ? input.record->limits() : scenario::TimelineLimits{};
    w.begin_object("limits");
    w.integer("points_per_series", static_cast<std::int64_t>(limits.points_per_series));
    w.integer("points_total", static_cast<std::int64_t>(limits.points_total));
    w.begin_array("dropped");
    if (input.record != nullptr)
    {
        for (const scenario::TimelineRecord::Series& series : input.record->series())
        {
            if (series.dropped > 0)
            {
                w.begin_object();
                w.string("variable", input.scenario->variables[series.variable].text);
                w.string("source", series.sampled ? "sampled" : "written");
                w.integer("dropped", static_cast<std::int64_t>(series.dropped));
                write_ms(w, "dropped_from_ms", series.dropped_from);
                write_ms(w, "dropped_before_ms", series.dropped_before);
                w.end_object();
            }
        }
    }
    w.end_array();
    w.end_object();
}

} // namespace

bool timeline_takes_finding(const std::string& code, bool& code_only)
{
    // Not "behind-real-time": how well the run kept its pace changes nothing of what the
    // scenario wrote and saw in virtual time, and the file must not differ for it.
    code_only = code == "control-link-late-values";
    return code_only || code.rfind("scenario-", 0) == 0;
}

std::string format_timeline_json(const TimelineInput& input)
{
    const scenario::Scenario& scenario = *input.scenario;
    const scenario::ScenarioRun& run = *input.run;
    JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_timeline", kTimelineFormat);
    w.string("vehicle_sil", version());
    w.string("stage", "run");
    w.string("file", input.file);
    write_text_or_null(w, "variant", input.variant);
    detail::write_params_json(w, "params", input.params);
    w.integer("time_step_ms", input.time_step_ms);
    w.begin_object("scenario");
    w.string("file", run.file);
    write_text_or_null(w, "name", run.name);
    if (!run.case_name.empty())
    {
        w.string("case", run.case_name);
    }
    w.string("status", scenario::scenario_status_name(run.status));
    write_ms(w, "over_at_ms", run.over_at);
    write_ms(w, "recorded_to_ms", input.record != nullptr ? input.record->recorded_to() : std::nullopt);
    if (run.end_ms)
    {
        w.integer("end_ms", *run.end_ms);
    }
    else
    {
        w.null("end_ms");
    }
    w.end_object();
    write_variables(w, input);
    w.begin_array("steps");
    for (std::size_t i = 0; i < run.steps.size() && i < scenario.steps.size(); ++i)
    {
        w.begin_object();
        detail::write_scenario_step_members(w, run.steps[i]);
        write_step_lines(w, input, i);
        write_watch(w, scenario, scenario.steps[i], run.steps[i],
                    input.record != nullptr ? input.record->watches()[i] : std::nullopt);
        write_failed(w, scenario, run.steps[i], input.record != nullptr ? input.record->failures()[i] : std::nullopt, input.record);
        w.end_object();
    }
    w.end_array();
    write_series(w, input);
    write_lines(w, input);
    w.begin_array("findings");
    for (const config::Finding& finding : input.findings)
    {
        bool codeOnly = false;
        if (!timeline_takes_finding(finding.code, codeOnly))
        {
            continue;
        }
        w.begin_object();
        if (codeOnly)
        {
            // Its code and severity; the rest may differ from run to run, so it is null.
            w.string("severity", detail::severity_text(finding.severity));
            w.string("code", finding.code);
            for (const char* key : {"file", "line", "participant", "network", "message", "detail"})
            {
                w.null(key);
            }
        }
        else
        {
            detail::write_finding_members(w, input.file, finding);
        }
        w.end_object();
    }
    w.end_array();
    write_limits(w, input);
    w.end_object();
    return w.take() + "\n";
}

} // namespace vehicle_sil::launch
