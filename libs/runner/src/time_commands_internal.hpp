// SPDX-License-Identifier: MIT
//
// Private helper to attach a SimulationController to TimeCommands without exposing
// SimulationController in TimeCommands' public header (libs/runner internal).
#pragma once

#include "vehicle_sil/runner/time_commands.hpp"

#include <cstdint>
#include <optional>

namespace vehicle_sil {
class SimulationController;
} // namespace vehicle_sil

namespace vehicle_sil::runner::detail {

struct TimeCommandsAccess
{
    static void attach(TimeCommands& commands, SimulationController& controller, std::int64_t step_ms,
                       std::optional<double> speed);
};

inline void attach_controller(TimeCommands& commands, SimulationController& controller, std::int64_t step_ms,
                              std::optional<double> speed)
{
    TimeCommandsAccess::attach(commands, controller, step_ms, speed);
}

} // namespace vehicle_sil::runner::detail
