// SPDX-License-Identifier: MIT
//
// The 64-bit NAME of a J1939 control function (vehicle_sil::j1939).
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <span>

namespace vehicle_sil::j1939 {

/// The NAME of a control function: 64 bits that identify it and order it in address
/// arbitration (the lower value wins).
///
/// Fields, from the least significant bit: identity number (21 bits), manufacturer code (11),
/// ECU instance (3), function instance (5), function (8), reserved (1), vehicle system (7),
/// vehicle system instance (4), industry group (3), arbitrary address capable (1).
/// On the bus the NAME travels as 8 bytes, least significant byte first.
class Name
{
public:
    constexpr Name() noexcept = default;
    constexpr explicit Name(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    [[nodiscard]] constexpr std::uint32_t identity_number() const noexcept { return static_cast<std::uint32_t>(get(0, 21)); }
    [[nodiscard]] constexpr std::uint16_t manufacturer_code() const noexcept { return static_cast<std::uint16_t>(get(21, 11)); }
    [[nodiscard]] constexpr std::uint8_t ecu_instance() const noexcept { return static_cast<std::uint8_t>(get(32, 3)); }
    [[nodiscard]] constexpr std::uint8_t function_instance() const noexcept { return static_cast<std::uint8_t>(get(35, 5)); }
    [[nodiscard]] constexpr std::uint8_t function() const noexcept { return static_cast<std::uint8_t>(get(40, 8)); }
    [[nodiscard]] constexpr bool reserved() const noexcept { return get(48, 1) != 0; }
    [[nodiscard]] constexpr std::uint8_t vehicle_system() const noexcept { return static_cast<std::uint8_t>(get(49, 7)); }
    [[nodiscard]] constexpr std::uint8_t vehicle_system_instance() const noexcept { return static_cast<std::uint8_t>(get(56, 4)); }
    [[nodiscard]] constexpr std::uint8_t industry_group() const noexcept { return static_cast<std::uint8_t>(get(60, 3)); }
    [[nodiscard]] constexpr bool arbitrary_address_capable() const noexcept { return get(63, 1) != 0; }

    // Setters keep the other fields; bits above a field's width are dropped.
    constexpr Name& set_identity_number(std::uint32_t v) noexcept { return set(0, 21, v); }
    constexpr Name& set_manufacturer_code(std::uint16_t v) noexcept { return set(21, 11, v); }
    constexpr Name& set_ecu_instance(std::uint8_t v) noexcept { return set(32, 3, v); }
    constexpr Name& set_function_instance(std::uint8_t v) noexcept { return set(35, 5, v); }
    constexpr Name& set_function(std::uint8_t v) noexcept { return set(40, 8, v); }
    constexpr Name& set_reserved(bool v) noexcept { return set(48, 1, v ? 1u : 0u); }
    constexpr Name& set_vehicle_system(std::uint8_t v) noexcept { return set(49, 7, v); }
    constexpr Name& set_vehicle_system_instance(std::uint8_t v) noexcept { return set(56, 4, v); }
    constexpr Name& set_industry_group(std::uint8_t v) noexcept { return set(60, 3, v); }
    constexpr Name& set_arbitrary_address_capable(bool v) noexcept { return set(63, 1, v ? 1u : 0u); }

    /// The 8 bytes on the bus, least significant byte first.
    [[nodiscard]] constexpr std::array<std::uint8_t, 8> to_bytes() const noexcept
    {
        std::array<std::uint8_t, 8> out{};
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            out[i] = static_cast<std::uint8_t>(value_ >> (8 * i));
        }
        return out;
    }

    /// The NAME in the first 8 bytes of `bytes` (least significant first). Missing bytes (fewer
    /// than 8) count as 0; callers check the length first.
    [[nodiscard]] static constexpr Name from_bytes(std::span<const std::uint8_t> bytes) noexcept
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8 && i < bytes.size(); ++i)
        {
            value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
        }
        return Name{value};
    }

    /// Arbitration order: the lower value has the higher priority.
    [[nodiscard]] friend constexpr bool operator==(Name, Name) noexcept = default;
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(Name a, Name b) noexcept { return a.value_ <=> b.value_; }

private:
    [[nodiscard]] constexpr std::uint64_t get(unsigned shift, unsigned bits) const noexcept
    {
        return (value_ >> shift) & ((std::uint64_t{1} << bits) - 1);
    }
    constexpr Name& set(unsigned shift, unsigned bits, std::uint64_t v) noexcept
    {
        const std::uint64_t mask = ((std::uint64_t{1} << bits) - 1) << shift;
        value_ = (value_ & ~mask) | ((v << shift) & mask);
        return *this;
    }

    std::uint64_t value_ = 0;
};

} // namespace vehicle_sil::j1939
