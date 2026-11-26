// SPDX-License-Identifier: MIT
//
// The send time a vehicle-sil CAN port puts into SIL Kit's userContext, for the simple CAN
// network simulator (vehicle_sil::can_silkit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace vehicle_sil::can {

static_assert(sizeof(void*) == 8, "vehicle-sil supports 64-bit platforms only (the send time tag needs 64-bit pointers)");

/// SIL Kit's network simulator API does not tell the simulator when a frame was sent. A
/// vehicle-sil port therefore passes its step time as the userContext of SendFrame():
/// - Legacy 0xA5 format: top byte 0xA5, low 56 bits are the time in nanoseconds (about 2.28 years).
/// - Correlated 0xA6 format: top byte 0xA6, bits [55:40] are a 16-bit correlation tag, low 40 bits
///   are the send time modulo 2^40 ns (~1099.51 s), unwrapped near the simulator's reference time.
/// SIL Kit carries the value unchanged to the network simulator and back in the transmit
/// acknowledgement; other receivers never see it. User-space pointers on x86-64 Linux and
/// Windows are below 2^47, so a real pointer from another participant never carries either marker.
inline constexpr std::uint64_t kMarkerMask = 0xFFull << 56;
inline constexpr std::uint64_t kSendTimeMarker = 0xA5ull << 56;
inline constexpr std::uint64_t kSendTimeMask = (1ull << 56) - 1;

inline constexpr std::uint64_t kCorrelatedSendTimeMarker = 0xA6ull << 56;
inline constexpr unsigned kCorrelatedTimeBits = 40;
inline constexpr std::uint64_t kCorrelatedTimeMod = 1ull << kCorrelatedTimeBits;
inline constexpr std::uint64_t kCorrelatedTimeHalf = 1ull << (kCorrelatedTimeBits - 1);
inline constexpr std::uint64_t kCorrelatedTimeMask = kCorrelatedTimeMod - 1;
inline constexpr std::uint64_t kCorrelatedTagMask = 0xFFFFull;

/// The legacy 0xA5 userContext for a frame sent at `time` (>= 0).
[[nodiscard]] inline void* encode_send_time(SimTime time) noexcept
{
    const auto ns = static_cast<std::uint64_t>(to_ns(time)) & kSendTimeMask;
    return reinterpret_cast<void*>(kSendTimeMarker | ns);
}

/// The correlated 0xA6 userContext for a frame sent at `time` (>= 0) with 16-bit correlation `tag`.
[[nodiscard]] inline void* encode_correlated_send_time(SimTime time, std::uint16_t tag) noexcept
{
    const auto low40 = static_cast<std::uint64_t>(to_ns(time)) & kCorrelatedTimeMask;
    const auto bits = kCorrelatedSendTimeMarker | (static_cast<std::uint64_t>(tag) << kCorrelatedTimeBits) | low40;
    return reinterpret_cast<void*>(bits);
}

/// The 16-bit correlation tag in a 0xA6 `user_context`, or std::nullopt if it does not carry 0xA6.
[[nodiscard]] inline std::optional<std::uint16_t> decode_correlated_tag(const void* user_context) noexcept
{
    const auto value = reinterpret_cast<std::uint64_t>(user_context);
    if ((value & kMarkerMask) != kCorrelatedSendTimeMarker)
    {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>((value >> kCorrelatedTimeBits) & kCorrelatedTagMask);
}

/// The send time in a legacy 0xA5 `user_context`, or std::nullopt if it carries no 0xA5 marker.
[[nodiscard]] inline std::optional<SimTime> decode_send_time(const void* user_context) noexcept
{
    const auto value = reinterpret_cast<std::uint64_t>(user_context);
    if ((value & kMarkerMask) != kSendTimeMarker)
    {
        return std::nullopt;
    }
    return SimTime{} + std::chrono::nanoseconds(static_cast<std::int64_t>(value & kSendTimeMask));
}

/// The send time in either a legacy 0xA5 or correlated 0xA6 `user_context`, unwrapping 0xA6's
/// 40-bit nanosecond residue near `reference` with non-negative clamping, or std::nullopt if it
/// carries neither marker (or would overflow int64_t nanoseconds).
[[nodiscard]] inline std::optional<SimTime> decode_send_time(const void* user_context, SimTime reference) noexcept
{
    const auto value = reinterpret_cast<std::uint64_t>(user_context);
    const auto marker = value & kMarkerMask;
    if (marker == kSendTimeMarker)
    {
        return SimTime{} + std::chrono::nanoseconds(static_cast<std::int64_t>(value & kSendTimeMask));
    }
    if (marker != kCorrelatedSendTimeMarker)
    {
        return std::nullopt;
    }
    const std::int64_t refSigned = to_ns(reference);
    const std::uint64_t ref = static_cast<std::uint64_t>(refSigned < 0 ? 0 : refSigned);
    const std::uint64_t low40 = value & kCorrelatedTimeMask;
    const std::uint64_t diff = (low40 - (ref & kCorrelatedTimeMask)) & kCorrelatedTimeMask;
    constexpr std::uint64_t kMaxInt64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    std::uint64_t candidate = 0;
    if (diff < kCorrelatedTimeHalf)
    {
        if (ref > kMaxInt64 - diff)
        {
            return std::nullopt;
        }
        candidate = ref + diff;
    }
    else
    {
        const std::uint64_t back = kCorrelatedTimeMod - diff; // in [1, 2^39]
        if (ref < back)
        {
            // Negative send_time is impossible in virtual time; smallest valid non-negative
            // representative is ref + diff (>= 2^39 ns ahead, rejected by future_limit).
            if (ref > kMaxInt64 - diff)
            {
                return std::nullopt;
            }
            candidate = ref + diff;
        }
        else
        {
            candidate = ref - back;
        }
    }
    return SimTime{} + std::chrono::nanoseconds(static_cast<std::int64_t>(candidate));
}

} // namespace vehicle_sil::can
