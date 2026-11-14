// SPDX-License-Identifier: MIT
//
// The transport protocol as it is read from the frames, shared by the two readers of the J1939
// stack: the engine's Transport (transport.cpp), which takes part in transfers, and the
// TransferSniffer (transfer_sniffer.cpp), which follows the transfers of others. Both use the
// same control bytes, timeouts, packet count, shape checks of an announcement and the same
// list of groups that are not taken from a transfer, so that they cannot drift apart.
//
// Internal. Depends on the value types only (types.hpp): no Network, so the sniffer stays
// usable without one. The values are those of this repository's engine; they were not compared
// with the standard.
#pragma once

#include "vehicle_sil/j1939/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vehicle_sil::j1939::transport_wire {

// The control byte of a TP.CM frame (its first data byte).
inline constexpr std::uint8_t kRts = 16;
inline constexpr std::uint8_t kCts = 17;
inline constexpr std::uint8_t kEndOfMessageAck = 19;
inline constexpr std::uint8_t kBam = 32;
inline constexpr std::uint8_t kAbort = 255;

// Timeouts: between the packets of a transfer (T1), for the first packet after a clear-to-send
// (T2), for the receiver's answer (T3) and for the next clear-to-send after one that holds the
// connection (T4).
inline constexpr SimDuration kT1 = std::chrono::milliseconds(750);
inline constexpr SimDuration kT2 = std::chrono::milliseconds(1250);
inline constexpr SimDuration kT3 = std::chrono::milliseconds(1250);
inline constexpr SimDuration kT4 = std::chrono::milliseconds(1050);
inline constexpr std::uint8_t kMaxRetransmitRequests = 2;

/// The number of data packets of a message of `length` bytes.
[[nodiscard]] constexpr std::uint16_t packets_for(std::size_t length) noexcept
{
    return static_cast<std::uint16_t>((length + 6) / 7);
}

/// The group a TP.CM frame is about (its last three bytes). `data` has at least 8 bytes.
[[nodiscard]] constexpr std::uint32_t raw_pgn_of(std::span<const std::uint8_t> data) noexcept
{
    return static_cast<std::uint32_t>(data[5]) | (static_cast<std::uint32_t>(data[6]) << 8) |
           (static_cast<std::uint32_t>(data[7]) << 16);
}

/// The message size an RTS or BAM announces. `data` has at least 8 bytes.
[[nodiscard]] constexpr std::size_t announced_size(std::span<const std::uint8_t> data) noexcept
{
    return static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8);
}

/// What is wrong with an announcement (RTS or BAM). The engine's receiver answers an RTS with a
/// different abort reason for each, so the check returns the reason and not only yes or no.
enum class Shape : std::uint8_t
{
    Ok,
    TooLarge,  ///< More than kMaxTransportLength bytes.
    Malformed, ///< Any other error (see check_rts() and check_bam()).
};

/// The shape of an RTS: `size` bytes in `packets` packets, at most `limit` packets per
/// clear-to-send, for the group `rawPgn`. Malformed: a size that fits a single frame, a packet
/// count that does not match the size, a limit of 0, or a group that is not valid.
[[nodiscard]] constexpr Shape check_rts(std::size_t size, std::uint8_t packets, std::uint8_t limit, std::uint32_t rawPgn) noexcept
{
    if (size > kMaxTransportLength)
    {
        return Shape::TooLarge;
    }
    if (size <= can::kMaxPayload || packets != packets_for(size) || limit == 0 || !Pgn{rawPgn}.is_valid())
    {
        return Shape::Malformed;
    }
    return Shape::Ok;
}

/// The shape of a BAM: as check_rts(), without the limit (a broadcast has none).
[[nodiscard]] constexpr Shape check_bam(std::size_t size, std::uint8_t packets, std::uint32_t rawPgn) noexcept
{
    if (size > kMaxTransportLength)
    {
        return Shape::TooLarge;
    }
    if (size <= can::kMaxPayload || packets != packets_for(size) || !Pgn{rawPgn}.is_valid())
    {
        return Shape::Malformed;
    }
    return Shape::Ok;
}

/// true for the groups that are single-frame only: a message of such a group that was assembled
/// from a transfer is not handed on (the engine's receiver ends the transfer without a message,
/// the sniffer reports SniffEnd::Refused).
[[nodiscard]] constexpr bool single_frame_only(Pgn pgn) noexcept
{
    return pgn == kPgnAddressClaimed || pgn == kPgnRequest;
}

} // namespace vehicle_sil::j1939::transport_wire
