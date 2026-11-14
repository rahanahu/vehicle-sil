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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <optional>
#include <span>
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

    /// `echo`: the link returns the bridge's own frames (no link of vehicle-sil does today).
    explicit Fixture(EthernetBridgeOptions options = {}, bool echo = false) :
      bridge(silkit, link, std::move(options))
    {
        link.set_echo_own(echo);
        bridge.start();
        bridge.start_controller(); // what the participant's CommunicationReady handler does
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
    Fixture f({}, true);
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
    Fixture f(options, true);
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        f.silkit.deliver(make_frame(kBroadcast, kEcu, kExperimental, 46, i));
    }
    CHECK(eventually([&] { return f.silkit.sent_count() == 5; }, 5000ms));
    CHECK_EQ(f.bridge.statistics().own_tx_dropped, std::uint64_t{0});
    // Said at the start, once, before any frame: what the frames above then showed.
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_TEXT(warnings.at(0),
               "the MAC guard is off (--no-mac-guard) and this external link gives the bridge's own transmissions back to "
               "it: every frame the bridge sends out comes back and is passed into SIL Kit again (counted in "
               "ext_to_silkit), so its sender receives its own frame and every other participant receives it twice. A "
               "network stack that looks for its own address in use by others then finds it. Run without --no-mac-guard "
               "unless this is what you want.");
}

TEST_CASE(bridge_warns_of_frames_coming_back_only_without_the_mac_guard_on_a_link_that_echoes)
{
    {
        EthernetBridgeOptions options;
        options.mac_guard = false;
        Fixture f(options, false); // the link does not give them back: nothing comes back
        CHECK(f.bridge.take_warnings().empty());
    }
    {
        Fixture f({}, true); // the MAC guard drops what comes back
        CHECK(f.bridge.take_warnings().empty());
    }
    {
        // Said again by every start: the warnings of the run before are gone by then.
        EthernetBridgeOptions options;
        options.mac_guard = false;
        Fixture f(options, true);
        CHECK_EQ(f.bridge.take_warnings().size(), std::size_t{1});
        f.bridge.stop();
        f.bridge.start();
        CHECK_EQ(f.bridge.take_warnings().size(), std::size_t{1});
    }
}

TEST_CASE(bridge_without_mac_guard_says_once_that_a_frame_it_sent_came_back_from_a_link_that_does_not_echo)
{
    // The link says it does not return the bridge's own frames, so start() has nothing to warn
    // of. A link that returns them all the same would put them back into SIL Kit without a word:
    // the bridge tells it from the last frame it sent.
    EthernetBridgeOptions options;
    options.mac_guard = false;
    Fixture f(options, false);
    const auto sent = make_frame(kBroadcast, kEcu, kExperimental, 46, 1);
    f.silkit.deliver(sent);
    // Not the frame sent: another payload, another length, and the same bytes from another address.
    f.segment.send(make_frame(kBroadcast, kEcu, kExperimental, 46, 2));
    f.segment.send(make_frame(kBroadcast, kEcu, kExperimental, 47, 1));
    f.segment.send(make_frame(kBroadcast, kOutside, kExperimental, 46, 1));
    CHECK(eventually([&] { return f.bridge.statistics().ext_to_silkit == 3 && f.segment.idle(); }, 5000ms));
    CHECK(f.bridge.take_warnings().empty());
    f.segment.send(sent);
    f.segment.send(sent);
    CHECK(eventually([&] { return f.bridge.statistics().ext_to_silkit == 5 && f.segment.idle(); }, 5000ms));
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1}); // once, and the frames are forwarded all the same
    CHECK_TEXT(warnings.at(0),
               "a frame like the one the bridge had just sent out (the same length and first bytes) came from the "
               "external link, and with the MAC guard off (--no-mac-guard) it was passed into SIL Kit again (counted in "
               "ext_to_silkit). A second bridge or a device outside sends the frames back, or the external link gives "
               "the bridge's own transmissions back although it says it does not: with --backend npcap, install the "
               "current Npcap or try another adapter, and report it to vehicle-sil if it stays. Run without "
               "--no-mac-guard to have such frames dropped and their addresses named. This is said once.");
    CHECK_EQ(f.bridge.statistics().loop_suspected, std::uint64_t{0});
    // Said again by the next start, and only for a frame sent since then.
    f.bridge.stop();
    f.bridge.start();
    f.bridge.start_controller();
    f.segment.send(sent);
    CHECK(eventually([&] { return f.bridge.statistics().ext_to_silkit == 6 && f.segment.idle(); }, 5000ms));
    CHECK(f.bridge.take_warnings().empty());
    f.silkit.deliver(sent);
    f.segment.send(sent);
    CHECK(eventually([&] { return f.bridge.statistics().ext_to_silkit == 7 && f.segment.idle(); }, 5000ms));
    CHECK_EQ(f.bridge.take_warnings().size(), std::size_t{1});
}

TEST_CASE(bridge_with_the_mac_guard_does_not_compare_with_the_frame_it_sent)
{
    // With the MAC guard the address tells it: the conflict is the warning, and the only one.
    Fixture f;
    const auto sent = make_frame(kBroadcast, kEcu, kExperimental);
    f.silkit.deliver(sent);
    f.segment.send(sent);
    CHECK(eventually([&] { return f.bridge.statistics().loop_suspected == 1 && f.segment.idle(); }, 5000ms));
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(warnings.at(0).starts_with("MAC 02:00:00:00:00:01 is on both sides"));
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
               "networks and give each device its own MAC address, then restart the bridge: it keeps dropping them "
               "until it restarts.");
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

namespace {

/// Two bridges between the same SIL Kit network and the same wire: a configuration error. The
/// controllers are wired to each other (what one bridge sends into SIL Kit, the other receives),
/// the links share one segment. A device in SIL Kit is played by delivering to c1 and c2, a
/// device outside by the segment or by one link alone.
struct TwoBridges
{
    FakeSegment segment;
    FakeLink link1{segment, "b1"};
    FakeLink link2{segment, "b2"};
    FakeEthernetController c1;
    FakeEthernetController c2;
    std::optional<EthernetBridge> b1;
    std::optional<EthernetBridge> b2;

    TwoBridges(bool echo, bool guard1 = true, bool guard2 = true)
    {
        link1.set_echo_own(echo);
        link2.set_echo_own(echo);
        c1.on_send = [this](std::span<const std::uint8_t> frame) { c2.deliver(frame); };
        c2.on_send = [this](std::span<const std::uint8_t> frame) { c1.deliver(frame); };
        EthernetBridgeOptions o1;
        o1.mac_guard = guard1;
        EthernetBridgeOptions o2;
        o2.mac_guard = guard2;
        b1.emplace(c1, link1, o1);
        b2.emplace(c2, link2, o2);
        b1->start();
        b2->start();
        b1->start_controller();
        b2->start_controller();
    }

    ~TwoBridges()
    {
        b1->stop();
        b2->stop();
    }

    /// Nothing in flight any more (a loop ended).
    [[nodiscard]] bool quiet() { return eventually([&] { return segment.idle(); }, 5000ms); }
    [[nodiscard]] std::uint64_t taken_in() const { return b1->statistics().ext_to_silkit + b2->statistics().ext_to_silkit; }
    /// A loop is going: the bridges keep taking frames in (checks that something happens).
    [[nodiscard]] bool going_around()
    {
        const auto before = taken_in();
        return eventually([&] { return taken_in() >= before + 20; }, 5000ms);
    }
};

} // namespace

TEST_CASE(two_bridges_stop_a_loop_started_in_sil_kit)
{
    // The ECU's frame reaches bridge 1 first; bridge 1 sends it out, bridge 2 takes it in from
    // the wire, bridge 1 sends it out again: the loop runs until bridge 2 also gets the frame from
    // SIL Kit and sees the address on both sides. The same whether the links echo or not.
    for (const bool echo : {false, true})
    {
        TwoBridges t(echo);
        const auto frame = make_frame(kBroadcast, kEcu, kExperimental);
        t.c1.deliver(frame);
        CHECK(t.going_around());
        t.c2.deliver(frame);
        CHECK(t.quiet());
        CHECK(t.b2->statistics().loop_suspected >= 1);
        CHECK_EQ(t.b1->statistics().loop_suspected, std::uint64_t{0});
        const auto warnings = t.b2->take_warnings();
        CHECK_EQ(warnings.size(), std::size_t{1});
        CHECK(warnings.at(0).starts_with("MAC 02:00:00:00:00:01 is on both sides: frames from it came from the "
                                         "external link and now come from SIL Kit"));
    }
}

TEST_CASE(two_bridges_stop_a_loop_when_sil_kit_brings_an_outside_frame_first)
{
    // A device outside sends; bridge 1 takes it in, and bridge 2 gets it from SIL Kit before its
    // own copy from the wire: bridge 2 learns the device's address on the SIL Kit side and sends
    // the frame out, bridge 1 takes that in again... When bridge 2's own copy arrives, its link
    // does not echo, so the frame cannot be bridge 2's own: the address is on both sides.
    TwoBridges t(false);
    const auto frame = make_frame(kBroadcast, kOutside, kExperimental);
    t.link1.receive(frame);
    CHECK(t.going_around());
    t.link2.receive(frame);
    CHECK(t.quiet());
    CHECK(t.b2->statistics().loop_suspected >= 1);
    const auto warnings = t.b2->take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_TEXT(warnings.at(0),
               "MAC 02:00:00:00:00:99 is on both sides: frames from it came from SIL Kit and now come from the "
               "external link, so frames from it are dropped both ways (loop_suspected). Run one bridge between "
               "these networks and give each device its own MAC address, then restart the bridge: it keeps "
               "dropping them until it restarts. If there is no second bridge and no other device with this address, "
               "the external link gives the bridge's own transmissions back although it says it does not: with "
               "--backend npcap, install the current Npcap or try another adapter, and report it to vehicle-sil if it "
               "stays.");
}

TEST_CASE(two_bridges_on_echoing_links_do_not_stop_a_loop_started_outside)
{
    // The same order on links that echo: bridge 2 takes its copy from the wire for its own frame
    // coming back. Neither bridge sees the address on both sides; the loop goes on, unnoticed.
    // (A record of the frames sent could find it; until then it is a documented limit.)
    TwoBridges t(true);
    const auto frame = make_frame(kBroadcast, kOutside, kExperimental);
    t.link1.receive(frame);
    CHECK(t.going_around());
    t.link2.receive(frame);
    CHECK(t.going_around()); // still
    CHECK(t.b1->take_warnings().empty());
    CHECK(t.b2->take_warnings().empty());
    CHECK_EQ(t.b1->statistics().loop_suspected + t.b2->statistics().loop_suspected, std::uint64_t{0});
    // stop() ends it all the same, in the middle of the traffic.
    t.b1->stop();
    t.b2->stop();
    CHECK(t.quiet());
}

TEST_CASE(two_bridges_stop_a_loop_started_outside)
{
    // Both links get the device's frame, in whatever order the threads take it.
    TwoBridges t(false);
    t.segment.send(make_frame(kBroadcast, kOutside, kExperimental));
    CHECK(t.quiet());
    CHECK(t.b1->statistics().loop_suspected + t.b2->statistics().loop_suspected >= 1);
    CHECK(t.b1->take_warnings().size() + t.b2->take_warnings().size() >= 1);
}

TEST_CASE(two_bridges_one_without_the_mac_guard_stop_a_loop_started_in_sil_kit)
{
    // Bridge 2 runs with --no-mac-guard: bridge 1 alone finds the loop, from bridge 2's copy of
    // the ECU's frame on the wire (bridge 1's link does not echo).
    TwoBridges t(false, true, false);
    const auto frame = make_frame(kBroadcast, kEcu, kExperimental);
    // Bridge 2 says something only when bridge 1's copy comes from the wire after bridge 2 sent
    // its own. Left to the threads, the copy can come first, and bridge 1 can then drop what
    // bridge 2 passes on, so nothing comes after. The order is made certain: bridge 2's receive
    // thread is held, in the middle of passing on a frame from a device, until both bridges have
    // been given the ECU's frame.
    const auto from_device = make_frame(kBroadcast, kOutside, kExperimental, 60);
    std::atomic<bool> held{false};
    std::latch release{1};
    t.c2.on_send = [&](std::span<const std::uint8_t> sent) {
        if (std::ranges::equal(sent, from_device))
        {
            held.store(true);
            release.wait();
            return; // not passed on to bridge 1: the device's frame is not part of the loop
        }
        t.c1.deliver(sent);
    };
    t.link2.receive(from_device);
    const bool holding = eventually([&] { return held.load(); }, 5000ms);
    CHECK(holding);
    t.c1.deliver(frame);
    t.c2.deliver(frame);
    release.count_down();
    CHECK(t.quiet());
    CHECK(t.b1->statistics().loop_suspected >= 1);
    const auto warnings = t.b1->take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(warnings.at(0).starts_with("MAC 02:00:00:00:00:01 is on both sides"));
    // Bridge 2 got from the wire what it had just sent there itself (bridge 1's copy), and says so.
    const auto unguarded = t.b2->take_warnings();
    CHECK_EQ(unguarded.size(), std::size_t{1});
    CHECK(unguarded.at(0).starts_with("a frame like the one the bridge had just sent out"));
}

TEST_CASE(two_bridges_one_without_the_mac_guard_stop_a_loop_started_outside)
{
    TwoBridges t(false, true, false);
    t.segment.send(make_frame(kBroadcast, kOutside, kExperimental));
    CHECK(t.quiet());
    CHECK(t.b1->statistics().loop_suspected >= 1);
    const auto warnings = t.b1->take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(warnings.at(0).starts_with("MAC 02:00:00:00:00:99 is on both sides"));
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

TEST_CASE(bridge_warns_once_when_the_mac_table_is_full_and_counts_what_it_could_not_guard)
{
    EthernetBridgeOptions options;
    options.mac_table_size = 16;
    Fixture f(options);
    for (unsigned n = 0; n < 20; ++n)
    {
        f.silkit.deliver(make_frame(kBroadcast, numbered_mac(n), kExperimental));
    }
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.silkit_to_ext, std::uint64_t{20}); // forwarded all the same
    CHECK_EQ(s.mac_unguarded, std::uint64_t{4});
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_TEXT(warnings.at(0), "the MAC table is full (16 addresses): frames from addresses that do not fit are "
                               "forwarded without the check against loops (mac_unguarded); addresses of the SIL Kit "
                               "side take the places of the oldest addresses outside. Raise --mac-table-size if this "
                               "many devices are expected.");
}

TEST_CASE(bridge_sil_kit_addresses_push_outside_addresses_out_of_a_full_table)
{
    // A busy LAN fills the table with outside addresses; an ECU that starts sending later is
    // still guarded.
    EthernetBridgeOptions options;
    options.mac_table_size = 16;
    Fixture f(options);
    for (unsigned n = 0; n < 20; ++n)
    {
        f.segment.send(make_frame(kBroadcast, numbered_mac(n), kExperimental));
    }
    CHECK(eventually([&] { return f.silkit.sent_count() == 20 && f.segment.idle(); }, 5000ms));
    CHECK_EQ(f.bridge.statistics().mac_unguarded, std::uint64_t{4});
    f.silkit.deliver(make_frame(kBroadcast, kEcu, kExperimental));
    CHECK_EQ(f.bridge.statistics().mac_unguarded, std::uint64_t{4}); // the ECU's address fit
    f.segment.send(make_frame(kBroadcast, kEcu, kExperimental)); // the link does not echo: a conflict
    CHECK(eventually([&] { return f.bridge.statistics().loop_suspected == 1 && f.segment.idle(); }, 5000ms));
}

TEST_CASE(bridge_on_a_link_that_does_not_echo_warns_of_a_sil_kit_address_outside)
{
    // The link never returns the bridge's own frames, so a frame from the outside with the ECU's
    // address is someone else's: the address is on both sides.
    Fixture f;
    f.silkit.deliver(make_frame(kBroadcast, kEcu, kExperimental));
    f.segment.send(make_frame(kOutside, kEcu, kExperimental));
    CHECK(eventually([&] { return f.bridge.statistics().loop_suspected == 1 && f.segment.idle(); }, 5000ms));
    CHECK_EQ(f.bridge.statistics().own_tx_dropped, std::uint64_t{0});
    CHECK_EQ(f.silkit.sent_count(), std::size_t{0});
    const auto warnings = f.bridge.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_TEXT(warnings.at(0),
               "MAC 02:00:00:00:00:01 is on both sides: frames from it came from SIL Kit and now come from the "
               "external link, so frames from it are dropped both ways (loop_suspected). Run one bridge between "
               "these networks and give each device its own MAC address, then restart the bridge: it keeps "
               "dropping them until it restarts. If there is no second bridge and no other device with this address, "
               "the external link gives the bridge's own transmissions back although it says it does not: with "
               "--backend npcap, install the current Npcap or try another adapter, and report it to vehicle-sil if it "
               "stays.");
    // Blocked both ways from now on, warned once.
    f.silkit.deliver(make_frame(kBroadcast, kEcu, kExperimental));
    f.segment.send(make_frame(kOutside, kEcu, kExperimental));
    CHECK(eventually([&] { return f.bridge.statistics().loop_suspected == 3 && f.segment.idle(); }, 5000ms));
    CHECK_EQ(f.bridge.statistics().silkit_to_ext, std::uint64_t{1});
    CHECK(f.bridge.take_warnings().empty());
}

TEST_CASE(bridge_named_sil_kit_addresses_are_guarded_from_the_start)
{
    EthernetBridgeOptions options;
    options.filter.silkit_macs = {*vehicle_sil::eth_link::parse_mac_address(kEcu)};
    Fixture f(options);
    f.segment.send(make_frame(kBroadcast, kEcu, kExperimental)); // before the ECU sent anything
    CHECK(eventually([&] { return f.bridge.statistics().loop_suspected == 1 && f.segment.idle(); }, 5000ms));
    CHECK_EQ(f.bridge.take_warnings().size(), std::size_t{1});
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

TEST_CASE(bridge_passes_and_counts_frames_with_an_incomplete_checksum)
{
    EthernetBridgeOptions options;
    options.filter.ether_types = {kExperimental};
    Fixture f(options);
    const auto frame = make_frame(kEcu, kOutside, kExperimental);
    f.link.receive(frame);
    f.link.receive_checksum_incomplete(frame);
    f.link.receive_checksum_incomplete(make_frame(kEcu, kOutside, 0x0800)); // filtered: not handed over, not counted
    CHECK(eventually([&] { return f.silkit.sent_count() == 2 && f.link.idle(); }, 5000ms));
    const auto s = f.bridge.statistics();
    CHECK_EQ(s.ext_to_silkit, std::uint64_t{2});
    CHECK_EQ(s.ext_checksum_incomplete, std::uint64_t{1});
    CHECK_EQ(s.filtered_from_ext, std::uint64_t{1});
    // The warning is the link's (a packet link gives it once); the bridge adds none of its own.
    CHECK(f.bridge.take_warnings().empty());
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
    bridge.start();
    // A controller that cannot be activated: a fault (the caller stops), not an exception out of
    // SIL Kit's handler.
    bridge.start_controller();
    CHECK(bridge.fault() ==
          std::optional<std::string>("activating the SIL Kit Ethernet controller failed: fake Activate failure"));
    bridge.stop();
    // After CommunicationReady, start() activates the controller and throws if that fails.
    CHECK(vehicle_sil::unit::throws_with<std::runtime_error>([&] { bridge.start(); }, "fake Activate failure"));
    CHECK(!bridge.running());
    CHECK(!link.started());
    CHECK_EQ(silkit.frame_handler_count(), std::size_t{0});
    CHECK_EQ(silkit.transmit_handler_count(), std::size_t{0});
}

TEST_CASE(bridge_activates_the_controller_at_communication_ready_and_forwards_into_sil_kit_from_then_on)
{
    FakeSegment segment;
    FakeLink link(segment, "eth-test");
    FakeEthernetController silkit;
    EthernetBridge bridge(silkit, link);
    bridge.start();
    CHECK(bridge.running());
    CHECK(!silkit.active());
    // SIL Kit to the link: at once. The link to SIL Kit: not before the controller is active
    // (SIL Kit would refuse the frame), and not counted.
    silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    CHECK_EQ(segment.transmitted().size(), std::size_t{1});
    segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(eventually([&] { return segment.idle(); }, 5000ms));
    CHECK_EQ(silkit.sent_count(), std::size_t{0});
    CHECK_EQ(bridge.statistics().ext_to_silkit, 0u);
    bridge.start_controller();
    CHECK(silkit.active());
    bridge.start_controller(); // a second time: nothing
    segment.send(make_frame(kEcu, kOutside, kExperimental));
    CHECK(eventually([&] { return silkit.sent_count() == 1; }, 5000ms));
    CHECK(!bridge.fault());

    // CommunicationReady before start(): start() activates.
    FakeSegment segment2;
    FakeLink link2(segment2, "eth-test");
    FakeEthernetController silkit2;
    EthernetBridge early(silkit2, link2);
    early.start_controller();
    CHECK(!silkit2.active());
    early.start();
    CHECK(silkit2.active());
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

TEST_CASE(bridge_counts_a_link_that_throws_in_transmit_and_goes_on)
{
    Fixture f;
    f.link.set_transmit_throws(true);
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    CHECK_EQ(f.bridge.statistics().ext_tx_failures, std::uint64_t{1});
    CHECK_TEXT(f.bridge.last_error(), "sending on the external link threw: fake link 'eth-test' threw in transmit()");
    f.link.set_transmit_throws(false);
    f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
    CHECK_EQ(f.bridge.statistics().silkit_to_ext, std::uint64_t{1});
}

TEST_CASE(bridge_stop_in_the_middle_of_traffic_ends_forwarding)
{
    Fixture f;
    std::atomic<bool> done{false};
    std::thread outside([&] {
        for (unsigned i = 0; !done.load(); ++i)
        {
            f.segment.send(make_frame(kEcu, kOutside, kExperimental, 46, static_cast<std::uint8_t>(i)));
        }
    });
    std::thread inside([&] {
        for (unsigned i = 0; !done.load(); ++i)
        {
            f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental, 46, static_cast<std::uint8_t>(i)));
        }
    });
    CHECK(eventually(
        [&] {
            const auto s = f.bridge.statistics();
            return s.silkit_to_ext > 100 && s.ext_to_silkit > 100;
        },
        5000ms));
    f.bridge.stop(); // while both threads keep sending
    const auto atStop = f.bridge.statistics();
    const auto sentAtStop = f.silkit.sent_count();
    const auto transmittedAtStop = f.link.transmitted().size();
    // The traffic goes on for a while; nothing more is forwarded.
    for (int i = 0; i < 100; ++i)
    {
        f.silkit.deliver(make_frame(kOutside, kEcu, kExperimental));
        f.segment.send(make_frame(kEcu, kOutside, kExperimental));
    }
    done.store(true);
    outside.join();
    inside.join();
    CHECK_EQ(f.silkit.sent_count(), sentAtStop);
    CHECK_EQ(f.link.transmitted().size(), transmittedAtStop);
    const auto after = f.bridge.statistics();
    CHECK_EQ(after.silkit_to_ext, atStop.silkit_to_ext);
    CHECK_EQ(after.ext_to_silkit, atStop.ext_to_silkit);
}
