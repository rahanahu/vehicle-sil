// SPDX-License-Identifier: MIT
//
// vehicle_sil_control_peer: one J1939 peer with a control agent, in its own process (possibly on
// another PC). The role decides the model:
//   --role engine  engine.throttle (input, %), engine.speed (output, rpm); broadcasts its speed
//   --role body    cabin.parking_brake (input), cabin.lamp (enum input), body.engine_speed (output)
// The peer is Coordinated: start vehicle_sil_control_server --required <participants> first.
//
//   vehicle_sil_control_peer --role engine --participant Engine --registry-uri silkit://host:8500
//                            --control-server host:8510 [--token T] [--root-cert server.crt]
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/control/peer_agent.hpp"
#include "vehicle_sil/control_grpc/agent_client.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;

constexpr j1939::Pgn kPgnEngineSpeed{0xFF10}; // proprietary B

enum class Lamp : std::uint8_t
{
    Off = 0,
    Low = 1,
    High = 2,
};

void log(const std::string& text)
{
    std::printf("[control-peer] %s\n", text.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    std::string role;
    PeerOptions options;
    control_grpc::AgentClientOptions link;
    std::chrono::milliseconds timeout{};
    try
    {
        const cli::Args args(argc, argv);
        role = args.required("role");
        if (role != "engine" && role != "body")
        {
            throw cli::UsageError("--role must be engine or body");
        }
        options.participant = args.required("participant");
        options.registry_uri = args.required("registry-uri");
        options.lifecycle = LifecycleMode::Coordinated;
        options.virtual_time = VirtualTimeOptions{role == "engine" ? SimDuration(10ms) : SimDuration(5ms), std::nullopt, std::nullopt};
        link.server = args.required("control-server");
        link.participant = options.participant;
        link.token = args.str("token", "");
        if (args.has("root-cert"))
        {
            std::ifstream in(args.required("root-cert"), std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            link.root_certificates_pem = text.str();
            if (!in || link.root_certificates_pem.empty())
            {
                throw cli::UsageError("cannot read the root certificate " + args.required("root-cert"));
            }
        }
        link.tls = args.str("tls", link.root_certificates_pem.empty() ? "0" : "1") == "1";
        timeout = args.millis("timeout-ms", std::chrono::milliseconds(600000));
    }
    catch (const cli::UsageError& e)
    {
        std::fprintf(stderr,
                     "error: %s\nusage: vehicle_sil_control_peer --role engine|body --participant <name> --registry-uri <uri>\n"
                     "       --control-server <host:port> [--token <t>] [--root-cert <pem file>] [--tls 0|1] [--timeout-ms <ms>]\n"
                     "       (--tls 1 without --root-cert: the system's root certificates)\n",
                     e.what());
        return cli::kExitUsage;
    }

    // Models and the agent before the peer.
    double throttle = 0.0;
    double rpm = 800.0;
    bool parking_brake = true;
    Lamp lamp = Lamp::Off;
    double received_rpm = 0.0;
    control::Agent agent;
    if (role == "engine")
    {
        agent.input("engine.throttle", &throttle, {.unit = "%", .min = 0.0, .max = 100.0, .step = 1.0, .description = "Accelerator pedal"});
        agent.output("engine.speed", &rpm, {.unit = "rpm"});
    }
    else
    {
        agent.input("cabin.parking_brake", &parking_brake, {.description = "Parking brake switch"});
        agent.input_enum("cabin.lamp", &lamp, {{"off", 0}, {"low", 1}, {"high", 2}}, {.description = "Head lamp switch"});
        agent.output("body.engine_speed", &received_rpm, {.unit = "rpm", .description = "Engine speed received on J1939"});
    }

    try
    {
        Peer peer(options);
        if (role == "engine")
        {
            auto& ecu = peer.j1939("J1939_CAN").add_node({0x00, j1939::Name{}.set_identity_number(0x100).set_manufacturer_code(0x7F0)});
            peer.on_step([&](SimTime) { rpm += (800.0 + 52.0 * throttle - rpm) * 0.05; });
            peer.every(20ms, [&](SimTime) {
                const auto value = static_cast<std::uint16_t>(std::clamp(rpm, 0.0, 65535.0));
                const std::uint8_t data[2]{static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>(value >> 8)};
                static_cast<void>(ecu.send(kPgnEngineSpeed, data));
            });
        }
        else
        {
            auto& node = peer.j1939("J1939_CAN").add_node({0x21});
            node.on_message(kPgnEngineSpeed, [&](const j1939::Message& m) {
                if (m.data.size() >= 2)
                {
                    received_rpm = static_cast<double>(m.data[0] | (m.data[1] << 8));
                }
            });
        }
        control::attach(peer, agent);
        link.status = [&] { return control::PeerStatus{peer.failed(), peer.error()}; };
        control_grpc::AgentClient client(agent, link);
        log(options.participant + " waits for the simulation to start");
        peer.start(timeout);
        log("running");
        // Until it ends, however long it stands still (--timeout-ms is for the start only).
        decltype(peer.wait()) state;
        while (!(state = peer.wait(std::chrono::minutes(1))))
        {
        }
        peer.stop(); // the steps are over: finish() may run now
        agent.finish(); // cancels what is still waiting ...
        client.stop();  // ... and the client sends those results before it closes
        log(std::string("ended") + (peer.failed() ? ": FAILED " + peer.error() : ""));
        return state && !peer.failed() ? cli::kExitOk : cli::kExitFailed;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return cli::kExitSetupFailed;
    }
}
