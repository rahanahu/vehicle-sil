// SPDX-License-Identifier: MIT
//
// RawChannel: raw frames of an application on one port, received and sent inside the host's
// steps, with a fixed capacity per step and no retry. Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::Frame;
using vehicle_sil::can::Host;
using vehicle_sil::can::IdFormat;
using vehicle_sil::can::RawChannel;
using vehicle_sil::can::RawChannelOptions;
using vehicle_sil::can::RawSendStatus;
using vehicle_sil::can::StepDriver;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::can::VirtualCanBusOptions;
using vehicle_sil::can::VirtualCanBusTiming;
using vehicle_sil::unit::throws;

namespace {

SimTime at(std::chrono::nanoseconds t) { return SimTime{} + t; }
long long ms(SimTime t) { return (t - SimTime{}) / 1ms; }

Frame standard(std::uint32_t id, std::uint8_t value)
{
    Frame f;
    f.id = id;
    f.format = IdFormat::Standard;
    f.length = 1;
    f.data[0] = value;
    return f;
}

using Record = std::tuple<long long, std::uint32_t, int>; ///< (time, id, first byte)

} // namespace

// Frames sent in a step leave in its flush with the step's time, in send order; the receiver's
// handlers get them in its first step after that time, each frame to every handler in the order
// added. A handler may send; the channel does not receive its own frames.
TEST_CASE(raw_channel_sends_and_receives_inside_the_steps)
{
    VirtualCanBus bus;
    Host a;
    Host b;
    RawChannel ra;
    RawChannel rb;
    a.add(ra, bus.attach());
    b.add(rb, bus.attach());

    std::vector<Record> seenByA;
    std::vector<Record> seenByB;
    std::vector<int> handlerOrder;
    ra.on_frame([&](const TimedFrame& f) { seenByA.emplace_back(ms(f.time), f.frame.id, f.frame.data[0]); });
    rb.on_frame([&](const TimedFrame& f) {
        seenByB.emplace_back(ms(f.time), f.frame.id, f.frame.data[0]);
        handlerOrder.push_back(1);
        if (f.frame.id == 0x100)
        {
            CHECK(rb.send(standard(0x101, static_cast<std::uint8_t>(f.frame.data[0] + 1))) == RawSendStatus::Ok);
        }
    });
    rb.on_frame([&](const TimedFrame&) { handlerOrder.push_back(2); });
    a.set_application([&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(ra.send(standard(0x100, 7)) == RawSendStatus::Ok);
            CHECK(ra.send(standard(0x050, 9)) == RawSendStatus::Ok);
        }
    });

    StepDriver driver;
    driver.add(a, 10ms);
    driver.add(b, 5ms);
    driver.run_until(at(40ms));

    // B handles both in its step at 5 ms (time 0, send order, not by identifier) and answers
    // 0x100 in that step; A gets the answer in its step at 10 ms.
    CHECK((seenByB == std::vector<Record>{{0, 0x100, 7}, {0, 0x050, 9}}));
    CHECK((handlerOrder == std::vector<int>{1, 2, 1, 2}));
    CHECK((seenByA == std::vector<Record>{{5, 0x101, 8}}));
    CHECK_EQ(ra.statistics().frames_sent, 2u);
    CHECK_EQ(ra.statistics().frames_received, 1u);
    CHECK_EQ(rb.statistics().frames_sent, 1u);
    CHECK_EQ(rb.statistics().frames_received, 2u);
    CHECK_EQ(rb.statistics().sends_rejected, 0u);
}

// send() takes send_capacity frames per step; beyond that (and for an invalid frame) it takes
// nothing, says so and counts it. The capacity is per step.
TEST_CASE(raw_channel_reports_and_counts_a_full_step)
{
    VirtualCanBus bus;
    Host a;
    Host b;
    RawChannel ra(RawChannelOptions{.send_capacity = 2});
    RawChannel rb;
    a.add(ra, bus.attach());
    b.add(rb, bus.attach());
    std::vector<RawSendStatus> results;
    a.set_application([&](SimTime now) {
        if (now < at(20ms))
        {
            results.push_back(ra.send(standard(0x10, 1)));
            results.push_back(ra.send(standard(0x11, 2)));
            results.push_back(ra.send(standard(0x12, 3)));
        }
        if (now == at(20ms))
        {
            Frame bad = standard(0x800, 0); // does not fit 11 bits
            results.push_back(ra.send(bad));
        }
    });
    std::vector<Record> seen;
    rb.on_frame([&](const TimedFrame& f) { seen.emplace_back(ms(f.time), f.frame.id, f.frame.data[0]); });

    StepDriver driver;
    driver.add(a, 10ms);
    driver.add(b, 10ms);
    driver.run_until(at(40ms));

    CHECK((results == std::vector<RawSendStatus>{RawSendStatus::Ok, RawSendStatus::Ok, RawSendStatus::QueueFull,
                                                 RawSendStatus::Ok, RawSendStatus::Ok, RawSendStatus::QueueFull,
                                                 RawSendStatus::InvalidFrame}));
    CHECK((seen == std::vector<Record>{{0, 0x10, 1}, {0, 0x11, 2}, {10, 0x10, 1}, {10, 0x11, 2}}));
    CHECK_EQ(ra.statistics().frames_sent, 4u);
    CHECK_EQ(ra.statistics().sends_rejected, 3u);
    CHECK_EQ(ra.statistics().frames_dropped, 0u);
}

// What the port does not take at the flush is dropped and counted, not retried: a full port
// queue drops that frame and the rest of the step; a failed frame only itself.
TEST_CASE(raw_channel_drops_what_the_port_does_not_take)
{
    {
        VirtualCanBusOptions options;
        options.timing = VirtualCanBusTiming{.transmit_queue_capacity = 1};
        VirtualCanBus bus(options);
        Host a;
        Host b;
        RawChannel ra;
        RawChannel rb;
        a.add(ra, bus.attach());
        b.add(rb, bus.attach());
        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                for (std::uint8_t i = 0; i < 3; ++i)
                {
                    CHECK(ra.send(standard(0x20, i)) == RawSendStatus::Ok);
                }
            }
        });
        std::vector<Record> seen;
        rb.on_frame([&](const TimedFrame& f) { seen.emplace_back(0, f.frame.id, f.frame.data[0]); });
        StepDriver driver;
        driver.add(a, 10ms);
        driver.add(b, 10ms);
        driver.run_until(at(50ms));
        CHECK((seen == std::vector<Record>{{0, 0x20, 0}}));
        CHECK_EQ(ra.statistics().frames_sent, 1u);
        CHECK_EQ(ra.statistics().frames_dropped, 2u);
    }
    {
        VirtualCanBus bus;
        Host a;
        Host b;
        RawChannel ra;
        RawChannel rb;
        auto& endpoint = bus.attach();
        int sends = 0;
        endpoint.set_fault_injector([&sends](const TimedFrame&) {
            return sends++ == 1 ? FaultAction::failed() : FaultAction::deliver();
        });
        a.add(ra, endpoint);
        b.add(rb, bus.attach());
        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                for (std::uint8_t i = 0; i < 3; ++i)
                {
                    CHECK(ra.send(standard(0x20, i)) == RawSendStatus::Ok);
                }
            }
        });
        std::vector<Record> seen;
        rb.on_frame([&](const TimedFrame& f) { seen.emplace_back(0, f.frame.id, f.frame.data[0]); });
        StepDriver driver;
        driver.add(a, 10ms);
        driver.add(b, 10ms);
        driver.run_until(at(20ms));
        CHECK((seen == std::vector<Record>{{0, 0x20, 0}, {0, 0x20, 2}}));
        CHECK_EQ(ra.statistics().frames_sent, 2u);
        CHECK_EQ(ra.statistics().frames_dropped, 1u);
    }
}

// More frames in a step than receive_capacity: the rest are dropped and counted.
TEST_CASE(raw_channel_counts_a_full_receive_buffer)
{
    VirtualCanBus bus;
    Host a;
    Host b;
    RawChannel ra;
    RawChannel rb(RawChannelOptions{.receive_capacity = 2});
    a.add(ra, bus.attach());
    b.add(rb, bus.attach());
    a.set_application([&](SimTime now) {
        if (now == at(0ms))
        {
            for (std::uint8_t i = 0; i < 3; ++i)
            {
                static_cast<void>(ra.send(standard(0x30, i)));
            }
        }
    });
    std::vector<int> seen;
    rb.on_frame([&](const TimedFrame& f) { seen.push_back(f.frame.data[0]); });
    StepDriver driver;
    driver.add(a, 10ms);
    driver.add(b, 10ms);
    driver.run_until(at(30ms));
    CHECK((seen == std::vector<int>{0, 1}));
    CHECK_EQ(rb.statistics().frames_received, 2u);
    CHECK_EQ(rb.statistics().receive_overflows, 1u);
}

// The rules: capacities > 0, a handler, handlers only before the host starts, send() only inside
// a step.
TEST_CASE(raw_channel_rules)
{
    CHECK(throws<std::invalid_argument>([] { RawChannel r(RawChannelOptions{.send_capacity = 0}); }));
    CHECK(throws<std::invalid_argument>([] { RawChannel r(RawChannelOptions{.receive_capacity = 0}); }));

    VirtualCanBus bus;
    Host host;
    RawChannel raw;
    host.add(raw, bus.attach());
    CHECK(throws<std::invalid_argument>([&] { raw.on_frame({}); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(raw.send(standard(0x1, 0))); }));
    bool sentFromStep = false;
    host.set_application([&](SimTime) {
        sentFromStep = raw.send(standard(0x1, 0)) == RawSendStatus::Ok;
        CHECK(throws<std::logic_error>([&] { raw.on_frame([](const TimedFrame&) {}); }));
    });
    StepDriver driver;
    driver.add(host, 10ms);
    driver.run_until(at(10ms));
    CHECK(sentFromStep);
    CHECK(throws<std::logic_error>([&] { raw.on_frame([](const TimedFrame&) {}); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(raw.send(standard(0x1, 0))); }));
    CHECK_EQ(raw.statistics().sends_rejected, 0u);
}

// Same-ID, same-timestamp, same-payload burst with distinct tokens (including token == 0):
// transmit_capabilities() is available at setup time before start(), on_transmit() never runs
// in send()/flush(), and runs strictly before on_frame() in T' > T.
TEST_CASE(raw_channel_correlates_same_id_burst_including_zero_token_and_orders_before_on_frame)
{
    for (const bool withTiming : {false, true})
    {
        VirtualCanBusOptions busOptions;
        if (withTiming)
        {
            busOptions.timing = VirtualCanBusTiming{.bitrate = 500'000};
        }
        VirtualCanBus bus(busOptions);
        Host a;
        Host b;
        RawChannel ra;
        RawChannel rb;
        CHECK(ra.transmit_capabilities() == vehicle_sil::can::TransmitCapabilities::None); // unbound
        a.add(ra, bus.attach());
        b.add(rb, bus.attach());
        // Available immediately at setup time before start().
        CHECK(ra.transmit_capabilities() == vehicle_sil::can::TransmitCapabilities::Correlated);
        CHECK(throws<std::invalid_argument>([&] { ra.on_transmit({}); }));

        std::vector<vehicle_sil::can::TransmitEvent> txEvents;
        std::vector<std::string> stepLog;
        bool inSendOrFlush = false;
        bool firedDuringSend = false;

        ra.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) {
            if (inSendOrFlush)
            {
                firedDuringSend = true;
            }
            txEvents.push_back(ev);
            stepLog.push_back("tx:" + std::to_string(ev.token));
        });
        ra.on_frame([&](const TimedFrame& f) { stepLog.push_back("rx:" + std::to_string(f.frame.id)); });

        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                inSendOrFlush = true;
                // Identical CAN ID, identical payload, identical step timestamp, distinct tokens including 0.
                const Frame identical = standard(0x123, 0xAB);
                CHECK(ra.send(identical, 0) == RawSendStatus::Ok);
                CHECK(ra.send(identical, 100) == RawSendStatus::Ok);
                CHECK(ra.send(identical, 0xFFFF'FFFF'FFFF'FFFFull) == RawSendStatus::Ok);
                CHECK_EQ(ra.statistics().outstanding, 3u);
                inSendOrFlush = false;
            }
        });
        b.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(rb.send(standard(0x200, 1)) == RawSendStatus::Ok);
            }
        });

        StepDriver driver;
        driver.add(a, 10ms);
        driver.add(b, 10ms);
        driver.run_until(at(20ms));

        CHECK(!firedDuringSend);
        CHECK_EQ(txEvents.size(), 3u);
        CHECK_EQ(txEvents[0].token, 0u);
        CHECK_EQ(txEvents[1].token, 100u);
        CHECK_EQ(txEvents[2].token, 0xFFFF'FFFF'FFFF'FFFFull);
        for (const auto& ev : txEvents)
        {
            CHECK(ev.kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
            CHECK_EQ(ev.id, 0x123u);
            CHECK(ev.send_time == at(0ms));
            CHECK(withTiming ? (ev.time > at(0ms)) : (ev.time == at(0ms)));
        }
        // All on_transmit events ran before on_frame in the 10ms step.
        CHECK((stepLog == std::vector<std::string>{"tx:0", "tx:100", "tx:18446744073709551615", "rx:512"}));
        CHECK_EQ(ra.statistics().frames_sent, 3u);
        CHECK_EQ(ra.statistics().transmit_completed, 3u);
        CHECK_EQ(ra.statistics().transmit_failed, 0u);
        CHECK_EQ(ra.statistics().outstanding, 0u);
        CHECK(throws<std::logic_error>([&] { ra.on_transmit([](const vehicle_sil::can::TransmitEvent&) {}); }));
    }
}

// Synchronous QueueFull / InvalidFrame are unaccepted (emit no event, do not change outstanding),
// so retrying with the same token later produces exactly one send and one completion.
// Conversely, flush() rejections (QueueFull and Failed) deliver Failed in T' > T with proper merge order.
TEST_CASE(raw_channel_sync_retry_and_flush_failure_merge_ordering)
{
    // Part 1: Sync QueueFull retry with same token (including token == 0).
    {
        VirtualCanBus bus;
        Host a;
        Host b;
        RawChannel ra(RawChannelOptions{.send_capacity = 1});
        RawChannel rb;
        a.add(ra, bus.attach());
        b.add(rb, bus.attach());

        std::vector<vehicle_sil::can::TransmitEvent> txEvents;
        std::vector<std::uint8_t> received;
        ra.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) { txEvents.push_back(ev); });
        rb.on_frame([&](const TimedFrame& f) { received.push_back(f.frame.data[0]); });

        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(ra.send(standard(0x10, 1), 1) == RawSendStatus::Ok);
                CHECK_EQ(ra.statistics().outstanding, 1u);
                // Rejected synchronously: token 0 must NOT emit an event or increment outstanding.
                CHECK(ra.send(standard(0x10, 2), 0) == RawSendStatus::QueueFull);
                Frame bad = standard(0x800, 3);
                CHECK(ra.send(bad, 999) == RawSendStatus::InvalidFrame);
                CHECK_EQ(ra.statistics().outstanding, 1u);
            }
            else if (now == at(10ms))
            {
                // Retry with token 0 succeeds cleanly with no double completion.
                CHECK(ra.send(standard(0x10, 2), 0) == RawSendStatus::Ok);
            }
        });

        StepDriver driver;
        driver.add(a, 10ms);
        driver.add(b, 10ms);
        driver.run_until(at(30ms));

        CHECK((received == std::vector<std::uint8_t>{1, 2}));
        CHECK_EQ(txEvents.size(), 2u);
        CHECK_EQ(txEvents[0].token, 1u);
        CHECK(txEvents[0].send_time == at(0ms));
        CHECK_EQ(txEvents[1].token, 0u);
        CHECK(txEvents[1].send_time == at(10ms));
        CHECK_EQ(ra.statistics().outstanding, 0u);
        CHECK_EQ(ra.statistics().sends_rejected, 2u);
    }

    // Part 2: Timing-less QueueFull in flush() -> Accepted(T) before Failed(T), preserving send order.
    {
        VirtualCanBus bus;
        auto& portA = bus.attach(vehicle_sil::can::VirtualCanBusEndpointOptions{.transmit_queue_capacity = 1});
        Host a;
        RawChannel ra(RawChannelOptions{.send_capacity = 4});
        a.add(ra, portA);

        std::vector<vehicle_sil::can::TransmitEvent> txEvents;
        ra.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) { txEvents.push_back(ev); });

        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(ra.send(standard(0x20, 0), 10) == RawSendStatus::Ok);
                CHECK(ra.send(standard(0x20, 1), 11) == RawSendStatus::Ok);
                CHECK(ra.send(standard(0x20, 2), 12) == RawSendStatus::Ok);
            }
        });

        StepDriver driver;
        driver.add(a, 10ms);
        driver.run_until(at(20ms));

        CHECK_EQ(txEvents.size(), 3u);
        CHECK(txEvents[0].kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
        CHECK_EQ(txEvents[0].token, 10u);
        CHECK(txEvents[1].kind == vehicle_sil::can::TransmitEvent::Kind::Failed);
        CHECK_EQ(txEvents[1].token, 11u);
        CHECK(txEvents[2].kind == vehicle_sil::can::TransmitEvent::Kind::Failed);
        CHECK_EQ(txEvents[2].token, 12u);
        for (const auto& ev : txEvents)
        {
            CHECK(ev.send_time == at(0ms));
            CHECK(ev.time == at(0ms));
        }
        CHECK_EQ(ra.statistics().frames_sent, 1u);
        CHECK_EQ(ra.statistics().frames_dropped, 2u);
        CHECK_EQ(ra.statistics().transmit_completed, 1u);
        CHECK_EQ(ra.statistics().transmit_failed, 2u);
        CHECK_EQ(ra.statistics().outstanding, 0u);
    }

    // Part 3: Timed bus fault Failed in flush() -> Failed(time == T) precedes Completed(time == tx.end > T).
    {
        VirtualCanBusOptions busOptions;
        busOptions.timing = VirtualCanBusTiming{.bitrate = 500'000};
        VirtualCanBus bus(busOptions);
        auto& portA = bus.attach();
        int idx = 0;
        portA.set_fault_injector([&idx](const TimedFrame&) {
            return idx++ == 1 ? FaultAction::failed() : FaultAction::deliver();
        });
        Host a;
        RawChannel ra;
        a.add(ra, portA);

        std::vector<vehicle_sil::can::TransmitEvent> txEvents;
        ra.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) { txEvents.push_back(ev); });
        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(ra.send(standard(0x30, 0), 100) == RawSendStatus::Ok);
                CHECK(ra.send(standard(0x30, 1), 101) == RawSendStatus::Ok); // fails in flush() at t=0
                CHECK(ra.send(standard(0x30, 2), 102) == RawSendStatus::Ok);
            }
        });

        StepDriver driver;
        driver.add(a, 10ms);
        driver.run_until(at(20ms));

        CHECK_EQ(txEvents.size(), 3u);
        CHECK(txEvents[0].kind == vehicle_sil::can::TransmitEvent::Kind::Failed);
        CHECK_EQ(txEvents[0].token, 101u);
        CHECK(txEvents[0].time == at(0ms));
        CHECK(txEvents[1].kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
        CHECK_EQ(txEvents[1].token, 100u);
        CHECK(txEvents[1].time > at(0ms));
        CHECK(txEvents[2].kind == vehicle_sil::can::TransmitEvent::Kind::Completed);
        CHECK_EQ(txEvents[2].token, 102u);
        CHECK(txEvents[2].time > txEvents[1].time);
        CHECK_EQ(ra.statistics().outstanding, 0u);
    }
}

// Reentrant send() inside on_transmit(), draining without handlers, EventOverflow &
// transmit_event_capacity overflow conservation, and TransmitCapabilities::None backend behavior.
TEST_CASE(raw_channel_reentrancy_overflow_conservation_and_none_backend)
{
    // Part 1: Reentrant send() inside on_transmit() keeping pool of size 2 saturated.
    {
        VirtualCanBus bus;
        Host a;
        RawChannel ra;
        a.add(ra, bus.attach());

        std::uint64_t nextToken = 0;
        std::vector<std::uint64_t> doneTokens;
        ra.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) {
            doneTokens.push_back(ev.token);
            // outstanding was decremented BEFORE calling on_transmit().
            CHECK(ra.statistics().outstanding < 2u);
            if (nextToken < 6u)
            {
                CHECK(ra.send(standard(0x40, static_cast<std::uint8_t>(nextToken)), nextToken) == RawSendStatus::Ok);
                ++nextToken;
            }
        });
        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                while (ra.statistics().outstanding < 2u)
                {
                    CHECK(ra.send(standard(0x40, static_cast<std::uint8_t>(nextToken)), nextToken) == RawSendStatus::Ok);
                    ++nextToken;
                }
            }
        });
        StepDriver driver;
        driver.add(a, 5ms);
        driver.run_until(at(30ms));
        CHECK((doneTokens == std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5}));
        CHECK_EQ(ra.statistics().outstanding, 0u);
    }

    // Part 2: Port EventOverflow + channel transmit_event_capacity overflow preserve outstanding conservation,
    // even when no on_transmit() handler is registered.
    {
        VirtualCanBus bus;
        auto& portA = bus.attach(vehicle_sil::can::VirtualCanBusEndpointOptions{.event_capacity = 3});
        Host a;
        RawChannel ra(RawChannelOptions{.send_capacity = 8, .transmit_event_capacity = 2});
        a.add(ra, portA);
        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                for (std::uint64_t i = 0; i < 5; ++i)
                {
                    CHECK(ra.send(standard(0x50, static_cast<std::uint8_t>(i)), i) == RawSendStatus::Ok);
                }
            }
        });
        StepDriver driver;
        driver.add(a, 10ms);
        driver.run_until(at(20ms));
        const auto& s = ra.statistics();
        CHECK_EQ(s.frames_sent, 5u);
        CHECK_EQ(s.port_event_overflows, 2u);     // port kept 3 of 5
        CHECK_EQ(s.transmit_event_overflows, 1u); // channel kept 2 of 3
        CHECK_EQ(s.transmit_completed, 2u);
        CHECK_EQ(s.outstanding, 0u);
        CHECK_EQ(s.frames_sent + s.frames_dropped,
                 s.transmit_completed + s.transmit_failed + s.transmit_event_overflows + s.port_event_overflows +
                     s.outstanding);
    }

    // Part 3: TransmitCapabilities::None port never grows outstanding on Accepted, never fabricates
    // completions, yet still delivers Failed on flush() rejection.
    {
        struct LegacyPort final : vehicle_sil::can::Port
        {
            vehicle_sil::can::TimeDomain time_domain() const noexcept override
            {
                return vehicle_sil::can::TimeDomain::Virtual;
            }
            std::size_t release_before(SimTime, vehicle_sil::can::FrameSink) override { return 0; }
            vehicle_sil::can::SendResult send(const TimedFrame&) override
            {
                ++calls;
                return calls == 2 ? vehicle_sil::can::SendResult::Failed : vehicle_sil::can::SendResult::Accepted;
            }
            int calls = 0;
        } legacy;

        Host a;
        RawChannel ra;
        a.add(ra, legacy);
        CHECK(ra.transmit_capabilities() == vehicle_sil::can::TransmitCapabilities::None);

        std::vector<vehicle_sil::can::TransmitEvent> txEvents;
        ra.on_transmit([&](const vehicle_sil::can::TransmitEvent& ev) { txEvents.push_back(ev); });
        a.set_application([&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(ra.send(standard(0x60, 1), 10) == RawSendStatus::Ok);
                CHECK(ra.send(standard(0x60, 2), 20) == RawSendStatus::Ok); // fails in flush()
                CHECK_EQ(ra.statistics().outstanding, 0u);                  // None backend
            }
        });
        StepDriver driver;
        driver.add(a, 10ms);
        driver.run_until(at(20ms));

        CHECK_EQ(ra.statistics().frames_sent, 1u);
        CHECK_EQ(ra.statistics().frames_dropped, 1u);
        CHECK_EQ(ra.statistics().transmit_completed, 0u);
        CHECK_EQ(ra.statistics().transmit_failed, 1u);
        CHECK_EQ(ra.statistics().outstanding, 0u);
        CHECK_EQ(txEvents.size(), 1u);
        CHECK(txEvents[0].kind == vehicle_sil::can::TransmitEvent::Kind::Failed);
        CHECK_EQ(txEvents[0].token, 20u);
    }

    // Part 4: Options validation & derived capacities.
    {
        RawChannelOptions defaults;
        CHECK_EQ(RawChannel::port_inbox_capacity(defaults), 2048u);
        CHECK_EQ(RawChannel::port_event_capacity(defaults), 1024u);

        RawChannelOptions custom;
        custom.send_capacity = 100;
        custom.receive_capacity = 500;
        custom.port_transmit_queue_capacity = 2000;
        CHECK_EQ(RawChannel::port_inbox_capacity(custom), 1000u);
        CHECK_EQ(RawChannel::port_event_capacity(custom), 2100u);

        RawChannelOptions bad;
        bad.port_event_capacity = 16;
        bad.port_transmit_queue_capacity = 17;
        CHECK(throws<std::invalid_argument>([&] { RawChannel::validate(bad); }));
        CHECK(throws<std::invalid_argument>([&] { RawChannel ch(bad); }));
    }
}
