// SPDX-License-Identifier: MIT
//
// The pieces of the check's output that the run's output reuses. Internal.
#pragma once

#include "vehicle_sil/ecu/detail/json_writer.hpp"
#include "vehicle_sil/launch/check.hpp"

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

} // namespace vehicle_sil::launch::detail
