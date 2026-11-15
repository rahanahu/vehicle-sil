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
/// Each finding as "<severity>: <file>[:<line>]: <message> [<code>]" (the detail indented below;
/// the code last, as GCC puts its [-W...] option, so that "error:" stays at the start), then an
/// empty line; "" for none.
[[nodiscard]] std::string findings_text(const std::string& file, const std::vector<Finding>& findings);
/// "<n> errors, <n> warnings, <n> notes".
[[nodiscard]] std::string count_summary(std::size_t errors, std::size_t warnings, std::size_t notes);
/// Columns of the width of their longest cell, two spaces apart.
[[nodiscard]] std::string table_text(const std::vector<std::vector<std::string>>& rows);
[[nodiscard]] const char* severity_text(Severity severity);
/// format_json()'s object, as the member `key` of the object `w` is in ("" at the top).
void write_check_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const CheckReport& report);
/// The array "networks" of a participant's "reported": what its executable described, under the
/// executable's names (check_networks.cpp).
void write_reported_networks(vehicle_sil::detail::JsonWriter& w, const EcuDescription& ecu);
/// An array `key` of parameters: each with "name", "value" (as a scenario reads it: true or false,
/// a number, or a text) and "from" (param_source_name()).
void write_params_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const std::vector<ParamValue>& params);
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
/// The result's "scenario" object, as the member `key` of the object `w` is in. `timeline`: the
/// timeline file of the run (run --timeline), its last member "timeline"; "": no such member.
void write_scenario_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const ScenarioRun& scenario,
                         const std::string& timeline = {});
/// The members "inputs" (the dithers the scenario's "inputs" declare: variable, line, dither with
/// its shape and text) and "lines" (what the text puts on the inputs that have a dither: variable,
/// kind, line, step, from_ms, to_ms, time_source, after_line, shape, text), into the object `w` is in.
/// What a line of a timeline file has too has the same name and meaning; a time that is not known
/// before the run is null ("time_source" says why), and so is the end of a dither, which has none.
/// Of a scenario with a dither (Scenario::has_dither); the check's and the run's are the same.
void write_scenario_lines_json(vehicle_sil::detail::JsonWriter& w, const std::vector<ScenarioLineText>& inputs,
                               const std::vector<ScenarioLineText>& lines);
/// The same list for people: a table (input, from, to, what, line).
[[nodiscard]] std::string scenario_lines_text(const std::vector<ScenarioLineText>& lines);
/// The members of one step of "steps" (and of the event "scenario_step").
void write_scenario_step_members(vehicle_sil::detail::JsonWriter& w, const ScenarioStepRun& step);

} // namespace vehicle_sil::launch::detail
