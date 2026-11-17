// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/run_controls.hpp"

#include <algorithm>
#include <utility>

namespace vehicle_sil::launch {

const char* phase_name(Phase phase)
{
    switch (phase)
    {
    case Phase::Checking: return "checking";
    case Phase::Starting: return "starting";
    case Phase::Waiting: return "waiting";
    case Phase::Running: return "running";
    case Phase::Stopping: return "stopping";
    case Phase::Ended: return "ended";
    }
    return "ended";
}

const char* pacing_name(Pacing pacing)
{
    switch (pacing)
    {
    case Pacing::NotPaced: return "not-paced";
    case Pacing::OnTime: return "on-time";
    case Pacing::Late: return "late";
    }
    return "not-paced";
}

RunControls::RunControls(StopRequests& stops, bool start_paused, Observers observers, bool scenario) :
  stops_(stops), time_(start_paused, observers.on_command, scenario), on_phase_(std::move(observers.on_phase)),
  on_state_(std::move(observers.on_state)), has_command_observer_(static_cast<bool>(observers.on_command))
{
    // Before any other thread has this object.
    time_.set_on_change([this](const TimeFacts& facts) { tell(facts); });
}

RunControls::RunControls(StopRequests& stops, bool start_paused) : RunControls(stops, start_paused, Observers{}) {}

void RunControls::set_phase(Phase phase)
{
    phase_.store(phase);
    if (on_phase_)
    {
        on_phase_(phase);
    }
    publish();
}

RunState RunControls::compose(const TimeFacts& facts) const
{
    RunState out;
    out.phase = phase_.load();
    out.paused = facts.standing;
    out.holding = facts.holding;
    out.speed = facts.speed;
    // Something to keep up with: the simulation runs and goes, at a speed that is a number. What
    // the watch saw last is older than these facts otherwise.
    const bool paced = out.phase == Phase::Running && facts.situation.stage == ControlStage::Attached && !facts.standing &&
                       facts.speed && *facts.speed;
    // The watch may have looked when there was nothing to keep up with: on time until it sees
    // otherwise.
    out.pacing = !paced ? Pacing::NotPaced : pacing_.load() == Pacing::Late ? Pacing::Late : Pacing::OnTime;
    out.stop_requests = std::clamp(stops_.count(), 0, 2);
    if (stops_.first() != 0)
    {
        out.stopped_by = stop_name_ ? stop_name_() : std::string("interrupt");
    }
    out.scenario = facts.situation.scenario;
    const auto ask = [&facts](Operation o, Entry e) { return launch::allowed(facts.situation, e, o); };
    out.allowed.pause = ask(Operation::Pause, Entry::Stdin);
    out.allowed.resume = ask(Operation::Resume, Entry::Stdin);
    out.allowed.step = ask(Operation::Step, Entry::Stdin);
    out.allowed.speed = ask(Operation::Speed, Entry::Stdin);
    out.allowed.stop = ask(Operation::Stop, Entry::Stdin);
    // Standard input has neither: as the entry that has them answers.
    out.allowed.run_until = ask(Operation::RunUntil, Entry::ControlServer);
    out.allowed.write = ask(Operation::Write, Entry::ControlServer);
    return out;
}

void RunControls::tell(const TimeFacts& facts)
{
    RunState now = compose(facts);
    now.rev = told_.rev;
    if (told_.rev != 0 && now == told_)
    {
        return;
    }
    ++now.rev;
    told_ = std::move(now);
    if (on_state_)
    {
        on_state_(told_);
    }
}

void RunControls::publish()
{
    time_.look([this](const TimeFacts& facts) { tell(facts); });
}

RunState RunControls::state()
{
    RunState out;
    time_.look([this, &out](const TimeFacts& facts) {
        out = compose(facts);
        out.rev = told_.rev;
    });
    return out;
}

} // namespace vehicle_sil::launch
