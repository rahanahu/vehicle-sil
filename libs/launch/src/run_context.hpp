// SPDX-License-Identifier: MIT
//
// What every part of a run is given. Internal.
#pragma once

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/config.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/run_events.hpp"
#include "vehicle_sil/launch/run_plan.hpp"
#include "vehicle_sil/launch/scenario.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace vehicle_sil::launch::detail {

/// What a run is given, fixed before it begins: the parts of the run (run_*.hpp) keep a reference.
/// Three things: the plan (a value: what was settled before the run), the observers (what the run
/// calls back) and the controls (borrowed: they may outlive the run).
///
/// Threads and locks: any thread of the run reads it, and nothing here is written while the run
/// lasts, so there is no lock. What it points to that does change keeps itself: the stop
/// requests are atomics (StopRequests), TimeCommands and the RunEvents keep themselves, and the
/// phase of the controls is the main thread's to set.
struct RunContext
{
    /// The plan's configuration and check (RunPlan::config(), RunPlan::check), named here because
    /// every part reads them.
    const SimulationConfig& config;
    const CheckReport& check;
    const RunPlan& plan;
    /// The Runner's copy of the caller's functions (run.hpp says of each which thread calls it and
    /// what it may do); none of them is in the RunPlan.
    const RunObservers& observers;
    /// What the run tells while it lasts (run --events), as the interface the run knows
    /// (run_events.hpp): the object of RunObservers::events, which the parts of a run do not read.
    /// Null: nothing is told this way, and the lines go to RunObservers::write_line.
    RunEvents* events;
    /// run --scenario: the scenario the check read (RunPlan::scenario()); null without one.
    const Scenario* scenario;
    /// The run's controls: RunLent::controls, or the run's own (on RunLent::stops).
    RunControls& controls;
    /// The time commands of `controls`.
    TimeCommands& commands;

    /// The stop requests so far (of `controls`).
    [[nodiscard]] int interrupts() const { return controls.stops().count(); }
    [[nodiscard]] std::string interrupt_name() const
    {
        return observers.interrupt_name ? observers.interrupt_name() : std::string("interrupt");
    }
    /// What asked first to stop, as messages name it before it is noted (OutcomeEnd::stop_text()).
    [[nodiscard]] std::string stop_text() const
    {
        OutcomeEnd end;
        note_interrupt(end);
        return end.stop_text();
    }
    /// Notes in `end` what asked first to stop the run: its name and its kind (the one place that
    /// writes them). The kind is of the first request, whatever came after it.
    void note_interrupt(OutcomeEnd& end) const
    {
        end.interrupted_by = interrupt_name();
        end.interrupted_kind = stop_kind(controls.stops().first());
    }
    /// run --start-paused with a control server: the run holds before the first step.
    [[nodiscard]] bool holds() const { return plan.holds_before_start; }
};

inline std::string one_line(std::string text)
{
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

} // namespace vehicle_sil::launch::detail
