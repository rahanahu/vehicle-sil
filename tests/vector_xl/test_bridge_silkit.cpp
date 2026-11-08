// SPDX-License-Identifier: MIT
//
// The complete bridge with real SIL Kit (in-process registry, two participants) and the real XL
// backend on the fake XL driver:
//
//   SIL Kit participant "Peer" <-> [CanBridge + XlCanPort] <-> fake XL bus <-> fake_xl::TestNode
#include "fake_external_port.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"
#include "fake_xl_frames.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/vector_xl/can_bridge.hpp"
#include "vehicle_sil/vector_xl/xl_driver.hpp"

#include <silkit/services/can/all.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

namespace can = SilKit::Services::Can;
using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::vector_xl;
using vehicle_sil::test::frame_matrix;
using vehicle_sil::test::MatrixFrame;
using vehicle_sil::test::to_fake;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::record;
using vehicle_sil::unit::RecordedFrame;

constexpr unsigned kVirtual1 = 0;
const std::string kNetwork = "XL_BRIDGE_TEST";

/// Frames a SIL Kit participant received.
class Inbox
{
public:
    void push(const can::CanFrameEvent& event)
    {
        {
            const std::lock_guard lock(mutex_);
            frames_.push_back(record(event.frame));
        }
        cv_.notify_all();
    }

    std::vector<RecordedFrame> wait_for(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return frames_.size() >= count; });
        return frames_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<RecordedFrame> frames_;
};

/// Registry + bridge participant (with XL port on fake "Virtual Channel 1") + peer participant.
struct Fixture
{
    InProcessRegistry registry;
    std::unique_ptr<XlDriver> driver;

    std::unique_ptr<ParticipantRuntime> bridgeRuntime;
    std::unique_ptr<XlCanPort> port;
    std::unique_ptr<CanBridge> bridge;

    Inbox peerInbox;
    std::unique_ptr<ParticipantRuntime> peerRuntime;
    can::ICanController* peerCan = nullptr;
    std::optional<SilKit::Util::HandlerId> peerHandler;

    Fixture()
    {
        driver = std::make_unique<XlDriver>();

        bridgeRuntime = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{"Bridge", registry.uri});
        auto& bridgeCan = *bridgeRuntime->participant().CreateCanController("XL", kNetwork);
        port = std::make_unique<XlCanPort>(*driver, driver->channels().at(kVirtual1));
        bridge = std::make_unique<CanBridge>(bridgeCan, *port);
        bridge->start(); // before the lifecycle, as vehicle_sil_vector_xl_bridge does
        bridgeRuntime->start(10s);

        peerRuntime = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{"Peer", registry.uri});
        peerCan = peerRuntime->participant().CreateCanController("CAN", kNetwork);
        peerHandler = peerCan->AddFrameHandler([this](can::ICanController*, const can::CanFrameEvent& e) { peerInbox.push(e); });
        peerCan->Start();
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
            peerCan->RemoveFrameHandler(*peerHandler);
        }
    }
};

} // namespace

TEST_CASE(silkit_bridge_carries_silkit_frames_to_xl_with_dlc_at_most_8)
{
    fake_xl::reset();
    fake_xl::TestNode node(kVirtual1);
    Fixture f;

    const auto matrix = frame_matrix();
    for (const auto& frame : matrix)
    {
        f.peerCan->SendFrame(frame.to_silkit());
    }
    for (const auto& frame : matrix)
    {
        const auto received = node.receive(3000ms);
        CHECK(received.has_value());
        CHECK(*received == as_transmitted_on_xl(frame));
    }
    CHECK(!node.receive(300ms).has_value()); // nothing extra
    CHECK_EQ(f.bridge->statistics().silkit_to_external, matrix.size());
    CHECK(f.peerInbox.wait_for(1, 200ms).empty()); // nothing came back into SIL Kit
}

TEST_CASE(silkit_bridge_carries_xl_frames_to_silkit_unchanged)
{
    fake_xl::reset();
    fake_xl::TestNode node(kVirtual1);
    Fixture f;

    const auto matrix = frame_matrix();
    for (const auto& frame : matrix)
    {
        node.send(to_fake(frame));
    }
    const auto received = f.peerInbox.wait_for(matrix.size(), 5000ms);
    CHECK_EQ(received.size(), matrix.size());
    for (std::size_t i = 0; i < matrix.size() && i < received.size(); ++i)
    {
        CHECK(received[i] == record(matrix[i].to_silkit()));
    }
    // The bridge must not see its own SIL Kit transmissions and send them back to XL.
    CHECK(!node.receive(300ms).has_value());
    const auto stats = f.bridge->statistics();
    CHECK_EQ(stats.external_to_silkit, matrix.size());
    CHECK_EQ(stats.silkit_to_external, 0u);
}

TEST_CASE(silkit_bridge_does_not_loop_when_the_driver_sends_tx_receipts)
{
    fake_xl::reset();
    fake_xl::set_ignore_channel_mode(true); // worst case: receipts arrive although disabled
    fake_xl::TestNode node(kVirtual1);
    Fixture f;

    const auto matrix = frame_matrix();
    for (const auto& frame : matrix)
    {
        f.peerCan->SendFrame(frame.to_silkit());
    }
    for (std::size_t i = 0; i < matrix.size(); ++i)
    {
        CHECK(node.receive(3000ms).has_value());
    }
    std::this_thread::sleep_for(300ms);
    CHECK(f.peerInbox.wait_for(1, 100ms).empty()); // receipts were not forwarded into SIL Kit
    CHECK(!node.receive(100ms).has_value());       // and nothing was sent twice
    const auto stats = f.bridge->statistics();
    CHECK_EQ(stats.silkit_to_external, matrix.size());
    CHECK_EQ(stats.own_transmissions_dropped, matrix.size());
    CHECK_EQ(stats.external_to_silkit, 0u);
}

TEST_CASE(silkit_bridge_bidirectional_burst_keeps_order)
{
    fake_xl::reset();
    fake_xl::TestNode node(kVirtual1);
    Fixture f;

    constexpr std::uint32_t kFrames = 500;
    auto payload = [](std::uint32_t i) {
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8), 0xC3};
    };
    std::thread fromXl([&] {
        for (std::uint32_t i = 0; i < kFrames; ++i)
        {
            node.send({0x1000 + i, true, false, 3, payload(i)});
        }
    });
    std::vector<std::vector<std::uint8_t>> data(kFrames);
    for (std::uint32_t i = 0; i < kFrames; ++i)
    {
        data[i] = payload(i);
        can::CanFrame frame{};
        frame.canId = 0x200 + (i % 0x100);
        frame.dlc = 3;
        frame.dataField = data[i];
        f.peerCan->SendFrame(frame);
    }
    fromXl.join();

    for (std::uint32_t i = 0; i < kFrames; ++i)
    {
        const auto received = node.receive(5000ms);
        CHECK(received.has_value());
        CHECK_EQ(received->id, 0x200u + (i % 0x100));
        CHECK(received->data == payload(i));
    }
    const auto inbox = f.peerInbox.wait_for(kFrames, 10000ms);
    CHECK_EQ(inbox.size(), static_cast<std::size_t>(kFrames));
    for (std::uint32_t i = 0; i < kFrames && i < inbox.size(); ++i)
    {
        CHECK_EQ(inbox[i].canId, 0x1000u + i);
        CHECK(inbox[i].data == payload(i));
    }
    const auto stats = f.bridge->statistics();
    CHECK_EQ(stats.silkit_to_external, static_cast<std::uint64_t>(kFrames));
    CHECK_EQ(stats.external_to_silkit, static_cast<std::uint64_t>(kFrames));
    CHECK_EQ(stats.external_overruns, 0u);
}
