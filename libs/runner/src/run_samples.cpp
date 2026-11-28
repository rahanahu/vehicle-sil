// SPDX-License-Identifier: MIT
#include "vehicle_sil/runner/detail/run_samples.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace vehicle_sil::runner::detail {

RunSamples::RunSamples(std::vector<std::string> names, TrafficPlan plan) :
  names_(std::move(names)), plan_(std::move(plan)), kept_(names_.size())
{
}

RunSamples::~RunSamples()
{
    untap();
}

void RunSamples::tap(control::Hub& hub, SimDuration step, Now now, bool tap_events)
{
    hub_ = &hub;
    step_ = step;
    now_ = std::move(now);
    tapped_ = true;
    hub.set_values_tap([this](const std::string& participant, const std::string& incarnation,
                              const std::shared_ptr<const control::Snapshot>& sample) {
        const auto name = std::find(names_.begin(), names_.end(), participant);
        if (name == names_.end())
        {
            return;
        }
        const auto i = static_cast<std::size_t>(name - names_.begin());
        // Read before the lock; the hub's lock, which this runs under, orders the samples.
        const SimTime controller = now_();
        const std::lock_guard lock(mutex_);
        Kept& kept = kept_[i];
        if (kept.incarnation != incarnation)
        {
            // Another process: its counters began again.
            kept.incarnation = incarnation;
            ++kept.incarnations;
            kept.latest.clear();
        }
        kept.latest.put(sample, controller, step_);
        if (!scenario_.empty() && !scenario_[i].empty() && scenario_[i] == incarnation)
        {
            queue_.emplace_back(i, sample);
        }
    });
    if (tap_events)
    {
        events_tapped_ = true;
        hub.set_events_tap([this](const std::string& participant, const std::string& /*incarnation*/,
                                  std::span<const control::Event> events, std::uint64_t agent_dropped) {
            const std::lock_guard lock(mutex_);
            if (!events_recording_)
            {
                return;
            }
            EventBatch batch;
            batch.participant = participant;
            batch.events.assign(events.begin(), events.end());
            batch.agent_dropped = agent_dropped;
            event_queue_.push_back(std::move(batch));
        });
    }
}

void RunSamples::untap()
{
    if (tapped_)
    {
        tapped_ = false;
        hub_->set_values_tap({});
    }
    if (events_tapped_)
    {
        events_tapped_ = false;
        hub_->set_events_tap({});
    }
}

void RunSamples::scenario_begins(std::vector<std::string> incarnations)
{
    incarnations.resize(names_.size());
    const std::lock_guard lock(mutex_);
    scenario_ = std::move(incarnations);
    queue_.clear();
}

void RunSamples::scenario_ends()
{
    const std::lock_guard lock(mutex_);
    scenario_.clear();
    queue_.clear();
    events_recording_ = false;
    event_queue_.clear();
}

void RunSamples::events_begins()
{
    const std::lock_guard lock(mutex_);
    events_recording_ = true;
    event_queue_.clear();
}

void RunSamples::events_ends()
{
    const std::lock_guard lock(mutex_);
    events_recording_ = false;
    event_queue_.clear();
}

std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> RunSamples::take()
{
    std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> taken;
    const std::lock_guard lock(mutex_);
    taken.swap(queue_);
    return taken;
}

std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> RunSamples::take_and_end()
{
    std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> taken;
    const std::lock_guard lock(mutex_);
    taken.swap(queue_);
    scenario_.clear();
    return taken;
}

std::vector<RunSamples::EventBatch> RunSamples::take_events()
{
    std::vector<EventBatch> taken;
    const std::lock_guard lock(mutex_);
    taken.swap(event_queue_);
    return taken;
}

std::vector<RunSamples::EventBatch> RunSamples::take_events_and_end()
{
    std::vector<EventBatch> taken;
    const std::lock_guard lock(mutex_);
    taken.swap(event_queue_);
    events_recording_ = false;
    return taken;
}

RunTraffic RunSamples::finish(std::int64_t end_ms)
{
    const SimTime end = SimTime{std::chrono::milliseconds(end_ms)};
    std::vector<TrafficSeen> seen(names_.size());
    std::vector<std::string> incarnations(names_.size());
    {
        const std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < names_.size(); ++i)
        {
            const Kept& kept = kept_[i];
            seen[i].restarted = kept.incarnations > 1;
            seen[i].off_time = kept.latest.off_time();
            seen[i].last = kept.latest.last_before(end, step_);
            incarnations[i] = kept.incarnation;
        }
    }
    if (hub_ != nullptr)
    {
        // What each control agent's hello says. The hub cannot be asked inside the tap.
        for (const control::PeerInfo& peer : hub_->peers())
        {
            const auto name = std::find(names_.begin(), names_.end(), peer.participant);
            if (name == names_.end())
            {
                continue;
            }
            const auto i = static_cast<std::size_t>(name - names_.begin());
            if (incarnations[i].empty() || incarnations[i] == peer.incarnation)
            {
                seen[i].hello = true;
                seen[i].schema = traffic_schema(peer);
            }
            else
            {
                // The hello is a later process's than the samples: they are not of its schema.
                seen[i].restarted = true;
            }
        }
    }
    RunTraffic traffic = count_traffic(plan_, seen, end_ms);
    untap();
    return traffic;
}

} // namespace vehicle_sil::runner::detail
