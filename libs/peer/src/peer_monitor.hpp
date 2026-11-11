// SPDX-License-Identifier: MIT
//
// The monitoring outputs of a Peer's control agent (PeerOptions::monitor). Internal to
// vehicle_sil::peer.
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::detail {

/// What the monitoring keeps between steps. Used on the step thread only (and by the outputs'
/// getters, which run in the samples on the step thread).
struct PeerMonitor
{
    using Wall = std::chrono::steady_clock;

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

    /// Before the application of a step (after the agent applied the writes).
    void before_step() { app_begin = Wall::now(); }
    /// After the application and the LIN nodes of the step at `t`, before the agent samples:
    /// counts the step, measures it and logs the nodes' address changes.
    void after_step(SimTime t, control::Agent& agent);
    /// Simulation seconds per wall-clock second since the previous sample.
    double speed();
};

/// Adds the monitoring outputs of `peer` (its J1939 networks and their nodes as they are now)
/// to `agent`, after the variables it has. std::invalid_argument (starting with `who`) if a
/// variable has the name of one of them; nothing is added then.
std::shared_ptr<PeerMonitor> add_peer_monitoring(Peer& peer, control::Agent& agent, const std::string& who);

} // namespace vehicle_sil::detail
