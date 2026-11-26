// SPDX-License-Identifier: MIT
//
// The opt-in Control Hub transport metrics report (`vehicle_sil_launch run --control-metrics`,
// schema/control-metrics.schema.json): pure formatting of RunReport into UTF-8 JSON with LF line
// endings. No file system access (writing is in probe.hpp). Internal.
#pragma once

#include "vehicle_sil/launch/run.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace vehicle_sil::launch {

/// The version of the JSON that format_control_metrics_json() writes
/// (its "vehicle_sil_control_metrics" key).
inline constexpr int kControlMetricsFormat = 1;

/// Formats the Control Hub transport metrics report of `report` as pretty-printed UTF-8 JSON with
/// LF line endings (schema/control-metrics.schema.json). Pure; performs no file I/O.
[[nodiscard]] std::string format_control_metrics_json(const RunReport& report);

/// Formats an unavailable Control Hub transport metrics report for an unexpected std::exception
/// escaping run_file() before transport counters were collected ("unavailable_reason":
/// "internal_error", "run.result": null, "counters": null). Pure; performs no file I/O.
[[nodiscard]] std::string format_control_metrics_internal_error_json(
    std::string_view file, const std::optional<std::string>& variant = std::nullopt);

} // namespace vehicle_sil::launch
