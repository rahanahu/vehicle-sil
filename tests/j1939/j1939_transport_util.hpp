// SPDX-License-Identifier: MIT
//
// Transport protocol frames for the tests of the J1939 stack, written out
// independently of the library under test.
#pragma once

#include "j1939_bench.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

/// CHECK_J1939 for a frame list that is not a braced list.
#define CHECK_FRAMES(actual, ...) ::vehicle_sil::unit::j1939::check_frames((actual), (__VA_ARGS__), __FILE__, __LINE__)

namespace vehicle_sil::unit::j1939 {

// Written out independently of the stack; the public constants must match them.
inline constexpr std::uint32_t kPgnTpCm = 0xEC00;
inline constexpr std::uint32_t kPgnTpDt = 0xEB00;
static_assert(vehicle_sil::j1939::kPgnTransportControl.value() == kPgnTpCm);
static_assert(vehicle_sil::j1939::kPgnTransportData.value() == kPgnTpDt);

/// The test message of `length` bytes: a pattern that differs in every packet.
[[nodiscard]] inline Bytes payload(std::size_t length, std::uint8_t seed = 0)
{
    Bytes out(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        out[i] = static_cast<std::uint8_t>(i * 7 + seed + (i >> 8));
    }
    return out;
}

[[nodiscard]] inline std::uint8_t packet_count(std::size_t length) { return static_cast<std::uint8_t>((length + 6) / 7); }

[[nodiscard]] inline Bytes cm(std::uint8_t code, std::uint8_t b1, std::uint8_t b2, std::uint8_t b3, std::uint8_t b4, std::uint32_t pgn)
{
    return {code, b1, b2, b3, b4, static_cast<std::uint8_t>(pgn), static_cast<std::uint8_t>(pgn >> 8),
            static_cast<std::uint8_t>(pgn >> 16)};
}

/// TP.CM request to send: size, packets, the most packets per clear-to-send.
[[nodiscard]] inline J1939Frame rts(std::uint8_t sa, std::uint8_t da, std::size_t size, std::uint32_t pgn,
                                    std::uint8_t maxPerCts = 0xFF, int packets = -1)
{
    return frame(7, kPgnTpCm, sa, da,
                 cm(16, static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                    packets < 0 ? packet_count(size) : static_cast<std::uint8_t>(packets), maxPerCts, pgn));
}

[[nodiscard]] inline J1939Frame cts(std::uint8_t sa, std::uint8_t da, std::uint8_t count, std::uint8_t next, std::uint32_t pgn)
{
    return frame(7, kPgnTpCm, sa, da, cm(17, count, next, 0xFF, 0xFF, pgn));
}

[[nodiscard]] inline J1939Frame eoma(std::uint8_t sa, std::uint8_t da, std::size_t size, std::uint32_t pgn)
{
    return frame(7, kPgnTpCm, sa, da,
                 cm(19, static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8), packet_count(size), 0xFF, pgn));
}

[[nodiscard]] inline J1939Frame bam(std::uint8_t sa, std::size_t size, std::uint32_t pgn, int packets = -1,
                                    std::uint8_t da = kGlobal)
{
    return frame(7, kPgnTpCm, sa, da,
                 cm(32, static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                    packets < 0 ? packet_count(size) : static_cast<std::uint8_t>(packets), 0xFF, pgn));
}

[[nodiscard]] inline J1939Frame conn_abort(std::uint8_t sa, std::uint8_t da, std::uint8_t reason, std::uint32_t pgn)
{
    return frame(7, kPgnTpCm, sa, da, cm(255, reason, 0xFF, 0xFF, 0xFF, pgn));
}

/// Packet `sequence` (1-based) of `message`, padded with 0xFF.
[[nodiscard]] inline J1939Frame dt(std::uint8_t sa, std::uint8_t da, std::uint8_t sequence, const Bytes& message)
{
    Bytes data(8, 0xFF);
    data[0] = sequence;
    for (std::size_t i = 0; i < 7; ++i)
    {
        const std::size_t at = (static_cast<std::size_t>(sequence) - 1) * 7 + i;
        if (at < message.size())
        {
            data[1 + i] = message[at];
        }
    }
    return frame(7, kPgnTpDt, sa, da, data);
}

/// Packets first..last of `message`.
[[nodiscard]] inline std::vector<J1939Frame> dts(std::uint8_t sa, std::uint8_t da, const Bytes& message, int first, int last)
{
    std::vector<J1939Frame> out;
    for (int k = first; k <= last; ++k)
    {
        out.push_back(dt(sa, da, static_cast<std::uint8_t>(k), message));
    }
    return out;
}

/// Concatenates frame lists.
[[nodiscard]] inline std::vector<J1939Frame> operator+(std::vector<J1939Frame> a, const std::vector<J1939Frame>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// Records the transfer ends a node reports.
struct Ends
{
    std::vector<vehicle_sil::j1939::TransferEnd> list;
    void attach(vehicle_sil::j1939::Node& node)
    {
        node.on_transfer_end([this](const vehicle_sil::j1939::TransferEnd& e) { list.push_back(e); });
    }
};

} // namespace vehicle_sil::unit::j1939
