// SPDX-License-Identifier: MIT
//
// vehicle_sil::Simulation: the same ECUs (classes that take an Ecu&) on the local virtual buses and
// on SIL Kit with a registry of this process give the same records (also of Ecu::at()); a failed
// ECU ends the run with SimulationError on both; the rules of a run; a controller elsewhere; a
// participant made by the caller; a gateway ECU on two networks; raw CAN channels (Ecu::can()),
// also beside a J1939 network on the same network; the same input refused on both; an application
// whose constructor throws; the failures of the SIL Kit run (an ECU that ends it, another
// participant that fails, a start that times out); ECUs added through local().
#include "test_harness.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/peer/peer.hpp"
#include "vehicle_sil/simulation/simulation.hpp"
#include "vehicle_sil/testkit/local_can.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::Ecu;
using vehicle_sil::Peer;
using vehicle_sil::PeerOptions;
using vehicle_sil::SilKitOptions;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::Simulation;
using vehicle_sil::SimulationError;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;
namespace j1939 = vehicle_sil::j1939;
namespace orch = SilKit::Services::Orchestration;

namespace {

constexpr j1939::Pgn kPgnValue{0xFF20}; ///< proprietary B: a counter

SimTime at(std::chrono::nanoseconds t) { return SimTime{} + t; }
long long ms(SimTime t) { return (t - SimTime{}) / 1ms; }

using Records = std::vector<std::pair<long long, int>>;

/// Sends kPgnValue with a counter every `period` on the network `network`.
class Sender
{
public:
    explicit Sender(Ecu& ecu, const std::string& network = "CAN", SimDuration period = 20ms) :
      node_(ecu.j1939(network).add_node({.address = 0x10}))
    {
        ecu.every(period, [this](SimTime) {
            const std::uint8_t data[8]{counter_++, 0, 0, 0, 0, 0, 0, 0};
            static_cast<void>(node_.send(kPgnValue, data));
        });
    }

private:
    j1939::Node& node_;
    std::uint8_t counter_ = 0;
};

/// Records (time, value) of every kPgnValue it receives on the network `network`.
class Receiver
{
public:
    explicit Receiver(Ecu& ecu, const std::string& network = "CAN")
    {
        auto& node = ecu.j1939(network).add_node({.address = 0x30});
        node.on_message(kPgnValue, [this](const j1939::Message& m) { seen.emplace_back(ms(m.time), m.data[0]); });
    }
    Records seen;
};

/// Receives kPgnValue on network "A" and sends it on network "B" from the same step.
class Gateway
{
public:
    explicit Gateway(Ecu& ecu) : out_(ecu.j1939("B").add_node({.address = 0x21}))
    {
        auto& in = ecu.j1939("A").add_node({.address = 0x20});
        in.on_message(kPgnValue, [this](const j1939::Message& m) {
            const std::uint8_t data[8]{static_cast<std::uint8_t>(m.data[0] + 100), 0, 0, 0, 0, 0, 0, 0};
            static_cast<void>(out_.send(kPgnValue, data));
        });
    }

private:
    j1939::Node& out_;
};

/// Throws from its step at `when`.
class Failing
{
public:
    Failing(Ecu& ecu, SimTime when)
    {
        ecu.on_step([when](SimTime now) {
            if (now == when)
            {
                throw std::runtime_error("the model diverged");
            }
        });
    }
};

/// The records before `end` (a run without an end of its own runs a little further).
Records before(const Records& records, long long end)
{
    Records out;
    for (const auto& r : records)
    {
        if (r.first < end)
        {
            out.push_back(r);
        }
    }
    return out;
}

Records sender_and_receiver(Simulation& sim)
{
    sim.add<Sender>("Sender", 10ms);
    auto& receiver = sim.add<Receiver>("Receiver", 5ms);
    sim.run_for(500ms);
    return receiver.seen;
}

Records local_reference()
{
    Simulation sim;
    return sender_and_receiver(sim);
}

} // namespace

// 1, 2: the same ECUs, owned by the simulation (no model declared outside), on the local
// virtual bus and on SIL Kit with a registry of this process.
TEST_CASE(simulation_runs_the_same_ecus_locally_and_on_silkit)
{
    Simulation local;
    CHECK(local.is_local());
    const Records onLocal = sender_and_receiver(local);
    CHECK(local.now() == at(500ms));

    Simulation silkit(SilKitOptions{});
    CHECK(!silkit.is_local());
    CHECK(silkit.registry_uri().rfind("silkit://", 0) == 0);
    const Records onSilKit = sender_and_receiver(silkit);
    CHECK(silkit.now() == at(500ms));
    CHECK(!silkit.peer("Sender").failed());

    // Sent every 20 ms from 0 until 480 ms (the last step before the end that reaches the
    // receiver); no step at or after the end on SIL Kit either.
    CHECK_EQ(onLocal.size(), 25u);
    CHECK(onLocal.front() == std::make_pair(0LL, 0));
    CHECK(onLocal.back() == std::make_pair(480LL, 24));
    CHECK(onSilKit == onLocal);
}

namespace {

/// Records (time, which) of its every() and at() functions. With 7 ms steps no time below but 0
/// is on the step grid.
class Script
{
public:
    explicit Script(Ecu& ecu)
    {
        ecu.at(at(100ms), [this](SimTime now) { calls.emplace_back(ms(now), 1); });
        ecu.every(30ms, [this](SimTime now) { calls.emplace_back(ms(now), 0); });
        ecu.at(at(100ms), [this](SimTime now) { calls.emplace_back(ms(now), 2); });
        ecu.at(at(0ms), [this](SimTime now) { calls.emplace_back(ms(now), 3); });
    }
    Records calls;
};

Records script(Simulation& sim)
{
    auto& s = sim.add<Script>("Script", 7ms);
    sim.run_for(140ms);
    return s.calls;
}

} // namespace

TEST_CASE(simulation_runs_at_once_alike_locally_and_on_silkit)
{
    Simulation local;
    const Records onLocal = script(local);
    Simulation silkit(SilKitOptions{});
    const Records onSilKit = script(silkit);
    // Steps 0, 7, 14, ...: every(30 ms) at 0, 35, 63, 91, 126; both at(100 ms) at 105; in each
    // step in the order added (at 0, every() before at(0 ms)).
    const Records expected{{0, 0}, {0, 3}, {35, 0}, {63, 0}, {91, 0}, {105, 1}, {105, 2}, {126, 0}};
    CHECK(onLocal == expected);
    CHECK(onSilKit == onLocal);
}

// 3: a step that throws ends the run with SimulationError naming the ECU, on both; on SIL Kit
// within seconds (the failed participant stalls virtual time; the run must see it).
TEST_CASE(simulation_reports_a_failed_ecu)
{
    {
        Simulation sim;
        sim.add<Sender>("Sender", 10ms);
        sim.add<Failing>("Faulty", 10ms, at(50ms));
        CHECK(throws_with<SimulationError>([&] { sim.run_for(200ms); }, "Faulty: the model diverged"));
        CHECK(throws_with<SimulationError>([&] { sim.run_for(200ms); }, "Faulty: the model diverged"));
        CHECK(sim.local().host("Faulty").failed());
        CHECK(!sim.local().host("Sender").failed());
    }
    {
        Simulation sim(SilKitOptions{});
        sim.add<Sender>("Sender", 10ms);
        sim.add<Failing>("Faulty", 10ms, at(50ms));
        const auto start = std::chrono::steady_clock::now();
        std::string what;
        try
        {
            sim.run_for(10s);
        }
        catch (const SimulationError& e)
        {
            what = e.what();
        }
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(what.find("Faulty: ") == 0);
        CHECK(what.find("the model diverged") != std::string::npos);
        CHECK(what.find("Sender") == std::string::npos);
        CHECK(took < 20s);
        CHECK(sim.now() == SimTime{});
        CHECK(sim.peer("Faulty").failed());
        CHECK(!sim.peer("Sender").failed());
    }
}

// 4: the rules of a run.
TEST_CASE(simulation_rules)
{
    {
        Simulation sim(SilKitOptions{});
        auto& ecu = sim.add_ecu("A", 10ms);
        std::vector<long long> steps;
        ecu.on_step([&steps](SimTime now) { steps.push_back(ms(now)); });
        CHECK(&sim.peer("A") == &ecu);
        CHECK(throws<std::invalid_argument>([&] { sim.add_ecu("A", 10ms); }));
        CHECK(throws<std::invalid_argument>([&] { sim.add_ecu("", 10ms); }));
        CHECK(throws<std::invalid_argument>([&] { sim.add_ecu("SimulationController", 10ms); }));
        CHECK(throws<std::invalid_argument>([&] { sim.add_ecu("B", 0ms); }));
        CHECK(throws<std::logic_error>([&] { static_cast<void>(sim.local()); }));
        CHECK(throws<std::out_of_range>([&] { static_cast<void>(sim.peer("missing")); }));
        CHECK(throws<std::invalid_argument>([&] { sim.run_for(0ms); }));
        sim.run_for(50ms);
        CHECK((steps == std::vector<long long>{0, 10, 20, 30, 40}));
        CHECK(throws<std::logic_error>([&] { sim.run_for(50ms); }));
        CHECK(throws<std::logic_error>([&] { sim.run(); }));
        CHECK(throws<std::logic_error>([&] { sim.add_ecu("B", 10ms); }));
        CHECK(throws<std::logic_error>([&] { ecu.on_step([](SimTime) {}); }));
        CHECK(throws<std::logic_error>([&] { ecu.at(at(100ms), [](SimTime) {}); }));
    }
    {
        Simulation sim;
        auto& ecu = sim.add_ecu("A", 10ms);
        std::vector<long long> steps;
        ecu.on_step([&steps](SimTime now) { steps.push_back(ms(now)); });
        CHECK(throws<std::invalid_argument>([&] { sim.add_ecu("A", 10ms); }));
        CHECK(throws<std::logic_error>([&] { sim.run(); }));
        CHECK(throws<std::logic_error>([&] { static_cast<void>(sim.peer("A")); }));
        CHECK(throws<std::logic_error>([&] { static_cast<void>(sim.registry_uri()); }));
        CHECK(&sim.local().ecu("A") == &ecu);
        sim.run_for(30ms);
        sim.run_for(30ms); // continues
        CHECK((steps == std::vector<long long>{0, 10, 20, 30, 40, 50}));
        CHECK(sim.now() == at(60ms));
        CHECK(throws<std::logic_error>([&] { sim.add_ecu("B", 10ms); }));
    }
    CHECK(throws<std::invalid_argument>([] { Simulation sim(SilKitOptions{.controller = false, .other_participants = {"X"}}); }));
    CHECK(throws<std::invalid_argument>([] { Simulation sim(SilKitOptions{.other_participants = {"X", "X"}}); }));
    {
        Simulation sim(SilKitOptions{.other_participants = {"X"}});
        CHECK(throws<std::invalid_argument>([&] { sim.add_ecu("X", 10ms); }));
    }
    {
        Simulation sim(SilKitOptions{.controller = false});
        sim.add_ecu("A", 10ms);
        CHECK(throws<std::logic_error>([&] { sim.run_for(50ms); }));
    }
}

// 5: without a controller of its own, run() runs until the controller elsewhere ends the
// simulation.
TEST_CASE(simulation_runs_under_a_controller_elsewhere)
{
    const Records reference = local_reference();
    Simulation sim(SilKitOptions{.controller = false});
    sim.add<Sender>("Sender", 10ms);
    auto& receiver = sim.add<Receiver>("Receiver", 5ms);
    vehicle_sil::SimulationControllerOptions c;
    c.registry_uri = sim.registry_uri();
    c.required_participants = {"Sender", "Receiver"};
    c.stop_at = 500ms;
    vehicle_sil::SimulationController controller(c);
    std::exception_ptr error;
    std::optional<orch::ParticipantState> end;
    {
        std::jthread control([&] {
            try
            {
                controller.start(30s);
                end = controller.wait_until_finished(60s);
            }
            catch (...)
            {
                error = std::current_exception();
            }
        });
        sim.run();
    }
    CHECK(!error);
    CHECK(end == orch::ParticipantState::Shutdown);
    CHECK(before(receiver.seen, 500) == reference);
    CHECK(sim.now() == SimTime{});
}

// 6: a Coordinated participant made by the caller (here a Peer in this process, with the same
// Receiver on it) runs in the simulation of this process's controller.
TEST_CASE(simulation_waits_for_other_participants)
{
    const Records reference = local_reference();
    Simulation sim(SilKitOptions{.other_participants = {"Receiver"}});
    sim.add<Sender>("Sender", 10ms);
    PeerOptions o;
    o.participant = "Receiver";
    o.registry_uri = sim.registry_uri();
    o.lifecycle = vehicle_sil::LifecycleMode::Coordinated;
    o.virtual_time = vehicle_sil::VirtualTimeOptions{5ms, std::nullopt, std::nullopt};
    Peer other(o);
    auto& receiver = other.emplace<Receiver>(other);
    std::exception_ptr error;
    std::optional<orch::ParticipantState> end;
    {
        std::jthread participant([&] {
            try
            {
                other.start(30s);
                end = other.wait(60s);
            }
            catch (...)
            {
                error = std::current_exception();
            }
        });
        sim.run_for(500ms);
    }
    other.stop();
    CHECK(!error);
    CHECK(end == orch::ParticipantState::Shutdown);
    CHECK(!other.failed());
    CHECK(before(receiver.seen, 500) == reference);
}

// 7: a gateway ECU with networks on two buses gives the same results on both.
TEST_CASE(simulation_runs_a_gateway_ecu_on_two_networks)
{
    const auto run = [](Simulation& sim) {
        sim.add<Sender>("Sender", 10ms, "A", 20ms);
        sim.add<Gateway>("Gateway", 10ms);
        auto& onB = sim.add<Receiver>("ReceiverB", 5ms, "B");
        auto& onA = sim.add<Receiver>("ReceiverA", 5ms, "A");
        sim.run_for(300ms);
        return std::make_pair(onA.seen, onB.seen);
    };
    Simulation local;
    const auto onLocal = run(local);
    Simulation silkit(SilKitOptions{});
    const auto onSilKit = run(silkit);
    // B gets each value 100 higher, sent by the gateway in its step after the sender's.
    CHECK_EQ(onLocal.first.size(), 15u);
    CHECK_EQ(onLocal.second.size(), 15u);
    CHECK(onLocal.second.front() == std::make_pair(10LL, 100));
    CHECK(onSilKit == onLocal);
}

namespace {

namespace can = vehicle_sil::can;

constexpr std::uint32_t kRequestId = 0x100; ///< 11 bit, a protocol of one's own
constexpr std::uint32_t kReplyId = 0x101;

can::Frame standard(std::uint32_t id, std::uint8_t value)
{
    can::Frame f;
    f.id = id;
    f.format = can::IdFormat::Standard;
    f.length = 1;
    f.data[0] = value;
    return f;
}

bool is_standard(const can::Frame& frame, std::uint32_t id)
{
    return frame.format == can::IdFormat::Standard && frame.id == id;
}

/// Sends a request with a counter every 50 ms on the raw channel of "CAN" and records (time,
/// value) of the replies.
class RawClient
{
public:
    explicit RawClient(Ecu& ecu) : raw_(ecu.can("CAN"))
    {
        raw_.on_frame([this](const can::TimedFrame& f) {
            if (is_standard(f.frame, kReplyId))
            {
                replies.emplace_back(ms(f.time), f.frame.data[0]);
            }
        });
        ecu.every(50ms, [this](SimTime) { results.push_back(raw_.send(standard(kRequestId, counter_++))); });
    }
    Records replies;
    std::vector<can::RawSendStatus> results;

private:
    can::RawChannel& raw_;
    std::uint8_t counter_ = 0;
};

/// Answers the request it received last with its value + 1, from its every(10 ms).
class RawServer
{
public:
    explicit RawServer(Ecu& ecu) : raw_(ecu.can("CAN"))
    {
        raw_.on_frame([this](const can::TimedFrame& f) {
            if (is_standard(f.frame, kRequestId))
            {
                pending_ = f.frame.data[0];
            }
        });
        ecu.every(10ms, [this](SimTime) {
            if (pending_)
            {
                static_cast<void>(raw_.send(standard(kReplyId, static_cast<std::uint8_t>(*pending_ + 1))));
                pending_.reset();
            }
        });
    }

private:
    can::RawChannel& raw_;
    std::optional<std::uint8_t> pending_;
};

using RawRecords = std::vector<std::tuple<long long, std::uint32_t, int>>; ///< (time, id, first byte)

/// A J1939 node (0x10) and a raw channel on the same network "CAN": a raw request (0x100) is
/// answered with kPgnValue, a kPgnValue with a raw 0x200. Records what the raw channel receives.
class Mixed
{
public:
    explicit Mixed(Ecu& ecu) : network(ecu.j1939("CAN")), node_(network.add_node({.address = 0x10})), raw_(ecu.can("CAN"))
    {
        raw_.on_frame([this](const can::TimedFrame& f) {
            raw_seen.emplace_back(ms(f.time), f.frame.id, f.frame.data[0]);
            if (is_standard(f.frame, kRequestId))
            {
                const std::uint8_t data[8]{f.frame.data[0], 0, 0, 0, 0, 0, 0, 0};
                CHECK(node_.send(kPgnValue, data) == j1939::SendStatus::Ok);
            }
        });
        node_.on_message(kPgnValue, [this](const j1939::Message& m) {
            CHECK(raw_.send(standard(0x200, m.data[0])) == can::RawSendStatus::Ok);
        });
    }
    j1939::Network& network;
    RawRecords raw_seen;

private:
    j1939::Node& node_;
    can::RawChannel& raw_;
};

/// A raw channel and then a J1939 node (0x30) on "CAN": sends the raw request at 0 ms and
/// kPgnValue at 5 ms; records what both receive.
class Other
{
public:
    explicit Other(Ecu& ecu) : raw_(ecu.can("CAN")), node_(ecu.j1939("CAN").add_node({.address = 0x30}))
    {
        raw_.on_frame([this](const can::TimedFrame& f) { raw_seen.emplace_back(ms(f.time), f.frame.id, f.frame.data[0]); });
        node_.on_message(kPgnValue, [this](const j1939::Message& m) { j1939_seen.emplace_back(ms(m.time), m.data[0]); });
        ecu.on_step([this](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(raw_.send(standard(kRequestId, 7)) == can::RawSendStatus::Ok);
            }
            if (now == at(5ms))
            {
                const std::uint8_t data[8]{9, 0, 0, 0, 0, 0, 0, 0};
                CHECK(node_.send(kPgnValue, data) == j1939::SendStatus::Ok);
            }
        });
    }
    RawRecords raw_seen;
    Records j1939_seen;

private:
    can::RawChannel& raw_;
    j1939::Node& node_;
};

} // namespace

// 8: two ECUs with a request/response of their own over raw 11-bit frames give the same record
// on the local virtual bus and on SIL Kit.
TEST_CASE(simulation_runs_raw_can_ecus_locally_and_on_silkit)
{
    const auto run = [](Simulation& sim) {
        auto& client = sim.add<RawClient>("Client", 10ms);
        sim.add<RawServer>("Server", 5ms);
        sim.run_for(300ms);
        CHECK_EQ(client.results.size(), 6u);
        CHECK(std::all_of(client.results.begin(), client.results.end(),
                          [](can::RawSendStatus s) { return s == can::RawSendStatus::Ok; }));
        return client.replies;
    };
    Simulation local;
    const Records onLocal = run(local);
    Simulation silkit(SilKitOptions{});
    const Records onSilKit = run(silkit);
    // A request at t (0, 50, ..., 250) reaches the server in its step at t + 5, which answers in
    // its every() at t + 10; the client gets the reply in its step at t + 20.
    CHECK((onLocal == Records{{10, 1}, {60, 2}, {110, 3}, {160, 4}, {210, 5}, {260, 6}}));
    CHECK(onSilKit == onLocal);
}

// 9: an ECU with a J1939 network and a raw channel on the same network: each receives the
// other's frames in the step after they were sent, the same on both backends; a handler of one
// sends on the other in the same step.
TEST_CASE(simulation_runs_raw_can_beside_j1939_on_one_network)
{
    const auto run = [](Simulation& sim) {
        auto& mixed = sim.add<Mixed>("Mixed", 10ms);
        auto& other = sim.add<Other>("Other", 5ms);
        sim.run_for(40ms);
        // The 11-bit frames: the request (0 ms) and the neighbour's 0x200 (10 ms).
        CHECK_EQ(mixed.network.statistics().frames_ignored, 2u);
        return std::make_tuple(mixed.raw_seen, other.raw_seen, other.j1939_seen);
    };
    Simulation local;
    const auto onLocal = run(local);
    Simulation silkit(SilKitOptions{});
    const auto onSilKit = run(silkit);
    constexpr std::uint32_t kFromOther = 0x18FF2030u;
    constexpr std::uint32_t kFromMixed = 0x18FF2010u;
    // Mixed (steps of 10 ms) handles the request (0 ms) and the J1939 message (5 ms) in its step
    // at 10 ms and answers both there; its raw channel gets its own J1939 frame of that step in
    // the next one.
    CHECK((std::get<0>(onLocal) == RawRecords{{0, kRequestId, 7}, {5, kFromOther, 9}, {10, kFromMixed, 7}}));
    // Other's raw channel gets its own J1939 frame too, and Mixed's two frames of its step at
    // 10 ms in the order Mixed added its J1939 network and raw channel.
    CHECK((std::get<1>(onLocal) == RawRecords{{5, kFromOther, 9}, {10, kFromMixed, 7}, {10, 0x200, 9}}));
    CHECK((std::get<2>(onLocal) == Records{{10, 7}}));
    CHECK(onSilKit == onLocal);
}

// 10: the rules of can() on both backends.
TEST_CASE(simulation_raw_can_rules)
{
    for (const bool onSilKit : {false, true})
    {
        auto sim = onSilKit ? std::make_unique<Simulation>(SilKitOptions{}) : std::make_unique<Simulation>();
        auto& ecu = sim->add_ecu("A", 10ms);
        auto& raw = ecu.can("CAN");
        ecu.j1939("CAN"); // a J1939 network beside it is allowed
        CHECK(throws<std::invalid_argument>([&] { ecu.can("CAN"); }));
        CHECK(throws<std::invalid_argument>([&] { ecu.can("X", can::RawChannelOptions{.send_capacity = 0}); }));
        ecu.can("X"); // the failed can() kept nothing
        CHECK(throws<std::invalid_argument>([&] { ecu.can(""); }));
        CHECK(throws<std::invalid_argument>([&] { ecu.j1939(""); }));
        int steps = 0;
        ecu.on_step([&](SimTime) {
            ++steps;
            CHECK(raw.send(standard(0x1, 0)) == can::RawSendStatus::Ok);
        });
        sim->run_for(30ms);
        CHECK_EQ(steps, 3);
        CHECK_EQ(raw.statistics().frames_sent, 3u);
        CHECK(throws<std::logic_error>([&] { ecu.can("Y"); }));
        CHECK(throws<std::logic_error>([&] { raw.on_frame([](const can::TimedFrame&) {}); }));
        CHECK(throws<std::logic_error>([&] { static_cast<void>(raw.send(standard(0x1, 0))); }));
    }
}

namespace {

/// Counts how often it was made.
struct Made
{
    explicit Made(int& count) { ++count; }
};

/// Sets up a handler that refers to itself, then throws from its constructor (a second
/// j1939("CAN") is refused).
class Broken
{
public:
    explicit Broken(Ecu& ecu)
    {
        ecu.j1939("CAN").add_node({.address = 0x40}).on_message(kPgnValue, [this](const j1939::Message&) { ++seen_; });
        ecu.j1939("CAN"); // std::invalid_argument
    }

private:
    int seen_ = 0;
};

} // namespace

// 11: both backends refuse the same input: a run without an ECU, the controller's name for an
// ECU, an empty network name; emplace() after a run makes nothing.
TEST_CASE(simulation_backends_refuse_the_same_input)
{
    for (const bool onSilKit : {false, true})
    {
        auto sim = onSilKit ? std::make_unique<Simulation>(SilKitOptions{}) : std::make_unique<Simulation>();
        CHECK(throws_with<std::logic_error>([&] { sim->run_for(50ms); }, "without an ECU"));
        if (onSilKit)
        {
            CHECK(throws_with<std::logic_error>([&] { sim->run(); }, "without an ECU"));
        }
        CHECK(throws<std::invalid_argument>([&] { sim->add_ecu("SimulationController", 10ms); }));
        auto& ecu = sim->add_ecu("A", 10ms);
        CHECK(throws<std::invalid_argument>([&] { ecu.j1939(""); }));
        CHECK(throws<std::invalid_argument>([&] { ecu.can(""); }));
        int steps = 0;
        ecu.on_step([&steps](SimTime) { ++steps; });
        sim->run_for(30ms); // the refused run_for() did not count as the run
        CHECK_EQ(steps, 3);
        int made = 0;
        CHECK(throws<std::logic_error>([&] { ecu.emplace<Made>(made); }));
        CHECK_EQ(made, 0);
    }
    Simulation sim(SilKitOptions{.controller = false});
    CHECK(throws<std::invalid_argument>([&] { sim.add_ecu("SimulationController", 10ms); }));
    CHECK(throws_with<std::logic_error>([&] { sim.run(); }, "without an ECU"));
}

// 12: an application whose constructor throws: its exception propagates, and the Simulation
// cannot be used any more (what the constructor set up refers to the destroyed application).
TEST_CASE(simulation_is_unusable_after_an_application_threw)
{
    for (const bool onSilKit : {false, true})
    {
        auto sim = onSilKit ? std::make_unique<Simulation>(SilKitOptions{}) : std::make_unique<Simulation>();
        sim->add<Sender>("Sender", 10ms);
        CHECK(throws<std::invalid_argument>([&] { sim->add<Broken>("Broken", 10ms); }));
        CHECK(throws_with<std::logic_error>([&] { sim->add_ecu("Next", 10ms); }, "ECU Broken"));
        CHECK(throws_with<std::logic_error>([&] { sim->add<Receiver>("Next", 10ms); }, "ECU Broken"));
        CHECK(throws_with<std::logic_error>([&] { sim->run_for(50ms); }, "ECU Broken"));
        CHECK(throws_with<std::logic_error>([&] { sim->run(); }, "ECU Broken"));
    }
}

// 13: an ECU that ends the run of this process's controller with request_stop() is no failure.
TEST_CASE(simulation_run_ends_when_an_ecu_stops_it)
{
    Simulation sim(SilKitOptions{});
    auto& receiver = sim.add<Receiver>("Receiver", 5ms);
    auto& ecu = sim.add_ecu("Stopper", 10ms);
    sim.add<Sender>("Sender", 10ms);
    Peer& stopper = sim.peer("Stopper");
    std::vector<long long> steps;
    ecu.on_step([&steps, &stopper](SimTime now) {
        steps.push_back(ms(now));
        if (now == at(100ms))
        {
            stopper.request_stop("done");
        }
    });
    sim.run();
    CHECK(steps.size() >= 11u);
    CHECK((std::vector<long long>(steps.begin(), steps.begin() + 11) ==
           std::vector<long long>{0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}));
    CHECK(before(receiver.seen, 100) == before(local_reference(), 100));
    CHECK(!stopper.failed());
    CHECK(sim.now() == SimTime{});
}

// 14: a participant of another process (here a Peer made by the test) that fails ends the run
// with SimulationError naming it, within seconds.
TEST_CASE(simulation_reports_a_failed_other_participant)
{
    Simulation sim(SilKitOptions{.other_participants = {"Other"}});
    sim.add<Sender>("Sender", 10ms);
    PeerOptions o;
    o.participant = "Other";
    o.registry_uri = sim.registry_uri();
    o.lifecycle = vehicle_sil::LifecycleMode::Coordinated;
    o.virtual_time = vehicle_sil::VirtualTimeOptions{10ms, std::nullopt, std::nullopt};
    Peer other(o);
    other.emplace<Failing>(other, at(50ms));
    std::exception_ptr error;
    std::string what;
    const auto start = std::chrono::steady_clock::now();
    {
        std::jthread participant([&] {
            try
            {
                other.start(30s);
            }
            catch (...)
            {
                error = std::current_exception();
            }
        });
        try
        {
            sim.run_for(10s);
        }
        catch (const SimulationError& e)
        {
            what = e.what();
        }
    }
    const auto took = std::chrono::steady_clock::now() - start;
    other.stop();
    CHECK(!error);
    CHECK(what.find("Other: left the simulation") == 0);
    CHECK(what.find("Sender") == std::string::npos);
    CHECK(took < 20s);
    CHECK(other.failed());
    CHECK(!sim.peer("Sender").failed());
}

// 15: a start that times out (a required participant never came): everything is stopped, then
// the start's exception propagates (not SimulationError); the simulation does not run again.
TEST_CASE(simulation_stops_everything_when_the_start_times_out)
{
    Simulation sim(SilKitOptions{.other_participants = {"Missing"}, .start_timeout = 500ms});
    auto& ecu = sim.add_ecu("A", 10ms);
    int steps = 0;
    ecu.on_step([&steps](SimTime) { ++steps; });
    bool simulationError = false;
    std::string what;
    try
    {
        sim.run_for(1s);
    }
    catch (const SimulationError&)
    {
        simulationError = true;
    }
    catch (const std::runtime_error& e)
    {
        what = e.what();
    }
    CHECK(!simulationError);
    CHECK(!what.empty());
    CHECK(sim.peer("A").runtime().stop_requested()); // stopped before the exception came out
    CHECK_EQ(steps, 0);
    CHECK(throws<std::logic_error>([&] { sim.run_for(1s); }));
}

// 16: on local buses, a failed ECU added through local() is a SimulationError too; local()
// reaches the ECUs' bus by its name.
TEST_CASE(simulation_local_covers_the_ecus_of_local)
{
    {
        Simulation sim;
        sim.add<Sender>("Sender", 10ms);
        auto& tester = sim.local().add_ecu("Tester", 10ms);
        tester.emplace<Failing>(tester, at(30ms));
        CHECK(throws_with<SimulationError>([&] { sim.run_for(100ms); }, "Tester: the model diverged"));
        CHECK(throws_with<SimulationError>([&] { sim.run_for(100ms); }, "Tester: the model diverged"));
    }
    {
        Simulation sim;
        auto& receiver = sim.add<Receiver>("Receiver", 10ms);
        can::Frame frame;
        frame.id = 0x18FF2040; // priority 6, kPgnValue, from 0x40
        frame.length = 8;
        frame.data[0] = 9;
        CHECK(sim.local().inject("CAN", at(0ms), frame) == can::SendResult::Accepted);
        sim.run_for(20ms);
        CHECK((receiver.seen == Records{{0, 9}}));
    }
}

// With one bus, local() needs no bus name: inject(time, frame) and add_j1939() use the ECUs' bus.
// With two, they throw std::logic_error.
TEST_CASE(simulation_local_without_a_bus_name_uses_the_only_bus)
{
    can::Frame frame;
    frame.id = 0x18FF2040; // priority 6, kPgnValue, from 0x40
    frame.length = 8;
    frame.data[0] = 9;
    {
        Simulation sim;
        auto& receiver = sim.add<Receiver>("Receiver", 10ms);
        CHECK(sim.local().inject(at(0ms), frame) == can::SendResult::Accepted);
        auto& tool = sim.local().add_j1939("Tool", 10ms);
        CHECK(&sim.local().network("Tool", "CAN") == &tool);
        sim.run_for(20ms);
        CHECK((receiver.seen == Records{{0, 9}}));
    }
    {
        Simulation sim;
        sim.add<Receiver>("Receiver", 10ms);
        sim.add<Receiver>("Other", 10ms, "CAN2");
        CHECK(throws_with<std::logic_error>([&] { sim.local().inject(at(0ms), frame); }, "\"CAN\", \"CAN2\""));
        CHECK(throws<std::logic_error>([&] { sim.local().add_j1939("Tool", 10ms); }));
    }
}

// The place to run chosen at run time: a std::optional<SilKitOptions>, empty for the local
// buses. Simulation sim; is local and Simulation sim(SilKitOptions{...}) is on SIL Kit, as
// before; the options are checked the same way.
TEST_CASE(simulation_place_can_be_chosen_at_run_time)
{
    Simulation byDefault;
    CHECK(byDefault.is_local());

    std::optional<SilKitOptions> silkit;
    Simulation local(silkit);
    CHECK(local.is_local());
    CHECK(throws<std::logic_error>([&] { static_cast<void>(local.registry_uri()); }));

    silkit.emplace();
    Simulation onSilKit(silkit);
    CHECK(!onSilKit.is_local());
    CHECK(onSilKit.registry_uri().rfind("silkit://", 0) == 0);
    CHECK(sender_and_receiver(onSilKit) == sender_and_receiver(local));

    Simulation direct(SilKitOptions{.controller = false});
    CHECK(!direct.is_local());
    silkit = SilKitOptions{.controller = false, .other_participants = {"X"}};
    CHECK(throws<std::invalid_argument>([&] { Simulation sim(silkit); }));
    CHECK(throws<std::invalid_argument>([&] { Simulation sim(*silkit); }));

    // A braced list without the type name is SilKitOptions. (Simulation sim{{}} does not compile:
    // the empty list fits both SilKitOptions and std::optional, so it is not quietly local.)
    Simulation braced({.registry_uri = "silkit://127.0.0.1:1", .controller = false});
    CHECK(!braced.is_local());
    CHECK(braced.registry_uri() == "silkit://127.0.0.1:1");
    Simulation none(std::nullopt);
    CHECK(none.is_local());
}
