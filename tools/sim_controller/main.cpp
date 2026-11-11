// SPDX-License-Identifier: MIT
//
// vehicle_sil_sim_controller: starts a coordinated SIL Kit simulation once all required
// participants are connected, and stops it when virtual time reaches --stop-at-ms (or on
// Ctrl+C / SIGTERM).
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/simulation_watch.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

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
    std::printf("[sim-controller] %s\n", text.c_str());
    std::fflush(stdout);
}

void print_usage()
{
    std::fputs("usage: vehicle_sil_sim_controller --registry-uri <uri> --required <name,name,...> [options]\n"
               "  --participant <name>   participant name of the controller (SimulationController)\n"
               "  --stop-at-ms <ms>      stop when virtual time reaches this value (default: run until Ctrl+C)\n"
               "  --timeout-ms <ms>      wall-clock limit for start-up and, with --stop-at-ms, for the whole run (60000)\n"
               "a required participant that leaves the running simulation in state Error fails it at once; one that\n"
               "leaves in another state fails it if the simulation has not ended 5 s later\n"
               "exit codes: 0 simulation shut down normally, 1 a participant failed or the run ended abnormally,\n"
               "            2 timeout, 3 setup failure, 4 usage\n",
               stderr);
}

} // namespace

int main(int argc, char** argv)
{
    SimulationControllerOptions options;
    std::chrono::milliseconds timeout{};
    try
    {
        const cli::Args args(argc, argv);
        options.registry_uri = args.required("registry-uri");
        options.required_participants = args.list("required");
        options.participant_name = args.str("participant", options.participant_name);
        if (const auto stopAt = args.optional_millis("stop-at-ms"))
        {
            options.stop_at = *stopAt;
        }
        timeout = args.millis("timeout-ms", std::chrono::milliseconds(60000));
    }
    catch (const cli::UsageError& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        print_usage();
        return cli::kExitUsage;
    }

    std::unique_ptr<SimulationController> controller;
    try
    {
        controller = std::make_unique<SimulationController>(options);
    }
    catch (const std::invalid_argument& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        print_usage();
        return cli::kExitUsage;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL setup: ") + e.what());
        return cli::kExitSetupFailed;
    }
    try
    {
        std::string names;
        for (const auto& name : options.required_participants)
        {
            names += (names.empty() ? "" : ",") + name;
        }
        log("waiting for " + names);
        controller->start(timeout);
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL setup: ") + e.what());
        return cli::kExitSetupFailed;
    }
    // Installed only now: until the simulation runs, Ctrl+C keeps its default effect and ends
    // the process at once (start() cannot be interrupted).
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    log("simulation running");

    // Without --stop-at-ms the simulation runs until it is interrupted.
    WatchOptions watch;
    if (options.stop_at)
    {
        watch.deadline = std::chrono::steady_clock::now() + timeout;
    }
    watch.interrupted = [] {
        if (!g_interrupted.load())
        {
            return false;
        }
        log("interrupted, stopping the simulation");
        return true;
    };
    watch.stop_reason = "vehicle_sil_sim_controller interrupted";
    // The reason first: abort() may take a while.
    watch.before_abort = [](const WatchResult& why) {
        if (why.end == WatchEnd::TimedOut)
        {
            log("FAIL timeout: simulation did not reach --stop-at-ms in time; aborting");
        }
        else if (why.failure)
        {
            log("FAIL: participant '" + why.failure->participant + "' " + why.failure->reason + "; aborting");
        }
        else
        {
            log("FAIL: a participant failed; aborting");
        }
    };
    const WatchResult watched = watch_simulation(*controller, watch);
    if (watched.end == WatchEnd::Failed)
    {
        return cli::kExitFailed;
    }
    if (watched.end == WatchEnd::TimedOut)
    {
        return cli::kExitTimeout;
    }
    const std::optional<orch::ParticipantState> state = watched.state;

    if (const auto wall = controller->wall_time_to_stop_at())
    {
        const auto simMs = options.stop_at->count() / 1'000'000;
        const auto wallUs = std::chrono::duration_cast<std::chrono::microseconds>(*wall).count();
        std::printf("[sim-controller] result sim_ms=%lld wall_us=%lld speed=%.3f\n", static_cast<long long>(simMs),
                    static_cast<long long>(wallUs), wallUs > 0 ? static_cast<double>(simMs) * 1000.0 / static_cast<double>(wallUs) : 0.0);
        std::fflush(stdout);
    }
    if (!state || *state != orch::ParticipantState::Shutdown)
    {
        log("FAIL: final state " + (state ? orch::to_string(*state) : std::string("unknown")));
        return cli::kExitFailed;
    }
    log("simulation shut down");
    return cli::kExitOk;
}
