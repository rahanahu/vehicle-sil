// SPDX-License-Identifier: MIT
//
// Runs the two ECUs of ecu_pair.hpp as two SIL Kit participants of this process, in virtual
// time, and prints what the cab shows. Needs a running registry (sil-kit-registry).
//
//     vehicle_sil_j1939_ecu_pair [--registry-uri silkit://localhost:8500] [--seconds 3]

#include "ecu_pair.hpp"

#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace {

vehicle_sil::PeerOptions coordinated(std::string participant, const std::string& uri, vehicle_sil::SimDuration step)
{
    vehicle_sil::PeerOptions o;
    o.participant = std::move(participant);
    o.registry_uri = uri;
    o.lifecycle = vehicle_sil::LifecycleMode::Coordinated;
    o.virtual_time = vehicle_sil::VirtualTimeOptions{step, std::nullopt, std::nullopt};
    return o;
}

int run(const std::string& uri, int seconds)
{
    using namespace ecu_pair;
    EngineModel engine; // what the steps use: declared before the peers
    Dashboard dash;

    vehicle_sil::SimulationControllerOptions control;
    control.registry_uri = uri;
    control.required_participants = {"Engine", "Cab"};
    control.stop_at = std::chrono::seconds(seconds);
    vehicle_sil::SimulationController controller(control);

    vehicle_sil::Peer enginePeer(coordinated("Engine", uri, 10ms));
    install_engine(enginePeer.j1939("J1939_CAN"), engine);
    enginePeer.every(10ms, [&](vehicle_sil::SimTime) { engine.tick(); });

    vehicle_sil::Peer cabPeer(coordinated("Cab", uri, 5ms));
    auto& cab = install_cab(cabPeer.j1939("J1939_CAN"), dash);
    cabPeer.every(250ms, [&](vehicle_sil::SimTime now) {
        std::printf("t=%5lld ms  cab 0x%02X  engine %4u rpm%s  software %zu bytes\n",
                    static_cast<long long>((now - vehicle_sil::SimTime{}) / 1ms), cab.address(), dash.engine_rpm,
                    dash.engine_silent ? " (silent)" : "", dash.engine_software.size());
        dash.wanted_rpm = now < vehicle_sil::SimTime{} + 1500ms ? 1500 : 900;
    });

    vehicle_sil::start_together(controller, {&enginePeer, &cabPeer});
    enginePeer.wait();
    cabPeer.wait();
    if (enginePeer.failed() || cabPeer.failed())
    {
        std::fprintf(stderr, "failed: %s%s\n", enginePeer.error().c_str(), cabPeer.error().c_str());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string uri = "silkit://localhost:8500";
        int seconds = 3;
        for (int i = 1; i + 1 < argc; i += 2)
        {
            const std::string_view option = argv[i];
            if (option == "--registry-uri")
            {
                uri = argv[i + 1];
            }
            else if (option == "--seconds")
            {
                seconds = std::stoi(argv[i + 1]);
            }
        }
        return run(uri, seconds > 0 ? seconds : 3);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
