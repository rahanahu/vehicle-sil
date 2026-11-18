// SPDX-License-Identifier: MIT
#include "vehicle_sil/ecu/describe.hpp"

#include "vehicle_sil/ecu/describe_json.hpp"
#include "vehicle_sil/json/json_writer.hpp"

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

void write_variables(json::JsonWriter& w, std::string_view key, const std::vector<control::VariableInfo>& variables)
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
        w.boolean("builtin", is_builtin_variable(variable.name));
        w.end_object();
    }
    w.end_array();
}

namespace detail {

bool is_utf8(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size();)
    {
        const std::size_t n = json::utf8_sequence_length(text.substr(i));
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
    const std::string_view name = detail::network_kind_name(kind);
    if (name.empty())
    {
        bad_value("NetworkKind", static_cast<int>(kind));
    }
    return name.data(); // a string literal: ends with '\0'
}

const char* to_string(NetworkUse use)
{
    const std::string_view name = detail::network_use_name(use);
    if (name.empty())
    {
        bad_value("NetworkUse", static_cast<int>(use));
    }
    return name.data(); // a string literal: ends with '\0'
}

std::optional<NetworkKind> network_kind_named(std::string_view name) noexcept
{
    for (const NetworkKind kind : kNetworkKinds)
    {
        if (detail::network_kind_name(kind) == name)
        {
            return kind;
        }
    }
    return std::nullopt;
}

std::optional<NetworkUse> network_use_named(std::string_view name) noexcept
{
    for (const NetworkUse use : kNetworkUses)
    {
        if (detail::network_use_name(use) == name)
        {
            return use;
        }
    }
    return std::nullopt;
}

std::string to_json(const EcuDescription& description)
{
    json::JsonWriter w;
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
                w.string("j1939_name", json::hex64(node.name->value()));
            }
            else
            {
                w.null("j1939_name");
            }
            w.boolean("arbitrary_address_capable", node.arbitrary_address_capable());
            w.end_object();
        }
        w.end_array();
        if (network.mac)
        {
            w.string("mac", network.mac->to_string());
        }
        else
        {
            w.null("mac");
        }
        w.end_object();
    }
    w.end_array();
    write_variables(w, "variables", description.variables);
    if (description.control)
    {
        w.begin_object("control");
        w.string("transport", description.control->transport);
        w.integer("wire_version", description.control->wire_version);
        w.integer("wire_revision", description.control->wire_revision);
        w.end_object();
    }
    else
    {
        w.null("control");
    }
    w.begin_array("features");
    for (const auto& feature : description.features)
    {
        w.string({}, feature);
    }
    w.end_array();
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil
