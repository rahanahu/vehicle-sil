// SPDX-License-Identifier: MIT
#include "vehicle_sil/ecu/j1939_node_argument.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/ecu/executable.hpp"

namespace vehicle_sil::ecu_executable {

namespace {

const std::string kArgument{kJ1939NodeArgument};
const std::string kForm = "<simulation's network name>=<setting>:<value>[,...]";
const std::string kExample = "--j1939-node VehicleCAN=from:0x00,address:0x01,identity_number:2";

std::string in_quotes(std::string_view text) { return "\"" + std::string(text) + "\""; }

std::string hex_bytes(std::string_view text)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text)
    {
        const auto b = static_cast<unsigned char>(c);
        out += out.empty() ? "" : " ";
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

} // namespace

std::string format_j1939_node_argument(const J1939NodeOverride& entry)
{
    return entry.network + "=" + j1939::format_node_override(entry.node);
}

std::string j1939_node_argument_missing()
{
    return kArgument + " needs " + kForm + " after it (e.g. " + kExample + ")";
}

J1939NodeArgument parse_j1939_node_argument(std::string_view text)
{
    const auto failed = [](std::string message) { return J1939NodeArgument{std::nullopt, std::move(message)}; };
    if (!detail::is_utf8(text))
    {
        return failed(kArgument + " needs ASCII or UTF-8 text, not the bytes " + hex_bytes(text) +
                      " (a name in another encoding, e.g. a Windows code page)");
    }
    const std::string quoted = kArgument + " " + in_quotes(text);
    const auto split = text.find('=');
    if (split == std::string_view::npos)
    {
        return failed(kArgument + " needs " + kForm + ", not " + in_quotes(text) + " (e.g. " + kExample +
                      ": the node that the ECU's code puts at 0x00 on VehicleCAN is at 0x01, with identity number 2)");
    }
    const std::string_view network = text.substr(0, split);
    if (network.empty())
    {
        return failed(quoted + " has an empty network name: it needs the simulation's name of a J1939 network before "
                               "the '='");
    }
    if (network.front() == '-')
    {
        return failed(quoted + " has a network name that starts with '-': it needs the simulation's name of a J1939 "
                               "network before the '='");
    }
    const j1939::NodeOverrideText settings = j1939::parse_node_override(text.substr(split + 1));
    if (!settings.entry)
    {
        const std::string list{j1939::kNodeOverrideSettings};
        switch (settings.error)
        {
        case j1939::NodeOverrideTextError::EmptySetting:
            return failed(quoted + " has an empty setting: write <setting>:<value> between the commas (settings: " + list +
                          ")");
        case j1939::NodeOverrideTextError::NoValue:
            return failed(quoted + " has the setting " + in_quotes(settings.setting) + " without a value: write " +
                          settings.setting + ":<value>");
        case j1939::NodeOverrideTextError::UnknownSetting:
            return failed(quoted + " has the unknown setting " + in_quotes(settings.setting) + ": the settings are " +
                          list);
        case j1939::NodeOverrideTextError::Twice:
            return failed(quoted + " sets " + settings.setting + " twice: give each setting once");
        case j1939::NodeOverrideTextError::NotANumber:
            return failed(quoted + " has " + settings.setting + " " + in_quotes(settings.value) +
                          ", which is not a number: write it in decimal or as 0x.. in hexadecimal");
        case j1939::NodeOverrideTextError::OutOfRange:
            break;
        }
        return failed(quoted + " has " + settings.setting + " " + settings.value + ", which is out of its range 0.." +
                      std::to_string(settings.max));
    }
    if (!settings.entry->changes())
    {
        // The launcher gives no such entry: only a hand-written command line has one.
        return failed(quoted + " changes nothing: give at least one of address, identity_number, function_instance, "
                               "ecu_instance, or leave it out");
    }
    return J1939NodeArgument{J1939NodeOverride{std::string(network), *settings.entry}, {}};
}

std::string j1939_node_arguments_error(std::span<const J1939NodeOverride> entries)
{
    for (std::size_t second = 1; second < entries.size(); ++second)
    {
        for (std::size_t first = 0; first < second; ++first)
        {
            const J1939NodeOverride& a = entries[first];
            const J1939NodeOverride& b = entries[second];
            if (a.network != b.network)
            {
                continue;
            }
            const std::string textA = kArgument + " " + format_j1939_node_argument(a);
            const std::string textB = kArgument + " " + format_j1939_node_argument(b);
            if (a.node.from && b.node.from)
            {
                if (*a.node.from != *b.node.from)
                {
                    continue;
                }
                return textA + " and " + textB + " change the same node (at " + j1939::coded_address_text(*a.node.from) +
                       " in the ECU's code): give its settings in one " + kArgument;
            }
            // The one without from is named first, whichever was given first.
            const bool firstHasNone = !a.node.from;
            return (firstHasNone ? textA : textB) + " has no from, so it is for the only node of the ECU on " +
                   a.network + ", and " + (firstHasNone ? textB : textA) +
                   " is for the same network: give every " + kArgument + " of a network a from";
        }
    }
    return {};
}

} // namespace vehicle_sil::ecu_executable
