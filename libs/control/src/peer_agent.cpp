// SPDX-License-Identifier: MIT
#include "vehicle_sil/control/peer_agent.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vehicle_sil::control {
namespace {

using Wall = std::chrono::steady_clock;

const char* state_name(j1939::AddressState state)
{
    switch (state)
    {
    case j1939::AddressState::Fixed:
        return "fixed";
    case j1939::AddressState::NotStarted:
        return "not started";
    case j1939::AddressState::Claiming:
        return "claiming";
    case j1939::AddressState::Claimed:
        return "claimed";
    case j1939::AddressState::CannotClaim:
        return "cannot claim";
    }
    return "unknown";
}

/// What the monitoring keeps between steps. Used on the step thread only.
struct Monitor
{
    struct NodeWatch
    {
        std::string name;
        j1939::Node* node = nullptr;
        j1939::AddressState state{};
        j1939::Address address{};
    };

    std::vector<NodeWatch> nodes;
    std::uint64_t steps = 0;
    SimTime now{};
    Wall::time_point app_begin{};
    double app_us_max = 0.0;
    std::optional<std::pair<SimTime, Wall::time_point>> speed_ref;

    void after_step(SimTime t, Agent& agent)
    {
        app_us_max = std::max(app_us_max, std::chrono::duration<double, std::micro>(Wall::now() - app_begin).count());
        ++steps;
        now = t;
        for (auto& w : nodes)
        {
            const auto state = w.node->address_state();
            const auto address = w.node->address();
            if (state != w.state || address != w.address)
            {
                w.state = state;
                w.address = address;
                agent.log(state == j1939::AddressState::CannotClaim ? EventLevel::Warning : EventLevel::Info,
                          w.name + ": " + state_name(state) + " at address " + std::to_string(address));
            }
        }
    }

    /// Simulation seconds per wall-clock second since the previous sample.
    double speed()
    {
        const auto wall = Wall::now();
        double result = 0.0;
        if (speed_ref && wall > speed_ref->second)
        {
            result = std::chrono::duration<double>(now - speed_ref->first).count()
                     / std::chrono::duration<double>(wall - speed_ref->second).count();
        }
        speed_ref = std::make_pair(now, wall);
        return result;
    }
};

/// The names add_monitoring() would register (to check them before anything changes).
std::vector<std::string> monitoring_names(Peer& peer)
{
    std::vector<std::string> names{"peer.steps", "peer.speed", "peer.app_time_max_us"};
    for (const auto& name : peer.network_names())
    {
        const std::string prefix = "j1939." + name + ".";
        for (const char* field : {"bus_state", "frames_received", "frames_sent", "send_failures", "send_queue_full",
                                  "receive_overflows", "held_dropped", "address_violations", "transport_aborts_sent",
                                  "transport_aborts_received"})
        {
            names.push_back(prefix + field);
        }
        for (std::size_t i = 0; i < peer.network(name).node_count(); ++i)
        {
            names.push_back(prefix + "node" + std::to_string(i) + ".address");
            names.push_back(prefix + "node" + std::to_string(i) + ".address_state");
        }
    }
    return names;
}

/// Registers the outputs that monitoring_names() lists.
void add_monitoring(Peer& peer, Agent& agent, const std::shared_ptr<Monitor>& m)
{
    agent.output("peer.steps", [m] { return m->steps; }, {.description = "Steps run"});
    agent.output("peer.speed", [m] { return m->speed(); },
                 {.unit = "s/s", .description = "Simulation seconds per wall-clock second since the previous sample"});
    agent.output("peer.app_time_max_us", [m] { return std::exchange(m->app_us_max, 0.0); },
                 {.unit = "us", .description = "Longest application and LIN stage of a step since the previous sample"});
    for (const auto& name : peer.network_names())
    {
        auto& network = peer.network(name);
        const std::string prefix = "j1939." + name + ".";
        agent.output_enum(prefix + "bus_state",
                          [&network] { return network.bus_state() ? static_cast<int>(*network.bus_state()) : -1; },
                          {{"unknown", -1}, {"error_active", 0}, {"error_passive", 1}, {"bus_off", 2}},
                          {.description = "Error state of the CAN controller"});
        const auto counter = [&](const char* field, std::uint64_t j1939::NetworkStatistics::*member) {
            agent.output(prefix + field, [&network, member] { return network.statistics().*member; });
        };
        counter("frames_received", &j1939::NetworkStatistics::frames_received);
        counter("frames_sent", &j1939::NetworkStatistics::frames_sent);
        counter("send_failures", &j1939::NetworkStatistics::send_failures);
        counter("send_queue_full", &j1939::NetworkStatistics::send_queue_full);
        counter("receive_overflows", &j1939::NetworkStatistics::receive_overflows);
        counter("held_dropped", &j1939::NetworkStatistics::held_dropped);
        counter("address_violations", &j1939::NetworkStatistics::address_violations);
        counter("transport_aborts_sent", &j1939::NetworkStatistics::transport_aborts_sent);
        counter("transport_aborts_received", &j1939::NetworkStatistics::transport_aborts_received);
        for (std::size_t i = 0; i < network.node_count(); ++i)
        {
            auto& node = network.node(i);
            const std::string node_prefix = prefix + "node" + std::to_string(i);
            m->nodes.push_back({node_prefix, &node, node.address_state(), node.address()});
            agent.output(node_prefix + ".address", [&node] { return static_cast<int>(node.address()); });
            agent.output_enum(node_prefix + ".address_state", [&node] { return static_cast<int>(node.address_state()); },
                              {{"fixed", 0}, {"not_started", 1}, {"claiming", 2}, {"claimed", 3}, {"cannot_claim", 4}});
        }
    }
}

} // namespace

void attach(Peer& peer, Agent& agent, AttachOptions options)
{
    std::function<void()> wakeup;
    if (!peer.uses_virtual_time())
    {
        auto pending = std::make_shared<std::atomic<bool>>(false);
        wakeup = [pending, &host = peer.host()] {
            if (!pending->exchange(true, std::memory_order_acq_rel))
            {
                host.post([pending] { pending->store(false, std::memory_order_release); });
            }
        };
    }
    std::shared_ptr<Monitor> monitor;
    if (options.monitor)
    {
        // Checked before anything changes: the outputs must not clash with the agent's variables.
        for (const auto& name : monitoring_names(peer))
        {
            for (const auto& info : agent.schema())
            {
                if (info.name == name)
                {
                    throw std::invalid_argument("vehicle_sil::control::attach(): the agent already has a variable '" + name
                                                + "' (use AttachOptions::monitor = false or another name)");
                }
            }
        }
        monitor = std::make_shared<Monitor>();
    }
    agent.bind_driver(std::move(wakeup)); // checks the agent first: nothing is changed if it throws
    try
    {
        peer.set_control(
            [&agent, monitor](SimTime now) {
                agent.on_step_begin(now);
                if (monitor)
                {
                    monitor->app_begin = Wall::now();
                }
            },
            [&agent, monitor](SimTime now) {
                if (monitor)
                {
                    monitor->after_step(now, agent);
                }
                agent.on_step_end(now);
            },
            [&agent] { agent.clear_wakeup(); });
    }
    catch (...)
    {
        agent.unbind_driver();
        throw;
    }
    if (monitor)
    {
        add_monitoring(peer, agent, monitor); // the names were checked; registration is still open
    }
}

} // namespace vehicle_sil::control
