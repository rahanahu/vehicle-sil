// SPDX-License-Identifier: MIT
//
// MAC addresses and the fields at fixed places of an Ethernet frame (no SIL Kit).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vehicle_sil::eth_link {

/// Destination MAC (6 bytes), source MAC (6 bytes), EtherType or the first VLAN tag's TPID (2 bytes).
inline constexpr std::size_t kEthernetHeaderBytes = 14;

struct MacAddress
{
    std::array<std::uint8_t, 6> bytes{};

    /// The group bit (the lowest bit of the first byte): broadcast and multicast addresses.
    [[nodiscard]] constexpr bool is_group() const noexcept { return (bytes[0] & 0x01u) != 0; }
    /// The six bytes as one number (for hashing and ordering).
    [[nodiscard]] constexpr std::uint64_t value() const noexcept
    {
        std::uint64_t v = 0;
        for (const auto b : bytes)
        {
            v = (v << 8) | b;
        }
        return v;
    }

    [[nodiscard]] friend constexpr bool operator==(const MacAddress&, const MacAddress&) noexcept = default;
};

/// "02:00:00:00:00:01" or "02-00-00-00-00-01" (the form Windows shows), either case. Six bytes of
/// two hex digits each, one kind of separator. nullopt if `text` is not such an address.
[[nodiscard]] std::optional<MacAddress> parse_mac_address(std::string_view text);

/// Lower case with colons: "02:00:00:00:00:01".
[[nodiscard]] std::string to_string(const MacAddress& address);

/// The destination MAC of `frame`; nullopt if the frame is shorter than kEthernetHeaderBytes.
[[nodiscard]] std::optional<MacAddress> destination_of(std::span<const std::uint8_t> frame) noexcept;
/// The source MAC of `frame`; nullopt if the frame is shorter than kEthernetHeaderBytes.
[[nodiscard]] std::optional<MacAddress> source_of(std::span<const std::uint8_t> frame) noexcept;

} // namespace vehicle_sil::eth_link
