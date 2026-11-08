// SPDX-License-Identifier: MIT
//
// Backend-neutral classic CAN frame values (vehicle_sil::can). No SIL Kit, no Vector XL.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vehicle_sil::can {

/// Identifier format of a CAN frame.
enum class IdFormat : std::uint8_t
{
    Standard, ///< 11-bit identifier
    Extended, ///< 29-bit identifier (J1939)
};

inline constexpr std::uint32_t kMaxStandardId = 0x7FFu;
inline constexpr std::uint32_t kMaxExtendedId = 0x1FFF'FFFFu;
inline constexpr std::size_t kMaxPayload = 8;

/// A classic CAN 2.0 data frame. Remote, CAN FD and CAN XL frames are not represented: J1939
/// does not use them.
///
/// `length` is the payload size in bytes (0..8), not the DLC code: for classic CAN, DLC 9..15
/// also mean 8 bytes. Bytes of `data` at and after `length` are not part of the frame and are
/// ignored by comparisons.
struct Frame
{
    std::uint32_t id = 0;
    IdFormat format = IdFormat::Extended;
    std::uint8_t length = 0;
    std::array<std::uint8_t, kMaxPayload> data{};

    /// The payload bytes (the first `length` bytes of `data`, at most 8).
    [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept
    {
        return {data.data(), std::min<std::size_t>(length, kMaxPayload)};
    }

    /// Equal identifier, format and payload.
    [[nodiscard]] friend bool operator==(const Frame& a, const Frame& b) noexcept
    {
        if (a.id != b.id || a.format != b.format || a.length != b.length)
        {
            return false;
        }
        const auto pa = a.payload();
        const auto pb = b.payload();
        return std::equal(pa.begin(), pa.end(), pb.begin(), pb.end());
    }
};

/// true if the identifier fits the format and the payload is at most 8 bytes.
[[nodiscard]] constexpr bool is_valid(const Frame& frame) noexcept
{
    const std::uint32_t maxId = frame.format == IdFormat::Extended ? kMaxExtendedId : kMaxStandardId;
    return frame.id <= maxId && frame.length <= kMaxPayload;
}

/// A frame with a time in the stack's time domain (SimTime).
///
/// Received frames carry the time the backend assigned (with SIL Kit virtual time: the sender's
/// step time). Frames handed to Port::send() carry the time of the step that sends them.
struct TimedFrame
{
    SimTime time{};
    Frame frame{};

    [[nodiscard]] friend bool operator==(const TimedFrame&, const TimedFrame&) noexcept = default;
};

} // namespace vehicle_sil::can
