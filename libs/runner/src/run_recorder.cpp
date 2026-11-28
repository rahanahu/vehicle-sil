// SPDX-License-Identifier: MIT
#include "run_recorder.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <chrono>
#include <string>

namespace vehicle_sil::runner::detail {

namespace {

namespace orch = SilKit::Services::Orchestration;

bool enum_items_match(const std::vector<control::EnumItem>& a, const std::vector<control::EnumItem>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].name != b[i].name || a[i].value != b[i].value)
        {
            return false;
        }
    }
    return true;
}

} // namespace

void RunRecorder::begin(control_silkit::HubTransport& transport)
{
    if (!active())
    {
        return;
    }
    record_ = std::make_shared<scenario::TimelineRecord>(ctx_.plan.recorded_variables);
    if (ctx_.plan.record_events)
    {
        record_->enable_events();
    }
    const std::size_t participant_count = ctx_.config.participants.size();
    bindings_.assign(participant_count, {});
    std::vector<std::string> incarnations(participant_count);

    const std::vector<control::PeerInfo> peers = control_.hub().peers();
    const std::vector<control_silkit::LinkInfo> links = transport.links();
    const auto& recorded = record_->extra_variables();

    for (std::size_t i = 0; i < recorded.size(); ++i)
    {
        const scenario::RecordedVariable& rv = recorded[i];
        std::optional<std::size_t> place;
        for (std::size_t p = 0; p < participant_count; ++p)
        {
            if (ctx_.config.participants[p].name == rv.participant)
            {
                place = p;
                break;
            }
        }
        if (!place)
        {
            continue;
        }
        const auto peer = std::find_if(peers.begin(), peers.end(), [&rv](const control::PeerInfo& info) {
            return info.participant == rv.participant && info.connected;
        });
        const auto link = std::find_if(links.begin(), links.end(), [&rv](const control_silkit::LinkInfo& info) {
            return info.participant == rv.participant && info.state == control_silkit::LinkState::Connected && info.ready;
        });
        if (peer == peers.end() || link == links.end())
        {
            continue;
        }
        const auto var = std::find_if(peer->schema.begin(), peer->schema.end(), [&rv](const control::VariableInfo& info) {
            return info.name == rv.name;
        });
        if (var == peer->schema.end() || var->type != rv.type ||
            (rv.type == control::ValueType::Enum && !enum_items_match(var->items, rv.items)))
        {
            continue;
        }
        incarnations[*place] = peer->incarnation;
        bindings_[*place].push_back(Binding{i, static_cast<std::size_t>(var - peer->schema.begin())});
    }

    samples_.recording_begins(std::move(incarnations));
    if (ctx_.plan.record_events)
    {
        samples_.events_begins();
    }
}

void RunRecorder::consume_events(std::vector<RunSamples::EventBatch> batches)
{
    if (!record_ || !record_->events_enabled())
    {
        return;
    }
    for (auto& batch : batches)
    {
        if (batch.agent_dropped > 0)
        {
            record_->note_agent_events_dropped(batch.participant, batch.agent_dropped);
        }
        for (auto& ev : batch.events)
        {
            const SimTime t = ev.time;
            record_->event(batch.participant, t, ev.level, std::move(ev.text));
            last_seen_ = last_seen_ ? std::max(*last_seen_, t) : t;
        }
    }
}

void RunRecorder::consume(std::vector<std::pair<std::size_t, std::shared_ptr<const control::Snapshot>>> taken)
{
    if (!record_)
    {
        return;
    }
    for (const auto& [participant, sample] : taken)
    {
        if (!sample || participant >= bindings_.size())
        {
            continue;
        }
        const std::vector<Binding>& list = bindings_[participant];
        if (list.empty())
        {
            continue;
        }
        bool recorded_any = false;
        for (const Binding& b : list)
        {
            if (b.value_index < sample->values.size())
            {
                record_->sampled(b.timeline_index, sample->time, sample->values[b.value_index]);
                recorded_any = true;
            }
        }
        if (recorded_any)
        {
            last_seen_ = last_seen_ ? std::max(*last_seen_, sample->time) : sample->time;
        }
    }
}

void RunRecorder::drain()
{
    if (!record_)
    {
        return;
    }
    consume(samples_.take());
    if (record_->events_enabled())
    {
        consume_events(samples_.take_events());
    }
}

void RunRecorder::finish(SimulationController& controller, const WatchResult& watched, RunOutcome& outcome)
{
    if (!active())
    {
        return;
    }
    auto remaining = samples_.take_and_end();
    auto remaining_events =
        ctx_.plan.record_events ? samples_.take_events_and_end() : std::vector<RunSamples::EventBatch>{};
    if (!record_)
    {
        return;
    }
    consume(std::move(remaining));
    consume_events(std::move(remaining_events));
    std::optional<SimTime> to = last_seen_;
    if (watched.end == WatchEnd::Ended && watched.state == orch::ParticipantState::Shutdown &&
        ctx_.plan.stop_at_ms && controller.wall_time_to_stop_at().has_value())
    {
        to = SimTime{} + std::chrono::milliseconds(*ctx_.plan.stop_at_ms);
    }
    record_->close(to);
    outcome.scenario.timeline_record = record_;
}

void RunRecorder::end()
{
    if (active())
    {
        samples_.recording_ends();
        samples_.events_ends();
    }
}

} // namespace vehicle_sil::runner::detail
