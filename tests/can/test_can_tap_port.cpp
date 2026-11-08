// SPDX-License-Identifier: MIT
//
// TapPort: a read-only tap on a port that reports the raw frames and events passing through it.
// Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/tap_port.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::can::BusState;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::Frame;
using vehicle_sil::can::Host;
using vehicle_sil::can::IdFormat;
using vehicle_sil::can::Port;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::Steppable;
using vehicle_sil::can::StepDriver;
using vehicle_sil::can::TapListener;
using vehicle_sil::can::TapPort;
using vehicle_sil::can::TapRecord;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::unit::throws;

namespace {

SimTime at(std::chrono::nanoseconds t)
{
    return SimTime{} + t;
}

Frame frame(std::uint32_t id, IdFormat format = IdFormat::Extended)
{
    Frame f;
    f.id = id;
    f.format = format;
    f.length = 2;
    f.data[0] = static_cast<std::uint8_t>(id);
    f.data[1] = 0xA5;
    return f;
}

/// Records what the host hands it and sends a scripted frame list per step.
class Echo final : public Steppable
{
public:
    void begin(SimTime now) override { now_ = now; }
    void receive(const TimedFrame& f) override { seen.push_back("rx " + std::to_string(f.time.time_since_epoch().count()) + " " + std::to_string(f.frame.id)); }
    void on_port_event(const PortEvent& e) override
    {
        seen.push_back("ev " + std::to_string(e.time.time_since_epoch().count()) + " " + std::to_string(static_cast<int>(e.kind)));
    }
    void process() override {}
    void flush(Port& port) override
    {
        if (send && send(now_))
        {
            const auto result = port.send(TimedFrame{now_, frame(0x100 + static_cast<std::uint32_t>(sent_++))});
            seen.push_back("tx " + std::to_string(static_cast<int>(result)));
        }
    }
    std::function<bool(SimTime)> send;
    std::vector<std::string> seen;

private:
    SimTime now_{};
    int sent_ = 0;
};

struct Log final : TapListener
{
    void on_tap(const TapRecord& r) override
    {
        records.push_back(r);
        if (during)
        {
            during(r);
        }
    }
    std::vector<TapRecord> records;
    std::function<void(const TapRecord&)> during;
};

/// One Echo on a tapped endpoint and a peer endpoint, run for `steps` steps of 10 ms.
struct Rig
{
    explicit Rig(TapListener* listener) : tap(port, listener)
    {
        host.add(echo, tap);
        driver.add(host, 10ms);
    }
    VirtualCanBus bus;
    VirtualCanBus::Endpoint& peer = bus.attach();
    VirtualCanBus::Endpoint& port = bus.attach();
    TapPort tap;
    Echo echo;
    Host host;
    StepDriver driver;
};

std::vector<std::string> run_rig(TapListener* listener, std::vector<TapRecord>* records = nullptr)
{
    Rig rig(listener);
    rig.echo.send = [](SimTime now) { return now == at(10ms) || now == at(30ms); };
    rig.port.set_fault_injector([](const TimedFrame& f) {
        return f.frame.id == 0x101 ? FaultAction::queue_full() : FaultAction::deliver();
    });
    rig.peer.send(TimedFrame{at(5ms), frame(0x7FF, IdFormat::Standard)});
    rig.peer.send(TimedFrame{at(5ms), frame(0x18FEF100)});
    rig.port.inject_event(PortEvent{PortEvent::Kind::BusStateChanged, at(15ms), 0, std::nullopt, 0, BusState::ErrorPassive});
    rig.driver.run_until(at(50ms));
    if (records != nullptr)
    {
        auto* log = dynamic_cast<Log*>(listener);
        *records = log->records;
    }
    return rig.echo.seen;
}

} // namespace

TEST_CASE(tap_port_reports_received_sent_and_events_in_order)
{
    Log log;
    std::vector<TapRecord> records;
    run_rig(&log, &records);
    CHECK_EQ(records.size(), 5u);
    // Step at 10 ms: the two frames sent at 5 ms (11-bit first, as they arrived), then the frame sent.
    CHECK(records[0].direction == TapRecord::Direction::Received);
    CHECK(records[0].time == at(5ms));
    CHECK_EQ(records[0].frame.id, 0x7FFu);
    CHECK(records[0].frame.format == IdFormat::Standard);
    CHECK(records[1].direction == TapRecord::Direction::Received);
    CHECK_EQ(records[1].frame.id, 0x18FEF100u);
    CHECK(records[1].frame.format == IdFormat::Extended);
    CHECK(records[1].frame == frame(0x18FEF100));
    CHECK(records[2].direction == TapRecord::Direction::Sent);
    CHECK(records[2].time == at(10ms));
    CHECK_EQ(records[2].frame.id, 0x100u);
    CHECK(records[2].result == SendResult::Accepted);
    // Step at 20 ms: the injected bus state.
    CHECK(records[3].direction == TapRecord::Direction::Event);
    CHECK(records[3].time == at(15ms));
    CHECK(records[3].event.kind == PortEvent::Kind::BusStateChanged);
    CHECK(records[3].event.bus_state == BusState::ErrorPassive);
    // Step at 30 ms: the refused frame, with the result send() returned.
    CHECK(records[4].direction == TapRecord::Direction::Sent);
    CHECK_EQ(records[4].frame.id, 0x101u);
    CHECK(records[4].result == SendResult::QueueFull);
}

TEST_CASE(tap_port_does_not_change_what_the_stack_sees)
{
    Log log;
    const auto with = run_rig(&log);
    const auto without = run_rig(nullptr);
    CHECK(with == without);
    CHECK_EQ(with.size(), 5u);
}

TEST_CASE(tap_port_listener_must_not_use_the_port)
{
    for (int mode = 0; mode < 3; ++mode)
    {
        Log log;
        Rig rig(&log);
        rig.peer.send(TimedFrame{at(5ms), frame(1)});
        log.during = [&rig, mode](const TapRecord&) {
            if (mode == 0)
            {
                static_cast<void>(rig.tap.send(TimedFrame{at(10ms), frame(2)}));
            }
            else if (mode == 1)
            {
                rig.tap.release_before(at(10ms), [](const TimedFrame&) {});
            }
            else
            {
                rig.tap.set_listener(nullptr);
            }
        };
        CHECK(throws<std::logic_error>([&] { rig.driver.run_until(at(20ms)); }));
        CHECK(rig.host.failed());
    }
}

TEST_CASE(tap_port_and_listener_can_be_destroyed_in_either_order)
{
    VirtualCanBus bus;
    auto& peer = bus.attach();
    auto& port = bus.attach();
    {
        auto log = std::make_unique<Log>();
        auto tap = std::make_unique<TapPort>(port, log.get());
        CHECK(tap->listener() == log.get());
        log.reset(); // the listener goes first: the tap forgets it
        CHECK(tap->listener() == nullptr);
        peer.send(TimedFrame{at(1ms), frame(1)});
        CHECK_EQ(tap->release_before(at(2ms), [](const TimedFrame&) {}), 1u);
    }
    {
        Log log;
        auto tap = std::make_unique<TapPort>(port, &log);
        tap.reset(); // the tap goes first: the listener forgets it and can be destroyed safely
        TapPort other(port);
        other.set_listener(&log);
        peer.send(TimedFrame{at(3ms), frame(2)});
        CHECK_EQ(other.release_before(at(4ms), [](const TimedFrame&) {}), 1u);
        CHECK_EQ(log.records.size(), 1u);
    }
    {
        // A listener moved to another tap leaves the first one.
        Log log;
        TapPort first(port, &log);
        TapPort second(port);
        second.set_listener(&log);
        CHECK(first.listener() == nullptr);
        CHECK(second.listener() == &log);
        second.set_listener(nullptr);
        CHECK(second.listener() == nullptr);
    }
}

TEST_CASE(tap_port_forwards_the_time_domain_and_the_counts)
{
    VirtualCanBus bus;
    auto& peer = bus.attach();
    auto& port = bus.attach();
    Log log;
    TapPort tap(port, &log);
    CHECK(tap.time_domain() == vehicle_sil::can::TimeDomain::Virtual);
    CHECK(&tap.inner() == &port);
    peer.send(TimedFrame{at(1ms), frame(1)});
    peer.send(TimedFrame{at(1ms), frame(2)});
    std::vector<std::uint32_t> ids;
    CHECK_EQ(tap.release_before(at(2ms), [&](const TimedFrame& f) { ids.push_back(f.frame.id); }), 2u);
    CHECK((ids == std::vector<std::uint32_t>{1, 2}));
    CHECK(port.inject_event(PortEvent{PortEvent::Kind::ErrorFrame, at(2ms), 0, std::nullopt, 0, BusState::ErrorActive}));
    CHECK_EQ(tap.release_events_before(at(3ms), [](const PortEvent&) {}), 1u);
    Frame invalid = frame(1);
    invalid.length = 9;
    CHECK(tap.send(TimedFrame{at(3ms), invalid}) == SendResult::Failed);
    CHECK_EQ(log.records.size(), 4u);
    CHECK(log.records[3].result == SendResult::Failed);
}
