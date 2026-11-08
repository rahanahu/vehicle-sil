// SPDX-License-Identifier: MIT
//
// vehicle_sil_control_demo: a small simulation that a Control API client can drive. One process
// runs a SIL Kit registry, the control server with the controller's time gate (it starts
// paused), and two J1939 peers with control agents:
//   Engine  engine.throttle (input, %), engine.speed (output, rpm); broadcasts its speed
//   Body    cabin.parking_brake (input), cabin.lamp (enum input), body.engine_speed (output:
//           the speed it received)
// It prints "control server on port N" and runs until Ctrl+C / SIGTERM or until a client stops
// the simulation.
//
//   vehicle_sil_control_demo [--listen 127.0.0.1:8510]
//   python -m vehicle_sil_control peers
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/control/peer_agent.hpp"
#include "vehicle_sil/control_grpc/agent_client.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <silkit/SilKit.hpp>
#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;

constexpr j1939::Pgn kPgnEngineSpeed{0xFF10}; // proprietary B

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

enum class Lamp : std::uint8_t
{
    Off = 0,
    Low = 1,
    High = 2,
};

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

    // The models and their agents (before the peers).
    double throttle = 0.0;
    double rpm = 800.0;
    bool parking_brake = true;
    Lamp lamp = Lamp::Off;
    double received_rpm = 0.0;
    control::Agent engine_agent;
    engine_agent.input("engine.throttle", &throttle, {.unit = "%", .min = 0.0, .max = 100.0, .step = 1.0, .description = "Accelerator pedal"});
    engine_agent.output("engine.speed", &rpm, {.unit = "rpm"});
    control::Agent body_agent;
    body_agent.input("cabin.parking_brake", &parking_brake, {.description = "Parking brake switch"});
    body_agent.input_enum("cabin.lamp", &lamp, {{"off", 0}, {"low", 1}, {"high", 2}}, {.description = "Head lamp switch"});
    body_agent.output("body.engine_speed", &received_rpm, {.unit = "rpm", .description = "Engine speed received on J1939"});

    Peer engine(coordinated("Engine", uri, 10ms));
    auto& ecu = engine.j1939("J1939_CAN").add_node({0x00, j1939::Name{}.set_identity_number(0x100).set_manufacturer_code(0x7F0)});
    engine.on_step([&](SimTime) {
        // A first-order engine: 800 rpm idle, 6000 rpm at full throttle.
        const double target = 800.0 + 52.0 * throttle;
        rpm += (target - rpm) * 0.05;
    });
    engine.every(20ms, [&](SimTime) {
        const auto value = static_cast<std::uint16_t>(std::clamp(rpm, 0.0, 65535.0));
        const std::uint8_t data[2]{static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>(value >> 8)};
        static_cast<void>(ecu.send(kPgnEngineSpeed, data));
    });
    Peer body(coordinated("Body", uri, 5ms));
    auto& listener = body.j1939("J1939_CAN").add_node({0x21});
    listener.on_message(kPgnEngineSpeed, [&](const j1939::Message& m) {
        if (m.data.size() >= 2)
        {
            received_rpm = static_cast<double>(m.data[0] | (m.data[1] << 8));
        }
    });
    control::attach(engine, engine_agent);
    control::attach(body, body_agent);
    control_grpc::AgentClient engine_link(engine_agent, {.server = address, .participant = "Engine",
                                                         .status = [&] { return control::PeerStatus{engine.failed(), engine.error()}; }});
    control_grpc::AgentClient body_link(body_agent, {.server = address, .participant = "Body",
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
    engine.stop();
    body.stop();
    engine_agent.finish(); // before the links stop: they send the last results
    body_agent.finish();
    engine_link.stop();
    body_link.stop();
    server.shutdown();
    std::printf("stopped\n");
    return 0;
}
