// SPDX-License-Identifier: MIT
//
// SIL Kit CAN frame helpers shared by the tests.
#pragma once

#include <silkit/services/can/CanDatatypes.hpp>

#include <cstdint>
#include <vector>

namespace vehicle_sil::unit {

[[nodiscard]] constexpr SilKit::Services::Can::CanFrameFlagMask flag(SilKit::Services::Can::CanFrameFlag f) noexcept
{
    return static_cast<SilKit::Services::Can::CanFrameFlagMask>(f);
}

/// A frame whose payload is `data`. The frame refers to `data`, which must outlive it.
[[nodiscard]] inline SilKit::Services::Can::CanFrame make_frame(std::uint32_t id, SilKit::Services::Can::CanFrameFlagMask flags,
                                                                std::uint16_t dlc, const std::vector<std::uint8_t>& data)
{
    SilKit::Services::Can::CanFrame frame{};
    frame.canId = id;
    frame.flags = flags;
    frame.dlc = dlc;
    frame.dataField = SilKit::Util::Span<const std::uint8_t>(data.data(), data.size());
    return frame;
}

/// An extended (29 bit) data frame with DLC = data.size(). The frame refers to `data`.
[[nodiscard]] inline SilKit::Services::Can::CanFrame extended_frame(std::uint32_t id, const std::vector<std::uint8_t>& data)
{
    return make_frame(id, flag(SilKit::Services::Can::CanFrameFlag::Ide), static_cast<std::uint16_t>(data.size()), data);
}

/// An owning copy of a frame, for recording and comparing.
struct RecordedFrame
{
    std::uint32_t canId = 0;
    SilKit::Services::Can::CanFrameFlagMask flags = 0;
    std::uint16_t dlc = 0;
    std::vector<std::uint8_t> data;

    bool operator==(const RecordedFrame&) const = default;
};

[[nodiscard]] inline RecordedFrame record(const SilKit::Services::Can::CanFrame& frame)
{
    return {frame.canId, frame.flags, frame.dlc, std::vector<std::uint8_t>(frame.dataField.begin(), frame.dataField.end())};
}

} // namespace vehicle_sil::unit
