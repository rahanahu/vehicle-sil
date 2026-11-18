// SPDX-License-Identifier: MIT
//
// The piece of the run's output that another output reuses (the test's). Internal. The pieces
// that need no run (the traffic, the scenario's steps) are in check_format.hpp.
#pragma once

#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <string_view>

namespace vehicle_sil::launch::detail {

/// format_run_json()'s object, as the member `key` of the object `w` is in ("" at the top).
void write_run_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const RunReport& report);

/// The line of the text output about the time ("time: ..."), or "" if no speed was set and no
/// step was paced.
[[nodiscard]] std::string timing_text(const RunTiming& timing);

} // namespace vehicle_sil::launch::detail
