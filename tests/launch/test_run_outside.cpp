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
#include <map>
#include <optional>
#include <string>
#include <utility>

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

namespace {

using launch::detail::OutsideBusValue;
using launch::detail::OutsideLinkLine;
using launch::detail::OutsideLinkValue;
using launch::detail::OutsideState;
using launch::detail::read_outside_link_line;
using launch::detail::read_outside_stats;
using Clock = std::chrono::steady_clock;

OutsideLinkLine link_of(OutsideLinkValue v) { return OutsideLinkLine{v, std::nullopt}; }
OutsideLinkLine bus_of(OutsideBusValue v) { return OutsideLinkLine{std::nullopt, v}; }

} // namespace

TEST_CASE(read_outside_stats_takes_every_key_and_refuses_what_read_outside_counts_refuses)
{
    const auto all = read_outside_stats(kEth, "[eth-bridge] stats silkit_to_ext=1 ext_from_devices=2 ext_queue_full=3 new_key=4");
    CHECK(all.has_value());
    if (!all)
    {
        return;
    }
    CHECK(all->size() == 4 && all->at("ext_queue_full") == 3 && all->at("new_key") == 4);
    // Also without the two keys that read_outside_counts needs.
    CHECK(read_outside_stats(kXl, "stats error_frames=5")->at("error_frames") == 5);
    CHECK(!read_outside_stats(kXl, "stats a=1 a=2") && !read_outside_stats(kXl, "stats a=x") && !read_outside_stats(kXl, "link link=up"));
    CHECK(!read_outside_stats("some_tool", "stats a=1"));
}

TEST_CASE(read_outside_link_line_takes_the_known_keys_and_skips_the_rest)
{
    auto line = read_outside_link_line("[eth-bridge] link link=down");
    CHECK(line.has_value());
    if (!line)
    {
        return;
    }
    CHECK(line->link == OutsideLinkValue::Down && !line->bus);
    line = read_outside_link_line("[bridge] link bus=bus-off\r");
    CHECK(line && !line->link && line->bus == OutsideBusValue::BusOff);
    // Keys of later versions, a value that is not known, a part of another form: skipped.
    line = read_outside_link_line("link link=up device=ab12 marked=1 shared=0 peers=4321,77 bus=nonsense loose =x");
    CHECK(line && line->link == OutsideLinkValue::Up && !line->bus);
    line = read_outside_link_line("link link=unknown bus=unknown");
    CHECK(line && line->link == OutsideLinkValue::Unknown && line->bus == OutsideBusValue::Unknown);
    // Not a link line.
    CHECK(!read_outside_link_line("stats link=up") && !read_outside_link_line("[eth-bridge] linked"));
    // A line with no known key is a link line that says nothing.
    line = read_outside_link_line("link future=1");
    CHECK(line && !line->link && !line->bus);
}

TEST_CASE(outside_state_adds_up_the_down_spans_and_counts_one_that_is_still_on)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    CHECK(state.link_text().empty());
    CHECK(state.on_link(link_of(OutsideLinkValue::Up), t0) && state.link_text() == "up");
    // The same value again is no change.
    CHECK(!state.on_link(link_of(OutsideLinkValue::Up), t0 + 1s));
    CHECK(state.on_link(link_of(OutsideLinkValue::Down), t0 + 10s));
    CHECK(state.on_link(link_of(OutsideLinkValue::Up), t0 + 12s + 700ms));
    // Down again, and still down when the summary is asked: it counts to then.
    CHECK(state.on_link(link_of(OutsideLinkValue::Down), t0 + 20s));
    const OutsideLink sum = state.summary(t0 + 21s);
    CHECK(sum.down_count == 2 && sum.down_total == 3700ms);
    CHECK(sum.bus_off_count == 0 && sum.passive_total == 0ms && sum.lost.empty());
    // A line without the key leaves the value alone.
    CHECK(state.on_link(bus_of(OutsideBusValue::Unknown), t0 + 22s));
    CHECK(state.link_text() == "down");
}

TEST_CASE(outside_state_does_not_count_unknown_as_down)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_link(link_of(OutsideLinkValue::Unknown), t0);
    state.on_link(bus_of(OutsideBusValue::Unknown), t0);
    CHECK(state.summary(t0 + 100s).down_count == 0 && state.summary(t0 + 100s).bus_off_count == 0);
    CHECK(state.lost(t0 + 100s, 30s).empty());
    // Down, then unknown: the span ends there.
    state.on_link(link_of(OutsideLinkValue::Down), t0 + 100s);
    state.on_link(link_of(OutsideLinkValue::Unknown), t0 + 101s);
    const OutsideLink sum = state.summary(t0 + 200s);
    CHECK(sum.down_count == 1 && sum.down_total == 1s);
    CHECK(state.lost(t0 + 200s, 30s).empty());
}

TEST_CASE(outside_state_counts_bus_off_and_passive_apart)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_link(bus_of(OutsideBusValue::Active), t0);
    state.on_link(bus_of(OutsideBusValue::Passive), t0 + 1s);
    state.on_link(bus_of(OutsideBusValue::BusOff), t0 + 3s);   // passive 2 s
    state.on_link(bus_of(OutsideBusValue::Active), t0 + 4500ms); // bus off 1.5 s
    state.on_link(bus_of(OutsideBusValue::Passive), t0 + 5s);
    const OutsideLink sum = state.summary(t0 + 6s);
    CHECK(sum.bus_off_count == 1 && sum.bus_off_total == 1500ms);
    CHECK(sum.passive_total == 3s);
    CHECK(sum.down_count == 0);
    CHECK(state.bus_text() == "passive");
}

TEST_CASE(outside_state_says_lost_once_the_link_or_the_bus_has_been_down_for_the_limit)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_link(link_of(OutsideLinkValue::Down), t0);
    CHECK(state.lost(t0 + 29999ms, 30s).empty());
    CHECK(state.lost(t0 + 30s, 30s) == "link down for 30 s");
    // Back up: no more, and the next down starts again at 0.
    state.on_link(link_of(OutsideLinkValue::Up), t0 + 31s);
    CHECK(state.lost(t0 + 100s, 30s).empty());
    state.on_link(link_of(OutsideLinkValue::Down), t0 + 100s);
    CHECK(state.lost(t0 + 120s, 30s).empty());
    // A short limit (the tests').
    CHECK(state.lost(t0 + 101s, 1s) == "link down for 1 s");
    OutsideState can;
    can.on_link(bus_of(OutsideBusValue::BusOff), t0);
    CHECK(can.lost(t0 + 30s, 30s) == "bus off for 30 s");
    // Passive is not down.
    OutsideState passive;
    passive.on_link(bus_of(OutsideBusValue::Passive), t0);
    CHECK(passive.lost(t0 + 1h, 30s).empty());
}

TEST_CASE(dropped_counts_names_the_keys_of_the_table_that_are_not_zero)
{
    std::map<std::string, std::uint64_t> counts{{"ext_queue_full", 3},     {"ext_overruns", 0},   {"xl_tx_unsent", 1},
                                                {"silkit_rejected", 2},    {"own_tx_dropped", 9}, {"filtered_to_ext", 4},
                                                {"oversize", 5},           {"malformed", 6},      {"mac_unguarded", 7},
                                                {"ext_checksum_incomplete", 8}, {"loop_suspected", 1}, {"error_frames", 1}};
    const auto dropped = launch::detail::dropped_counts(counts);
    CHECK(dropped.size() == 3);
    if (dropped.size() != 3)
    {
        return;
    }
    CHECK(dropped[0] == std::make_pair(std::string("ext_queue_full"), std::uint64_t{3}));
    CHECK(dropped[1].first == "xl_tx_unsent" && dropped[2].first == "silkit_rejected");
    // Every key of the table counts, and only those.
    for (const std::string_view key : launch::detail::kOutsideDroppedKeys)
    {
        CHECK(launch::detail::dropped_counts({{std::string(key), 1}}).size() == 1);
    }
    CHECK(launch::detail::dropped_counts({}).empty());
}
