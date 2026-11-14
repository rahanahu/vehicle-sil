// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/mac_address.hpp"

#include <algorithm>

namespace vehicle_sil::eth_link {
namespace {

int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

std::optional<MacAddress> mac_at(std::span<const std::uint8_t> frame, std::size_t offset) noexcept
{
    if (frame.size() < kEthernetHeaderBytes)
    {
        return std::nullopt;
    }
    MacAddress address;
    std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(offset), address.bytes.size(), address.bytes.begin());
    return address;
}

} // namespace

std::optional<MacAddress> parse_mac_address(std::string_view text)
{
    // "xx:xx:xx:xx:xx:xx": 6 * 2 digits and 5 separators.
    if (text.size() != 17)
    {
        return std::nullopt;
    }
    const char separator = text[2];
    if (separator != ':' && separator != '-')
    {
        return std::nullopt;
    }
    MacAddress address;
    for (std::size_t i = 0; i < address.bytes.size(); ++i)
    {
        const std::size_t at = i * 3;
        if (i > 0 && text[at - 1] != separator)
        {
            return std::nullopt;
        }
        const int high = hex_digit(text[at]);
        const int low = hex_digit(text[at + 1]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        address.bytes[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return address;
}

std::string to_string(const MacAddress& address)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(17);
    for (std::size_t i = 0; i < address.bytes.size(); ++i)
    {
        if (i > 0)
        {
            text.push_back(':');
        }
        text.push_back(kDigits[address.bytes[i] >> 4]);
        text.push_back(kDigits[address.bytes[i] & 0x0Fu]);
    }
    return text;
}

std::optional<MacAddress> destination_of(std::span<const std::uint8_t> frame) noexcept
{
    return mac_at(frame, 0);
}

std::optional<MacAddress> source_of(std::span<const std::uint8_t> frame) noexcept
{
    return mac_at(frame, 6);
}

} // namespace vehicle_sil::eth_link
