// SPDX-License-Identifier: MIT
//
// format_link_line(): the one form of the "link <key>=<value>" line of every bridge.
#include "test_harness.hpp"

#include "vehicle_sil/bridge_runtime/link_line.hpp"

using vehicle_sil::bridge_runtime::format_link_line;

TEST_CASE(link_line_is_link_then_key_equals_value)
{
    CHECK(format_link_line("link", "up") == std::string("link link=up"));
    CHECK(format_link_line("link", "down") == std::string("link link=down"));
    CHECK(format_link_line("link", "unknown") == std::string("link link=unknown"));
    CHECK(format_link_line("bus", "bus-off") == std::string("link bus=bus-off"));
}
