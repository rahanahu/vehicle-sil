// SPDX-License-Identifier: MIT
//
// EthernetBridge on pcap links that are opened the way Npcap's are, on one device of the fake
// pcap library (tests/eth_link/fake_pcap): the device gives a capture what it sent unless the
// capture asked it not to, and gives every capture what the others sent, as an adapter of Npcap
// does. Two bridges between the same SIL Kit network and that device (a configuration error)
// must find the loop in whatever order the copies of a frame arrive, and one bridge without the
// MAC rule must not get its own frames back.
#include "check_text.hpp"
#include "fake_ethernet_controller.hpp"
#include "fake_link.hpp"
#include "test_harness.hpp"

#include "pcap_link.hpp"
#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil::ethernet_bridge;
using namespace vehicle_sil::eth_link;
using vehicle_sil::eth_link::detail::PcapFlavor;
using vehicle_sil::fake_eth::Bytes;
using vehicle_sil::fake_eth::make_frame;
using vehicle_sil::unit::eventually;
using vehicle_sil::unit::FakeEthernetController;

#ifdef _WIN32
const char* const kSuffix = ".dll";
#else
const char* const kSuffix = ".so";
#endif
const std::string kFake = std::string(VEHICLE_SIL_FAKE_PCAP_DIR) + "/vehicle_sil_fake_pcap" + kSuffix;

constexpr std::uint16_t kExperimental = 0x88B5;
const char* const kEcu = "02:00:00:00:00:01";
const char* const kOutside = "02:00:00:00:00:99";
const char* const kBroadcast = "ff:ff:ff:ff:ff:ff";
/// The fake device that returns what is sent on it.
const char* const kAdapter = "fake-echo";

/// The fake library, taken for Npcap on every platform, and its controls.
struct NpcapFake
{
    std::shared_ptr<detail::PcapLibrary> library = detail::load_pcap_library_as(kFake, PcapFlavor::Npcap);

    template <typename Function>
    Function* control(const char* name) const
    {
        void* const address = library->symbol(name);
        if (address == nullptr)
        {
            throw std::runtime_error(std::string("the fake pcap library lacks its control ") + name);
        }
        return reinterpret_cast<Function*>(address);
    }
    /// The frame arrives at every capture open on the adapter.
    void inject(const Bytes& frame) const
    {
        CHECK(control<int(const char*, const unsigned char*, int)>("fake_pcap_inject")(kAdapter, frame.data(), static_cast<int>(frame.size())) >= 1);
    }
    /// The frame arrives at one capture only (number `capture` of those open, in the order they were opened).
    void inject_one(int capture, const Bytes& frame) const
    {
        CHECK_EQ(control<int(const char*, int, const unsigned char*, int)>("fake_pcap_inject_one")(kAdapter, capture, frame.data(),
                                                                                                  static_cast<int>(frame.size())),
                 1);
    }
    [[nodiscard]] long sent() const { return control<long(const char*, const char*)>("fake_pcap_get")(kAdapter, "sent"); }
    [[nodiscard]] std::unique_ptr<ExternalLink> link() const { return detail::make_pcap_link(library, {kAdapter, false, kFake}); }
};

/// Two bridges between the same SIL Kit network (the controllers are wired to each other) and
/// the same adapter. Bridge 1's capture is number 0 of the adapter, bridge 2's is number 1.
struct TwoBridges
{
    NpcapFake fake;
    std::unique_ptr<ExternalLink> link1 = fake.link();
    std::unique_ptr<ExternalLink> link2 = fake.link();
    FakeEthernetController c1;
    FakeEthernetController c2;
    std::optional<EthernetBridge> b1;
    std::optional<EthernetBridge> b2;

    TwoBridges()
    {
        c1.on_send = [this](std::span<const std::uint8_t> frame) { c2.deliver(frame); };
        c2.on_send = [this](std::span<const std::uint8_t> frame) { c1.deliver(frame); };
        b1.emplace(c1, *link1, EthernetBridgeOptions{});
        b2.emplace(c2, *link2, EthernetBridgeOptions{});
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

    [[nodiscard]] std::uint64_t taken_in() const { return b1->statistics().ext_to_silkit + b2->statistics().ext_to_silkit; }
    /// A loop is going: the bridges keep taking frames in.
    [[nodiscard]] bool going_around() const
    {
        const auto before = taken_in();
        return eventually([&] { return taken_in() >= before + 20; }, 5000ms);
    }
    /// Nothing moves any more: for a while no bridge took a frame in and none was sent on the adapter.
    [[nodiscard]] bool quiet() const
    {
        return eventually(
            [&] {
                const auto in = taken_in();
                const long out = fake.sent();
                std::this_thread::sleep_for(200ms);
                return taken_in() == in && fake.sent() == out;
            },
            5000ms);
    }
    [[nodiscard]] std::uint64_t suspected() const { return b1->statistics().loop_suspected + b2->statistics().loop_suspected; }
};

} // namespace

TEST_CASE(two_bridges_on_one_npcap_adapter_stop_a_loop_started_outside_whichever_copy_comes_first)
{
    // A device outside sends one frame, and one bridge gets its copy long before the other. The
    // early bridge passes it into SIL Kit, the late bridge gets it from there and sends it out,
    // the early one takes that in again: the loop runs until the late bridge's own copy arrives.
    // Its capture does not get what it sent itself, so the frame is not its own: the address is
    // on both sides.
    for (const int early : {0, 1})
    {
        TwoBridges t;
        CHECK(!t.link1->echoes_own_transmissions());
        const auto frame = make_frame(kBroadcast, kOutside, kExperimental);
        t.fake.inject_one(early, frame);
        CHECK(t.going_around());
        t.fake.inject_one(1 - early, frame);
        CHECK(t.quiet());
        EthernetBridge& late = early == 0 ? *t.b2 : *t.b1;
        EthernetBridge& first = early == 0 ? *t.b1 : *t.b2;
        CHECK(late.statistics().loop_suspected >= 1);
        CHECK_EQ(late.statistics().own_tx_dropped, std::uint64_t{0});
        CHECK_EQ(first.statistics().own_tx_dropped, std::uint64_t{0});
        const auto warnings = late.take_warnings();
        CHECK_EQ(warnings.size(), std::size_t{1});
        CHECK(warnings.at(0).starts_with("MAC 02:00:00:00:00:99 is on both sides: frames from it came from SIL Kit and now come "
                                         "from the external link"));
    }
}

TEST_CASE(two_bridges_on_one_npcap_adapter_stop_a_loop_started_outside_at_both_at_once)
{
    // Both captures get the device's frame, in whatever order the threads take it.
    TwoBridges t;
    t.fake.inject(make_frame(kBroadcast, kOutside, kExperimental));
    CHECK(t.quiet());
    CHECK(t.suspected() >= 1);
    CHECK(t.b1->take_warnings().size() + t.b2->take_warnings().size() >= 1);
}

TEST_CASE(two_bridges_on_one_npcap_adapter_stop_a_loop_started_in_sil_kit)
{
    // The ECU's frame reaches bridge 1 first: it sends the frame out, bridge 2's capture gets it
    // (the adapter gives every capture what the others send) and takes it in, bridge 1 sends it
    // out again. When bridge 2 gets the frame from SIL Kit too, the address is on both sides.
    TwoBridges t;
    const auto frame = make_frame(kBroadcast, kEcu, kExperimental);
    t.c1.deliver(frame);
    CHECK(t.going_around());
    t.c2.deliver(frame);
    CHECK(t.quiet());
    CHECK(t.b2->statistics().loop_suspected >= 1);
    const auto warnings = t.b2->take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(warnings.at(0).starts_with("MAC 02:00:00:00:00:01 is on both sides: frames from it came from the external link and now "
                                     "come from SIL Kit"));
}

TEST_CASE(bridge_on_an_npcap_adapter_without_the_mac_guard_does_not_get_its_own_frames_back)
{
    // The MAC rule is the only thing that drops a bridge's own frames coming back. On an adapter
    // that returns them, the capture keeps them away: nothing goes back into SIL Kit, and
    // start() has nothing to warn about.
    const NpcapFake fake;
    const auto link = fake.link();
    FakeEthernetController silkit;
    EthernetBridgeOptions options;
    options.mac_guard = false;
    EthernetBridge bridge(silkit, *link, options);
    bridge.start();
    bridge.start_controller();
    const long before = fake.sent();
    for (std::uint8_t i = 0; i < 10; ++i)
    {
        silkit.deliver(make_frame(kBroadcast, kEcu, kExperimental, 46, i));
    }
    CHECK_EQ(fake.sent(), before + 10);
    // A frame from outside behind them: once it is in, what the adapter had for the bridge before it is in too.
    fake.inject(make_frame(kBroadcast, kOutside, kExperimental));
    CHECK(eventually([&] { return bridge.statistics().ext_to_silkit >= 1; }, 10000ms));
    bridge.stop();
    CHECK_EQ(bridge.statistics().ext_to_silkit, std::uint64_t{1});
    CHECK_EQ(bridge.statistics().silkit_to_ext, std::uint64_t{10});
    CHECK_EQ(silkit.sent().size(), std::size_t{1});
    CHECK(bridge.take_warnings().empty());
}
