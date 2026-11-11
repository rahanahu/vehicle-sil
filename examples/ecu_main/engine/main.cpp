// SPDX-License-Identifier: MIT
//
// The engine ECU: its speed follows the throttle (a variable that can be written from outside)
// and goes out on J1939 every 100 ms. main() is ecu_main(), so the executable has the command
// line every ECU has:
//
//     vehicle_sil_engine_ecu --describe                  what the ECU is, as JSON
//     vehicle_sil_engine_ecu --local --run-for-ms 1000   alone on a local virtual bus
//     vehicle_sil_engine_ecu --registry-uri <uri>        in a SIL Kit simulation
#include "engine_speed.hpp"

#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>

using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;

namespace {

class Engine
{
public:
    explicit Engine(vehicle_sil::Ecu& ecu) :
      node_(ecu.j1939("J1939_CAN").add_node(
          {.address = 0x00, .name = j1939::Name{}.set_identity_number(1).set_manufacturer_code(0x7F0)}))
    {
        ecu.input("engine.throttle", &throttle_,
                  {.unit = "%", .min = 0.0, .max = 100.0, .description = "accelerator pedal position"});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
        // The model: the speed moves towards 800 rpm plus 20 rpm per percent of throttle.
        ecu.every(10ms, [this](vehicle_sil::SimTime) {
            const int target = 800 + static_cast<int>(throttle_ * 20.0);
            rpm_ += rpm_ < target ? 50 : rpm_ > target ? -50 : 0;
        });
        ecu.every(100ms, [this](vehicle_sil::SimTime) {
            static_cast<void>(node_.send(ecu_main_example::kEngineSpeed, ecu_main_example::encode_speed(rpm_)));
        });
    }

private:
    j1939::Node& node_;
    double throttle_ = 20.0;
    int rpm_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Engine>(argc, argv, {.name = "Engine", .step = 10ms});
}
