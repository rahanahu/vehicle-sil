// SPDX-License-Identifier: MIT
//
// Helpers for the tests of the protocol parts of the J1939 stack (DM13, diagnostics,
// identification): the expected payloads written out independently of the parts, and a tester
// that receives the node's transfers (BAM, RTS/CTS) frame by frame.
#pragma once

#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::unit::j1939 {

inline constexpr std::uint32_t kPgnDm1 = 0xFECA;
inline constexpr std::uint32_t kPgnDm2 = 0xFECB;
inline constexpr std::uint32_t kPgnDm3 = 0xFECC;
inline constexpr std::uint32_t kPgnDm11 = 0xFED3;
inline constexpr std::uint32_t kPgnDm13 = 0xDF00;
inline constexpr std::uint32_t kPgnDm22 = 0xC300;
inline constexpr std::uint32_t kPgnSoftwareId = 0xFEDA;
inline constexpr std::uint32_t kPgnEcuId = 0xFDC5;
inline constexpr std::uint32_t kPgnProductId = 0xFC8D;
inline constexpr std::uint32_t kPgnDiagnosticProtocolId = 0xFD32;

/// A DTC in DM1/DM2: SPN (19 bits, the top 3 in byte 3), FMI, occurrence count.
[[nodiscard]] inline Bytes dtc_bytes(std::uint32_t spn, std::uint8_t fmi, std::uint8_t occurrences)
{
    return {static_cast<std::uint8_t>(spn), static_cast<std::uint8_t>(spn >> 8),
            static_cast<std::uint8_t>((((spn >> 16) & 0x7u) << 5) | (fmi & 0x1Fu)), static_cast<std::uint8_t>(occurrences & 0x7Fu)};
}

/// DM1/DM2: lamp status, lamp flash, DTCs (4 zero bytes without); padded to 8 bytes.
[[nodiscard]] inline Bytes dm_payload(std::uint8_t lamps, std::uint8_t flash, const std::vector<Bytes>& dtcs)
{
    // Sized first and filled in place: growing a 2-byte vector makes GCC 16 (-O3) warn falsely
    // about its bounds.
    Bytes out(std::max<std::size_t>(2 + 4 * std::max<std::size_t>(dtcs.size(), 1), 8), 0xFF);
    out[0] = lamps;
    out[1] = flash;
    std::size_t at = 2;
    for (const auto& dtc : dtcs)
    {
        for (const std::uint8_t b : dtc)
        {
            out[at++] = b;
        }
    }
    if (dtcs.empty())
    {
        std::fill_n(out.begin() + 2, 4, std::uint8_t{0});
    }
    return out;
}

inline const Bytes kNoDtcs = dm_payload(0x00, 0xFF, {});

/// DM13: one 2-bit command per network (byte n / 4, bits 2 * (n % 4)), others "don't care";
/// byte 4: hold signal (high nibble), suspend signal (low nibble); bytes 5-6 the duration.
[[nodiscard]] inline Bytes dm13(int network, std::uint8_t command, std::uint8_t suspendSignal = 0xF,
                                std::uint16_t duration = 0xFFFF, std::uint8_t holdSignal = 0xF)
{
    Bytes out{0xFF, 0xFF, 0xFF, static_cast<std::uint8_t>((holdSignal << 4) | (suspendSignal & 0xFu)),
              static_cast<std::uint8_t>(duration), static_cast<std::uint8_t>(duration >> 8), 0xFF, 0xFF};
    if (network >= 0)
    {
        const unsigned n = static_cast<unsigned>(network);
        const unsigned shift = 2u * (n % 4u);
        out[n / 4u] = static_cast<std::uint8_t>((out[n / 4u] & ~(0x3u << shift)) | ((command & 0x3u) << shift));
    }
    return out;
}
inline constexpr std::uint8_t kStop = 0;
inline constexpr std::uint8_t kStart = 1;
inline constexpr int kNetwork1 = 0;
inline constexpr int kJ1587 = 2;
inline constexpr int kCurrentDataLink = 3;
inline constexpr int kNetwork2 = 7;
inline constexpr int kProprietary1 = 10;

[[nodiscard]] inline Bytes text(const std::string& value) { return Bytes(value.begin(), value.end()); }
[[nodiscard]] inline Bytes padded(Bytes value)
{
    value.resize(std::max<std::size_t>(value.size(), 8), 0xFF);
    return value;
}

/// A frame the network sent and the step it left in.
struct Sent
{
    long long ms;
    J1939Frame frame;
};

/// Runs `steps` steps and returns what the network sent, with the step times.
inline std::vector<Sent> record(Bench& bench, int steps)
{
    std::vector<Sent> out;
    for (int i = 0; i < steps; ++i)
    {
        bench.step();
        const long long ms = static_cast<long long>(to_ns(bench.last_time()) / 1000000);
        for (auto& f : bench.take_sent())
        {
            out.push_back(Sent{ms, std::move(f)});
        }
    }
    return out;
}

[[nodiscard]] inline std::vector<Sent> only(const std::vector<Sent>& frames, std::uint32_t pgn)
{
    std::vector<Sent> out;
    std::copy_if(frames.begin(), frames.end(), std::back_inserter(out), [pgn](const Sent& s) { return s.frame.pgn == pgn; });
    return out;
}

/// The messages of `pgn` that `sa` broadcast with BAM in `frames`, reassembled.
[[nodiscard]] inline std::vector<Bytes> bam_messages(const std::vector<Sent>& frames, std::uint8_t sa, std::uint32_t pgn)
{
    std::vector<Bytes> out;
    std::optional<Bytes> current;
    std::size_t size = 0;
    for (const auto& s : frames)
    {
        const auto& f = s.frame;
        if (f.sa != sa || f.da != kGlobal)
        {
            continue;
        }
        if (f.pgn == kPgnTpCm && f.data[0] == 32)
        {
            const auto p = static_cast<std::uint32_t>(f.data[5] | (f.data[6] << 8) | (f.data[7] << 16));
            current.reset();
            if (p == pgn)
            {
                current = Bytes{};
                size = f.data[1] | (f.data[2] << 8);
            }
        }
        else if (f.pgn == kPgnTpDt && current)
        {
            current->insert(current->end(), f.data.begin() + 1, f.data.end());
            if (current->size() >= size)
            {
                current->resize(size);
                out.push_back(*current);
                current.reset();
            }
        }
    }
    return out;
}

/// Plays `tool` receiving a connection (RTS/CTS) of `pgn` from `sa`: steps until the RTS, sends
/// one clear-to-send for all packets, steps until the last packet and acknowledges it. Returns
/// the message (empty if no RTS came within `limit` steps). Other frames go to `others`.
inline Bytes receive_rts_cts(Bench& bench, std::uint8_t sa, std::uint8_t tool, std::uint32_t pgn,
                             std::vector<J1939Frame>* others = nullptr, int limit = 300)
{
    std::size_t size = 0;
    std::uint8_t packets = 0;
    Bytes message;
    bool open = false;
    for (int i = 0; i < limit; ++i)
    {
        bench.step();
        for (const auto& f : bench.take_sent())
        {
            const bool ours = f.sa == sa && f.da == tool;
            if (ours && f.pgn == kPgnTpCm && f.data[0] == 16 && static_cast<std::uint32_t>(f.data[5] | (f.data[6] << 8) | (f.data[7] << 16)) == pgn)
            {
                size = f.data[1] | (f.data[2] << 8);
                packets = f.data[3];
                open = true;
                bench.inject(cts(tool, sa, packets, 1, pgn));
            }
            else if (ours && f.pgn == kPgnTpDt && open)
            {
                message.insert(message.end(), f.data.begin() + 1, f.data.end());
                if (f.data[0] == packets)
                {
                    message.resize(size);
                    bench.inject(eoma(tool, sa, size, pgn));
                    return message;
                }
            }
            else if (others != nullptr)
            {
                others->push_back(f);
            }
        }
    }
    return {};
}

} // namespace vehicle_sil::unit::j1939
