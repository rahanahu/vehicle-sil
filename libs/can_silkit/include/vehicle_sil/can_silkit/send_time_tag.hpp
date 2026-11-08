// SPDX-License-Identifier: MIT
//
// The send time a vehicle-sil CAN port puts into SIL Kit's userContext, for the simple CAN
// network simulator (vehicle_sil::can_silkit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace vehicle_sil::can {

static_assert(sizeof(void*) == 8, "vehicle-sil supports 64-bit platforms only (the send time tag needs 64-bit pointers)");

/// SIL Kit's network simulator API does not tell the simulator when a frame was sent. A
/// vehicle-sil port therefore passes its step time as the userContext of SendFrame(): the top
/// byte is a marker, the low 56 bits are the time in nanoseconds (about 2.28 years). SIL Kit
/// carries the value unchanged to the network simulator and back in the transmit
/// acknowledgement; other receivers never see it. User-space pointers on x86-64 Linux and
/// Windows are below 2^47, so a real pointer from another participant never carries the marker.
inline constexpr std::uint64_t kSendTimeMarker = 0xA5ull << 56;
inline constexpr std::uint64_t kSendTimeMask = (1ull << 56) - 1;

/// The userContext for a frame sent at `time` (>= 0).
[[nodiscard]] inline void* encode_send_time(SimTime time) noexcept
{
    const auto ns = static_cast<std::uint64_t>(to_ns(time)) & kSendTimeMask;
    return reinterpret_cast<void*>(kSendTimeMarker | ns);
}

/// The send time in `user_context`, or std::nullopt if it carries no marker (another sender,
/// or no userContext).
[[nodiscard]] inline std::optional<SimTime> decode_send_time(const void* user_context) noexcept
{
    const auto value = reinterpret_cast<std::uint64_t>(user_context);
    if ((value & ~kSendTimeMask) != kSendTimeMarker)
    {
        return std::nullopt;
    }
    return SimTime{} + std::chrono::nanoseconds(static_cast<std::int64_t>(value & kSendTimeMask));
}

} // namespace vehicle_sil::can
