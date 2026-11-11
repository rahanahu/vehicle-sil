// SPDX-License-Identifier: MIT
//
// vehicle_sil_control_demo: a small simulation that a Control API client can drive. One process
// runs a SIL Kit registry, the control server with the controller's time gate (it starts
// paused), and two J1939 peers whose ECUs declare control variables (control_apps.hpp):
//   Engine  engine.throttle (input, %), engine.speed (output, rpm); broadcasts its speed
//   Body    cabin.parking_brake (input), cabin.lamp (enum input), body.engine_speed (output:
//           the speed it received)
// It prints "control server on port N" and runs until Ctrl+C / SIGTERM or until a client stops
// the simulation.
//
//   vehicle_sil_control_demo [--listen 127.0.0.1:8510]
//   python -m vehicle_sil_control peers
#include "control_apps.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/control_grpc/agent_client.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <silkit/SilKit.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;

std::atomic<bool> g_interrupted{false};

void on_signal(int)
{
    g_interrupted = true;
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

} // namespace

int main(int argc, char** argv)
{
    std::string listen = "127.0.0.1:8510";
    try
    {
        const cli::Args args(argc, argv);
        listen = args.str("listen", listen);
    }
    catch (const cli::UsageError& e)
    {
        std::fprintf(stderr, "error: %s\nusage: vehicle_sil_control_demo [--listen host:port]\n", e.what());
        return cli::kExitUsage;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto registry = SilKit::Vendor::Vector::CreateSilKitRegistry(SilKit::Config::ParticipantConfigurationFromString("{}"));
    const auto uri = registry->StartListening("silkit://127.0.0.1:0");

    SimulationControllerOptions controlOptions;
    controlOptions.registry_uri = uri;
    controlOptions.required_participants = {"Engine", "Body"};
    controlOptions.time_control = TimeControlOptions{.step = 10ms, .start_paused = true};
    SimulationController controller(controlOptions);
    const auto time = control_grpc::time_control_of(controller);
    control_grpc::ControlServer server({.listen = listen, .time = time.get()});
    const std::string address = "127.0.0.1:" + std::to_string(server.port());

    // Each peer owns its ECU (emplace()); the ECU declares its variables on the peer's agent.
    Peer engine(coordinated("Engine", uri, 10ms));
    engine.emplace<control_example::Engine>(engine);
    Peer body(coordinated("Body", uri, 5ms));
    body.emplace<control_example::Body>(body);
    // The links complete the agents' schemas: made after the ECUs declared their variables (and
    // before the start, so that they connect first), destroyed before the peers.
    control_grpc::AgentClient engine_link(engine.agent(), {.server = address, .participant = "Engine",
                                                           .status = [&] { return control::PeerStatus{engine.failed(), engine.error()}; }});
    control_grpc::AgentClient body_link(body.agent(), {.server = address, .participant = "Body",
                                                       .status = [&] { return control::PeerStatus{body.failed(), body.error()}; }});

    std::printf("control server on port %d\n", server.port());
    std::fflush(stdout);
    start_together(controller, {&engine, &body});
    std::printf("simulation started (paused at 0)\n");
    std::fflush(stdout);

    while (!g_interrupted && !controller.wait_until_finished(50ms))
    {
    }
    controller.stop("demo ends");
    engine.stop(); // cancels what is still waiting on the agents, before the links stop:
    body.stop();   // they send those results
    engine_link.stop();
    body_link.stop();
    server.shutdown();
    std::printf("stopped\n");
    return 0;
}
