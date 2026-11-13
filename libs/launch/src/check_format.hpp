// SPDX-License-Identifier: MIT
//
// The pieces of the check's and the run's output that other outputs reuse (the run's, the
// events). Internal.
#pragma once

#include "vehicle_sil/ecu/detail/json_writer.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch::detail {

/// format_text() without its last line: the tables and the findings.
[[nodiscard]] std::string check_text_body(const CheckReport& report);
/// Each finding as "<severity>: <file>[:<line>]: <message>" (the detail indented below), then an
/// empty line; "" for none.
[[nodiscard]] std::string findings_text(const std::string& file, const std::vector<Finding>& findings);
/// "<n> errors, <n> warnings, <n> notes".
[[nodiscard]] std::string count_summary(std::size_t errors, std::size_t warnings, std::size_t notes);
/// Columns of the width of their longest cell, two spaces apart.
[[nodiscard]] std::string table_text(const std::vector<std::vector<std::string>>& rows);
[[nodiscard]] const char* severity_text(Severity severity);
/// format_json()'s object, as the member `key` of the object `w` is in ("" at the top).
void write_check_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const CheckReport& report);
/// The array "findings".
void write_findings_json(vehicle_sil::detail::JsonWriter& w, const std::string& file, const std::vector<Finding>& findings);
/// The members of one finding of "findings" (severity, code, file, ...), into the object `w` is in.
void write_finding_members(vehicle_sil::detail::JsonWriter& w, const std::string& file, const Finding& finding);
/// format_run_json()'s object, as the member `key` of the object `w` is in ("" at the top).
void write_run_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const RunReport& report);

/// run --scenario, for people: one line with the result, then a table of the steps (line, what,
/// result, the virtual time that decided it).
[[nodiscard]] std::string scenario_text(const ScenarioRun& scenario);
/// One step as a line of the run's output: "line 4: set ...: ok at 100 ms".
[[nodiscard]] std::string scenario_step_text(const ScenarioStepRun& step);
/// The result's "scenario" object, as the member `key` of the object `w` is in.
void write_scenario_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const ScenarioRun& scenario);
/// The members of one step of "steps" (and of the event "scenario_step").
void write_scenario_step_members(vehicle_sil::detail::JsonWriter& w, const ScenarioStepRun& step);

} // namespace vehicle_sil::launch::detail
