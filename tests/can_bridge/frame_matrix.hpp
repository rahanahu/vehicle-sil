// SPDX-License-Identifier: MIT
//
// Classic CAN frames that exercise every field the bridge must preserve: 11 and 29 bit
// identifiers including their limits, DLC 0..8 with distinct payloads, remote frames, and a
// DLC > 8 frame (classic CAN: 8 data bytes).
#pragma once

#include <silkit/services/can/CanDatatypes.hpp>

#include <cstdint>
#include <vector>

namespace vehicle_sil::test {

struct MatrixFrame
{
    std::uint32_t id;
    bool extended;
    bool remote;
    std::uint16_t dlc;
    std::vector<std::uint8_t> data;

    [[nodiscard]] SilKit::Services::Can::CanFrame to_silkit() const
    {
        namespace can = SilKit::Services::Can;
        can::CanFrame frame{};
        frame.canId = id;
        frame.flags = static_cast<can::CanFrameFlagMask>(
            (extended ? static_cast<can::CanFrameFlagMask>(can::CanFrameFlag::Ide) : 0u) |
            (remote ? static_cast<can::CanFrameFlagMask>(can::CanFrameFlag::Rtr) : 0u));
        frame.dlc = dlc;
        frame.dataField = SilKit::Util::Span<const std::uint8_t>(data.data(), data.size());
        return frame;
    }
};

inline std::vector<MatrixFrame> frame_matrix()
{
    std::vector<MatrixFrame> frames;
    std::uint8_t seed = 0x10;
    // 11 bit and 29 bit identifiers including 0 and the maximum, every DLC 0..8.
    for (const auto& [id, extended] : std::vector<std::pair<std::uint32_t, bool>>{
             {0x000, false}, {0x7FF, false}, {0x123, false}, {0x0000'0000, true}, {0x1FFF'FFFF, true}, {0x18FF'1080, true}})
    {
        for (std::uint16_t dlc = 0; dlc <= 8; ++dlc)
        {
            MatrixFrame frame{id, extended, false, dlc, {}};
            for (std::uint16_t i = 0; i < dlc; ++i)
            {
                frame.data.push_back(static_cast<std::uint8_t>(seed + i * 17));
            }
            ++seed;
            frames.push_back(frame);
        }
    }
    // Remote frames (no data, DLC is the requested length).
    frames.push_back({0x321, false, true, 0, {}});
    frames.push_back({0x321, false, true, 8, {}});
    frames.push_back({0x18EA'FF00, true, true, 3, {}});
    // DLC 15 on classic CAN carries 8 bytes.
    frames.push_back({0x456, false, false, 15, {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, 0x55, 0xAA}});
    return frames;
}

} // namespace vehicle_sil::test
