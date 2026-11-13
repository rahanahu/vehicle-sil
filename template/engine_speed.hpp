// SPDX-License-Identifier: MIT-0
// What the two ECUs agree on: the J1939 message with the engine speed. Put the messages your ECUs
// share in a header like this one (or define them in YAML: vehicle_sil_add_j1939_codec()).
#pragma once

#include "vehicle_sil/j1939/types.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace my_ecus {

/// A proprietary group: 2 bytes, the engine speed in rpm, least significant byte first; every
/// 100 ms from the engine.
inline constexpr vehicle_sil::j1939::Pgn kEngineSpeed{0xFF10};

inline std::array<std::uint8_t, 2> encode_speed(int rpm)
{
    return {static_cast<std::uint8_t>(rpm & 0xFF), static_cast<std::uint8_t>((rpm >> 8) & 0xFF)};
}

inline int decode_speed(std::span<const std::uint8_t> data)
{
    return data.size() < 2 ? 0 : data[0] | (data[1] << 8);
}

} // namespace my_ecus
