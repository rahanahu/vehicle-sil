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
    CHECK(reads(kXl, "[bridge] stats silkit_to_ext=40 ext_from_devices=7 own_tx_dropped=40", 40, 7));
    // Without the tag, and with the "\r" of a Windows line end.
    CHECK(reads(kEth, "stats silkit_to_ext=1 ext_from_devices=0", 1, 0));
    CHECK(reads(kXl, "[bridge] stats silkit_to_ext=0 ext_from_devices=0\r", 0, 0));
    // The largest count that the JSON forms write (a signed 64-bit integer), and leading zeros.
    CHECK(reads(kEth, "stats silkit_to_ext=9223372036854775807 ext_from_devices=007", 9223372036854775807u, 7));
}

TEST_CASE(read_outside_counts_refuses_any_other_line)
{
    // Of a bridge of another version (no ext_from_devices).
    CHECK(refuses(kEth, "[eth-bridge] stats silkit_to_ext=12 ext_to_silkit=11 own_tx_dropped=0"));
    // The names the CAN bridge used before: a bridge of another release.
    CHECK(refuses(kXl, "[bridge] stats silkit_to_xl=40 xl_to_silkit=7 own_tx_dropped=40"));
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
using launch::detail::OutsideForeign;
using launch::detail::OutsideOwn;
using launch::detail::OutsideSharing;
using launch::detail::OutsideState;
using launch::detail::foreign_sharing;
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
    // The sharing keys.
    line = read_outside_link_line("[eth-bridge] link link=up device=ab12 marked=1 shared=2 peers=4321,4400");
    CHECK(line && line->device == "ab12" && line->marked == true && line->shared == 2u && line->peers == std::vector<long>({4321, 4400}));
    line = read_outside_link_line("link bus=active device=ff marked=0 shared=0");
    CHECK(line && line->device == "ff" && line->marked == false && line->shared == 0u && line->peers.empty());
    // A value that is not what the key says leaves the key out; the others stay.
    line = read_outside_link_line("link device= marked=2 shared=x peers=1,,2 link=up");
    CHECK(line && !line->device && !line->marked && !line->shared && line->peers.empty() && line->link == OutsideLinkValue::Up);
    line = read_outside_link_line("link peers=4321,abc shared=-1");
    CHECK(line && line->peers.empty() && !line->shared);
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
    std::map<std::string, std::uint64_t> counts{{"ext_queue_full", 3},     {"ext_overruns", 0},   {"ext_tx_unsent", 1},
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
    CHECK(dropped[1].first == "ext_tx_unsent" && dropped[2].first == "silkit_rejected");
    // Every key of the table counts, and only those.
    for (const std::string_view key : launch::detail::kOutsideDroppedKeys)
    {
        CHECK(launch::detail::dropped_counts({{std::string(key), 1}}).size() == 1);
    }
    CHECK(launch::detail::dropped_counts({}).empty());
}

namespace {

OutsideSharing sharing_of(std::string device, bool marked, std::uint32_t shared, std::vector<long> peers = {})
{
    return OutsideSharing{true, std::move(device), marked, shared, std::move(peers)};
}

} // namespace

TEST_CASE(foreign_sharing_takes_the_pids_of_the_runs_own_bridges_from_the_peers)
{
    // Bridge 0 and 1 are the run's (pids 100 and 101) and share a device; 2 is not a bridge.
    const std::vector<OutsideOwn> own{{true, 100}, {true, 101}, {}};
    // Only the run's own bridge: nothing foreign.
    std::vector<OutsideSharing> all{sharing_of("d", true, 1, {101}), sharing_of("d", true, 1, {100}), {}};
    CHECK(foreign_sharing(0, all, own).count == 0);
    // A bridge outside the run as well: it, and its pid.
    all[0] = sharing_of("d", true, 2, {101, 4321});
    OutsideForeign out = foreign_sharing(0, all, own);
    CHECK(out.count == 1 && out.pids == std::vector<long>({4321}));
    // One of the run's own that has ended is still in the peers of the other: not foreign either.
    const std::vector<OutsideOwn> ended{{true, 100}, {false, 101}, {}};
    all[0] = sharing_of("d", true, 1, {101});
    CHECK(foreign_sharing(0, all, ended).count == 0);
    // Two foreign.
    all[0] = sharing_of("d", true, 3, {4400, 101, 4321});
    out = foreign_sharing(0, all, own);
    CHECK(out.count == 2 && out.pids == std::vector<long>({4400, 4321}));
    // Not shared, or no line yet: none.
    all[0] = sharing_of("d", true, 0);
    CHECK(foreign_sharing(0, all, own).count == 0);
    all[0] = OutsideSharing{};
    CHECK(foreign_sharing(0, all, own).count == 0);
}

TEST_CASE(foreign_sharing_counts_without_the_pids_the_runs_own_live_marked_bridges_on_the_same_device)
{
    // As on Windows: shared, no peers.
    const std::vector<OutsideOwn> own{{true, 100}, {true, 101}, {}};
    std::vector<OutsideSharing> all{sharing_of("d", true, 1), sharing_of("d", true, 1), {}};
    // One's own: the other bridge of the run.
    CHECK(foreign_sharing(0, all, own).count == 0);
    // One more outside.
    all[0] = sharing_of("d", true, 2);
    OutsideForeign out = foreign_sharing(0, all, own);
    CHECK(out.count == 1 && out.pids.empty());
    // The other bridge of the run has ended, or sits on another device, or placed no mark: it is not subtracted.
    CHECK(foreign_sharing(0, all, {{true, 100}, {false, 101}, {}}).count == 2);
    all[1] = sharing_of("other", true, 1);
    CHECK(foreign_sharing(0, all, own).count == 2);
    all[1] = sharing_of("d", false, 1);
    CHECK(foreign_sharing(0, all, own).count == 2);
    // The other bridge of the run has not written its first line: cannot tell yet.
    all[1] = OutsideSharing{};
    CHECK(foreign_sharing(0, all, own).count == 0);
    // Never below 0 (the other has gone but the report is not renewed).
    all[0] = sharing_of("d", true, 1);
    all[1] = sharing_of("d", true, 1);
    CHECK(foreign_sharing(0, all, {{true, 100}, {true, 101}, {}}).count == 0);
    // A live bridge whose process id is not known: the numbers decide although the peers are there.
    all[0] = sharing_of("d", true, 2, {101, 4321});
    CHECK(foreign_sharing(0, all, {{true, 100}, {true, 0}, {}}).count == 1);
}

TEST_CASE(shared_text_names_the_bridges_and_their_pids)
{
    CHECK(launch::detail::shared_text({1, {4321}}) == "another vehicle-sil bridge (process 4321)");
    CHECK(launch::detail::shared_text({2, {4321, 4400}}) == "2 other vehicle-sil bridges (processes 4321, 4400)");
    CHECK(launch::detail::shared_text({1, {}}) == "another vehicle-sil bridge");
    CHECK(launch::detail::shared_text({3, {}}) == "3 other vehicle-sil bridges");
}

TEST_CASE(is_shared_warning_takes_the_warnings_of_the_bridges_about_a_device_in_use)
{
    CHECK(launch::detail::is_shared_warning("[eth-bridge] warning: interface 'fake0' is also used by another vehicle-sil bridge (process 4321): the frames of both mix there: ..."));
    CHECK(launch::detail::is_shared_warning("[bridge] warning: XL 'Virtual Channel 1' is also used by 2 more vehicle-sil bridges (processes 1, 2): ..."));
    CHECK(launch::detail::is_shared_warning("warning: x is also used by another vehicle-sil bridge"));
    CHECK(!launch::detail::is_shared_warning("[bridge] warning: --tx-timeout-ms is not used: it needs init access"));
    CHECK(!launch::detail::is_shared_warning("[bridge] link link=up device=ab marked=1 shared=1"));
    CHECK(!launch::detail::is_shared_warning("[bridge] FAIL: interface is also used by another vehicle-sil bridge"));
}

TEST_CASE(outside_state_keeps_the_device_and_the_bridges_outside_the_run)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    CHECK(!state.link_line_seen());
    OutsideLinkLine line;
    line.device = "ab12";
    line.marked = true;
    line.shared = 1;
    line.peers = {4321};
    state.on_link(line, t0);
    CHECK(state.link_line_seen() && state.sharing().device == "ab12" && state.sharing().shared == 1 && state.sharing().peers == std::vector<long>({4321}));
    // A line that says shared again replaces the peers (no peers key: none).
    line.shared = 0;
    line.peers = {};
    state.on_link(line, t0 + 1s);
    CHECK(state.sharing().shared == 0 && state.sharing().peers.empty());
    // A line with no sharing keys leaves them as they were.
    state.on_link(link_of(OutsideLinkValue::Up), t0 + 2s);
    CHECK(state.sharing().device == "ab12");

    // The first sight is told, the same count is not told again, a larger one is.
    CHECK(!state.shared_for(t0 + 1h, 0ms));
    CHECK(state.on_foreign({1, {4321}}, t0 + 3s, 3100ms));
    CHECK(!state.on_foreign({1, {4321}}, t0 + 4s, 4100ms));
    CHECK(state.on_foreign({2, {4321, 4400}}, t0 + 5s, 5100ms));
    // For the limit: from the first sight on, counted again after a gap.
    CHECK(!state.shared_for(t0 + 5s + 999ms, 3s));
    CHECK(state.shared_for(t0 + 6s, 3s));
    CHECK(!state.shared_for(t0 + 5s, 3s));
    CHECK(!state.on_foreign({}, t0 + 6s, 6100ms));
    CHECK(!state.shared_for(t0 + 1h, 3s));
    CHECK(!state.on_foreign({1, {4321}}, t0 + 10s, 10100ms));
    CHECK(!state.shared_for(t0 + 12s, 3s));
    CHECK(state.shared_for(t0 + 13s, 3s));
    // What the result gets.
    state.stop_shared(OutsideLink::SharedStop::AfterDelay);
    const OutsideLink sum = state.summary(t0 + 20s);
    CHECK(sum.device == "ab12" && sum.shared_most == 2 && sum.shared_peers == std::vector<long>({4321, 4400}));
    CHECK(sum.shared_first == 3100ms && sum.shared_stop == OutsideLink::SharedStop::AfterDelay);
}

namespace {

using launch::detail::kOutsideDegradedHold;
using launch::detail::kOutsideNothingReceivedAfter;
using launch::detail::sent_without_answer;

using Counts = std::map<std::string, std::uint64_t>;
using Reasons = std::vector<std::string>;

OutsideLinkLine line_of(OutsideLinkValue link, OutsideBusValue bus)
{
    return OutsideLinkLine{link, bus};
}

} // namespace

TEST_CASE(outside_facts_are_ok_without_a_reason_until_something_is_seen)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    CHECK(state.facts(t0).condition == "ok" && state.facts(t0).reasons.empty());
    CHECK(state.facts(t0).link.empty() && state.facts(t0).bus.empty() && !state.facts(t0).nothing_received);
    state.on_link(line_of(OutsideLinkValue::Up, OutsideBusValue::Active), t0);
    const auto up = state.facts(t0 + 1s);
    CHECK(up.condition == "ok" && up.reasons.empty() && up.link == "up" && up.bus == "active");
}

TEST_CASE(outside_facts_say_interrupted_for_a_link_that_is_down_or_a_bus_that_is_off)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_link(line_of(OutsideLinkValue::Down, OutsideBusValue::Active), t0);
    auto f = state.facts(t0);
    CHECK(f.condition == "interrupted" && f.reasons == Reasons({"link-down"}));
    state.on_link(line_of(OutsideLinkValue::Up, OutsideBusValue::BusOff), t0 + 1s);
    f = state.facts(t0 + 1s);
    CHECK(f.condition == "interrupted" && f.reasons == Reasons({"bus-off"}));
    state.on_link(line_of(OutsideLinkValue::Up, OutsideBusValue::Active), t0 + 2s);
    CHECK(state.facts(t0 + 2s).condition == "ok");
}

TEST_CASE(outside_facts_say_degraded_for_a_passive_bus_as_long_as_it_is_passive)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_link(line_of(OutsideLinkValue::Up, OutsideBusValue::Passive), t0);
    // Not held for 5 s only: for as long as the bus is passive.
    auto f = state.facts(t0 + 1h);
    CHECK(f.condition == "degraded" && f.reasons == Reasons({"bus-errors"}));
    state.on_link(line_of(OutsideLinkValue::Up, OutsideBusValue::Active), t0 + 1h + 1s);
    CHECK(state.facts(t0 + 1h + 1s).condition == "ok");
}

TEST_CASE(outside_facts_keep_dropped_frames_and_loops_for_five_seconds_after_the_count_grew)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_counts(Counts{{"ext_queue_full", 0}, {"loop_suspected", 0}}, t0);
    CHECK(state.facts(t0).condition == "ok");
    state.on_counts(Counts{{"ext_queue_full", 3}, {"loop_suspected", 0}}, t0 + 1s);
    CHECK(state.facts(t0 + 1s).reasons == Reasons({"frames-dropped"}));
    // The same count again does not renew the hold; the hold ends 5 s after the growth.
    state.on_counts(Counts{{"ext_queue_full", 3}, {"loop_suspected", 0}}, t0 + 3s);
    CHECK(state.facts(t0 + 1s + kOutsideDegradedHold - 1ms).condition == "degraded");
    CHECK(state.facts(t0 + 1s + kOutsideDegradedHold).condition == "ok");
    // A count that grows again renews it; the loops have their own reason, after the dropped one.
    state.on_counts(Counts{{"ext_queue_full", 4}, {"loop_suspected", 1}}, t0 + 10s);
    CHECK(state.facts(t0 + 10s).reasons == Reasons({"frames-dropped", "loop-suspected"}));
    CHECK(state.facts(t0 + 10s).condition == "degraded");
    // Keys that are no losses do not count.
    OutsideState other;
    other.on_counts(Counts{{"oversize", 9}, {"filtered_to_ext", 9}}, t0);
    CHECK(other.facts(t0).condition == "ok");
}

TEST_CASE(outside_facts_say_degraded_while_a_bridge_outside_the_run_shares_the_device)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    CHECK(state.on_foreign({1, {4321}}, t0, 100ms));
    // Not 5 s: as long as the sharing lasts.
    CHECK(state.facts(t0 + 1h).reasons == Reasons({"device-shared"}));
    CHECK(state.facts(t0 + 1h).condition == "degraded");
    state.on_foreign({}, t0 + 1h + 1s, 1h);
    CHECK(state.facts(t0 + 1h + 1s).condition == "ok");
}

TEST_CASE(outside_facts_put_interrupted_before_degraded_and_name_the_reasons_of_both_in_order)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_link(line_of(OutsideLinkValue::Down, OutsideBusValue::Passive), t0);
    state.on_counts(Counts{{"overruns", 1}, {"loop_suspected", 2}}, t0);
    state.on_foreign({1, {}}, t0, 1s);
    const auto f = state.facts(t0 + 1s);
    CHECK(f.condition == "interrupted");
    CHECK(f.reasons == Reasons({"link-down", "bus-errors", "frames-dropped", "loop-suspected", "device-shared"}));
}

TEST_CASE(outside_facts_say_nothing_received_ten_seconds_after_the_first_frame_to_the_bus_if_none_came)
{
    const auto t0 = Clock::time_point{} + 1h;
    OutsideState state;
    state.on_counts(Counts{{"silkit_to_ext", 0}, {"ext_from_devices", 0}}, t0);
    CHECK(!state.facts(t0 + 1h).nothing_received); // nothing was sent
    state.on_counts(Counts{{"silkit_to_ext", 5}, {"ext_from_devices", 0}}, t0 + 2s);
    CHECK(!state.facts(t0 + 2s + kOutsideNothingReceivedAfter - 1ms).nothing_received);
    CHECK(state.facts(t0 + 2s + kOutsideNothingReceivedAfter).nothing_received);
    // Nothing_received is no reason of the condition.
    CHECK(state.facts(t0 + 1h).condition == "ok");
    // Something came: it is over.
    state.on_counts(Counts{{"silkit_to_ext", 6}, {"ext_from_devices", 1}}, t0 + 20s);
    CHECK(!state.facts(t0 + 20s).nothing_received);
    // The condition of the note at the end is the same function.
    CHECK(sent_without_answer(5, 0) && !sent_without_answer(5, 1) && !sent_without_answer(0, 0) &&
          !sent_without_answer(std::nullopt, 0) && !sent_without_answer(5, std::nullopt));
}
