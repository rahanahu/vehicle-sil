// SPDX-License-Identifier: MIT
//
// A control agent driven by vehicle_sil::Peer (vehicle_sil::control::attach), with real SIL Kit
// participants (in-process registry).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/peer_agent.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::throws;

constexpr j1939::Pgn kPgnEngine{0xFF10}; ///< proprietary B: the engine's broadcast

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

std::int64_t ms(SimTime t)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

PeerOptions coordinated(std::string name, const std::string& uri, SimDuration step)
{
    PeerOptions o;
    o.participant = std::move(name);
    o.registry_uri = uri;
    o.lifecycle = LifecycleMode::Coordinated;
    o.virtual_time = VirtualTimeOptions{step, std::nullopt, std::nullopt};
    return o;
}

/// Two peers: Engine broadcasts `level` every 20 ms, Body records what it receives with its step
/// time. Each peer optionally has an agent; `while_running` runs on another thread meanwhile.
struct Run
{
    std::vector<std::pair<std::int64_t, std::uint8_t>> received; ///< (Body's step time, value)
    std::vector<CommandResult> results;
    Snapshot engine_snapshot;
    Snapshot body_snapshot;
    std::vector<VariableInfo> engine_schema;
    std::vector<VariableInfo> body_schema;
    std::vector<Event> engine_events;
};

/// The value of the variable `name` in a snapshot of an agent with `schema`.
Value value_of(const std::vector<VariableInfo>& schema, const Snapshot& snapshot, const std::string& name)
{
    for (std::size_t i = 0; i < schema.size(); ++i)
    {
        if (schema[i].name == name)
        {
            return snapshot.values.at(i);
        }
    }
    throw vehicle_sil::unit::Failure("no variable " + name);
}

Run run_pair(bool with_agents, const std::vector<Command>& engine_commands, bool hammer)
{
    Run run;
    InProcessRegistry registry;
    SimulationControllerOptions control;
    control.registry_uri = registry.uri;
    control.required_participants = {"Engine", "Body"};
    control.stop_at = 400ms;
    SimulationController controller(control);

    // Models and agents first: they outlive the peers' steps.
    std::uint8_t level = 1;
    Agent engine_agent(AgentOptions{.sample_period = SimDuration::zero()});
    engine_agent.input("engine.level", &level, {.min = 0.0, .max = 200.0});
    Agent body_agent;
    std::uint8_t last = 0;
    body_agent.output("body.last", &last);

    Peer engine(coordinated("Engine", registry.uri, 10ms));
    auto& ecu = engine.j1939("J1939_CAN").add_node({0x00, j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});
    engine.every(20ms, [&](SimTime) {
        const std::uint8_t data[1]{level};
        static_cast<void>(ecu.send(kPgnEngine, data));
    });
    Peer body(coordinated("Body", registry.uri, 5ms));
    auto& listener = body.j1939("J1939_CAN").add_node({0x21});
    listener.on_message(kPgnEngine, [&](const j1939::Message& m) {
        last = m.data[0];
        run.received.emplace_back(ms(body.runtime().now()), m.data[0]);
    });
    if (with_agents)
    {
        attach(engine, engine_agent);
        attach(body, body_agent);
        for (const auto& c : engine_commands)
        {
            CHECK(engine_agent.submit(c).status == SubmitStatus::Queued);
        }
    }

    std::atomic<bool> running{true};
    std::thread transport([&] {
        while (hammer && running.load())
        {
            static_cast<void>(engine_agent.latest());
            static_cast<void>(body_agent.latest());
            std::vector<Event> events;
            engine_agent.drain_events(events);
        }
    });
    start_together(controller, {&engine, &body});
    for (Peer* p : {&engine, &body})
    {
        const auto state = p->wait(60s);
        CHECK(state.has_value() && *state == orch::ParticipantState::Shutdown);
        CHECK(!p->failed());
    }
    running = false;
    transport.join();
    engine_agent.finish();
    engine_agent.drain_results(run.results);
    run.engine_snapshot = engine_agent.latest();
    run.body_snapshot = body_agent.latest();
    run.engine_schema = engine_agent.schema();
    run.body_schema = body_agent.schema();
    engine_agent.drain_events(run.engine_events);
    return run;
}

} // namespace

TEST_CASE(control_peer_agent_applies_before_the_application_of_its_step)
{
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Alone";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(200ms)};

    bool brake = false;
    double speed = 0.0;
    std::vector<std::pair<std::int64_t, bool>> seen;
    Agent agent(AgentOptions{.sample_period = 50ms});
    agent.input("cabin.brake", &brake);
    agent.output("vehicle.speed", &speed, {.unit = "km/h"});

    Peer peer(o);
    // Added before attach(): the agent's hook still runs before it.
    peer.on_step([&](SimTime now) {
        seen.emplace_back(ms(now), brake);
        speed += 1.0;
    });
    attach(peer, agent);
    CHECK(throws<std::logic_error>([&] { peer.set_control({}, {}); }));
    CHECK(throws<std::logic_error>([&] { attach(peer, agent); })); // the agent is bound already
    CHECK(agent.submit(Command{1, {{"cabin.brake", true}}, at(55ms)}).status == SubmitStatus::Queued);
    CHECK(agent.submit(Command{2, {{"cabin.brake", false}}, at(120ms)}).status == SubmitStatus::Queued);
    CHECK(peer.run(60s) == orch::ParticipantState::Shutdown);
    CHECK(throws<std::logic_error>([&] { peer.set_control({}, {}); }));
    agent.finish();

    std::vector<CommandResult> results;
    agent.drain_results(results);
    CHECK_EQ(results.size(), std::size_t{2});
    CHECK(results[0].applied_at == std::optional<SimTime>(at(60ms)));
    CHECK(results[1].applied_at == std::optional<SimTime>(at(120ms)));
    for (const auto& [t, b] : seen)
    {
        CHECK_EQ(b, t >= 60 && t < 120);
    }
    // Sampled after the application at 0, 50, 100, 150: the last sample is at 150 with 16 steps.
    const auto snap = agent.latest();
    CHECK(snap.time == at(150ms));
    CHECK_EQ(std::get<double>(snap.values[1]), 16.0);
}

TEST_CASE(control_peer_agent_does_not_change_the_traffic)
{
    // No agent, and agents with a busy reader: Body receives the same values at the same steps.
    const auto plain = run_pair(false, {}, false);
    const auto observed = run_pair(true, {}, true);
    CHECK(!plain.received.empty());
    CHECK(plain.received == observed.received);
}

TEST_CASE(control_peer_agent_timed_writes_are_reproducible)
{
    // Engine's level changes at 105 ms (applied at 110 ms) and 250 ms: Body sees 7 from the first
    // broadcast at or after 110 ms, and the same in every run.
    const std::vector<Command> commands{
        Command{1, {{"engine.level", std::int64_t{7}}}, at(105ms)},
        Command{2, {{"engine.level", std::int64_t{9}}}, at(250ms)},
    };
    const auto first = run_pair(true, commands, true);
    const auto second = run_pair(true, commands, false);
    CHECK(first.received == second.received);
    CHECK_EQ(first.results.size(), std::size_t{2});
    CHECK(first.results[0].applied_at == std::optional<SimTime>(at(110ms)));
    CHECK(first.results[1].applied_at == std::optional<SimTime>(at(250ms)));
    // Engine broadcasts at 0, 20, ... : the one at 120 ms carries 7, the one at 260 ms carries 9;
    // Body handles a broadcast in its first step after it was sent.
    for (const auto& [t, v] : first.received)
    {
        const std::uint8_t expected = t <= 120 ? 1 : (t <= 260 ? 7 : 9);
        CHECK_EQ(static_cast<int>(v), static_cast<int>(expected));
    }
    CHECK(std::get<std::int64_t>(first.engine_snapshot.values[0]) == 9);
}

TEST_CASE(control_peer_agent_attach_errors_leave_nothing_behind)
{
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Errors";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(50ms)};
    int x = 0;
    {
        // An agent that already took a command cannot be attached (in both time modes).
        // Agents are declared before the peers: a peer uses its agent until it is destroyed.
        Agent agent;
        Agent other;
        agent.input("x", &x);
        CHECK(agent.submit(Command{1, {{"x", std::int64_t{1}}}, std::nullopt}).status == SubmitStatus::Queued);
        Peer peer(o);
        CHECK(throws<std::logic_error>([&] { attach(peer, agent); }));
        // The peer is untouched: another agent can still be attached.
        attach(peer, other);
    }
    {
        // One agent, two peers: the second attach fails and leaves the second peer free.
        Agent agent;
        Agent other;
        agent.input("x", &x);
        Peer first(o);
        PeerOptions o2 = o;
        o2.participant = "Errors2";
        Peer second(o2);
        attach(first, agent);
        CHECK(throws<std::logic_error>([&] { attach(second, agent); }));
        attach(second, other);
    }
    {
        // A peer whose control is set: attach fails and leaves the agent free.
        Agent agent;
        agent.input("x", &x);
        Peer peer(o);
        peer.set_control({}, {});
        CHECK(throws<std::logic_error>([&] { attach(peer, agent); }));
        Peer other(PeerOptions{.participant = "Errors3", .registry_uri = registry.uri,
                               .virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(50ms)}});
        attach(other, agent);
    }
}

TEST_CASE(control_peer_agent_wakes_a_real_time_peer)
{
    // A real-time peer that otherwise steps only every 2 s (max_idle): queued commands wake it, so
    // they take effect within a fraction of that, again and again. After the peer is gone, a
    // submit does not touch it.
    InProcessRegistry registry;
    std::atomic<int> value{0};
    Agent agent;
    agent.input("value", [&] { return value.load(); }, [&](int v) { value = v; });
    std::vector<CommandResult> results;
    {
        PeerOptions o;
        o.participant = "RealTimeAgent";
        o.registry_uri = registry.uri;
        o.virtual_time = std::nullopt;
        o.max_idle = 2s;
        Peer peer(o);
        attach(peer, agent);
        peer.start();
        std::this_thread::sleep_for(50ms);
        for (int i = 1; i <= 3; ++i)
        {
            const auto begin = std::chrono::steady_clock::now();
            CHECK(agent.submit(Command{static_cast<std::uint64_t>(i), {{"value", std::int64_t{i}}}, std::nullopt}).status
                  == SubmitStatus::Queued);
            while (value.load() != i && std::chrono::steady_clock::now() - begin < 5s)
            {
                std::this_thread::sleep_for(1ms);
            }
            CHECK_EQ(value.load(), i);
            CHECK(std::chrono::steady_clock::now() - begin < 1s);
        }
        peer.stop();
        CHECK(!peer.failed());
    }
    CHECK(agent.submit(Command{9, {{"value", std::int64_t{9}}}, std::nullopt}).status == SubmitStatus::Queued);
    agent.finish();
    agent.drain_results(results);
    CHECK_EQ(results.size(), std::size_t{4});
    CHECK(results[3].status == CommandStatus::Cancelled);
}

TEST_CASE(control_peer_agent_monitors_the_networks)
{
    const auto run = run_pair(true, {}, false);
    const auto get = [&](const std::vector<VariableInfo>& schema, const Snapshot& snap, const char* name) {
        return value_of(schema, snap, name);
    };
    // Engine: a NAME, so it claimed its address; it sent its broadcasts and its claim.
    CHECK_EQ(std::get<std::int64_t>(get(run.engine_schema, run.engine_snapshot, "j1939.J1939_CAN.node0.address_state")),
             std::int64_t{3});
    CHECK_EQ(std::get<std::int64_t>(get(run.engine_schema, run.engine_snapshot, "j1939.J1939_CAN.node0.address")), std::int64_t{0});
    CHECK(std::get<std::int64_t>(get(run.engine_schema, run.engine_snapshot, "j1939.J1939_CAN.frames_sent")) > 10);
    CHECK_EQ(std::get<std::int64_t>(get(run.engine_schema, run.engine_snapshot, "j1939.J1939_CAN.send_failures")), std::int64_t{0});
    CHECK(std::get<std::int64_t>(get(run.engine_schema, run.engine_snapshot, "peer.steps")) > 30);
    CHECK(std::get<double>(get(run.engine_schema, run.engine_snapshot, "peer.speed")) > 0.0);
    // Body: no NAME (a fixed address); it received the engine's frames.
    CHECK_EQ(std::get<std::int64_t>(get(run.body_schema, run.body_snapshot, "j1939.J1939_CAN.node0.address_state")), std::int64_t{0});
    CHECK(std::get<std::int64_t>(get(run.body_schema, run.body_snapshot, "j1939.J1939_CAN.frames_received")) > 10);
    // The schema describes the states.
    for (const auto& info : run.engine_schema)
    {
        if (info.name == "j1939.J1939_CAN.node0.address_state")
        {
            CHECK(info.type == ValueType::Enum);
            CHECK(info.causality == Causality::Output);
            CHECK_EQ(info.items.size(), std::size_t{5});
        }
    }
    // Events: the engine's node went from not started to claiming to claimed.
    std::vector<std::string> texts;
    for (const auto& e : run.engine_events)
    {
        texts.push_back(e.text);
    }
    CHECK(std::find(texts.begin(), texts.end(), "j1939.J1939_CAN.node0: claiming at address 0") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "j1939.J1939_CAN.node0: claimed at address 0") != texts.end());
}

TEST_CASE(control_peer_agent_monitor_names_must_not_clash)
{
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Clash";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(50ms)};
    double x = 0.0;
    Agent agent;
    Agent plain;
    agent.input("peer.speed", &x);
    plain.input("peer.speed", &x);
    Peer peer(o);
    CHECK(throws<std::invalid_argument>([&] { attach(peer, agent); }));
    attach(peer, plain, AttachOptions{.monitor = false}); // the peer was left free
    CHECK_EQ(plain.schema().size(), std::size_t{1});
}
