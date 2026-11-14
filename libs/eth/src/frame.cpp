// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth/frame.hpp"

namespace vehicle_sil::eth {

namespace {

int hex_value(char c) noexcept
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

} // namespace

std::optional<MacAddress> MacAddress::parse(std::string_view text) noexcept
{
    // Six pairs of hex digits with one separator between them: 6 * 2 + 5 characters.
    constexpr std::size_t kLength = 17;
    if (text.size() != kLength)
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
        const int high = hex_value(text[at]);
        const int low = hex_value(text[at + 1]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        address.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return address;
}

std::string MacAddress::to_string() const
{
    constexpr const char* kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(17);
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (i > 0)
        {
            text.push_back(':');
        }
        text.push_back(kDigits[bytes[i] >> 4]);
        text.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return text;
}

} // namespace vehicle_sil::eth
