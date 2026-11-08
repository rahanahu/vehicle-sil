// SPDX-License-Identifier: MIT
//
// Fault injection of the virtual bus: asynchronous transmit failures, injected events, the
// overflow event, and outages of one endpoint. Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::can::BusState;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::Frame;
using vehicle_sil::can::OutageDirection;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::can::VirtualCanBusOptions;
using vehicle_sil::can::VirtualCanBusTiming;
using vehicle_sil::unit::throws;

namespace {

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

Frame frame(std::uint32_t id)
{
    Frame f;
    f.id = id;
    f.length = 1;
    return f;
}

std::vector<TimedFrame> frames_before(VirtualCanBus::Endpoint& endpoint, SimTime now)
{
    std::vector<TimedFrame> out;
    endpoint.release_before(now, [&](const TimedFrame& f) { out.push_back(f); });
    return out;
}

std::vector<PortEvent> events_before(VirtualCanBus::Endpoint& endpoint, SimTime now)
{
    std::vector<PortEvent> out;
    endpoint.release_events_before(now, [&](const PortEvent& e) { out.push_back(e); });
    return out;
}

VirtualCanBusOptions timed()
{
    VirtualCanBusOptions options;
    options.timing = VirtualCanBusTiming{};
    return options;
}

} // namespace

TEST_CASE(virtual_bus_transmit_failed_is_accepted_and_reported_later)
{
    for (const bool timing : {false, true})
    {
        VirtualCanBus bus(timing ? timed() : VirtualCanBusOptions{});
        auto& a = bus.attach();
        auto& b = bus.attach();
        a.set_fault_injector([](const TimedFrame& f) {
            return f.frame.id == 0x2 ? FaultAction::transmit_failed() : FaultAction::deliver();
        });
        CHECK(a.send(TimedFrame{at(1ms), frame(0x1)}) == SendResult::Accepted);
        CHECK(a.send(TimedFrame{at(1ms), frame(0x2)}) == SendResult::Accepted);
        CHECK(a.send(TimedFrame{at(1ms), frame(0x3)}) == SendResult::Accepted);
        const auto received = frames_before(b, at(10ms));
        CHECK_EQ(received.size(), 2u);
        CHECK_EQ(received[0].frame.id, 0x1u);
        CHECK_EQ(received[1].frame.id, 0x3u);
        // The failure is reported at the send time, before the completions (with timing).
        const auto events = events_before(a, at(10ms));
        CHECK_EQ(events.size(), timing ? 3u : 1u);
        CHECK(events[0].kind == PortEvent::Kind::TransmitFailed);
        CHECK_EQ(events[0].id, 0x2u);
        CHECK(events[0].time == at(1ms));
        CHECK(events[0].send_time == std::optional<SimTime>(at(1ms)));
        if (timing)
        {
            // The failed frame did not take the bus: 0x3 follows 0x1 directly.
            CHECK(events[1].kind == PortEvent::Kind::TransmitCompleted);
            CHECK(events[2].kind == PortEvent::Kind::TransmitCompleted);
            CHECK(received[1].time - received[0].time ==
                  vehicle_sil::can::frame_duration(frame(0x3), VirtualCanBusTiming{}.bitrate));
            CHECK_EQ(bus.transmissions(), 2u);
        }
    }
}

TEST_CASE(virtual_bus_releases_injected_events_in_time_order)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    PortEvent passive;
    passive.kind = PortEvent::Kind::BusStateChanged;
    passive.bus_state = BusState::ErrorPassive;
    passive.time = at(5ms);
    PortEvent errorFrame;
    errorFrame.kind = PortEvent::Kind::ErrorFrame;
    errorFrame.time = at(3ms);
    PortEvent busOff = passive;
    busOff.bus_state = BusState::BusOff;
    busOff.time = at(20ms);
    CHECK(a.inject_event(passive));
    CHECK(a.inject_event(errorFrame));
    CHECK(a.inject_event(busOff));
    auto events = events_before(a, at(10ms));
    CHECK_EQ(events.size(), 2u);
    CHECK(events[0].kind == PortEvent::Kind::ErrorFrame);
    CHECK(events[1].bus_state == BusState::ErrorPassive);
    events = events_before(a, at(30ms));
    CHECK_EQ(events.size(), 1u);
    CHECK(events[0].bus_state == BusState::BusOff);
}

TEST_CASE(virtual_bus_reports_an_inbox_overflow_as_one_event)
{
    VirtualCanBusOptions options;
    options.inbox_capacity = 2;
    VirtualCanBus bus(options);
    auto& a = bus.attach();
    auto& b = bus.attach();
    for (std::uint32_t id = 1; id <= 5; ++id)
    {
        CHECK(a.send(TimedFrame{at(std::chrono::milliseconds(id)), frame(id)}) == SendResult::Accepted);
    }
    CHECK_EQ(b.overflows(), 3u);
    // Events first (as a Host does): the three dropped frames are one event at the first drop.
    auto events = events_before(b, at(10ms));
    CHECK_EQ(events.size(), 1u);
    CHECK(events[0].kind == PortEvent::Kind::ReceiveOverflow);
    CHECK(events[0].time == at(3ms));
    CHECK_EQ(events[0].count, 3u);
    CHECK_EQ(frames_before(b, at(10ms)).size(), 2u);
    // An overflow that has not happened before `now` waits for a later release.
    CHECK(a.send(TimedFrame{at(20ms), frame(6)}) == SendResult::Accepted);
    CHECK(a.send(TimedFrame{at(20ms), frame(7)}) == SendResult::Accepted);
    CHECK(a.send(TimedFrame{at(20ms), frame(8)}) == SendResult::Accepted);
    CHECK(events_before(b, at(20ms)).empty());
    // Drops after a release that could not hand the event over yet join the same event.
    CHECK(a.send(TimedFrame{at(20ms), frame(9)}) == SendResult::Accepted);
    events = events_before(b, at(21ms));
    CHECK_EQ(events.size(), 1u);
    CHECK_EQ(events[0].count, 2u);
}

TEST_CASE(virtual_bus_outage_cuts_one_endpoint_off_for_a_time_span)
{
    for (const bool timing : {false, true})
    {
        VirtualCanBus bus(timing ? timed() : VirtualCanBusOptions{});
        auto& a = bus.attach();
        auto& b = bus.attach();
        auto& c = bus.attach();
        b.add_outage(at(10ms), at(20ms), OutageDirection::Receive);
        c.add_outage(at(10ms), at(20ms), OutageDirection::Send);
        bool asked = false;
        c.set_fault_injector([&](const TimedFrame&) {
            asked = true;
            return FaultAction::deliver();
        });
        for (const auto t : {5ms, 10ms, 19ms, 20ms})
        {
            CHECK(a.send(TimedFrame{at(t), frame(0x100)}) == SendResult::Accepted);
        }
        CHECK(c.send(TimedFrame{at(15ms), frame(0x200)}) == SendResult::Accepted); // lost
        CHECK(!asked);
        CHECK(c.send(TimedFrame{at(25ms), frame(0x201)}) == SendResult::Accepted);
        CHECK(asked);

        // b misses a's frames that reach it in [10, 20): with timing they arrive a little later
        // (the end of the transmission), so the 19 ms one lands at 19 ms + its duration < 20 ms.
        const auto atB = frames_before(b, at(30ms));
        std::vector<std::uint32_t> idsB;
        for (const auto& f : atB)
        {
            idsB.push_back(f.frame.id);
        }
        CHECK((idsB == std::vector<std::uint32_t>{0x100, 0x100, 0x201}));
        CHECK(atB[0].time < at(10ms));
        CHECK(atB[1].time >= at(20ms));
        CHECK_EQ(b.outage_dropped(), 2u);
        // c still receives all of a's frames; nobody gets c's frame from the outage.
        CHECK_EQ(frames_before(c, at(30ms)).size(), 4u);
        const auto atA = frames_before(a, at(30ms));
        CHECK_EQ(atA.size(), 1u);
        CHECK_EQ(atA[0].frame.id, 0x201u);
        CHECK_EQ(c.outage_lost(), 1u);
        const auto events = events_before(c, at(30ms));
        CHECK(!events.empty());
        CHECK(events[0].kind == PortEvent::Kind::TransmitFailed);
        CHECK_EQ(events[0].id, 0x200u);
        CHECK(events[0].time == at(15ms));
    }
}

TEST_CASE(virtual_bus_outage_both_ways_disconnects_and_reconnects)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    b.add_outage(at(10ms), at(20ms)); // Both
    CHECK(a.send(TimedFrame{at(12ms), frame(1)}) == SendResult::Accepted);
    CHECK(b.send(TimedFrame{at(12ms), frame(2)}) == SendResult::Accepted);
    CHECK(a.send(TimedFrame{at(22ms), frame(3)}) == SendResult::Accepted);
    CHECK(b.send(TimedFrame{at(22ms), frame(4)}) == SendResult::Accepted);
    const auto atA = frames_before(a, at(30ms));
    const auto atB = frames_before(b, at(30ms));
    CHECK_EQ(atA.size(), 1u);
    CHECK_EQ(atA[0].frame.id, 4u);
    CHECK_EQ(atB.size(), 1u);
    CHECK_EQ(atB[0].frame.id, 3u);
    CHECK(throws<std::invalid_argument>([&] { b.add_outage(at(5ms), at(5ms)); }));
    // A frame lost in an outage is still checked against the decided bus.
    CHECK(throws<std::logic_error>([&] { b.send(TimedFrame{at(12ms), frame(5)}); }));
}

TEST_CASE(virtual_bus_outage_judges_sending_by_the_send_time)
{
    // With timing, a frame sent before the outage that waits for the bus goes out during it.
    VirtualCanBus bus(timed());
    auto& a = bus.attach();
    auto& b = bus.attach();
    auto& c = bus.attach();
    b.add_outage(at(0ms) + 50us, at(20ms), OutageDirection::Send); // a's frame takes longer than 50 us
    CHECK(a.send(TimedFrame{at(0ms), frame(0x10)}) == SendResult::Accepted);
    CHECK(b.send(TimedFrame{at(0ms), frame(0x20)}) == SendResult::Accepted); // loses arbitration
    const auto received = frames_before(c, at(30ms));
    CHECK_EQ(received.size(), 2u);
    CHECK_EQ(received[1].frame.id, 0x20u);
    CHECK(received[1].time > at(0ms) + 50us);
    CHECK_EQ(b.outage_lost(), 0u);
}

TEST_CASE(virtual_bus_counts_events_beyond_the_event_queue)
{
    VirtualCanBusOptions options;
    options.inbox_capacity = 2; // also the event queue
    VirtualCanBus bus(options);
    auto& a = bus.attach();
    a.set_fault_injector([](const TimedFrame&) { return FaultAction::transmit_failed(); });
    for (int i = 0; i < 3; ++i)
    {
        CHECK(a.send(TimedFrame{at(1ms), frame(0x10)}) == SendResult::Accepted);
    }
    CHECK_EQ(a.event_overflows(), 1u);
    CHECK_EQ(events_before(a, at(2ms)).size(), 2u);
}
