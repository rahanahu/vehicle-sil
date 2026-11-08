// SPDX-License-Identifier: MIT
//
// vehicle_sil_sim_controller: starts a coordinated SIL Kit simulation once all required
// participants are connected, and stops it when virtual time reaches --stop-at-ms (or on
// Ctrl+C / SIGTERM).
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

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
    const std::optional<std::chrono::steady_clock::time_point> deadline =
        options.stop_at ? std::optional(std::chrono::steady_clock::now() + timeout) : std::nullopt;
    std::optional<orch::ParticipantState> state;
    while (!(state = controller->wait_until_finished(std::chrono::milliseconds(50))))
    {
        if (g_interrupted.load())
        {
            log("interrupted, stopping the simulation");
            controller->stop("vehicle_sil_sim_controller interrupted");
            state = controller->wait_until_finished(std::chrono::milliseconds(5000));
            break;
        }
        // A failed participant stalls virtual time for everyone; SIL Kit does not end the run.
        if (const auto failed = controller->failed_participant())
        {
            log("FAIL: participant '" + *failed + "' is in state Error; aborting");
            controller->abort();
            return cli::kExitFailed;
        }
        if (deadline && std::chrono::steady_clock::now() >= *deadline)
        {
            log("FAIL timeout: simulation did not reach --stop-at-ms in time; aborting");
            controller->abort();
            return cli::kExitTimeout;
        }
    }

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
