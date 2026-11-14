// SPDX-License-Identifier: MIT
#include "run_samples.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace vehicle_sil::launch::detail {

RunSamples::RunSamples(std::vector<std::string> names, TrafficPlan plan) :
  names_(std::move(names)), known_(names_.size()), count_(std::move(plan))
{
    queue_.incarnations.resize(names_.size());
    queue_.dropped.assign(names_.size(), false);
}

RunSamples::~RunSamples()
{
    untap();
}

void RunSamples::tap(control::Hub& hub)
{
    {
        const std::lock_guard lock(take_mutex_);
        hub_ = &hub;
    }
    tapped_ = true;
    hub.set_values_tap([this](const std::string& participant, const std::string& incarnation,
                              const std::shared_ptr<const control::Snapshot>& sample) {
        const std::lock_guard lock(queue_mutex_);
        for (std::size_t i = 0; i < names_.size(); ++i)
        {
            if (names_[i] != participant)
            {
                continue;
            }
            std::vector<std::string>& seen = queue_.incarnations[i];
            if (seen.empty() || seen.back() != incarnation)
            {
                seen.push_back(incarnation);
            }
            if (queue_.samples.size() >= kMaxQueued || queue_.values + sample->values.size() > kMaxQueuedValues)
            {
                queue_.dropped[i] = true;
                return;
            }
            queue_.samples.push_back(Queued{i, seen.size(), sample});
            queue_.values += sample->values.size();
            ++queue_.put;
            return;
        }
    });
}

void RunSamples::untap()
{
    if (tapped_)
    {
        tapped_ = false;
        hub_->set_values_tap({});
    }
}

void RunSamples::scenario_begins(Sink sink)
{
    const std::lock_guard lock(take_mutex_);
    sink_ = std::move(sink);
    const std::lock_guard queue(queue_mutex_);
    from_ = queue_.put;
}

void RunSamples::scenario_ends()
{
    const std::lock_guard lock(take_mutex_);
    sink_ = {};
}

void RunSamples::take(SimTime now)
{
    const std::lock_guard lock(take_mutex_);
    take_locked(now);
}

void RunSamples::take_locked(SimTime now)
{
    if (finished_ || hub_ == nullptr)
    {
        return;
    }
    std::vector<Queued> batch;
    std::uint64_t first = 0;
    std::vector<std::size_t> fresh;   // participants of which a new process was seen
    std::vector<std::size_t> dropped; // participants of which a sample was dropped
    {
        const std::lock_guard queue(queue_mutex_);
        batch.swap(queue_.samples);
        queue_.values = 0;
        first = queue_.put - batch.size();
        for (std::size_t i = 0; i < names_.size(); ++i)
        {
            const std::vector<std::string>& seen = queue_.incarnations[i];
            if (seen.size() > known_[i].size())
            {
                known_[i].insert(known_[i].end(), seen.begin() + static_cast<std::ptrdiff_t>(known_[i].size()), seen.end());
                fresh.push_back(i);
            }
            if (queue_.dropped[i])
            {
                queue_.dropped[i] = false;
                dropped.push_back(i);
            }
        }
    }
    if (!fresh.empty())
    {
        // What the hello of the new process says. The hub cannot be asked inside the tap.
        const std::vector<control::PeerInfo> peers = hub_->peers();
        for (const std::size_t i : fresh)
        {
            const auto peer = std::find_if(peers.begin(), peers.end(),
                                           [this, i](const control::PeerInfo& info) { return info.participant == names_[i]; });
            if (peer != peers.end() && peer->incarnation == known_[i].back())
            {
                count_.hello(i, traffic_schema(*peer));
            }
            else
            {
                // The hub has the hello of a later process already: these samples are not its.
                count_.restarted(i);
            }
        }
    }
    for (std::size_t k = 0; k < batch.size(); ++k)
    {
        Queued& q = batch[k];
        if (sink_ && first + k >= from_)
        {
            sink_(q.participant, known_[q.participant][q.incarnation - 1], q.sample);
        }
        count_.sample(q.participant, q.incarnation, std::move(q.sample));
    }
    for (const std::size_t i : dropped)
    {
        count_.dropped(i);
    }
    count_.settle(now);
}

RunTraffic RunSamples::finish(std::int64_t end_ms)
{
    RunTraffic traffic;
    {
        const std::lock_guard lock(take_mutex_);
        take_locked(SimTime{std::chrono::milliseconds(end_ms)});
        if (hub_ != nullptr && !finished_)
        {
            // Those of which no sample came: whether a control agent said hello at all, and what
            // it has.
            for (const control::PeerInfo& peer : hub_->peers())
            {
                for (std::size_t i = 0; i < names_.size(); ++i)
                {
                    if (names_[i] == peer.participant && known_[i].empty())
                    {
                        count_.hello(i, traffic_schema(peer));
                    }
                }
            }
        }
        finished_ = true;
        traffic = count_.finish(end_ms);
    }
    untap();
    return traffic;
}

} // namespace vehicle_sil::launch::detail
