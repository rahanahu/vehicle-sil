// SPDX-License-Identifier: MIT
//
// The statistics line and the monitor line: their wording is documented (docs/ethernet-bridge.md).
#include "check_text.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <cstdint>

namespace {

using namespace vehicle_sil::ethernet_bridge;
using vehicle_sil::eth_link::LinkState;

EthernetBridgeHealth health(LinkState state, EthernetBridgeStatistics statistics = {})
{
    return EthernetBridgeHealth{statistics, state};
}

} // namespace

TEST_CASE(format_statistics_names_every_counter)
{
    EthernetBridgeStatistics s;
    s.silkit_to_ext = 1;
    s.ext_to_silkit = 2;
    s.own_tx_dropped = 3;
    s.loop_suspected = 4;
    s.mac_unguarded = 13;
    s.filtered_to_ext = 5;
    s.filtered_from_ext = 6;
    s.oversize = 7;
    s.malformed = 8;
    s.ext_overruns = 9;
    s.ext_queue_full = 10;
    s.ext_tx_failures = 11;
    s.silkit_tx_failures = 12;
    s.ext_checksum_incomplete = 14;
    CHECK_TEXT(format_statistics(s),
               "stats silkit_to_ext=1 ext_to_silkit=2 own_tx_dropped=3 loop_suspected=4 mac_unguarded=13 filtered_to_ext=5 "
               "filtered_from_ext=6 oversize=7 malformed=8 ext_overruns=9 ext_queue_full=10 ext_tx_failures=11 "
               "silkit_tx_failures=12 ext_checksum_incomplete=14");
}

TEST_CASE(problems_grew_ignores_forwarded_filtered_and_own_frames)
{
    EthernetBridgeStatistics before;
    EthernetBridgeStatistics now;
    now.silkit_to_ext = 100;
    now.ext_to_silkit = 100;
    now.own_tx_dropped = 100;
    now.filtered_to_ext = 100;
    now.filtered_from_ext = 100;
    CHECK(!problems_grew(before, now));
    for (auto member : {&EthernetBridgeStatistics::loop_suspected, &EthernetBridgeStatistics::mac_unguarded,
                        &EthernetBridgeStatistics::oversize,
                        &EthernetBridgeStatistics::malformed, &EthernetBridgeStatistics::ext_overruns,
                        &EthernetBridgeStatistics::ext_queue_full, &EthernetBridgeStatistics::ext_tx_failures,
                        &EthernetBridgeStatistics::silkit_tx_failures,
                        &EthernetBridgeStatistics::ext_checksum_incomplete})
    {
        auto grown = now;
        grown.*member += 1;
        CHECK(problems_grew(now, grown));
    }
}

TEST_CASE(format_health_change_is_quiet_for_a_healthy_link)
{
    CHECK(!format_health_change(health(LinkState::Unknown), health(LinkState::Up), "enp3s0").has_value());
    CHECK(!format_health_change(health(LinkState::Up), health(LinkState::Up), "enp3s0").has_value());
    CHECK(!format_health_change(health(LinkState::Unknown), health(LinkState::Unknown), "enp3s0").has_value());
}

TEST_CASE(format_health_change_reports_link_changes_and_problems)
{
    CHECK_TEXT(*format_health_change(health(LinkState::Up), health(LinkState::Down), "enp3s0"), "interface enp3s0: link down");
    CHECK_TEXT(*format_health_change(health(LinkState::Down), health(LinkState::Up), "enp3s0"), "interface enp3s0: link up");
    CHECK_TEXT(*format_health_change(health(LinkState::Unknown), health(LinkState::Down), "enp3s0"),
               "interface enp3s0: link down");

    EthernetBridgeStatistics before;
    before.ext_tx_failures = 5;
    EthernetBridgeStatistics now = before;
    now.ext_tx_failures = 17;
    now.loop_suspected = 1;
    now.silkit_to_ext = 1000; // not a problem
    CHECK_TEXT(*format_health_change(health(LinkState::Up, before), health(LinkState::Down, now), "enp3s0"),
               "interface enp3s0: link down; since the last report: loop_suspected +1, ext_tx_failures +12");
}

TEST_CASE(format_health_change_says_when_the_problems_stopped)
{
    CHECK_TEXT(*format_health_change(health(LinkState::Up), health(LinkState::Up), "enp3s0", true),
               "interface enp3s0: link up; no new problems since the last report");
    CHECK_TEXT(*format_health_change(health(LinkState::Unknown), health(LinkState::Unknown), "vsil0", true),
               "interface vsil0: link state unknown; no new problems since the last report");
}
