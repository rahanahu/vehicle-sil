// SPDX-License-Identifier: MIT
//
// The dashboard ECU: receives the engine speed on J1939, shows it as a variable and prints it
// every 500 ms. main() is ecu_main(), so the executable has the command line every ECU has
// (vehicle_sil_dashboard_ecu --help).
#include "engine_speed.hpp"

#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>
#include <cstdio>

using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;

namespace {

class Dashboard
{
public:
    explicit Dashboard(vehicle_sil::Ecu& ecu)
    {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x28});
        node.on_message(ecu_main_example::kEngineSpeed, [this](const j1939::Message& m) {
            rpm_ = ecu_main_example::decode_speed(m.data);
            ++received_;
        });
        ecu.output("dashboard.engine_speed", &rpm_, {.unit = "rpm", .description = "the speed shown"});
        ecu.every(500ms, [this](vehicle_sil::SimTime now) {
            std::printf("t=%lld ms engine speed %d rpm (%d messages)\n",
                        static_cast<long long>((now - vehicle_sil::SimTime{}) / 1ms), rpm_, received_);
            std::fflush(stdout);
        });
    }

private:
    int rpm_ = 0;
    int received_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Dashboard>(argc, argv, {.name = "Dashboard", .step = 5ms});
}
