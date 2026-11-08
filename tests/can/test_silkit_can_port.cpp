// SPDX-License-Identifier: MIT
//
// vehicle_sil::can::SilKitCanPort against an in-memory SIL Kit CAN controller.
#include "fake_can_controller.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
namespace sk = SilKit::Services::Can;
using vehicle_sil::SimTime;
using vehicle_sil::can::BusState;
using vehicle_sil::can::Frame;
using vehicle_sil::can::ManualRealTimeClock;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::IdFormat;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::SilKitCanPort;
using vehicle_sil::can::SilKitCanPortOptions;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::TimeDomain;
using vehicle_sil::unit::extended_frame;
using vehicle_sil::unit::FakeCanController;
using vehicle_sil::unit::flag;
using vehicle_sil::unit::make_frame;

namespace {

std::vector<TimedFrame> release(SilKitCanPort& port, SimTime now)
{
    std::vector<TimedFrame> out;
    port.release_before(now, [&](const TimedFrame& f) { out.push_back(f); });
    return out;
}

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

} // namespace

TEST_CASE(silkit_port_manages_the_controller_and_its_handlers)
{
    FakeCanController controller;
    {
        SilKitCanPort port(controller);
        CHECK(port.is_open());
        CHECK(port.time_domain() == TimeDomain::Virtual);
        CHECK(controller.started());
        CHECK_EQ(controller.frame_handler_count(), 1u);
        CHECK_EQ(controller.transmit_handler_count(), 1u);
        CHECK_EQ(controller.error_state_handler_count(), 1u);
        // RX only: SIL Kit's trivial simulation also hands a sender its own frames as TX.
        CHECK((controller.frame_handler_masks() ==
               std::vector<SilKit::Services::DirectionMask>{
                   static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX)}));

        port.close();
        CHECK(!port.is_open());
        CHECK(!controller.started());
        CHECK_EQ(controller.frame_handler_count(), 0u);
        CHECK_EQ(controller.transmit_handler_count(), 0u);
        CHECK_EQ(controller.error_state_handler_count(), 0u);
        port.close(); // idempotent
    }
    // The destructor closes as well.
    {
        SilKitCanPort port(controller);
        CHECK_EQ(controller.frame_handler_count(), 1u);
    }
    CHECK_EQ(controller.frame_handler_count(), 0u);
    CHECK(!controller.started());
}

TEST_CASE(silkit_port_construction_failure_leaves_no_handlers)
{
    FakeCanController controller;
    controller.throw_on_start = true;
    CHECK(vehicle_sil::unit::throws<std::runtime_error>([&] { SilKitCanPort port(controller); }));
    CHECK_EQ(controller.frame_handler_count(), 0u);
    CHECK_EQ(controller.transmit_handler_count(), 0u);
}

TEST_CASE(silkit_port_rejects_a_zero_inbox_capacity)
{
    FakeCanController controller;
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { SilKitCanPort port(controller, SilKitCanPortOptions{0}); }));
    CHECK_EQ(controller.frame_handler_count(), 0u);
    // A transmit queue larger than the event queue could lose acknowledgements for good.
    SilKitCanPortOptions options;
    options.event_capacity = 8;
    options.transmit_queue_capacity = 9;
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { SilKitCanPort port(controller, options); }));
    CHECK_EQ(controller.frame_handler_count(), 0u);
}

TEST_CASE(silkit_frame_conversion_accepts_only_classic_data_frames)
{
    const std::vector<std::uint8_t> eight{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint8_t> two{0xAB, 0xCD};

    auto ok = vehicle_sil::can::from_silkit_frame(extended_frame(0x18FEF100, two));
    CHECK(ok.has_value());
    CHECK_EQ(ok->id, 0x18FEF100u);
    CHECK(ok->format == IdFormat::Extended);
    CHECK_EQ(ok->length, 2);
    CHECK_EQ(ok->data[1], 0xCD);

    ok = vehicle_sil::can::from_silkit_frame(make_frame(0x123, 0, 2, two));
    CHECK(ok.has_value() && ok->format == IdFormat::Standard);
    // DLC 9..15 mean 8 bytes in classic CAN.
    CHECK(vehicle_sil::can::from_silkit_frame(make_frame(1, flag(sk::CanFrameFlag::Ide), 15, eight)).has_value());

    CHECK(!vehicle_sil::can::from_silkit_frame(make_frame(1, flag(sk::CanFrameFlag::Ide) | flag(sk::CanFrameFlag::Rtr), 2, two)));
    CHECK(!vehicle_sil::can::from_silkit_frame(make_frame(1, flag(sk::CanFrameFlag::Ide) | flag(sk::CanFrameFlag::Fdf), 2, two)));
    CHECK(!vehicle_sil::can::from_silkit_frame(make_frame(1, flag(sk::CanFrameFlag::Ide) | flag(sk::CanFrameFlag::Xlf), 2, two)));
    CHECK(!vehicle_sil::can::from_silkit_frame(make_frame(1, flag(sk::CanFrameFlag::Ide), 3, two))); // DLC mismatch
    CHECK(!vehicle_sil::can::from_silkit_frame(make_frame(0x800, 0, 2, two)));                         // > 11 bits
    CHECK(!vehicle_sil::can::from_silkit_frame(extended_frame(0x2000'0000, two)));                     // > 29 bits
    const std::vector<std::uint8_t> nine(9, 0);
    CHECK(!vehicle_sil::can::from_silkit_frame(make_frame(1, flag(sk::CanFrameFlag::Ide), 9, nine)));

    Frame f;
    f.id = 0x7FF;
    f.format = IdFormat::Standard;
    f.length = 3;
    f.data = {9, 8, 7};
    sk::CanFrame out{};
    CHECK(vehicle_sil::can::to_silkit_frame(f, out));
    CHECK_EQ(out.canId, 0x7FFu);
    CHECK_EQ(out.flags, 0u);
    CHECK_EQ(out.dlc, 3);
    CHECK_EQ(out.dataField.size(), 3u);
    CHECK_EQ(out.dataField[2], 7);
    f.length = 9;
    CHECK(!vehicle_sil::can::to_silkit_frame(f, out));
}

TEST_CASE(silkit_port_releases_frames_before_now_in_time_then_arrival_order)
{
    FakeCanController controller;
    SilKitCanPort port(controller);
    const std::vector<std::uint8_t> d1{1}, d2{2}, d3{3}, d4{4}, d5{5};
    controller.deliver(extended_frame(0x300, d1), 3ms);
    controller.deliver(extended_frame(0x100, d2), 3ms);
    controller.deliver(extended_frame(0x200, d3), 2ms);
    controller.deliver(extended_frame(0x050, d4), 3ms);
    controller.deliver(extended_frame(0x400, d5), 4ms);
    controller.deliver(make_frame(1, flag(sk::CanFrameFlag::Rtr), 0, {}), 1ms); // rejected

    CHECK(release(port, at(2ms)).empty());
    const auto out = release(port, at(4ms));
    CHECK_EQ(out.size(), 4u);
    CHECK_EQ(out[0].frame.data[0], 3);
    CHECK(out[0].time == at(2ms));
    CHECK_EQ(out[1].frame.data[0], 1); // equal times keep the arrival order (no arbitration)
    CHECK_EQ(out[2].frame.data[0], 2);
    CHECK_EQ(out[3].frame.data[0], 4);
    const auto later = release(port, at(5ms));
    CHECK_EQ(later.size(), 1u);
    CHECK(later[0].time == at(4ms));

    const auto stats = port.statistics();
    CHECK_EQ(stats.received, 5u);
    CHECK_EQ(stats.receive_rejected, 1u);
    CHECK_EQ(stats.receive_overflows, 0u);
}

TEST_CASE(silkit_port_counts_inbox_overflow_and_close_discards_queued_frames)
{
    FakeCanController controller;
    SilKitCanPort port(controller, SilKitCanPortOptions{2});
    const std::vector<std::uint8_t> d{0};
    controller.deliver(extended_frame(1, d), 0ms);
    controller.deliver(extended_frame(2, d), 0ms);
    controller.deliver(extended_frame(3, d), 0ms);
    CHECK_EQ(port.statistics().receive_overflows, 1u);
    CHECK_EQ(port.statistics().received, 2u);

    port.close();
    CHECK(release(port, at(10ms)).empty());
}

TEST_CASE(silkit_port_sends_valid_frames_and_counts_acknowledgements)
{
    FakeCanController controller;
    SilKitCanPort port(controller);
    Frame f;
    f.id = 0x18EAFF00;
    f.length = 3;
    f.data = {0x00, 0xEE, 0x00};
    CHECK(port.send(TimedFrame{at(0ms), f}) == SendResult::Accepted);
    controller.ack_status = sk::CanTransmitStatus::TransmitQueueFull;
    CHECK(port.send(TimedFrame{at(0ms), f}) == SendResult::Accepted); // trivial: no synchronous result

    const auto sent = controller.sent();
    CHECK_EQ(sent.size(), 2u);
    CHECK_EQ(sent[0].canId, 0x18EAFF00u);
    CHECK_EQ(sent[0].flags, flag(sk::CanFrameFlag::Ide));
    CHECK_EQ(sent[0].dlc, 3);
    CHECK((sent[0].data == std::vector<std::uint8_t>{0x00, 0xEE, 0x00}));

    auto stats = port.statistics();
    CHECK_EQ(stats.sent, 2u);
    CHECK_EQ(stats.tx_transmitted, 1u);
    CHECK_EQ(stats.tx_queue_full, 1u);

    f.length = 9;
    CHECK(port.send(TimedFrame{at(0ms), f}) == SendResult::Failed);
    f.length = 1;
    controller.throw_on_send = true;
    CHECK(port.send(TimedFrame{at(0ms), f}) == SendResult::Failed);
    controller.throw_on_send = false;
    port.close();
    CHECK(port.send(TimedFrame{at(0ms), f}) == SendResult::Failed);
    stats = port.statistics();
    CHECK_EQ(stats.send_failed, 3u);
    CHECK_EQ(stats.sent, 2u);
    CHECK_EQ(controller.sent().size(), 2u);
}

TEST_CASE(silkit_bind_host_requires_virtual_time)
{
    vehicle_sil::unit::InProcessRegistry registry;
    vehicle_sil::ParticipantRuntimeOptions options;
    options.participant_name = "NoSync";
    options.registry_uri = registry.uri;
    vehicle_sil::ParticipantRuntime runtime(options);
    vehicle_sil::can::Host host;
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { vehicle_sil::can::bind_host(runtime, host); }));
}

namespace {

std::vector<PortEvent> release_events(SilKitCanPort& port, SimTime now)
{
    std::vector<PortEvent> out;
    port.release_events_before(now, [&](const PortEvent& e) { out.push_back(e); });
    return out;
}

} // namespace

TEST_CASE(silkit_port_reports_error_states_and_inbox_overflow_as_events)
{
    FakeCanController controller;
    SilKitCanPort port(controller, SilKitCanPortOptions{2});
    controller.deliver_error_state(sk::CanErrorState::ErrorPassive, 3ms);
    controller.deliver_error_state(sk::CanErrorState::NotAvailable, 4ms); // not a bus state
    controller.deliver_error_state(sk::CanErrorState::BusOff, 5ms);
    const std::vector<std::uint8_t> d{0};
    for (std::uint32_t id = 1; id <= 4; ++id)
    {
        controller.deliver(extended_frame(id, d), 2ms);
    }
    const auto events = release_events(port, at(10ms));
    CHECK_EQ(events.size(), 3u);
    CHECK(events[0].kind == PortEvent::Kind::ReceiveOverflow);
    CHECK(events[0].time == at(2ms));
    CHECK_EQ(events[0].count, 2u);
    CHECK(events[1].kind == PortEvent::Kind::BusStateChanged);
    CHECK(events[1].bus_state == BusState::ErrorPassive);
    CHECK(events[1].time == at(3ms));
    CHECK(events[2].bus_state == BusState::BusOff);
    CHECK_EQ(port.statistics().error_state_changes, 2u);
    CHECK_EQ(port.statistics().receive_overflows, 2u);
}

TEST_CASE(silkit_port_frees_its_transmit_queue_only_on_acknowledgements)
{
    FakeCanController controller;
    SilKitCanPortOptions options;
    options.transmit_queue_capacity = 1;
    SilKitCanPort port(controller, options);
    controller.ack_timestamp = 50ms; // a network simulator acknowledges at the end of the transmission
    Frame f;
    f.id = 0x100;
    f.length = 1;
    CHECK(port.send(TimedFrame{at(0ms), f}) == SendResult::Accepted);
    // A bus state event released before the acknowledgement does not free the place.
    controller.deliver_error_state(sk::CanErrorState::ErrorPassive, 1ms);
    CHECK_EQ(release_events(port, at(10ms)).size(), 1u);
    CHECK_EQ(port.statistics().outstanding, 1u);
    CHECK(port.send(TimedFrame{at(10ms), f}) == SendResult::QueueFull);
    CHECK_EQ(release_events(port, at(60ms)).size(), 1u);
    CHECK_EQ(port.statistics().outstanding, 0u);
    CHECK(port.send(TimedFrame{at(60ms), f}) == SendResult::Accepted);
}

TEST_CASE(silkit_port_on_real_time_stamps_arrivals_and_wakes_the_driver)
{
    FakeCanController controller;
    ManualRealTimeClock clock(at(7ms));
    SilKitCanPortOptions options;
    options.real_time_clock = &clock;
    SilKitCanPort port(controller, options);
    CHECK(port.time_domain() == TimeDomain::RealTime);
    std::vector<SimTime> wakes;
    port.set_wakeup([&](SimTime t) { wakes.push_back(t); });

    // SIL Kit's timestamps (here 0 and a negative one, as unsynchronised senders give) are
    // replaced by the arrival time on the clock.
    const std::vector<std::uint8_t> d{1};
    controller.deliver(extended_frame(1, d), 0ms);
    controller.deliver(extended_frame(2, d), std::chrono::nanoseconds::min());
    controller.deliver_error_state(sk::CanErrorState::ErrorPassive, 0ms);
    CHECK_EQ(wakes.size(), 3u);
    CHECK(wakes[0] == at(7ms));
    CHECK(wakes[1] == at(7ms) + 1ns);

    const auto events = release_events(port, at(8ms));
    CHECK_EQ(events.size(), 1u);
    CHECK(events[0].time == at(7ms) + 2ns);
    const auto frames = release(port, at(8ms));
    CHECK_EQ(frames.size(), 2u);
    CHECK(frames[0].time == at(7ms));
    CHECK(frames[1].time == at(7ms) + 1ns);

    // Sending: no send time tag in the userContext; the acknowledgement is stamped on arrival.
    Frame f;
    f.id = 0x18FF0001;
    f.length = 2;
    CHECK(port.send(TimedFrame{at(8ms), f}) == SendResult::Accepted);
    const auto acks = release_events(port, at(9ms));
    CHECK_EQ(acks.size(), 1u);
    CHECK(acks[0].kind == PortEvent::Kind::TransmitCompleted);
    CHECK(acks[0].time == at(8ms));          // after the released 8 ms
    CHECK(acks[0].send_time == std::nullopt); // no tag
    CHECK_EQ(wakes.size(), 4u);

    port.close();
    controller.deliver(extended_frame(3, d), 0ms); // no handler any more
    CHECK_EQ(wakes.size(), 4u);
}

TEST_CASE(silkit_port_kinds_are_checked_against_the_drivers)
{
    FakeCanController controller;
    ManualRealTimeClock clock;
    SilKitCanPortOptions options;
    options.real_time_clock = &clock;
    SilKitCanPort port(controller, options);
    vehicle_sil::unit::InProcessRegistry registry;
    vehicle_sil::ParticipantRuntimeOptions runtimeOptions;
    runtimeOptions.participant_name = "Synced";
    runtimeOptions.registry_uri = registry.uri;
    runtimeOptions.virtual_time = vehicle_sil::VirtualTimeOptions{};
    vehicle_sil::ParticipantRuntime runtime(runtimeOptions);
    vehicle_sil::can::Host host;
    struct Nothing final : vehicle_sil::can::Steppable
    {
        void begin(SimTime) override {}
        void receive(const TimedFrame&) override {}
        void process() override {}
        void flush(vehicle_sil::can::Port&) override {}
    } nothing;
    host.add(nothing, port);
    // A real-time port cannot be stepped by SIL Kit's time synchronisation.
    CHECK(vehicle_sil::unit::throws<std::logic_error>([&] { vehicle_sil::can::bind_host(runtime, host); }));
    vehicle_sil::can::RealTimeDriver driver(host, clock); // but by a real-time driver
}

TEST_CASE(silkit_port_keeps_places_for_outstanding_acknowledgements)
{
    FakeCanController controller;
    SilKitCanPortOptions options;
    options.event_capacity = 3;
    options.transmit_queue_capacity = 1;
    SilKitCanPort port(controller, options);
    Frame f;
    f.id = 0x100;
    f.length = 1;
    CHECK(port.send(TimedFrame{at(0ms), f}) == SendResult::Accepted); // its acknowledgement is queued
    // One place stays free for the outstanding frame: one error state fits, the next does not.
    controller.deliver_error_state(sk::CanErrorState::ErrorPassive, 1ms);
    controller.deliver_error_state(sk::CanErrorState::BusOff, 2ms);
    CHECK_EQ(port.statistics().error_state_changes, 2u);
    CHECK_EQ(port.statistics().event_overflows, 1u);
    const auto events = release_events(port, at(10ms));
    CHECK_EQ(events.size(), 2u);
    CHECK(events[0].kind == PortEvent::Kind::TransmitCompleted);
    CHECK(events[1].bus_state == BusState::ErrorPassive);
    CHECK_EQ(port.statistics().outstanding, 0u);
}
