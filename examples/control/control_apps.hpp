// SPDX-License-Identifier: MIT
//
// The two ECUs of the control examples, written against vehicle_sil::Ecu: each declares the
// variables a Control API client may read and write.
//   Engine  engine.throttle (input, %), engine.speed (output, rpm); broadcasts its speed
//   Body    cabin.parking_brake (input), cabin.lamp (enum input), body.engine_speed (output:
//           the speed it received)
#pragma once

#include "vehicle_sil/ecu/ecu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace control_example {

using namespace std::chrono_literals;

inline constexpr vehicle_sil::j1939::Pgn kPgnEngineSpeed{0xFF10}; // proprietary B

/// A first-order engine: 800 rpm idle, 6000 rpm at full throttle.
class Engine
{
public:
    explicit Engine(vehicle_sil::Ecu& ecu) :
      node_(ecu.j1939("J1939_CAN").add_node(
          {0x00, vehicle_sil::j1939::Name{}.set_identity_number(0x100).set_manufacturer_code(0x7F0)}))
    {
        ecu.input("engine.throttle", &throttle_,
                  {.unit = "%", .min = 0.0, .max = 100.0, .step = 1.0, .description = "Accelerator pedal"});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
        ecu.on_step([this](vehicle_sil::SimTime) { rpm_ += (800.0 + 52.0 * throttle_ - rpm_) * 0.05; });
        ecu.every(20ms, [this](vehicle_sil::SimTime) {
            const auto value = static_cast<std::uint16_t>(std::clamp(rpm_, 0.0, 65535.0));
            const std::uint8_t data[2]{static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>(value >> 8)};
            static_cast<void>(node_.send(kPgnEngineSpeed, data));
        });
    }

private:
    vehicle_sil::j1939::Node& node_;
    double throttle_ = 0.0;
    double rpm_ = 800.0;
};

/// Switches in the cabin, and the engine speed it receives on J1939.
class Body
{
public:
    enum class Lamp : std::uint8_t
    {
        Off = 0,
        Low = 1,
        High = 2,
    };

    explicit Body(vehicle_sil::Ecu& ecu)
    {
        ecu.input("cabin.parking_brake", &parking_brake_, {.description = "Parking brake switch"});
        ecu.input_enum("cabin.lamp", &lamp_, {{"off", 0}, {"low", 1}, {"high", 2}}, {.description = "Head lamp switch"});
        ecu.output("body.engine_speed", &received_rpm_, {.unit = "rpm", .description = "Engine speed received on J1939"});
        auto& node = ecu.j1939("J1939_CAN").add_node({0x21});
        node.on_message(kPgnEngineSpeed, [this](const vehicle_sil::j1939::Message& m) {
            if (m.data.size() >= 2)
            {
                received_rpm_ = static_cast<double>(m.data[0] | (m.data[1] << 8));
            }
        });
    }

private:
    bool parking_brake_ = true;
    Lamp lamp_ = Lamp::Off;
    double received_rpm_ = 0.0;
};

} // namespace control_example
