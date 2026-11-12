// SPDX-License-Identifier: MIT
//
// vehicle_sil_control_server: the Control API server of a SIL Kit simulation that
// vehicle_sil_launch does not run. It joins the simulation as a participant of its own and
// connects the ECUs' control agents over SIL Kit (control_silkit::HubTransport), as the launcher
// does. With --required its participant is the simulation's controller (a SimulationController
// with its time gate): it starts the simulation once the required participants are there, and
// clients can pause, step and pace it. Without --required it joins as an autonomous participant
// in wall-clock time: clients read and write the variables, the time is not theirs.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;

std::atomic<bool> g_interrupted{false};

void on_signal(int)
{
    g_interrupted.store(true);
}

void log(const std::string& text)
{
    std::printf("[control-server] %s\n", text.c_str());
    std::fflush(stdout);
}

/// How long the window before the start waits for the agents that have said hello to answer
/// that they receive commands (they normally have at once; see wait_for_agents()).
constexpr std::chrono::seconds kAgentAnswerLimit{5};

void print_usage()
{
    std::fputs("usage: vehicle_sil_control_server --registry-uri <uri> [options]\n"
               "  --registry-uri <uri>     SIL Kit registry of the simulation: the server joins it to reach the ECUs'\n"
               "                           control agents (ecu_main offers an ECU's variables unless --no-control)\n"
               "  --listen <host:port>     gRPC address (default 127.0.0.1:8510; port 0: any free port)\n"
               "  --required <a,b,...>     coordinate a simulation of these participants (they use LifecycleMode::Coordinated);\n"
               "                           without it the server joins as an autonomous participant and does not control the time\n"
               "  --participant <name>     participant name of the server (ControlServer)\n"
               "  --gate-step-ms <ms>      step of the time gate: the simulation can stand still at multiples of it (10)\n"
               "  --start-paused <0|1>     stand still at T = 0 until a client resumes (0)\n"
               "  --speed <factor>         initial speed, simulation seconds per wall-clock second (default: as fast as possible)\n"
               "  --stop-at-ms <ms>        stop the simulation at this time\n"
               "  --timeout-ms <ms>        how long to wait for the required participants (600000)\n"
               "  --credentials <file>     tokens and roles of the clients, one per line: <role> <name> <token>\n"
               "  --tls-cert <file>        PEM certificate chain of the server (with --tls-key: TLS)\n"
               "  --tls-key <file>         PEM private key of the server\n"
               "  --allow-insecure <0|1>   allow a non-loopback address without TLS and credentials (0)\n"
               "  --audit-log <file>       append who changed what (default: standard output)\n"
               "The server runs until Ctrl+C / SIGTERM (or, with --required, until the simulation ended).\n"
               "With --required, a required participant that leaves the running simulation in state Error fails it at\n"
               "once; one that leaves in another state fails it if the simulation has not ended 5 s later.\n"
               "exit codes: 0 ok, 1 the simulation failed or was interrupted before it started, 2 timeout,\n"
               "3 setup failure, 4 usage\n",
               stderr);
}

std::string joined(const std::vector<std::string>& names)
{
    std::string out;
    for (const auto& n : names)
    {
        out += (out.empty() ? "" : ", ") + n;
    }
    return out;
}

/// The window before the start (on the controller's window thread): every required participant
/// waits before its first step with its publish/subscribe services connected. Waits until the
/// agents that have said hello answer that they receive commands, so that a client's write
/// before the first step reaches them, and says who is connected.
void wait_for_agents(control_silkit::HubTransport& transport, const std::vector<std::string>& required, const StartWindow& window)
{
    const auto deadline = std::min(std::chrono::steady_clock::now() + kAgentAnswerLimit, window.deadline());
    const control_silkit::HelloReport report =
        transport.wait_for_announced(required, deadline, [&window] { return window.cancelled() || g_interrupted.load(); });
    if (report.cancelled)
    {
        return;
    }
    if (!report.connected.empty())
    {
        log("control agents connected: " + joined(report.connected));
    }
    if (!report.missing.empty())
    {
        log("the control agents of " + joined(report.missing) + " said hello but did not answer within " +
            std::to_string(kAgentAnswerLimit.count()) + " s that they receive commands: writes to them are refused until they do");
    }
    if (!report.mismatched.empty())
    {
        log("the control agents of " + joined(report.mismatched) +
            " use another version of the control messages than this server: their variables cannot be read or written; "
            "build the ECUs and the server from the same vehicle-sil");
    }
    if (!report.ended.empty())
    {
        log("left before the start: " + joined(report.ended));
    }
    if (!report.not_announced.empty())
    {
        log("no hello from a control agent of " + joined(report.not_announced) +
            " before the start (no variables, or run with --no-control; one that says hello later is connected then)");
    }
}

} // namespace

int main(int argc, char** argv)
{
    control_grpc::ServerOptions serverOptions;
    serverOptions.listen = "127.0.0.1:8510";
    std::optional<SimulationControllerOptions> coordination;
    ParticipantRuntimeOptions own;
    std::string auditPath;
    std::chrono::milliseconds timeout{};
    try
    {
        const cli::Args args(argc, argv);
        serverOptions.listen = args.str("listen", serverOptions.listen);
        const auto read_file = [](const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                throw cli::UsageError("cannot read " + path);
            }
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        };
        if (!args.has("registry-uri"))
        {
            throw cli::UsageError("--registry-uri is missing: the server joins the SIL Kit simulation to reach the ECUs' "
                                  "control agents (for example --registry-uri silkit://localhost:8500)");
        }
        own.registry_uri = args.required("registry-uri");
        own.participant_name = args.str("participant", "ControlServer");
        if (args.has("credentials"))
        {
            serverOptions.security.credentials = control_grpc::load_credentials(args.required("credentials"));
        }
        if (args.has("tls-cert") || args.has("tls-key"))
        {
            serverOptions.security.certificate_chain_pem = read_file(args.required("tls-cert"));
            serverOptions.security.private_key_pem = read_file(args.required("tls-key"));
        }
        serverOptions.security.allow_insecure = args.str("allow-insecure", "0") == "1";
        auditPath = args.str("audit-log", "");
        timeout = args.millis("timeout-ms", std::chrono::milliseconds(600000));
        if (args.has("required"))
        {
            SimulationControllerOptions c;
            c.registry_uri = own.registry_uri;
            c.required_participants = args.list("required");
            c.participant_name = own.participant_name;
            TimeControlOptions t;
            t.step = args.millis("gate-step-ms", std::chrono::milliseconds(10));
            t.start_paused = args.str("start-paused", "0") == "1";
            if (args.has("speed"))
            {
                t.speed = std::stod(args.required("speed"));
            }
            c.time_control = t;
            if (const auto stopAt = args.optional_millis("stop-at-ms"))
            {
                c.stop_at = *stopAt;
            }
            coordination = c;
        }
        else
        {
            for (const char* option : {"gate-step-ms", "start-paused", "speed", "stop-at-ms"})
            {
                if (args.has(option))
                {
                    throw cli::UsageError(std::string("--") + option +
                                          " needs --required: only a server that coordinates the simulation controls its time");
                }
            }
        }
    }
    catch (const cli::UsageError& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        print_usage();
        return cli::kExitUsage;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        print_usage();
        return cli::kExitUsage;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto auditFile = std::make_shared<std::ofstream>();
    auto auditMutex = std::make_shared<std::mutex>();
    if (!auditPath.empty())
    {
        auditFile->open(auditPath, std::ios::app);
        if (!*auditFile)
        {
            log("FAIL setup: cannot open " + auditPath);
            return cli::kExitSetupFailed;
        }
    }
    serverOptions.audit = [auditFile, auditMutex](const std::string& line) {
        const std::lock_guard lock(*auditMutex);
        if (auditFile->is_open())
        {
            *auditFile << line << std::endl;
        }
        else
        {
            log("audit: " + line);
        }
    };
    // In this order, so that they go in the other: the transport before the server's hub and
    // before the participant it is on, the server before the time control it uses.
    std::unique_ptr<SimulationController> controller;
    std::unique_ptr<ParticipantRuntime> runtime; // without --required: the server's own participant
    std::unique_ptr<control_grpc::TimeControl> time;
    std::unique_ptr<control_grpc::ControlServer> server;
    std::unique_ptr<control_silkit::HubTransport> transport;
    try
    {
        if (coordination)
        {
            controller = std::make_unique<SimulationController>(*coordination);
            time = control_grpc::time_control_of(*controller);
            serverOptions.time = time.get();
        }
        else
        {
            runtime = std::make_unique<ParticipantRuntime>(own);
        }
        server = std::make_unique<control_grpc::ControlServer>(serverOptions);
        // Before the participant starts: SIL Kit connects the services made before the start with
        // every participant.
        transport = std::make_unique<control_silkit::HubTransport>(server->hub(), controller ? controller->runtime() : *runtime);
        if (controller)
        {
            controller->set_start_window([t = transport.get(), required = coordination->required_participants](const StartWindow& window) {
                wait_for_agents(*t, required, window);
            });
        }
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL setup: ") + e.what());
        return cli::kExitSetupFailed;
    }
    log("listening on port " + std::to_string(server->port()));

    int exitCode = cli::kExitOk;
    if (controller)
    {
        try
        {
            controller->start(timeout, [] { return g_interrupted ? std::string("interrupted") : std::string(); });
        }
        catch (const std::exception& e)
        {
            if (g_interrupted)
            {
                log("interrupted while waiting for the participants");
                return cli::kExitFailed;
            }
            log(std::string("FAIL start: ") + e.what());
            return cli::kExitTimeout;
        }
        log("simulation started");
        // The server's monitor tells when the simulation changed (a failure, the end).
        std::mutex changedMutex;
        std::condition_variable changedCv;
        bool changed = false;
        auto* monitor = server->simulation();
        auto subscription = monitor->subscribe([&](const control_grpc::SimulationSnapshot&) {
            {
                const std::lock_guard lock(changedMutex);
                changed = true;
            }
            changedCv.notify_one();
        });
        while (!g_interrupted)
        {
            const auto snapshot = monitor->snapshot();
            if (snapshot.failure)
            {
                log("FAIL: participant '" + snapshot.failure->participant + "' " + snapshot.failure->reason + "; aborting");
                controller->abort();
                exitCode = cli::kExitFailed;
                break;
            }
            if (snapshot.ended)
            {
                log("simulation ended");
                break;
            }
            // In slices: a signal handler cannot notify the condition variable.
            std::unique_lock lock(changedMutex);
            changedCv.wait_for(lock, std::chrono::milliseconds(50), [&] { return changed; });
            changed = false;
        }
        subscription.reset();
        controller->stop("control server ends");
        // A moment for the agents' last results and values before the server goes.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    else
    {
        try
        {
            runtime->start(timeout, [] { return g_interrupted ? std::string("interrupted") : std::string(); });
        }
        catch (const std::exception& e)
        {
            log(std::string("FAIL start: ") + e.what());
            return g_interrupted ? cli::kExitFailed : cli::kExitSetupFailed;
        }
        log("joined the simulation as " + own.participant_name);
        while (!g_interrupted)
        {
            // Its own participant has left (an error, a lost connection): the agents it reached
            // are out of reach, and serving their last values would mislead the clients.
            if (const auto state = runtime->state(); has_finished(PeerStatus{state}))
            {
                log("FAIL: the server's participant '" + own.participant_name + "' left the simulation (state " +
                    std::string(orch::to_string(state)) +
                    "): the simulation was stopped or aborted, or the participant failed; the control agents are out of reach");
                exitCode = cli::kExitFailed;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    server->shutdown();
    // What is still open for the agents is settled here; then the participant leaves.
    transport.reset();
    if (runtime)
    {
        runtime->stop("control server ends");
    }
    log("stopped");
    return exitCode;
}
