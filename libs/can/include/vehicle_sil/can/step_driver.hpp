// SPDX-License-Identifier: MIT
//
// Drives hosts on virtual time in a test loop, without SIL Kit (vehicle_sil::can). The driver
// belongs to vehicle_sil::step; this is the name CAN code has used.
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/step/step_driver.hpp"

namespace vehicle_sil::can {

using StepDriver = step::StepDriver;

} // namespace vehicle_sil::can
