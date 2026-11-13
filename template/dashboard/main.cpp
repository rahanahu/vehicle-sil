// SPDX-License-Identifier: MIT-0
// The dashboard ECU: receives the engine speed on J1939, shows it as a variable and prints it every
// 500 ms. It notices when the speed stops coming (dashboard.engine_lost). main() is ecu_main(), so
// the executable has the command line every ECU has (bin/dashboard_ecu --help).
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
        node.on_message(my_ecus::kEngineSpeed, [this](const j1939::Message& m) {
            rpm_ = my_ecus::decode_speed(m.data);
            ++received_;
            last_ = m.time;
        });
        ecu.output("dashboard.engine_speed", &rpm_, {.unit = "rpm", .description = "the speed shown"});
        ecu.output("dashboard.engine_lost", &lost_,
                   {.description = "no engine speed for more than 300 ms (it comes every 100 ms)"});
        ecu.on_step([this](vehicle_sil::SimTime now) { lost_ = received_ > 0 && now - last_ > 300ms; });
        ecu.every(500ms, [this](vehicle_sil::SimTime now) {
            std::printf("t=%lld ms engine speed %d rpm (%d messages)\n",
                        static_cast<long long>((now - vehicle_sil::SimTime{}) / 1ms), rpm_, received_);
            std::fflush(stdout);
        });
    }

private:
    int rpm_ = 0;
    int received_ = 0;
    vehicle_sil::SimTime last_{};
    bool lost_ = false;
};

} // namespace

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Dashboard>(argc, argv, {.name = "Dashboard", .step = 5ms});
}
