// SPDX-License-Identifier: MIT
//
// The first ECUs: an engine that broadcasts its speed and a display that shows it. Each is a
// class that takes a vehicle_sil::Ecu&, so the same code runs on virtual buses (the default) or
// on SIL Kit with a registry of this process.
//
//     vehicle_sil_getting_started [--silkit | --registry-uri <uri>]
#include "getting_started_messages.hpp"

#include "vehicle_sil/j1939/codec_node.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>

using namespace std::chrono_literals;
using getting_started::messages::EngineStatus;
namespace codec = vehicle_sil::j1939::codec;

class Engine
{
public:
    explicit Engine(vehicle_sil::Ecu& ecu)
    {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x00});
        codec::attach_sender<EngineStatus>(node, 20ms, [this](EngineStatus& m) { m.speed.set(rpm_); });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { rpm_ = rpm_ < 1500 ? rpm_ + 25 : rpm_; }); // the model
    }

private:
    int rpm_ = 0;
};

class Display
{
public:
    explicit Display(vehicle_sil::Ecu& ecu)
    {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x80});
        codec::on_message<EngineStatus>(node, [this](const EngineStatus& m) {
            rpm_ = m.speed.value().value_or(0.0);
            ++received_;
        });
        ecu.every(250ms, [this](vehicle_sil::SimTime now) {
            std::printf("t=%4lld ms  engine %4.0f rpm\n", static_cast<long long>((now - vehicle_sil::SimTime{}) / 1ms), rpm_);
        });
    }
    int received() const { return received_; }

private:
    double rpm_ = 0;
    int received_ = 0;
};

int main(int argc, char** argv)
{
    try
    {
        vehicle_sil::SilKitOptions silkit;
        bool use_silkit = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            use_silkit = use_silkit || arg == "--silkit" || arg == "--registry-uri";
            if (arg == "--registry-uri" && i + 1 < argc)
            {
                silkit.registry_uri = argv[++i];
            }
        }
        vehicle_sil::Simulation sim = use_silkit ? vehicle_sil::Simulation(silkit) : vehicle_sil::Simulation();
        sim.add<Engine>("Engine", 10ms);
        auto& display = sim.add<Display>("Display", 5ms);
        sim.run_for(2s);
        std::printf("the display received %d messages\n", display.received());
        return display.received() > 0 ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
