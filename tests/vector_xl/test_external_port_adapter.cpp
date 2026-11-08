// SPDX-License-Identifier: MIT
//
// vehicle_sil::vector_xl::ExternalPortAdapter: an ExternalCanPort (here a fake) as a
// vehicle_sil::can::Port of real time, stepped by a RealTimeDriver.
#include "can_test_util.hpp"
#include "fake_external_port.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/vector_xl/external_port_adapter.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimTime;
using vehicle_sil::can::Frame;
using vehicle_sil::can::ManualRealTimeClock;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::TimeDomain;
using vehicle_sil::unit::FakeExternalPort;
using vehicle_sil::vector_xl::ExternalEventKind;
using vehicle_sil::vector_xl::ExternalPortAdapter;
using vehicle_sil::vector_xl::TransmitResult;

namespace {

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

Frame can_frame(std::uint32_t id, std::uint8_t value)
{
    Frame f;
    f.id = id;
    f.length = 1;
    f.data[0] = value;
    return f;
}

/// Answers every received frame with the same identifier + 1 and the same data.
class Echo final : public vehicle_sil::can::Steppable
{
public:
    std::vector<TimedFrame> received;
    std::vector<PortEvent> events;

    void begin(SimTime now) override { now_ = now; }
    void receive(const TimedFrame& f) override { received.push_back(f); pending_.push_back(f.frame); }
    void on_port_event(const PortEvent& e) override { events.push_back(e); }
    void process() override {}
    void flush(vehicle_sil::can::Port& port) override
    {
        for (auto f : pending_)
        {
            f.id += 1;
            static_cast<void>(port.send(TimedFrame{now_, f}));
        }
        pending_.clear();
    }

private:
    SimTime now_{};
    std::vector<Frame> pending_;
};

} // namespace

TEST_CASE(external_port_adapter_converts_the_events_of_the_external_port)
{
    FakeExternalPort external;
    ManualRealTimeClock clock(at(4ms));
    {
        ExternalPortAdapter adapter(external, clock);
        CHECK(external.started());
        CHECK(adapter.time_domain() == TimeDomain::RealTime);

        const std::vector<std::uint8_t> d{7};
        external.deliver(ExternalEventKind::Frame, vehicle_sil::unit::extended_frame(0x18FF0001, d));
        external.deliver(ExternalEventKind::OwnTransmission, vehicle_sil::unit::extended_frame(0x18FF0002, d));
        external.deliver(ExternalEventKind::ErrorFrame);
        external.deliver(ExternalEventKind::Overrun);
        auto remote = vehicle_sil::unit::extended_frame(0x18FF0003, {});
        remote.flags |= vehicle_sil::unit::flag(SilKit::Services::Can::CanFrameFlag::Rtr);
        external.deliver(ExternalEventKind::Frame, remote);

        std::vector<PortEvent> events;
        adapter.release_events_before(at(5ms), [&](const PortEvent& e) { events.push_back(e); });
        CHECK_EQ(events.size(), 2u);
        CHECK(events[0].kind == PortEvent::Kind::ErrorFrame);
        CHECK(events[0].time == at(4ms) + 1ns);
        CHECK(events[1].kind == PortEvent::Kind::ReceiveOverflow);
        CHECK_EQ(events[1].count, 0u); // the external port does not tell how many
        std::vector<TimedFrame> frames;
        adapter.release_before(at(5ms), [&](const TimedFrame& f) { frames.push_back(f); });
        CHECK_EQ(frames.size(), 1u);
        CHECK(frames[0].time == at(4ms));
        CHECK_EQ(frames[0].frame.id, 0x18FF0001u);
        CHECK(frames[0].frame.format == vehicle_sil::can::IdFormat::Extended);

        // Synchronous results of transmit().
        CHECK(adapter.send(TimedFrame{at(5ms), can_frame(0x100, 1)}) == SendResult::Accepted);
        external.result = TransmitResult::QueueFull;
        CHECK(adapter.send(TimedFrame{at(5ms), can_frame(0x100, 2)}) == SendResult::QueueFull);
        external.result = TransmitResult::Failed;
        CHECK(adapter.send(TimedFrame{at(5ms), can_frame(0x100, 3)}) == SendResult::Failed);
        external.result = TransmitResult::Ok;
        Frame invalid = can_frame(0x100, 4);
        invalid.length = 9;
        CHECK(adapter.send(TimedFrame{at(5ms), invalid}) == SendResult::Failed);
        CHECK_EQ(external.transmitted().size(), 1u);
        CHECK_EQ(external.transmitted()[0].canId, 0x100u);

        const auto stats = adapter.statistics();
        CHECK_EQ(stats.received, 1u);
        CHECK_EQ(stats.receive_rejected, 1u);
        CHECK_EQ(stats.own_transmissions_dropped, 1u);
        CHECK_EQ(stats.error_frames, 1u);
        CHECK_EQ(stats.external_overruns, 1u);
        CHECK_EQ(stats.sent, 1u);
        CHECK_EQ(stats.send_queue_full, 1u);
        CHECK_EQ(stats.send_failed, 2u);

        adapter.close();
        CHECK(!external.started());
        CHECK(adapter.send(TimedFrame{at(6ms), can_frame(0x100, 5)}) == SendResult::Failed);
        adapter.close(); // idempotent
        CHECK_EQ(external.stop_calls(), 1);
    }
    CHECK_EQ(external.stop_calls(), 1);

    // The external port's start failure propagates.
    external.fail_start = true;
    CHECK(vehicle_sil::unit::throws<std::runtime_error>([&] { ExternalPortAdapter adapter(external, clock); }));
}

TEST_CASE(external_port_adapter_runs_under_the_real_time_driver)
{
    FakeExternalPort external;
    ManualRealTimeClock clock;
    ExternalPortAdapter adapter(external, clock);
    Echo echo;
    vehicle_sil::can::Host host;
    host.add(echo, adapter);
    vehicle_sil::can::RealTimeDriver driver(host, clock);
    driver.start();
    CHECK(driver.wait_idle(10s));

    // Frames from the receive thread of the external port: each wakes the driver, which
    // answers in the step after the arrival.
    clock.advance_to(at(3ms));
    std::thread receiver([&] {
        for (std::uint8_t i = 0; i < 20; ++i)
        {
            external.deliver(ExternalEventKind::Frame, vehicle_sil::unit::extended_frame(0x200, {i}));
        }
    });
    receiver.join();
    CHECK(driver.wait_idle(10s));
    const auto sent = external.transmitted();
    CHECK_EQ(sent.size(), 20u);
    for (std::size_t i = 0; i < sent.size(); ++i)
    {
        CHECK_EQ(sent[i].canId, 0x201u);
        CHECK_EQ(static_cast<unsigned>(sent[i].data.at(0)), static_cast<unsigned>(i));
    }
    driver.stop();
    CHECK(!driver.failed());
    CHECK_EQ(echo.received.size(), 20u);
    for (std::size_t i = 1; i < echo.received.size(); ++i)
    {
        CHECK(echo.received[i].time > echo.received[i - 1].time);
    }
    // Shutdown order: driver stopped, then the adapter closes (the destructor) before the port.
}

TEST_CASE(external_port_adapter_closes_while_the_driver_runs)
{
    FakeExternalPort external;
    ManualRealTimeClock clock;
    ExternalPortAdapter adapter(external, clock);
    Echo echo;
    vehicle_sil::can::Host host;
    host.add(echo, adapter);
    vehicle_sil::can::RealTimeDriver driver(host, clock);
    driver.start();
    std::atomic<bool> done{false};
    std::thread receiver([&] {
        for (std::uint8_t i = 0; !done.load(); ++i)
        {
            external.deliver(ExternalEventKind::Frame, vehicle_sil::unit::extended_frame(0x300, {i}));
        }
    });
    CHECK(vehicle_sil::unit::eventually([&] { return !external.transmitted().empty(); }, 10s));
    adapter.close(); // the driver keeps stepping; the adapter releases nothing and refuses sends
    CHECK(!external.started());
    const std::size_t sent = external.transmitted().size();
    clock.advance_to(at(100ms));
    CHECK(driver.wait_idle(10s));
    done.store(true);
    receiver.join();
    driver.stop();
    CHECK(!driver.failed());
    CHECK_EQ(external.transmitted().size(), sent);
}
