// SPDX-License-Identifier: MIT
//
// The J1939 stack on the virtual bus with timing (arbitration, transmission time, bus
// occupancy): the protocol results are those of the bus without timing, the frame times do not
// depend on the receivers' step sizes, a loaded bus slows transfers down and a saturated one
// makes them time out, and fault injection and a small transmit queue work with timing.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::FaultInjector;
using vehicle_sil::can::Host;
using vehicle_sil::can::Port;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::Steppable;
using vehicle_sil::can::StepDriver;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::can::VirtualCanBusOptions;
using vehicle_sil::can::VirtualCanBusTiming;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::Pgn;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::TransferEnd;
using vehicle_sil::j1939::TransferResult;

namespace {

constexpr Pgn kStatus{0xFF30};
constexpr Pgn kBroadcast{0xFF31};
constexpr Pgn kData{0xEF00};

VirtualCanBusOptions untimed()
{
    return {};
}

VirtualCanBusOptions timed(std::uint32_t bitrate, SimDuration simulatorStep = {}, std::size_t queue = 64)
{
    VirtualCanBusOptions options;
    options.timing = VirtualCanBusTiming{bitrate, queue, simulatorStep};
    return options;
}

/// An 11-bit frame with identifier 0 (it wins every arbitration), `per_step` of them in every
/// step in [from, until). Refused frames (full transmit queue) are not retried.
class Load final : public Steppable
{
public:
    Load(SimTime from, SimTime until, int perStep) : from_(from), until_(until), per_step_(perStep) {}
    void begin(SimTime now) override { now_ = now; }
    void receive(const TimedFrame&) override {}
    void process() override {}
    void flush(Port& port) override
    {
        if (now_ < from_ || now_ >= until_)
        {
            return;
        }
        for (int i = 0; i < per_step_; ++i)
        {
            vehicle_sil::can::Frame f;
            f.id = 0;
            f.format = vehicle_sil::can::IdFormat::Standard;
            f.length = 8;
            if (port.send({now_, f}) == SendResult::Accepted)
            {
                ++sent_;
            }
        }
    }
    [[nodiscard]] int sent() const { return sent_; }

private:
    SimTime from_;
    SimTime until_;
    int per_step_;
    SimTime now_{};
    int sent_ = 0;
};

/// One J1939 network with one node; records messages and transfer ends.
struct Ecu
{
    explicit Ecu(NodeOptions options) : node(network.add_node(options))
    {
        node.on_any_message([this](const Message& m) {
            char text[96];
            std::snprintf(text, sizeof(text), "%05X %02X->%02X n%zu", static_cast<unsigned>(m.pgn.value()), static_cast<unsigned>(m.source),
                          static_cast<unsigned>(m.destination), m.data.size());
            messages.push_back(text);
            timed_messages.push_back(std::to_string(to_ns(m.time)) + " " + text);
        });
        node.on_transfer_end([this](const TransferEnd& e) {
            ends.push_back(e);
        });
    }

    Network network;
    Node& node;
    std::vector<std::string> messages;       ///< without times, in arrival order
    std::vector<std::string> timed_messages; ///< with the frame times
    std::vector<TransferEnd> ends;
};

/// What a scenario does at which time (the first step at or after it).
struct Action
{
    SimTime at;
    std::size_t ecu;
    std::function<void(Node&)> run;
    bool done = false;
};

struct Setup
{
    VirtualCanBusOptions bus = untimed();
    std::vector<SimDuration> steps{10ms, 1ms, 7ms}; ///< X, Y, Z
    std::vector<Action> actions;
    std::optional<Load> load;
    FaultInjector x_faults;
    SimTime end = SimTime{} + 2000ms;
};

struct Result
{
    std::vector<std::unique_ptr<Ecu>> ecus;
    std::size_t transmit_pending_x = 0;
};

void check_ok(SendStatus status)
{
    if (status != SendStatus::Ok)
    {
        throw std::runtime_error("send refused in the scenario: " + std::to_string(static_cast<int>(status)));
    }
}

Result run(Setup setup)
{
    VirtualCanBus bus(setup.bus);
    Result result;
    const std::uint64_t names[3] = {0xA0008200FE001001u, 0xA0008200FE001002u, 0xA0008200FE001003u};
    const Address addresses[3] = {0x10, 0x20, 0x30};
    std::vector<std::unique_ptr<Host>> hosts;
    StepDriver driver;
    VirtualCanBus::Endpoint* xEndpoint = nullptr;
    for (std::size_t i = 0; i < 3; ++i)
    {
        result.ecus.push_back(std::make_unique<Ecu>(NodeOptions{addresses[i], Name{names[i]}}));
        hosts.push_back(std::make_unique<Host>());
        auto& endpoint = bus.attach();
        if (i == 0)
        {
            xEndpoint = &endpoint;
            if (setup.x_faults)
            {
                endpoint.set_fault_injector(setup.x_faults);
            }
        }
        hosts.back()->add(result.ecus.back()->network, endpoint);
        Node& node = result.ecus.back()->node;
        hosts.back()->set_application([&setup, &node, i](SimTime now) {
            for (auto& action : setup.actions)
            {
                if (!action.done && action.ecu == i && action.at <= now)
                {
                    action.done = true;
                    action.run(node);
                }
            }
        });
        driver.add(*hosts.back(), setup.steps[i]);
    }
    Host loadHost;
    if (setup.load)
    {
        loadHost.add(*setup.load, bus.attach());
        driver.add(loadHost, 1ms);
    }
    driver.run_until(setup.end);
    for (const auto& ecu : result.ecus)
    {
        if (ecu->network.failed())
        {
            throw std::runtime_error("network failed: " + ecu->network.error());
        }
    }
    result.transmit_pending_x = xEndpoint->transmit_pending();
    return result;
}

/// The usual traffic after claiming, in this order: X sends a status every 100 ms from 300 to
/// 1900 ms (17), a broadcast of 30 bytes (BAM) at 300 ms, and 100 bytes to Y (RTS/CTS) at
/// 400 ms; Z sends 60 bytes to X at 400 ms.
std::vector<Action> traffic()
{
    std::vector<Action> actions;
    for (int k = 3; k < 20; ++k)
    {
        actions.push_back({SimTime{} + k * 100ms, 0, [k](Node& n) {
                               const std::array<std::uint8_t, 2> d{static_cast<std::uint8_t>(k), 0};
                               check_ok(n.send(kStatus, d));
                           }});
    }
    actions.push_back({SimTime{} + 300ms, 0, [](Node& n) { check_ok(n.send(kBroadcast, payload(30, 1))); }});
    actions.push_back({SimTime{} + 400ms, 0, [](Node& n) { check_ok(n.send(kData, payload(100, 2), 0x20)); }});
    actions.push_back({SimTime{} + 400ms, 2, [](Node& n) { check_ok(n.send(kData, payload(60, 3), 0x10)); }});
    return actions;
}

std::vector<std::string> sorted(std::vector<std::string> v)
{
    std::sort(v.begin(), v.end());
    return v;
}

std::string ends_of(const Ecu& ecu)
{
    std::string out;
    for (const auto& e : ecu.ends)
    {
        out += std::to_string(e.pgn.value()) + "/" + std::to_string(e.length) + "/" + std::to_string(static_cast<int>(e.result)) + "/" +
               std::to_string(static_cast<int>(e.reason)) + " ";
    }
    return out;
}

} // namespace

TEST_CASE(j1939_gives_the_same_protocol_results_with_and_without_bus_timing)
{
    Setup reference;
    reference.actions = traffic();
    const Result base = run(reference);
    // X: the connection (done at 410 ms) and the broadcast (its last packet at 550 ms)
    // completed; Y got both; X got Z's 60 bytes.
    CHECK(ends_of(*base.ecus[0]) == "61184/100/0/0 65329/30/0/0 ");
    CHECK(ends_of(*base.ecus[2]) == "61184/60/0/0 ");
    CHECK(std::count(base.ecus[1]->messages.begin(), base.ecus[1]->messages.end(), "0FF31 10->FF n30") == 1);
    CHECK(std::count(base.ecus[1]->messages.begin(), base.ecus[1]->messages.end(), "0EF00 10->20 n100") == 1);
    CHECK(std::count(base.ecus[0]->messages.begin(), base.ecus[0]->messages.end(), "0EF00 30->10 n60") == 1);
    for (const auto& options : {timed(250'000), timed(250'000, 1ms), timed(125'000), timed(1'000'000, 3ms)})
    {
        Setup setup;
        setup.bus = options;
        setup.actions = traffic();
        const Result r = run(setup);
        for (std::size_t i = 0; i < 3; ++i)
        {
            CHECK(sorted(r.ecus[i]->messages) == sorted(base.ecus[i]->messages));
            CHECK(ends_of(*r.ecus[i]) == ends_of(*base.ecus[i]));
            CHECK(r.ecus[i]->node.address_state() == AddressState::Claimed);
            const auto& s = r.ecus[i]->network.statistics();
            CHECK_EQ(s.send_queue_full + s.send_failures + s.transport_timeouts + s.transport_aborts_sent, 0u);
            // Every frame the port accepted was reported transmitted.
            CHECK_EQ(s.frames_transmitted, s.frames_sent);
        }
        // With timing the frames arrive later than they were sent.
        CHECK(r.ecus[1]->timed_messages != base.ecus[1]->timed_messages);
    }
    CHECK_EQ(base.ecus[0]->network.statistics().frames_transmitted, 0u); // no events without timing
}

TEST_CASE(j1939_frame_times_on_the_timed_bus_do_not_depend_on_the_receivers_step_sizes)
{
    // Only X sends after the claims (statuses and a broadcast); Y and Z listen. Their claims go
    // out in their first step (time 0) whatever their step size.
    const auto listen = [](std::vector<SimDuration> steps) {
        Setup setup;
        setup.bus = timed(250'000);
        setup.steps = std::move(steps);
        auto actions = traffic();
        actions.resize(17 + 1); // the statuses and the broadcast of X
        setup.actions = actions;
        const Result r = run(setup);
        return std::vector<std::vector<std::string>>{r.ecus[1]->timed_messages, r.ecus[2]->timed_messages};
    };
    const auto reference = listen({10ms, 1ms, 1ms});
    CHECK_EQ(reference[0].size(), 2u + 17u + 1u); // the claims of X and Z, the statuses, the broadcast
    for (const auto& steps : std::vector<std::vector<SimDuration>>{{10ms, 3ms, 7ms}, {10ms, 10ms, 13ms}, {10ms, 50ms, 2ms}})
    {
        CHECK(listen(steps) == reference);
    }
}

TEST_CASE(j1939_transfer_takes_longer_on_a_loaded_bus)
{
    const auto completion = [](std::optional<Load> load) {
        Setup setup;
        setup.bus = timed(500'000);
        setup.steps = {1ms, 1ms, 1ms};
        setup.actions.push_back({SimTime{} + 800ms, 0, [](Node& n) { check_ok(n.send(kData, payload(200, 4), 0x20)); }});
        setup.load = std::move(load);
        const Result r = run(setup);
        CHECK(ends_of(*r.ecus[0]) == "61184/200/0/0 ");
        return r.ecus[0]->ends.at(0).time;
    };
    const SimTime idle = completion(std::nullopt);
    // About 44 % of the bus: 2 frames of 222 us per millisecond, each ahead of every J1939 frame.
    const SimTime loaded = completion(Load(SimTime{} + 700ms, SimTime{} + 1500ms, 2));
    CHECK(idle < SimTime{} + 900ms);
    CHECK(loaded > idle);
}

TEST_CASE(j1939_transfer_times_out_on_a_saturated_bus_and_recovers_after_it)
{
    Setup setup;
    setup.bus = timed(500'000);
    // 5 frames of 222 us per millisecond: more than the bus can carry, so a J1939 frame never
    // wins arbitration while the load lasts.
    setup.load = Load(SimTime{} + 700ms, SimTime{} + 3000ms, 5);
    setup.end = SimTime{} + 4500ms;
    setup.actions.push_back({SimTime{} + 800ms, 0, [](Node& n) { check_ok(n.send(kData, payload(100, 5), 0x20)); }});
    setup.actions.push_back({SimTime{} + 3500ms, 0, [](Node& n) { check_ok(n.send(kData, payload(100, 6), 0x20)); }});
    const Result r = run(setup);
    const auto& ends = r.ecus[0]->ends;
    CHECK_EQ(ends.size(), 2u);
    CHECK(ends.at(0).result == TransferResult::Aborted);
    CHECK(ends.at(0).reason == AbortReason::Timeout);
    CHECK(ends.at(0).time < SimTime{} + 3000ms); // timed out while the bus was saturated
    CHECK(ends.at(1).result == TransferResult::Completed);
    CHECK(std::count(r.ecus[1]->messages.begin(), r.ecus[1]->messages.end(), "0EF00 10->20 n100") == 1);
    CHECK_EQ(r.transmit_pending_x, 0u);
}

TEST_CASE(j1939_fault_injection_works_on_the_timed_bus)
{
    // The third data packet of X's first connection to Y is lost: Y asks for it again and the
    // transfer completes, as on the bus without timing.
    for (const auto& options : {untimed(), timed(250'000), timed(250'000, 1ms)})
    {
        Setup setup;
        setup.bus = options;
        setup.actions = traffic();
        bool dropped = false;
        setup.x_faults = [&dropped](const TimedFrame& f) {
            const auto j = from_can(f.frame);
            if (!dropped && j.pgn == kPgnTpDt && j.da == 0x20 && !j.data.empty() && j.data[0] == 3)
            {
                dropped = true;
                return FaultAction::drop();
            }
            return FaultAction::deliver();
        };
        const Result r = run(setup);
        CHECK(dropped);
        CHECK(ends_of(*r.ecus[0]) == "61184/100/0/0 65329/30/0/0 ");
        CHECK_EQ(r.ecus[1]->network.statistics().transport_retransmit_requests, 1u);
    }
}

TEST_CASE(j1939_waits_for_a_full_transmit_queue_on_the_timed_bus)
{
    // Two places in the transmit queue: a window of 16 packets does not fit; the network keeps
    // the rest for the next steps (QueueFull) and the transfers still complete.
    Setup setup;
    setup.bus = timed(250'000, {}, 2);
    setup.actions = traffic();
    setup.actions.push_back({SimTime{} + 1000ms, 0, [](Node& n) { check_ok(n.send(kData, payload(500, 7), 0x30)); }});
    const Result r = run(setup);
    CHECK(ends_of(*r.ecus[0]) == "61184/100/0/0 65329/30/0/0 61184/500/0/0 ");
    CHECK(r.ecus[0]->network.statistics().send_queue_full > 0u);
    CHECK_EQ(r.ecus[0]->network.statistics().send_failures, 0u);
}
