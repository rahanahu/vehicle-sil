// SPDX-License-Identifier: MIT
//
// Private helpers to attach a SimulationController to TimeCommands and coordinate single-snapshot
// time/state publication without exposing SimulationController in public headers (libs/runner internal).
#pragma once

#include "vehicle_sil/runner/time_commands.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace vehicle_sil {
class SimulationController;
struct TimeState;
} // namespace vehicle_sil

namespace vehicle_sil::runner {
class RunControls;
} // namespace vehicle_sil::runner

namespace vehicle_sil::runner::detail {

struct TimeCommandsAccess
{
    static void attach(TimeCommands& commands, SimulationController& controller, std::int64_t step_ms,
                       std::optional<double> speed);
    /// Reads `controller.time_state()` once under TimeCommands' mutex and passes both that snapshot
    /// and the `TimeFacts` derived from it to `f` under the same mutex.
    static void look_with_gate(TimeCommands& commands, SimulationController& controller,
                               const std::function<void(const TimeState& gate, const TimeFacts& facts)>& f);
};

struct RunControlsAccess
{
    /// Runs `emit_time` with one `controller.time_state()` snapshot under TimeCommands' mutex, then
    /// (when `publish_state` is true) publishes the state derived from that exact same snapshot
    /// before releasing the mutex. Keeps a look's `time` event and its `state` atomic against
    /// concurrent commands and gate transitions.
    static void look_time(RunControls& controls, SimulationController& controller, bool publish_state,
                          const std::function<void(const TimeState& gate)>& emit_time);
};

inline void attach_controller(TimeCommands& commands, SimulationController& controller, std::int64_t step_ms,
                              std::optional<double> speed)
{
    TimeCommandsAccess::attach(commands, controller, step_ms, speed);
}

} // namespace vehicle_sil::runner::detail
