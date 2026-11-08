// SPDX-License-Identifier: MIT
#include "vehicle_sil/control/peer_agent.hpp"

#include <atomic>
#include <memory>

namespace vehicle_sil::control {

void attach(Peer& peer, Agent& agent)
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
    agent.bind_driver(std::move(wakeup)); // checks the agent first: nothing is changed if it throws
    try
    {
        peer.set_control([&agent](SimTime now) { agent.on_step_begin(now); },
                         [&agent](SimTime now) { agent.on_step_end(now); },
                         [&agent] { agent.clear_wakeup(); });
    }
    catch (...)
    {
        agent.unbind_driver();
        throw;
    }
}

} // namespace vehicle_sil::control
