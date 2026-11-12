// SPDX-License-Identifier: MIT
#include "vehicle_sil/ecu/describe.hpp"

#include "vehicle_sil/ecu/detail/json_writer.hpp"

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

} // namespace

namespace detail {

const char* value_type_name(control::ValueType type)
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

void write_variables(JsonWriter& w, std::string_view key, const std::vector<control::VariableInfo>& variables)
{
    w.begin_array(key);
    for (const auto& variable : variables)
    {
        w.begin_object();
        w.string("name", variable.name);
        w.string("type", value_type_name(variable.type));
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
}

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
    detail::JsonWriter w;
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
                w.string("j1939_name", detail::hex64(node.name->value()));
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
    detail::write_variables(w, "variables", description.variables);
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil
