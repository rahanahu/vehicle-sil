// SPDX-License-Identifier: MIT
//
// What the two ECUs of this example agree on: the J1939 message with the engine speed.
#pragma once

#include "vehicle_sil/j1939/types.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace ecu_main_example {

/// A proprietary group of this example: 2 bytes, the engine speed in rpm, least significant byte
/// first; every 100 ms from the engine.
inline constexpr vehicle_sil::j1939::Pgn kEngineSpeed{0xFF10};

inline std::array<std::uint8_t, 2> encode_speed(int rpm)
{
    return {static_cast<std::uint8_t>(rpm & 0xFF), static_cast<std::uint8_t>((rpm >> 8) & 0xFF)};
}

inline int decode_speed(std::span<const std::uint8_t> data)
{
    return data.size() < 2 ? 0 : data[0] | (data[1] << 8);
}

} // namespace ecu_main_example
