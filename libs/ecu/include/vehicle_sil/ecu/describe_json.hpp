// SPDX-License-Identifier: MIT
//
// The parts of to_json()'s layout (vehicle_sil/ecu/describe.hpp) for a tool that reads the
// output of --describe or repeats its variables in JSON of its own (vehicle_sil_launch), so that
// both keep the names and the layout of to_json().
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/json/json_writer.hpp"

#include <string_view>
#include <vector>

namespace vehicle_sil {

/// The names to_json() uses: "bool", "integer", "real", "enum"; "input", "output", "parameter".
/// std::logic_error for a value outside the enumerators.
[[nodiscard]] const char* value_type_name(control::ValueType type);
[[nodiscard]] const char* causality_name(control::Causality causality);

/// The array `key` of to_json() ("variables"): each variable with its keys in to_json()'s order.
void write_variables(json::JsonWriter& w, std::string_view key, const std::vector<control::VariableInfo>& variables);

} // namespace vehicle_sil
