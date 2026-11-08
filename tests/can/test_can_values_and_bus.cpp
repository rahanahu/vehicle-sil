// SPDX-License-Identifier: MIT
//
// vehicle_sil::can frame values and the virtual bus. Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::can::Frame;
using vehicle_sil::can::IdFormat;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::TimeDomain;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::can::VirtualCanBusOptions;

namespace {

Frame frame(std::uint32_t id, std::uint8_t first = 0, std::uint8_t length = 1)
{
    Frame f;
    f.id = id;
    f.length = length;
    f.data[0] = first;
    return f;
}

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

std::vector<TimedFrame> release(vehicle_sil::can::Port& port, SimTime now)
{
    std::vector<TimedFrame> out;
    port.release_before(now, [&](const TimedFrame& f) { out.push_back(f); });
    return out;
}

} // namespace

TEST_CASE(frame_validity_follows_the_identifier_format_and_length)
{
    Frame f;
    f.id = vehicle_sil::can::kMaxExtendedId;
    CHECK(is_valid(f));
    f.id = vehicle_sil::can::kMaxExtendedId + 1;
    CHECK(!is_valid(f));

    f.format = IdFormat::Standard;
    f.id = vehicle_sil::can::kMaxStandardId;
    CHECK(is_valid(f));
    f.id = vehicle_sil::can::kMaxStandardId + 1;
    CHECK(!is_valid(f));

    f.id = 1;
    f.length = 8;
    CHECK(is_valid(f));
    f.length = 9;
    CHECK(!is_valid(f));
    CHECK_EQ(f.payload().size(), 8u); // never past the array
}

TEST_CASE(frame_comparison_ignores_bytes_after_the_length)
{
    Frame a = frame(0x100, 7, 2);
    Frame b = a;
    b.data[5] = 0xAA; // not part of the frame
    CHECK(a == b);
    b.data[1] = 1;
    CHECK(!(a == b));
    b = a;
    b.format = IdFormat::Standard;
    CHECK(!(a == b));
    b = a;
    b.length = 3;
    CHECK(!(a == b));
    CHECK_EQ(a.payload().size(), 2u);
}

TEST_CASE(virtual_bus_delivers_to_every_other_endpoint_but_not_back_to_the_sender)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    auto& c = bus.attach();
    CHECK(a.time_domain() == TimeDomain::Virtual);
    CHECK(a.send(TimedFrame{at(0ms), frame(0x10)}) == SendResult::Accepted);

    CHECK(release(a, at(1ms)).empty());
    const auto atB = release(b, at(1ms));
    const auto atC = release(c, at(1ms));
    CHECK_EQ(atB.size(), 1u);
    CHECK_EQ(atC.size(), 1u);
    CHECK(atB[0] == (TimedFrame{at(0ms), frame(0x10)}));
    CHECK(atC[0] == atB[0]);
}

TEST_CASE(virtual_bus_releases_only_frames_stamped_before_now)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    CHECK(a.send(TimedFrame{at(5ms), frame(1)}) == SendResult::Accepted);
    CHECK(a.send(TimedFrame{at(10ms), frame(2)}) == SendResult::Accepted);

    CHECK(release(b, at(5ms)).empty()); // stamped == now waits for a later step
    auto out = release(b, at(10ms));
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].frame.id, 1u);
    CHECK_EQ(b.pending(), 1u);
    out = release(b, at(11ms));
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].frame.id, 2u);
    CHECK(out[0].time == at(10ms));
}

TEST_CASE(virtual_bus_orders_by_time_and_keeps_arrival_order_for_equal_times)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    auto& rx = bus.attach();
    // Equal times, interleaved senders, ids deliberately not in arbitration order.
    CHECK(a.send(TimedFrame{at(3ms), frame(0x300, 1)}) == SendResult::Accepted);
    CHECK(b.send(TimedFrame{at(3ms), frame(0x100, 2)}) == SendResult::Accepted);
    CHECK(a.send(TimedFrame{at(3ms), frame(0x200, 3)}) == SendResult::Accepted);
    // An earlier time sent later still comes first.
    CHECK(b.send(TimedFrame{at(2ms), frame(0x400, 4)}) == SendResult::Accepted);

    const auto out = release(rx, at(4ms));
    CHECK_EQ(out.size(), 4u);
    CHECK_EQ(out[0].frame.data[0], 4);
    CHECK_EQ(out[1].frame.data[0], 1);
    CHECK_EQ(out[2].frame.data[0], 2);
    CHECK_EQ(out[3].frame.data[0], 3);
}

TEST_CASE(virtual_bus_rejects_invalid_frames)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    Frame bad = frame(1);
    bad.length = 9;
    CHECK(a.send(TimedFrame{at(0ms), bad}) == SendResult::Failed);
    bad = frame(vehicle_sil::can::kMaxStandardId + 1);
    bad.format = IdFormat::Standard;
    CHECK(a.send(TimedFrame{at(0ms), bad}) == SendResult::Failed);
    CHECK(release(b, at(1ms)).empty());
}

TEST_CASE(virtual_bus_counts_inbox_overflow)
{
    VirtualCanBus bus(VirtualCanBusOptions{2});
    auto& a = bus.attach();
    auto& b = bus.attach();
    for (std::uint8_t i = 0; i < 3; ++i)
    {
        CHECK(a.send(TimedFrame{at(0ms), frame(i, i)}) == SendResult::Accepted);
    }
    CHECK_EQ(b.overflows(), 1u);
    const auto out = release(b, at(1ms));
    CHECK_EQ(out.size(), 2u);
    CHECK_EQ(out[0].frame.data[0], 0);
    CHECK_EQ(out[1].frame.data[0], 1);
    CHECK_EQ(a.overflows(), 0u);
}

TEST_CASE(virtual_bus_endpoint_attached_later_misses_earlier_frames)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    CHECK(a.send(TimedFrame{at(0ms), frame(1)}) == SendResult::Accepted);
    auto& late = bus.attach();
    CHECK(a.send(TimedFrame{at(1ms), frame(2)}) == SendResult::Accepted);
    const auto out = release(late, at(2ms));
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].frame.id, 2u);
    CHECK_EQ(bus.endpoint_count(), 2u);
}

TEST_CASE(virtual_bus_rejects_a_frame_older_than_a_receivers_last_step)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    CHECK(release(b, at(10ms)).empty()); // b has stepped to 10 ms
    CHECK(a.send(TimedFrame{at(10ms), frame(1)}) == SendResult::Accepted); // not older: fine
    CHECK(vehicle_sil::unit::throws_with<std::logic_error>([&] { a.send(TimedFrame{at(9ms), frame(2)}); }, "older"));
    const auto out = release(b, at(11ms));
    CHECK_EQ(out.size(), 1u); // the rejected frame reached nobody
    CHECK_EQ(out[0].frame.id, 1u);
}

TEST_CASE(virtual_bus_rejects_zero_capacity)
{
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { VirtualCanBus bus(VirtualCanBusOptions{0}); }));
}
