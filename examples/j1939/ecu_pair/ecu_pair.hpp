// SPDX-License-Identifier: MIT
//
// Two J1939 ECUs built from the stack's parts: an engine that broadcasts its status and obeys a
// speed request, and a cab that shows the engine speed, notices when the engine goes silent,
// asks for a speed and reads the engine's software identification.
//
// The install functions take a vehicle_sil::Ecu&: the same code runs on virtual buses and on SIL
// Kit (vehicle_sil::Simulation), on a Peer, and on a vehicle_sil::LocalBench. They also set up the
// periodic step of the model.
// The messages (ecu_pair_messages.yaml) are typed values generated from YAML: no bit or byte
// handling here.
#pragma once

#include "ecu_pair_messages.hpp"
#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/j1939/codec_node.hpp"
#include "vehicle_sil/j1939/identification.hpp"
#include "vehicle_sil/j1939/monitor.hpp"
#include "vehicle_sil/j1939/periodic_sender.hpp"
#include "vehicle_sil/j1939/requester.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace ecu_pair {

using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;
namespace codec = vehicle_sil::j1939::codec;
using messages::EngineStatus;
using messages::SpeedRequest;

inline constexpr j1939::Pgn kPgnEngineStatus = EngineStatus::kPgn;
inline constexpr j1939::Pgn kPgnSpeedRequest = SpeedRequest::kPgn;
inline constexpr j1939::Pgn kPgnSoftwareId{0xFEDA}; ///< Answered by the engine's Identification.

inline constexpr j1939::Address kEngineAddress = 0x00;
inline constexpr j1939::Address kCabAddress = 0x80;
inline const j1939::Name kEngineName = j1939::Name{}.set_identity_number(0x100).set_manufacturer_code(0x7F0);
inline const j1939::Name kCabName =
    j1939::Name{}.set_identity_number(0x200).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true);

/// The engine's model: plain C++, no J1939 in it.
struct EngineModel
{
    std::uint16_t rpm = 0;
    std::uint16_t requested = 800;

    /// Called every 10 ms: 25 rpm closer to the request.
    void tick() noexcept
    {
        rpm = rpm < requested ? static_cast<std::uint16_t>(std::min<int>(requested, rpm + 25))
                              : static_cast<std::uint16_t>(std::max<int>(requested, rpm - 25));
    }
};

/// What the cab shows and asks for.
struct Dashboard
{
    std::uint16_t engine_rpm = 0;
    bool engine_running = false;
    bool engine_silent = false;
    std::string engine_software; ///< The identification as it came (field count, fields with '*').
    std::uint16_t wanted_rpm = 1500;
};

/// The engine on the network J1939_CAN of `ecu`; `engine` steps every 10 ms.
inline j1939::Node& install_engine(vehicle_sil::Ecu& ecu, EngineModel& engine)
{
    auto& node = ecu.j1939("J1939_CAN").add_node({.address = kEngineAddress, .name = kEngineName});
    ecu.every(10ms, [&engine](vehicle_sil::SimTime) { engine.tick(); });
    codec::attach_sender<EngineStatus>(node, 20ms, [&engine](EngineStatus& m) {
        m.speed.set(engine.rpm);
        m.state.set(engine.rpm > 0 ? EngineStatus::State::running : EngineStatus::State::stopped);
    });
    codec::on_message<SpeedRequest>(node, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) // not when the cab sends "not available"
        {
            engine.requested = static_cast<std::uint16_t>(*rpm);
        }
    });
    const std::string software[] = {"ENGINE", "1.0.3"};
    node.attach<j1939::Identification>().set_software_identification(software);
    return node;
}

/// The cab on the network J1939_CAN of `ecu`.
inline j1939::Node& install_cab(vehicle_sil::Ecu& ecu, Dashboard& dash)
{
    auto& cab = ecu.j1939("J1939_CAN").add_node({.address = kCabAddress, .name = kCabName});
    cab.attach<j1939::Monitor>(
        kPgnEngineStatus,
        j1939::MonitorOptions{.name = kEngineName,
                              .timeout = 100ms,
                              .on_message =
                                  [&dash](const j1939::Message& m) {
                                      if (const auto status = EngineStatus::decode(m.data))
                                      {
                                          dash.engine_rpm = static_cast<std::uint16_t>(status->speed.value_or(0.0));
                                          dash.engine_running = status->state.value() == EngineStatus::State::running;
                                      }
                                  },
                              .on_state = [&dash](const j1939::MonitorChange& c) {
                                  dash.engine_silent = c.state == j1939::MonitorState::Lost;
                              }});
    codec::attach_sender<SpeedRequest>(cab, 100ms, [&dash](SpeedRequest& m) { m.speed.set(dash.wanted_rpm); },
                                       j1939::PeriodicSenderOptions{.destination = kEngineAddress});
    j1939::Requester::of(cab).request(kPgnSoftwareId, kEngineAddress, [&dash](const j1939::RequestResult& r) {
        if (r.outcome == j1939::RequestOutcome::Response)
        {
            dash.engine_software.assign(r.data.begin(), r.data.end());
        }
    });
    return cab;
}

} // namespace ecu_pair
