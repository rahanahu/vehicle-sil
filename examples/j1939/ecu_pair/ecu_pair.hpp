// SPDX-License-Identifier: MIT
//
// Two J1939 ECUs built from the stack's parts: an engine that broadcasts its status and obeys a
// speed request, and a cab that shows the engine speed, notices when the engine goes silent,
// asks for a speed and reads the engine's software identification.
//
// The install functions take a j1939::Network&: the same code runs on a SIL Kit participant
// (vehicle_sil::Peer) and on the local virtual bus of the tests (testkit::LocalCan).
// The groups 0xFF10 and 0xEF00 are proprietary, with a layout of this example.
#pragma once

#include "vehicle_sil/j1939/identification.hpp"
#include "vehicle_sil/j1939/monitor.hpp"
#include "vehicle_sil/j1939/periodic_sender.hpp"
#include "vehicle_sil/j1939/requester.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace ecu_pair {

using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;

inline constexpr j1939::Pgn kPgnEngineStatus{0xFF10}; ///< Engine -> all, 20 ms: speed (rpm, bytes 0-1).
inline constexpr j1939::Pgn kPgnSpeedRequest{0xEF00}; ///< Cab -> engine, 100 ms: speed (rpm, bytes 0-1).
inline constexpr j1939::Pgn kPgnSoftwareId{0xFEDA};   ///< Answered by the engine's Identification.

inline constexpr j1939::Address kEngineAddress = 0x00;
inline constexpr j1939::Address kCabAddress = 0x80;
inline const j1939::Name kEngineName = j1939::Name{}.set_identity_number(0x100).set_manufacturer_code(0x7F0);
inline const j1939::Name kCabName =
    j1939::Name{}.set_identity_number(0x200).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true);

[[nodiscard]] inline std::uint16_t read_u16(std::span<const std::uint8_t> data) noexcept
{
    return data.size() < 2 ? 0 : static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

/// Writes an 8-byte message into `out` (a PeriodicSender's buffer: 8 bytes by default).
[[nodiscard]] inline std::size_t write_u16(std::span<std::uint8_t> out, std::uint16_t value) noexcept
{
    std::fill(out.begin(), out.begin() + 8, std::uint8_t{0xFF}); // not available
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    return 8;
}

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
    bool engine_silent = false;
    std::string engine_software; ///< The identification as it came (field count, fields with '*').
    std::uint16_t wanted_rpm = 1500;
};

inline j1939::Node& install_engine(j1939::Network& can, EngineModel& engine)
{
    auto& ecu = can.add_node({.address = kEngineAddress, .name = kEngineName});
    ecu.attach<j1939::PeriodicSender>(kPgnEngineStatus, 20ms,
                                      [&engine](std::span<std::uint8_t> out) { return write_u16(out, engine.rpm); });
    ecu.on_message(kPgnSpeedRequest, [&engine](const j1939::Message& m) { engine.requested = read_u16(m.data); });
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
                              .on_message = [&dash](const j1939::Message& m) { dash.engine_rpm = read_u16(m.data); },
                              .on_state = [&dash](const j1939::MonitorChange& c) {
                                  dash.engine_silent = c.state == j1939::MonitorState::Lost;
                              }});
    cab.attach<j1939::PeriodicSender>(
        kPgnSpeedRequest, 100ms, [&dash](std::span<std::uint8_t> out) { return write_u16(out, dash.wanted_rpm); },
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
