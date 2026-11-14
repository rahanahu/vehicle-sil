// SPDX-License-Identifier: MIT
//
// Drives a host on the wall clock with one thread (vehicle_sil::can). The driver belongs to
// vehicle_sil::step; these are the names CAN code has used.
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/step/real_time_driver.hpp"

namespace vehicle_sil::can {

using RealTimeDriverOptions = step::RealTimeDriverOptions;
using RealTimeDriver = step::RealTimeDriver;

} // namespace vehicle_sil::can
