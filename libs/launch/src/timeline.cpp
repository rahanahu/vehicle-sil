// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/timeline.hpp"

#include "check_format.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/ecu/detail/json_writer.hpp"
#include "vehicle_sil/launch/scenario_run.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using vehicle_sil::detail::JsonWriter;

constexpr double kNanosPerMilli = 1'000'000.0;

/// Whether two values are written with the same text: -0 and 0 are not, and two values that are
/// no numbers (both written as null) are.
bool same_text(const control::Value& a, const control::Value& b)
{
    if (a.index() != b.index())
    {
        return false;
    }
    if (const auto* x = std::get_if<double>(&a))
    {
        const double y = std::get<double>(b);
        if (!std::isfinite(*x) || !std::isfinite(y))
        {
            return !std::isfinite(*x) && !std::isfinite(y);
        }
        return std::bit_cast<std::uint64_t>(*x) == std::bit_cast<std::uint64_t>(y);
    }
    return a == b;
}

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

const char* compare_name(Compare op)
{
    switch (op)
    {
    case Compare::Eq: return "eq";
    case Compare::Ne: return "ne";
    case Compare::Ge: return "ge";
    case Compare::Le: return "le";
    case Compare::Gt: return "gt";
    case Compare::Lt: return "lt";
    case Compare::Near: return "near";
    }
    return "eq";
}

const control::VariableInfo* described(const ScenarioCatalog* catalog, const Scenario& scenario, const ScenarioVariable& variable)
{
    if (catalog == nullptr)
    {
        return nullptr;
    }
    const std::string& participant = scenario.participants[variable.participant].name;
    for (const CatalogParticipant& p : *catalog)
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
    const Scenario& scenario = *input.scenario;
    // Without a record (the scenario never began): from the steps, as the record does.
    const TimelineRecord fromSteps(scenario);
    const TimelineRecord& uses = input.record != nullptr ? *input.record : fromSteps;
    w.begin_array("variables");
    for (std::size_t i = 0; i < scenario.variables.size(); ++i)
    {
        const ScenarioVariable& v = scenario.variables[i];
        const control::VariableInfo* info = described(input.catalog, scenario, v);
        w.begin_object();
        w.string("name", v.text);
        w.string("participant", scenario.participants[v.participant].name);
        w.string("type", vehicle_sil::detail::value_type_name(v.type));
        if (info != nullptr)
        {
            w.string("causality", vehicle_sil::detail::causality_name(info->causality));
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

void write_watch(JsonWriter& w, const Scenario& scenario, const ScenarioStep& step, const ScenarioStepRun& run,
                 const std::optional<TimelineRecord::Watch>& watch)
{
    if (step.conditions.empty())
    {
        w.null("watch");
        return;
    }
    WatchMode mode = WatchMode::At;
    if (step.kind == StepKind::Ramp || step.kind == StepKind::Wave)
    {
        mode = WatchMode::During;
    }
    else if (step.within_ms)
    {
        mode = WatchMode::Once;
    }
    else if (step.hold_for_ms)
    {
        mode = WatchMode::Always;
    }
    w.begin_object("watch");
    w.string("mode", watch_mode_name(mode));
    // The times of a step that has its time; a step the scenario did not get to has none.
    const bool timed = watch && run.start;
    write_ms(w, "from_ms", timed ? std::optional<SimTime>(watch->from) : std::nullopt);
    write_ms(w, "until_ms", timed ? watch->until : std::nullopt);
    w.begin_array("conditions");
    for (const ScenarioCondition& condition : step.conditions)
    {
        w.begin_object();
        w.string("variable", scenario.variables[condition.variable].text);
        w.begin_array("compare");
        for (const ScenarioComparison& c : condition.all)
        {
            w.begin_object();
            w.string("op", compare_name(c.op));
            write_value(w, "value", c.value);
            if (c.op == Compare::Near)
            {
                w.real("tolerance", c.tolerance);
            }
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

void write_failed(JsonWriter& w, const Scenario& scenario, const ScenarioStepRun& run, const std::optional<TimelineFailure>& failure,
                  const TimelineRecord* record)
{
    if (run.status != StepStatus::Failed)
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
        for (const TimelineFailedCondition& c : failure->conditions)
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
        for (const TimelineFailedWrite& write : failure->writes)
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
        for (const TimelineRecord::Series& series : input.record->series())
        {
            w.begin_object();
            w.string("variable", input.scenario->variables[series.variable].text);
            w.string("source", series.sampled ? "sampled" : "written");
            w.string("encoding", series.sampled ? "changes" : "all");
            if (series.sampled)
            {
                w.integer("samples", static_cast<std::int64_t>(series.sample_count));
                write_ms(w, "first_ms", series.first_sample);
                write_ms(w, "last_ms", series.last_sample);
            }
            else
            {
                // Sent and not known to be applied: the first write of the failed request that
                // was not applied (or has no result) and what was sent after it, and what was
                // written for a time after the step at which the scenario was over (no ECU got
                // to that time).
                const auto it = std::find_if(series.written.begin(), series.written.end(), [&](const TimelineRecord::Written& p) {
                    return p.refused || (input.run->over_at && p.at > *input.run->over_at);
                });
                if (it != series.written.end())
                {
                    w.integer("not_applied_from", it - series.written.begin());
                }
            }
            w.begin_array("points");
            for (const TimelineRecord::Written& point : series.written)
            {
                w.begin_array_on_one_line({});
                write_ms(w, {}, point.at);
                write_value(w, {}, point.value);
                w.end_array();
            }
            for (const TimelineRecord::Sampled& point : series.samples)
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

void write_shape_value(JsonWriter& w, std::string_view key, const ScenarioShapeText::Value& value)
{
    if (const auto* d = std::get_if<double>(&value))
    {
        w.real(key, *d);
    }
    else if (const auto* i = std::get_if<std::int64_t>(&value))
    {
        w.integer(key, *i);
    }
    else if (const auto* b = std::get_if<bool>(&value))
    {
        w.boolean(key, *b);
    }
    else
    {
        w.string(key, std::get<std::string>(value));
    }
}

/// The lines of the inputs: what each step put on the line of a variable it writes.
void write_lines(JsonWriter& w, const TimelineInput& input)
{
    const Scenario& scenario = *input.scenario;
    w.begin_array("lines");
    if (input.record != nullptr)
    {
        for (const TimelineRecord::Line& line : input.record->lines())
        {
            const ScenarioStep& step = scenario.steps[line.step];
            const ScenarioMotion* motion = step.kind == StepKind::Set ? nullptr : &step.motions[line.part];
            w.begin_object();
            w.string("variable", scenario.variables[line.variable].text);
            w.string("kind", step_kind_name(step.kind));
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
                    write_shape_value(w, key, value);
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
    const TimelineLimits limits = input.record != nullptr ? input.record->limits() : TimelineLimits{};
    w.begin_object("limits");
    w.integer("points_per_series", static_cast<std::int64_t>(limits.points_per_series));
    w.integer("points_total", static_cast<std::int64_t>(limits.points_total));
    w.begin_array("dropped");
    if (input.record != nullptr)
    {
        for (const TimelineRecord::Series& series : input.record->series())
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

const char* watch_mode_name(WatchMode mode)
{
    switch (mode)
    {
    case WatchMode::At: return "at";
    case WatchMode::Always: return "always";
    case WatchMode::Once: return "once";
    case WatchMode::During: return "during";
    }
    return "at";
}

TimelineRecord::TimelineRecord(const Scenario& scenario, TimelineLimits limits) : limits_(limits)
{
    const std::size_t count = scenario.variables.size();
    std::vector<bool> writes(count, false);
    std::vector<bool> reads(count, false);
    for (const ScenarioStep& step : scenario.steps)
    {
        for (const ScenarioWrite& w : step.writes)
        {
            writes[w.variable] = true;
        }
        for (const ScenarioMotion& s : step.motions)
        {
            writes[s.variable] = true;
        }
        for (const ScenarioCondition& c : step.conditions)
        {
            reads[c.variable] = true;
        }
    }
    written_.resize(count);
    sampled_.resize(count);
    for (std::size_t v = 0; v < count; ++v)
    {
        if (writes[v])
        {
            written_[v] = series_.size();
            series_.push_back(Series{v, false});
        }
        if (reads[v])
        {
            sampled_[v] = series_.size();
            series_.push_back(Series{v, true});
        }
    }
    watches_.resize(scenario.steps.size());
    failures_.resize(scenario.steps.size());
}

std::optional<std::uint64_t> TimelineRecord::written(std::size_t step, std::size_t variable, SimTime at, const control::Value& value, bool motion)
{
    if (!recorded_to_)
    {
        // The first write of a line is the one for the time it was put at.
        for (auto line = lines_.rbegin(); line != lines_.rend() && line->from == at; ++line)
        {
            if (line->step == step && line->variable == variable)
            {
                line->sent = true;
            }
        }
    }
    if (recorded_to_ || variable >= written_.size() || !written_[variable])
    {
        return std::nullopt;
    }
    Series& series = series_[*written_[variable]];
    const std::uint64_t serial = next_serial_++;
    series.written.push_back(Written{at, value, motion, false, serial});
    if (motion)
    {
        ++total_;
        ++series.droppable;
        if (series.droppable >= 2 * limits_.points_per_series || total_ >= 2 * limits_.points_total)
        {
            enforce(false);
        }
    }
    return serial;
}

std::optional<std::size_t> TimelineRecord::point_of(std::size_t variable, std::uint64_t serial) const
{
    if (variable >= written_.size() || !written_[variable])
    {
        return std::nullopt;
    }
    // From the newest: a write that fails is among the last sent.
    const std::vector<Written>& points = series_[*written_[variable]].written;
    const auto it = std::find_if(points.rbegin(), points.rend(), [serial](const Written& p) { return p.serial == serial; });
    return it == points.rend() ? std::nullopt : std::optional<std::size_t>(points.rend() - it - 1);
}

void TimelineRecord::sampled(std::size_t variable, SimTime at, const control::Value& value)
{
    if (recorded_to_ || variable >= sampled_.size() || !sampled_[variable])
    {
        return;
    }
    Series& series = series_[*sampled_[variable]];
    ++series.sample_count;
    if (!series.first_sample)
    {
        series.first_sample = at;
    }
    series.last_sample = at;
    bool repeats = false;
    if (!series.samples.empty())
    {
        Sampled& back = series.samples.back();
        if (same_text(back.value, value))
        {
            if (back.count == 1)
            {
                back.interval = at - back.first;
            }
            if (at - back.last == back.interval)
            {
                back.last = at;
                ++back.count;
                return;
            }
            repeats = true;
        }
    }
    series.samples.push_back(Sampled{at, at, SimDuration{}, 1, value, repeats});
    ++total_;
    ++series.droppable;
    if (series.droppable >= 2 * limits_.points_per_series || total_ >= 2 * limits_.points_total)
    {
        enforce(false);
    }
}

void TimelineRecord::line(Line line)
{
    if (!recorded_to_)
    {
        lines_.push_back(std::move(line));
    }
}

void TimelineRecord::watch(std::size_t step, Watch watch)
{
    if (!recorded_to_ && step < watches_.size())
    {
        watches_[step] = watch;
    }
}

std::uint64_t TimelineRecord::dropped() const noexcept
{
    std::uint64_t out = 0;
    for (const Series& series : series_)
    {
        out += series.dropped;
    }
    return out;
}

void TimelineRecord::failed(std::size_t step, TimelineFailure failure)
{
    if (!recorded_to_ && step < failures_.size())
    {
        // The points that are not known to be applied, each by what written() named it (not by
        // its time: a variable can have several points of one time).
        for (const TimelineFailedWrite& write : failure.writes)
        {
            if (write.applied != std::optional<bool>(true) && write.sent)
            {
                if (const auto point = point_of(write.variable, *write.sent))
                {
                    series_[*written_[write.variable]].written[*point].refused = true;
                }
            }
        }
        failures_[step] = std::move(failure);
    }
}

void TimelineRecord::close(SimTime to)
{
    if (recorded_to_)
    {
        return;
    }
    recorded_to_ = to;
    // A line put after `to` is of a write for a time the controller's step let the engine reach
    // early; with a shorter step it is not there. (They are in the order of their times.) A line
    // of which no write went out was put, and then the scenario failed at a write before its own.
    std::erase_if(lines_, [to](const Line& line) { return line.from > to || !line.sent; });
    for (Series& series : series_)
    {
        std::erase_if(series.written, [&](const Written& point) {
            if (point.at > to && point.motion)
            {
                --series.droppable;
            }
            return point.at > to;
        });
        while (!series.samples.empty() && series.samples.back().first > to)
        {
            series.sample_count -= series.samples.back().count;
            series.samples.pop_back();
            --series.droppable;
        }
        if (!series.samples.empty() && series.samples.back().last > to)
        {
            // Its samples come at one distance: those up to `to` can be counted.
            Sampled& back = series.samples.back();
            const std::uint64_t kept = static_cast<std::uint64_t>((to - back.first).count() / back.interval.count()) + 1;
            series.sample_count -= back.count - kept;
            back.count = kept;
            back.last = back.first + static_cast<std::int64_t>(kept - 1) * back.interval;
        }
        if (series.sampled)
        {
            if (series.sample_count == 0)
            {
                series.first_sample.reset();
            }
            series.last_sample = series.samples.empty() ? std::nullopt : std::optional<SimTime>(series.samples.back().last);
        }
    }
    enforce(true);
}

void TimelineRecord::drop_oldest(Series& series, std::size_t keep)
{
    if (series.droppable <= keep)
    {
        return;
    }
    std::size_t drop = series.droppable - keep;
    series.droppable = keep;
    if (series.sampled)
    {
        const auto end = series.samples.begin() + static_cast<std::ptrdiff_t>(drop);
        if (!series.dropped_from)
        {
            series.dropped_from = series.samples.front().first;
        }
        series.dropped += static_cast<std::uint64_t>(
            std::count_if(series.samples.begin(), end, [](const Sampled& point) { return !point.repeats; }));
        series.samples.erase(series.samples.begin(), end);
        if (!series.samples.empty())
        {
            if (series.samples.front().repeats)
            {
                // The oldest that stays: a point now, in place of one of those that went.
                series.samples.front().repeats = false;
                --series.dropped;
            }
            series.dropped_before = series.samples.front().first;
        }
        return;
    }
    series.dropped += drop;
    std::erase_if(series.written, [&drop, &series](const Written& point) {
        if (point.motion && drop > 0)
        {
            if (!series.dropped_from)
            {
                series.dropped_from = point.at;
            }
            --drop;
            return true;
        }
        return false;
    });
    const auto first = std::find_if(series.written.begin(), series.written.end(), [](const Written& point) { return point.motion; });
    if (first != series.written.end())
    {
        series.dropped_before = first->at;
    }
}

void TimelineRecord::enforce(bool final)
{
    // While the scenario runs, half a limit more is kept: close() may still take away the newest
    // points (those after the time the scenario was decided at), and what then stays must be the
    // newest of the rest.
    const std::size_t perSeries = final ? limits_.points_per_series : limits_.points_per_series + limits_.points_per_series / 2;
    const std::size_t all = final ? limits_.points_total : limits_.points_total + limits_.points_total / 2;
    std::vector<std::size_t> counts(series_.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < series_.size(); ++i)
    {
        drop_oldest(series_[i], perSeries);
        counts[i] = series_[i].droppable;
        total += counts[i];
    }
    if (total > all)
    {
        // The series with the most points give first: the highest count that no series may
        // exceed so that all fit.
        const auto fits = [&](std::size_t level) {
            std::size_t sum = 0;
            for (const std::size_t c : counts)
            {
                sum += std::min(c, level);
            }
            return sum <= all;
        };
        std::size_t low = 0;
        std::size_t high = *std::max_element(counts.begin(), counts.end());
        while (low < high)
        {
            const std::size_t middle = low + (high - low + 1) / 2;
            if (fits(middle))
            {
                low = middle;
            }
            else
            {
                high = middle - 1;
            }
        }
        total = 0;
        for (std::size_t i = 0; i < series_.size(); ++i)
        {
            drop_oldest(series_[i], low);
            total += series_[i].droppable;
        }
    }
    total_ = total;
}

bool timeline_takes_finding(const std::string& code, bool& code_only)
{
    // Not "behind-real-time": how well the run kept its pace changes nothing of what the
    // scenario wrote and saw in virtual time, and the file must not differ for it.
    code_only = code == "control-link-late-values";
    return code_only || code.rfind("scenario-", 0) == 0;
}

std::string format_timeline_json(const TimelineInput& input)
{
    const Scenario& scenario = *input.scenario;
    const ScenarioRun& run = *input.run;
    JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_timeline", kTimelineFormat);
    w.string("vehicle_sil", version());
    w.string("stage", "run");
    w.string("file", input.file);
    write_text_or_null(w, "variant", input.variant);
    detail::write_params_json(w, "params", input.params);
    w.integer("tick_ms", input.tick_ms);
    w.begin_object("scenario");
    w.string("file", run.file);
    write_text_or_null(w, "name", run.name);
    w.string("status", scenario_status_name(run.status));
    write_ms(w, "over_at_ms", run.over_at);
    write_ms(w, "recorded_to_ms", input.record != nullptr ? input.record->recorded_to() : std::nullopt);
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
    for (const Finding& finding : input.findings)
    {
        bool codeOnly = false;
        if (!timeline_takes_finding(finding.code, codeOnly))
        {
            continue;
        }
        w.begin_object();
        if (codeOnly)
        {
            w.string("code", finding.code);
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
