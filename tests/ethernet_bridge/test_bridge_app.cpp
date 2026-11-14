// SPDX-License-Identifier: MIT
//
// What the bridge executable says outside the forwarding: which backend works on which operating
// system, the start line, and the hint about promiscuous mode (with a clock the test moves).
#include "check_text.hpp"
#include "fake_link.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil::ethernet_bridge;
using vehicle_sil::eth_link::LinkEvent;
using vehicle_sil::eth_link::LinkEventKind;
using vehicle_sil::eth_link::TransmitResult;
using vehicle_sil::fake_eth::FakeLink;
using vehicle_sil::fake_eth::FakeSegment;
using vehicle_sil::fake_eth::make_frame;
using vehicle_sil::unit::eventually;

// Locally administered addresses; 0x88B5 is an EtherType known to be free for local experiments
// (not checked against the standard).
constexpr std::uint16_t kExperimental = 0x88B5;
const char* const kEcu = "02:00:00:00:00:01";
const char* const kOtherEcu = "02:00:00:00:00:03";
const char* const kDevice = "02:00:00:00:00:02";
const char* const kBroadcast = "ff:ff:ff:ff:ff:ff";

BridgeOptions options_for(Backend backend, std::vector<std::string_view> extra = {})
{
    std::vector<std::string_view> args = {"--registry-uri", "silkit://127.0.0.1:8500", "--network", "ETH1", "--backend",
                                          to_string(backend), "--interface", "enp3s0"};
    args.insert(args.end(), extra.begin(), extra.end());
    return parse_bridge_options(args);
}

/// A PromiscuousHint on a fake link, with a clock that only the test moves.
struct Hinted
{
    FakeSegment segment;
    FakeLink link{segment, "enp3s0"};
    std::atomic<std::int64_t> now_ms{1000};
    PromiscuousHint hint{link, 5s, [this] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(now_ms.load())); }};
    std::atomic<int> frames{0};

    Hinted()
    {
        hint.start([this](const LinkEvent& event) { frames += event.kind == LinkEventKind::Frame ? 1 : 0; });
    }
    ~Hinted() { hint.stop(); }

    void receive(const char* destination, const char* source)
    {
        const int before = frames.load();
        segment.send(make_frame(destination, source, kExperimental));
        CHECK(eventually([&] { return frames.load() == before + 1; }, 10s));
    }
    void send(const char* destination, const char* source)
    {
        CHECK(hint.transmit(make_frame(destination, source, kExperimental)) == TransmitResult::Ok);
    }
};

const std::string kHint =
    "no frame to a MAC address of the SIL Kit side has come from fake 'enp3s0' (virtual) in the 5 s since the bridge first "
    "sent a frame there, and --promiscuous is off: without it an interface passes on only frames to its own address and to "
    "group addresses, so frames to the SIL Kit devices (they have other addresses) do not arrive. If devices outside are "
    "meant to answer, add --promiscuous.";

} // namespace

TEST_CASE(app_tap_packet_and_pcap_are_for_linux)
{
    CHECK(!backend_unavailable(Backend::Tap, HostOs::Linux));
    CHECK(!backend_unavailable(Backend::Packet, HostOs::Linux));
    CHECK(!backend_unavailable(Backend::Pcap, HostOs::Linux));
    CHECK(!backend_unavailable(std::nullopt, HostOs::Linux));
    CHECK_TEXT(backend_unavailable(Backend::Npcap, HostOs::Linux).value_or(""),
               "--backend npcap is for Windows. On Linux use --backend packet or --backend pcap (a network interface), or "
               "--backend tap (a TAP device).");
}

TEST_CASE(app_npcap_is_for_windows_and_the_message_names_it)
{
    CHECK(!backend_unavailable(Backend::Npcap, HostOs::Windows));
    CHECK(!backend_unavailable(std::nullopt, HostOs::Windows));
    for (const auto backend : {Backend::Tap, Backend::Packet, Backend::Pcap})
    {
        CHECK_TEXT(backend_unavailable(backend, HostOs::Windows).value_or(""),
                   "--backend " + std::string(to_string(backend)) +
                       " is for Linux. On Windows use --backend npcap (a network adapter through Npcap).");
    }
}

TEST_CASE(app_no_backend_works_elsewhere_and_the_message_tells_where_to_run_the_bridge)
{
    const std::string why = ": vehicle_sil_ethernet_bridge has no backend for this operating system (tap, packet and pcap are for "
                            "Linux; npcap is for Windows). Run the bridge on a Linux or Windows computer that reaches the same SIL "
                            "Kit registry.";
    for (const auto backend : {Backend::Tap, Backend::Packet, Backend::Npcap, Backend::Pcap})
    {
        CHECK_TEXT(backend_unavailable(backend, HostOs::Other).value_or(""),
                   "--backend " + std::string(to_string(backend)) + " cannot be used here" + why);
    }
    CHECK_TEXT(backend_unavailable(std::nullopt, HostOs::Other).value_or(""), "there is no interface to list here" + why);
}

TEST_CASE(app_host_os_matches_the_build)
{
#if defined(__linux__)
    CHECK(host_os() == HostOs::Linux);
#elif defined(_WIN32)
    CHECK(host_os() == HostOs::Windows);
#else
    CHECK(host_os() == HostOs::Other);
#endif
}

TEST_CASE(app_start_line_tells_what_is_bridged_and_what_passes)
{
    CHECK_TEXT(format_start_line(options_for(Backend::Packet, {"--ethertype", "0x88b5,0x0800"}), "packet 'enp3s0' (network interface)"),
               "bridging SIL Kit network 'ETH1' (participant 'EthernetBridge', silkit://127.0.0.1:8500) <-> packet 'enp3s0' "
               "(network interface): EtherType 0x88b5,0x0800 only, both ways; promiscuous off (only frames to the interface's "
               "own address and to group addresses arrive); MAC guard on; Ctrl+C to stop");
    CHECK_TEXT(format_start_line(options_for(Backend::Packet, {"--all-frames", "--promiscuous", "--no-mac-guard", "--participant", "B"}),
                                 "packet 'enp3s0' (network interface)"),
               "bridging SIL Kit network 'ETH1' (participant 'B', silkit://127.0.0.1:8500) <-> packet 'enp3s0' (network "
               "interface): every frame, both ways; promiscuous on; MAC guard off; Ctrl+C to stop");
}

TEST_CASE(app_start_line_of_a_tap_says_nothing_about_promiscuous_mode)
{
    CHECK_TEXT(format_start_line(options_for(Backend::Tap, {"--src-mac", "02:00:00:00:00:01"}), "tap 'vsil0' (TAP device in this computer)"),
               "bridging SIL Kit network 'ETH1' (participant 'EthernetBridge', silkit://127.0.0.1:8500) <-> tap 'vsil0' (TAP "
               "device in this computer): SIL Kit MAC 02:00:00:00:00:01 only, both ways; MAC guard on; Ctrl+C to stop");
}

TEST_CASE(app_promiscuous_hint_warns_once_when_nothing_answers_the_silkit_side)
{
    Hinted h;
    CHECK(!h.hint.take_warning()); // nothing sent yet
    h.now_ms = 60000;
    CHECK(!h.hint.take_warning()); // the time counts from the first frame sent, not from the start
    h.send(kDevice, kEcu);
    h.now_ms = 60000 + 4999;
    CHECK(!h.hint.take_warning());
    // Frames to group addresses and to other addresses arrive without promiscuous mode too.
    h.receive(kBroadcast, kDevice);
    h.receive(kOtherEcu, kDevice);
    h.now_ms = 60000 + 5000;
    CHECK_TEXT(h.hint.take_warning().value_or(""), kHint);
    h.now_ms = 60000 + 500000;
    CHECK(!h.hint.take_warning()); // once
}

TEST_CASE(app_promiscuous_hint_is_quiet_once_a_frame_to_the_silkit_side_arrived)
{
    Hinted h;
    h.send(kDevice, kEcu);
    h.send(kDevice, kOtherEcu);
    h.receive(kOtherEcu, kDevice); // to an address frames were sent from
    h.now_ms += 3600000;
    CHECK(!h.hint.take_warning());
}

TEST_CASE(app_promiscuous_hint_is_quiet_when_the_frame_to_the_silkit_side_arrived_before_the_first_one_sent)
{
    // The frame came while the address was not known as one of the SIL Kit side; the bridge sent from it later.
    Hinted h;
    h.receive(kEcu, kDevice);
    h.send(kDevice, kEcu);
    h.now_ms += 3600000;
    CHECK(!h.hint.take_warning());
}

TEST_CASE(app_promiscuous_hint_warns_when_the_earlier_frames_were_to_other_addresses)
{
    Hinted h;
    h.receive(kOtherEcu, kDevice); // the bridge never sends from this address
    h.receive(kBroadcast, kDevice);
    h.send(kDevice, kEcu);
    h.now_ms += 5000;
    CHECK_TEXT(h.hint.take_warning().value_or(""), kHint);
}

TEST_CASE(app_promiscuous_hint_warns_also_when_frames_to_more_addresses_than_it_remembers_came_in)
{
    // What this computer sends on the interface comes in from the link too, to as many addresses as it talks to:
    // many destinations, none of them of the SIL Kit side, are no answer.
    const auto flood = [](Hinted& h) {
        for (std::size_t i = 0; i < 2 * PromiscuousHint::kMaxAddresses; ++i)
        {
            std::vector<std::uint8_t> frame = make_frame(kOtherEcu, kDevice, kExperimental);
            frame[5] = static_cast<std::uint8_t>(0x40 + i); // a destination of its own
            const int before = h.frames.load();
            h.segment.send(frame);
            CHECK(eventually([&] { return h.frames.load() == before + 1; }, 10s));
        }
    };
    Hinted h;
    flood(h);
    h.send(kDevice, kEcu);
    flood(h);
    h.now_ms += 5000;
    CHECK_TEXT(h.hint.take_warning().value_or(""), kHint);
    // An address that was remembered before the others still counts once the bridge sends from it...
    Hinted early;
    early.receive(kEcu, kDevice);
    flood(early);
    early.send(kDevice, kEcu);
    early.now_ms += 3600000;
    CHECK(!early.hint.take_warning());
    // ...and so does a frame to the SIL Kit side that comes in after the others.
    Hinted late;
    flood(late);
    late.send(kDevice, kEcu);
    late.receive(kEcu, kDevice);
    late.now_ms += 3600000;
    CHECK(!late.hint.take_warning());
}

TEST_CASE(app_promiscuous_hint_does_not_count_frames_that_were_not_sent)
{
    Hinted h;
    h.link.set_transmit_result(TransmitResult::QueueFull);
    CHECK(h.hint.transmit(make_frame(kDevice, kEcu, kExperimental)) == TransmitResult::QueueFull);
    h.now_ms += 3600000;
    CHECK(!h.hint.take_warning());
}

TEST_CASE(app_promiscuous_hint_with_more_addresses_than_it_remembers_takes_any_individual_address)
{
    Hinted h;
    for (std::size_t i = 0; i <= PromiscuousHint::kMaxAddresses; ++i)
    {
        std::vector<std::uint8_t> frame = make_frame(kDevice, kEcu, kExperimental);
        frame[11] = static_cast<std::uint8_t>(0x10 + i); // a source of its own
        CHECK(h.hint.transmit(frame) == TransmitResult::Ok);
    }
    h.receive("02:00:00:00:00:7e", kDevice); // not one of them: cannot be told apart any more
    h.now_ms += 3600000;
    CHECK(!h.hint.take_warning());
}

TEST_CASE(app_promiscuous_hint_passes_the_link_through_and_starts_over)
{
    Hinted h;
    CHECK_TEXT(h.hint.describe(), "fake 'enp3s0' (virtual)");
    CHECK(!h.hint.echoes_own_transmissions());
    h.send(kDevice, kEcu);
    CHECK_EQ(h.link.transmitted().size(), std::size_t{1});
    h.hint.stop();
    CHECK(!h.link.started());
    h.hint.start([](const LinkEvent&) {});
    h.now_ms += 3600000;
    CHECK(!h.hint.take_warning()); // the frame sent before the restart is forgotten
}
