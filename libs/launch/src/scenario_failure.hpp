// SPDX-License-Identifier: MIT
//
// What the scenario's engine (scenario_run.cpp) and the explanation of why a step failed
// (scenario_failure.cpp) share. Internal.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <string>

namespace vehicle_sil::launch::detail {

/// A virtual time in milliseconds, in the shortest form ("300", "2.5"): as the messages say it.
[[nodiscard]] std::string ms_text(SimTime t);

} // namespace vehicle_sil::launch::detail
