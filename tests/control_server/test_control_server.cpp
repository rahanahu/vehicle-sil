// SPDX-License-Identifier: MIT
//
// The Control API end to end in one process: a coordinated simulation of two J1939 peers whose
// agents reach the hub over SIL Kit, the control server with the controller's time gate, and a
// gRPC client.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/control_silkit/agent_transport.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
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

/// Waits until `pred` holds or 10 s (scaled) passed.
template <typename Pred>
bool eventually(Pred pred)
{
    return vehicle_sil::unit::eventually(pred, 10s, 10ms);
}

/// A link to an agent that takes every command and never answers.
struct SilentLink final : control::AgentLink
{
    bool send(const control::Command&) override { return true; }
};

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
    // The hub's end of the agents' connections on the controller's participant, before it starts.
    // The window before the start waits until the agents receive commands.
    control_silkit::HubTransport hub_link(server.hub(), controller.runtime());
    std::vector<std::string> announced;
    controller.set_start_window([&](const StartWindow& window) {
        const auto report = hub_link.wait_for_hellos({"Engine", "Body"}, window.deadline(), [&] { return window.cancelled(); });
        announced = report.connected;
    });

    // Models before the peers.
    std::uint8_t level = 1;
    std::uint8_t mode = 0; // an enum variable: its items reach the client, and a write by value
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
    engine.set_agent_options(control::AgentOptions{.sample_period = 10ms});
    engine.input("engine.level", &level, {.unit = "%", .min = 0.0, .max = 200.0});
    engine.input_enum("engine.mode", &mode, {{"off", 0}, {"eco", 1}, {"sport", 2}});
    // Body declares nothing: its agent has ecu.connected and the peer's monitoring outputs. The links: after
    // the ECUs, before the start; destroyed before the peers, after their stop().
    auto engine_link = std::make_unique<control_silkit::AgentTransport>(engine.agent(), engine.runtime());
    auto body_link = std::make_unique<control_silkit::AgentTransport>(body.agent(), body.runtime());

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
                CHECK(p.schema(0).name() == "ecu.connected"); // every ECU has it, first
                CHECK(p.schema(1).name() == "engine.level");
                CHECK(p.schema(1).unit() == "%");
                CHECK(p.schema(1).has_max() && p.schema(1).max() == 200.0);
                const auto& m = p.schema(2);
                CHECK(m.name() == "engine.mode" && m.type() == v1::VALUE_TYPE_ENUM && m.causality() == v1::CAUSALITY_INPUT);
                CHECK(m.items_size() == 3 && m.items(2).name() == "sport" && m.items(2).value() == 2);
            }
        }
    }

    start_together(controller, {&engine, &body});
    CHECK((announced == std::vector<std::string>{"Engine", "Body"}));
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
    auto* wm = write.add_writes();
    wm->set_peer("Engine");
    wm->set_name("engine.mode");
    wm->mutable_value()->set_i(2);
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
               && values.snapshot().values(1).i() == 7 && values.snapshot().values(2).i() == 2
               && values.snapshot().time_ns() >= ns(290ms);
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

    // A value that is no item of the enum: the agent refuses it (a result, not a gRPC error).
    {
        v1::WriteRequest bad_mode;
        auto* b = bad_mode.add_writes();
        b->set_peer("Engine");
        b->set_name("engine.mode");
        b->mutable_value()->set_i(5);
        v1::WriteReply refused;
        CHECK(call(&v1::ControlService::Stub::Write, bad_mode, refused).ok());
        v1::WaitOperationRequest wait_refused;
        wait_refused.set_seq(refused.seq());
        wait_refused.set_timeout_ms(10000);
        v1::Operation refused_op;
        CHECK(call(&v1::ControlService::Stub::WaitOperation, wait_refused, refused_op).ok());
        CHECK(refused_op.done() && refused_op.targets(0).status() == v1::TARGET_STATUS_INVALID);
        CHECK(!refused_op.targets(0).reason().empty());
    }

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

    engine.stop(); // cancels what is still waiting on the agents ...
    body.stop();
    engine_link.reset(); // ... and the links send those results as their last word
    body_link.reset();
}

TEST_CASE(control_server_shutdown_ends_waiting_calls)
{
    // A client waits for an operation that will not finish (the agent never answers); shutting
    // the server down ends the wait at once instead of after the client's timeout.
    auto server = std::make_unique<control_grpc::ControlServer>(control_grpc::ServerOptions{.listen = "127.0.0.1:0"});
    const std::string address = "127.0.0.1:" + std::to_string(server->port());
    control::PeerHello hello;
    hello.participant = "Idle";
    hello.incarnation = "1";
    control::VariableInfo x;
    x.name = "x";
    hello.schema = {x};
    server->hub().agent_connected(hello, std::make_shared<SilentLink>());
    const auto seq = server->hub().write(control::OperationRequest{{{"Idle", "x", 1.0}}});
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
    CHECK(!returned.load());
    const auto begin = std::chrono::steady_clock::now();
    server->shutdown();
    CHECK(std::chrono::steady_clock::now() - begin < 5s);
    CHECK(eventually([&] { return returned.load(); }));
    waiter.join();
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
