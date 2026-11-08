// SPDX-License-Identifier: MIT
//
// The tests' way into the internals of a vehicle_sil::j1939::Network. Only the test targets
// have this header on their include path; protocol parts cannot use it.
#pragma once

#include "vehicle_sil/j1939/network.hpp"

namespace vehicle_sil::j1939::detail {

struct NetworkTestAccess
{
    /// Network::deliver_message(): hands a complete message to the nodes (application stage).
    static void deliver_message(Network& network, const Message& message) { network.deliver_message(message); }
    /// The network's protocol timers, started directly (without a reserved slot).
    static TimerSet& timers(Network& network) noexcept { return network.timers_; }
};

} // namespace vehicle_sil::j1939::detail
