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
#include <limits>
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
using vehicle_sil::eth::TransmitCapabilities;
using vehicle_sil::eth::TransmitEvent;
using vehicle_sil::eth::VirtualEthernetBus;
using vehicle_sil::eth::VirtualEthernetEndpointOptions;
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

TEST_CASE(channel_says_how_large_the_inbox_of_its_port_must_be)
{
    // One step's frames and the frames that carry the time of the next step.
    CHECK_EQ(Channel::port_inbox_capacity({}), std::size_t{2048});
    CHECK_EQ(Channel::port_inbox_capacity({.receive_capacity = 8}), std::size_t{16});
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    CHECK_EQ(Channel::port_inbox_capacity({.receive_capacity = kMax / 2}), kMax - 1);
    CHECK_EQ(Channel::port_inbox_capacity({.receive_capacity = kMax / 2 + 1}), kMax);
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
    CHECK(throws_with<std::invalid_argument>([&] { ok.on_transmit({}); }, "empty handler"));

    ChannelOptions badTxQueue;
    badTxQueue.port_event_capacity = 8;
    badTxQueue.port_transmit_queue_capacity = 9;
    CHECK(throws_with<std::invalid_argument>([&] { Channel::validate(badTxQueue); },
                                             "port_transmit_queue_capacity must be <= port_event_capacity"));
    CHECK(throws_with<std::invalid_argument>([&] { Channel c(port, badTxQueue); },
                                             "port_transmit_queue_capacity must be <= port_event_capacity"));
}

TEST_CASE(channel_derives_port_inbox_and_event_capacities_with_overrides)
{
    CHECK_EQ(Channel::port_inbox_capacity({.receive_capacity = 8, .port_inbox_capacity = 100}), std::size_t{100});
    CHECK_EQ(Channel::port_event_capacity({}), std::size_t{1024});
    CHECK_EQ(Channel::port_event_capacity({.send_capacity = 600}), std::size_t{1200});
    CHECK_EQ(Channel::port_event_capacity({.send_capacity = 100, .transmit_event_capacity = 2000}), std::size_t{2000});
    CHECK_EQ(Channel::port_event_capacity({.send_capacity = 100, .port_transmit_queue_capacity = 1500}),
             std::size_t{1600});
    CHECK_EQ(Channel::port_event_capacity({.send_capacity = 100, .port_event_capacity = 64}), std::size_t{64});
}

TEST_CASE(channel_on_transmit_fires_before_on_frame_in_next_step_with_token_zero_and_reentrancy)
{
    VirtualEthernetBus bus;
    auto& epA = bus.attach();
    auto& epB = bus.attach();
    Channel a(epA);
    Channel b(epB);
    CHECK(a.transmit_capabilities() == TransmitCapabilities::Correlated);

    std::vector<std::string> trace;
    std::vector<TransmitEvent> txEvents;
    bool reentrantSent = false;
    a.on_transmit([&](const TransmitEvent& ev) {
        trace.push_back("tx:" + std::to_string(ev.token));
        txEvents.push_back(ev);
        if (ev.token == 0 && !reentrantSent)
        {
            // outstanding has already been decremented before invoking on_transmit.
            CHECK_EQ(a.statistics().outstanding, 2u);
            reentrantSent = true;
            CHECK(a.send(make_frame(mac(2), mac(1), 1, 99), 999) == SendStatus::Ok);
            CHECK_EQ(a.statistics().outstanding, 3u);
        }
    });
    a.on_frame([&](const TimedFrame& f) { trace.push_back("rx:" + std::to_string(f.frame.bytes()[15])); });

    member(a).start(std::nullopt);
    member(b).start(std::nullopt);
    CHECK(throws_with<std::logic_error>([&] { a.on_transmit([](const TransmitEvent&) {}); },
                                        "on_transmit() is only allowed before the host starts"));

    // Step 1ms: A sends identical frames with distinct tokens including 0; B also sends to A.
    const Frame identical = make_frame(mac(2), mac(1), 1, 7, 64);
    run_step(a, at(1ms), [&] {
        CHECK(a.send(identical, 0) == SendStatus::Ok);
        CHECK(a.send(identical, 100) == SendStatus::Ok);
        CHECK(a.send(identical, 200) == SendStatus::Ok);
        CHECK_EQ(a.statistics().outstanding, 3u);
        CHECK(txEvents.empty()); // never fires in send()
    });
    CHECK(txEvents.empty()); // never fires in flush()
    CHECK_EQ(a.statistics().outstanding, 3u);
    CHECK_EQ(a.statistics().transmit_completed, 0u);

    run_step(b, at(1ms), [&] { CHECK(b.send(make_frame(mac(1), mac(2), 2, 55)) == SendStatus::Ok); });

    // Step 2ms: A sees all 3 on_transmit events BEFORE on_frame(55), and token 0 reentrantly sends 999.
    run_step(a, at(2ms), [] {});
    const std::vector<std::string> expectedTrace = {"tx:0", "tx:100", "tx:200", "rx:55"};
    CHECK(trace == expectedTrace);
    CHECK_EQ(txEvents.size(), 3u);
    CHECK((txEvents[0] == TransmitEvent{TransmitEvent::Kind::Completed, 0, at(1ms), at(1ms)}));
    CHECK((txEvents[1] == TransmitEvent{TransmitEvent::Kind::Completed, 100, at(1ms), at(1ms)}));
    CHECK((txEvents[2] == TransmitEvent{TransmitEvent::Kind::Completed, 200, at(1ms), at(1ms)}));
    CHECK_EQ(a.statistics().transmit_completed, 3u);
    CHECK_EQ(a.statistics().outstanding, 1u); // token 999 still awaiting step 3ms

    // Step 3ms: reentrant send 999 completes; outstanding returns to 0.
    run_step(a, at(3ms), [] {});
    CHECK_EQ(txEvents.size(), 4u);
    CHECK((txEvents[3] == TransmitEvent{TransmitEvent::Kind::Completed, 999, at(2ms), at(2ms)}));
    CHECK_EQ(a.statistics().transmit_completed, 4u);
    CHECK_EQ(a.statistics().outstanding, 0u);
}

TEST_CASE(channel_sync_rejections_do_not_emit_events_and_retry_with_same_token_works)
{
    VirtualEthernetBus bus;
    ChannelOptions options;
    options.send_capacity = 1;
    options.max_frame_bytes = 64;
    Channel ch(bus.attach(), options);
    std::vector<TransmitEvent> txEvents;
    ch.on_transmit([&](const TransmitEvent& ev) { txEvents.push_back(ev); });
    member(ch).start(std::nullopt);

    const Frame valid = make_frame(kBroadcast, mac(1), 1, 0, 64);
    const Frame tooLong = make_frame(kBroadcast, mac(1), 1, 1, 65);
    run_step(ch, at(1ms), [&] {
        CHECK(ch.send(tooLong, 77) == SendStatus::InvalidFrame);
        CHECK_EQ(ch.statistics().outstanding, 0u);
        CHECK(ch.send(valid, 11) == SendStatus::Ok);
        CHECK_EQ(ch.statistics().outstanding, 1u);
        CHECK(ch.send(valid, 22) == SendStatus::QueueFull);
        CHECK_EQ(ch.statistics().outstanding, 1u);
    });
    // Step 2ms: only token 11 completes; retry token 22 cleanly.
    run_step(ch, at(2ms), [&] {
        CHECK_EQ(txEvents.size(), 1u);
        CHECK((txEvents[0] == TransmitEvent{TransmitEvent::Kind::Completed, 11, at(1ms), at(1ms)}));
        CHECK(ch.send(valid, 22) == SendStatus::Ok);
    });
    // Step 3ms: token 22 completes exactly once.
    run_step(ch, at(3ms), [] {});
    CHECK_EQ(txEvents.size(), 2u);
    CHECK((txEvents[1] == TransmitEvent{TransmitEvent::Kind::Completed, 22, at(2ms), at(2ms)}));
    CHECK_EQ(ch.statistics().sends_rejected, 2u);
    CHECK_EQ(ch.statistics().transmit_completed, 2u);
    CHECK_EQ(ch.statistics().transmit_failed, 0u);
    CHECK_EQ(ch.statistics().outstanding, 0u);
}

TEST_CASE(channel_flush_rejections_merge_chronologically_and_conserve_outstanding)
{
    // 1. Same-step send-order merge on VirtualEthernetBus with transmit_queue_capacity = 2.
    {
        VirtualEthernetBus bus;
        auto& ep = bus.attach(VirtualEthernetEndpointOptions{.transmit_queue_capacity = 2});
        Channel ch(ep);
        std::vector<TransmitEvent> txEvents;
        ch.on_transmit([&](const TransmitEvent& ev) { txEvents.push_back(ev); });
        member(ch).start(std::nullopt);

        const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
        run_step(ch, at(1ms), [&] {
            for (std::uint64_t tok = 0; tok < 4; ++tok)
            {
                CHECK(ch.send(frame, tok) == SendStatus::Ok);
            }
        });
        // Flush rejected tokens 2 and 3; none delivered yet.
        CHECK(txEvents.empty());
        CHECK_EQ(ch.statistics().frames_sent, 2u);
        CHECK_EQ(ch.statistics().frames_dropped, 2u);
        CHECK_EQ(ch.statistics().outstanding, 4u);

        run_step(ch, at(2ms), [] {});
        CHECK_EQ(txEvents.size(), 4u);
        CHECK((txEvents[0] == TransmitEvent{TransmitEvent::Kind::Completed, 0, at(1ms), at(1ms)}));
        CHECK((txEvents[1] == TransmitEvent{TransmitEvent::Kind::Completed, 1, at(1ms), at(1ms)}));
        CHECK((txEvents[2] == TransmitEvent{TransmitEvent::Kind::Failed, 2, at(1ms), at(1ms)}));
        CHECK((txEvents[3] == TransmitEvent{TransmitEvent::Kind::Failed, 3, at(1ms), at(1ms)}));
        CHECK_EQ(ch.statistics().transmit_completed, 2u);
        CHECK_EQ(ch.statistics().transmit_failed, 2u);
        CHECK_EQ(ch.statistics().outstanding, 0u);
    }

    // 2. Chronological merge when port completion has time > flush failure time.
    {
        class DelayedCorrelatedPort final : public Port
        {
        public:
            std::deque<SendResult> results;
            std::deque<PortEvent> events;
            [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
            [[nodiscard]] TransmitCapabilities transmit_capabilities() const noexcept override
            {
                return TransmitCapabilities::Correlated;
            }
            std::size_t release_before(SimTime, FrameSink) override { return 0; }
            SendResult send(const TimedFrame& frame) override { return send(frame, 0); }
            SendResult send(const TimedFrame& frame, std::uint64_t token) override
            {
                SendResult r = SendResult::Accepted;
                if (!results.empty())
                {
                    r = results.front();
                    results.pop_front();
                }
                if (r == SendResult::Accepted)
                {
                    events.push_back(PortEvent{PortEvent::Kind::TransmitCompleted, frame.time + 500us, 0, frame.time,
                                               token, true});
                }
                return r;
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

        DelayedCorrelatedPort port;
        port.results = {SendResult::Accepted, SendResult::Failed};
        Channel ch(port);
        std::vector<TransmitEvent> txEvents;
        ch.on_transmit([&](const TransmitEvent& ev) { txEvents.push_back(ev); });
        member(ch).start(std::nullopt);

        const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
        run_step(ch, at(1ms), [&] {
            CHECK(ch.send(frame, 10) == SendStatus::Ok);
            CHECK(ch.send(frame, 20) == SendStatus::Ok);
        });
        run_step(ch, at(2ms), [] {});
        CHECK_EQ(txEvents.size(), 2u);
        // Failed at 1ms precedes Completed at 1.5ms (strict monotonic TransmitEvent::time).
        CHECK((txEvents[0] == TransmitEvent{TransmitEvent::Kind::Failed, 20, at(1ms), at(1ms)}));
        CHECK((txEvents[1] == TransmitEvent{TransmitEvent::Kind::Completed, 10, at(1ms), at(1ms) + 500us}));
        CHECK_EQ(ch.statistics().outstanding, 0u);
    }
}

TEST_CASE(channel_conserves_outstanding_on_overflow_without_handler_and_on_uncorrelated_port)
{
    // 1. Port EventOverflow + Channel transmit_event_capacity overflow + no on_transmit handler.
    {
        VirtualEthernetBus bus;
        auto& ep = bus.attach(VirtualEthernetEndpointOptions{.event_capacity = 3, .transmit_queue_capacity = 0});
        ChannelOptions options;
        options.transmit_event_capacity = 2;
        Channel ch(ep, options); // no on_transmit handler registered
        member(ch).start(std::nullopt);

        const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
        run_step(ch, at(1ms), [&] {
            for (std::uint64_t i = 0; i < 5; ++i)
            {
                CHECK(ch.send(frame, i) == SendStatus::Ok);
            }
        });
        CHECK_EQ(ch.statistics().outstanding, 5u);
        // Next step: port kept 3 (EventOverflow count=2); channel keeps 2 (transmit_event_overflows=1).
        run_step(ch, at(2ms), [] {});
        const auto& s = ch.statistics();
        CHECK_EQ(s.transmit_completed, 2u);
        CHECK_EQ(s.transmit_event_overflows, 1u);
        CHECK_EQ(s.port_event_overflows, 2u);
        CHECK_EQ(s.outstanding, 0u);
        CHECK_EQ(s.frames_sent, s.transmit_completed + s.transmit_failed + s.transmit_event_overflows +
                                    s.port_event_overflows + s.outstanding);
    }

    // 2. FakePort (TransmitCapabilities::None): accepted sends do not grow outstanding,
    // uncorrelated PortEvents do not fabricate on_transmit, flush() rejections still notify Failed.
    {
        FakePort port;
        port.results = {SendResult::Accepted, SendResult::Failed};
        Channel ch(port);
        CHECK(ch.transmit_capabilities() == TransmitCapabilities::None);
        std::vector<TransmitEvent> txEvents;
        ch.on_transmit([&](const TransmitEvent& ev) { txEvents.push_back(ev); });
        member(ch).start(std::nullopt);

        const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
        run_step(ch, at(1ms), [&] {
            CHECK(ch.send(frame, 1) == SendStatus::Ok);
            CHECK(ch.send(frame, 2) == SendStatus::Ok);
            CHECK_EQ(ch.statistics().outstanding, 0u); // None port: not incremented on send()
        });
        CHECK_EQ(ch.statistics().frames_sent, 1u);
        CHECK_EQ(ch.statistics().frames_dropped, 1u);
        CHECK_EQ(ch.statistics().outstanding, 1u); // only the flush-rejected token 2

        // Inject an uncorrelated PortEvent: updates legacy counter without fabricating on_transmit.
        port.events.push_back(PortEvent{PortEvent::Kind::TransmitCompleted, at(1ms), 0});
        run_step(ch, at(2ms), [] {});
        CHECK_EQ(ch.statistics().transmit_completed, 1u);
        CHECK_EQ(ch.statistics().transmit_failed, 1u);
        CHECK_EQ(ch.statistics().outstanding, 0u);
        CHECK_EQ(txEvents.size(), 1u);
        CHECK((txEvents[0] == TransmitEvent{TransmitEvent::Kind::Failed, 2, at(1ms), at(1ms)}));
    }

    // 3. Correlated port receiving uncorrelated/duplicate acks: must NOT increment
    // transmit_completed / transmit_failed or fabricate on_transmit, preserving the
    // outstanding conservation identity strictly.
    {
        class MixedCorrelatedPort final : public Port
        {
        public:
            std::deque<PortEvent> events;
            [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
            [[nodiscard]] TransmitCapabilities transmit_capabilities() const noexcept override
            {
                return TransmitCapabilities::Correlated;
            }
            std::size_t release_before(SimTime, FrameSink) override { return 0; }
            SendResult send(const TimedFrame&) override { return SendResult::Accepted; }
            SendResult send(const TimedFrame&, std::uint64_t) override { return SendResult::Accepted; }
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

        MixedCorrelatedPort port;
        Channel ch(port);
        std::vector<TransmitEvent> txEvents;
        ch.on_transmit([&](const TransmitEvent& ev) { txEvents.push_back(ev); });
        member(ch).start(std::nullopt);

        const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
        run_step(ch, at(1ms), [&] {
            CHECK(ch.send(frame, 42) == SendStatus::Ok);
            CHECK(ch.send(frame, 43) == SendStatus::Ok);
        });
        // Simulator omits userContext for token 42 (uncorrelated Completed + Failed) while
        // correlating token 43 normally.
        port.events.push_back(PortEvent{PortEvent::Kind::TransmitCompleted, at(1ms), 0, std::nullopt, 0, false});
        port.events.push_back(PortEvent{PortEvent::Kind::TransmitFailed, at(1ms), 0, std::nullopt, 0, false});
        port.events.push_back(PortEvent{PortEvent::Kind::TransmitCompleted, at(1ms), 0, at(1ms), 43, true});

        run_step(ch, at(2ms), [] {});
        const auto& s = ch.statistics();
        CHECK_EQ(txEvents.size(), 1u);
        CHECK((txEvents[0] == TransmitEvent{TransmitEvent::Kind::Completed, 43, at(1ms), at(1ms)}));
        CHECK_EQ(s.transmit_completed, 1u);
        CHECK_EQ(s.transmit_failed, 0u);
        CHECK_EQ(s.outstanding, 1u); // token 42 remains observably outstanding
        CHECK_EQ(s.frames_sent, s.transmit_completed + s.transmit_failed + s.transmit_event_overflows +
                                    s.port_event_overflows + s.outstanding);
    }
}

TEST_CASE(channel_equal_now_flush_rejections_cap_buffer_and_conserve_outstanding)
{
    // A correlated port that always returns Failed so two flush() calls at equal now try to
    // queue > send_capacity flush_failed_ entries.
    class AlwaysFailCorrelatedPort final : public Port
    {
    public:
        [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::Virtual; }
        [[nodiscard]] TransmitCapabilities transmit_capabilities() const noexcept override
        {
            return TransmitCapabilities::Correlated;
        }
        std::size_t release_before(SimTime, FrameSink) override { return 0; }
        SendResult send(const TimedFrame&) override { return SendResult::Failed; }
        SendResult send(const TimedFrame&, std::uint64_t) override { return SendResult::Failed; }
    };

    AlwaysFailCorrelatedPort port;
    ChannelOptions options;
    options.send_capacity = 2;
    options.transmit_event_capacity = 1; // intentionally smaller than send_capacity (m3 check)
    Channel ch(port, options);
    std::vector<TransmitEvent> txEvents;
    ch.on_transmit([&](const TransmitEvent& ev) { txEvents.push_back(ev); });
    member(ch).start(std::nullopt);

    const Frame frame = make_frame(kBroadcast, mac(1), 1, 0, 64);
    // Two steps at equal timestamp 1ms: first queues 2 flush_failed_ (full), second overflows 2.
    run_step(ch, at(1ms), [&] {
        CHECK(ch.send(frame, 10) == SendStatus::Ok);
        CHECK(ch.send(frame, 11) == SendStatus::Ok);
    });
    run_step(ch, at(1ms), [&] {
        CHECK(ch.send(frame, 12) == SendStatus::Ok);
        CHECK(ch.send(frame, 13) == SendStatus::Ok);
    });
    CHECK(txEvents.empty()); // none delivered at equal timestamp 1ms
    CHECK_EQ(ch.statistics().frames_dropped, 4u);
    CHECK_EQ(ch.statistics().transmit_event_overflows, 2u);
    CHECK_EQ(ch.statistics().outstanding, 2u);

    // Step at 2ms delivers the 2 retained flush_failed_ entries (even though transmit_event_capacity == 1).
    run_step(ch, at(2ms), [] {});
    CHECK_EQ(txEvents.size(), 2u);
    CHECK((txEvents[0] == TransmitEvent{TransmitEvent::Kind::Failed, 10, at(1ms), at(1ms)}));
    CHECK((txEvents[1] == TransmitEvent{TransmitEvent::Kind::Failed, 11, at(1ms), at(1ms)}));
    const auto& s = ch.statistics();
    CHECK_EQ(s.transmit_failed, 2u);
    CHECK_EQ(s.transmit_event_overflows, 2u);
    CHECK_EQ(s.outstanding, 0u);
    CHECK_EQ(s.frames_sent + s.frames_dropped,
             s.transmit_completed + s.transmit_failed + s.transmit_event_overflows + s.port_event_overflows +
                 s.outstanding);
}
