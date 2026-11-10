// SPDX-License-Identifier: MIT
//
// A small vehicle in one Simulation. CAN "Powertrain": Engine, Gateway. CAN "Body": Gateway,
// BodyController. LIN "Door": BodyController (master), DoorModule (slave). The engine broadcasts
// its speed, the gateway forwards it to the body network, the body controller sends it on LIN,
// and the door module answers with its lock state. By default everything runs on SIL Kit with a
// registry in this process; --local uses virtual buses, where there is no LIN.
//
// To split it over processes, give each process SilKitOptions{.registry_uri = "silkit://...",
// .controller = false} and run vehicle_sil_sim_controller once (see docs/architecture.md). Without
// the controller in the process, run_for() throws std::logic_error: wait with run() instead, and
// let the controller decide the required participants and the end (--required ... --stop-at-ms ...).
#include "vehicle_sil/j1939/periodic_sender.hpp"
#include "vehicle_sil/peer/peer.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>

using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;
namespace lin = vehicle_sil::lin;
using vehicle_sil::SimTime;

namespace {

constexpr j1939::Pgn kSpeed{0xFF50};                          // a group of this example: 2 bytes, km/h
constexpr lin::LinId kCommand = 0x10, kDoorStatus = 0x11;     // LIN frames: speed (master) / locked (slave)

// The J1939 ECUs are classes of an Ecu&, so they run on virtual buses and on SIL Kit alike.
class Engine
{
public:
    explicit Engine(vehicle_sil::Ecu& ecu)
    {
        auto& node = ecu.j1939("Powertrain").add_node({.address = 0x00});
        node.attach<j1939::PeriodicSender>(kSpeed, 20ms, [this](std::span<std::uint8_t> out) {
            out[0] = static_cast<std::uint8_t>(speed_);
            out[1] = static_cast<std::uint8_t>(speed_ >> 8);
            return std::size_t{2};
        });
        ecu.every(10ms, [this](SimTime) { ++speed_; }); // accelerates 100 km/h per second
    }

private:
    unsigned speed_ = 0;
};

class Gateway // one ECU on two networks: what it receives on one it sends on the other
{
public:
    explicit Gateway(vehicle_sil::Ecu& ecu)
    {
        auto& to_body = ecu.j1939("Body").add_node({.address = 0x31});
        ecu.j1939("Powertrain").add_node({.address = 0x30}).on_message(kSpeed, [&to_body](const j1939::Message& m) {
            static_cast<void>(to_body.send(kSpeed, m.data));
        });
    }
};

class BodyController
{
public:
    explicit BodyController(vehicle_sil::Ecu& ecu)
    {
        ecu.j1939("Body").add_node({.address = 0x40}).on_message(kSpeed, [this](const j1939::Message& m) {
            speed = static_cast<unsigned>(m.data[0] | (m.data[1] << 8));
        });
    }
    unsigned speed = 0;      // the last speed from the gateway
    unsigned door_locked = 0; // the last answer of the door module (LIN)
};

struct DoorModule
{
    unsigned speed = 0; // the last command from the body controller (LIN)
};

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const bool local = argc > 1 && std::string(argv[1]) == "--local";
        DoorModule door;
        vehicle_sil::Simulation sim = local ? vehicle_sil::Simulation() : vehicle_sil::Simulation(vehicle_sil::SilKitOptions{});
        sim.add<Engine>("Engine", 10ms);
        sim.add<Gateway>("Gateway", 10ms);
        auto& body = sim.add<BodyController>("BodyController", 5ms);
        sim.add_ecu("DoorModule", 5ms);
        if (local)
        {
            std::printf("--local: virtual buses have no LIN, the door module is not connected\n");
        }
        else // LIN is not part of Ecu: it goes through the SIL Kit participant of the ECU
        {
            auto& body_ecu = sim.peer("BodyController");
            auto& master = body_ecu.lin_master(
                "Door",
                {{kCommand, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Master},
                 {kDoorStatus, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Slave}},
                [&body](const lin::LinScheduleResult& r) {
                    if (r.id == kDoorStatus && r.status == lin::LinResultStatus::Ok) body.door_locked = r.data[0];
                });
            body_ecu.on_step([&](SimTime) {
                const std::uint8_t speed = static_cast<std::uint8_t>((std::min)(body.speed, 255u));
                master.set_frame_data(kCommand, std::array{speed});
            });
            auto& door_ecu = sim.peer("DoorModule");
            auto& slave = door_ecu.lin_slave("Door", {.subscriptions = {kCommand}}, [&door](const lin::LinReceivedFrame& f) {
                door.speed = f.data[0];
            });
            door_ecu.on_step([&](SimTime now) { // locks the doors above 10 km/h
                slave.set_response(now, kDoorStatus, std::array<std::uint8_t, 1>{door.speed > 10 ? std::uint8_t{1} : std::uint8_t{0}});
            });
        }
        sim.run_for(1500ms);
        std::printf("body controller: speed %u km/h from the gateway, door locked %u\n", body.speed, body.door_locked);
        if (!local)
        {
            std::printf("door module: speed %u km/h from the body controller over LIN\n", door.speed);
        }
        const bool ok = body.speed > 100 && (local || (door.speed > 100 && body.door_locked == 1));
        std::printf("%s\n", ok ? "[ok] values reached the end of the chain" : "[FAILED] the chain is broken");
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
