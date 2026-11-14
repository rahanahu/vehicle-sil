// SPDX-License-Identifier: MIT
#include "run_traffic.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>

namespace vehicle_sil::launch {

const char* traffic_not_counted_name(TrafficNotCounted why)
{
    switch (why)
    {
    case TrafficNotCounted::None: return "";
    case TrafficNotCounted::NoControlLink: return "no-control-link";
    case TrafficNotCounted::Restarted: return "restarted";
    case TrafficNotCounted::RealTime: return "real-time";
    case TrafficNotCounted::NoCounters: return "no-counters";
    case TrafficNotCounted::Mapped: return "mapped";
    case TrafficNotCounted::NoSample: return "no-sample";
    }
    return "";
}

std::size_t TrafficNetwork::counted() const
{
    return static_cast<std::size_t>(
        std::count_if(participants.begin(), participants.end(), [](const TrafficParticipant& p) { return p.counted(); }));
}

std::size_t TrafficNetwork::senders() const
{
    return static_cast<std::size_t>(std::count_if(participants.begin(), participants.end(), [](const TrafficParticipant& p) {
        return p.counted() && p.frames_sent.value_or(0) > 0;
    }));
}

std::size_t TrafficNetwork::receivers() const
{
    return static_cast<std::size_t>(std::count_if(participants.begin(), participants.end(), [](const TrafficParticipant& p) {
        return p.counted() && p.frames_received.value_or(0) > 0;
    }));
}

std::optional<bool> TrafficNetwork::delivered() const
{
    const TrafficParticipant* sender = nullptr;
    const TrafficParticipant* receiver = nullptr;
    const std::size_t sent = senders();
    const std::size_t got = receivers();
    for (const TrafficParticipant& p : participants)
    {
        sender = p.counted() && p.frames_sent.value_or(0) > 0 ? &p : sender;
        receiver = p.counted() && p.frames_received.value_or(0) > 0 ? &p : receiver;
    }
    if (sent == 0)
    {
        return std::nullopt; // nobody counted, or nobody sent
    }
    // A participant's own frames are not in its frames_received: a receiver that is not the one
    // sender got a frame of someone else.
    if (got > 1 || (got == 1 && (sent > 1 || sender != receiver)))
    {
        return true;
    }
    // Nobody else that is counted received. One that is not counted may have.
    return counted() < participants.size() ? std::nullopt : std::optional<bool>(false);
}

namespace detail {

namespace {

constexpr std::string_view kPrefix = "j1939.";
constexpr std::string_view kSent = ".frames_sent";
constexpr std::string_view kReceived = ".frames_received";

/// The N of "j1939.<N><field>", or empty if `name` is not of that form.
std::string_view counter_network(std::string_view name, std::string_view field)
{
    if (name.size() <= kPrefix.size() + field.size() || name.substr(0, kPrefix.size()) != kPrefix ||
        name.substr(name.size() - field.size()) != field)
    {
        return {};
    }
    return name.substr(kPrefix.size(), name.size() - kPrefix.size() - field.size());
}

std::optional<std::int64_t> counter(const control::Snapshot& sample, const std::optional<std::size_t>& index)
{
    if (!index || *index >= sample.values.size())
    {
        return std::nullopt;
    }
    const std::int64_t* value = std::get_if<std::int64_t>(&sample.values[*index]);
    return value != nullptr ? std::optional<std::int64_t>(*value) : std::nullopt;
}

} // namespace

TrafficSchema traffic_schema(const control::PeerInfo& peer)
{
    TrafficSchema out;
    out.virtual_time = peer.virtual_time;
    for (std::size_t i = 0; i < peer.schema.size(); ++i)
    {
        const control::VariableInfo& variable = peer.schema[i];
        // With a variable map, the name the agent announced is beside the one the clients see.
        const bool mapped = i < peer.sources.size() && !peer.sources[i].empty();
        const std::string_view announced = mapped ? std::string_view(peer.sources[i]) : std::string_view(variable.name);
        const std::string_view sent = counter_network(announced, kSent);
        const std::string_view received = counter_network(announced, kReceived);
        const std::string_view network = !sent.empty() ? sent : received;
        if (network.empty() || variable.type != control::ValueType::Integer)
        {
            continue;
        }
        auto columns = std::find_if(out.counters.begin(), out.counters.end(),
                                    [network](const TrafficColumns& c) { return c.network == network; });
        if (columns == out.counters.end())
        {
            out.counters.push_back(TrafficColumns{std::string(network), {}, {}, false});
            columns = out.counters.end() - 1;
        }
        (!sent.empty() ? columns->sent : columns->received) = i;
        columns->mapped = columns->mapped || mapped;
    }
    return out;
}

TrafficPlan traffic_plan(const SimulationConfig& config, const CheckReport& check)
{
    TrafficPlan plan;
    for (const NetworkConfig& network : config.networks)
    {
        if (network.valid)
        {
            plan.buses.push_back({network.name, to_string(network.kind)});
        }
    }
    for (std::size_t i = 0; i < config.participants.size(); ++i)
    {
        const ParticipantConfig& p = config.participants[i];
        TrafficPlan::Participant who;
        who.name = p.name;
        who.in_run = p.enabled;
        if (p.describe && i < check.participants.size() && check.participants[i].ecu)
        {
            who.described = true;
            for (const SeenNetwork& seen : seen_networks(p, *check.participants[i].ecu))
            {
                who.on.push_back(seen.network.name);
            }
        }
        plan.participants.push_back(std::move(who));
    }
    return plan;
}

namespace {

std::int64_t step_of(SimTime time, SimDuration step)
{
    return to_ns(time) / step.count();
}

bool later(const std::shared_ptr<const control::Snapshot>& a, const std::shared_ptr<const control::Snapshot>& than)
{
    return !than || a->time > than->time || (a->time == than->time && a->sequence >= than->sequence);
}

} // namespace

void TrafficLatest::put(std::shared_ptr<const control::Snapshot> sample, SimTime now, SimDuration step)
{
    if (off_time_)
    {
        return;
    }
    const std::int64_t running = step_of(now, step);
    // The controller's steps before the one that runs are complete: of their samples the last stays.
    std::size_t done = 0;
    while (done < ahead_.size() && ahead_[done].first < running)
    {
        if (later(ahead_[done].second, settled_))
        {
            settled_ = std::move(ahead_[done].second);
        }
        ++done;
    }
    ahead_.erase(ahead_.begin(), ahead_.begin() + static_cast<std::ptrdiff_t>(done));
    const std::int64_t at = step_of(sample->time, step);
    if (at < running)
    {
        if (later(sample, settled_))
        {
            settled_ = std::move(sample);
        }
        return;
    }
    const auto place = std::find_if(ahead_.begin(), ahead_.end(), [at](const auto& kept) { return kept.first >= at; });
    if (place != ahead_.end() && place->first == at)
    {
        if (later(sample, place->second))
        {
            place->second = std::move(sample);
        }
        return;
    }
    ahead_.insert(place, {at, std::move(sample)});
    if (ahead_.size() > kMaxAhead)
    {
        off_time_ = true;
        ahead_.clear();
        settled_.reset();
    }
}

void TrafficLatest::clear()
{
    settled_.reset();
    ahead_.clear();
    off_time_ = false;
}

std::shared_ptr<const control::Snapshot> TrafficLatest::last_before(SimTime end, SimDuration step) const
{
    std::shared_ptr<const control::Snapshot> last = settled_ && settled_->time < end ? settled_ : nullptr;
    // Whole steps of the controller only: of a step that `end` is inside (it never is), an
    // earlier sample than the one kept may have been the answer.
    const std::int64_t ending = step_of(end, step);
    for (const auto& [at, sample] : ahead_)
    {
        if (at < ending && sample->time < end && later(sample, last))
        {
            last = sample;
        }
    }
    return last;
}

namespace {

TrafficParticipant row(const std::string& name, const TrafficSeen& c, const std::string& network)
{
    TrafficParticipant out;
    out.name = name;
    if (c.hello)
    {
        out.virtual_time = c.schema.virtual_time;
    }
    const auto columns = std::find_if(c.schema.counters.begin(), c.schema.counters.end(),
                                      [&network](const TrafficColumns& n) { return n.network == network; });
    const bool has = columns != c.schema.counters.end() && columns->complete();
    const std::optional<std::int64_t> sent = has && c.last ? counter(*c.last, columns->sent) : std::nullopt;
    const std::optional<std::int64_t> received = has && c.last ? counter(*c.last, columns->received) : std::nullopt;
    out.not_counted = c.restarted                              ? TrafficNotCounted::Restarted
                      : !c.hello                               ? TrafficNotCounted::NoControlLink
                      : !c.schema.virtual_time || c.off_time   ? TrafficNotCounted::RealTime
                      : !has                                   ? TrafficNotCounted::NoCounters
                      : columns->mapped                        ? TrafficNotCounted::Mapped
                      : !c.last || !sent || !received          ? TrafficNotCounted::NoSample
                                                               : TrafficNotCounted::None;
    if (out.counted())
    {
        out.at = c.last->time;
        out.frames_sent = sent;
        out.frames_received = received;
    }
    return out;
}

} // namespace

RunTraffic count_traffic(const TrafficPlan& plan, const std::vector<TrafficSeen>& seen, std::int64_t end_ms)
{
    RunTraffic out;
    out.before_ms = end_ms;
    for (const TrafficPlan::Network& network : plan.buses)
    {
        TrafficNetwork counted{network.name, network.kind, {}};
        for (std::size_t i = 0; i < plan.participants.size(); ++i)
        {
            const TrafficPlan::Participant& p = plan.participants[i];
            const TrafficSeen& c = seen[i];
            // --describe says where it is; without it, the counters its agent has.
            const bool on = p.in_run &&
                            (p.described ? std::find(p.on.begin(), p.on.end(), network.name) != p.on.end()
                                         : c.hello && std::any_of(c.schema.counters.begin(), c.schema.counters.end(),
                                                                  [&network](const TrafficColumns& n) {
                                                                      return n.network == network.name;
                                                                  }));
            if (on)
            {
                counted.participants.push_back(row(p.name, c, network.name));
            }
        }
        // Also one that nobody is known to be on: it is in the file, and frames may go over it.
        out.per_network.push_back(std::move(counted));
    }
    return out;
}

} // namespace detail

} // namespace vehicle_sil::launch
