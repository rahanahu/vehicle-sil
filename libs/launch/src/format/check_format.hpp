// SPDX-License-Identifier: MIT
//
// The pieces of the check's and the run's output that other outputs reuse (the run's, the
// events). Internal.
#pragma once

#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/runner/traffic.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::scenario {
struct ScenarioCheck; // scenario_check.hpp
} // namespace vehicle_sil::scenario

namespace vehicle_sil::launch::detail {

/// format_text() without its last line: the tables and the findings. `scenario`: the line of the
/// scenario of --scenario, its parameters and its lines; null for none.
[[nodiscard]] std::string check_text_body(const config::CheckReport& report, const scenario::ScenarioCheck* scenario);
/// Each finding as "<severity>: <file>[:<line>]: <message> [<code>]" (the detail indented below;
/// the code last, as GCC puts its [-W...] option, so that "error:" stays at the start), then an
/// empty line; "" for none.
[[nodiscard]] std::string findings_text(const std::string& file, const std::vector<config::Finding>& findings);
/// "<n> errors, <n> warnings, <n> notes".
[[nodiscard]] std::string count_summary(std::size_t errors, std::size_t warnings, std::size_t notes);
/// Columns of the width of their longest cell, two spaces apart.
[[nodiscard]] std::string table_text(const std::vector<std::vector<std::string>>& rows);
[[nodiscard]] const char* severity_text(config::Severity severity);
/// format_json()'s object, as the member `key` of the object `w` is in ("" at the top).
void write_check_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const Checked& checked);
/// The array "networks" of a participant's "reported": what its executable described, under the
/// executable's names (check_networks.cpp).
void write_reported_networks(vehicle_sil::json::JsonWriter& w, const EcuDescription& ecu);
/// Writes a single parameter value (`key`: boolean, integer (decimal or hex), real, or string) into the object `w` is in.
void write_param_scalar(vehicle_sil::json::JsonWriter& w, std::string_view key, const std::string& text, bool quoted);
/// Writes one value of ScenarioShapeText into the object `w` is in.
void write_shape_value(vehicle_sil::json::JsonWriter& w, std::string_view key, const scenario::ScenarioShapeText::Value& value);
/// Writes a non-empty `call_stack` array (`key`) into the object `w` is in; does nothing if `stack` is empty.
void write_call_stack_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const std::vector<scenario::SourceFrame>& stack);
/// An array `key` of parameters: each with "name", "value" (as a scenario reads it: true or false,
/// a number, or a text) and "from" (param_source_name()), plus optional "type", "values", and
/// "description" when `meta` provides template-derived metadata for that parameter.
void write_params_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const std::vector<config::ParamValue>& params,
                       const std::vector<scenario::ScenarioParamMeta>* meta = nullptr);
/// The array "findings".
void write_findings_json(vehicle_sil::json::JsonWriter& w, const std::string& file, const std::vector<config::Finding>& findings);
/// The members of one finding of "findings" (severity, code, file, ...), into the object `w` is in.
void write_finding_members(vehicle_sil::json::JsonWriter& w, const std::string& file, const config::Finding& finding);

/// The result's traffic, for people: the line "traffic: ..." (per network of the configuration
/// how many counted participants sent and how many received, or "nothing sent", or "not
/// counted"; and before which virtual time), an empty line, and the table (network, participant,
/// sent, received; a row that is not counted has "-" and says why to its right; a network nobody
/// is known to be on has one such row). A configuration without a network: one line.
[[nodiscard]] std::string traffic_text(const runner::RunTraffic& traffic);
/// run --scenario, for people: one line with the result, then a table of the steps (line, what,
/// result, the virtual time that decided it).
[[nodiscard]] std::string scenario_text(const scenario::ScenarioRun& scenario);
/// The result's "scenario" object, as the member `key` of the object `w` is in. `timeline`: the
/// timeline file of the run (run --timeline), its last member "timeline"; "": no such member.
void write_scenario_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const scenario::ScenarioRun& scenario,
                         const std::string& timeline = {});
/// The members "inputs" (the dithers the scenario's "inputs" declare: variable, line, dither with
/// its shape and text) and "lines" (what the text puts on the inputs that have a dither: variable,
/// kind, line, step, from_ms, to_ms, time_source, after_line, shape, text), into the object `w` is in.
/// What a line of a timeline file has too has the same name and meaning; a time that is not known
/// before the run is null ("time_source" says why), and so is the end of a dither, which has none.
/// Of a scenario with a dither (Scenario::has_dither); the check's and the run's are the same.
void write_scenario_lines_json(vehicle_sil::json::JsonWriter& w, const std::vector<scenario::ScenarioLineText>& inputs,
                               const std::vector<scenario::ScenarioLineText>& lines);
/// The same list for people: a table (input, from, to, what, line).
[[nodiscard]] std::string scenario_lines_text(const std::vector<scenario::ScenarioLineText>& lines);
/// The members of one step of "steps" (and of the event "scenario_step").
void write_scenario_step_members(vehicle_sil::json::JsonWriter& w, const scenario::ScenarioStepRun& step);

} // namespace vehicle_sil::launch::detail
