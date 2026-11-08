// SPDX-License-Identifier: MIT
//
// Conversion of the test frame matrix to frames of the fake XL driver.
#pragma once

#include <algorithm>

#include "fake_xl_control.hpp"
#include "frame_matrix.hpp"

#include <cstdint>

namespace vehicle_sil::test {

[[nodiscard]] inline fake_xl::Frame to_fake(const MatrixFrame& frame)
{
    return {frame.id, frame.extended, frame.remote, static_cast<std::uint8_t>(frame.dlc), frame.data};
}

/// `frame` as it appears on the XL bus after the bridge transmitted it: xlCanTransmit takes DLC 0..8,
/// so a classic DLC of 9..15 (8 data bytes) leaves as DLC 8.
[[nodiscard]] inline fake_xl::Frame as_transmitted_on_xl(const MatrixFrame& frame)
{
    auto result = to_fake(frame);
    result.dlc = std::min<std::uint8_t>(result.dlc, 8);
    return result;
}

} // namespace vehicle_sil::test
