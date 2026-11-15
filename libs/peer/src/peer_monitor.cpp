// SPDX-License-Identifier: MIT
#include "peer_monitor.hpp"

#include "vehicle_sil/ecu/monitor_names.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::detail {
namespace {

using control::Agent;
using control::EventLevel;
namespace names = monitor_names;

/// The counters of a J1939 network, in the order of the outputs.
constexpr std::pair<std::string_view, std::uint64_t j1939::NetworkStatistics::*> kCounters[] = {
    {names::kFramesReceived, &j1939::NetworkStatistics::frames_received},
    {names::kFramesSent, &j1939::NetworkStatistics::frames_sent},
    {names::kSendFailures, &j1939::NetworkStatistics::send_failures},
    {names::kSendQueueFull, &j1939::NetworkStatistics::send_queue_full},
    {names::kReceiveOverflows, &j1939::NetworkStatistics::receive_overflows},
    {names::kHeldDropped, &j1939::NetworkStatistics::held_dropped},
    {names::kAddressViolations, &j1939::NetworkStatistics::address_violations},
    {names::kTransportAbortsSent, &j1939::NetworkStatistics::transport_aborts_sent},
    {names::kTransportAbortsReceived, &j1939::NetworkStatistics::transport_aborts_received},
    {names::kTransportTimeouts, &j1939::NetworkStatistics::transport_timeouts},
    {names::kTransportRejected, &j1939::NetworkStatistics::transport_rejected},
    {names::kTransportBamDropped, &j1939::NetworkStatistics::transport_bam_dropped},
    {names::kTransportBamReplaced, &j1939::NetworkStatistics::transport_bam_replaced},
};

// The enum outputs use the engine's values: keep them in step.
static_assert(static_cast<int>(j1939::AddressState::Fixed) == 0 && static_cast<int>(j1939::AddressState::NotStarted) == 1
              && static_cast<int>(j1939::AddressState::Claiming) == 2 && static_cast<int>(j1939::AddressState::Claimed) == 3
              && static_cast<int>(j1939::AddressState::CannotClaim) == 4);
static_assert(static_cast<int>(can::BusState::ErrorActive) == 0 && static_cast<int>(can::BusState::ErrorPassive) == 1
              && static_cast<int>(can::BusState::BusOff) == 2);

/// The name of a state, as in the items of the address_state output.
const char* state_name(j1939::AddressState state)
{
    switch (state)
    {
    case j1939::AddressState::Fixed:
        return "fixed";
    case j1939::AddressState::NotStarted:
        return "not_started";
    case j1939::AddressState::Claiming:
        return "claiming";
    case j1939::AddressState::Claimed:
        return "claimed";
    case j1939::AddressState::CannotClaim:
        return "cannot_claim";
    }
    return "unknown";
}

/// The names add_monitoring() would register (to check them before anything changes).
std::vector<std::string> monitoring_names(Peer& peer)
{
    std::vector<std::string> out{std::string(names::kPeerSteps), std::string(names::kPeerSpeed),
                                 std::string(names::kPeerAppTimeMaxUs)};
    for (const auto& name : peer.network_names())
    {
        // Looked up by the code's name, named by the simulation's (PeerOptions::network_names).
        const std::string& network = simulation_network_name(peer, name);
        const std::string prefix = names::j1939_prefix(network);
        out.push_back(prefix + std::string(names::kBusState));
        for (const auto& [field, member] : kCounters)
        {
            out.push_back(prefix + std::string(field));
        }
        for (std::size_t i = 0; i < peer.network(name).node_count(); ++i)
        {
            const std::string node_prefix = names::j1939_node_prefix(network, i) + ".";
            out.push_back(node_prefix + std::string(names::kNodeAddress));
            out.push_back(node_prefix + std::string(names::kNodeAddressState));
        }
    }
    return out;
}

/// Registers the outputs that monitoring_names() lists.
void add_monitoring(Peer& peer, Agent& agent, const std::shared_ptr<PeerMonitor>& m)
{
    agent.output(std::string(names::kPeerSteps), [m] { return m->steps; }, {.description = "Steps run"});
    agent.output(std::string(names::kPeerSpeed), [m] { return m->speed(); },
                 {.unit = "s/s",
                  .description = "Simulation seconds per wall-clock second since the previous sample (0 at the first sample; "
                                 "includes the time the simulation stood still)"});
    agent.output(std::string(names::kPeerAppTimeMaxUs), [m] { return std::exchange(m->app_us_max, 0.0); },
                 {.unit = "us",
                  .description = "Longest application and LIN stage of a step since the previous sample (without the "
                                 "J1939 handling and the commands applied before it)"});
    for (const auto& name : peer.network_names())
    {
        auto& network = peer.network(name);
        // As in monitoring_names(): also the events of the nodes (PeerMonitor::after_step()).
        const std::string& simulation = simulation_network_name(peer, name);
        const std::string prefix = names::j1939_prefix(simulation);
        agent.output_enum(prefix + std::string(names::kBusState),
                          [&network] { return network.bus_state() ? static_cast<int>(*network.bus_state()) : -1; },
                          {{"unknown", -1}, {"error_active", 0}, {"error_passive", 1}, {"bus_off", 2}},
                          {.description = "Error state of the CAN controller"});
        for (const auto& [field, member] : kCounters)
        {
            agent.output(prefix + std::string(field), [&network, member] { return network.statistics().*member; });
        }
        for (std::size_t i = 0; i < network.node_count(); ++i)
        {
            auto& node = network.node(i);
            const std::string node_prefix = names::j1939_node_prefix(simulation, i);
            m->nodes.push_back({node_prefix, &node, node.address_state(), node.address()});
            agent.output(node_prefix + "." + std::string(names::kNodeAddress),
                         [&node] { return static_cast<int>(node.address()); });
            agent.output_enum(node_prefix + "." + std::string(names::kNodeAddressState), [&node] { return static_cast<int>(node.address_state()); },
                              {{"fixed", 0}, {"not_started", 1}, {"claiming", 2}, {"claimed", 3}, {"cannot_claim", 4}});
        }
    }
}

} // namespace

void PeerMonitor::after_step(SimTime t, Agent& agent)
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

double PeerMonitor::speed()
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

std::shared_ptr<PeerMonitor> add_peer_monitoring(Peer& peer, Agent& agent, const std::string& who)
{
    // Checked before anything changes: the outputs must not clash with the ECU's variables.
    for (const auto& name : monitoring_names(peer))
    {
        for (const auto& info : agent.schema())
        {
            if (info.name == name)
            {
                std::string renamed;
                for (const auto& network : peer.network_names())
                {
                    const std::string& simulation = simulation_network_name(peer, network);
                    if (simulation != network && name.starts_with(monitor_names::j1939_prefix(simulation)))
                    {
                        renamed = "; the monitoring outputs of the network \"" + network + "\" have its name in the "
                                  "simulation, \"" + simulation + "\" (network_names, --map-network)";
                    }
                }
                throw std::invalid_argument(who + "the ECU declares a variable '" + name
                                            + "', the name of a monitoring output of the peer (rename the variable, or "
                                              "set PeerOptions::monitor = false" + renamed + ")");
            }
        }
    }
    auto monitor = std::make_shared<PeerMonitor>();
    add_monitoring(peer, agent, monitor); // the names were checked; registration is still open
    return monitor;
}

} // namespace vehicle_sil::detail
