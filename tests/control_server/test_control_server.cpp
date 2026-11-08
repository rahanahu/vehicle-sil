// SPDX-License-Identifier: MIT
//
// The Control API end to end in one process: a coordinated simulation of two J1939 peers with
// agents, the control server with the controller's time gate, and a gRPC client.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/peer_agent.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/agent_client.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::unit::InProcessRegistry;

constexpr j1939::Pgn kPgnEngine{0xFF10};

std::int64_t ns(std::chrono::nanoseconds d)
{
    return d.count();
}

std::int64_t ms_of(SimTime t)
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

/// Waits until `pred` holds or 10 s passed.
template <typename Pred>
bool eventually(Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

} // namespace

TEST_CASE(control_server_drives_a_simulation_end_to_end)
{
    InProcessRegistry registry;
    SimulationControllerOptions controlOptions;
    controlOptions.registry_uri = registry.uri;
    controlOptions.required_participants = {"Engine", "Body"};
    controlOptions.time_control = TimeControlOptions{.step = 10ms, .start_paused = true};
    SimulationController controller(controlOptions);
    const auto time = control_grpc::time_control_of(controller);
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .time = time.get()});
    const std::string address = "127.0.0.1:" + std::to_string(server.port());

    // Models and agents before the peers.
    std::uint8_t level = 1;
    control::Agent engine_agent(control::AgentOptions{.sample_period = 10ms});
    engine_agent.input("engine.level", &level, {.unit = "%", .min = 0.0, .max = 200.0});
    control::Agent body_agent;
    std::mutex received_mutex;
    std::vector<std::pair<std::int64_t, std::uint8_t>> received;

    Peer engine(coordinated("Engine", registry.uri, 10ms));
    auto& ecu = engine.j1939("J1939_CAN").add_node({0x00, j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});
    engine.every(20ms, [&](SimTime) {
        const std::uint8_t data[1]{level};
        static_cast<void>(ecu.send(kPgnEngine, data));
    });
    Peer body(coordinated("Body", registry.uri, 5ms));
    auto& listener = body.j1939("J1939_CAN").add_node({0x21});
    listener.on_message(kPgnEngine, [&](const j1939::Message& m) {
        const std::lock_guard lock(received_mutex);
        received.emplace_back(ms_of(body.runtime().now()), m.data[0]);
    });
    control::attach(engine, engine_agent);
    control::attach(body, body_agent);
    control_grpc::AgentClient engine_link(engine_agent, {.server = address, .participant = "Engine",
                                                         .status = [&] { return control::PeerStatus{engine.failed(), engine.error()}; }});
    control_grpc::AgentClient body_link(body_agent, {.server = address, .participant = "Body"});

    const auto stub = v1::ControlService::NewStub(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
    const auto call = [&](auto method, const auto& request, auto& reply) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 20s);
        return (stub.get()->*method)(&context, request, &reply);
    };

    // A watcher that collects what it is told.
    std::mutex updates_mutex;
    std::vector<v1::Update> updates;
    grpc::ClientContext watch_context;
    std::thread watcher([&] {
        v1::WatchRequest request;
        request.set_values(true);
        request.set_events(true);
        request.set_operations(true);
        request.set_time(true);
        auto reader = stub->Watch(&watch_context, request);
        v1::Update update;
        while (reader->Read(&update))
        {
            const std::lock_guard lock(updates_mutex);
            updates.push_back(update);
        }
        static_cast<void>(reader->Finish());
    });

    // Both agents show up.
    CHECK(eventually([&] {
        v1::ListPeersReply peers;
        return call(&v1::ControlService::Stub::ListPeers, v1::ListPeersRequest{}, peers).ok() && peers.peers_size() == 2
               && peers.peers(0).connected() && peers.peers(1).connected();
    }));
    {
        v1::ListPeersReply peers;
        CHECK(call(&v1::ControlService::Stub::ListPeers, v1::ListPeersRequest{}, peers).ok());
        for (const auto& p : peers.peers())
        {
            CHECK(p.has_agent());
            if (p.participant() == "Engine")
            {
                CHECK(p.schema(0).name() == "engine.level");
                CHECK(p.schema(0).unit() == "%");
                CHECK(p.schema(0).has_max() && p.schema(0).max() == 200.0);
            }
        }
    }

    start_together(controller, {&engine, &body});
    v1::TimeState state;
    v1::WaitPausedRequest wait_paused;
    wait_paused.set_timeout_ms(10000);
    CHECK(call(&v1::ControlService::Stub::WaitPaused, wait_paused, state).ok());
    CHECK(state.available() && state.paused() && state.now_ns() == 0);

    // While paused: schedule a write at 105 ms and make sure the engine queued it before resuming.
    v1::WriteRequest write;
    auto* w = write.add_writes();
    w->set_peer("Engine");
    w->set_name("engine.level");
    w->mutable_value()->set_i(7);
    write.set_at_ns(ns(105ms));
    v1::WriteReply written;
    CHECK(call(&v1::ControlService::Stub::Write, write, written).ok());
    v1::WaitOperationRequest wait_queued;
    wait_queued.set_seq(written.seq());
    wait_queued.set_until(v1::WAIT_FOR_QUEUED);
    wait_queued.set_timeout_ms(10000);
    v1::Operation op;
    CHECK(call(&v1::ControlService::Stub::WaitOperation, wait_queued, op).ok());
    CHECK(op.targets(0).status() == v1::TARGET_STATUS_QUEUED);

    // The links drop and come back (same processes): the engine still holds the write, says so
    // in its hello, and the server keeps the write open instead of giving up on it.
    const auto sessions_before = engine_link.sessions();
    server.disconnect_agents();
    CHECK(eventually([&] { return engine_link.sessions() > sessions_before; })); // dropped and welcomed again
    CHECK(eventually([&] {
        v1::GetOperationRequest get;
        get.set_seq(written.seq());
        v1::Operation current;
        v1::ListPeersReply peers;
        return call(&v1::ControlService::Stub::GetOperation, get, current).ok()
               && current.targets(0).status() == v1::TARGET_STATUS_QUEUED
               && call(&v1::ControlService::Stub::ListPeers, v1::ListPeersRequest{}, peers).ok() && peers.peers_size() == 2
               && peers.peers(0).connected() && peers.peers(1).connected() && engine_link.connected();
    }));

    // Run to 300 ms and stand still there.
    v1::RunUntilRequest run;
    run.set_at_ns(ns(300ms));
    CHECK(call(&v1::ControlService::Stub::RunUntil, run, state).ok());
    CHECK(call(&v1::ControlService::Stub::WaitPaused, wait_paused, state).ok());
    CHECK_EQ(state.now_ns(), ns(300ms));

    v1::WaitOperationRequest wait_done;
    wait_done.set_seq(written.seq());
    wait_done.set_timeout_ms(10000);
    CHECK(call(&v1::ControlService::Stub::WaitOperation, wait_done, op).ok());
    CHECK(op.done());
    CHECK(op.targets(0).status() == v1::TARGET_STATUS_APPLIED);
    CHECK_EQ(op.targets(0).applied_at_ns(), ns(110ms));
    {
        // Engine broadcasts every 20 ms: the one at 120 ms is the first to carry 7.
        const std::lock_guard lock(received_mutex);
        CHECK(!received.empty());
        for (const auto& [t, v] : received)
        {
            CHECK_EQ(static_cast<int>(v), t <= 120 ? 1 : 7);
        }
    }

    // The engine's values (with the monitoring outputs) reach the server.
    CHECK(eventually([&] {
        v1::GetValuesRequest request;
        request.set_peer("Engine");
        v1::GetValuesReply values;
        return call(&v1::ControlService::Stub::GetValues, request, values).ok() && values.has_values()
               && values.snapshot().values(0).i() == 7 && values.snapshot().time_ns() >= ns(290ms);
    }));

    // A step and a speed change.
    v1::SetSpeedRequest speed;
    speed.set_speed(1.0);
    CHECK(call(&v1::ControlService::Stub::SetSpeed, speed, state).ok());
    CHECK(state.has_speed() && state.speed() == 1.0);
    v1::StepRequest step;
    step.set_duration_ns(ns(50ms));
    const auto before = std::chrono::steady_clock::now();
    CHECK(call(&v1::ControlService::Stub::Step, step, state).ok());
    CHECK(call(&v1::ControlService::Stub::WaitPaused, wait_paused, state).ok());
    CHECK_EQ(state.now_ns(), ns(350ms));
    CHECK(std::chrono::steady_clock::now() - before >= 40ms); // real time: about 50 ms

    // Errors are reported as gRPC statuses.
    v1::WriteRequest bad;
    v1::WriteReply bad_reply;
    CHECK(call(&v1::ControlService::Stub::Write, bad, bad_reply).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    v1::GetOperationRequest missing;
    missing.set_seq(1);
    CHECK(call(&v1::ControlService::Stub::GetOperation, missing, op).error_code() == grpc::StatusCode::NOT_FOUND);

    // Stop the simulation from two clients at once (the stop is only requested: thread safe).
    v1::StopRequest stop;
    stop.set_reason("test done");
    std::atomic<int> stops_ok{0};
    std::thread second([&] {
        v1::TimeState s2;
        if (call(&v1::ControlService::Stub::Stop, stop, s2).ok())
        {
            ++stops_ok;
        }
    });
    if (call(&v1::ControlService::Stub::Stop, stop, state).ok())
    {
        ++stops_ok;
    }
    second.join();
    CHECK_EQ(stops_ok.load(), 2);
    for (Peer* p : {&engine, &body})
    {
        const auto finished = p->wait(30s);
        CHECK(finished.has_value() && *finished == orch::ParticipantState::Shutdown);
        CHECK(!p->failed());
    }

    // The watcher saw both peers, the operation's progress and the engine's values and events.
    watch_context.TryCancel();
    watcher.join();
    bool saw_engine = false;
    bool saw_applied = false;
    bool saw_values = false;
    bool saw_claim = false;
    bool saw_initial = false;
    bool saw_paused_time = false;
    for (const auto& u : updates)
    {
        saw_initial = saw_initial || (u.has_initial_done() && u.initial_done());
        saw_paused_time = saw_paused_time || (u.has_time() && u.time().paused() && u.time().now_ns() == ns(300ms));
        saw_engine = saw_engine || (u.has_peer() && u.peer().participant() == "Engine");
        saw_applied = saw_applied
                      || (u.has_operation() && u.operation().seq() == written.seq()
                          && u.operation().targets(0).status() == v1::TARGET_STATUS_APPLIED);
        saw_values = saw_values || (u.has_values() && u.values().peer() == "Engine");
        saw_claim = saw_claim || (u.has_event() && u.event().event().text() == "j1939.J1939_CAN.node0: claimed at address 0");
    }
    CHECK(saw_engine);
    CHECK(saw_applied);
    CHECK(saw_values);
    CHECK(saw_claim);
    CHECK(saw_initial);
    CHECK(saw_paused_time);

    engine_agent.finish();
    body_agent.finish();
    engine_link.stop();
    body_link.stop();
}

TEST_CASE(control_server_agents_reconnect_after_a_server_restart)
{
    // The agent's link survives a restart of the server on the same port: it reconnects with
    // back-off and the peer is listed again.
    control::Agent agent;
    double x = 0.0;
    agent.input("x", &x);
    agent.bind_driver(); // no peer: the test drives no steps
    int port = 0;
    {
        control_grpc::ControlServer first({.listen = "127.0.0.1:0"});
        port = first.port();
        control_grpc::AgentClient link(agent, {.server = "127.0.0.1:" + std::to_string(port), .participant = "Lonely",
                                               .reconnect_min = 20ms, .reconnect_max = 200ms});
        CHECK(link.wait_connected(10s));
        CHECK(eventually([&] { return first.hub().peers().size() == 1; }));
        first.shutdown();
        CHECK(eventually([&] { return !link.connected(); }));
        control_grpc::ControlServer second({.listen = "127.0.0.1:" + std::to_string(port)});
        CHECK(link.wait_connected(10s));
        CHECK(eventually([&] {
            const auto peers = second.hub().peers();
            return peers.size() == 1 && peers[0].connected && peers[0].incarnation == link.incarnation();
        }));
        // A write through the new server reaches the agent.
        const auto seq = second.hub().write(control::OperationRequest{{{"Lonely", "x", 2.5}}});
        const auto queued = second.hub().wait_queued(seq, 10s);
        CHECK(queued.has_value());
        CHECK(queued->targets.at(0).status == control::TargetStatus::Queued);
        link.stop();
    }
}

TEST_CASE(control_server_shutdown_ends_waiting_calls)
{
    // A client waits for an operation that will not finish (the agent never steps); shutting the
    // server down ends the wait at once instead of after the client's timeout.
    control::Agent agent;
    double x = 0.0;
    agent.input("x", &x);
    agent.bind_driver();
    auto server = std::make_unique<control_grpc::ControlServer>(control_grpc::ServerOptions{.listen = "127.0.0.1:0"});
    const std::string address = "127.0.0.1:" + std::to_string(server->port());
    control_grpc::AgentClient link(agent, {.server = address, .participant = "Idle"});
    CHECK(link.wait_connected(10s));
    CHECK(eventually([&] { return server->hub().peers().size() == 1; }));
    const auto seq = server->hub().write(control::OperationRequest{{{"Idle", "x", 1.0}}});
    CHECK(server->hub().wait_queued(seq, 10s).has_value());
    const auto stub = v1::ControlService::NewStub(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        grpc::ClientContext context;
        v1::WaitOperationRequest request;
        request.set_seq(seq);
        request.set_timeout_ms(50000);
        v1::Operation op;
        static_cast<void>(stub->WaitOperation(&context, request, &op));
        returned = true;
    });
    std::this_thread::sleep_for(300ms);
    const auto begin = std::chrono::steady_clock::now();
    server->shutdown();
    CHECK(std::chrono::steady_clock::now() - begin < 5s);
    CHECK(eventually([&] { return returned.load(); }));
    waiter.join();
    link.stop();
}

TEST_CASE(control_server_lists_simulation_participants_without_an_agent)
{
    // The server, as the controller, sees every participant of the simulation through the SIL Kit
    // registry, on whatever PC it runs: one without an agent is listed with its SIL Kit state.
    InProcessRegistry registry;
    SimulationControllerOptions controlOptions;
    controlOptions.registry_uri = registry.uri;
    controlOptions.required_participants = {"Plain"};
    controlOptions.time_control = TimeControlOptions{.step = 10ms};
    SimulationController controller(controlOptions);
    const auto time = control_grpc::time_control_of(controller);
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .time = time.get()});
    ParticipantRuntime plain(vehicle_sil::unit::coordinated_options("Plain", registry.uri, VirtualTimeOptions{10ms}));
    const auto stub = v1::ControlService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
    const auto listed = [&](const std::string& state) {
        grpc::ClientContext context;
        v1::ListPeersReply peers;
        return stub->ListPeers(&context, v1::ListPeersRequest{}, &peers).ok() && peers.peers_size() == 1
               && peers.peers(0).participant() == "Plain" && !peers.peers(0).has_agent()
               && peers.peers(0).simulation_state() == state;
    };
    // Before its lifecycle starts, SIL Kit has no state for it yet; once it runs, it is Running.
    CHECK(listed(""));
    plain.set_step_handler([](SimTime, SimDuration) {});
    vehicle_sil::unit::start_together(controller, {&plain}, 30s);
    CHECK(eventually([&] { return listed("Running"); }));
    controller.stop("done");
    plain.stop("done");
}
