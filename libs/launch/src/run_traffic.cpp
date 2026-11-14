// SPDX-License-Identifier: MIT
#include "run_traffic.hpp"

#include <algorithm>
#include <chrono>
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
    case TrafficNotCounted::QueueFull: return "queue-full";
    case TrafficNotCounted::NoSample: return "no-sample";
    }
    return "";
}

std::size_t TrafficNetwork::counted() const
{
    return static_cast<std::size_t>(
        std::count_if(participants.begin(), participants.end(), [](const TrafficParticipant& p) { return p.counted(); }));
}

std::size_t TrafficNetwork::received() const
{
    return static_cast<std::size_t>(std::count_if(participants.begin(), participants.end(), [](const TrafficParticipant& p) {
        return p.counted() && p.frames_received.value_or(0) > 0;
    }));
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

TrafficCount::TrafficCount(TrafficPlan plan) : plan_(std::move(plan)), counted_(plan_.participants.size()) {}

void TrafficCount::hello(std::size_t i, TrafficSchema schema)
{
    counted_[i].hello = true;
    counted_[i].schema = std::move(schema);
}

void TrafficCount::restarted(std::size_t i)
{
    counted_[i].restarted = true;
}

void TrafficCount::dropped(std::size_t i)
{
    counted_[i].dropped = true;
}

void TrafficCount::sample(std::size_t i, std::uint64_t incarnation, std::shared_ptr<const control::Snapshot> sample)
{
    Counted& c = counted_[i];
    if (c.incarnation != incarnation)
    {
        // Another process: its counters began again, and so does the count.
        c.restarted = c.restarted || c.incarnation != 0;
        c.incarnation = incarnation;
        c.settled.reset();
        c.pending.clear();
    }
    if (c.restarted || c.dropped)
    {
        return; // not counted whatever comes: nothing is kept
    }
    c.pending.push_back(std::move(sample));
}

void TrafficCount::settle(SimTime now)
{
    for (Counted& c : counted_)
    {
        // In the order taken, which is the order of the steps: the last one before `now` stays.
        while (!c.pending.empty() && c.pending.front()->time < now)
        {
            c.settled = std::move(c.pending.front());
            c.pending.pop_front();
        }
        if (c.pending.size() > kMaxPending)
        {
            // So far ahead of the controller: not a participant whose steps the run's time orders.
            c.dropped = true;
            c.pending.clear();
        }
    }
}

TrafficParticipant TrafficCount::row(std::size_t i, const std::string& network) const
{
    const Counted& c = counted_[i];
    TrafficParticipant out;
    out.name = plan_.participants[i].name;
    if (c.hello)
    {
        out.virtual_time = c.schema.virtual_time;
    }
    const auto columns = std::find_if(c.schema.counters.begin(), c.schema.counters.end(),
                                      [&network](const TrafficColumns& n) { return n.network == network; });
    const bool has = columns != c.schema.counters.end() && columns->complete();
    const std::optional<std::int64_t> sent = has && c.settled ? counter(*c.settled, columns->sent) : std::nullopt;
    const std::optional<std::int64_t> received = has && c.settled ? counter(*c.settled, columns->received) : std::nullopt;
    out.not_counted = c.restarted                              ? TrafficNotCounted::Restarted
                      : !c.hello                               ? TrafficNotCounted::NoControlLink
                      : !c.schema.virtual_time                 ? TrafficNotCounted::RealTime
                      : !has                                   ? TrafficNotCounted::NoCounters
                      : columns->mapped                        ? TrafficNotCounted::Mapped
                      : c.dropped                              ? TrafficNotCounted::QueueFull
                      : !c.settled || !sent || !received       ? TrafficNotCounted::NoSample
                                                               : TrafficNotCounted::None;
    if (out.counted())
    {
        out.at = c.settled->time;
        out.frames_sent = sent;
        out.frames_received = received;
    }
    return out;
}

RunTraffic TrafficCount::finish(std::int64_t end_ms)
{
    settle(SimTime{std::chrono::milliseconds(end_ms)});
    for (Counted& c : counted_)
    {
        // Of steps from the end on: they came, or did not, before the run was over.
        c.pending.clear();
    }
    RunTraffic out;
    out.before_ms = end_ms;
    for (const TrafficPlan::Network& network : plan_.buses)
    {
        TrafficNetwork counted{network.name, network.kind, {}};
        for (std::size_t i = 0; i < plan_.participants.size(); ++i)
        {
            const TrafficPlan::Participant& p = plan_.participants[i];
            const Counted& c = counted_[i];
            // --describe says where it is; without it, the counters its agent has.
            const bool on = p.in_run &&
                            (p.described ? std::find(p.on.begin(), p.on.end(), network.name) != p.on.end()
                                         : c.hello && std::any_of(c.schema.counters.begin(), c.schema.counters.end(),
                                                                  [&network](const TrafficColumns& n) {
                                                                      return n.network == network.name;
                                                                  }));
            if (on)
            {
                counted.participants.push_back(row(i, network.name));
            }
        }
        if (!counted.participants.empty())
        {
            out.per_network.push_back(std::move(counted));
        }
    }
    return out;
}

} // namespace detail

} // namespace vehicle_sil::launch
