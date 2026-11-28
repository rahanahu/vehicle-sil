// SPDX-License-Identifier: MIT
//
// The timeline file of a scenario run (run --scenario --timeline <file>): what it is made of
// (TimelineInput: the scenario, how it ran, the record the engine filled, timeline.hpp) and its
// text (format_timeline_json()). Internal; what a user sees of it: docs/simulation-run.md,
// schema/timeline.schema.json.
#pragma once

#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/scenario/timeline.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vehicle_sil::scenario {
struct ScenarioRun;
} // namespace vehicle_sil::scenario

namespace vehicle_sil::launch {

/// The version of the timeline file (its "vehicle_sil_timeline" key).
inline constexpr int kTimelineFormat = 1;

/// What a timeline file is made of.
struct TimelineInput
{
    std::string file;      ///< the configuration file, as the user named it
    std::string variant{}; ///< the chosen variant, or ""
    /// The scenario's parameters with the values in effect (ScenarioRead::params).
    std::vector<config::ParamValue> params{};
    std::int64_t time_step_ms = 0; ///< the time step of the launcher's controller
    const scenario::Scenario* scenario = nullptr;
    const scenario::ScenarioRun* run = nullptr;
    /// What the scenario's participants describe: the unit, the range and the causality of the
    /// variables. May be null (then they are left out).
    const scenario::ScenarioCatalog* catalog = nullptr;
    /// Null if the scenario never began: no series.
    const scenario::TimelineRecord* record = nullptr;
    /// Extra variables requested via RunIntent::record (used when `record` is null, e.g. not_run).
    std::vector<scenario::RecordedVariable> recorded_variables{};
    /// The findings of the check and of the run; the file takes those of the scenario
    /// (timeline_takes_finding()).
    std::vector<config::Finding> findings{};
};

/// Whether a timeline file has the finding with this code: whole if it is about the scenario
/// ("scenario-..."); with its code alone (`code_only`: its message has numbers that differ from
/// run to run) if it says that an ECU's values came late ("control-link-late-values": what such a
/// run recorded may differ from run to run). Nothing else: not the pace of the run in real time.
[[nodiscard]] bool timeline_takes_finding(const std::string& code, bool& code_only);

/// The timeline file: JSON with one member per line and one point per line, LF line ends. Nothing
/// of the wall clock is in it: runs that decide the same in virtual time give the same text.
[[nodiscard]] std::string format_timeline_json(const TimelineInput& input);

} // namespace vehicle_sil::launch
