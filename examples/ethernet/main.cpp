// SPDX-License-Identifier: MIT
//
// Two ECUs that exchange raw Ethernet frames, in one process: on the local virtual network, or
// with --silkit on SIL Kit (each ECU a participant, with a registry of this process). The result
// is the same on both, and the example checks it.
//
//     vehicle_sil_ethernet_example [--silkit | --registry-uri <uri>] [--ethertype <number>]
//
// The ECUs are in ethernet_ecus.hpp; sensor/ and display/ make an executable of each
// (vehicle_sil_launch run simulation.yaml).
#include "ethernet_ecus.hpp"

#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>

using namespace std::chrono_literals;

namespace {

long long ms_of(vehicle_sil::SimTime t) { return static_cast<long long>((t - vehicle_sil::SimTime{}) / 1ms); }

int usage(const std::string& why)
{
    std::fprintf(stderr,
                 "error: %s\n"
                 "usage: vehicle_sil_ethernet_example [--silkit | --registry-uri <uri>] [--ethertype <number>]\n"
                 "  --silkit              run the two ECUs as SIL Kit participants (a registry of this process)\n"
                 "  --registry-uri <uri>  the same, with the registry at <uri>\n"
                 "  --ethertype <number>  bytes 12 and 13 of the frames, 0 to 65535 (0x88B5 by default; 0x for hexadecimal)\n",
                 why.c_str());
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::optional<vehicle_sil::SilKitOptions> silkit; // empty: the virtual network
        ethernet_example::Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--silkit")
            {
                silkit.emplace(); // an empty registry URI: a registry of this process
            }
            else if (arg == "--registry-uri" && i + 1 < argc)
            {
                silkit.emplace();
                silkit->registry_uri = argv[++i];
            }
            else if (arg == "--ethertype" && i + 1 < argc)
            {
                const std::string text = argv[++i];
                char* end = nullptr;
                const unsigned long value = std::strtoul(text.c_str(), &end, 0);
                if (text.empty() || *end != '\0' || value > 0xFFFF)
                {
                    return usage("--ethertype " + text + " is not a number from 0 to 65535: write for example 0x88B5");
                }
                options.ether_type = static_cast<std::uint16_t>(value);
            }
            else
            {
                return usage("unknown or incomplete argument " + arg);
            }
        }

        vehicle_sil::Simulation sim(silkit);
        auto& sensor = sim.add<ethernet_example::Sensor>("Sensor", 10ms, options);
        auto& display = sim.add<ethernet_example::Display>("Display", 5ms, options);
        sim.run_for(1s);

        // The sensor sends at 0, 100, ..., 900 ms. The display (step 5 ms) gets each reading in its
        // first step after that and acknowledges it there; the sensor (step 10 ms) gets the
        // acknowledgement in its first step after that.
        std::printf("sensor:  sent %d, acknowledged %d (the last: reading %u, sent by the display at %lld ms)\n",
                    sensor.sent(), sensor.acked(), static_cast<unsigned>(sensor.last_acked()),
                    ms_of(sensor.last_ack_time()));
        std::printf("display: received %d (the last: value %d, sent by the sensor at %lld ms)\n", display.received(),
                    display.value(), ms_of(display.last_reading_time()));
        const bool ok = sensor.sent() == 10 && sensor.acked() == 10 && sensor.last_acked() == 9 &&
                        ms_of(sensor.last_ack_time()) == 905 && display.received() == 10 && display.value() == 20 &&
                        ms_of(display.last_reading_time()) == 900;
        if (!ok)
        {
            std::fprintf(stderr, "error: expected 10 readings and 10 acknowledgements, the last reading sent at 900 ms and "
                                 "acknowledged at 905 ms\n");
        }
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
