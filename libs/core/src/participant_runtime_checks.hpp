// SPDX-License-Identifier: MIT
//
// Checks of ParticipantRuntime that need no SIL Kit participant to be tried. Internal to
// vehicle_sil::core and not installed: the library and its tests use them.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <string>

namespace vehicle_sil::detail {

/// Empty if the step of `given` that SIL Kit calls the step handler with at `now` is the
/// participant's own (`own`, VirtualTimeOptions::step). Otherwise the error that the step fails
/// with: both widths, the time, and the key of the SIL Kit participant configuration that keeps
/// SIL Kit from cutting the step (dynamic simulation steps).
[[nodiscard]] std::string step_width_error(std::chrono::nanoseconds given, SimDuration own, SimTime now);

} // namespace vehicle_sil::detail
