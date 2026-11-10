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
