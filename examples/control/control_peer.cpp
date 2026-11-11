// SPDX-License-Identifier: MIT
//
// vehicle_sil_control_peer: one J1939 peer whose ECU declares control variables, in its own
// process (possibly on another PC). The role decides the ECU (control_apps.hpp):
//   --role engine  engine.throttle (input, %), engine.speed (output, rpm); broadcasts its speed
//   --role body    cabin.parking_brake (input), cabin.lamp (enum input), body.engine_speed (output)
// The peer is Coordinated: start vehicle_sil_control_server --required <participants> first.
//
//   vehicle_sil_control_peer --role engine --participant Engine --registry-uri silkit://host:8500
//                            --control-server host:8510 [--token T] [--root-cert server.crt]
#include "control_apps.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/control_grpc/agent_client.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;

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

    try
    {
        // The peer owns the ECU (emplace()); the ECU declares its variables on the peer's agent.
        Peer peer(options);
        if (role == "engine")
        {
            peer.emplace<control_example::Engine>(peer);
        }
        else
        {
            peer.emplace<control_example::Body>(peer);
        }
        link.status = [&] { return control::PeerStatus{peer.failed(), peer.error()}; };
        // Another process took this participant over at the control server: say why, leave the
        // simulation and end with a failure instead of running on without control.
        link.on_replaced = [&](const std::string& why) {
            std::fprintf(stderr, "[control-peer] FAIL: %s\n", why.c_str());
            std::fflush(stderr);
            peer.request_stop("replaced at the control server");
        };
        // It completes the agent's schema (with the peer's monitoring outputs): after the ECU,
        // before the start (so it connects first); destroyed before the peer.
        control_grpc::AgentClient client(peer.agent(), link);
        log(options.participant + " waits for the simulation to start");
        peer.start(timeout);
        log("running");
        // Until it ends, however long it stands still (--timeout-ms is for the start only).
        decltype(peer.wait()) state;
        while (!(state = peer.wait(std::chrono::minutes(1))))
        {
        }
        peer.stop();   // the steps are over: cancels what is still waiting on the agent ...
        client.stop(); // ... and the client sends those results before it closes
        log(std::string("ended") + (peer.failed() ? ": FAILED " + peer.error() : "")
            + (client.replaced() ? ": FAILED replaced at the control server" : ""));
        return state && !peer.failed() && !client.replaced() ? cli::kExitOk : cli::kExitFailed;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return cli::kExitSetupFailed;
    }
}
