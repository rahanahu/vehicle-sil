// SPDX-License-Identifier: MIT
//
// Two J1939 ECUs built from the stack's parts: an engine that broadcasts its status and obeys a
// speed request, and a cab that shows the engine speed, notices when the engine goes silent,
// asks for a speed and reads the engine's software identification.
//
// The install functions take a j1939::Network&: the same code runs on a SIL Kit participant
// (vehicle_sil::Peer) and on the local virtual bus of the tests (testkit::LocalCan).
// The messages (ecu_pair_messages.yaml) are typed values generated from YAML: no bit or byte
// handling here.
#pragma once

#include "ecu_pair_messages.hpp"
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

    /// Every 10 ms: 25 rpm closer to the request.
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

inline j1939::Node& install_engine(j1939::Network& can, EngineModel& engine)
{
    auto& ecu = can.add_node({.address = kEngineAddress, .name = kEngineName});
    codec::attach_sender<EngineStatus>(ecu, 20ms, [&engine](EngineStatus& m) {
        m.speed.set(engine.rpm);
        m.state.set(engine.rpm > 0 ? EngineStatus::State::running : EngineStatus::State::stopped);
    });
    codec::on_message<SpeedRequest>(ecu, [&engine](const SpeedRequest& m) {
        if (const auto rpm = m.speed.value()) // not when the cab sends "not available"
        {
            engine.requested = static_cast<std::uint16_t>(*rpm);
        }
    });
    const std::string software[] = {"ENGINE", "1.0.3"};
    ecu.attach<j1939::Identification>().set_software_identification(software);
    return ecu;
}

inline j1939::Node& install_cab(j1939::Network& can, Dashboard& dash)
{
    auto& cab = can.add_node({.address = kCabAddress, .name = kCabName});
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
