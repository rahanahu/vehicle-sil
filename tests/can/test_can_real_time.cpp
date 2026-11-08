// SPDX-License-Identifier: MIT
//
// vehicle_sil::can on real time: RealTimeDriver, the clocks, RealTimeReceiver, Host::post() and
// the time domain checks. Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/can/real_time_receiver.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::can::Frame;
using vehicle_sil::can::FrameSink;
using vehicle_sil::can::Host;
using vehicle_sil::can::ManualRealTimeClock;
using vehicle_sil::can::Port;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::PortEventSink;
using vehicle_sil::can::RealTimeDriver;
using vehicle_sil::can::RealTimeDriverOptions;
using vehicle_sil::can::RealTimeReceiver;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::SteadyRealTimeClock;
using vehicle_sil::can::Steppable;
using vehicle_sil::can::StepDriver;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::TimeDomain;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::can::Wakeup;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

constexpr SimDuration kNs{1};
constexpr auto kWait = 10s; // upper bound for wait_idle(); a pass never waits that long

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

Frame frame(std::uint32_t id, std::uint8_t value = 0)
{
    Frame f;
    f.id = id;
    f.length = 1;
    f.data[0] = value;
    return f;
}

/// A port of real time over a RealTimeReceiver: the test pushes what it "receives".
class TestRealTimePort final : public Port
{
public:
    TestRealTimePort(const vehicle_sil::can::RealTimeClock& clock, std::size_t capacity = 64) : receiver(clock, capacity, capacity) {}

    RealTimeReceiver receiver;
    std::vector<TimedFrame> sent; ///< driver's thread

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return TimeDomain::RealTime; }
    std::size_t release_before(SimTime now, FrameSink sink) override { return receiver.release_before(now, sink); }
    std::size_t release_events_before(SimTime now, PortEventSink sink) override
    {
        return receiver.release_events_before(now, sink);
    }
    SendResult send(const TimedFrame& f) override
    {
        sent.push_back(f);
        return SendResult::Accepted;
    }
    void set_wakeup(Wakeup wakeup) override { receiver.set_wakeup(std::move(wakeup)); }
};

/// Records the steps, frames and events; its timer deadline is set by the test.
class Probe final : public Steppable
{
public:
    std::vector<SimTime> steps;
    std::vector<TimedFrame> frames;
    std::vector<PortEvent> events;
    std::vector<std::string> log;
    std::optional<SimTime> deadline;
    std::optional<SimTime> throw_at; ///< process() throws in the step at this time or later
    bool keep_deadline = false;      ///< report the deadline also after it passed
    std::optional<SimDuration> step_size;
    std::atomic<std::size_t> frame_count{0}; ///< readable while the driver runs
    std::atomic<std::size_t> step_count{0};  ///< readable while the driver runs
    std::vector<SimTime> frame_steps;        ///< the step that received each frame

    void start(std::optional<SimDuration> size) override { step_size = size; }
    void begin(SimTime now) override
    {
        steps.push_back(now);
        step_count.fetch_add(1);
        if (deadline && now > *deadline && !keep_deadline)
        {
            deadline.reset(); // like a timeout that fired
        }
    }
    void receive(const TimedFrame& f) override
    {
        frames.push_back(f);
        frame_steps.push_back(steps.back());
        frame_count.fetch_add(1);
    }
    void on_port_event(const PortEvent& e) override { events.push_back(e); }
    void process() override
    {
        log.push_back("process");
        if (throw_at && steps.back() >= *throw_at)
        {
            throw std::runtime_error("probe failure");
        }
    }
    void flush(Port&) override {}
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept override { return deadline; }
};

struct ManualRig
{
    ManualRealTimeClock clock;
    TestRealTimePort port{clock};
    Probe probe;
    Host host;
    std::unique_ptr<RealTimeDriver> driver;

    explicit ManualRig(RealTimeDriverOptions options = {})
    {
        host.add(probe, port);
        driver = std::make_unique<RealTimeDriver>(host, clock, options);
    }

    void settle() { CHECK(driver->wait_idle(kWait)); }
    void advance_to(SimTime t)
    {
        clock.advance_to(t);
        settle();
    }
};

} // namespace

TEST_CASE(real_time_driver_steps_at_start_then_after_max_idle)
{
    ManualRig rig;
    CHECK(rig.probe.step_size == std::nullopt); // start() runs before the first step
    rig.driver->start();
    rig.settle();
    CHECK(rig.probe.step_size == std::optional<SimDuration>(10ms)); // max_idle is the step size
    CHECK((rig.probe.steps == std::vector<SimTime>{at(0ms)}));

    rig.advance_to(at(9ms));
    CHECK_EQ(rig.probe.steps.size(), 1u);
    rig.advance_to(at(10ms));
    CHECK((rig.probe.steps == std::vector<SimTime>{at(0ms), at(10ms)}));
    // A jump over several idle periods gives one step at the new time.
    rig.advance_to(at(35ms));
    CHECK((rig.probe.steps == std::vector<SimTime>{at(0ms), at(10ms), at(35ms)}));
    CHECK_EQ(rig.driver->steps(), 3u);
    CHECK(rig.driver->last_step() == std::optional<SimTime>(at(35ms)));
    rig.driver->stop();
    CHECK(!rig.driver->running());
    CHECK(!rig.driver->failed());
}

TEST_CASE(real_time_driver_wakes_just_after_the_next_deadline)
{
    ManualRig rig;
    rig.probe.deadline = at(3ms); // read after each step
    rig.driver->start();
    rig.settle();
    // A timeout fires in the first step after its deadline: the driver wakes 1 ns after it.
    rig.advance_to(at(3ms));
    CHECK_EQ(rig.probe.steps.size(), 1u);
    rig.advance_to(at(3ms) + kNs);
    CHECK((rig.probe.steps == std::vector<SimTime>{at(0ms), at(3ms) + kNs}));
    // A deadline that is not earlier than max_idle does not delay the idle step.
    rig.probe.deadline = at(50ms);
    rig.advance_to(at(13ms) + kNs);
    CHECK_EQ(rig.probe.steps.size(), 3u);
    // No deadline: max_idle.
    rig.probe.deadline.reset();
    rig.advance_to(at(23ms));
    CHECK_EQ(rig.probe.steps.size(), 3u);
    rig.advance_to(at(23ms) + kNs);
    CHECK_EQ(rig.probe.steps.size(), 4u);

    // A deadline that stays in the past (a timer started late in a step): one more step 1 ns
    // later, not a stream of steps ahead of the clock.
    rig.probe.keep_deadline = true;
    rig.probe.deadline = at(24ms); // read after the next step
    rig.advance_to(at(33ms) + kNs);
    CHECK_EQ(rig.probe.steps.size(), 5u);
    rig.advance_to(at(33ms) + 2 * kNs);
    CHECK_EQ(rig.probe.steps.size(), 6u);
    CHECK(rig.probe.steps.back() == at(33ms) + 2 * kNs);
}

TEST_CASE(real_time_driver_steps_after_an_arrival_and_releases_it)
{
    ManualRig rig;
    rig.driver->start();
    rig.settle();
    rig.advance_to(at(5ms));
    // The clock does not move: the arrival is stamped 5 ms and the driver steps 1 ns after it.
    CHECK(rig.port.receiver.push_frame(frame(0x10)));
    rig.settle();
    CHECK((rig.probe.steps == std::vector<SimTime>{at(0ms), at(5ms) + kNs}));
    CHECK_EQ(rig.probe.frames.size(), 1u);
    CHECK(rig.probe.frames[0].time == at(5ms));
    CHECK_EQ(rig.probe.frames[0].frame.id, 0x10u);

    // More arrivals, the clock still at 5 ms: strictly increasing stamps, never before the
    // released step (5 ms + 1 ns). Whether the driver steps between them or not, the stamps are
    // the same, and all are released in arrival order.
    CHECK(rig.port.receiver.push_frame(frame(0x20)));
    CHECK(rig.port.receiver.push_frame(frame(0x21)));
    CHECK(rig.port.receiver.push_frame(frame(0x22)));
    rig.settle();
    CHECK_EQ(rig.probe.frames.size(), 4u);
    CHECK(rig.probe.frames[1].time == at(5ms) + kNs);
    CHECK(rig.probe.frames[2].time == at(5ms) + 2 * kNs);
    CHECK(rig.probe.frames[3].time == at(5ms) + 3 * kNs);
    CHECK_EQ(rig.probe.frames[3].frame.id, 0x22u);
    CHECK(rig.probe.steps.back() > rig.probe.frames.back().time);
}

TEST_CASE(real_time_receiver_stamps_after_the_released_step_and_reports_overflow)
{
    ManualRealTimeClock clock(at(10ms));
    RealTimeReceiver receiver(clock, 2, 4);
    std::vector<SimTime> wakes;
    receiver.set_wakeup([&](SimTime t) { wakes.push_back(t); });

    // A step at 50 ms released everything before it: a later arrival on a clock that is
    // behind (10 ms) is stamped 50 ms, not in the past of the stack.
    std::size_t released = receiver.release_before(at(50ms), [](const TimedFrame&) {});
    CHECK_EQ(released, 0u);
    CHECK(receiver.push_frame(frame(1)));
    CHECK(receiver.push_frame(frame(2)));
    CHECK(!receiver.push_frame(frame(3))); // full
    CHECK(!receiver.push_frame(frame(4)));
    CHECK_EQ(receiver.overflows(), 2u);
    CHECK((wakes == std::vector<SimTime>{at(50ms), at(50ms) + kNs, at(50ms) + 2 * kNs, at(50ms) + 3 * kNs}));

    std::vector<TimedFrame> frames;
    receiver.release_before(at(60ms), [&](const TimedFrame& f) { frames.push_back(f); });
    CHECK_EQ(frames.size(), 2u);
    CHECK(frames[0].time == at(50ms));
    CHECK(frames[1].time == at(50ms) + kNs);

    // The two dropped frames are one event, with the time of the first.
    std::vector<PortEvent> events;
    receiver.release_events_before(at(60ms), [&](const PortEvent& e) { events.push_back(e); });
    CHECK_EQ(events.size(), 1u);
    CHECK(events[0].kind == PortEvent::Kind::ReceiveOverflow);
    CHECK(events[0].time == at(50ms) + 2 * kNs);
    CHECK_EQ(events[0].count, 2u);

    // An overflow the backend reports (unknown count) and an event share the stamps.
    clock.advance_to(at(70ms));
    receiver.report_overflow(0);
    PortEvent state;
    state.kind = PortEvent::Kind::BusStateChanged;
    state.bus_state = vehicle_sil::can::BusState::BusOff;
    state.time = at(1ms); // replaced by the arrival stamp
    CHECK(receiver.push_event(state));
    events.clear();
    receiver.release_events_before(at(70ms) + 5 * kNs, [&](const PortEvent& e) { events.push_back(e); });
    CHECK_EQ(events.size(), 2u);
    CHECK(events[0].kind == PortEvent::Kind::ReceiveOverflow);
    CHECK(events[0].time == at(70ms));
    CHECK_EQ(events[0].count, 0u);
    CHECK(events[1].kind == PortEvent::Kind::BusStateChanged);
    CHECK(events[1].time == at(70ms) + kNs);
    CHECK(events[1].bus_state == vehicle_sil::can::BusState::BusOff);

    // An overflow at the time of a release is handed over later, with the drops after it.
    std::vector<TimedFrame> drained;
    receiver.release_before(at(70ms) + 10 * kNs, [&](const TimedFrame& f) { drained.push_back(f); });
    CHECK(receiver.push_frame(frame(6)));
    CHECK(receiver.push_frame(frame(7)));
    CHECK(!receiver.push_frame(frame(8))); // stamped 70 ms + 12 ns
    events.clear();
    receiver.release_events_before(at(70ms) + 12 * kNs, [&](const PortEvent& e) { events.push_back(e); });
    CHECK(events.empty());
    CHECK(!receiver.push_frame(frame(9)));
    receiver.release_events_before(at(80ms), [&](const PortEvent& e) { events.push_back(e); });
    CHECK_EQ(events.size(), 1u);
    CHECK_EQ(events[0].count, 2u);
    CHECK(events[0].time == at(70ms) + 12 * kNs);

    // After the wakeup is removed it is not called; clear() discards everything.
    receiver.set_wakeup({});
    const std::size_t woken = wakes.size();
    receiver.clear();
    CHECK(receiver.push_frame(frame(5)));
    CHECK_EQ(wakes.size(), woken);
    receiver.clear();
    CHECK_EQ(receiver.pending(), 0u);
}

TEST_CASE(host_post_runs_in_order_before_the_application_in_the_next_step)
{
    ManualRig rig;
    std::vector<std::string> log;
    bool postedFromStep = false;
    rig.host.set_application([&](SimTime now) {
        log.push_back("app " + std::to_string(to_ns(now)));
        if (now >= at(10ms) && !postedFromStep)
        {
            postedFromStep = true;
            rig.host.post([&] { log.push_back("posted from the step"); }); // runs in the next step
        }
    });
    rig.host.post([&] { log.push_back("a"); }); // before the first step
    rig.driver->start();
    rig.settle();
    CHECK((log == std::vector<std::string>{"a", "app 0"}));

    // From another thread while the driver sleeps: wakes it (the clock does not move).
    std::thread poster([&] {
        rig.host.post([&] { log.push_back("b"); });
        rig.host.post([&] { log.push_back("c"); });
    });
    poster.join();
    rig.settle();
    // Each post wakes the driver, which may step between the two posts or once more after
    // them; the steps are 1 ns apart, the functions run in order, each before the application
    // of its step.
    CHECK(rig.probe.steps.size() == 2u || rig.probe.steps.size() == 3u);
    CHECK(rig.probe.steps[1] == at(0ms) + kNs);
    std::vector<std::string> posted;
    for (const auto& entry : log)
    {
        if (entry.rfind("app ", 0) != 0)
        {
            posted.push_back(entry);
        }
    }
    CHECK((posted == std::vector<std::string>{"a", "b", "c"}));
    CHECK(log[2] == std::string("b"));
    CHECK(log.back().rfind("app ", 0) == 0);
    CHECK_EQ(log.size() - posted.size(), rig.probe.steps.size());

    // Posted during a step: wakes the driver, which runs it in the next step right away.
    rig.advance_to(at(20ms)); // an idle step
    CHECK(log[log.size() - 3] == std::string("app 20000000"));
    CHECK(log[log.size() - 2] == std::string("posted from the step"));
    CHECK(log[log.size() - 1] == std::string("app 20000001"));
    CHECK(throws<std::invalid_argument>([&] { rig.host.post({}); }));
}

TEST_CASE(host_next_deadline_is_the_earliest_of_its_steppables)
{
    VirtualCanBus bus;
    Probe a;
    Probe b;
    Probe c;
    Host host;
    host.add(a, bus.attach());
    host.add(b, bus.attach());
    host.add(c, bus.attach());
    CHECK(host.next_deadline() == std::nullopt);
    a.deadline = at(30ms);
    b.deadline = at(10ms);
    CHECK(host.next_deadline() == std::optional<SimTime>(at(10ms)));
    b.deadline.reset();
    c.deadline = at(20ms);
    CHECK(host.next_deadline() == std::optional<SimTime>(at(20ms)));
}

TEST_CASE(host_post_runs_on_virtual_time_too_and_its_exception_fails_the_host)
{
    VirtualCanBus bus;
    Probe probe;
    Host host;
    host.add(probe, bus.attach());
    std::vector<std::string> log;
    host.set_application([&](SimTime) { log.push_back("app"); });
    StepDriver driver;
    driver.add(host, 10ms);
    host.post([&] { log.push_back("posted"); });
    driver.run_until(at(20ms));
    CHECK((log == std::vector<std::string>{"posted", "app", "app"}));

    host.post([] { throw std::runtime_error("posted failure"); });
    bool ran = false;
    host.post([&] { ran = true; });
    CHECK(throws_with<std::runtime_error>([&] { driver.run_until(at(30ms)); }, "posted failure"));
    CHECK(host.failed());
    CHECK(!ran);
    CHECK(throws<std::logic_error>([&] { driver.run_until(at(40ms)); }));
}

TEST_CASE(real_time_driver_keeps_the_error_of_a_failed_step)
{
    ManualRig rig;
    rig.probe.throw_at = at(10ms);
    rig.driver->start();
    rig.settle();
    rig.advance_to(at(10ms));
    CHECK(rig.driver->failed());
    CHECK(!rig.driver->running());
    CHECK(rig.driver->error() == std::string("probe failure"));
    CHECK(rig.host.failed());
    // No more steps, also when frames arrive or the clock moves.
    CHECK(rig.port.receiver.push_frame(frame(1)));
    rig.advance_to(at(50ms));
    CHECK_EQ(rig.probe.steps.size(), 2u);
    rig.driver->stop();
}

TEST_CASE(real_time_driver_stop_detaches_the_ports_and_the_clock)
{
    ManualRealTimeClock clock;
    TestRealTimePort port(clock);
    Probe probe;
    Host host;
    host.add(probe, port);
    {
        RealTimeDriver driver(host, clock);
        driver.start();
        CHECK(driver.wait_idle(kWait));
        CHECK(throws<std::logic_error>([&] { driver.start(); }));
        driver.stop();
        driver.stop(); // idempotent
    } // destroyed
    // The port and the clock no longer call into the destroyed driver (ASan/TSan would see it).
    CHECK(port.receiver.push_frame(frame(1)));
    clock.advance_to(at(100ms));
    host.post([] {});
    CHECK_EQ(probe.steps.size(), 1u);

    // A driver that is never started stops cleanly, and wait_idle() does not block.
    Host host2;
    TestRealTimePort port2(clock);
    Probe probe2;
    host2.add(probe2, port2);
    RealTimeDriver idle(host2, clock);
    CHECK(idle.wait_idle(0ms));
    CHECK(!idle.running());
}

TEST_CASE(time_domains_cannot_be_mixed)
{
    ManualRealTimeClock clock;
    VirtualCanBus bus;
    Probe a;
    Probe b;

    // Ports of both domains in one host.
    {
        Host host;
        host.add(a, bus.attach());
        TestRealTimePort port(clock);
        CHECK(throws_with<std::logic_error>([&] { host.add(b, port); }, "share a time domain"));
        CHECK(host.time_domain() == std::optional<TimeDomain>(TimeDomain::Virtual));
    }
    // A real-time port in a host of virtual time (StepDriver), before and after the driver.
    {
        Host host;
        TestRealTimePort port(clock);
        host.add(a, port);
        StepDriver driver;
        CHECK(throws_with<std::logic_error>([&] { driver.add(host, 10ms); }, "requires ports of the Virtual"));
    }
    {
        Host host;
        StepDriver driver;
        driver.add(host, 10ms);
        TestRealTimePort port(clock);
        CHECK(throws_with<std::logic_error>([&] { host.add(a, port); }, "requires ports of the Virtual"));
    }
    // A virtual port (e.g. SIL Kit with time synchronisation) under a real-time driver.
    {
        Host host;
        host.add(a, bus.attach());
        CHECK(throws_with<std::logic_error>([&] { RealTimeDriver driver(host, clock); }, "requires ports of the RealTime"));
    }
    {
        Host host;
        RealTimeDriver driver(host, clock);
        CHECK(throws<std::logic_error>([&] { host.add(a, bus.attach()); }));
        // Two drivers with different domains.
        CHECK(throws<std::logic_error>([&] { host.require_time_domain(TimeDomain::Virtual); }));
        CHECK(host.time_domain() == std::optional<TimeDomain>(TimeDomain::RealTime));
    }
    CHECK(throws<std::invalid_argument>([&] {
        Host host;
        RealTimeDriver driver(host, clock, RealTimeDriverOptions{SimDuration::zero()});
    }));
}

TEST_CASE(manual_clock_does_not_go_back)
{
    ManualRealTimeClock clock(at(5ms));
    CHECK(clock.now() == at(5ms));
    clock.advance(2ms);
    CHECK(clock.now() == at(7ms));
    CHECK(throws<std::invalid_argument>([&] { clock.advance_to(at(6ms)); }));
    CHECK(throws<std::invalid_argument>([&] { clock.advance(-1ms); }));
    SteadyRealTimeClock steady;
    const SimTime t1 = steady.now();
    const SimTime t2 = steady.now();
    CHECK(t1 <= t2);
}

// Wall clock: only arrival and order are checked (no timing bounds).
TEST_CASE(real_time_driver_on_the_wall_clock_receives_concurrent_arrivals_in_order)
{
    SteadyRealTimeClock clock;
    TestRealTimePort port(clock, 4096);
    Probe probe;
    Host host;
    host.add(probe, port);
    std::atomic<int> posted{0};
    RealTimeDriver driver(host, clock, RealTimeDriverOptions{1ms});
    driver.start();
    // Without arrivals the driver still steps every max_idle.
    CHECK(vehicle_sil::unit::eventually([&] { return probe.step_count.load() >= 3; }, 10s));

    constexpr int kSenders = 3;
    std::atomic<int> refused{0}; // CHECK would terminate inside a thread
    constexpr int kFrames = 200;
    std::vector<std::thread> senders;
    for (int s = 0; s < kSenders; ++s)
    {
        senders.emplace_back([&, s] {
            for (int i = 0; i < kFrames; ++i)
            {
                if (!port.receiver.push_frame(frame(static_cast<std::uint32_t>(0x100 + s), static_cast<std::uint8_t>(i))))
                {
                    refused.fetch_add(1);
                }
                if (i % 50 == 0)
                {
                    host.post([&] { posted.fetch_add(1); });
                    std::this_thread::sleep_for(1ms);
                }
            }
        });
    }
    for (auto& t : senders)
    {
        t.join();
    }
    CHECK(vehicle_sil::unit::eventually([&] { return probe.frame_count.load() == kSenders * kFrames; }, 10s));
    CHECK(vehicle_sil::unit::eventually([&] { return posted.load() == kSenders * (kFrames / 50); }, 10s));
    driver.stop(); // joins: the probe can be read afterwards
    CHECK(!driver.failed());
    CHECK_EQ(posted.load(), kSenders * (kFrames / 50));
    CHECK_EQ(refused.load(), 0);
    // Strictly increasing times, each sender's frames in its order, every frame before the step
    // that released it.
    std::vector<int> next(kSenders, 0);
    for (std::size_t i = 0; i < probe.frames.size(); ++i)
    {
        const auto& f = probe.frames[i];
        if (i > 0)
        {
            CHECK(f.time > probe.frames[i - 1].time);
        }
        CHECK(f.time < probe.frame_steps[i]);
        const auto s = static_cast<int>(f.frame.id - 0x100);
        CHECK_EQ(static_cast<int>(f.frame.data[0]), next[static_cast<std::size_t>(s)]);
        ++next[static_cast<std::size_t>(s)];
    }
    CHECK(std::adjacent_find(probe.steps.begin(), probe.steps.end(), std::greater_equal<>{}) == probe.steps.end());
    CHECK_EQ(port.receiver.overflows(), 0u);
}

TEST_CASE(real_time_driver_stopped_from_a_step_detaches_before_it_is_destroyed)
{
    ManualRealTimeClock clock;
    TestRealTimePort port(clock);
    Probe probe;
    Host host;
    host.add(probe, port);
    {
        RealTimeDriver driver(host, clock);
        host.set_application([&](SimTime now) {
            if (now >= at(10ms))
            {
                driver.stop(); // from the driver's own thread: no join
            }
        });
        driver.start();
        CHECK(driver.wait_idle(kWait));
        clock.advance_to(at(10ms));
        CHECK(driver.wait_idle(kWait)); // the thread ended after the step
        CHECK(!driver.running());
        CHECK(!driver.failed());
    } // the destructor joins
    // Nothing calls the destroyed driver any more (ASan would see it).
    clock.advance_to(at(20ms));
    CHECK(port.receiver.push_frame(frame(1)));
    host.post([] {});
    CHECK_EQ(probe.steps.size(), 2u);
}

TEST_CASE(real_time_receiver_keeps_places_for_acknowledgements_and_overflow_needs_none)
{
    ManualRealTimeClock clock;
    RealTimeReceiver receiver(clock, 1, 3);
    PortEvent state;
    state.kind = PortEvent::Kind::BusStateChanged;
    // Two places kept: only one of three is free for a status event.
    CHECK(receiver.push_event(state, 2));
    CHECK(!receiver.push_event(state, 2));
    CHECK_EQ(receiver.event_overflows(), 1u);
    // Without a reserve, the rest of the queue fills up, then events are dropped.
    PortEvent ack;
    ack.kind = PortEvent::Kind::TransmitCompleted;
    CHECK(receiver.push_event(ack));
    CHECK(receiver.push_event(ack));
    CHECK(!receiver.push_event(ack));
    CHECK_EQ(receiver.event_overflows(), 2u);
    // An overflow is still reported with a full event queue, after the events before it, and an
    // unknown number makes the count unknown.
    CHECK(receiver.push_frame(frame(1)));
    CHECK(!receiver.push_frame(frame(2)));
    receiver.report_overflow(0);
    std::vector<PortEvent> events;
    receiver.release_events_before(at(1ms), [&](const PortEvent& e) { events.push_back(e); });
    CHECK_EQ(events.size(), 4u);
    CHECK(events[3].kind == PortEvent::Kind::ReceiveOverflow);
    CHECK_EQ(events[3].count, 0u);
}
