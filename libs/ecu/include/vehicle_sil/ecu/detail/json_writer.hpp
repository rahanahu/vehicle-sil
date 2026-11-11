// SPDX-License-Identifier: MIT
//
// The JSON writer of to_json() (vehicle_sil/ecu/describe.hpp), shared with the vehicle-sil tools
// that write JSON in the same layout. Not part of the API.
#pragma once

#include "vehicle_sil/ecu/describe.hpp"

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

namespace vehicle_sil::detail {

/// Appends JSON text with a fixed layout: one member or element per line, two spaces per level.
/// An empty key starts an element of an array; any other key a member of an object. Strings:
/// valid UTF-8 as it is, '"', '\\' and the control characters escaped, a byte that starts no
/// valid UTF-8 sequence as U+FFFD.
class JsonWriter
{
public:
    [[nodiscard]] std::string take() { return std::move(out_); }

    void begin_object(std::string_view key = {}) { open(key, '{'); }
    void end_object() { close('}'); }
    void begin_array(std::string_view key) { open(key, '['); }
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
        first_ = false;
        if (depth_ > 0)
        {
            out_ += '\n';
            out_.append(static_cast<std::size_t>(depth_) * 2, ' ');
        }
        if (!key.empty())
        {
            quoted(key);
            out_ += ": ";
        }
    }
    void open(std::string_view key, char bracket)
    {
        member(key);
        out_ += bracket;
        ++depth_;
        first_ = true;
    }
    void close(char bracket)
    {
        --depth_;
        if (!first_) // not empty: the bracket on a line of its own
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

} // namespace vehicle_sil::detail
