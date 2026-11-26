// SPDX-License-Identifier: MIT
//
// The JSON writer of vehicle-sil's libraries and tools (vehicle_sil::json, header only, no other
// vehicle-sil library): the output of an ECU's --describe (vehicle_sil/ecu/describe.hpp:
// to_json()) and the JSON that vehicle_sil_launch writes, in one layout. Not part of the API.
#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace vehicle_sil::json {

/// The length of the valid UTF-8 sequence at the start of `text` (1 to 4), or 0 if it starts
/// with none (an empty text, a stray or missing continuation byte, an overlong form, a surrogate,
/// a value above U+10FFFF).
[[nodiscard]] inline std::size_t utf8_sequence_length(std::string_view text) noexcept
{
    if (text.empty())
    {
        return 0;
    }
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char b0 = byte(0);
    if (b0 < 0x80)
    {
        return 1;
    }
    std::size_t n = 0;
    unsigned char low = 0x80; // the range of the second byte (no overlong form, no surrogate,
    unsigned char high = 0xBF; // nothing above U+10FFFF)
    if (b0 >= 0xC2 && b0 <= 0xDF)
    {
        n = 2;
    }
    else if (b0 >= 0xE0 && b0 <= 0xEF)
    {
        n = 3;
        low = b0 == 0xE0 ? 0xA0 : 0x80;
        high = b0 == 0xED ? 0x9F : 0xBF;
    }
    else if (b0 >= 0xF0 && b0 <= 0xF4)
    {
        n = 4;
        low = b0 == 0xF0 ? 0x90 : 0x80;
        high = b0 == 0xF4 ? 0x8F : 0xBF;
    }
    else
    {
        return 0;
    }
    if (text.size() < n || byte(1) < low || byte(1) > high)
    {
        return 0;
    }
    for (std::size_t i = 2; i < n; ++i)
    {
        if (byte(i) < 0x80 || byte(i) > 0xBF)
        {
            return 0;
        }
    }
    return n;
}

/// Appends JSON text with a fixed layout: one member or element per line, two spaces per level.
/// An empty key starts an element of an array; any other key a member of an object. Strings:
/// valid UTF-8 as it is, '"', '\\' and the control characters escaped, a byte that starts no
/// valid UTF-8 sequence as U+FFFD. Compact: the same without any line end or space (one line).
/// An array begun with begin_array_on_one_line() is written on one line with all that is in it,
/// its elements ", " apart ("[10, 50.3]").
class JsonWriter
{
public:
    JsonWriter() = default;
    explicit JsonWriter(bool compact) : compact_(compact) {}

    [[nodiscard]] std::string take() { return std::move(out_); }

    void begin_object(std::string_view key = {}) { open(key, '{'); }
    void end_object() { close('}'); }
    void begin_array(std::string_view key) { open(key, '['); }
    /// The array and everything in it on one line (a point of a long list: one line per point).
    void begin_array_on_one_line(std::string_view key)
    {
        open(key, '[');
        if (one_line_ == 0)
        {
            one_line_ = 1; // (inside another such array, open() has counted it already)
        }
    }
    void end_array() { close(']'); }

    void string(std::string_view key, std::string_view value)
    {
        member(key);
        quoted(value);
    }
    void integer(std::string_view key, std::int64_t value)
    {
        member(key);
        out_ += std::to_string(value);
    }
    void unsigned_integer(std::string_view key, std::uint64_t value)
    {
        member(key);
        out_ += std::to_string(value);
    }
    void boolean(std::string_view key, bool value)
    {
        member(key);
        out_ += value ? "true" : "false";
    }
    void null(std::string_view key)
    {
        member(key);
        out_ += "null";
    }
    /// The shortest text that reads back to `value`; null if it is not finite.
    void real(std::string_view key, const std::optional<double>& value)
    {
        // 32 characters hold any double in its shortest form ("-2.2250738585072014e-308" is 24).
        std::array<char, 32> buffer{};
        const auto result = value && std::isfinite(*value)
                                ? std::to_chars(buffer.data(), buffer.data() + buffer.size(), *value)
                                : std::to_chars_result{buffer.data(), std::errc::invalid_argument};
        if (result.ec != std::errc{})
        {
            null(key);
            return;
        }
        member(key);
        out_.append(buffer.data(), result.ptr);
    }

private:
    /// Starts a member (with its key, in an object) or an element (in an array).
    void member(std::string_view key)
    {
        if (!first_)
        {
            out_ += ',';
        }
        if (one_line_ > 0)
        {
            if (!first_ && !compact_)
            {
                out_ += ' ';
            }
        }
        else if (depth_ > 0 && !compact_)
        {
            out_ += '\n';
            out_.append(static_cast<std::size_t>(depth_) * 2, ' ');
        }
        first_ = false;
        if (!key.empty())
        {
            quoted(key);
            out_ += compact_ ? ":" : ": ";
        }
    }
    void open(std::string_view key, char bracket)
    {
        member(key);
        out_ += bracket;
        ++depth_;
        first_ = true;
        if (one_line_ > 0)
        {
            ++one_line_; // how deep in the array on one line (1: in the array itself)
        }
    }
    void close(char bracket)
    {
        --depth_;
        if (one_line_ > 0)
        {
            --one_line_;
        }
        else if (!first_ && !compact_) // not empty: the bracket on a line of its own
        {
            out_ += '\n';
            out_.append(static_cast<std::size_t>(depth_) * 2, ' ');
        }
        out_ += bracket;
        first_ = false;
    }
    void quoted(std::string_view text)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        out_ += '"';
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (static_cast<unsigned char>(c) >= 0x80)
            {
                // A valid UTF-8 sequence as it is; anything else as U+FFFD, one per byte that
                // starts no valid sequence, so that the output is always valid JSON.
                const std::size_t n = utf8_sequence_length(text.substr(i));
                if (n == 0)
                {
                    out_ += "\xEF\xBF\xBD";
                }
                else
                {
                    out_.append(text.substr(i, n));
                    i += n - 1;
                }
                continue;
            }
            switch (c)
            {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\b':
                out_ += "\\b";
                break;
            case '\f':
                out_ += "\\f";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    out_ += "\\u00";
                    out_ += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out_ += kHex[static_cast<unsigned char>(c) & 0xF];
                }
                else
                {
                    out_ += c;
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    int depth_ = 0;
    bool first_ = true;
    bool compact_ = false;
    int one_line_ = 0; ///< > 0: in an array on one line, this deep
};

/// "0x" and 16 upper-case hexadecimal digits (a J1939 NAME in JSON).
[[nodiscard]] inline std::string hex64(std::uint64_t value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "0x";
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        out += kHex[(value >> shift) & 0xF];
    }
    return out;
}

} // namespace vehicle_sil::json
