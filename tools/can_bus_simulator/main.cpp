// SPDX-License-Identifier: MIT
//
// vehicle_sil_can_bus_simulator: a simple CAN network simulator participant (arbitration,
// transmission time, bus occupancy) for SIL Kit simulations with virtual time. It takes the
// same command line and network simulator configuration file as the Vector SIL Kit Network
// Simulator, so one can replace the other by swapping the executable.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/can_netsim/can_bus_simulator.hpp"
#include "vehicle_sil/can_netsim/network_simulator_config.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/SilKitVersion.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef VEHICLE_SIL_VERSION
#define VEHICLE_SIL_VERSION "unknown"
#endif

namespace {

using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;

std::atomic<bool> g_interrupted{false};

void on_signal(int)
{
    g_interrupted.store(true);
}

enum class Level
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

const std::map<std::string, Level, std::less<>> kLevels{{"trace", Level::Trace}, {"debug", Level::Debug},
                                                         {"info", Level::Info},   {"warn", Level::Warn},
                                                         {"error", Level::Error}, {"critical", Level::Critical},
                                                         {"off", Level::Off}};

Level g_level = Level::Info;

void log(Level level, const std::string& text)
{
    if (level < g_level)
    {
        return;
    }
    const char* tag = level >= Level::Error ? "error" : level == Level::Warn ? "warn" : "info";
    std::printf("[can-bus-simulator] %s: %s\n", tag, text.c_str());
    std::fflush(stdout);
}

void print_usage(std::FILE* to)
{
    std::fputs(
        "usage: vehicle_sil_can_bus_simulator [--version] [--help] [--name <participantName>]\n"
        "           [--connect-uri <silkitUri>] [--log <level>] [--participant-configuration <configuration>]\n"
        "           [--default-bitrate <bit/s>] [--transmit-queue <frames>] [--start-timeout-ms <ms>] <configuration>\n"
        "  -v, --version                   print the version and exit\n"
        "  -h, --help                      print this help and exit\n"
        "  -n, --name <name>               participant name (default: NetworkSimulator)\n"
        "  -u, --connect-uri <uri>         registry URI (default: silkit://localhost:8500)\n"
        "  -l, --log <level>               trace, debug, info, warn, error, critical or off (default: info);\n"
        "                                  also SIL Kit's log level on stdout without -c\n"
        "  -c, --participant-configuration <file>  SIL Kit participant configuration (YAML or JSON); relative\n"
        "                                  paths inside it resolve against the current directory\n"
        "  <configuration>                 network simulator configuration (YAML or JSON, the Vector SIL Kit\n"
        "                                  Network Simulator format); only the CAN networks are simulated\n"
        "options of this simulator only:\n"
        "  --default-bitrate <bit/s>       bit rate of a network until a controller sets one (default: 500000)\n"
        "  --transmit-queue <frames>       frames each controller can have waiting for the bus (default: 64)\n"
        "  --start-timeout-ms <ms>         how long to wait for the simulation to start (default: 86400000)\n"
        "exit codes: 0 simulation shut down normally, 1 error during the simulation, 3 setup failure, 4 usage\n",
        to);
}

struct Options
{
    std::string name = "NetworkSimulator";
    std::string uri = "silkit://localhost:8500";
    std::string level = "info";
    std::optional<std::string> participant_configuration;
    std::string configuration;
    std::uint32_t default_bitrate = 500'000;
    std::size_t transmit_queue = 64;
    std::chrono::milliseconds start_timeout{std::chrono::hours(24)};
    bool help = false;
    bool version = false;
};

std::uint64_t parse_number(std::string_view option, std::string_view text, std::uint64_t min, std::uint64_t max)
{
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size() || value < min || value > max)
    {
        throw cli::UsageError(std::string(option) + " needs a number in " + std::to_string(min) + ".." + std::to_string(max) +
                              " (got '" + std::string(text) + "')");
    }
    return value;
}

Options parse(int argc, char** argv)
{
    Options options;
    bool hasConfiguration = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= argc)
            {
                throw cli::UsageError("missing value for " + std::string(arg));
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help")
        {
            options.help = true;
        }
        else if (arg == "-v" || arg == "--version")
        {
            options.version = true;
        }
        else if (arg == "-n" || arg == "--name")
        {
            options.name = value();
        }
        else if (arg == "-u" || arg == "--connect-uri")
        {
            options.uri = value();
        }
        else if (arg == "-l" || arg == "--log")
        {
            options.level = value();
            if (kLevels.find(options.level) == kLevels.end())
            {
                throw cli::UsageError("unknown log level '" + options.level + "'");
            }
        }
        else if (arg == "-c" || arg == "--participant-configuration")
        {
            options.participant_configuration = std::string(value());
        }
        else if (arg == "--default-bitrate")
        {
            options.default_bitrate = static_cast<std::uint32_t>(parse_number(arg, value(), 1, 2'000'000));
        }
        else if (arg == "--transmit-queue")
        {
            options.transmit_queue = static_cast<std::size_t>(parse_number(arg, value(), 1, 1'000'000));
        }
        else if (arg == "--start-timeout-ms")
        {
            options.start_timeout = std::chrono::milliseconds(parse_number(arg, value(), 1, 365ull * 24 * 3600 * 1000));
        }
        else if (arg.size() > 1 && arg[0] == '-')
        {
            throw cli::UsageError("unknown option " + std::string(arg));
        }
        else if (!hasConfiguration)
        {
            options.configuration = arg;
            hasConfiguration = true;
        }
        else
        {
            throw cli::UsageError("unexpected argument " + std::string(arg));
        }
    }
    if (!options.help && !options.version && !hasConfiguration)
    {
        throw cli::UsageError("the network simulator configuration file is missing");
    }
    return options;
}

std::string read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error(path + ": cannot open the file");
    }
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

/// SIL Kit's participant configuration: the given file, or stdout logging at `level`.
std::string participant_configuration(const Options& options)
{
    if (options.participant_configuration)
    {
        return read_file(*options.participant_configuration);
    }
    if (options.level == "off")
    {
        return "{}";
    }
    static const std::map<std::string, std::string, std::less<>> kSilKitLevels{
        {"trace", "Trace"}, {"debug", "Debug"}, {"info", "Info"}, {"warn", "Warn"}, {"error", "Error"}, {"critical", "Critical"}};
    return "{\"Logging\": {\"Sinks\": [{\"Type\": \"Stdout\", \"Level\": \"" + kSilKitLevels.at(options.level) + "\"}]}}";
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    try
    {
        options = parse(argc, argv);
    }
    catch (const cli::UsageError& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        print_usage(stderr);
        return cli::kExitUsage;
    }
    if (options.help)
    {
        print_usage(stdout);
        return cli::kExitOk;
    }
    if (options.version)
    {
        std::printf("vehicle-sil CAN bus simulator %s (SIL Kit %s)\n", VEHICLE_SIL_VERSION, SilKit::Version::String());
        return cli::kExitOk;
    }
    g_level = kLevels.at(options.level);

    can::NetworkSimulatorConfig config;
    std::vector<std::string> networks;
    std::string silkitConfiguration;
    try
    {
        config = can::load_network_simulator_config(options.configuration);
        networks = config.can_networks();
        silkitConfiguration = participant_configuration(options);
    }
    catch (const std::exception& e)
    {
        log(Level::Error, e.what());
        return cli::kExitSetupFailed;
    }
    for (const auto& warning : config.warnings)
    {
        log(Level::Warn, options.configuration + ": " + warning);
    }
    if (networks.empty())
    {
        log(Level::Error, options.configuration + ": no network of type CAN to simulate");
        return cli::kExitSetupFailed;
    }

    // The simulator is declared first, so it is destroyed after the runtime that steps it.
    std::unique_ptr<can::CanBusSimulator> simulator;
    std::unique_ptr<ParticipantRuntime> runtime;
    try
    {
        ParticipantRuntimeOptions runtimeOptions;
        runtimeOptions.participant_name = options.name;
        runtimeOptions.registry_uri = options.uri;
        runtimeOptions.participant_configuration = silkitConfiguration;
        runtimeOptions.lifecycle_mode = LifecycleMode::Coordinated;
        VirtualTimeOptions virtualTime;
        virtualTime.step = config.step_size;
        runtimeOptions.virtual_time = virtualTime;
        runtime = std::make_unique<ParticipantRuntime>(runtimeOptions);

        can::CanBusSimulatorOptions simulatorOptions;
        simulatorOptions.default_bitrate = options.default_bitrate;
        simulatorOptions.transmit_queue_capacity = options.transmit_queue;
        simulator = std::make_unique<can::CanBusSimulator>(runtime->participant(), networks, simulatorOptions,
                                                           [](can::CanBusSimulator::LogLevel level, std::string_view message) {
                                                               log(level == can::CanBusSimulator::LogLevel::Info ? Level::Info : Level::Warn,
                                                                   std::string(message));
                                                           });
        runtime->set_step_handler([&simulator](SimTime now, SimDuration step) { simulator->step(now, step); });

        std::string names;
        for (const auto& network : networks)
        {
            names += (names.empty() ? "" : ", ") + network;
        }
        log(Level::Info, "participant '" + options.name + "' simulates " + names + " with a step of " +
                             std::to_string(to_ns(SimTime{} + config.step_size)) + " ns; waiting for the simulation to start");
        runtime->start(options.start_timeout);
    }
    catch (const std::exception& e)
    {
        log(Level::Error, std::string("setup: ") + e.what());
        return cli::kExitSetupFailed;
    }

    // Installed only now: until the simulation runs, Ctrl+C keeps its default effect.
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    log(Level::Info, "simulation running");

    std::optional<orch::ParticipantState> state;
    while (!(state = runtime->wait_until_finished(std::chrono::milliseconds(50))))
    {
        if (g_interrupted.load())
        {
            log(Level::Info, "interrupted, stopping");
            runtime->stop("vehicle_sil_can_bus_simulator interrupted");
            state = runtime->wait_until_finished(std::chrono::milliseconds(5000));
            break;
        }
    }
    const std::string error = runtime->step_error();
    runtime->stop("vehicle_sil_can_bus_simulator finished");

    for (const auto& network : networks)
    {
        const auto s = simulator->statistics(network);
        log(Level::Info, network + ": bitrate=" + std::to_string(s.bitrate) + " requests=" + std::to_string(s.requests) +
                             " transmissions=" + std::to_string(s.transmissions) + " untagged=" + std::to_string(s.untagged_requests) +
                             " rejected=" + std::to_string(s.rejected_requests) + " queue_full=" + std::to_string(s.transmit_queue_full) +
                             " inbox_overflows=" + std::to_string(s.inbox_overflows) +
                             " bitrate_conflicts=" + std::to_string(s.bitrate_conflicts));
    }
    if (!error.empty())
    {
        log(Level::Error, "step failed: " + error);
        return cli::kExitFailed;
    }
    if (!state || *state != orch::ParticipantState::Shutdown)
    {
        log(Level::Error, "final state " + (state ? orch::to_string(*state) : std::string("unknown")));
        return cli::kExitFailed;
    }
    log(Level::Info, "simulation shut down");
    return cli::kExitOk;
}
