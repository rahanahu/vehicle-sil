// SPDX-License-Identifier: MIT
//
// Collects the frames a port drops into one PortEvent::ReceiveOverflow (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/step/overflow_collector.hpp"

namespace vehicle_sil::can {

/// For port implementations: collects dropped received frames into one
/// PortEvent::ReceiveOverflow (see vehicle_sil::step::OverflowCollector). `note(time, 0)` marks
/// an unknown number of frames: the event's count is then 0.
using OverflowCollector = step::OverflowCollector<PortEvent>;

} // namespace vehicle_sil::can
