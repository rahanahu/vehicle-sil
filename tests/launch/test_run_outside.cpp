// SPDX-License-Identifier: MIT
//
// What run reads of the bridges of --connect (run_outside.cpp): the counts in a statistics line,
// from the lines the bridges themselves format (both sides of the line's form), and when the
// event "outside" goes out (the time given by the test).
#include "launch_test_util.hpp"

#include "check_words.hpp"

#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/can_bridge/can_bridge.hpp"

#include <chrono>
#include <optional>
#include <string>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using launch::detail::OutsideCounts;
using launch::detail::OutsideNote;
using launch::detail::read_outside_counts;
using vehicle_sil::launch::test::check_text;

namespace {

constexpr const char* kEth = "vehicle_sil_ethernet_bridge";
constexpr const char* kXl = "vehicle_sil_vector_xl_bridge";

bool reads(const char* tool, const std::string& line, std::uint64_t to, std::uint64_t from)
{
    return read_outside_counts(tool, line) == OutsideCounts{to, from};
}

OutsideCounts counts(std::uint64_t to, std::uint64_t from) { return OutsideCounts{to, from}; }

bool refuses(const char* tool, const std::string& line) { return !read_outside_counts(tool, line).has_value(); }

} // namespace

TEST_CASE(read_outside_counts_takes_the_keys_of_each_bridge)
{
    CHECK(reads(kEth, "[eth-bridge] stats silkit_to_ext=152 ext_to_silkit=9 ext_from_devices=3 own_tx_dropped=0", 152, 3));
    CHECK(reads(kXl, "[bridge] stats silkit_to_xl=40 xl_to_silkit=7 own_tx_dropped=40", 40, 7));
    // Without the tag, and with the "\r" of a Windows line end.
    CHECK(reads(kEth, "stats silkit_to_ext=1 ext_from_devices=0", 1, 0));
    CHECK(reads(kXl, "[bridge] stats silkit_to_xl=0 xl_to_silkit=0\r", 0, 0));
    // The largest count that the JSON forms write (a signed 64-bit integer), and leading zeros.
    CHECK(reads(kEth, "stats silkit_to_ext=9223372036854775807 ext_from_devices=007", 9223372036854775807u, 7));
}

TEST_CASE(read_outside_counts_refuses_any_other_line)
{
    // Of the other bridge, or of a bridge of another version (no ext_from_devices).
    CHECK(refuses(kXl, "[eth-bridge] stats silkit_to_ext=1 ext_from_devices=1"));
    CHECK(refuses(kEth, "[eth-bridge] stats silkit_to_ext=12 ext_to_silkit=11 own_tx_dropped=0"));
    CHECK(refuses("some_tool", "stats silkit_to_ext=1 ext_from_devices=1"));
    // Not a statistics line.
    CHECK(refuses(kEth, "[eth-bridge] stopping"));
    CHECK(refuses(kEth, "[eth-bridge] warning: stats silkit_to_ext=1 ext_from_devices=1"));
    CHECK(refuses(kEth, "[eth-bridge] statsx silkit_to_ext=1 ext_from_devices=1"));
    CHECK(refuses(kEth, "[eth-bridge] stats"));
    CHECK(refuses(kEth, ""));
    // A part of another form.
    CHECK(refuses(kEth, "stats silkit_to_ext=abc ext_from_devices=1"));
    CHECK(refuses(kEth, "stats silkit_to_ext=-1 ext_from_devices=1"));
    CHECK(refuses(kEth, "stats silkit_to_ext=+1 ext_from_devices=1"));
    CHECK(refuses(kEth, "stats silkit_to_ext= ext_from_devices=1"));
    CHECK(refuses(kEth, "stats silkit_to_ext=1 ext_from_devices=1 =2"));
    CHECK(refuses(kEth, "stats silkit_to_ext=1 ext_from_devices=1 loose"));
    CHECK(refuses(kEth, "stats silkit_to_ext=1  ext_from_devices=1"));
    CHECK(refuses(kEth, "stats silkit_to_ext=1 ext_from_devices=1 "));
    CHECK(refuses(kEth, "stats silkit_to_ext=1 ext_from_devices=1\r\r"));
    // More than a signed 64-bit integer, and more than 64 bits.
    CHECK(refuses(kEth, "stats silkit_to_ext=9223372036854775808 ext_from_devices=1"));
    CHECK(refuses(kEth, "stats silkit_to_ext=1 ext_from_devices=18446744073709551615"));
    CHECK(refuses(kEth, "stats silkit_to_ext=18446744073709551616 ext_from_devices=1"));
    // A key twice, also one that is not read.
    CHECK(refuses(kEth, "stats silkit_to_ext=1 ext_from_devices=1 silkit_to_ext=2"));
    CHECK(refuses(kEth, "stats silkit_to_ext=1 oversize=0 ext_from_devices=1 oversize=0"));
}

// The form of the line on both sides: the bridges' own formatting (what they print) is read. A key that a
// bridge renames fails here and in the bridge's test of the order of its keys.
TEST_CASE(read_outside_counts_reads_the_lines_the_bridges_print)
{
    ethernet_bridge::EthernetBridgeStatistics eth;
    eth.silkit_to_ext = 101;
    eth.ext_to_silkit = 102;
    eth.ext_from_devices = 103;
    eth.own_tx_dropped = 104;
    eth.filtered_from_ext = 105;
    CHECK(reads(kEth, "[eth-bridge] " + ethernet_bridge::format_statistics(eth), 101, 103));
    can_bridge::CanBridgeStatistics can;
    can.silkit_to_external = 201;
    can.external_to_silkit = 202;
    can.own_transmissions_dropped = 203;
    CHECK(reads(kXl, "[bridge] " + can_bridge::format_statistics(can), 201, 202));
}

TEST_CASE(outside_note_writes_changes_at_most_once_a_second_and_the_last_at_the_end)
{
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    OutsideNote note;
    CHECK(!note.last() && !note.flush() && !note.due(t0));
    // The first counts at once.
    CHECK(note.on_counts({1, 0}, t0) == counts(1, 0));
    // The same again: nothing.
    CHECK(!note.on_counts({1, 0}, t0 + 500ms));
    // A change within the second is held back; the next one a second after the last written goes out.
    CHECK(!note.on_counts({2, 0}, t0 + 600ms) && note.last() == counts(2, 0));
    CHECK(note.on_counts({3, 0}, t0 + 1s) == counts(3, 0));
    // Held back, and out once the second has passed (the run asks at each look).
    CHECK(!note.on_counts({4, 1}, t0 + 1s + 999ms) && note.last() == counts(4, 1));
    CHECK(!note.due(t0 + 1s + 999ms) && note.due(t0 + 2s) == counts(4, 1));
    CHECK(!note.due(t0 + 4s));
    // Back to what was written last: nothing is held back.
    CHECK(!note.on_counts({5, 1}, t0 + 2s + 1ms) && !note.on_counts({4, 1}, t0 + 2s + 2ms) && !note.due(t0 + 4s));
    // At the end, whatever the time; once.
    CHECK(note.on_counts({6, 2}, t0 + 4s) == counts(6, 2));
    CHECK(!note.on_counts({7, 2}, t0 + 4s + 1ms) && note.flush() == counts(7, 2) && !note.flush());
    CHECK(note.last() == counts(7, 2));
}

TEST_CASE(is_link_line_takes_the_link_lines_of_the_bridges)
{
    CHECK(launch::detail::is_link_line("[eth-bridge] link link=up"));
    CHECK(launch::detail::is_link_line("[bridge] link bus=bus-off"));
    CHECK(launch::detail::is_link_line("link link=down"));
    CHECK(!launch::detail::is_link_line("[eth-bridge] linked to enp3s0"));
    CHECK(!launch::detail::is_link_line("[eth-bridge] stats silkit_to_ext=1"));
    CHECK(!launch::detail::is_link_line("warning: link down"));
}

TEST_CASE(without_tag_takes_off_a_word_in_brackets)
{
    check_text(std::string(launch::detail::without_tag("[eth-bridge] stats a=1")), "stats a=1");
    check_text(std::string(launch::detail::without_tag("[bridge] FAIL")), "FAIL");
    // Not a tag: a space in it, nothing in it, no space after it.
    check_text(std::string(launch::detail::without_tag("[not a tag] FAIL")), "[not a tag] FAIL");
    check_text(std::string(launch::detail::without_tag("[] FAIL")), "[] FAIL");
    check_text(std::string(launch::detail::without_tag("[bridge]FAIL")), "[bridge]FAIL");
}
