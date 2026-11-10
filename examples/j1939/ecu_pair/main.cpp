// SPDX-License-Identifier: MIT
//
// Runs the two ECUs of ecu_pair.hpp in virtual time and prints what the cab shows: as two SIL Kit
// participants of this process (the default; a registry of this process, or the one given), or
// on virtual buses (--local).
//
//     vehicle_sil_j1939_ecu_pair [--local | --registry-uri silkit://localhost:8500] [--seconds 3]

#include "ecu_pair.hpp"

#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

int main(int argc, char** argv)
{
    using namespace ecu_pair;
    try
    {
        vehicle_sil::SilKitOptions silkit; // an empty registry URI: a registry of this process
        bool local = false;
        int seconds = 3;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view option = argv[i];
            if (option == "--local")
            {
                local = true;
            }
            else if (option == "--registry-uri" && i + 1 < argc)
            {
                silkit.registry_uri = argv[++i];
            }
            else if (option == "--seconds" && i + 1 < argc)
            {
                seconds = std::stoi(argv[++i]);
            }
        }
        vehicle_sil::Simulation sim = local ? vehicle_sil::Simulation() : vehicle_sil::Simulation(silkit);

        auto& engine_ecu = sim.add_ecu("Engine", 10ms);
        install_engine(engine_ecu, engine_ecu.emplace<EngineModel>());

        auto& cab_ecu = sim.add_ecu("Cab", 5ms);
        auto& dash = cab_ecu.emplace<Dashboard>();
        auto& cab = install_cab(cab_ecu, dash);
        cab_ecu.every(250ms, [&](vehicle_sil::SimTime now) {
            std::printf("t=%5lld ms  cab 0x%02X  engine %4u rpm%s  software %zu bytes\n",
                        static_cast<long long>((now - vehicle_sil::SimTime{}) / 1ms), cab.address(), dash.engine_rpm,
                        dash.engine_silent ? " (silent)" : "", dash.engine_software.size());
            dash.wanted_rpm = now < vehicle_sil::SimTime{} + 1500ms ? 1500 : 900;
        });

        sim.run_for(std::chrono::seconds(seconds > 0 ? seconds : 3));
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
