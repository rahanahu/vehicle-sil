// SPDX-License-Identifier: MIT
//
// The line in which a bridge tells a program (the launcher) the state of its connection to the
// outside: "link <key>=<value>". One form for every bridge; each bridge names its own key and
// values (docs/bridges.md). The line is printed after the tag like the "stats" line.
#pragma once

#include <string>
#include <string_view>

namespace vehicle_sil::bridge_runtime {

/// "link <key>=<value>", for example "link link=up" or "link bus=bus-off". `key` and `value`
/// contain no space and no '='.
[[nodiscard]] std::string format_link_line(std::string_view key, std::string_view value);

} // namespace vehicle_sil::bridge_runtime
