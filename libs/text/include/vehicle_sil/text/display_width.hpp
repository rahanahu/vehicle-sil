// SPDX-License-Identifier: MIT
//
// How many columns of a console a UTF-8 text takes: what the tables of vehicle_sil_launch and the
// Ethernet bridge's --list-interfaces pad a column with, so that a name in Japanese does not put
// the next column out of line.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace vehicle_sil::text {

/// The columns that the UTF-8 text `text` takes in a console: one for each character, two for an
/// East Asian wide one (Hangul, kana, CJK ideographs and symbols, full-width forms). Combining
/// marks and emoji count one, which can be wrong; a byte that does not start a character counts one.
[[nodiscard]] inline std::size_t display_width(std::string_view text) noexcept
{
    // The blocks of wide characters, first and last code point.
    constexpr std::array<std::pair<char32_t, char32_t>, 12> kWide{{{0x1100, 0x115F},
                                                                  {0x2E80, 0x303E},
                                                                  {0x3041, 0x33FF},
                                                                  {0x3400, 0x4DBF},
                                                                  {0x4E00, 0x9FFF},
                                                                  {0xA000, 0xA4CF},
                                                                  {0xAC00, 0xD7A3},
                                                                  {0xF900, 0xFAFF},
                                                                  {0xFE30, 0xFE4F},
                                                                  {0xFF00, 0xFF60},
                                                                  {0xFFE0, 0xFFE6},
                                                                  {0x20000, 0x3FFFD}}};
    std::size_t width = 0;
    std::size_t at = 0;
    while (at < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[at]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        bool whole = length != 0 && at + length <= text.size();
        char32_t code = length <= 1 ? lead : lead & (0x7Fu >> length);
        for (std::size_t i = 1; whole && i < length; ++i)
        {
            const auto next = static_cast<unsigned char>(text[at + i]);
            whole = (next & 0xC0) == 0x80;
            code = (code << 6) | (next & 0x3Fu);
        }
        if (!whole)
        {
            ++width;
            ++at;
            continue;
        }
        bool wide = false;
        for (const auto& [first, last] : kWide)
        {
            wide = wide || (code >= first && code <= last);
        }
        width += wide ? 2 : 1;
        at += length;
    }
    return width;
}

} // namespace vehicle_sil::text
