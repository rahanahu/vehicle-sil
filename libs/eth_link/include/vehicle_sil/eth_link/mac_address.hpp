// SPDX-License-Identifier: MIT
//
// MAC addresses and the fields at fixed places of an Ethernet frame (no SIL Kit).
#pragma once

#include <array>
#include <atomic>
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
    /// The address whose value() is the low 48 bits of `value`.
    [[nodiscard]] static constexpr MacAddress from_value(std::uint64_t value) noexcept
    {
        MacAddress address;
        for (std::size_t i = address.bytes.size(); i-- > 0;)
        {
            address.bytes[i] = static_cast<std::uint8_t>(value & 0xFFu);
            value >>= 8;
        }
        return address;
    }

    [[nodiscard]] friend constexpr bool operator==(const MacAddress&, const MacAddress&) noexcept = default;
};

/// A MAC address or none, that one thread may set while others read it, without a lock.
class AtomicMacAddress
{
public:
    void store(const std::optional<MacAddress>& address) noexcept { value_.store(address ? address->value() | kKnown : 0); }
    [[nodiscard]] std::optional<MacAddress> load() const noexcept
    {
        const std::uint64_t value = value_.load();
        return (value & kKnown) != 0 ? std::optional<MacAddress>(MacAddress::from_value(value)) : std::nullopt;
    }
    /// Whether an address is set and it is `address`.
    [[nodiscard]] bool is(const MacAddress& address) const noexcept { return value_.load() == (address.value() | kKnown); }

private:
    static constexpr std::uint64_t kKnown = std::uint64_t{1} << 48;
    std::atomic<std::uint64_t> value_{0};
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
