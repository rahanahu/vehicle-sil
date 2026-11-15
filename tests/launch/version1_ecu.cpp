// SPDX-License-Identifier: MIT
//
// An ECU executable that offers its variables as an ECU built with vehicle-sil 0.1.0 does: its
// --describe says "wire_version": 1, and its control agent speaks version 1 of the control
// messages and drops everything else (tests/control_silkit/version1_agent.hpp, where that rule
// is written out by hand). For the test that such an executable still works under
// vehicle_sil_launch run. The product has no way to make an ECU speak version 1, so this one does
// not use ecu_main: it takes the arguments vehicle_sil_launch gives an ECU it describes
// (--describe, or --name, --registry-uri and --start-timeout-ms) and nothing else.
//
// It steps every 10 ms and has the input "level" (an integer, 0 to 100) and the output "doubled"
// (twice the level its last step saw). With the file "<name>.refuse-13" in its working directory,
// a level of 13 is refused (the setter throws), as a write that fails in a step. With the file
// "<name>.silent", it has no control agent's transport at all (it never says hello), although its
// --describe offers the variables.
#include "version1_agent.hpp"

#include "vehicle_sil/peer/peer.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;
    bool describe = false;
    std::string name = "Version1Ecu";
    std::string registry_uri;
    long long start_timeout_ms = 30'000;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--describe")
        {
            describe = true;
        }
        else if (arg == "--name" && has_value)
        {
            name = argv[++i];
        }
        else if (arg == "--registry-uri" && has_value)
        {
            registry_uri = argv[++i];
        }
        else if (arg == "--start-timeout-ms" && has_value)
        {
            start_timeout_ms = std::stoll(argv[++i]);
        }
        else
        {
            std::fprintf(stderr, "error: unknown argument \"%s\"\n", arg.c_str());
            return 4;
        }
    }
    if (describe)
    {
        const auto variable = [](const char* variable_name, const char* causality, const char* min, const char* max) {
            return std::string("{\"name\": \"") + variable_name + "\", \"type\": \"integer\", \"causality\": \"" + causality +
                   "\", \"unit\": \"\", \"min\": " + min + ", \"max\": " + max +
                   ", \"step\": null, \"description\": \"\", \"items\": [], \"builtin\": false}";
        };
        std::printf("{\"vehicle_sil_ecu\": 1, \"vehicle_sil\": \"0.1.0\", \"name\": \"%s\", \"step_ns\": 10000000, "
                    "\"networks\": [], \"variables\": [%s, %s], "
                    "\"control\": {\"transport\": \"silkit\", \"wire_version\": 1}, \"features\": [\"map-network\"]}\n",
                    name.c_str(), variable("level", "input", "0", "100").c_str(),
                    variable("doubled", "output", "null", "null").c_str());
        return 0;
    }
    if (registry_uri.empty())
    {
        std::fprintf(stderr, "error: one of --describe and --registry-uri is needed\n");
        return 4;
    }
    try
    {
        vehicle_sil::PeerOptions options;
        options.participant = name;
        options.registry_uri = registry_uri;
        options.lifecycle = vehicle_sil::LifecycleMode::Coordinated;
        options.virtual_time = vehicle_sil::VirtualTimeOptions{10ms, std::nullopt, std::nullopt};
        vehicle_sil::Peer peer(options);
        const bool refuse_13 = static_cast<bool>(std::ifstream(name + ".refuse-13"));
        std::int64_t level = 0;
        std::int64_t doubled = 0;
        vehicle_sil::control::VariableOptions range;
        range.min = 0;
        range.max = 100;
        peer.input(
            "level", [&level] { return level; },
            [&level, refuse_13](std::int64_t value) {
                if (refuse_13 && value == 13)
                {
                    throw std::runtime_error("13 is not taken");
                }
                level = value;
            },
            range);
        peer.output("doubled", &doubled);
        peer.on_step([&](vehicle_sil::SimTime) { doubled = 2 * level; });
        // A session number of its own for every process (as a transport's is).
        const auto session = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1U;
        std::unique_ptr<vehicle_sil::test::Version1Transport> transport;
        if (!std::ifstream(name + ".silent"))
        {
            transport = std::make_unique<vehicle_sil::test::Version1Transport>(peer.agent(), peer.runtime(), 10ms, session, 5s);
        }
        peer.start(std::chrono::milliseconds(start_timeout_ms));
        peer.wait(std::chrono::hours(1));
        const bool failed = peer.failed();
        peer.stop("the run is over");
        transport.reset();
        return failed ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", name.c_str(), e.what());
        return 3;
    }
}
