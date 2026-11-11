// SPDX-License-Identifier: MIT
//
// J1939 value types of the J1939 stack (vehicle_sil::j1939): addresses, parameter group
// numbers, the 29-bit identifier and the received-message view. No SIL Kit.
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace vehicle_sil::j1939 {

/// A J1939 source or destination address.
using Address = std::uint8_t;

/// The address of a control function that has not claimed an address (J1939-81).
inline constexpr Address kNullAddress = 0xFE;
/// The destination address that addresses every node.
inline constexpr Address kGlobalAddress = 0xFF;

/// true for the addresses one control function can own (0..253).
[[nodiscard]] constexpr bool is_unicast(Address address) noexcept { return address < kNullAddress; }

/// Message priority, 0 (highest) .. 7 (lowest).
using Priority = std::uint8_t;
inline constexpr Priority kMaxPriority = 7;
inline constexpr Priority kDefaultPriority = 6;

/// A parameter group number: the 18 bits extended data page, data page, PDU format (PF) and
/// PDU specific (PS) of the identifier.
///
/// PDU1 (PF < 240) groups are sent to one destination; the destination travels in the PS field
/// of the identifier, so the PGN of a PDU1 group has PS = 0. PDU2 (PF >= 240) groups are
/// always sent to all nodes and PS is part of the PGN (group extension).
class Pgn
{
public:
    constexpr Pgn() noexcept = default;
    constexpr explicit Pgn(std::uint32_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    [[nodiscard]] constexpr std::uint8_t pdu_format() const noexcept { return static_cast<std::uint8_t>(value_ >> 8); }
    [[nodiscard]] constexpr std::uint8_t pdu_specific() const noexcept { return static_cast<std::uint8_t>(value_); }
    [[nodiscard]] constexpr bool data_page() const noexcept { return ((value_ >> 16) & 0x1u) != 0; }
    [[nodiscard]] constexpr bool extended_data_page() const noexcept { return ((value_ >> 17) & 0x1u) != 0; }
    /// PF < 240: destination specific.
    [[nodiscard]] constexpr bool is_pdu1() const noexcept { return pdu_format() < 0xF0u; }
    [[nodiscard]] constexpr bool is_pdu2() const noexcept { return !is_pdu1(); }

    /// Fits in 18 bits and, for PDU1, has PS = 0.
    [[nodiscard]] constexpr bool is_valid() const noexcept
    {
        return value_ <= 0x3FFFFu && (is_pdu2() || pdu_specific() == 0);
    }

    [[nodiscard]] friend constexpr bool operator==(Pgn, Pgn) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(Pgn, Pgn) noexcept = default;

private:
    std::uint32_t value_ = 0;
};

/// Parameter groups the stack itself handles.
inline constexpr Pgn kPgnRequest{0xEA00u};
inline constexpr Pgn kPgnAcknowledgement{0xE800u};
inline constexpr Pgn kPgnAddressClaimed{0xEE00u};
/// Commanded Address: 9 bytes (NAME, new address), so it arrives over the transport protocol.
inline constexpr Pgn kPgnCommandedAddress{0xFED8u};
/// The groups of the transport protocol: connection management (TP.CM) and the data packets
/// (TP.DT) of messages of 9 .. kMaxTransportLength bytes. The network takes their frames itself
/// and hands the assembled message to the handlers, never these frames; the constants name
/// them where frames are seen, e.g. a fault injector that drops a packet or a frame log.
inline constexpr Pgn kPgnTransportControl{0xEC00u};
inline constexpr Pgn kPgnTransportData{0xEB00u};

/// The fields of a 29-bit J1939 identifier.
struct Identifier
{
    Priority priority = kDefaultPriority;
    Pgn pgn{};
    Address destination = kGlobalAddress; ///< PDU2: always kGlobalAddress
    Address source = kNullAddress;

    /// The 29-bit CAN identifier. Throws std::invalid_argument if the PGN is not valid, the
    /// priority is above 7, or a PDU2 group has a destination other than kGlobalAddress.
    [[nodiscard]] std::uint32_t encode() const;

    /// The fields of `id` (29 bits; higher bits are ignored). The reserved and data page bits
    /// are part of the PGN. PDU1: the PS field is the destination and the PGN has PS = 0.
    [[nodiscard]] static Identifier decode(std::uint32_t id) noexcept;

    /// decode() of an extended-format frame; std::nullopt for an 11-bit frame (not J1939).
    [[nodiscard]] static std::optional<Identifier> decode(const can::Frame& frame) noexcept;

    [[nodiscard]] friend constexpr bool operator==(const Identifier&, const Identifier&) noexcept = default;
};

/// A received (or locally looped back) single-frame message, as handlers see it.
///
/// `data` points into a buffer of the network: it is valid only during the handler call.
struct Message
{
    Pgn pgn{};
    Priority priority = kDefaultPriority;
    Address source = kNullAddress;
    Address destination = kGlobalAddress;
    SimTime time{}; ///< The frame's time (the sender's step time on virtual time).
    std::span<const std::uint8_t> data{};
};

} // namespace vehicle_sil::j1939
