// SPDX-License-Identifier: MIT
// The cooler ECU: the coolant temperature moves towards a target that is written from outside.
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace {

class Cooler
{
public:
    explicit Cooler(vehicle_sil::Ecu& ecu)
    {
        ecu.input("cooler.target", &target_,
                  {.unit = "degC", .min = 0.0, .max = 120.0, .description = "target coolant temperature"});
        ecu.output("cooler.temperature", &temperature_, {.unit = "degC"});
        // The model: every 100 ms the temperature moves 0.5 degC towards the target.
        ecu.every(100ms, [this](vehicle_sil::SimTime) {
            temperature_ += temperature_ < target_ ? 0.5 : temperature_ > target_ ? -0.5 : 0.0;
        });
    }

private:
    double target_ = 90.0;
    double temperature_ = 20.0;
};

} // namespace

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Cooler>(argc, argv, {.name = "Cooler", .step = 100ms});
}
