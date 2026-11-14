// SPDX-License-Identifier: MIT
//
// EthernetBridge between a fake SIL Kit controller and fake links: forwarding, the filter, the
// MAC rule (own frames, addresses on both sides, a loop through two bridges), lost frames,
// faults, warnings, start and stop.
#include "check_text.hpp"
#include "fake_ethernet_controller.hpp"
#include "fake_link.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil::ethernet_bridge;
using vehicle_sil::eth_link::LinkError;
using vehicle_sil::eth_link::LinkErrorKind;
using vehicle_sil::eth_link::LinkState;
using vehicle_sil::eth_link::TransmitResult;
using vehicle_sil::fake_eth::Bytes;
using vehicle_sil::fake_eth::FakeLink;
using vehicle_sil::fake_eth::FakeSegment;
using vehicle_sil::fake_eth::make_frame;
using vehicle_sil::fake_eth::make_tagged_frame;
using vehicle_sil::unit::eventually;
using vehicle_sil::unit::FakeEthernetController;

// Locally administered addresses; 0x88B5 is an EtherType known to be free for local experiments
// (not checked against the standard).
constexpr std::uint16_t kExperimental = 0x88B5;
const char* const kEcu = "02:00:00:00:00:01";
const char* const kOutside = "02:00:00:00:00:99";
const char* const kBroadcast = "ff:ff:ff:ff:ff:ff";

/// "02:00:00:00:01:nn"
std::string numbered_mac(unsigned n)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    return std::string("02:00:00:00:01:") + kDigits[(n >> 4) & 0xF] + kDigits[n & 0xF];
}

struct Fixture
{
    FakeSegment segment;
    FakeLink link{segment, "eth-test"};
    FakeEthernetController silkit;
    EthernetBridge bridge;

    explicit Fixture(EthernetBridgeOptions options = {}) :
      bridge(silkit, link, std::move(options))
    {
        bridge.start();
    }

    /// Everything sent so far went through.
    [[nodiscard]] bool settled() { return eventually([&] { return segment.idle(); }, 5000ms); }
};

} // namespace

TEST_CASE(bridge_carries_frames_both_ways_unchanged)
{
    Fixture f;
    CHECK(f.silkit.active());
    CHECK_EQ(f.silkit.frame_handler_mask(), static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));

    const auto out = make_frame(kOutside, kEcu, kExperimental, 100, 7);
    const auto tagged = make_tagged_frame(kOutside, kEcu, 0x8100, 0x2005, kExperimental, 60);
    f.silkit.deliver(out);
    f.silkit.deliver(tagged);
    const auto transmitted = f.segment.transmitted();
    CHECK_EQ(transmitted.size(), std::size_t{2});
    CHECK(transmitted.at(0) == out);
    CHECK(transmitted.at(1) == tagged);

    const auto in = make_frame(kEcu, kOutside, kExperimental, 46, 3);
    f.segment.send(in);
    CHECK(eventually([&] { return f.silkit.sent_count() == 1; }, 5000ms));
    CHECK(f.silkit.sent().at(0) == in);

    CHECK(f.settled());
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{2});
    CHECK_EQ(s.ext_to_silkit, std::uint64_t{1});
    CHECK_EQ(s.own_tx_dropped, std::uint64_t{0});
}

TEST_CASE(bridge_drops_its_own_frames_when_the_link_delivers_them_back)
{
    Fixture f;
    f.link.set_echo_own(true); // as Npcap does
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        f.silkit.deliver(make_frame(kBroadcast, kEcu, kExperimental, 46, i));
    }
    CHECK(eventually([&] { return f.bridge.statistics().own_tx_dropped == 5 && f.segment.idle(); }, 5000ms));
    CHECK_EQ(f.silkit.sent_count(), std::size_t{0});
    CHECK_EQ(f.bridge.statistics().silkit_to_ext, std::uint64_t{5});
    CHECK(f.bridge.take_warnings().empty());
}

TEST_CASE(bridge_without_mac_guard_forwards_what_the_link_delivers_back)
{
    EthernetBridgeOptions options;
    options.mac_guard = false;
    Fixture f(options);
    f.link.set_echo_own(true);
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        f.silkit.deliver(make_frame(kBroadcast, kEcu, kExperimental, 46, i));
    }
    CHECK(eventually([&] { return f.silkit.sent_count() == 5; }, 5000ms));
    CHECK_EQ(f.bridge.statistics().own_tx_dropped, std::uint64_t{0});
}

TEST_CASE(bridge_drops_frames_from_an_address_on_both_sides_and_warns_once)
{
    Fixture f;
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(eventually([&] { return f.silkit.sent_count() == 1; }, 5000ms));
    // The outside device's address now comes from SIL Kit too.
    f.silkit.deliver(make_frame(kBroadcast, kOutside, kExperimental));
    CHECK_EQ(f.bridge.statistics().loop_suspected, std::uint64_t{1});
    // From now on frames from it are dropped both ways.
    f.silkit.deliver(make_frame(kBroadcast, kOutside, kExperimental));
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(f.settled());
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.loop_suspected, std::uint64_t{3});
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{0});
    CHECK_EQ(s.ext_to_silkit, std::uint64_t{1});
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_TEXT(warnings.at(0),
               "MAC 02:00:00:00:00:99 is on both sides: frames from it came from the external link and now come from "
               "SIL Kit, so frames from it are dropped both ways (loop_suspected). Run one bridge between these "
               "networks, and give each device its own MAC address.");
    // Other addresses still pass.
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    CHECK_EQ(f.bridge.statistics().silkit_to_ext, std::uint64_t{1});
}

TEST_CASE(bridge_names_a_limited_number_of_addresses_on_both_sides)
{
    Fixture f;
    const unsigned count = static_cast<unsigned>(kMaxNamedConflicts) + 3;
    for (unsigned n = 0; n < count; ++n)
    {
        f.segment.send(make_frame(kBroadcast, numbered_mac(n), kExperimental));
    }
    CHECK(eventually([&] { return f.silkit.sent_count() == count; }, 5000ms));
    for (unsigned n = 0; n < count; ++n)
    {
        f.silkit.deliver(make_frame(kBroadcast, numbered_mac(n), kExperimental));
    }
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), kMaxNamedConflicts + 1);
    CHECK(warnings.front().starts_with("MAC 02:00:00:00:01:00 is on both sides"));
    CHECK_TEXT(warnings.back(), "more MAC addresses are on both sides than the first 8 named: frames from them are "
                                "dropped both ways too (loop_suspected).");
    CHECK_EQ(f.bridge.statistics().loop_suspected, std::uint64_t{count});
}

TEST_CASE(bridge_stops_a_loop_through_two_bridges)
{
    // Two bridges between the same SIL Kit network (c1, c2 wired to each other) and the same
    // wire. The ECU's frame reaches bridge 1 first; bridge 1 sends it out, bridge 2 takes it in
    // from the wire, bridge 1 sends it out again: the loop runs until bridge 2 also gets the
    // frame from SIL Kit and sees the address on both sides.
    FakeSegment segment;
    FakeLink link1(segment, "b1");
    FakeLink link2(segment, "b2");
    FakeEthernetController c1;
    FakeEthernetController c2;
    c1.on_send = [&](std::span<const std::uint8_t> frame) { c2.deliver(frame); };
    c2.on_send = [&](std::span<const std::uint8_t> frame) { c1.deliver(frame); };
    EthernetBridge b1(c1, link1);
    EthernetBridge b2(c2, link2);
    b1.start();
    b2.start();

    const auto frame = make_frame(kBroadcast, kEcu, kExperimental);
    c1.deliver(frame);
    CHECK(eventually([&] { return b2.statistics().ext_to_silkit >= 2; }, 5000ms)); // going around
    c2.deliver(frame);
    // Nothing in flight any more: the loop ended.
    CHECK(eventually([&] { return segment.idle(); }, 5000ms));
    CHECK(b2.statistics().loop_suspected >= 1);
    CHECK_EQ(b1.statistics().loop_suspected, std::uint64_t{0});
    const auto warnings = b2.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(warnings.at(0).starts_with("MAC 02:00:00:00:00:01 is on both sides"));
    b1.stop();
    b2.stop();
}

TEST_CASE(bridge_filters_both_ways_and_counts)
{
    EthernetBridgeOptions options;
    options.filter.ether_types = {kExperimental};
    Fixture f(options);
    f.silkit.deliver(make_frame(kOutside, kEcu, 0x0800));
    f.silkit.deliver(make_tagged_frame(kOutside, kEcu, 0x8100, 5, kExperimental));
    f.segment.send(make_frame(kEcu, kOutside, 0x86DD));
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(eventually([&] { return f.silkit.sent_count() == 1; }, 5000ms));
    CHECK(f.settled());
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.filtered_to_ext, std::uint64_t{1});
    CHECK_EQ(s.filtered_from_ext, std::uint64_t{1});
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{1});
    CHECK_EQ(s.ext_to_silkit, std::uint64_t{1});
    CHECK_EQ(f.segment.transmitted().size(), std::size_t{1});
    CHECK_TEXT(f.bridge.filter().describe(), "EtherType 0x88b5 only, both ways");
}

TEST_CASE(bridge_silkit_macs_select_the_source_out_and_the_destination_in)
{
    EthernetBridgeOptions options;
    options.filter.silkit_macs = {*vehicle_sil::eth_link::parse_mac_address(kEcu)};
    Fixture f(options);
    const auto fromEcu = make_frame(kOutside, kEcu, kExperimental);
    f.silkit.deliver(fromEcu);                                                // from the ECU: out
    f.silkit.deliver(make_frame(kEcu, "02:00:00:00:00:02", kExperimental)); // another source: filtered
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));              // to the ECU: in
    f.segment.send(make_frame(kBroadcast, kOutside, kExperimental));        // broadcast: in
    f.segment.send(make_frame("02:00:00:00:00:03", kOutside, kExperimental)); // to someone else: filtered
    CHECK(eventually([&] { return f.silkit.sent_count() == 2 && f.segment.idle(); }, 5000ms));
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{1});
    CHECK_EQ(s.filtered_to_ext, std::uint64_t{1});
    CHECK_EQ(s.ext_to_silkit, std::uint64_t{2});
    CHECK_EQ(s.filtered_from_ext, std::uint64_t{1});
    CHECK_EQ(f.segment.transmitted().size(), std::size_t{1});
    CHECK(f.segment.transmitted().at(0) == fromEcu);
}

TEST_CASE(bridge_filtered_frames_are_not_learned)
{
    // A frame the filter drops does not teach the table: a later SIL Kit frame from the same
    // address is not taken for one on both sides.
    EthernetBridgeOptions options;
    options.filter.ether_types = {kExperimental};
    Fixture f(options);
    f.segment.send(make_frame(kEcu, kOutside, 0x86DD));
    CHECK(eventually([&] { return f.bridge.statistics().filtered_from_ext == 1 && f.segment.idle(); }, 5000ms));
    f.silkit.deliver(make_frame(kBroadcast, kOutside, kExperimental));
    CHECK_EQ(f.bridge.statistics().silkit_to_ext, std::uint64_t{1});
    CHECK_EQ(f.bridge.statistics().loop_suspected, std::uint64_t{0});
}

TEST_CASE(bridge_counts_oversize_and_malformed_frames)
{
    EthernetBridgeOptions options;
    options.filter.max_frame_bytes = 100;
    Fixture f(options);
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental, 87)); // 101 bytes
    f.silkit.deliver(Bytes(13, 0x02));
    f.segment.send(make_frame(kEcu, kOutside, kExperimental, 87));
    f.segment.send(Bytes(10, 0x02));
    f.segment.send(make_frame(kEcu, kOutside, kExperimental, 86)); // 100 bytes: passes
    CHECK(eventually([&] { return f.silkit.sent_count() == 1; }, 5000ms));
    CHECK(f.settled());
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.oversize, std::uint64_t{2});
    CHECK_EQ(s.malformed, std::uint64_t{2});
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{0});
    CHECK_EQ(s.ext_to_silkit, std::uint64_t{1});
}

TEST_CASE(bridge_warns_once_when_the_mac_table_is_full)
{
    EthernetBridgeOptions options;
    options.mac_table_size = 16;
    Fixture f(options);
    for (unsigned n = 0; n < 20; ++n)
    {
        f.silkit.deliver(make_frame(kBroadcast, numbered_mac(n), kExperimental));
    }
    CHECK_EQ(f.bridge.statistics().silkit_to_ext, std::uint64_t{20}); // forwarded all the same
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_TEXT(warnings.at(0), "the MAC table is full (16 addresses): frames from addresses seen from now on are "
                               "forwarded without the check against loops. Raise --mac-table-size if this many "
                               "devices are expected.");
}

TEST_CASE(bridge_counts_what_the_link_could_not_send)
{
    Fixture f;
    f.link.set_transmit_result(TransmitResult::QueueFull);
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    f.link.set_transmit_result(TransmitResult::Failed);
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.ext_queue_full, std::uint64_t{1});
    CHECK_EQ(s.ext_tx_failures, std::uint64_t{2});
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{0});
    CHECK_TEXT(f.bridge.last_error(), "sending on the external link failed: fake link 'eth-test' refused the frame");
}

TEST_CASE(bridge_counts_what_sil_kit_did_not_transmit)
{
    Fixture f;
    f.silkit.ack_status = SilKit::Services::Ethernet::EthernetTransmitStatus::LinkDown;
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(eventually([&] { return f.bridge.statistics().silkit_tx_failures == 1 && f.segment.idle(); }, 5000ms));
    CHECK_TEXT(f.bridge.last_error(), "SIL Kit did not transmit a frame from the external link: the link is down");

    f.silkit.ack_status = SilKit::Services::Ethernet::EthernetTransmitStatus::Transmitted;
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(eventually([&] { return f.silkit.sent_count() == 2 && f.segment.idle(); }, 5000ms));
    CHECK_EQ(f.bridge.statistics().silkit_tx_failures, std::uint64_t{1});

    f.silkit.throw_on_send = true;
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(eventually([&] { return f.bridge.statistics().silkit_tx_failures == 2 && f.segment.idle(); }, 5000ms));
    CHECK_TEXT(f.bridge.last_error(), "SIL Kit SendFrame failed: fake SendFrame failure");
}

TEST_CASE(bridge_counts_overruns_also_those_reported_by_stop)
{
    Fixture f;
    f.link.set_overruns_at_stop(3);
    f.link.report_overrun(4);
    CHECK(eventually([&] { return f.bridge.statistics().ext_overruns == 4 && f.link.idle(); }, 5000ms));
    f.bridge.stop();
    CHECK_EQ(f.bridge.statistics().ext_overruns, std::uint64_t{7});
}

TEST_CASE(bridge_keeps_the_link_state_fault_and_warnings)
{
    Fixture f;
    CHECK(f.bridge.health().link_state == LinkState::Unknown);
    f.link.report_link_state(LinkState::Down);
    f.link.report_warning("interface eth-test has no carrier");
    f.link.report_fault("interface eth-test disappeared");
    CHECK(eventually([&] { return f.bridge.fault().has_value(); }, 5000ms));
    CHECK_TEXT(*f.bridge.fault(), "interface eth-test disappeared");
    CHECK(f.bridge.health().link_state == LinkState::Down);
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_TEXT(warnings.at(0), "interface eth-test has no carrier");
    CHECK(f.bridge.take_warnings().empty());
    CHECK(f.bridge.running()); // the caller decides to stop
}

TEST_CASE(bridge_start_failure_of_the_link_leaves_it_stopped)
{
    FakeSegment segment;
    FakeLink link(segment, "eth-test");
    FakeEthernetController silkit;
    link.fail_start(LinkErrorKind::PermissionDenied, "cannot open 'eth-test': Operation not permitted");
    EthernetBridge bridge(silkit, link);
    CHECK(vehicle_sil::unit::throws_if<LinkError>(
        [&] { bridge.start(); }, [](const LinkError& e) { return e.kind() == LinkErrorKind::PermissionDenied; }));
    CHECK(!bridge.running());
    CHECK_EQ(silkit.frame_handler_count(), std::size_t{0});
    CHECK(!silkit.active());
}

TEST_CASE(bridge_start_failure_of_sil_kit_stops_the_link_again)
{
    FakeSegment segment;
    FakeLink link(segment, "eth-test");
    FakeEthernetController silkit;
    silkit.throw_on_activate = true;
    EthernetBridge bridge(silkit, link);
    CHECK(vehicle_sil::unit::throws_with<std::runtime_error>([&] { bridge.start(); }, "fake Activate failure"));
    CHECK(!bridge.running());
    CHECK(!link.started());
    CHECK_EQ(silkit.frame_handler_count(), std::size_t{0});
    CHECK_EQ(silkit.transmit_handler_count(), std::size_t{0});
}

TEST_CASE(bridge_stop_removes_the_handlers_and_ends_forwarding)
{
    Fixture f;
    CHECK_EQ(f.silkit.frame_handler_count(), std::size_t{1});
    CHECK_EQ(f.silkit.transmit_handler_count(), std::size_t{1});
    f.bridge.stop();
    f.bridge.stop(); // twice is fine
    CHECK(!f.bridge.running());
    CHECK_EQ(f.silkit.frame_handler_count(), std::size_t{0});
    CHECK_EQ(f.silkit.transmit_handler_count(), std::size_t{0});
    CHECK(!f.silkit.active());
    CHECK(!f.link.started());
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(f.segment.transmitted().empty());
    CHECK_EQ(f.silkit.sent_count(), std::size_t{0});
    // And it starts again.
    f.bridge.start();
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    CHECK_EQ(f.segment.transmitted().size(), std::size_t{1});
}

TEST_CASE(bridge_both_directions_at_once)
{
    Fixture f;
    constexpr unsigned kFrames = 500;
    std::thread outside([&] {
        for (unsigned i = 0; i < kFrames; ++i)
        {
            f.segment.send(make_frame(kEcu, kOutside, kExperimental, 46, static_cast<std::uint8_t>(i)));
        }
    });
    for (unsigned i = 0; i < kFrames; ++i)
    {
        f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental, 46, static_cast<std::uint8_t>(i)));
    }
    outside.join();
    CHECK(eventually([&] { return f.silkit.sent_count() == kFrames && f.segment.idle(); }, 10000ms));
    const auto sent = f.silkit.sent();
    for (unsigned i = 0; i < kFrames; ++i)
    {
        CHECK_EQ(sent[i].at(14), static_cast<std::uint8_t>(i)); // in order
    }
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{kFrames});
    CHECK_EQ(s.ext_to_silkit, std::uint64_t{kFrames});
    CHECK_EQ(s.own_tx_dropped + s.loop_suspected, std::uint64_t{0});
}
