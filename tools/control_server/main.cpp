// SPDX-License-Identifier: MIT
//
// vehicle_sil_control_server: the Control API server. With --required it also coordinates the
// simulation (a SimulationController with its time gate): it starts the simulation once the
// required participants are connected, and clients can pause, step and pace it.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/core/failure_check.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <atomic>
#include <chrono>
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

namespace {

using namespace vehicle_sil;

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

void print_usage()
{
    std::fputs("usage: vehicle_sil_control_server [options]\n"
               "  --listen <host:port>     gRPC address (default 127.0.0.1:8510; port 0: any free port)\n"
               "  --registry-uri <uri>     SIL Kit registry (with --required)\n"
               "  --required <a,b,...>     coordinate a simulation of these participants (they use LifecycleMode::Coordinated)\n"
               "  --participant <name>     participant name of the controller (ControlServer)\n"
               "  --gate-step-ms <ms>      step of the time gate: the simulation can stand still at multiples of it (10)\n"
               "  --start-paused <0|1>     stand still at T = 0 until a client resumes (0)\n"
               "  --speed <factor>         initial speed, simulation seconds per wall-clock second (default: as fast as possible)\n"
               "  --stop-at-ms <ms>        stop the simulation at this time\n"
               "  --timeout-ms <ms>        how long to wait for the required participants (600000)\n"
               "  --credentials <file>     tokens and roles, one per line: <role> <name> <token> [participants]\n"
               "  --tls-cert <file>        PEM certificate chain of the server (with --tls-key: TLS)\n"
               "  --tls-key <file>         PEM private key of the server\n"
               "  --allow-insecure <0|1>   allow a non-loopback address without TLS and credentials (0)\n"
               "  --audit-log <file>       append who changed what (default: standard output)\n"
               "The server runs until Ctrl+C / SIGTERM (or, with --required, until the simulation ended).\n"
               "With --required, a required participant that leaves the running simulation in state Error fails it at\n"
               "once; one that leaves in another state fails it if the simulation has not ended 5 s later.\n"
               "exit codes: 0 ok, 1 the simulation failed, 2 timeout, 3 setup failure, 4 usage\n",
               stderr);
}

} // namespace

int main(int argc, char** argv)
{
    control_grpc::ServerOptions serverOptions;
    serverOptions.listen = "127.0.0.1:8510";
    std::optional<SimulationControllerOptions> coordination;
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
            c.registry_uri = args.required("registry-uri");
            c.required_participants = args.list("required");
            c.participant_name = args.str("participant", "ControlServer");
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
    std::unique_ptr<SimulationController> controller;
    std::unique_ptr<control_grpc::TimeControl> time;
    std::unique_ptr<control_grpc::ControlServer> server;
    try
    {
        if (coordination)
        {
            controller = std::make_unique<SimulationController>(*coordination);
            time = control_grpc::time_control_of(*controller);
            serverOptions.time = time.get();
        }
        server = std::make_unique<control_grpc::ControlServer>(serverOptions);
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
        std::atomic<bool> started{false};
        std::atomic<bool> startFailed{false};
        std::thread starter([&] {
            try
            {
                controller->start(timeout);
                started = true;
            }
            catch (const std::exception& e)
            {
                log(std::string("FAIL start: ") + e.what());
                startFailed = true;
            }
        });
        while (!g_interrupted && !started && !startFailed)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (g_interrupted && !started && !startFailed)
        {
            log("interrupted while waiting for the participants");
            std::_Exit(cli::kExitFailed); // SimulationController::start() cannot be interrupted
        }
        starter.join();
        if (startFailed)
        {
            return cli::kExitTimeout;
        }
        log("simulation started");
        FailureCheck check;
        while (!g_interrupted)
        {
            if (const auto failed = check.check(*controller, std::chrono::steady_clock::now()))
            {
                log("FAIL: participant '" + failed->participant + "' " + failed->reason + "; aborting");
                controller->abort();
                exitCode = cli::kExitFailed;
                break;
            }
            if (controller->wait_until_finished(std::chrono::milliseconds(50)))
            {
                log("simulation ended");
                break;
            }
        }
        controller->stop("control server ends");
        // A moment for the agents' last results and values before the server goes.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    else
    {
        while (!g_interrupted)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    server->shutdown();
    log("stopped");
    return exitCode;
}
