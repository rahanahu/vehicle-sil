// SPDX-License-Identifier: MIT
//
// What every part of a run is given. Internal.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/scenario.hpp"

#include <algorithm>
#include <string>

namespace vehicle_sil::launch::detail {

/// What a run is given, fixed before it begins: the parts of the run (run_*.hpp) keep a reference.
///
/// Threads and locks: any thread of the run reads it, and nothing here is written while the run
/// lasts, so there is no lock. What it points to that does change keeps itself: the interrupt
/// count is the caller's atomic, TimeCommands and EventStream have their own mutexes.
struct RunContext
{
    const SimulationConfig& config;
    const CheckReport& check;
    const RunOptions& options;
    /// run --scenario: the scenario the check read; null without one.
    const Scenario* scenario;
    /// The run's time commands: RunOptions::time_commands, or the run's own.
    TimeCommands& commands;

    /// The stop requests so far (RunOptions::interrupts).
    [[nodiscard]] int interrupts() const { return options.interrupts != nullptr ? options.interrupts->load() : 0; }
    [[nodiscard]] std::string interrupt_name() const
    {
        return options.interrupt_name ? options.interrupt_name() : std::string("interrupt");
    }
    /// run --start-paused with a control server: the run holds before the first step.
    [[nodiscard]] bool holds() const { return options.start_paused && static_cast<bool>(options.serve_control); }
};

inline std::string one_line(std::string text)
{
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

} // namespace vehicle_sil::launch::detail
