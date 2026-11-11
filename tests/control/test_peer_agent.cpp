// SPDX-License-Identifier: MIT
//
// The control agent of a vehicle_sil::Peer (its variables declared on the Ecu), with real SIL
// Kit participants (in-process registry): where the writes take effect, the traffic, real time,
// the monitoring outputs (PeerOptions::monitor).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
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
using vehicle_sil::unit::throws_with;

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
/// time. With `with_agents`, Engine declares `level` as an input and Body `last` as an output;
/// with `hammer`, another thread reads the agents meanwhile.
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

    // Models first: they outlive the peers and their agents.
    std::uint8_t level = 1;
    std::uint8_t last = 0;

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
    Agent* engine_agent = nullptr;
    Agent* body_agent = nullptr;
    if (with_agents)
    {
        engine.set_agent_options(AgentOptions{.sample_period = SimDuration::zero()});
        engine.input("engine.level", &level, {.min = 0.0, .max = 200.0});
        body.output("body.last", &last);
        engine_agent = &engine.agent();
        body_agent = &body.agent();
        for (const auto& c : engine_commands)
        {
            CHECK(engine_agent->submit(c).status == SubmitStatus::Queued);
        }
    }

    // Stopped by its destructor too, so a failed CHECK below does not wait for it forever.
    std::jthread transport([&](const std::stop_token& stop) {
        while (hammer && !stop.stop_requested())
        {
            static_cast<void>(engine_agent->latest());
            static_cast<void>(body_agent->latest());
            std::vector<Event> events;
            engine_agent->drain_events(events);
        }
    });
    start_together(controller, {&engine, &body});
    for (Peer* p : {&engine, &body})
    {
        const auto state = p->wait(60s);
        CHECK(state.has_value() && *state == orch::ParticipantState::Shutdown);
        CHECK(!p->failed());
    }
    transport.request_stop();
    transport.join();
    if (!with_agents)
    {
        // No variables: the peers made no agent, also not in the run.
        CHECK(throws<std::logic_error>([&] { static_cast<void>(engine.agent()); }));
        CHECK(throws<std::logic_error>([&] { static_cast<void>(body.agent()); }));
        return run;
    }
    engine.stop(); // the steps are over: the writes still waiting are cancelled
    body.stop();
    engine_agent->drain_results(run.results);
    run.engine_snapshot = engine_agent->latest();
    run.body_snapshot = body_agent->latest();
    run.engine_schema = engine_agent->schema();
    run.body_schema = body_agent->schema();
    engine_agent->drain_events(run.engine_events);
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
    Peer peer(o);
    peer.set_agent_options(AgentOptions{.sample_period = 50ms});
    // Added before the variables: the agent still applies the writes before it.
    peer.on_step([&](SimTime now) {
        seen.emplace_back(ms(now), brake);
        speed += 1.0;
    });
    peer.input("cabin.brake", &brake);
    peer.output("vehicle.speed", &speed, {.unit = "km/h"});
    auto& agent = peer.agent();
    CHECK(throws<std::logic_error>([&] { peer.set_agent_options({}); })); // the agent is made
    CHECK(agent.submit(Command{1, {{"cabin.brake", true}}, at(55ms)}).status == SubmitStatus::Queued);
    CHECK(agent.submit(Command{2, {{"cabin.brake", false}}, at(120ms)}).status == SubmitStatus::Queued);
    CHECK(peer.run(60s) == orch::ParticipantState::Shutdown);
    CHECK(throws<std::logic_error>([&] { peer.input("late", &brake); }));
    CHECK(&peer.agent() == &agent); // after the start too
    peer.stop();

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

TEST_CASE(control_peer_agent_wakes_a_real_time_peer)
{
    // A real-time peer that otherwise steps only every 2 s (max_idle): queued commands wake it, so
    // they take effect within a fraction of that, again and again. stop() cancels what is still
    // waiting, and a later submit is refused without touching the peer.
    InProcessRegistry registry;
    std::atomic<int> value{0};
    PeerOptions o;
    o.participant = "RealTimeAgent";
    o.registry_uri = registry.uri;
    o.virtual_time = std::nullopt;
    o.max_idle = 2s;
    Peer peer(o);
    peer.input("value", [&] { return value.load(); }, [&](int v) { value = v; });
    auto& agent = peer.agent();
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
    // Far in the future: still waiting when the peer stops.
    CHECK(agent.submit(Command{4, {{"value", std::int64_t{4}}}, at(std::chrono::hours(1))}).status == SubmitStatus::Queued);
    peer.stop();
    CHECK(!peer.failed());
    CHECK(agent.submit(Command{9, {{"value", std::int64_t{9}}}, std::nullopt}).status == SubmitStatus::Invalid);
    std::vector<CommandResult> results;
    agent.drain_results(results);
    CHECK_EQ(results.size(), std::size_t{4});
    CHECK(results[3].seq == 4);
    CHECK(results[3].status == CommandStatus::Cancelled);
    CHECK_EQ(value.load(), 3);
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
    {
        // The clash shows when the schema is completed (here: start()); nothing is added, the
        // message names the peer and the way out, and the peer is not started.
        Peer peer(o);
        peer.input("peer.speed", &x);
        CHECK(throws_with<std::invalid_argument>([&] { peer.start(); }, "\"Clash\""));
        CHECK(throws_with<std::invalid_argument>([&] { peer.agent().complete_schema(); }, "PeerOptions::monitor = false"));
        CHECK_EQ(peer.agent().schema().size(), std::size_t{1});
        peer.input("still.open", &x); // the schema stayed open
    }
    {
        PeerOptions plain = o;
        plain.monitor = false;
        Peer peer(plain);
        peer.input("peer.speed", &x);
        peer.agent().complete_schema();
        CHECK_EQ(peer.agent().schema().size(), std::size_t{1});
        // The schema is complete: the message names the peer and why.
        CHECK(throws_with<std::logic_error>([&] { peer.input("too.late", &x); }, "\"Clash\""));
        CHECK(throws_with<std::logic_error>([&] { peer.input("too.late", &x); }, "after the schema was complete"));
    }
}

TEST_CASE(control_peer_agent_monitor_covers_what_exists_when_the_schema_completes)
{
    // The agent made before the network (a variable declared first): the monitoring still covers
    // the network and its node, and comes after the ECU's variables.
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Order";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(50ms)};
    double x = 0.0;
    Peer peer(o);
    peer.output("app.x", &x);
    peer.j1939("J1939_CAN").add_node({0x21});
    peer.agent().complete_schema();
    const auto& schema = peer.agent().schema();
    CHECK(!schema.empty() && schema[0].name == "app.x");
    bool has_node = false;
    for (const auto& info : schema)
    {
        has_node = has_node || info.name == "j1939.J1939_CAN.node0.address_state";
    }
    CHECK(has_node);
}

TEST_CASE(control_peer_agent_refuses_a_network_the_monitoring_would_miss)
{
    // Once the schema is complete with the monitoring outputs, a J1939 network added later would
    // not be monitored: j1939() refuses it (both forms), naming the peer, the network and the way
    // out. Without monitoring, or before the schema is complete, it is fine.
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Late";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(50ms)};
    double x = 0.0;
    {
        Peer peer(o);
        peer.output("app.x", &x);
        peer.j1939("CAN1");
        peer.agent().complete_schema();
        CHECK(throws_with<std::logic_error>([&] { peer.j1939("CAN2"); }, "\"Late\""));
        CHECK(throws_with<std::logic_error>([&] { peer.j1939("CAN2"); }, "j1939(\"CAN2\")"));
        CHECK(throws_with<std::logic_error>([&] { peer.j1939("CAN2"); }, "PeerOptions::monitor = false"));
        CHECK(throws<std::logic_error>([&] { peer.j1939("CAN3", J1939ChannelOptions{}); }));
        Ecu& ecu = peer;
        CHECK(throws<std::logic_error>([&] { ecu.j1939("CAN4"); }));
        // Checked before the port (no port: std::invalid_argument otherwise).
        CHECK(vehicle_sil::unit::throws_if<std::logic_error>([&] { peer.j1939("EXT", std::unique_ptr<can::Port>{}); },
                                                             [](const std::logic_error& e) {
                                                                 return dynamic_cast<const std::invalid_argument*>(&e) == nullptr;
                                                             }));
        CHECK_EQ(peer.network_names().size(), std::size_t{1});
        peer.can("CAN5"); // raw CAN and LIN are not monitored
    }
    {
        PeerOptions plain = o;
        plain.monitor = false;
        Peer peer(plain);
        peer.output("app.x", &x);
        peer.agent().complete_schema();
        peer.j1939("CAN1");
        CHECK_EQ(peer.agent().schema().size(), std::size_t{1});
    }
}
