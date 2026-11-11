// SPDX-License-Identifier: MIT
#include "vehicle_sil/ecu/describe.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace vehicle_sil {

namespace {

/// std::logic_error for a value outside the enumeration (a cast): to_json() writes no made-up name.
[[noreturn]] void bad_value(const char* what, int value)
{
    throw std::logic_error(std::string("vehicle_sil::to_json(): ") + what + " " + std::to_string(value) +
                           " is not one of its enumerators");
}

const char* type_name(control::ValueType type)
{
    switch (type)
    {
    case control::ValueType::Bool:
        return "bool";
    case control::ValueType::Integer:
        return "integer";
    case control::ValueType::Real:
        return "real";
    case control::ValueType::Enum:
        return "enum";
    }
    bad_value("control::ValueType", static_cast<int>(type));
}

const char* causality_name(control::Causality causality)
{
    switch (causality)
    {
    case control::Causality::Input:
        return "input";
    case control::Causality::Output:
        return "output";
    case control::Causality::Parameter:
        return "parameter";
    }
    bad_value("control::Causality", static_cast<int>(causality));
}

/// Appends JSON text with a fixed layout: one member or element per line, two spaces per level.
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
                const std::size_t n = detail::utf8_sequence_length(text.substr(i));
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

/// "0x" and 16 upper-case hexadecimal digits.
std::string hex64(std::uint64_t value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "0x";
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        out += kHex[(value >> shift) & 0xF];
    }
    return out;
}

} // namespace

namespace detail {

std::size_t utf8_sequence_length(std::string_view text) noexcept
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

bool is_utf8(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size();)
    {
        const std::size_t n = utf8_sequence_length(text.substr(i));
        if (n == 0)
        {
            return false;
        }
        i += n;
    }
    return true;
}

} // namespace detail

const char* version() noexcept { return VEHICLE_SIL_VERSION; }

const char* to_string(NetworkKind kind)
{
    switch (kind)
    {
    case NetworkKind::Can:
        return "can";
    case NetworkKind::Lin:
        return "lin";
    }
    bad_value("NetworkKind", static_cast<int>(kind));
}

const char* to_string(NetworkUse use)
{
    switch (use)
    {
    case NetworkUse::Raw:
        return "raw";
    case NetworkUse::J1939:
        return "j1939";
    case NetworkUse::LinMaster:
        return "lin_master";
    case NetworkUse::LinSlave:
        return "lin_slave";
    }
    bad_value("NetworkUse", static_cast<int>(use));
}

std::string to_json(const EcuDescription& description)
{
    JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_ecu", kEcuDescriptionFormat);
    w.string("vehicle_sil", version());
    w.string("name", description.name);
    if (description.step)
    {
        w.integer("step_ns", static_cast<std::int64_t>(description.step->count()));
    }
    else
    {
        w.null("step_ns");
    }
    w.begin_array("networks");
    for (const auto& network : description.networks)
    {
        w.begin_object();
        w.string("name", network.name);
        w.string("kind", to_string(network.kind()));
        w.string("use", to_string(network.use));
        w.begin_array("nodes");
        for (const auto& node : network.nodes)
        {
            w.begin_object();
            w.integer("address", node.address);
            if (node.name)
            {
                w.string("j1939_name", hex64(node.name->value()));
            }
            else
            {
                w.null("j1939_name");
            }
            w.boolean("arbitrary_address_capable", node.arbitrary_address_capable());
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.begin_array("variables");
    for (const auto& variable : description.variables)
    {
        w.begin_object();
        w.string("name", variable.name);
        w.string("type", type_name(variable.type));
        w.string("causality", causality_name(variable.causality));
        w.string("unit", variable.unit);
        w.real("min", variable.min);
        w.real("max", variable.max);
        w.real("step", variable.step);
        w.string("description", variable.description);
        w.begin_array("items");
        for (const auto& item : variable.items)
        {
            w.begin_object();
            w.string("name", item.name);
            w.integer("value", item.value);
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil
