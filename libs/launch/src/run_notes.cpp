// SPDX-License-Identifier: MIT
#include "run_notes.hpp"

#include "vehicle_sil/launch/events.hpp"

#include <algorithm>
#include <cmath>

namespace vehicle_sil::launch::detail {

namespace {

/// run --events: the virtual time is written at most this often while it moves.
constexpr std::chrono::milliseconds kTimeInterval{100};

} // namespace

RunNotes::RunNotes(const RunContext& ctx, const RunParticipants& participants) : ctx_(ctx), participants_(participants)
{
    joined_.resize(ctx.config.participants.size(), false);
}

void RunNotes::publish_findings(const RunOutcome& outcome, bool final)
{
    if (ctx_.options.events == nullptr)
    {
        return;
    }
    for (const Finding& f : final ? assess_run(outcome) : detail::settled_findings(outcome))
    {
        if (std::find(published_.begin(), published_.end(), f) == published_.end())
        {
            ctx_.options.events->finding(ctx_.check.file, f);
            published_.push_back(f);
        }
    }
}

void RunNotes::note_failed(const RunOutcome& outcome, const std::string& name, std::string_view reason)
{
    if (ctx_.options.events == nullptr)
    {
        return;
    }
    std::string message;
    for (const Finding& f : detail::settled_findings(outcome))
    {
        if (f.participant == name && (f.code == "participant-failed" || f.code == "ended-before-start"))
        {
            message = f.message;
        }
    }
    ctx_.options.events->participant_failed(name, reason, message);
}

void RunNotes::note_joined(SimulationController& controller, const RunOutcome& outcome)
{
    if (ctx_.options.events == nullptr)
    {
        return;
    }
    for (std::size_t i = 0; i < outcome.participants.size(); ++i)
    {
        const ParticipantRun& run = outcome.participants[i];
        if (!participants_.in_run(i) || joined_[i] || run.exit || run.ending == Ending::Killed)
        {
            continue;
        }
        if (controller.runtime().peer_status(run.name).state)
        {
            joined_[i] = true;
            ctx_.options.events->participant(run.name, ParticipantState::Joined);
        }
    }
}

void RunNotes::note_time(SimulationController& controller, TimeNote note, const RunOutcome& outcome)
{
    if (ctx_.options.events == nullptr)
    {
        return;
    }
    const TimeState s = controller.time_state();
    const auto wall = std::chrono::steady_clock::now();
    const std::int64_t now = sim_ms(controller, s);
    const bool paused = noted_paused(s, note);
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
    const bool stalled = late > outcome.timing.late_tolerance_ms && late != last_late_ms_;
    // Only that it moved, or is late: not while other events wait for a slow reader (they
    // would pile up).
    const bool moved = (ms > last_time_ms_ || stalled || s.late_steps != last_late_steps_) &&
                       (note == TimeNote::Last ||
                        (wall - last_time_wall_ >= kTimeInterval && ctx_.options.events->waiting_events() == 0));
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
    ctx_.options.events->time(e);
    last_late_ms_ = late;
    last_late_steps_ = s.late_steps;
    last_time_ms_ = ms;
    last_time_wall_ = wall;
    last_paused_ = paused;
    last_speed_ = s.speed;
}

} // namespace vehicle_sil::launch::detail
