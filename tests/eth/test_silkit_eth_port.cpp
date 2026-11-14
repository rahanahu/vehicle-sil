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
    CHECK(release_events(port, at(10ms)).empty());
}
