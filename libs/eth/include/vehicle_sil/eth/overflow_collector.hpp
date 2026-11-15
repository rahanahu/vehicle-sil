// SPDX-License-Identifier: MIT
//
// Collects what a port drops into one PortEvent::ReceiveOverflow or EventOverflow
// (vehicle_sil::eth).
#pragma once

#include "vehicle_sil/eth/port.hpp"
#include "vehicle_sil/step/overflow_collector.hpp"

namespace vehicle_sil::eth {

/// For port implementations: collects dropped received frames (or dropped events) into one
/// PortEvent of the kind given to the constructor (ReceiveOverflow or EventOverflow; see
/// vehicle_sil::step::OverflowCollector).
using OverflowCollector = step::OverflowCollector<PortEvent>;

} // namespace vehicle_sil::eth
