// SPDX-License-Identifier: MIT
//
// vehicle_sil::eth values (MacAddress, Frame) and the virtual network: delivery like a hub, at
// the receiver's first step after the send time, padding, no echo, causality and overflow.
// Links only vehicle_sil::eth (no SIL Kit).
#include "eth_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/eth/frame.hpp"
#include "vehicle_sil/eth/virtual_ethernet_bus.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::eth::Frame;
using vehicle_sil::eth::kBroadcast;
using vehicle_sil::eth::MacAddress;
using vehicle_sil::eth::PortEvent;
using vehicle_sil::eth::SendResult;
using vehicle_sil::eth::TimedFrame;
using vehicle_sil::eth::VirtualEthernetBus;
using vehicle_sil::eth::VirtualEthernetBusOptions;
using vehicle_sil::unit::group;
using vehicle_sil::unit::mac;
using vehicle_sil::unit::make_frame;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

std::vector<TimedFrame> release(VirtualEthernetBus::Endpoint& endpoint, SimTime now)
{
    std::vector<TimedFrame> out;
    endpoint.release_before(now, [&](const TimedFrame& f) { out.push_back(f); });
    return out;
}

} // namespace

TEST_CASE(mac_address_parses_and_prints_six_hex_pairs)
{
    const auto a = MacAddress::parse("02:00:00:00:00:01");
    CHECK(a.has_value());
    CHECK(*a == mac(1));
    CHECK(a->to_string() == "02:00:00:00:00:01");
    const auto b = MacAddress::parse("0A-1b-FF-00-7f-80");
    CHECK(b.has_value());
    CHECK(b->to_string() == "0a:1b:ff:00:7f:80");
    CHECK(!MacAddress::parse("02:00:00:00:00").has_value());
    CHECK(!MacAddress::parse("02:00:00:00:00:001").has_value());
    CHECK(!MacAddress::parse("02:00-00:00:00:01").has_value());
    CHECK(!MacAddress::parse("02:00:00:00:00:0g").has_value());
    CHECK(!MacAddress::parse("02.00.00.00.00.01").has_value());
}

TEST_CASE(mac_address_tells_broadcast_and_multicast_apart)
{
    CHECK(kBroadcast.is_broadcast());
    CHECK(kBroadcast.is_multicast());
    CHECK(group(1).is_multicast());
    CHECK(!group(1).is_broadcast());
    CHECK(!mac(1).is_multicast());
    CHECK(mac(1) < mac(2));
    CHECK(mac(255) < group(0));
}

TEST_CASE(frame_holds_bytes_up_to_its_capacity_and_compares_them)
{
    Frame frame = make_frame(mac(2), mac(1), 7, 3, 20);
    CHECK_EQ(frame.size(), 20u);
    CHECK(frame.destination() == mac(2));
    CHECK(frame.source() == mac(1));
    CHECK_EQ(frame.bytes()[15], std::uint8_t{3});

    const Frame copy = frame;
    CHECK(copy == frame);
    frame.pad_to(60);
    CHECK_EQ(frame.size(), 60u);
    CHECK(!(copy == frame));
    CHECK_EQ(frame.bytes()[59], std::uint8_t{0});
    frame.pad_to(30); // already longer: unchanged
    CHECK_EQ(frame.size(), 60u);

    const std::vector<std::uint8_t> tooLong(Frame::kCapacity + 1, 0xAB);
    CHECK(!Frame::from(tooLong).has_value());
    CHECK(!frame.assign(tooLong));
    CHECK_EQ(frame.size(), 60u); // unchanged
    const std::vector<std::uint8_t> full(Frame::kCapacity, 0xAB);
    CHECK(Frame::from(full).has_value());

    const Frame shortFrame = *Frame::from(std::vector<std::uint8_t>{1, 2, 3});
    CHECK(shortFrame.source() == MacAddress{});
    CHECK(Frame{}.empty());
}

TEST_CASE(virtual_bus_delivers_to_every_other_endpoint_at_the_next_step)
{
    VirtualEthernetBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    auto& c = bus.attach();
    // To b only, but the network is a hub: c receives it as well.
    const Frame frame = make_frame(mac(2), mac(1), 1, 1, 64);
    CHECK(a.send(TimedFrame{at(5ms), frame}) == SendResult::Accepted);

    CHECK(release(b, at(5ms)).empty()); // time == now: the next step
    const auto atB = release(b, at(6ms));
    CHECK_EQ(atB.size(), 1u);
    CHECK(atB[0].time == at(5ms));
    CHECK(atB[0].frame == frame);
    CHECK_EQ(release(c, at(10ms)).size(), 1u);
    CHECK(release(a, at(10ms)).empty()); // no echo
}

TEST_CASE(virtual_bus_pads_short_frames_like_silkit)
{
    VirtualEthernetBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    const Frame shortFrame = make_frame(kBroadcast, mac(1), 1, 1, 20);
    CHECK(a.send(TimedFrame{at(1ms), shortFrame}) == SendResult::Accepted);
    const auto got = release(b, at(2ms));
    CHECK_EQ(got.size(), 1u);
    CHECK_EQ(got[0].frame.size(), 60u);
    Frame expected = shortFrame;
    expected.pad_to(60);
    CHECK(got[0].frame == expected);

    VirtualEthernetBusOptions noPadding;
    noPadding.min_frame_bytes = 0;
    VirtualEthernetBus plain(noPadding);
    auto& p = plain.attach();
    auto& q = plain.attach();
    CHECK(p.send(TimedFrame{at(1ms), shortFrame}) == SendResult::Accepted);
    CHECK_EQ(release(q, at(2ms)).at(0).frame.size(), 20u);
}

TEST_CASE(virtual_bus_refuses_frames_without_a_header_and_options_out_of_range)
{
    VirtualEthernetBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    const Frame tooShort = *Frame::from(std::vector<std::uint8_t>(13, 0));
    CHECK(a.send(TimedFrame{at(1ms), tooShort}) == SendResult::Failed);
    CHECK(release(b, at(2ms)).empty());

    VirtualEthernetBusOptions zero;
    zero.inbox_capacity = 0;
    CHECK(throws_with<std::invalid_argument>([&] { VirtualEthernetBus bad(zero); },
                                             "vehicle_sil::eth::VirtualEthernetBus: inbox_capacity must be > 0"));
    VirtualEthernetBusOptions tooLong;
    tooLong.min_frame_bytes = Frame::kCapacity + 1;
    CHECK(throws_with<std::invalid_argument>([&] { VirtualEthernetBus bad(tooLong); }, "min_frame_bytes must be at most"));
}

TEST_CASE(virtual_bus_refuses_a_frame_older_than_a_receivers_last_step)
{
    VirtualEthernetBus bus;
    auto& a = bus.attach();
    auto& b = bus.attach();
    static_cast<void>(release(b, at(10ms)));
    CHECK(throws_with<std::logic_error>([&] { a.send(TimedFrame{at(9ms), make_frame(kBroadcast, mac(1), 1, 1)}); },
                                        "step the hosts in time order"));
    CHECK(a.send(TimedFrame{at(10ms), make_frame(kBroadcast, mac(1), 1, 1)}) == SendResult::Accepted);
}

TEST_CASE(virtual_bus_counts_and_reports_inbox_overflow)
{
    VirtualEthernetBusOptions options;
    options.inbox_capacity = 2;
    VirtualEthernetBus bus(options);
    auto& a = bus.attach();
    auto& b = bus.attach();
    for (std::uint8_t i = 0; i < 4; ++i)
    {
        CHECK(a.send(TimedFrame{at(1ms + std::chrono::milliseconds(i)), make_frame(kBroadcast, mac(1), 1, i)}) ==
              SendResult::Accepted);
    }
    CHECK_EQ(b.overflows(), 2u);
    std::vector<PortEvent> events;
    b.release_events_before(at(3ms), [&](const PortEvent& e) { events.push_back(e); });
    CHECK(events.empty()); // the first dropped frame is at 3 ms
    b.release_events_before(at(10ms), [&](const PortEvent& e) { events.push_back(e); });
    CHECK_EQ(events.size(), 1u);
    CHECK(events[0].kind == PortEvent::Kind::ReceiveOverflow);
    CHECK(events[0].time == at(3ms));
    CHECK_EQ(events[0].count, 2u);
    CHECK_EQ(release(b, at(10ms)).size(), 2u);
}
