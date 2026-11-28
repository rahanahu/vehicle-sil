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

TimelineRecord::TimelineRecord(std::span<const RecordedVariable> recorded, TimelineLimits limits) :
  limits_(limits)
{
    for (const RecordedVariable& rv : recorded)
    {
        const bool already = std::any_of(extra_variables_.begin(), extra_variables_.end(),
                                         [&rv](const RecordedVariable& e) { return e.text == rv.text; });
        if (!already)
        {
            extra_variables_.push_back(rv);
        }
    }
    const std::size_t total = extra_variables_.size();
    written_.resize(total);
    sampled_.resize(total);
    watched_.assign(total, false);
    recorded_.assign(total, true);
    series_.reserve(total);
    for (std::size_t v = 0; v < total; ++v)
    {
        sampled_[v] = series_.size();
        series_.push_back(Series{v, true});
    }
}

std::optional<std::uint64_t> TimelineRecord::written(std::size_t step, std::size_t variable, SimTime at, const control::Value& value, bool motion,
                                                     double base)
{
    if (!closed_)
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
    if (closed_ || variable >= written_.size() || !written_[variable])
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
    if (closed_ || variable >= sampled_.size() || !sampled_[variable])
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
    if (!closed_)
    {
        lines_.push_back(std::move(line));
    }
}

void TimelineRecord::dither(Line line)
{
    if (!closed_ && line.variable < waiting_dither_.size())
    {
        line.dither = true;
        line.sent = false;
        waiting_dither_[line.variable] = std::move(line);
    }
}

void TimelineRecord::watch(std::size_t step, Watch watch)
{
    if (!closed_ && step < watches_.size())
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

std::uint64_t TimelineRecord::dropped_events() const noexcept
{
    std::uint64_t out = 0;
    if (closed_)
    {
        for (const EventDrop& d : events_dropped_)
        {
            out += d.dropped + d.agent_dropped;
        }
        return out;
    }
    for (const ParticipantEvents& pe : participant_events_)
    {
        out += pe.dropped + pe.agent_dropped;
    }
    return out;
}

std::uint64_t TimelineRecord::dropped_can() const noexcept
{
    std::uint64_t out = 0;
    if (closed_)
    {
        for (const CanDrop& d : can_dropped_)
        {
            out += d.dropped;
        }
        return out;
    }
    for (const NetworkCanFrames& nc : network_can_)
    {
        out += nc.dropped;
    }
    return out;
}

std::uint64_t TimelineRecord::untimed_can_dropped() const noexcept
{
    std::uint64_t out = 0;
    if (closed_)
    {
        for (const CanDrop& d : can_dropped_)
        {
            out += d.untimed_dropped;
        }
        return out;
    }
    for (const NetworkCanFrames& nc : network_can_)
    {
        out += nc.untimed_dropped;
    }
    return out;
}

TimelineRecord::NetworkCanFrames& TimelineRecord::network_can_for(std::string_view network)
{
    for (NetworkCanFrames& nc : network_can_)
    {
        if (nc.network == network)
        {
            return nc;
        }
    }
    NetworkCanFrames& added = network_can_.emplace_back();
    added.network = std::string(network);
    return added;
}

void TimelineRecord::can_frame(std::string_view network, SimTime at, std::uint32_t id, bool extended, bool rtr, bool fd,
                               std::uint8_t dlc, std::span<const std::uint8_t> data)
{
    if (closed_ || !can_enabled_)
    {
        return;
    }
    NetworkCanFrames& nc = network_can_for(network);
    StoredCanFrame f{};
    f.at = at;
    f.id = id & (extended ? 0x1FFFFFFFu : 0x7FFu);
    f.extended = extended;
    f.rtr = rtr;
    f.fd = fd;
    f.dlc = std::min<std::uint8_t>(dlc, 15u);
    const std::size_t max_len = fd ? std::size_t{64} : std::size_t{8};
    const std::size_t len = rtr ? std::size_t{0} : std::min<std::size_t>(data.size(), max_len);
    f.data_len = static_cast<std::uint8_t>(len);
    if (len > 0)
    {
        std::copy_n(data.data(), len, f.data.begin());
    }
    f.seq = next_can_seq_++;
    nc.frames.push_back(f);
    ++can_total_;
    if (limits_.can_frames_per_network == 0 || limits_.can_frames_total == 0 ||
        nc.frames.size() >= 2 * limits_.can_frames_per_network || can_total_ >= 2 * limits_.can_frames_total)
    {
        enforce_can(false);
    }
}

void TimelineRecord::note_untimed_can_dropped(std::string_view network, std::uint64_t count)
{
    if (closed_ || !can_enabled_ || count == 0)
    {
        return;
    }
    NetworkCanFrames& nc = network_can_for(network);
    nc.untimed_dropped += count;
}

TimelineRecord::ParticipantEvents& TimelineRecord::participant_events_for(std::string_view participant)
{
    for (ParticipantEvents& pe : participant_events_)
    {
        if (pe.participant == participant)
        {
            return pe;
        }
    }
    ParticipantEvents& added = participant_events_.emplace_back();
    added.participant = std::string(participant);
    return added;
}

void TimelineRecord::event(std::string participant, SimTime at, control::EventLevel level, std::string text)
{
    if (closed_ || !events_enabled_)
    {
        return;
    }
    ParticipantEvents& pe = participant_events_for(participant);
    pe.events.push_back(Event{at, std::move(participant), level, std::move(text), next_event_seq_++});
    ++events_total_;
    if (limits_.events_per_participant == 0 || limits_.events_total == 0 ||
        pe.events.size() >= 2 * limits_.events_per_participant || events_total_ >= 2 * limits_.events_total)
    {
        enforce_events(false);
    }
}

void TimelineRecord::note_agent_events_dropped(std::string participant, std::uint64_t count)
{
    if (closed_ || !events_enabled_ || count == 0)
    {
        return;
    }
    ParticipantEvents& pe = participant_events_for(participant);
    pe.agent_dropped += count;
}

void TimelineRecord::failed(std::size_t step, TimelineFailure failure)
{
    if (!closed_ && (step < failures_.size() || step == kNoStep))
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

void TimelineRecord::close(std::optional<SimTime> to)
{
    if (closed_)
    {
        return;
    }
    closed_ = true;
    recorded_to_ = to;
    if (!to)
    {
        lines_.clear();
        for (Series& series : series_)
        {
            series.written.clear();
            series.samples.clear();
            series.droppable = 0;
            series.sample_count = 0;
            series.first_sample.reset();
            series.last_sample.reset();
        }
        total_ = 0;
        events_.clear();
        events_dropped_.clear();
        for (const ParticipantEvents& pe : participant_events_)
        {
            if (pe.agent_dropped > 0)
            {
                events_dropped_.push_back(EventDrop{pe.participant, 0, std::nullopt, std::nullopt, pe.agent_dropped});
            }
        }
        std::sort(events_dropped_.begin(), events_dropped_.end(),
                  [](const EventDrop& a, const EventDrop& b) { return a.participant < b.participant; });
        participant_events_.clear();
        events_total_ = 0;
        can_frames_.clear();
        can_dropped_.clear();
        for (const NetworkCanFrames& nc : network_can_)
        {
            if (nc.untimed_dropped > 0)
            {
                can_dropped_.push_back(CanDrop{nc.network, 0, std::nullopt, std::nullopt, nc.untimed_dropped});
            }
        }
        std::sort(can_dropped_.begin(), can_dropped_.end(),
                  [](const CanDrop& a, const CanDrop& b) { return a.network < b.network; });
        network_can_.clear();
        can_total_ = 0;
        return;
    }
    const SimTime limit = *to;
    // A line put after `limit` is of a write for a time the controller's step let the engine reach
    // early; with a shorter step it is not there. A line of which no write went out was put, and
    // then the scenario failed at a write before its own. Canonicalize order across differing ECU
    // applied steps so lines[] stays strictly ordered by (from, dither, step, part) and invariant
    // to controller time_step_ms.
    std::erase_if(lines_, [limit](const Line& line) { return line.from > limit || !line.sent; });
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
            if (point.at > limit && point.motion)
            {
                --series.droppable;
            }
            return point.at > limit;
        });
        while (!series.samples.empty() && series.samples.back().first > limit)
        {
            series.sample_count -= series.samples.back().count;
            series.samples.pop_back();
            --series.droppable;
        }
        if (!series.samples.empty() && series.samples.back().last > limit)
        {
            // Its samples come at one distance: those up to `limit` can be counted.
            Sampled& back = series.samples.back();
            const std::uint64_t kept = static_cast<std::uint64_t>((limit - back.first).count() / back.interval.count()) + 1;
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
    if (events_enabled_)
    {
        events_total_ = 0;
        for (ParticipantEvents& pe : participant_events_)
        {
            std::stable_sort(pe.events.begin(), pe.events.end(), [](const Event& a, const Event& b) {
                return a.at != b.at ? a.at < b.at : a.seq < b.seq;
            });
            while (!pe.events.empty() && pe.events.back().at > limit)
            {
                pe.events.pop_back();
            }
            if (pe.dropped > 0)
            {
                if (pe.events.empty())
                {
                    pe.dropped_before.reset();
                }
                else
                {
                    pe.dropped_before = pe.events.front().at;
                }
            }
            events_total_ += pe.events.size();
        }
        enforce_events(true);
        events_.clear();
        events_.reserve(events_total_);
        events_dropped_.clear();
        for (ParticipantEvents& pe : participant_events_)
        {
            for (Event& ev : pe.events)
            {
                events_.push_back(std::move(ev));
            }
            if (pe.dropped > 0 || pe.agent_dropped > 0)
            {
                events_dropped_.push_back(
                    EventDrop{pe.participant, pe.dropped, pe.dropped_from, pe.dropped_before, pe.agent_dropped});
            }
        }
        std::stable_sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
            if (a.at != b.at)
            {
                return a.at < b.at;
            }
            if (a.participant != b.participant)
            {
                return a.participant < b.participant;
            }
            return a.seq < b.seq;
        });
        std::sort(events_dropped_.begin(), events_dropped_.end(),
                  [](const EventDrop& a, const EventDrop& b) { return a.participant < b.participant; });
        participant_events_.clear();
    }
    if (can_enabled_)
    {
        can_total_ = 0;
        for (NetworkCanFrames& nc : network_can_)
        {
            std::stable_sort(nc.frames.begin(), nc.frames.end(), [](const StoredCanFrame& a, const StoredCanFrame& b) {
                return a.at != b.at ? a.at < b.at : a.seq < b.seq;
            });
            while (!nc.frames.empty() && nc.frames.back().at > limit)
            {
                nc.frames.pop_back();
            }
            if (nc.dropped > 0)
            {
                if (nc.frames.empty())
                {
                    nc.dropped_before.reset();
                }
                else
                {
                    nc.dropped_before = nc.frames.front().at;
                }
            }
            can_total_ += nc.frames.size();
        }
        enforce_can(true);
        can_frames_.clear();
        can_frames_.reserve(can_total_);
        can_dropped_.clear();
        for (const NetworkCanFrames& nc : network_can_)
        {
            for (const StoredCanFrame& f : nc.frames)
            {
                CanFrame out{};
                out.at = f.at;
                out.network = nc.network;
                out.id = f.id;
                out.extended = f.extended;
                out.rtr = f.rtr;
                out.fd = f.fd;
                out.dlc = f.dlc;
                out.data_len = f.data_len;
                out.data = f.data;
                out.seq = f.seq;
                can_frames_.push_back(std::move(out));
            }
            if (nc.dropped > 0 || nc.untimed_dropped > 0)
            {
                can_dropped_.push_back(
                    CanDrop{nc.network, nc.dropped, nc.dropped_from, nc.dropped_before, nc.untimed_dropped});
            }
        }
        std::stable_sort(can_frames_.begin(), can_frames_.end(), [](const CanFrame& a, const CanFrame& b) {
            if (a.at != b.at)
            {
                return a.at < b.at;
            }
            if (a.network != b.network)
            {
                return a.network < b.network;
            }
            return a.seq < b.seq;
        });
        std::sort(can_dropped_.begin(), can_dropped_.end(),
                  [](const CanDrop& a, const CanDrop& b) { return a.network < b.network; });
        network_can_.clear();
    }
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

void TimelineRecord::drop_oldest_events(ParticipantEvents& pe, std::size_t keep)
{
    if (pe.events.size() <= keep)
    {
        return;
    }
    const std::size_t drop = pe.events.size() - keep;
    if (!pe.dropped_from)
    {
        pe.dropped_from = pe.events.front().at;
    }
    else
    {
        pe.dropped_from = std::min(*pe.dropped_from, pe.events.front().at);
    }
    pe.dropped += static_cast<std::uint64_t>(drop);
    pe.events.erase(pe.events.begin(), pe.events.begin() + static_cast<std::ptrdiff_t>(drop));
    if (!pe.events.empty())
    {
        pe.dropped_before = pe.events.front().at;
    }
    else
    {
        pe.dropped_before.reset();
    }
}

void TimelineRecord::enforce_events(bool final)
{
    const std::size_t perPart =
        final ? limits_.events_per_participant : limits_.events_per_participant + limits_.events_per_participant / 2;
    const std::size_t all = final ? limits_.events_total : limits_.events_total + limits_.events_total / 2;
    std::vector<std::size_t> counts(participant_events_.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < participant_events_.size(); ++i)
    {
        drop_oldest_events(participant_events_[i], perPart);
        counts[i] = participant_events_[i].events.size();
        total += counts[i];
    }
    if (total > all)
    {
        if (all == 0)
        {
            for (ParticipantEvents& pe : participant_events_)
            {
                drop_oldest_events(pe, 0);
            }
            events_total_ = 0;
            return;
        }
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
        for (std::size_t i = 0; i < participant_events_.size(); ++i)
        {
            drop_oldest_events(participant_events_[i], low + 1);
            total += participant_events_[i].events.size();
        }
        while (total > all)
        {
            std::size_t victim = 0;
            bool found = false;
            for (std::size_t i = 0; i < participant_events_.size(); ++i)
            {
                const auto& pe = participant_events_[i];
                if (pe.events.empty())
                {
                    continue;
                }
                if (!found)
                {
                    victim = i;
                    found = true;
                    continue;
                }
                const auto& cur = participant_events_[victim];
                if (pe.events.size() != cur.events.size())
                {
                    if (pe.events.size() > cur.events.size())
                    {
                        victim = i;
                    }
                }
                else
                {
                    const auto key = [](const ParticipantEvents& x) {
                        return std::tuple{x.events.front().at, std::string_view(x.participant), x.events.front().seq};
                    };
                    if (key(pe) < key(cur))
                    {
                        victim = i;
                    }
                }
            }
            if (!found)
            {
                break;
            }
            drop_oldest_events(participant_events_[victim], participant_events_[victim].events.size() - 1);
            --total;
        }
    }
    events_total_ = total;
}

void TimelineRecord::drop_oldest_can(NetworkCanFrames& nc, std::size_t keep)
{
    if (nc.frames.size() <= keep)
    {
        return;
    }
    const std::size_t drop = nc.frames.size() - keep;
    for (std::size_t i = 0; i < drop; ++i)
    {
        const SimTime t = nc.frames[i].at;
        nc.dropped_from = nc.dropped_from ? std::min(*nc.dropped_from, t) : t;
    }
    nc.dropped += static_cast<std::uint64_t>(drop);
    nc.frames.erase(nc.frames.begin(), nc.frames.begin() + static_cast<std::ptrdiff_t>(drop));
    if (!nc.frames.empty())
    {
        nc.dropped_before = nc.frames.front().at;
    }
    else
    {
        nc.dropped_before.reset();
    }
}

void TimelineRecord::enforce_can(bool final)
{
    const std::size_t perNet =
        final ? limits_.can_frames_per_network : limits_.can_frames_per_network + limits_.can_frames_per_network / 2;
    const std::size_t all = final ? limits_.can_frames_total : limits_.can_frames_total + limits_.can_frames_total / 2;
    std::vector<std::size_t> counts(network_can_.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < network_can_.size(); ++i)
    {
        drop_oldest_can(network_can_[i], perNet);
        counts[i] = network_can_[i].frames.size();
        total += counts[i];
    }
    if (total > all)
    {
        if (all == 0)
        {
            for (NetworkCanFrames& nc : network_can_)
            {
                drop_oldest_can(nc, 0);
            }
            can_total_ = 0;
            return;
        }
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
        for (std::size_t i = 0; i < network_can_.size(); ++i)
        {
            drop_oldest_can(network_can_[i], low + 1);
            total += network_can_[i].frames.size();
        }
        while (total > all)
        {
            std::size_t victim = 0;
            bool found = false;
            for (std::size_t i = 0; i < network_can_.size(); ++i)
            {
                const auto& nc = network_can_[i];
                if (nc.frames.empty())
                {
                    continue;
                }
                if (!found)
                {
                    victim = i;
                    found = true;
                    continue;
                }
                const auto& cur = network_can_[victim];
                if (nc.frames.size() != cur.frames.size())
                {
                    if (nc.frames.size() > cur.frames.size())
                    {
                        victim = i;
                    }
                }
                else
                {
                    const auto key = [](const NetworkCanFrames& x) {
                        return std::tuple{x.frames.front().at, std::string_view(x.network), x.frames.front().seq};
                    };
                    if (key(nc) < key(cur))
                    {
                        victim = i;
                    }
                }
            }
            if (!found)
            {
                break;
            }
            drop_oldest_can(network_can_[victim], network_can_[victim].frames.size() - 1);
            --total;
        }
    }
    can_total_ = total;
}

} // namespace vehicle_sil::scenario
