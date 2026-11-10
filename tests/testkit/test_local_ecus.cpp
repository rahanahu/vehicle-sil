// SPDX-License-Identifier: MIT
//
// The test kit's ECUs (LocalCan::add_ecu()): an ECU with networks on several named buses, its
// work per step, the lookups by ECU and network, the set-up rules, the forms without a bus name
// on the only bus, and the order of destruction. Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/testkit/local_can.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::Ecu;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;
namespace can = vehicle_sil::can;
namespace j1939 = vehicle_sil::j1939;
using vehicle_sil::testkit::LocalCan;

namespace {

constexpr j1939::Pgn kPgnValue{0xFF20}; ///< proprietary B: a counter

SimTime at(std::chrono::nanoseconds t) { return SimTime{} + t; }
long long ms(SimTime t) { return (t - SimTime{}) / 1ms; }

/// Sends kPgnValue with a counter every 10 ms on the network `network`.
void install_source(Ecu& ecu, const std::string& network, j1939::Address address)
{
    auto& node = ecu.j1939(network).add_node({.address = address});
    auto counter = std::make_shared<std::uint8_t>(0);
    ecu.every(10ms, [&node, counter](SimTime) {
        const std::uint8_t data[8]{(*counter)++, 0, 0, 0, 0, 0, 0, 0};
        static_cast<void>(node.send(kPgnValue, data));
    });
}

/// Records (time, first byte) of every kPgnValue it receives on the network `network`.
struct Recorder
{
    std::vector<std::pair<long long, int>> seen;
    Recorder(Ecu& ecu, const std::string& network, j1939::Address address)
    {
        auto& node = ecu.j1939(network).add_node({.address = address});
        node.on_message(kPgnValue, [this](const j1939::Message& m) { seen.emplace_back(ms(m.time), m.data[0]); });
    }
};

/// Receives kPgnValue on network "A" and sends it on network "B" from the same step.
void install_gateway(Ecu& ecu)
{
    auto& in = ecu.j1939("A").add_node({.address = 0x20});
    auto& out = ecu.j1939("B").add_node({.address = 0x21});
    in.on_message(kPgnValue, [&out](const j1939::Message& m) {
        const std::uint8_t data[8]{m.data[0], 0, 0, 0, 0, 0, 0, 0};
        static_cast<void>(out.send(kPgnValue, data));
    });
}

/// kPgnValue from 0x40 with `value` in its first byte.
can::Frame value_frame(std::uint8_t value)
{
    can::Frame frame;
    frame.id = 0x18FF2040; // priority 6, kPgnValue, from 0x40
    frame.length = 8;
    frame.data[0] = value;
    return frame;
}

/// Writes its name into `log` when destroyed.
struct Probe
{
    std::vector<std::string>& log;
    std::string name;
    Probe(std::vector<std::string>& l, std::string n) : log(l), name(std::move(n)) {}
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe() { log.push_back(name); }
};

} // namespace

TEST_CASE(testkit_gateway_ecu_forwards_between_two_buses_in_the_same_step)
{
    LocalCan buses;
    install_source(buses.add_ecu("source", 10ms), "A", 0x10);
    install_gateway(buses.add_ecu("gateway", 10ms));
    auto& sink = buses.add_ecu("sink", 5ms);
    auto& recorder = sink.emplace<Recorder>(sink, "B", 0x30);
    // A listener on bus A only: the source's frames never reach bus B but through the gateway.
    auto& listener = buses.add_ecu("listener_a", 10ms);
    auto& onA = listener.emplace<Recorder>(listener, "A", 0x31);
    buses.run_until(at(100ms));

    CHECK(&buses.bus("A") != &buses.bus("B"));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("")); })); // no unnamed bus
    CHECK_EQ(buses.bus("A").endpoint_count(), 4u); // injector, source, gateway, listener
    CHECK_EQ(buses.bus("B").endpoint_count(), 3u); // injector, gateway, sink
    // Sent at 0, 10, ..., 80 by the source; the gateway receives each in its next step and sends
    // it on from that step: on bus B 10 ms after the source sent it.
    std::vector<std::pair<long long, int>> expected;
    for (int i = 0; i < 9; ++i)
    {
        expected.emplace_back(i * 10 + 10, i);
    }
    CHECK(recorder.seen == expected);
    CHECK_EQ(onA.seen.size(), 9u); // sent at 0 .. 80, received in its steps at 10 .. 90
    CHECK_EQ(onA.seen.front().first, 0);
    // The lookups by ECU and network.
    CHECK(&buses.network("gateway", "A") != &buses.network("gateway", "B"));
    CHECK(&buses.network("sink") == &buses.network("sink", "B"));
    CHECK(&buses.endpoint("sink") == &buses.endpoint("sink", "B"));
    CHECK(&buses.tap("source") == &buses.tap("source", "A"));
    CHECK(&buses.ecu("sink") == &sink);

    // A raw frame injected on bus B reaches the sink only.
    can::Frame frame;
    frame.id = 0x18FF2040; // priority 6, kPgnValue, from 0x40
    frame.length = 8;
    frame.data[0] = 200;
    CHECK(buses.inject("B", at(100ms), frame) == can::SendResult::Accepted);
    buses.run_until(at(120ms));
    CHECK(std::find(recorder.seen.begin(), recorder.seen.end(), std::make_pair(100LL, 200)) != recorder.seen.end());
    for (const auto& r : onA.seen)
    {
        CHECK(r.second != 200);
    }
}

TEST_CASE(testkit_ecu_runs_on_step_and_every_in_the_order_added)
{
    LocalCan buses;
    auto& ecu = buses.add_ecu("ecu", 10ms);
    std::vector<std::string> calls;
    const auto record = [&calls](std::string what) {
        return [&calls, what = std::move(what)](SimTime now) { calls.push_back(what + " " + std::to_string(ms(now))); };
    };
    ecu.every(25ms, record("every25"));
    ecu.on_step(record("step"));
    buses.on_step("ecu", record("by_name"));
    ecu.every(20ms, record("every20"));
    buses.run_until(at(60ms));
    const std::vector<std::string> expected{
        "every25 0", "step 0",  "by_name 0",  "every20 0",              // 0
        "step 10",   "by_name 10",                                     // 10
        "step 20",   "by_name 20", "every20 20",                       // 20
        "every25 30", "step 30", "by_name 30",                         // 30: the first step at or after 25
        "step 40",   "by_name 40", "every20 40",                       // 40
        "every25 50", "step 50", "by_name 50",                         // 50
    };
    CHECK(calls == expected);
}

// The forms without a bus name (add_j1939(), inject(time, frame), injector(), bus()) use the only
// bus and make it, without a name, if there is none: the bus "" in the lookups by name. A lookup
// by name (bus(), injector(), inject() with the bus's name) names it; no second bus is made.
TEST_CASE(testkit_a_form_without_a_bus_name_makes_the_only_bus)
{
    {
        LocalCan buses;
        CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("J1939_CAN")); })); // no bus yet
        auto& legacy = buses.add_j1939("legacy", 10ms);
        CHECK(&buses.bus() == &buses.bus(""));
        CHECK(&buses.network("legacy") == &buses.network("legacy", ""));
        auto& bus = buses.bus("J1939_CAN"); // names it
        CHECK(&bus == &buses.bus());
        CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("")); }));
        CHECK(&buses.network("legacy", "J1939_CAN") == &legacy);
        CHECK_EQ(bus.endpoint_count(), 2u); // the injector, legacy
    }
    {
        LocalCan buses;
        CHECK(buses.inject(at(0ms), value_frame(1)) == can::SendResult::Accepted); // makes the bus
        auto& injector = buses.injector("J1939_CAN");                            // names it
        CHECK(&injector == &buses.injector());
        CHECK_EQ(buses.bus("J1939_CAN").endpoint_count(), 1u);
    }
    {
        LocalCan buses;
        auto& injector = buses.injector();
        CHECK(buses.inject("J1939_CAN", at(0ms), value_frame(1)) == can::SendResult::Accepted);
        CHECK(&injector == &buses.injector("J1939_CAN"));
    }
}

// The forms with and without a bus name reach the same bus in either order: an ECU's
// j1939(network) or can(network) names the bus without a name, and add_j1939() and inject(time,
// frame) use the one named bus. Both orders give the same buses and the same frames. A failed
// add_j1939() or j1939() makes and names no bus.
TEST_CASE(testkit_named_and_unnamed_forms_share_the_only_bus_in_either_order)
{
    struct Outcome
    {
        std::size_t endpoints = 0;
        std::vector<std::pair<long long, int>> seen;
    };
    const auto build = [](bool unnamed_first) {
        LocalCan buses;
        std::vector<std::pair<long long, int>> seen;
        const auto add_legacy = [&] {
            auto& node = buses.add_j1939("legacy", 10ms).add_node({.address = 0x20});
            node.on_message(kPgnValue, [&seen](const j1939::Message& m) { seen.emplace_back(ms(m.time), m.data[0]); });
        };
        const auto add_named = [&] {
            install_source(buses.add_ecu("engine", 10ms), "J1939_CAN", 0x40);
            buses.add_ecu("display", 10ms).can("J1939_CAN");
        };
        if (unnamed_first)
        {
            add_legacy();
            add_named();
        }
        else
        {
            add_named();
            add_legacy();
        }
        CHECK(&buses.bus() == &buses.bus("J1939_CAN"));
        CHECK(&buses.injector() == &buses.injector("J1939_CAN"));
        CHECK(&buses.network("legacy") == &buses.network("legacy", "J1939_CAN"));
        CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("")); }));
        CHECK(buses.inject(at(5ms), value_frame(200)) == can::SendResult::Accepted);
        buses.run_until(at(30ms));
        return Outcome{buses.bus("J1939_CAN").endpoint_count(), seen};
    };
    const Outcome unnamedFirst = build(true);
    const Outcome namedFirst = build(false);
    CHECK_EQ(unnamedFirst.endpoints, 4u); // the injector, legacy, engine, display
    CHECK_EQ(namedFirst.endpoints, unnamedFirst.endpoints);
    CHECK(std::any_of(unnamedFirst.seen.begin(), unnamedFirst.seen.end(), [](const auto& r) { return r.second == 200; }));
    CHECK(unnamedFirst.seen.size() > 1); // the injected frame and the engine's
    CHECK(namedFirst.seen == unnamedFirst.seen);

    j1939::NetworkOptions bad;
    bad.max_frames_per_step = 0;
    {
        LocalCan buses;
        CHECK(throws<std::invalid_argument>([&] { buses.add_j1939("half", 10ms, bad); }));
        CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.ecu("half")); }));
        CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("")); })); // not made
        buses.add_j1939("legacy", 10ms);
        auto& ecu = buses.add_ecu("ecu", 10ms);
        CHECK(throws<std::invalid_argument>([&] { ecu.j1939("A", bad); }));
        CHECK(throws<std::invalid_argument>([&] { ecu.can("A", can::RawChannelOptions{.send_capacity = 0}); }));
        CHECK_EQ(buses.bus("").endpoint_count(), 2u); // not named, nothing attached
        // legacy's network is on the bus j1939("A") would name.
        CHECK(throws<std::invalid_argument>([&] { buses.ecu("legacy").j1939("A"); }));
        CHECK(throws<std::invalid_argument>([&] { ecu.j1939(""); }));
        CHECK(throws<std::invalid_argument>([&] { ecu.can(""); }));
        CHECK_EQ(buses.bus("").endpoint_count(), 2u);
    }
}

// A second bus name makes a second bus, unless an ECU was added without a bus name before: which
// bus it is meant to be on is unclear, so std::logic_error names those ECUs, adds nothing and
// makes no bus.
TEST_CASE(testkit_a_second_bus_after_an_ecu_without_a_bus_name_is_an_error)
{
    {
        // inject() without a bus name added no ECU: a second bus is fine.
        LocalCan buses;
        CHECK(buses.inject(at(0ms), value_frame(1)) == can::SendResult::Accepted);
        auto& first = buses.bus();
        buses.add_ecu("engine", 10ms).j1939("A");
        buses.add_ecu("cab", 10ms).j1939("B");
        CHECK(&buses.bus("A") == &first);
        CHECK(&buses.bus("B") != &first);
    }
    for (const bool unnamedFirst : {true, false})
    {
        LocalCan buses;
        if (unnamedFirst)
        {
            buses.add_j1939("legacy", 10ms);
            buses.add_j1939("tool", 10ms);
            buses.add_ecu("engine", 10ms).j1939("A");
        }
        else
        {
            buses.add_ecu("engine", 10ms).j1939("A");
            buses.add_j1939("legacy", 10ms);
            buses.add_j1939("tool", 10ms);
        }
        auto& cab = buses.add_ecu("cab", 10ms);
        CHECK(throws_with<std::logic_error>([&] { cab.j1939("B"); }, "(legacy, tool)"));
        CHECK(throws_with<std::logic_error>([&] { cab.j1939("B"); }, "add_ecu(name, step).j1939(network)"));
        CHECK(throws_with<std::logic_error>([&] { cab.can("B"); }, "(legacy, tool)"));
        CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("B")); })); // nothing made
        CHECK(throws<std::logic_error>([&] { static_cast<void>(buses.network("cab")); }));   // none
        CHECK_EQ(buses.bus("A").endpoint_count(), 4u); // the injector, engine, legacy, tool
        cab.j1939("A");
        cab.can("A");
    }
}

// With two or more buses, every form without a bus name throws std::logic_error that lists the
// buses, and adds nothing.
TEST_CASE(testkit_forms_without_a_bus_name_need_a_single_bus)
{
    LocalCan buses;
    buses.add_ecu("engine", 10ms).j1939("A");
    buses.add_ecu("cab", 10ms).can("B");
    const std::string list = "(\"A\", \"B\")";
    CHECK(throws_with<std::logic_error>([&] { buses.add_j1939("tool", 10ms); }, list));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.ecu("tool")); }));
    CHECK(throws_with<std::logic_error>([&] { buses.inject(at(0ms), value_frame(1)); }, list));
    CHECK(throws_with<std::logic_error>([&] { static_cast<void>(buses.injector()); }, list));
    CHECK(throws_with<std::logic_error>([&] { static_cast<void>(buses.bus()); }, list));
    CHECK(throws_with<std::logic_error>([&] { static_cast<void>(buses.bus()); }, "bus(network)"));
    CHECK(buses.inject("A", at(0ms), value_frame(1)) == can::SendResult::Accepted);
    CHECK_EQ(buses.bus("A").endpoint_count(), 2u); // the injector, engine
}

TEST_CASE(testkit_ecu_lookups_and_setup_errors)
{
    LocalCan buses;
    auto& gateway = buses.add_ecu("gateway", 10ms);
    gateway.j1939("A");
    gateway.j1939("B");
    buses.add_ecu("empty", 10ms);
    CHECK(throws<std::invalid_argument>([&] { buses.add_ecu("gateway", 10ms); }));
    CHECK(throws<std::invalid_argument>([&] { buses.add_ecu("", 10ms); }));
    CHECK(throws<std::invalid_argument>([&] { buses.add_ecu("slow", 0ms); }));
    CHECK(throws<std::invalid_argument>([&] { gateway.j1939("A"); }));
    CHECK(throws<std::invalid_argument>([&] { gateway.j1939(""); }));
    CHECK(throws<std::invalid_argument>([&] { gateway.every(0ms, [](SimTime) {}); }));
    CHECK(throws<std::invalid_argument>([&] { gateway.every(10ms, {}); }));
    CHECK(throws<std::invalid_argument>([&] { gateway.on_step({}); }));
    // One-name lookups need exactly one network.
    CHECK(throws<std::logic_error>([&] { static_cast<void>(buses.network("gateway")); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(buses.endpoint("gateway")); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(buses.tap("gateway")); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(buses.network("empty")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.network("missing")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.network("gateway", "C")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.ecu("missing")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.host("missing")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("C")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.injector("C")); }));
    CHECK(throws<std::out_of_range>([&] { buses.inject("C", at(0ms), can::Frame{}); }));
    CHECK((buses.ecu_names() == std::vector<std::string>{"gateway", "empty"}));
    // add_j1939() checks the name like add_ecu().
    LocalCan unnamed;
    unnamed.add_j1939("tool", 10ms);
    CHECK(throws<std::invalid_argument>([&] { unnamed.add_j1939("tool", 10ms); }));
    CHECK(throws<std::invalid_argument>([&] { unnamed.add_j1939("", 10ms); }));
    CHECK(throws<std::invalid_argument>([&] { unnamed.add_j1939("slow", 0ms); }));
    CHECK((unnamed.ecu_names() == std::vector<std::string>{"tool"}));

    buses.run_for(20ms);
    const std::size_t endpoints = buses.bus("A").endpoint_count();
    CHECK(throws<std::logic_error>([&] { buses.add_ecu("late", 10ms); }));
    CHECK(throws<std::logic_error>([&] { buses.add_j1939("late", 10ms); }));
    CHECK(throws<std::logic_error>([&] { gateway.j1939("C"); }));
    CHECK(throws<std::logic_error>([&] { gateway.on_step([](SimTime) {}); }));
    CHECK(throws<std::logic_error>([&] { gateway.every(10ms, [](SimTime) {}); }));
    std::vector<std::string> log;
    CHECK(throws<std::logic_error>([&] { gateway.emplace<Probe>(log, "late"); }));
    CHECK(log.empty()); // no Probe was made
    CHECK_EQ(buses.bus("A").endpoint_count(), endpoints);
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("C")); }));
}

TEST_CASE(testkit_ecu_destroys_what_it_emplaced_after_its_networks)
{
    std::vector<std::string> log;
    {
        LocalCan buses;
        for (const std::string name : {"first", "second"})
        {
            auto& ecu = buses.add_ecu(name, 10ms);
            ecu.emplace<Probe>(log, name + " model");
            auto& node = ecu.j1939("A").add_node({.address = static_cast<j1939::Address>(name == "first" ? 0x10 : 0x11)});
            // The handler refers to the model; the network destroys the handler.
            auto handler = std::make_shared<Probe>(log, name + " handler");
            node.on_message(kPgnValue, [handler](const j1939::Message&) {});
            ecu.on_step([handler](SimTime) {});
            handler.reset();
            ecu.emplace<Probe>(log, name + " app");
        }
        buses.run_for(30ms);
        CHECK(log.empty());
    }
    // The ECU added last first; in each: the networks (the handler), then the emplaced objects,
    // the one emplaced last first.
    const std::vector<std::string> expected{"second handler", "second app", "second model",
                                            "first handler",  "first app",  "first model"};
    CHECK(log == expected);
}

namespace {

can::Frame standard(std::uint32_t id, std::uint8_t value)
{
    can::Frame f;
    f.id = id;
    f.format = can::IdFormat::Standard;
    f.length = 1;
    f.data[0] = value;
    return f;
}

} // namespace

// A raw CAN channel has an endpoint of its own; the lookups by network name are J1939 only, so
// it does not count in the one-name forms. The set-up rules.
TEST_CASE(testkit_ecu_raw_can_lookups_and_setup_errors)
{
    LocalCan buses;
    auto& mixed = buses.add_ecu("mixed", 10ms);
    mixed.j1939("A");
    auto& rawA = mixed.can("A");
    auto& rawB = mixed.can("B");
    CHECK(&rawA != &rawB);
    CHECK_EQ(buses.bus("A").endpoint_count(), 3u); // the injector, the J1939 network, the raw channel
    CHECK_EQ(buses.bus("B").endpoint_count(), 2u); // made by can("B")
    CHECK(&buses.network("mixed") == &buses.network("mixed", "A"));
    CHECK(&buses.endpoint("mixed") == &buses.endpoint("mixed", "A"));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.network("mixed", "B")); }));
    CHECK(throws<std::invalid_argument>([&] { mixed.can("A"); }));
    CHECK(throws<std::invalid_argument>([&] { mixed.can("C", can::RawChannelOptions{.send_capacity = 0}); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("C")); })); // nothing attached
    mixed.can("C");
    // No raw channel on the unnamed bus.
    auto& onDefault = buses.add_ecu("default", 10ms).can("D");
    CHECK(throws<std::invalid_argument>([&] { mixed.can(""); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(buses.bus()); }));

    buses.run_for(20ms);
    CHECK(throws<std::logic_error>([&] { mixed.can("D"); }));
    CHECK(throws<std::logic_error>([&] { onDefault.on_frame([](const can::TimedFrame&) {}); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(rawA.send(standard(0x1, 0))); }));
    CHECK_EQ(buses.bus("D").endpoint_count(), 2u);
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.bus("E")); }));
}

// raw_channel(ecu, network) finds an ECU's raw channel by the network's name, like network()
// finds a J1939 network; std::out_of_range for an unknown ECU or network.
TEST_CASE(testkit_ecu_raw_channel_lookup)
{
    LocalCan buses;
    auto& mixed = buses.add_ecu("mixed", 10ms);
    mixed.j1939("A");
    auto& rawA = mixed.can("A");
    auto& rawB = mixed.can("B");
    auto& other = buses.add_ecu("other", 10ms).can("A");
    CHECK(&buses.raw_channel("mixed", "A") == &rawA);
    CHECK(&buses.raw_channel("mixed", "B") == &rawB);
    CHECK(&buses.raw_channel("other", "A") == &other);
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.raw_channel("mixed", "C")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.raw_channel("missing", "A")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(buses.raw_channel("other", "B")); }));
}

// An ECU with a J1939 network and a raw channel on the same bus: each receives the other's
// frames (in the step after the one that sent them); a raw handler may send on the J1939 network
// in the same step, and the other way round.
TEST_CASE(testkit_ecu_raw_can_beside_j1939_on_one_bus)
{
    LocalCan buses;
    auto& mixed = buses.add_ecu("mixed", 10ms);
    auto& node = mixed.j1939("A").add_node({.address = 0x10});
    auto& raw = mixed.can("A");
    std::vector<std::tuple<long long, std::uint32_t, bool>> rawSeen; // (time, id, extended)
    raw.on_frame([&](const can::TimedFrame& f) {
        rawSeen.emplace_back(ms(f.time), f.frame.id, f.frame.format == can::IdFormat::Extended);
        if (f.frame.id == 0x100) // a raw request answered with a J1939 message in this step
        {
            const std::uint8_t data[8]{f.frame.data[0], 0, 0, 0, 0, 0, 0, 0};
            CHECK(node.send(kPgnValue, data) == j1939::SendStatus::Ok);
        }
    });
    node.on_message(kPgnValue, [&](const j1939::Message& m) { // from the other ECU: answered raw
        CHECK(raw.send(standard(0x200, m.data[0])) == can::RawSendStatus::Ok);
    });

    auto& other = buses.add_ecu("other", 5ms);
    auto& otherRaw = other.can("A");
    Recorder otherJ1939(other, "A", 0x30);
    auto& otherNode = buses.network("other").node(0); // the raw channel does not count
    std::vector<std::tuple<long long, std::uint32_t, int>> otherSeen;
    otherRaw.on_frame(
        [&](const can::TimedFrame& f) { otherSeen.emplace_back(ms(f.time), f.frame.id, f.frame.data[0]); });
    other.on_step([&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(otherRaw.send(standard(0x100, 7)) == can::RawSendStatus::Ok);
        }
        if (now == at(5ms))
        {
            const std::uint8_t data[8]{9, 0, 0, 0, 0, 0, 0, 0};
            CHECK(otherNode.send(kPgnValue, data) == j1939::SendStatus::Ok);
        }
    });
    buses.run_for(40ms);

    // mixed's step at 10 ms: the raw request (time 0) and the J1939 message (time 5); it answers
    // both in that step. Its raw channel also gets the other's J1939 frame, and in its step at
    // 20 ms the frame its own J1939 network sent at 10 ms.
    const std::uint32_t idFromOther = 0x18FF2030u;
    const std::uint32_t idFromMixed = 0x18FF2010u;
    CHECK((rawSeen == std::vector<std::tuple<long long, std::uint32_t, bool>>{
               {0, 0x100, false}, {5, idFromOther, true}, {10, idFromMixed, true}}));
    CHECK((otherJ1939.seen == std::vector<std::pair<long long, int>>{{10, 7}}));
    // The other ECU's raw channel gets its own J1939 frame too, and the two frames of mixed's step
    // at 10 ms in the order mixed added its J1939 network and raw channel.
    CHECK((otherSeen ==
           std::vector<std::tuple<long long, std::uint32_t, int>>{{5, idFromOther, 9}, {10, idFromMixed, 7}, {10, 0x200, 9}}));
    // The J1939 network ignores the raw 11-bit frames of its neighbour (0x200 at 10 ms, received
    // at 20 ms) as it does other ECUs' (0x100).
    CHECK_EQ(buses.network("mixed").statistics().frames_ignored, 2u);
}

// The raw channels go after the networks and before the emplaced objects.
TEST_CASE(testkit_ecu_destroys_its_raw_channels_after_its_networks)
{
    std::vector<std::string> log;
    {
        LocalCan buses;
        auto& ecu = buses.add_ecu("ecu", 10ms);
        ecu.emplace<Probe>(log, "model");
        auto& raw = ecu.can("A");
        auto rawHandler = std::make_shared<Probe>(log, "raw handler");
        raw.on_frame([rawHandler](const can::TimedFrame&) {});
        rawHandler.reset();
        auto& node = ecu.j1939("A").add_node({.address = 0x10});
        auto handler = std::make_shared<Probe>(log, "handler");
        node.on_message(kPgnValue, [handler](const j1939::Message&) {});
        handler.reset();
        ecu.every(10ms, [&raw](SimTime now) {
            static_cast<void>(raw.send(standard(0x1, static_cast<std::uint8_t>(ms(now)))));
        });
        buses.run_for(30ms);
        CHECK(log.empty());
    }
    CHECK((log == std::vector<std::string>{"handler", "raw handler", "model"}));
}
