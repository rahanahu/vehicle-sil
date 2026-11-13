// SPDX-License-Identifier: MIT
//
// A small vehicle in one Simulation. CAN "Powertrain": Engine, Gateway. CAN "Body": Gateway,
// BodyController. LIN "Door": BodyController (master), DoorModule (slave). The engine broadcasts
// its speed, the gateway forwards it to the body network, the body controller sends it on LIN,
// and the door module answers with its lock state. The engine declares control variables (an
// input that stops it accelerating, its speed as an output); main() writes the input at a set
// time, as a Control API client could. By default everything runs on SIL Kit with a registry in
// this process; --local runs the same ECU classes on virtual CAN and LIN buses.
//
// To split it over processes, give each process SilKitOptions{.registry_uri = "silkit://...",
// .controller = false} and run vehicle_sil_sim_controller once (see docs/internals/virtual-time.md). Without
// the controller in the process, run_for() throws std::logic_error: wait with run() instead, and
// let the controller decide the required participants and the end (--required ... --stop-at-ms ...).
#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/j1939/periodic_sender.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;
namespace lin = vehicle_sil::lin;
using vehicle_sil::SimTime;

namespace {

constexpr j1939::Pgn kSpeed{0xFF50};                          // a group of this example: 2 bytes, km/h
constexpr lin::LinId kCommand = 0x10, kDoorStatus = 0x11;     // LIN frames: speed (master) / locked (slave)

// The ECUs are classes of an Ecu&, so they run on virtual buses and on SIL Kit alike.
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
        // Variables for control from outside (Ecu::agent(), or a Control API client through it).
        ecu.input("engine.accelerating", &accelerating_, {.description = "Accelerates while true"});
        ecu.output("engine.speed", &speed_, {.unit = "km/h"});
        ecu.every(10ms, [this](SimTime) { // accelerates 100 km/h per second
            if (accelerating_) ++speed_;
        });
    }

private:
    bool accelerating_ = true;
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

class BodyController // a J1939 node on "Body" and the LIN master of "Door"
{
public:
    explicit BodyController(vehicle_sil::Ecu& ecu)
    {
        ecu.j1939("Body").add_node({.address = 0x40}).on_message(kSpeed, [this](const j1939::Message& m) {
            speed = static_cast<unsigned>(m.data[0] | (m.data[1] << 8));
        });
        auto& master = ecu.lin_master("Door", {{kCommand, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Master},
                                               {kDoorStatus, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Slave}});
        master.on_result([this](const lin::LinScheduleResult& r) {
            if (r.id == kDoorStatus && r.status == lin::LinResultStatus::Ok) door_locked = r.data[0];
        });
        ecu.on_step([this, &master](SimTime) { // runs before the master's step: the header of this step carries it
            master.set_frame_data(kCommand, std::array{static_cast<std::uint8_t>((std::min)(speed, 255u))});
        });
    }
    unsigned speed = 0;       // the last speed from the gateway
    unsigned door_locked = 0; // the last answer of the door module (LIN)
};

class DoorModule // a LIN slave on "Door"
{
public:
    explicit DoorModule(vehicle_sil::Ecu& ecu)
    {
        auto& slave = ecu.lin_slave("Door", {.subscriptions = {kCommand}});
        slave.on_frame([this](const lin::LinReceivedFrame& f) { speed = f.data[0]; });
        ecu.on_step([this, &slave](SimTime now) { // locks the doors above 10 km/h
            slave.set_response(now, kDoorStatus, std::array<std::uint8_t, 1>{speed > 10 ? std::uint8_t{1} : std::uint8_t{0}});
        });
    }
    unsigned speed = 0; // the last command from the body controller (LIN)
};

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const bool local = argc > 1 && std::string(argv[1]) == "--local";
        std::optional<vehicle_sil::SilKitOptions> silkit; // empty: the virtual buses
        if (!local)
        {
            silkit.emplace(); // a registry of this process
        }
        vehicle_sil::Simulation sim(silkit);
        sim.add<Engine>("Engine", 10ms);
        sim.add<Gateway>("Gateway", 10ms);
        auto& body = sim.add<BodyController>("BodyController", 5ms);
        auto& door = sim.add<DoorModule>("DoorModule", 5ms);
        // The engine stops accelerating at 1200 ms: the write takes effect in its step at 1200 ms,
        // before its application, so it has accelerated in 120 steps.
        auto& control = sim.ecu("Engine").agent();
        static_cast<void>(control.submit({.seq = 1, .writes = {{"engine.accelerating", false}}, .at = SimTime{} + 1200ms}));
        sim.run_for(1500ms);
        // engine.speed, the last sample (the variables: ecu.connected, which every ECU has, then the
        // engine's in the order declared)
        const auto speed = std::get<std::int64_t>(control.latest().values.at(2));
        std::printf("engine: speed %lld km/h (stopped accelerating at 1200 ms)\n", static_cast<long long>(speed));
        std::printf("body controller: speed %u km/h from the gateway, door locked %u\n", body.speed, body.door_locked);
        std::printf("door module: speed %u km/h from the body controller over LIN\n", door.speed);
        const bool ok = speed == 120 && body.speed == 120 && door.speed == 120 && body.door_locked == 1;
        std::printf("%s\n", ok ? "[ok] values reached the end of the chain" : "[FAILED] the chain is broken");
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
