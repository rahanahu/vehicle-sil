// SPDX-License-Identifier: MIT
//
// Frames and addresses for the vehicle_sil::eth tests.
#pragma once

#include "vehicle_sil/eth/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vehicle_sil::unit {

/// Locally administered unicast addresses 02:00:00:00:00:<n>.
inline eth::MacAddress mac(std::uint8_t n)
{
    return eth::MacAddress{{0x02, 0x00, 0x00, 0x00, 0x00, n}};
}

/// A locally administered multicast (group) address 03:00:00:00:00:<n>.
inline eth::MacAddress group(std::uint8_t n)
{
    return eth::MacAddress{{0x03, 0x00, 0x00, 0x00, 0x00, n}};
}

/// EtherType the tests put into their frames: a value known as one for local experiments
/// (from general references, not checked against the standard). vehicle-sil does not read it.
inline constexpr std::uint16_t kTestEtherType = 0x88B5;

/// `length` bytes (at least 16): destination, source, kTestEtherType, then `tag` and the
/// sequence number `seq`, then zero bytes.
inline std::vector<std::uint8_t> frame_bytes(const eth::MacAddress& to, const eth::MacAddress& from, std::uint8_t tag,
                                             std::uint8_t seq, std::size_t length)
{
    std::vector<std::uint8_t> bytes(length < 16 ? 16 : length, 0);
    for (std::size_t i = 0; i < 6; ++i)
    {
        bytes[i] = to.bytes[i];
        bytes[6 + i] = from.bytes[i];
    }
    bytes[12] = static_cast<std::uint8_t>(kTestEtherType >> 8);
    bytes[13] = static_cast<std::uint8_t>(kTestEtherType & 0xFF);
    bytes[14] = tag;
    bytes[15] = seq;
    return bytes;
}

/// The same as frame_bytes(), as a Frame (without allocating; `length` at most
/// Frame::kCapacity).
inline eth::Frame make_frame(const eth::MacAddress& to, const eth::MacAddress& from, std::uint8_t tag, std::uint8_t seq,
                             std::size_t length = 64)
{
    std::array<std::uint8_t, eth::Frame::kCapacity> bytes{};
    for (std::size_t i = 0; i < 6; ++i)
    {
        bytes[i] = to.bytes[i];
        bytes[6 + i] = from.bytes[i];
    }
    bytes[12] = static_cast<std::uint8_t>(kTestEtherType >> 8);
    bytes[13] = static_cast<std::uint8_t>(kTestEtherType & 0xFF);
    bytes[14] = tag;
    bytes[15] = seq;
    eth::Frame frame;
    static_cast<void>(frame.assign(std::span<const std::uint8_t>(bytes.data(), length < 16 ? 16 : length)));
    return frame;
}

} // namespace vehicle_sil::unit
