// SPDX-License-Identifier: MIT
//
// The display of ethernet_ecus.hpp as an executable of its own (ecu_main(): --describe,
// --registry-uri, --local, --help), for vehicle_sil_launch run simulation.yaml.
#include "ethernet_ecus.hpp"

#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<ethernet_example::Display>(argc, argv,
                                                            {.name = "Display", .step = std::chrono::milliseconds(5)});
}
