// SPDX-License-Identifier: MIT
//
// Connects a control agent to a vehicle_sil::Peer (vehicle_sil::peer_control).
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/peer/peer.hpp"

namespace vehicle_sil::control {

/// Drives `agent` from the steps of `peer`: commands take effect just before the application
/// of a step (Agent::on_step_begin()), values are sampled just after the application and the LIN
/// nodes (Agent::on_step_end()). In real time, a queued command wakes the peer's driver
/// (Host::post(), with at most one wake-up pending), so it takes effect in the next step instead
/// of after up to PeerOptions::max_idle; when the peer stops, the agent stops waking it.
///
/// Once per agent and once per peer, before peer.start() and before the agent's first submit()
/// (std::logic_error otherwise, in both time modes; on an error neither is changed).
/// Ownership: the agent is used by the peer's steps, so it must outlive them, like the models:
/// declare it before the peer. Call Agent::finish() once the peer has stopped, to cancel the
/// commands that are still waiting.
void attach(Peer& peer, Agent& agent);

} // namespace vehicle_sil::control
