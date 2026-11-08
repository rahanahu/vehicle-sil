// SPDX-License-Identifier: MIT
//
// The virtual-time example peers that run on a vehicle_sil::Peer: its options from the common
// command line, and the run until the system controller ends the simulation.
#pragma once

#include "vt_common.hpp"

#include "vehicle_sil/peer/peer.hpp"

#include <exception>

namespace vehicle_sil::vt {

/// A coordinated, time-synchronised peer. Its steps run only before `--end-ms`
/// (VirtualTimeOptions::end), so a peer's work and trace end at the same virtual time however
/// quickly the system controller's stop arrives.
inline vehicle_sil::PeerOptions peer_options(const CommonOptions& o)
{
    vehicle_sil::PeerOptions options;
    options.participant = o.participant;
    options.registry_uri = o.registry_uri;
    options.lifecycle = LifecycleMode::Coordinated;
    options.virtual_time = VirtualTimeOptions{o.step, o.real_time_factor, o.end};
    return options;
}

/// Starts the peer, waits for the end of the simulation and reports how it ended. On every path
/// it stops the peer before returning, so no step runs any more and objects the steps use may be
/// destroyed. Returns the process exit code.
inline int run_until_finished(Peer& peer, const CommonOptions& o)
{
    struct StopOnReturn
    {
        Peer& peer;
        ~StopOnReturn() { peer.stop("peer finished"); }
    } stopOnReturn{peer};
    try
    {
        peer.start(o.timeout);
    }
    catch (const std::exception& e)
    {
        log(o.participant, "FAIL setup: %s", e.what());
        return kExitSetupFailed;
    }
    log(o.participant, "running, step %lld us", static_cast<long long>(o.step / std::chrono::microseconds(1)));
    const auto state = peer.wait(run_limit(o));
    return check_finished(state, peer.runtime(), peer.error(), o);
}

} // namespace vehicle_sil::vt
