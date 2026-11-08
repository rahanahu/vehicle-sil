// SPDX-License-Identifier: MIT
//
// vehicle_sil::can::Host and StepDriver on the virtual bus. Built without SIL Kit.
#include "test_harness.hpp"
#include "trace_protocol.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::can::Frame;
using vehicle_sil::can::Host;
using vehicle_sil::can::Port;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::Steppable;
using vehicle_sil::can::StepDriver;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;
using vehicle_sil::unit::TraceProtocol;

namespace {

SimTime at(std::chrono::milliseconds t)
{
    return SimTime{} + t;
}

/// Records every call; optionally sends one frame per step and throws at a chosen stage.
class Recorder final : public Steppable
{
public:
    Recorder(std::string name, std::vector<std::string>& log) : name_(std::move(name)), log_(log) {}

    std::function<void(const char* stage)> on_stage; // may throw
    bool send_each_step = false;
    bool record_start = false; ///< log start() (off for the tests that predate it)
    std::uint32_t id = 0x100;
    std::vector<Frame> to_send;

    void begin(SimTime now) override
    {
        now_ = now;
        stage("begin");
    }
    void receive(const TimedFrame& frame) override
    {
        log_.push_back(name_ + ".receive " + std::to_string(to_ns(frame.time)) + " " + std::to_string(frame.frame.id));
    }
    void process() override { stage("process"); }
    void start(std::optional<SimDuration> step_size) override
    {
        if (record_start)
        {
            log_.push_back(name_ + ".start " + (step_size ? std::to_string(step_size->count()) : std::string("none")));
        }
        if (on_stage)
        {
            on_stage("start");
        }
    }
    void flush(Port& port) override
    {
        stage("flush");
        if (send_each_step)
        {
            Frame f;
            f.id = id;
            to_send.push_back(f);
        }
        for (const auto& f : to_send)
        {
            if (port.send(TimedFrame{now_, f}) != SendResult::Accepted)
            {
                throw std::runtime_error("send failed");
            }
        }
        to_send.clear();
    }

private:
    void stage(const char* what)
    {
        log_.push_back(name_ + "." + what);
        if (on_stage)
        {
            on_stage(what);
        }
    }

    std::string name_;
    std::vector<std::string>& log_;
    SimTime now_{};
};

} // namespace

TEST_CASE(host_runs_the_stages_over_all_channels_in_registration_order)
{
    VirtualCanBus bus;
    auto& portA = bus.attach();
    auto& portB = bus.attach();
    auto& remote = bus.attach();
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    Host host;
    host.add(a, portA);
    host.add(b, portB);
    host.set_application([&](SimTime now) { log.push_back("app " + std::to_string(to_ns(now))); });

    remote.send(TimedFrame{at(0ms), Frame{0x7, vehicle_sil::can::IdFormat::Extended, 0, {}}});
    host.step(at(1ms));

    const std::vector<std::string> expected{"a.begin", "b.begin", "a.receive 0 7", "b.receive 0 7", "a.process",
                                            "b.process", "app 1000000",   "a.flush",       "b.flush"};
    CHECK(log == expected);
    CHECK(host.last_step() == at(1ms));
    CHECK(!host.failed());
}

TEST_CASE(host_frames_sent_in_a_step_arrive_in_the_next_step_of_the_receiver)
{
    VirtualCanBus bus;
    auto& p1 = bus.attach();
    auto& p2 = bus.attach();
    std::vector<std::string> log;
    Recorder sender("s", log);
    Recorder receiver("r", log);
    sender.id = 0x42;
    sender.send_each_step = true;
    Host h1;
    Host h2;
    h1.add(sender, p1);
    h2.add(receiver, p2);

    h1.step(at(0ms));
    h2.step(at(0ms)); // stamped 0 is not before 0
    h2.step(at(1ms));
    std::vector<std::string> received;
    for (const auto& line : log)
    {
        if (line.rfind("r.receive", 0) == 0)
        {
            received.push_back(line);
        }
    }
    CHECK(received == std::vector<std::string>{"r.receive 0 66"});
}

TEST_CASE(host_rejects_a_time_that_does_not_increase_and_fails)
{
    VirtualCanBus bus;
    auto& port = bus.attach();
    std::vector<std::string> log;
    Recorder r("r", log);
    Host host;
    host.add(r, port);
    host.step(at(10ms));
    CHECK(throws_with<std::logic_error>([&] { host.step(at(10ms)); }, "must increase"));
    CHECK(host.failed());
    CHECK(throws_with<std::logic_error>([&] { host.step(at(20ms)); }, "has failed"));
}

TEST_CASE(host_exception_in_a_stage_fails_the_host_and_skips_the_rest_of_the_step)
{
    VirtualCanBus bus;
    auto& p1 = bus.attach();
    auto& p2 = bus.attach();
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    a.on_stage = [](const char* stage) {
        if (std::string(stage) == "process")
        {
            throw std::runtime_error("broken protocol");
        }
    };
    Host host;
    host.add(a, p1);
    host.add(b, p2);
    bool appRan = false;
    host.set_application([&](SimTime) { appRan = true; });

    CHECK(throws_with<std::runtime_error>([&] { host.step(at(0ms)); }, "broken protocol"));
    CHECK(host.failed());
    CHECK(host.error() == "broken protocol");
    CHECK(!appRan);
    CHECK(log.back() == "a.process"); // b.process and the flushes did not run
    log.clear();
    CHECK(throws_with<std::logic_error>([&] { host.step(at(1ms)); }, "broken protocol"));
    CHECK(log.empty());
}

TEST_CASE(host_application_exception_fails_the_host)
{
    VirtualCanBus bus;
    auto& port = bus.attach();
    std::vector<std::string> log;
    Recorder r("r", log);
    Host host;
    host.add(r, port);
    host.set_application([](SimTime now) {
        if (now >= SimTime{} + 2ms)
        {
            throw std::runtime_error("application gave up");
        }
    });
    host.step(at(0ms));
    host.step(at(1ms));
    CHECK(throws<std::runtime_error>([&] { host.step(at(2ms)); }));
    CHECK(host.failed());
    CHECK(throws<std::logic_error>([&] { host.step(at(3ms)); }));
}

TEST_CASE(host_rejects_a_step_from_inside_a_step)
{
    VirtualCanBus bus;
    auto& port = bus.attach();
    std::vector<std::string> log;
    Recorder r("r", log);
    Host host;
    host.add(r, port);
    host.set_application([&](SimTime now) { host.step(now + 1ms); });
    CHECK(throws_with<std::logic_error>([&] { host.step(at(0ms)); }, "inside a step"));
    CHECK(host.failed());
}

TEST_CASE(host_stays_failed_when_the_application_swallows_a_nested_failure)
{
    VirtualCanBus bus;
    auto& port = bus.attach();
    std::vector<std::string> log;
    Recorder r("r", log);
    Host host;
    host.add(r, port);
    host.set_application([&](SimTime now) {
        try
        {
            host.step(now + 1ms);
        }
        catch (const std::logic_error&)
        {
        }
    });
    CHECK(throws_with<std::logic_error>([&] { host.step(at(0ms)); }, "failed during the step"));
    CHECK(host.failed());
    CHECK(log.back() == "r.process"); // no flush after the failure
}

TEST_CASE(host_registration_is_only_allowed_before_the_first_step)
{
    VirtualCanBus bus;
    auto& p1 = bus.attach();
    auto& p2 = bus.attach();
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    Host host;
    host.add(a, p1);
    CHECK(throws<std::logic_error>([&] { host.add(b, p1); })); // port already used
    CHECK(throws<std::logic_error>([&] { host.add(a, p2); })); // steppable already used
    host.step(at(0ms));
    CHECK(throws<std::logic_error>([&] { host.add(b, p2); }));
    CHECK(throws<std::logic_error>([&] { host.set_application([](SimTime) {}); }));
    CHECK(!host.failed()); // misuse outside step() does not fail the host
}

TEST_CASE(step_driver_runs_steps_in_time_then_registration_order)
{
    VirtualCanBus bus;
    auto& p1 = bus.attach();
    auto& p2 = bus.attach();
    std::vector<std::string> log;
    Recorder a("a", log);
    Recorder b("b", log);
    Host ha;
    Host hb;
    ha.add(a, p1);
    hb.add(b, p2);
    std::vector<std::string> steps;
    ha.set_application([&](SimTime now) { steps.push_back("a" + std::to_string(to_ns(now) / 1'000'000)); });
    hb.set_application([&](SimTime now) { steps.push_back("b" + std::to_string(to_ns(now) / 1'000'000)); });

    StepDriver driver;
    driver.add(ha, 10ms);
    driver.add(hb, 3ms);
    driver.run_until(at(10ms));
    CHECK((steps == std::vector<std::string>{"a0", "b0", "b3", "b6", "b9"}));
    CHECK(driver.next_step_time() == at(10ms));
    driver.run_until(at(13ms)); // continues; equal start times run in registration order
    CHECK((steps == std::vector<std::string>{"a0", "b0", "b3", "b6", "b9", "a10", "b12"}));
}

TEST_CASE(step_driver_misuse_is_rejected)
{
    VirtualCanBus bus;
    auto& port = bus.attach();
    std::vector<std::string> log;
    Recorder r("r", log);
    Host host;
    host.add(r, port);
    StepDriver driver;
    CHECK(throws<std::invalid_argument>([&] { driver.add(host, 0ms); }));
    driver.add(host, 1ms);
    CHECK(throws<std::logic_error>([&] { driver.add(host, 2ms); }));
    driver.run_until(at(1ms));
    Host other;
    CHECK(throws<std::logic_error>([&] { driver.add(other, 1ms); }));
}

TEST_CASE(step_driver_propagates_a_host_failure)
{
    VirtualCanBus bus;
    auto& port = bus.attach();
    std::vector<std::string> log;
    Recorder r("r", log);
    Host host;
    host.add(r, port);
    host.set_application([](SimTime now) {
        if (now == SimTime{} + 2ms)
        {
            throw std::runtime_error("stop here");
        }
    });
    StepDriver driver;
    driver.add(host, 1ms);
    CHECK(throws<std::runtime_error>([&] { driver.run_until(at(10ms)); }));
    CHECK(host.failed());
    CHECK(throws<std::logic_error>([&] { driver.run_until(at(10ms)); }));
}

TEST_CASE(step_driver_shutdown_order_leaves_nothing_dangling)
{
    // Design 4.2 for the test loop: stop driving (no more run_until on that driver), destroy the
    // stack, and only then the port (the bus owns it). The bus keeps queueing frames for an
    // endpoint whose stack is gone without calling into it. ASan checks the accesses.
    VirtualCanBus bus;
    auto& portA = bus.attach();
    auto& portB = bus.attach();
    auto a = std::make_unique<TraceProtocol>(1, 3ms);
    auto b = std::make_unique<TraceProtocol>(2, 5ms);
    auto hostA = std::make_unique<Host>();
    auto hostB = std::make_unique<Host>();
    hostA->add(*a, portA);
    hostB->add(*b, portB);
    {
        StepDriver driver;
        driver.add(*hostA, 1ms);
        driver.add(*hostB, 1ms);
        driver.run_until(at(20ms));
    } // 1. the driver that stepped B is gone
    hostB.reset(); // 2./4. B's host and stack
    b.reset();
    const auto pendingAtStop = portB.pending();

    // A goes on alone (stepped by hand) and keeps sending to B's endpoint. Its frames are not
    // older than B's last release (19 ms).
    const auto before = a->trace().size();
    for (auto t = 20ms; t < 40ms; t += 1ms)
    {
        hostA->step(at(t));
    }
    CHECK(a->trace().size() > before);
    CHECK(portB.pending() > pendingAtStop);
    CHECK_EQ(portB.overflows(), 0u);
}

namespace {

/// Runs the three-node trace scenario on the virtual bus. `order` permutes the registration of
/// the hosts with the driver.
std::vector<std::vector<std::string>> run_trace_scenario(const std::vector<SimDuration>& steps, const std::vector<int>& order)
{
    VirtualCanBus bus;
    std::vector<std::unique_ptr<TraceProtocol>> nodes;
    std::vector<std::unique_ptr<Host>> hosts;
    const SimDuration periods[] = {7ms, 11ms, 13ms};
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        nodes.push_back(std::make_unique<TraceProtocol>(static_cast<std::uint8_t>(i + 1), periods[i % 3]));
        hosts.push_back(std::make_unique<Host>());
        hosts.back()->add(*nodes.back(), bus.attach());
    }
    StepDriver driver;
    for (int index : order)
    {
        driver.add(*hosts[static_cast<std::size_t>(index)], steps[static_cast<std::size_t>(index)]);
    }
    driver.run_until(SimTime{} + 100ms);
    std::vector<std::vector<std::string>> traces;
    for (const auto& node : nodes)
    {
        traces.push_back(node->trace());
    }
    return traces;
}

} // namespace

TEST_CASE(virtual_bus_traces_do_not_depend_on_the_order_of_equal_time_steps)
{
    const std::vector<SimDuration> steps{10ms, 10ms, 5ms};
    const auto reference = run_trace_scenario(steps, {0, 1, 2});
    CHECK(run_trace_scenario(steps, {2, 1, 0}) == reference);
    CHECK(run_trace_scenario(steps, {1, 2, 0}) == reference);
    CHECK(run_trace_scenario(steps, {0, 1, 2}) == reference); // repeatable
    // The scenario really has traffic and same-time frames from different senders.
    for (const auto& trace : reference)
    {
        CHECK(trace.size() > 50u);
    }
    CHECK(vehicle_sil::unit::same_time_receptions(reference[2]) > 0u);
}

TEST_CASE(two_nodes_exchange_frames_and_repeat_identically_for_each_step_size)
{
    // Same-time frames from different senders are covered by the three-node test above; this one
    // checks that two nodes talk in both directions for each step size combination, that a run
    // repeats exactly, and that the frames of one sender keep their send order.
    const auto has = [](const std::vector<std::string>& trace, const std::string& what, const std::string& id_prefix) {
        return std::any_of(trace.begin(), trace.end(), [&](const std::string& line) {
            return line.find(what) != std::string::npos && line.find(" " + id_prefix) != std::string::npos;
        });
    };
    const std::vector<std::vector<SimDuration>> combinations{{10ms, 10ms}, {1ms, 10ms}, {10ms, 1ms}, {3ms, 7ms}};
    for (const auto& steps : combinations)
    {
        const auto reference = run_trace_scenario(steps, {0, 1});
        CHECK(run_trace_scenario(steps, {0, 1}) == reference); // repeatable
        CHECK(run_trace_scenario(steps, {1, 0}) == reference); // independent of registration order
        // Node 1 receives periodic frames of node 2 and answers them (kind 0x31), and vice versa.
        CHECK(has(reference[0], " rx ", "18002002"));
        CHECK(has(reference[0], " tx ", "18003101"));
        CHECK(has(reference[1], " rx ", "18002001"));
        CHECK(has(reference[1], " tx ", "18003102"));
    }
    // With 10 ms steps, node 1 sees node 2's two periodic frames of T = 0 in its step at 10 ms in
    // the order node 2 sent them. A goes first although B has the lower id, so a port that sorted
    // same-time frames by id would fail here. Node 1 answers both in the same step; the two
    // answers are equal frames.
    const auto traces = run_trace_scenario({10ms, 10ms}, {0, 1});
    const auto& node1 = traces[0];
    const auto first = std::find(node1.begin(), node1.end(), "10000000 rx 0 18002002 00020010");
    const auto second = std::find(node1.begin(), node1.end(), "10000000 rx 0 18001002 00020010");
    CHECK(first != node1.end());
    CHECK(second != node1.end());
    CHECK(first < second);
    CHECK_EQ(std::count(node1.begin(), node1.end(), "10000000 tx 10000000 18003101 0102002E"), 2);
    // Node 2 receives both answers one step later.
    CHECK_EQ(std::count(traces[1].begin(), traces[1].end(), "20000000 rx 10000000 18003101 0102002E"), 2);
}

TEST_CASE(host_passes_the_step_size_to_every_steppable_once_before_the_first_step)
{
    std::vector<std::string> log;
    VirtualCanBus bus;
    Recorder a("a", log);
    Recorder b("b", log);
    a.record_start = true;
    b.record_start = true;
    Host host;
    host.add(a, bus.attach());
    host.add(b, bus.attach());
    StepDriver driver;
    driver.add(host, 10ms);
    driver.run_until(at(20ms));
    const std::vector<std::string> expected{"a.start 10000000", "b.start 10000000", "a.begin", "b.begin", "a.process",
                                            "b.process", "a.flush", "b.flush", "a.begin", "b.begin", "a.process",
                                            "b.process", "a.flush", "b.flush"};
    CHECK(log == expected);
}

TEST_CASE(host_without_a_step_size_starts_with_none)
{
    std::vector<std::string> log;
    VirtualCanBus bus;
    Recorder a("a", log);
    a.record_start = true;
    Host host;
    host.add(a, bus.attach());
    host.step(at(0ms));
    CHECK(log.front() == "a.start none");
}

TEST_CASE(host_step_size_is_checked_and_only_set_before_the_first_step)
{
    Host host;
    CHECK(throws<std::invalid_argument>([&] { host.set_step_size(SimDuration::zero()); }));
    CHECK(throws<std::invalid_argument>([&] { host.set_step_size(-1ms); }));
    host.set_step_size(5ms);
    host.step(at(0ms));
    CHECK(throws<std::logic_error>([&] { host.set_step_size(5ms); }));
}

TEST_CASE(host_fails_when_a_steppable_rejects_the_step_size)
{
    std::vector<std::string> log;
    VirtualCanBus bus;
    Recorder a("a", log);
    a.record_start = true;
    a.on_stage = [](const char* stage) {
        if (std::string(stage) == "start")
        {
            throw std::invalid_argument("step too large");
        }
    };
    Host host;
    host.add(a, bus.attach());
    host.set_step_size(1s);
    CHECK(throws<std::invalid_argument>([&] { host.step(at(0ms)); }));
    CHECK(host.failed());
    CHECK(host.error() == "step too large");
    CHECK(throws<std::logic_error>([&] { host.step(at(1s)); }));
    CHECK(log == (std::vector<std::string>{"a.start 1000000000"})); // begin never ran
}
