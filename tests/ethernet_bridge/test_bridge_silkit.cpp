// SPDX-License-Identifier: MIT
//
// The bridge with real SIL Kit (in-process registry on a free port, two participants) and a fake
// link:
//
//   SIL Kit participant "Peer" <-> [EthernetBridge] <-> fake link <-> FakeSegment (the test as a device)
#include "check_text.hpp"
#include "fake_link.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <silkit/services/ethernet/all.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

namespace ethernet = SilKit::Services::Ethernet;
using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::ethernet_bridge;
using vehicle_sil::fake_eth::Bytes;
using vehicle_sil::fake_eth::FakeLink;
using vehicle_sil::fake_eth::FakeSegment;
using vehicle_sil::fake_eth::make_frame;
using vehicle_sil::unit::eventually;
using vehicle_sil::unit::InProcessRegistry;

const std::string kNetwork = "ETH_BRIDGE_TEST";
// Locally administered addresses; 0x88B5 is an EtherType known to be free for local experiments
// (not checked against the standard).
constexpr std::uint16_t kExperimental = 0x88B5;
const char* const kPeer = "02:00:00:00:00:01";
const char* const kDevice = "02:00:00:00:00:99";

/// Frames a SIL Kit participant received.
class Inbox
{
public:
    void push(const ethernet::EthernetFrameEvent& event)
    {
        {
            const std::lock_guard lock(mutex_);
            frames_.emplace_back(event.frame.raw.begin(), event.frame.raw.end());
        }
        cv_.notify_all();
    }

    std::vector<Bytes> wait_for(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, vehicle_sil::unit::scaled_timeout(timeout), [&] { return frames_.size() >= count; });
        return frames_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Bytes> frames_;
};

/// Registry + bridge participant (with the fake link) + peer participant.
struct Fixture
{
    InProcessRegistry registry;
    FakeSegment segment;
    FakeLink link{segment, "eth-test"};

    std::unique_ptr<ParticipantRuntime> bridgeRuntime;
    std::unique_ptr<EthernetBridge> bridge;

    Inbox peerInbox;
    std::unique_ptr<ParticipantRuntime> peerRuntime;
    ethernet::IEthernetController* peerEth = nullptr;
    std::optional<SilKit::Util::HandlerId> peerHandler;

    explicit Fixture(bool echo_own = false)
    {
        link.set_echo_own(echo_own);
        bridgeRuntime = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{"Bridge", registry.uri});
        auto& bridgeEth = *bridgeRuntime->participant().CreateEthernetController("ETH", kNetwork);
        bridge = std::make_unique<EthernetBridge>(bridgeEth, link);
        // As the CAN bridge does: forwarding before the lifecycle, the controller at CommunicationReady.
        bridgeRuntime->add_communication_ready_handler([b = bridge.get()] { b->start_controller(); });
        bridge->start();
        bridgeRuntime->start(10s);

        peerRuntime = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{"Peer", registry.uri});
        peerEth = peerRuntime->participant().CreateEthernetController("ETH", kNetwork);
        peerHandler = peerEth->AddFrameHandler(
            [this](ethernet::IEthernetController*, const ethernet::EthernetFrameEvent& e) { peerInbox.push(e); });
        peerEth->Activate();
        peerRuntime->start(10s);
        if (!bridgeRuntime->wait_for_peer("Peer", is_running, 10s))
        {
            throw std::runtime_error("peer did not reach Running");
        }
    }

    ~Fixture()
    {
        bridge->stop();
        if (peerHandler)
        {
            peerEth->RemoveFrameHandler(*peerHandler);
        }
    }

    void peer_send(const Bytes& frame) { peerEth->SendFrame(ethernet::EthernetFrame{frame}); }
};

} // namespace

TEST_CASE(silkit_bridge_carries_frames_both_ways_unchanged)
{
    Fixture f;
    std::vector<Bytes> out;
    for (std::uint8_t i = 0; i < 20; ++i)
    {
        out.push_back(make_frame(kDevice, kPeer, kExperimental, 46 + i, i));
        f.peer_send(out.back());
    }
    CHECK(eventually([&] { return f.segment.transmitted().size() == out.size(); }, 5000ms));
    CHECK(f.segment.transmitted() == out);

    const auto in = make_frame(kPeer, kDevice, kExperimental, 46, 0x40);
    f.segment.send(in);
    // A frame shorter than 60 bytes arrives padded with zeros to 60 bytes (SIL Kit pads it).
    const auto shortFrame = make_frame(kPeer, kDevice, kExperimental, 10, 0x50);
    f.segment.send(shortFrame);
    const auto received = f.peerInbox.wait_for(2, 5000ms);
    CHECK_EQ(received.size(), std::size_t{2});
    CHECK(received.at(0) == in);
    auto padded = shortFrame;
    padded.resize(60, 0);
    CHECK(received.at(1) == padded);

    // The bridge counts a frame after it handed it over: the peer may see it first.
    CHECK(eventually(
        [&] {
            const auto s = f.bridge->statistics();
            return s.ext_to_silkit == 2 && s.silkit_to_ext == 20;
        },
        5000ms));
    // The bridge did not send its own SIL Kit frames back out (RX handler only).
    CHECK_EQ(f.segment.transmitted().size(), out.size());
}

TEST_CASE(silkit_bridge_drops_its_own_frames_when_the_link_delivers_them_back)
{
    Fixture f(true);
    for (std::uint8_t i = 0; i < 10; ++i)
    {
        f.peer_send(make_frame(kDevice, kPeer, kExperimental, 46, i));
    }
    CHECK(eventually([&] { return f.bridge->statistics().own_tx_dropped == 10; }, 5000ms));
    // A frame from the device after them: the link delivers in order and the bridge hands frames
    // to SIL Kit in order, so a returned frame forwarded by mistake would arrive before it.
    const auto marker = make_frame(kPeer, kDevice, kExperimental, 46, 0xEE);
    f.segment.send(marker);
    const auto received = f.peerInbox.wait_for(1, 5000ms);
    CHECK_EQ(received.size(), std::size_t{1});
    CHECK(received.at(0) == marker);
    CHECK(eventually(
        [&] {
            const auto s = f.bridge->statistics();
            return s.ext_to_silkit == 1 && s.silkit_to_ext == 10;
        },
        5000ms));
}

TEST_CASE(silkit_bridge_burst_both_ways_keeps_the_order)
{
    Fixture f;
    constexpr unsigned kFrames = 500;
    auto frame = [](const char* destination, const char* source, unsigned i) {
        auto bytes = make_frame(destination, source, kExperimental, 46);
        bytes[14] = static_cast<std::uint8_t>(i);
        bytes[15] = static_cast<std::uint8_t>(i >> 8);
        return bytes;
    };
    std::thread fromDevice([&] {
        for (unsigned i = 0; i < kFrames; ++i)
        {
            f.segment.send(frame(kPeer, kDevice, i));
        }
    });
    for (unsigned i = 0; i < kFrames; ++i)
    {
        f.peer_send(frame(kDevice, kPeer, i));
    }
    fromDevice.join();

    CHECK(eventually([&] { return f.segment.transmitted().size() == kFrames; }, 10000ms));
    const auto transmitted = f.segment.transmitted();
    for (unsigned i = 0; i < kFrames && i < transmitted.size(); ++i)
    {
        CHECK(transmitted[i] == frame(kDevice, kPeer, i));
    }
    const auto received = f.peerInbox.wait_for(kFrames, 10000ms);
    CHECK_EQ(received.size(), std::size_t{kFrames});
    for (unsigned i = 0; i < kFrames && i < received.size(); ++i)
    {
        CHECK(received[i] == frame(kPeer, kDevice, i));
    }
    CHECK(eventually(
        [&] {
            const auto s = f.bridge->statistics();
            return s.ext_to_silkit == kFrames && s.silkit_to_ext == kFrames;
        },
        10000ms));
    const auto s = f.bridge->statistics();
    CHECK_EQ(s.silkit_tx_failures + s.ext_tx_failures + s.ext_queue_full, std::uint64_t{0});
}
