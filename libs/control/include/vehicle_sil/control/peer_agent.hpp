// SPDX-License-Identifier: MIT
//
// Connects a control agent to a vehicle_sil::Peer (vehicle_sil::peer_control).
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/peer/peer.hpp"

namespace vehicle_sil::control {

struct AttachOptions
{
    /// Publish the peer's state as outputs and its changes as events (see attach()).
    bool monitor = true;
};

/// Drives `agent` from the steps of `peer`: commands take effect just before the application
/// of a step (Agent::on_step_begin()), values are sampled just after the application and the LIN
/// nodes (Agent::on_step_end()). In real time, a queued command wakes the peer's driver
/// (Host::post(), with at most one wake-up pending), so it takes effect in the next step instead
/// of after up to PeerOptions::max_idle; when the peer stops, the agent stops waking it.
///
/// With AttachOptions::monitor, the agent also gets these outputs (after the application's
/// variables), read at the agent's samples on the step thread:
///   peer.steps, peer.speed (simulation seconds per wall-clock second since the previous
///   sample: 0 at the first one, and low after the simulation stood still), peer.app_time_max_us
///   (the longest application-and-LIN stage since then, without the J1939 handling);
///   for each J1939 network N: j1939.N.bus_state (-1 unknown, 0 error active, 1 error passive,
///   2 bus off) and the counters j1939.N.frames_received, frames_sent, send_failures,
///   send_queue_full, receive_overflows, held_dropped, address_violations,
///   transport_aborts_sent, transport_aborts_received;
///   for each node i of N: j1939.N.node<i>.address and j1939.N.node<i>.address_state
///   (0 fixed, 1 not started, 2 claiming, 3 claimed, 4 cannot claim);
/// and events "j1939.N.node<i>: <state> at address <a>" (the state named as in the items) when
/// a node's address state or address changes (Warning for cannot_claim). The
/// networks and nodes that exist when attach() is called are covered; the names must not clash
/// with the agent's variables (std::invalid_argument). A failed step is not reported here: no
/// later step runs to report it; read Peer::failed() and Peer::error() (thread safe) instead.
///
/// Once per agent and once per peer, before peer.start() and before the agent's first submit()
/// (std::logic_error otherwise, in both time modes; on an error neither is changed).
/// Ownership: the agent is used by the peer's steps, so it must outlive them, like the models:
/// declare it before the peer. Call Agent::finish() once the peer has stopped, to cancel the
/// commands that are still waiting.
void attach(Peer& peer, Agent& agent, AttachOptions options = {});

} // namespace vehicle_sil::control
