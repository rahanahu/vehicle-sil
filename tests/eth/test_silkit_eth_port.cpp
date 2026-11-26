// SPDX-License-Identifier: MIT
//
// vehicle_sil::eth::SilKitEthPort against an in-memory SIL Kit Ethernet controller.
#include "eth_test_util.hpp"
#include "fake_ethernet_controller.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/eth_silkit/silkit_eth_port.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
namespace sk = SilKit::Services::Ethernet;
using vehicle_sil::SimTime;
using vehicle_sil::eth::Frame;
using vehicle_sil::eth::kBroadcast;
using vehicle_sil::eth::PortEvent;
using vehicle_sil::eth::SendResult;
using vehicle_sil::eth::SilKitEthPort;
using vehicle_sil::eth::SilKitEthPortOptions;
using vehicle_sil::eth::TimedFrame;
using vehicle_sil::eth::TimeDomain;
using vehicle_sil::eth::TransmitCapabilities;
using vehicle_sil::unit::FakeEthernetController;
using vehicle_sil::unit::frame_bytes;
using vehicle_sil::unit::mac;
using vehicle_sil::unit::make_frame;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

std::vector<TimedFrame> release(SilKitEthPort& port, SimTime now)
{
    std::vector<TimedFrame> out;
    port.release_before(now, [&](const TimedFrame& f) { out.push_back(f); });
    return out;
}

std::vector<PortEvent> release_events(SilKitEthPort& port, SimTime now)
{
    std::vector<PortEvent> out;
    port.release_events_before(now, [&](const PortEvent& e) { out.push_back(e); });
    return out;
}

/// What SIL Kit stamps the frames of a participant without time synchronisation with.
constexpr std::chrono::nanoseconds kNoTime = std::chrono::nanoseconds::min();

} // namespace

TEST_CASE(silkit_eth_port_activates_the_controller_and_removes_its_handlers_on_close)
{
    FakeEthernetController controller;
    {
        SilKitEthPort port(controller);
        CHECK(port.is_open());
        CHECK(port.time_domain() == TimeDomain::Virtual);
        // The constructor only listens; start() (from the CommunicationReady handler) activates.
        CHECK(!port.is_started());
        CHECK(!controller.active());
        CHECK_EQ(controller.handler_count(), 3u);
        // A step before start() is a mistake of the program, and says what to call.
        CHECK(throws_with<std::logic_error>(
            [&] { static_cast<void>(port.release_before(at(1ms), [](const TimedFrame&) {})); },
            "vehicle_sil::eth::SilKitEthPort: a step ran before start(): the controller is not activated, so SIL Kit "
            "would refuse every frame this port sends. Call start() from the participant's CommunicationReady handler "
            "(ParticipantRuntime::add_communication_ready_handler())"));
        CHECK(throws_with<std::logic_error>(
            [&] { static_cast<void>(port.send(TimedFrame{at(1ms), make_frame(kBroadcast, mac(1), 1, 0)})); },
            "vehicle_sil::eth::SilKitEthPort: send() before start(): "));
        port.start();
        CHECK(port.is_started());
        CHECK(controller.active());
        CHECK(port.statistics().link_up);
        port.start(); // a second time: nothing
        port.close();
        CHECK(!port.is_open());
        CHECK(!port.is_started());
        CHECK(!controller.active());
        CHECK_EQ(controller.handler_count(), 0u);
        CHECK(port.send(TimedFrame{at(1ms), make_frame(kBroadcast, mac(1), 1, 0)}) == SendResult::Failed);
        port.close(); // idempotent
        port.start(); // after close(): nothing
        CHECK(!controller.active());
    }
    // An activation that fails leaves the port open and not started.
    FakeEthernetController failing;
    failing.throw_on_activate = true;
    {
        SilKitEthPort port(failing);
        CHECK(throws<std::runtime_error>([&] { port.start(); }));
        CHECK(port.is_open() && !port.is_started());
    }
    CHECK_EQ(failing.handler_count(), 0u);

    SilKitEthPortOptions zero;
    zero.event_capacity = 0;
    CHECK(throws_with<std::invalid_argument>([&] { SilKitEthPort port(controller, zero); },
                                             "vehicle_sil::eth::SilKitEthPort: event_capacity must be > 0"));
}

TEST_CASE(silkit_eth_port_releases_frames_before_now_in_time_then_arrival_order)
{
    FakeEthernetController controller;
    SilKitEthPort port(controller);
    port.start();
    controller.deliver(vehicle_sil::unit::frame_bytes(kBroadcast, mac(3), 1, 0, 64), 2ms);
    controller.deliver(vehicle_sil::unit::frame_bytes(kBroadcast, mac(2), 1, 0, 64), 1ms);
    controller.deliver(vehicle_sil::unit::frame_bytes(kBroadcast, mac(1), 1, 0, 64), 2ms);
    controller.deliver(vehicle_sil::unit::frame_bytes(kBroadcast, mac(4), 1, 0, 64), 3ms);
    const auto got = release(port, at(3ms));
    CHECK_EQ(got.size(), 3u);
    CHECK(got[0].time == at(1ms));
    CHECK(got[1].frame.source() == mac(3)); // equal times: arrival order (the channel sorts)
    CHECK(got[2].frame.source() == mac(1));
    CHECK_EQ(release(port, at(4ms)).size(), 1u);
    CHECK_EQ(port.statistics().received, 4u);

    // Frames that do not fit into a Frame are counted, not queued.
    controller.deliver(std::vector<std::uint8_t>(Frame::kCapacity + 1, 0), 4ms);
    CHECK_EQ(port.statistics().receive_rejected, 1u);
    CHECK(release(port, at(10ms)).empty());
}

TEST_CASE(silkit_eth_port_gives_frames_without_time_the_time_of_the_latest_step)
{
    FakeEthernetController controller;
    SilKitEthPort port(controller);
    port.start();
    // Before the first step: time 0, released at the first step after 0.
    controller.deliver(frame_bytes(kBroadcast, mac(7), 1, 0, 64), kNoTime);
    CHECK(release(port, at(0ms)).empty());
    auto got = release(port, at(5ms));
    CHECK_EQ(got.size(), 1u);
    CHECK(got[0].time == at(0ms));

    // During the step at 5 ms: the time of that step, released at the next one, in time order
    // with the frames of synchronised senders.
    controller.deliver(frame_bytes(kBroadcast, mac(1), 1, 1, 64), 5ms);
    controller.deliver(frame_bytes(kBroadcast, mac(7), 1, 2, 64), kNoTime);
    controller.deliver(frame_bytes(kBroadcast, mac(1), 1, 3, 64), 4ms);
    got = release(port, at(6ms));
    CHECK_EQ(got.size(), 3u);
    CHECK(got[0].time == at(4ms));
    CHECK(got[1].time == at(5ms));
    CHECK_EQ(got[1].frame.bytes()[15], std::uint8_t{1});
    CHECK(got[2].time == at(5ms));
    CHECK(got[2].frame.source() == mac(7));

    // After the step at 6 ms started: 6 ms, released at the step after it.
    controller.deliver(frame_bytes(kBroadcast, mac(7), 1, 4, 64), kNoTime);
    CHECK(release(port, at(6ms)).empty());
    got = release(port, at(7ms));
    CHECK_EQ(got.size(), 1u);
    CHECK(got[0].time == at(6ms));
    CHECK_EQ(port.statistics().received_without_time, 3u);
}

TEST_CASE(silkit_eth_port_sends_and_queues_the_acknowledgements_for_the_next_step)
{
    FakeEthernetController controller;
    SilKitEthPort port(controller);
    port.start();
    controller.now = 5ms;
    const Frame frame = make_frame(mac(2), mac(1), 1, 0, 20);
    CHECK(port.send(TimedFrame{at(5ms), frame}) == SendResult::Accepted);
    CHECK_EQ(controller.sent().size(), 1u);
    // A frame shorter than 60 bytes goes out padded with zero bytes (a network simulator may
    // refuse a short one; a receiving SIL Kit controller would pad it anyway).
    std::vector<std::uint8_t> padded(frame.bytes().begin(), frame.bytes().end());
    CHECK(padded.size() < 60u);
    padded.resize(60, 0);
    CHECK(controller.sent()[0] == padded);
    CHECK(release_events(port, at(5ms)).empty());

    controller.ack_status = sk::EthernetTransmitStatus::LinkDown;
    CHECK(port.send(TimedFrame{at(5ms), frame}) == SendResult::Accepted);
    controller.ack_status = sk::EthernetTransmitStatus::Dropped;
    CHECK(port.send(TimedFrame{at(5ms), frame}) == SendResult::Accepted);
    const auto events = release_events(port, at(6ms));
    CHECK_EQ(events.size(), 3u);
    CHECK(events[0].kind == PortEvent::Kind::TransmitCompleted);
    CHECK(events[1].kind == PortEvent::Kind::TransmitFailed);
    CHECK(events[2].kind == PortEvent::Kind::TransmitFailed);
    CHECK(events[0].time == at(5ms));

    // Too short, or SendFrame() throws: Failed.
    CHECK(port.send(TimedFrame{at(6ms), *Frame::from(std::vector<std::uint8_t>(13, 0))}) == SendResult::Failed);
    controller.throw_on_send = true;
    CHECK(port.send(TimedFrame{at(6ms), frame}) == SendResult::Failed);
    const auto s = port.statistics();
    CHECK_EQ(s.sent, 3u);
    CHECK_EQ(s.send_failed, 2u);
    CHECK_EQ(s.tx_transmitted, 1u);
    CHECK_EQ(s.tx_link_down, 1u);
    CHECK_EQ(s.tx_dropped, 1u);
}

TEST_CASE(silkit_eth_port_reports_inbox_overflow_and_close_discards_what_is_queued)
{
    FakeEthernetController controller;
    SilKitEthPortOptions options;
    options.inbox_capacity = 2;
    SilKitEthPort port(controller, options);
    port.start();
    controller.now = 1ms;
    CHECK(port.send(TimedFrame{at(1ms), make_frame(kBroadcast, mac(1), 1, 0)}) == SendResult::Accepted);
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        controller.deliver(frame_bytes(kBroadcast, mac(2), 1, i, 64), 2ms);
    }
    CHECK_EQ(port.statistics().receive_overflows, 3u);
    const auto events = release_events(port, at(3ms));
    CHECK_EQ(events.size(), 2u);
    CHECK(events[0].kind == PortEvent::Kind::TransmitCompleted); // at 1 ms, before the overflow
    CHECK(events[1].kind == PortEvent::Kind::ReceiveOverflow);
    CHECK(events[1].time == at(2ms));
    CHECK_EQ(events[1].count, 3u);

    controller.deliver(frame_bytes(kBroadcast, mac(2), 1, 9, 64), 3ms);
    port.close();
    CHECK(release(port, at(10ms)).empty());
}

TEST_CASE(silkit_eth_port_reports_acknowledgements_its_event_queue_could_not_keep)
{
    // Acknowledgements dropped by a full event queue are reported (the channel's counts of
    // transmitted frames would otherwise fall short without a sign).
    FakeEthernetController controller;
    SilKitEthPortOptions options;
    options.event_capacity = 1;
    SilKitEthPort port(controller, options);
    port.start();
    controller.now = 1ms;
    for (std::uint8_t i = 0; i < 3; ++i)
    {
        CHECK(port.send(TimedFrame{at(1ms), make_frame(kBroadcast, mac(1), 1, i)}) == SendResult::Accepted);
    }
    controller.now = 2ms;
    CHECK(port.send(TimedFrame{at(2ms), make_frame(kBroadcast, mac(1), 1, 3)}) == SendResult::Accepted);
    CHECK_EQ(port.statistics().event_overflows, 3u);
    const auto events = release_events(port, at(3ms));
    CHECK_EQ(events.size(), 2u);
    CHECK(events[0].kind == PortEvent::Kind::TransmitCompleted);
    CHECK(events[0].time == at(1ms));
    CHECK(events[1].kind == PortEvent::Kind::EventOverflow);
    CHECK(events[1].time == at(1ms));
    CHECK_EQ(events[1].count, 3u);
    CHECK_EQ(port.statistics().outstanding, 0u);
    CHECK(release_events(port, at(10ms)).empty());
}

namespace {

/// In-memory IEthernetController that records SendFrame() calls and lets the test fire
/// transmit acknowledgements later, in any order and with any status.
class DeferredEthernetController final : public sk::IEthernetController
{
public:
    struct PendingSend
    {
        std::vector<std::uint8_t> bytes;
        void* user_context = nullptr;
    };

    bool throw_on_send = false;
    bool callback_then_throw = false;
    std::chrono::nanoseconds callback_time{0};
    std::vector<PendingSend> pending;

    void ack_index(std::size_t index, sk::EthernetTransmitStatus status, std::chrono::nanoseconds timestamp)
    {
        const auto context = pending.at(index).user_context;
        ack_raw(context, status, timestamp);
    }

    void ack_raw(void* user_context, sk::EthernetTransmitStatus status, std::chrono::nanoseconds timestamp)
    {
        const sk::EthernetFrameTransmitEvent ev{timestamp, status, user_context};
        for (const auto& [id, handler] : transmit_handlers_)
        {
            handler(this, ev);
        }
    }

    void Activate() override {}
    void Deactivate() override {}
    SilKit::Util::HandlerId AddFrameHandler(FrameHandler, SilKit::Services::DirectionMask) override { return next_id(); }
    void RemoveFrameHandler(SilKit::Util::HandlerId) override {}
    SilKit::Util::HandlerId AddFrameTransmitHandler(FrameTransmitHandler handler,
                                                    sk::EthernetTransmitStatusMask) override
    {
        const auto id = next_id();
        transmit_handlers_[id] = std::move(handler);
        return id;
    }
    void RemoveFrameTransmitHandler(SilKit::Util::HandlerId id) override { transmit_handlers_.erase(id); }
    SilKit::Util::HandlerId AddStateChangeHandler(StateChangeHandler) override { return next_id(); }
    void RemoveStateChangeHandler(SilKit::Util::HandlerId) override {}
    SilKit::Util::HandlerId AddBitrateChangeHandler(BitrateChangeHandler) override { return next_id(); }
    void RemoveBitrateChangeHandler(SilKit::Util::HandlerId) override {}
    void SendFrame(sk::EthernetFrame msg, void* userContext) override
    {
        if (callback_then_throw)
        {
            ack_raw(userContext, sk::EthernetTransmitStatus::Transmitted, callback_time);
            throw std::runtime_error("callback then throw");
        }
        if (throw_on_send)
        {
            throw std::runtime_error("send failed");
        }
        pending.push_back(PendingSend{std::vector<std::uint8_t>(msg.raw.begin(), msg.raw.end()), userContext});
    }

private:
    SilKit::Util::HandlerId next_id() { return static_cast<SilKit::Util::HandlerId>(++last_id_); }
    std::uint64_t last_id_ = 0;
    std::map<SilKit::Util::HandlerId, FrameTransmitHandler> transmit_handlers_;
};

} // namespace

TEST_CASE(silkit_eth_port_validates_options_and_reports_correlated_capability)
{
    FakeEthernetController controller;
    SilKitEthPort port(controller);
    CHECK(port.transmit_capabilities() == TransmitCapabilities::Correlated);

    CHECK(throws_with<std::invalid_argument>(
        [] { SilKitEthPort::validate(SilKitEthPortOptions{.inbox_capacity = 0}); }, "inbox_capacity must be > 0"));
    CHECK(throws_with<std::invalid_argument>(
        [] { SilKitEthPort::validate(SilKitEthPortOptions{.event_capacity = 0}); }, "event_capacity must be > 0"));
    CHECK(throws_with<std::invalid_argument>(
        [] { SilKitEthPort::validate(SilKitEthPortOptions{.event_capacity = 4, .transmit_queue_capacity = 5}); },
        "transmit_queue_capacity must be <= event_capacity"));
    CHECK(throws_with<std::invalid_argument>(
        [&] {
            SilKitEthPort bad(controller, SilKitEthPortOptions{.event_capacity = 4, .transmit_queue_capacity = 5});
        },
        "transmit_queue_capacity must be <= event_capacity"));
}

TEST_CASE(silkit_eth_port_correlates_out_of_order_acks_and_identical_frames_with_token_zero)
{
    DeferredEthernetController controller;
    SilKitEthPort port(controller);
    port.start();

    const Frame identical = make_frame(mac(2), mac(1), 1, 7, 64);
    const std::vector<std::uint64_t> tokens = {0, 11, 22, 33, 44};
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        CHECK(port.send(TimedFrame{at(1ms + std::chrono::milliseconds(i)), identical}, tokens[i]) ==
              SendResult::Accepted);
    }
    CHECK_EQ(port.statistics().outstanding, 5u);

    // Acknowledge out of order with every transmit status variant.
    controller.ack_index(3, sk::EthernetTransmitStatus::InvalidFrameFormat, 10ms);
    controller.ack_index(0, sk::EthernetTransmitStatus::Transmitted, 11ms);
    controller.ack_index(4, sk::EthernetTransmitStatus::ControllerInactive, 12ms);
    controller.ack_index(2, sk::EthernetTransmitStatus::Dropped, 13ms);
    controller.ack_index(1, sk::EthernetTransmitStatus::LinkDown, 14ms);

    // Replaying a stale context must NOT correlate or decrement outstanding.
    controller.ack_index(0, sk::EthernetTransmitStatus::Transmitted, 15ms);
    // Foreign nullptr context must remain uncorrelated.
    controller.ack_raw(nullptr, sk::EthernetTransmitStatus::Transmitted, 16ms);

    const auto events = release_events(port, at(20ms));
    CHECK_EQ(events.size(), 7u);

    CHECK((events[0] == PortEvent{PortEvent::Kind::TransmitFailed, at(10ms), 0, at(4ms), 33, true}));
    CHECK((events[1] == PortEvent{PortEvent::Kind::TransmitCompleted, at(11ms), 0, at(1ms), 0, true}));
    CHECK((events[2] == PortEvent{PortEvent::Kind::TransmitFailed, at(12ms), 0, at(5ms), 44, true}));
    CHECK((events[3] == PortEvent{PortEvent::Kind::TransmitFailed, at(13ms), 0, at(3ms), 22, true}));
    CHECK((events[4] == PortEvent{PortEvent::Kind::TransmitFailed, at(14ms), 0, at(2ms), 11, true}));
    CHECK(!events[5].correlated);
    CHECK(!events[6].correlated);
    CHECK_EQ(port.statistics().outstanding, 0u);
    CHECK_EQ(port.statistics().tx_invalid_frame, 1u);
    CHECK_EQ(port.statistics().tx_controller_inactive, 1u);
    CHECK_EQ(port.statistics().tx_dropped, 1u);
    CHECK_EQ(port.statistics().tx_link_down, 1u);
    CHECK_EQ(port.statistics().tx_transmitted, 3u);
    CHECK_EQ(port.statistics().tx_uncorrelated, 2u);
}

TEST_CASE(silkit_eth_port_enforces_transmit_queue_capacity_and_preserves_outstanding_on_close)
{
    DeferredEthernetController controller;
    SilKitEthPortOptions options;
    options.event_capacity = 4;
    options.transmit_queue_capacity = 2;
    SilKitEthPort port(controller, options);
    port.start();

    const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
    CHECK(port.send(TimedFrame{at(1ms), frame}, 100) == SendResult::Accepted);
    CHECK(port.send(TimedFrame{at(1ms), frame}, 200) == SendResult::Accepted);
    CHECK(port.send(TimedFrame{at(1ms), frame}, 300) == SendResult::QueueFull);
    CHECK_EQ(port.statistics().send_queue_full, 1u);
    CHECK_EQ(port.statistics().outstanding, 2u);

    controller.ack_index(0, sk::EthernetTransmitStatus::Transmitted, 2ms);
    const auto first = release_events(port, at(3ms));
    CHECK_EQ(first.size(), 1u);
    CHECK_EQ(first[0].token, 100u);
    CHECK_EQ(port.statistics().outstanding, 1u);

    // Room for one more frame now.
    CHECK(port.send(TimedFrame{at(3ms), frame}, 300) == SendResult::Accepted);
    CHECK_EQ(port.statistics().outstanding, 2u);

    // Close while 2 frames remain unacknowledged: outstanding stays inspectable for shutdown detection.
    port.close();
    CHECK_EQ(port.statistics().outstanding, 2u);
}

TEST_CASE(silkit_eth_port_does_not_leak_correlation_slots_on_overflow_or_send_exceptions)
{
    // 1. Repeated event_capacity overflows across > 2500 sends (exceeding 1024 correlation slots).
    {
        FakeEthernetController controller;
        SilKitEthPortOptions options;
        options.event_capacity = 2;
        options.transmit_queue_capacity = 0;
        SilKitEthPort port(controller, options);
        port.start();

        const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
        for (int burst = 0; burst < 250; ++burst)
        {
            const auto t = std::chrono::milliseconds(burst + 1);
            controller.now = t;
            for (int k = 0; k < 10; ++k)
            {
                const std::uint64_t tok = static_cast<std::uint64_t>(burst * 10 + k);
                CHECK(port.send(TimedFrame{at(t), frame}, tok) == SendResult::Accepted);
            }
            const auto events = release_events(port, at(t + 1ms));
            CHECK_EQ(events.size(), 3u); // 2 kept + 1 EventOverflow(count=8)
            CHECK(events[0].correlated);
            CHECK_EQ(events[0].token, static_cast<std::uint64_t>(burst * 10));
            CHECK(events[1].correlated);
            CHECK_EQ(events[1].token, static_cast<std::uint64_t>(burst * 10 + 1));
            CHECK(events[2].kind == PortEvent::Kind::EventOverflow);
            CHECK_EQ(events[2].count, 8u);
            CHECK_EQ(port.statistics().outstanding, 0u);
        }
        CHECK_EQ(port.statistics().send_queue_full, 0u);
        CHECK_EQ(port.statistics().event_overflows, 2000u);
    }

    // 2. Throwing SendFrame() (plain throw and callback-then-throw) reclaims slots cleanly.
    {
        DeferredEthernetController controller;
        SilKitEthPortOptions options;
        options.event_capacity = 4;
        SilKitEthPort port(controller, options);
        port.start();

        const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
        controller.throw_on_send = true;
        for (int i = 0; i < 1500; ++i)
        {
            CHECK(port.send(TimedFrame{at(1ms), frame}, static_cast<std::uint64_t>(i)) == SendResult::Failed);
        }
        CHECK_EQ(port.statistics().outstanding, 0u);
        CHECK_EQ(port.statistics().send_queue_full, 0u);

        controller.throw_on_send = false;
        controller.callback_then_throw = true;
        controller.callback_time = 2ms;
        CHECK(port.send(TimedFrame{at(2ms), frame}, 555) == SendResult::Failed);
        CHECK_EQ(port.statistics().outstanding, 0u);
        // Draining the event emitted before the throw must not underflow outstanding.
        static_cast<void>(release_events(port, at(3ms)));
        CHECK_EQ(port.statistics().outstanding, 0u);

        controller.callback_then_throw = false;
        CHECK(port.send(TimedFrame{at(3ms), frame}, 777) == SendResult::Accepted);
        controller.ack_index(0, sk::EthernetTransmitStatus::Transmitted, 4ms);
        const auto events = release_events(port, at(5ms));
        CHECK_EQ(events.size(), 1u);
        CHECK((events[0] == PortEvent{PortEvent::Kind::TransmitCompleted, at(4ms), 0, at(3ms), 777, true}));
        CHECK_EQ(port.statistics().outstanding, 0u);
    }
}

TEST_CASE(silkit_eth_port_uncorrelated_ack_overflow_does_not_decrement_outstanding)
{
    DeferredEthernetController controller;
    SilKitEthPortOptions options;
    options.event_capacity = 1;
    SilKitEthPort port(controller, options);
    port.start();

    const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
    CHECK(port.send(TimedFrame{at(1ms), frame}, 10) == SendResult::Accepted);
    CHECK(port.send(TimedFrame{at(1ms), frame}, 20) == SendResult::Accepted);
    CHECK_EQ(port.statistics().outstanding, 2u);

    // 4 uncorrelated acks overflow event_capacity=1 (1 queued, 3 dropped): must increment
    // tx_uncorrelated and must NOT emit EventOverflow or subtract from outstanding of the 2
    // genuinely in-flight correlated frames.
    for (int i = 0; i < 4; ++i)
    {
        controller.ack_raw(nullptr, sk::EthernetTransmitStatus::Transmitted, 2ms);
    }
    CHECK_EQ(port.statistics().tx_uncorrelated, 4u);

    const auto uncorrelated = release_events(port, at(3ms));
    CHECK_EQ(uncorrelated.size(), 1u);
    CHECK(uncorrelated[0].kind == PortEvent::Kind::TransmitCompleted);
    CHECK(!uncorrelated[0].correlated);
    CHECK_EQ(port.statistics().outstanding, 2u);

    // Genuine correlated acks arrive one by one and bring outstanding back to 0.
    controller.ack_index(0, sk::EthernetTransmitStatus::Transmitted, 3ms);
    const auto first = release_events(port, at(4ms));
    CHECK_EQ(first.size(), 1u);
    CHECK((first[0] == PortEvent{PortEvent::Kind::TransmitCompleted, at(3ms), 0, at(1ms), 10, true}));
    CHECK_EQ(port.statistics().outstanding, 1u);

    controller.ack_index(1, sk::EthernetTransmitStatus::Dropped, 4ms);
    const auto second = release_events(port, at(5ms));
    CHECK_EQ(second.size(), 1u);
    CHECK((second[0] == PortEvent{PortEvent::Kind::TransmitFailed, at(4ms), 0, at(1ms), 20, true}));
    CHECK_EQ(port.statistics().outstanding, 0u);
}
