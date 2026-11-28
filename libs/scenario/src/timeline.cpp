// SPDX-License-Identifier: MIT
#include "vehicle_sil/scenario/timeline.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <tuple>
#include <utility>

namespace vehicle_sil::scenario {

namespace {

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

TimelineRecord::TimelineRecord(const Scenario& scenario, TimelineLimits limits,
                               std::span<const RecordedVariable> extra_recorded) :
  limits_(limits)
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
            for (const ScenarioComparison& comp : c.all)
            {
                if (comp.rhs_variable)
                {
                    reads[*comp.rhs_variable] = true;
                }
            }
        }
    }
    // The inputs the scenario ever puts a dither on, by its text: not by what a run got to.
    std::vector<bool> dithered(count, false);
    for (const ScenarioOverlay& input : scenario.inputs)
    {
        dithered[input.variable] = true;
    }
    for (const ScenarioStep& step : scenario.steps)
    {
        for (const ScenarioOverlay& overlay : step.overlays)
        {
            dithered[overlay.variable] = dithered[overlay.variable] || overlay.shape != nullptr;
        }
    }
    std::vector<bool> recorded_scenario(count, false);
    for (const RecordedVariable& rv : extra_recorded)
    {
        const auto existing = std::find_if(scenario.variables.begin(), scenario.variables.end(),
                                           [&rv](const ScenarioVariable& v) { return v.text == rv.text; });
        if (existing != scenario.variables.end())
        {
            recorded_scenario[static_cast<std::size_t>(existing - scenario.variables.begin())] = true;
            continue;
        }
        const bool already_extra = std::any_of(extra_variables_.begin(), extra_variables_.end(),
                                               [&rv](const RecordedVariable& e) { return e.text == rv.text; });
        if (!already_extra)
        {
            extra_variables_.push_back(rv);
        }
    }
    const std::size_t total = count + extra_variables_.size();
    written_.resize(total);
    sampled_.resize(total);
    watched_.assign(total, false);
    recorded_.assign(total, false);
    for (std::size_t v = 0; v < count; ++v)
    {
        watched_[v] = reads[v];
        recorded_[v] = recorded_scenario[v];
        if (writes[v])
        {
            written_[v] = series_.size();
            series_.push_back(Series{v, false});
            series_.back().dithered = dithered[v];
        }
        if (watched_[v] || recorded_[v])
        {
            sampled_[v] = series_.size();
            series_.push_back(Series{v, true});
        }
    }
    for (std::size_t k = 0; k < extra_variables_.size(); ++k)
    {
        const std::size_t v = count + k;
        watched_[v] = false;
        recorded_[v] = true;
        sampled_[v] = series_.size();
        series_.push_back(Series{v, true});
    }
    watches_.resize(scenario.steps.size());
    failures_.resize(scenario.steps.size());
    waiting_dither_.resize(count);
}

std::optional<std::uint64_t> TimelineRecord::written(std::size_t step, std::size_t variable, SimTime at, const control::Value& value, bool motion,
                                                     double base)
{
    if (!recorded_to_)
    {
        // The first write of a line is the one for the time it was put at.
        for (auto line = lines_.rbegin(); line != lines_.rend() && line->from == at; ++line)
        {
            if (!line->dither && line->step == step && line->variable == variable)
            {
                line->sent = true;
            }
        }
        // The dither that waits on the variable is carried from this write on: a line now, after
        // those that the steps put at this time.
        if (variable < waiting_dither_.size() && waiting_dither_[variable])
        {
            Line line = std::move(*waiting_dither_[variable]);
            waiting_dither_[variable].reset();
            line.from = at;
            line.sent = true;
            lines_.push_back(std::move(line));
        }
    }
    if (recorded_to_ || variable >= written_.size() || !written_[variable])
    {
        return std::nullopt;
    }
    Series& series = series_[*written_[variable]];
    const std::uint64_t serial = next_serial_++;
    series.written.push_back(Written{at, value, motion, false, serial, base});
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

void TimelineRecord::dither(Line line)
{
    if (!recorded_to_ && line.variable < waiting_dither_.size())
    {
        line.dither = true;
        line.sent = false;
        waiting_dither_[line.variable] = std::move(line);
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
    if (!recorded_to_ && (step < failures_.size() || step == kNoStep))
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
        if (step < failures_.size())
        {
            failures_[step] = std::move(failure);
        }
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
    // early; with a shorter step it is not there. A line of which no write went out was put, and
    // then the scenario failed at a write before its own. Canonicalize order across differing ECU
    // applied steps so lines[] stays strictly ordered by (from, dither, step, part) and invariant
    // to controller time_step_ms.
    std::erase_if(lines_, [to](const Line& line) { return line.from > to || !line.sent; });
    std::stable_sort(lines_.begin(), lines_.end(), [](const Line& a, const Line& b) {
        const auto key = [](const Line& line) {
            return std::tuple{line.from, line.dither, line.dither ? std::size_t{0} : line.step,
                              line.dither ? std::size_t{0} : line.part};
        };
        return key(a) < key(b);
    });
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

} // namespace vehicle_sil::scenario
