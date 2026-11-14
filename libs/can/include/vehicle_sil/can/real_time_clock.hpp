// SPDX-License-Identifier: MIT
//
// Clocks of the real-time driver and real-time ports (vehicle_sil::can). They belong to
// vehicle_sil::step; these are the names CAN code has used.
#pragma once

#include "vehicle_sil/step/real_time_clock.hpp"

namespace vehicle_sil::can {

using RealTimeClock = step::RealTimeClock;
using SteadyRealTimeClock = step::SteadyRealTimeClock;
using ManualRealTimeClock = step::ManualRealTimeClock;

} // namespace vehicle_sil::can
