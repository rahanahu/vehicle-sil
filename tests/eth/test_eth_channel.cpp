// SPDX-License-Identifier: MIT
//
// vehicle_sil::eth::Channel: a step::Member on a step::Host (with the step driver and the
// virtual network), the order of received frames, on_frame() and receive(), sending in the
// flush, the limits, the filter and the port's events. Links only vehicle_sil::eth (no SIL Kit).
#include "eth_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/eth/channel.hpp"
#include "vehicle_sil/eth/virtual_ethernet_bus.hpp"
#include "vehicle_sil/step/host.hpp"
#include "vehicle_sil/step/step_driver.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::eth::Accept;
using vehicle_sil::eth::Channel;
using vehicle_sil::eth::ChannelOptions;
using vehicle_sil::eth::Frame;
using vehicle_sil::eth::FrameSink;
using vehicle_sil::eth::kBroadcast;
using vehicle_sil::eth::Port;
using vehicle_sil::eth::PortEvent;
using vehicle_sil::eth::PortEventSink;
using vehicle_sil::eth::SendResult;
using vehicle_sil::eth::SendStatus;
using vehicle_sil::eth::TimedFrame;
using vehicle_sil::eth::TimeDomain;
using vehicle_sil::eth::VirtualEthernetBus;
using vehicle_sil::unit::frame_bytes;
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

/// A port whose received frames, events and send results the test decides.
class FakePort final : public Port
{
public:
    std::deque<TimedFrame> incoming; ///< released in this order (those before `now`)
    std::deque<PortEvent> events;
    std::vector<TimedFrame> sent;
    std::deque<SendResult> results; ///< for the next sends; Accepted when empty

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
    std::size_t release_before(SimTime now, FrameSink sink) override
    {
        std::size_t n = 0;
        while (!incoming.empty() && incoming.front().time < now)
        {
            sink(incoming.front());
            incoming.pop_front();
            ++n;
        }
        return n;
    }
    SendResult send(const TimedFrame& frame) override
    {
        SendResult result = SendResult::Accepted;
        if (!results.empty())
        {
            result = results.front();
            results.pop_front();
        }
        if (result == SendResult::Accepted)
        {
            sent.push_back(frame);
        }
        return result;
    }
    std::size_t release_events_before(SimTime now, PortEventSink sink) override
    {
        std::size_t n = 0;
        while (!events.empty() && events.front().time < now)
        {
            sink(events.front());
            events.pop_front();
            ++n;
        }
        return n;
    }
};

/// The channel's member functions, as the host calls them.
vehicle_sil::step::Member& member(Channel& channel)
{
    return channel;
}

/// "<source>/<tag>/<seq>@<ms>" of a received frame.
std::string describe(const TimedFrame& f)
{
    return std::to_string(f.frame.source().bytes[5]) + "/" + std::to_string(f.frame.bytes()[14]) + "/" +
           std::to_string(f.frame.bytes()[15]) + "@" + std::to_string(to_ns(f.time) / 1'000'000);
}

/// Runs one step of a single channel by hand: the stages of step::Host, with `application` in
/// stage 4.
template <typename Application>
void run_step(Channel& channel, SimTime now, Application&& application)
{
    member(channel).begin(now);
    member(channel).receive(now);
    member(channel).process();
    application();
    member(channel).flush();
}

} // namespace

TEST_CASE(channel_steps_on_a_host_frames_reach_the_others_at_their_next_step)
{
    // Two channels as step::Members of two hosts on the virtual network, stepped by the step
    // driver with 1 ms and 3 ms. A sends one frame per step; B answers each frame from its
    // handler in the same step; A takes the answers with receive().
    VirtualEthernetBus bus;
    Channel a(bus.attach());
    Channel b(bus.attach());
    vehicle_sil::step::Host hostA;
    vehicle_sil::step::Host hostB;
    hostA.add(a);
    hostB.add(b);

    std::vector<std::string> atB;
    std::vector<std::string> atA;
    std::uint8_t seq = 0;
    b.on_frame([&](const TimedFrame& f) {
        atB.push_back(describe(f));
        CHECK(b.send(frame_bytes(f.frame.source(), mac(2), 2, f.frame.bytes()[15], 64)) == SendStatus::Ok);
    });
    hostA.set_application([&](SimTime) {
        while (const auto f = a.receive())
        {
            atA.push_back(describe(*f));
        }
        CHECK(a.send(make_frame(kBroadcast, mac(1), 1, seq++)) == SendStatus::Ok);
    });

    vehicle_sil::step::StepDriver driver;
    driver.add(hostA, 1ms);
    driver.add(hostB, 3ms);
    driver.run_until(at(7ms));

    // A's frames of 0, 1, 2 ms reach B at 3 ms, with their send times; B answers at 3 ms, and A
    // takes the answers at 4 ms. Then 3, 4, 5 ms at 6 ms.
    const std::vector<std::string> expectedB = {"1/1/0@0", "1/1/1@1", "1/1/2@2", "1/1/3@3", "1/1/4@4", "1/1/5@5"};
    CHECK(atB == expectedB);
    const std::vector<std::string> expectedA = {"2/2/0@3", "2/2/1@3", "2/2/2@3"};
    CHECK(atA == expectedA);
    CHECK_EQ(a.statistics().frames_taken, 3u);
    CHECK_EQ(a.statistics().frames_sent, 7u);
    CHECK_EQ(b.statistics().frames_received, 6u);
    CHECK_EQ(b.statistics().frames_sent, 6u);
    CHECK(!hostA.failed());
    CHECK(!hostB.failed());
}

TEST_CASE(channel_handler_may_send_on_another_channel_of_the_same_host)
{
    // A gateway: one host with a channel on each of two networks. What arrives on the first is
    // passed on to the second from the handler, inside the same step.
    VirtualEthernetBus left;
    VirtualEthernetBus right;
    Channel source(left.attach());
    Channel inbound(left.attach());
    Channel outbound(right.attach());
    Channel sink(right.attach());
    vehicle_sil::step::Host sourceHost;
    vehicle_sil::step::Host gateway;
    vehicle_sil::step::Host sinkHost;
    sourceHost.add(source);
    gateway.add(inbound);
    gateway.add(outbound);
    sinkHost.add(sink);
    std::uint8_t seq = 0;
    sourceHost.set_application([&](SimTime) { CHECK(source.send(make_frame(kBroadcast, mac(1), 1, seq++)) == SendStatus::Ok); });
    inbound.on_frame([&](const TimedFrame& f) { CHECK(outbound.send(f.frame) == SendStatus::Ok); });
    std::vector<std::string> atSink;
    sink.on_frame([&](const TimedFrame& f) { atSink.push_back(describe(f)); });

    vehicle_sil::step::StepDriver driver;
    driver.add(sourceHost, 1ms);
    driver.add(gateway, 1ms);
    driver.add(sinkHost, 1ms);
    driver.run_until(at(4ms));
    // Sent at 0 ms, passed on at 1 ms, received at 2 ms (with the gateway's time).
    const std::vector<std::string> expected = {"1/1/0@1", "1/1/1@2"};
    CHECK(atSink == expected);
}

TEST_CASE(channel_orders_equal_times_by_source_address_then_arrival)
{
    FakePort port;
    // Released by time, then arrival (as every port does); equal times from three senders in
    // an order a backend's threads might give.
    port.incoming = {
        TimedFrame{at(1ms), make_frame(kBroadcast, mac(9), 1, 0)},
        TimedFrame{at(2ms), make_frame(kBroadcast, mac(3), 1, 0)},
        TimedFrame{at(2ms), make_frame(kBroadcast, mac(1), 1, 0)},
        TimedFrame{at(2ms), make_frame(kBroadcast, mac(3), 1, 1)},
        TimedFrame{at(2ms), make_frame(kBroadcast, mac(2), 1, 0)},
        TimedFrame{at(2ms), make_frame(kBroadcast, mac(1), 1, 1)},
    };
    Channel channel(port);
    std::vector<std::string> handled;
    channel.on_frame([&](const TimedFrame& f) { handled.push_back(describe(f)); });
    member(channel).start(std::nullopt);
    std::vector<std::string> taken;
    run_step(channel, at(3ms), [&] {
        while (const auto f = channel.receive())
        {
            taken.push_back(describe(*f));
        }
    });
    const std::vector<std::string> expected = {"9/1/0@1", "1/1/0@2", "1/1/1@2", "2/1/0@2", "3/1/0@2", "3/1/1@2"};
    CHECK(handled == expected);
    CHECK(taken == expected);
}

TEST_CASE(channel_receive_gives_only_this_steps_frames_and_send_and_receive_need_a_step)
{
    FakePort port;
    port.incoming = {TimedFrame{at(1ms), make_frame(kBroadcast, mac(9), 1, 0)},
                     TimedFrame{at(2ms), make_frame(kBroadcast, mac(9), 1, 1)}};
    Channel channel(port);
    CHECK(throws_with<std::logic_error>([&] { static_cast<void>(channel.receive()); },
                                        "vehicle_sil::eth::Channel: receive() is only allowed inside a step"));
    CHECK(throws_with<std::logic_error>([&] { static_cast<void>(channel.send(make_frame(kBroadcast, mac(1), 1, 0))); },
                                        "vehicle_sil::eth::Channel: send() is only allowed inside a step"));
    member(channel).start(std::nullopt);
    CHECK(throws_with<std::logic_error>([&] { channel.on_frame([](const TimedFrame&) {}); },
                                        "on_frame() is only allowed before the host starts"));

    // Step 2 ms: one frame, not taken. Step 3 ms: only the frame of 2 ms.
    run_step(channel, at(2ms), [] {});
    std::vector<std::string> taken;
    run_step(channel, at(3ms), [&] {
        while (const auto f = channel.receive())
        {
            taken.push_back(describe(*f));
        }
    });
    const std::vector<std::string> expected = {"9/1/1@2"};
    CHECK(taken == expected);
    CHECK_EQ(channel.statistics().frames_received, 2u);
    CHECK_EQ(channel.statistics().frames_taken, 1u);
    // Not counted before receive() is used (handlers only).
    CHECK_EQ(channel.statistics().frames_not_taken, 0u);

    // Step 4 ms: two frames, one taken: the other is counted.
    port.incoming = {TimedFrame{at(3ms), make_frame(kBroadcast, mac(9), 1, 2)},
                     TimedFrame{at(3ms), make_frame(kBroadcast, mac(9), 1, 3)}};
    run_step(channel, at(4ms), [&] { CHECK(channel.receive().has_value()); });
    CHECK_EQ(channel.statistics().frames_not_taken, 1u);
    // After the flush: no step.
    CHECK(throws<std::logic_error>([&] { static_cast<void>(channel.receive()); }));
}

TEST_CASE(channel_sends_in_the_flush_in_order_with_the_step_time_within_its_limits)
{
    FakePort port;
    ChannelOptions options;
    options.send_capacity = 2;
    options.max_frame_bytes = 100;
    Channel channel(port, options);
    member(channel).start(std::nullopt);
    run_step(channel, at(5ms), [&] {
        CHECK(channel.send(make_frame(mac(2), mac(1), 1, 0, 100)) == SendStatus::Ok);
        CHECK(port.sent.empty()); // not before the flush
        CHECK(channel.send(make_frame(mac(2), mac(1), 1, 1, 101)) == SendStatus::InvalidFrame);
        CHECK(channel.send(std::vector<std::uint8_t>(13, 0)) == SendStatus::InvalidFrame);
        CHECK(channel.send(frame_bytes(mac(2), mac(1), 1, 2, 14)) == SendStatus::Ok);
        CHECK(channel.send(make_frame(mac(2), mac(1), 1, 3)) == SendStatus::QueueFull);
    });
    CHECK_EQ(port.sent.size(), 2u);
    CHECK(port.sent[0].time == at(5ms));
    CHECK_EQ(port.sent[0].frame.bytes()[15], std::uint8_t{0});
    CHECK_EQ(port.sent[1].frame.size(), 16u); // frame_bytes() makes at least 16
    CHECK_EQ(channel.statistics().frames_sent, 2u);
    CHECK_EQ(channel.statistics().sends_rejected, 3u);

    // The next step starts with an empty send queue.
    run_step(channel, at(6ms), [&] {
        CHECK(channel.send(make_frame(mac(2), mac(1), 1, 4)) == SendStatus::Ok);
        CHECK(channel.send(make_frame(mac(2), mac(1), 1, 5)) == SendStatus::Ok);
    });
    CHECK_EQ(port.sent.size(), 4u);
    CHECK(port.sent[3].time == at(6ms));
}

TEST_CASE(channel_drops_what_the_port_does_not_accept)
{
    FakePort port;
    Channel channel(port);
    member(channel).start(std::nullopt);
    // Failed drops that frame; QueueFull drops that frame and the rest of the step.
    port.results = {SendResult::Accepted, SendResult::Failed, SendResult::Accepted, SendResult::QueueFull};
    run_step(channel, at(1ms), [&] {
        for (std::uint8_t i = 0; i < 6; ++i)
        {
            CHECK(channel.send(make_frame(mac(2), mac(1), 1, i)) == SendStatus::Ok);
        }
    });
    CHECK_EQ(port.sent.size(), 2u);
    CHECK_EQ(port.sent[0].frame.bytes()[15], std::uint8_t{0});
    CHECK_EQ(port.sent[1].frame.bytes()[15], std::uint8_t{2});
    CHECK_EQ(channel.statistics().frames_sent, 2u);
    CHECK_EQ(channel.statistics().frames_dropped, 4u);
}

TEST_CASE(channel_keeps_receive_capacity_frames_per_step_and_drops_long_ones)
{
    FakePort port;
    ChannelOptions options;
    options.receive_capacity = 3;
    options.max_frame_bytes = 100;
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        port.incoming.push_back(TimedFrame{at(1ms), make_frame(kBroadcast, mac(9), 1, i)});
    }
    port.incoming.push_back(TimedFrame{at(1ms), make_frame(kBroadcast, mac(9), 1, 9, 101)});
    port.incoming.push_back(TimedFrame{at(2ms), make_frame(kBroadcast, mac(9), 1, 10)});
    Channel channel(port, options);
    std::vector<std::string> handled;
    channel.on_frame([&](const TimedFrame& f) { handled.push_back(describe(f)); });
    member(channel).start(std::nullopt);
    run_step(channel, at(2ms), [] {});
    const std::vector<std::string> first = {"9/1/0@1", "9/1/1@1", "9/1/2@1"};
    CHECK(handled == first);
    CHECK_EQ(channel.statistics().receive_overflows, 2u);
    CHECK_EQ(channel.statistics().frames_invalid, 1u);
    // The next step has room again.
    run_step(channel, at(3ms), [] {});
    CHECK_EQ(handled.size(), 4u);
    CHECK(handled.back() == "9/1/10@2");
}

TEST_CASE(channel_with_accept_addressed_takes_its_own_broadcast_and_listed_groups)
{
    FakePort port;
    ChannelOptions options;
    options.mac = mac(5);
    options.accept = Accept::Addressed;
    options.multicast = {group(1)};
    const std::deque<TimedFrame> frames = {
        TimedFrame{at(1ms), make_frame(mac(5), mac(9), 1, 0)},     // own
        TimedFrame{at(1ms), make_frame(mac(6), mac(9), 1, 1)},     // another node
        TimedFrame{at(1ms), make_frame(kBroadcast, mac(9), 1, 2)}, // broadcast
        TimedFrame{at(1ms), make_frame(group(1), mac(9), 1, 3)},   // listed group
        TimedFrame{at(1ms), make_frame(group(2), mac(9), 1, 4)},   // another group
    };
    port.incoming = frames;
    Channel channel(port, options);
    std::vector<std::string> handled;
    channel.on_frame([&](const TimedFrame& f) { handled.push_back(describe(f)); });
    member(channel).start(std::nullopt);
    run_step(channel, at(2ms), [] {});
    const std::vector<std::string> expected = {"9/1/0@1", "9/1/2@1", "9/1/3@1"};
    CHECK(handled == expected);
    CHECK_EQ(channel.statistics().frames_filtered, 2u);

    // The default takes every frame.
    FakePort all;
    all.incoming = frames;
    Channel open(all);
    member(open).start(std::nullopt);
    run_step(open, at(2ms), [] {});
    CHECK_EQ(open.statistics().frames_received, 5u);
    CHECK_EQ(open.statistics().frames_filtered, 0u);
}

TEST_CASE(channel_counts_the_ports_events)
{
    FakePort port;
    port.events = {PortEvent{PortEvent::Kind::TransmitCompleted, at(1ms), 0},
                   PortEvent{PortEvent::Kind::TransmitFailed, at(1ms), 0},
                   PortEvent{PortEvent::Kind::ReceiveOverflow, at(1ms), 4},
                   PortEvent{PortEvent::Kind::EventOverflow, at(1ms), 3},
                   PortEvent{PortEvent::Kind::TransmitCompleted, at(5ms), 0}};
    Channel channel(port);
    member(channel).start(std::nullopt);
    run_step(channel, at(2ms), [] {});
    CHECK_EQ(channel.statistics().transmit_completed, 1u);
    CHECK_EQ(channel.statistics().transmit_failed, 1u);
    CHECK_EQ(channel.statistics().port_receive_overflows, 4u);
    CHECK_EQ(channel.statistics().port_event_overflows, 3u);
}

TEST_CASE(channel_refuses_options_it_cannot_work_with)
{
    FakePort port;
    ChannelOptions noSend;
    noSend.send_capacity = 0;
    CHECK(throws_with<std::invalid_argument>([&] { Channel c(port, noSend); },
                                             "send_capacity and receive_capacity must be > 0"));
    ChannelOptions tooShort;
    tooShort.max_frame_bytes = 13;
    CHECK(throws_with<std::invalid_argument>([&] { Channel c(port, tooShort); },
                                             "max_frame_bytes must be from 14 to 2048, not 13"));
    ChannelOptions tooLong;
    tooLong.max_frame_bytes = Frame::kCapacity + 1;
    CHECK(throws<std::invalid_argument>([&] { Channel c(port, tooLong); }));
    ChannelOptions noMac;
    noMac.accept = Accept::Addressed;
    CHECK(throws_with<std::invalid_argument>([&] { Channel c(port, noMac); },
                                             "Accept::Addressed needs the channel's own address (mac)"));
    Channel ok(port);
    CHECK(throws_with<std::invalid_argument>([&] { ok.on_frame({}); }, "empty handler"));
}
