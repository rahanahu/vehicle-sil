// SPDX-License-Identifier: MIT
//
// A transport that outlives its ECU: destroying the ECU must say why on stderr and call
// std::terminate() instead of leaving the transport with a destroyed agent. CTest checks the
// output (the message, then the marker of the terminate handler below).
#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>

int main()
{
    using namespace std::chrono_literals;
    // No abort(): it may open a dialog or write a core file. The marker shows that terminate
    // was what ended the process.
    std::set_terminate([] {
        std::fputs("terminate handler reached\n", stderr);
        std::fflush(stderr);
        std::_Exit(3);
    });
    std::optional<vehicle_sil::Simulation> sim(std::in_place);
    vehicle_sil::Ecu& ecu = sim->add_ecu("Engine", 10ms);
    double speed = 0.0;
    ecu.output("engine.speed", &speed);
    const vehicle_sil::control::TransportLease lease(ecu.agent());
    sim.reset(); // the lease is still held
    std::fputs("the ECU was destroyed without terminate\n", stderr);
    return 0;
}
