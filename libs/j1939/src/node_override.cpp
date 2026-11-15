// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/node_override.hpp"

#include <array>
#include <cstdio>

namespace vehicle_sil::j1939 {

namespace {

constexpr std::uint32_t kMaxAddress = kNullAddress; // 254: any free address
constexpr std::uint32_t kMaxIdentityNumber = (std::uint32_t{1} << 21) - 1;
constexpr std::uint32_t kMaxFunctionInstance = (std::uint32_t{1} << 5) - 1;
constexpr std::uint32_t kMaxEcuInstance = (std::uint32_t{1} << 3) - 1;

/// One setting of an entry: its name, its largest value and its value in `entry`, if given.
struct Setting
{
    std::string_view name;
    std::uint32_t max;
    bool is_address;
    std::optional<std::uint32_t> value;
};

template <typename T>
std::optional<std::uint32_t> widened(const std::optional<T>& value)
{
    return value ? std::optional<std::uint32_t>(*value) : std::nullopt;
}

/// In the order of kNodeOverrideSettings.
std::array<Setting, 5> settings_of(const NodeOverride& entry)
{
    return {{
        {"from", kMaxAddress, true, widened(entry.from)},
        {"address", kMaxAddress, true, widened(entry.address)},
        {"identity_number", kMaxIdentityNumber, false, widened(entry.identity_number)},
        {"function_instance", kMaxFunctionInstance, false, widened(entry.function_instance)},
        {"ecu_instance", kMaxEcuInstance, false, widened(entry.ecu_instance)},
    }};
}

void set(NodeOverride& entry, std::size_t setting, std::uint32_t value)
{
    switch (setting)
    {
    case 0:
        entry.from = static_cast<Address>(value);
        break;
    case 1:
        entry.address = static_cast<Address>(value);
        break;
    case 2:
        entry.identity_number = value;
        break;
    case 3:
        entry.function_instance = static_cast<std::uint8_t>(value);
        break;
    default:
        entry.ecu_instance = static_cast<std::uint8_t>(value);
        break;
    }
}

/// The value of decimal or 0x.. hexadecimal digits; empty if `text` is not a number. A number
/// above 32 bits comes back as the largest one (it is out of every range).
std::optional<std::uint32_t> number(std::string_view text)
{
    std::uint32_t base = 10;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty())
    {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text)
    {
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9')
        {
            digit = static_cast<std::uint32_t>(c - '0');
        }
        else if (base == 16 && c >= 'a' && c <= 'f')
        {
            digit = static_cast<std::uint32_t>(c - 'a') + 10;
        }
        else if (base == 16 && c >= 'A' && c <= 'F')
        {
            digit = static_cast<std::uint32_t>(c - 'A') + 10;
        }
        else
        {
            return std::nullopt;
        }
        value = value * base + digit;
        if (value > UINT32_MAX)
        {
            value = UINT32_MAX; // stays there: every further digit is above it again
        }
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

NodeOverrideOutcome apply_node_override(const NodeValues& coded, const NodeOverride& entry) noexcept
{
    const auto refused = [&](NodeOverrideError error) { return NodeOverrideOutcome{coded, error}; };
    // The setters of Name drop the bits above a field's width: the ranges are checked first.
    if ((entry.identity_number && *entry.identity_number > kMaxIdentityNumber) ||
        (entry.function_instance && *entry.function_instance > kMaxFunctionInstance) ||
        (entry.ecu_instance && *entry.ecu_instance > kMaxEcuInstance))
    {
        return refused(NodeOverrideError::FieldOutOfRange);
    }
    if (entry.address && *entry.address > kMaxAddress)
    {
        return refused(NodeOverrideError::AddressOutOfRange);
    }
    if (entry.changes_name() && !coded.name)
    {
        return refused(NodeOverrideError::NameFieldWithoutName);
    }
    if (entry.address && *entry.address == kNullAddress)
    {
        if (!coded.name)
        {
            return refused(NodeOverrideError::AnyAddressWithoutName);
        }
        if (!coded.name->arbitrary_address_capable())
        {
            return refused(NodeOverrideError::AnyAddressNotCapable);
        }
    }
    NodeOverrideOutcome out{coded, std::nullopt};
    if (entry.address)
    {
        out.values.address = *entry.address;
    }
    if (entry.identity_number)
    {
        out.values.name->set_identity_number(*entry.identity_number);
    }
    if (entry.function_instance)
    {
        out.values.name->set_function_instance(*entry.function_instance);
    }
    if (entry.ecu_instance)
    {
        out.values.name->set_ecu_instance(*entry.ecu_instance);
    }
    return out;
}

std::optional<NodeOverrideClash> find_node_override_clash(std::span<const NodeOverride> entries) noexcept
{
    for (std::size_t second = 1; second < entries.size(); ++second)
    {
        for (std::size_t first = 0; first < second; ++first)
        {
            const bool withoutFrom = !entries[first].from || !entries[second].from;
            if (withoutFrom || *entries[first].from == *entries[second].from)
            {
                return NodeOverrideClash{first, second, withoutFrom};
            }
        }
    }
    return std::nullopt;
}

std::vector<NodeOverrideResult> apply_node_overrides(std::span<const NodeValues> coded,
                                                     std::span<const NodeOverride> entries)
{
    std::vector<NodeOverrideResult> out;
    out.reserve(entries.size());
    for (const NodeOverride& entry : entries)
    {
        NodeOverrideResult result;
        if (!entry.changes())
        {
            result.match = NodeOverrideMatch::Skipped;
            out.push_back(result);
            continue;
        }
        // The nodes the entry is for: always by the addresses in the code.
        std::size_t matching = 0;
        bool sameAddress = true;
        for (std::size_t i = 0; i < coded.size(); ++i)
        {
            if (!node_override_matches(entry, coded[i].address))
            {
                continue;
            }
            if (matching == 0)
            {
                result.node = i;
            }
            else if (coded[i].address != coded[result.node].address)
            {
                sameAddress = false;
            }
            ++matching;
        }
        if (matching == 0)
        {
            result.match = coded.empty() ? NodeOverrideMatch::NoNodes : NodeOverrideMatch::NoNodeAtFrom;
        }
        else if (matching > 1)
        {
            result.match = sameAddress ? NodeOverrideMatch::Indistinct : NodeOverrideMatch::Ambiguous;
        }
        else
        {
            const NodeOverrideOutcome outcome = apply_node_override(coded[result.node], entry);
            result.match = outcome.error ? NodeOverrideMatch::CannotApply : NodeOverrideMatch::Applied;
            result.error = outcome.error;
            if (!outcome.error)
            {
                result.values = outcome.values;
            }
        }
        out.push_back(result);
    }
    return out;
}

std::string node_override_address_text(Address address)
{
    if (address == kNullAddress)
    {
        return "254";
    }
    char text[8];
    std::snprintf(text, sizeof(text), "0x%02X", static_cast<unsigned>(address));
    return text;
}

std::string coded_address_text(Address address)
{
    return address == kNullAddress ? "any (254)" : node_override_address_text(address);
}

std::string format_node_override(const NodeOverride& entry)
{
    std::string out;
    for (const Setting& setting : settings_of(entry))
    {
        if (!setting.value)
        {
            continue;
        }
        out += out.empty() ? "" : ",";
        out += setting.name;
        out += ':';
        // An address above 254 is not one: written in decimal, as the message about it says it.
        out += setting.is_address && *setting.value <= kMaxAddress
                   ? node_override_address_text(static_cast<Address>(*setting.value))
                   : std::to_string(*setting.value);
    }
    return out;
}

std::string node_override_range_problem(const NodeOverride& entry)
{
    for (const Setting& setting : settings_of(entry))
    {
        if (setting.value && *setting.value > setting.max)
        {
            return std::string(setting.name) + " " + std::to_string(*setting.value) + ", which is out of its range 0.." +
                   std::to_string(setting.max);
        }
    }
    return {};
}

NodeOverrideText parse_node_override(std::string_view text)
{
    NodeOverrideText out;
    const auto failed = [&](NodeOverrideTextError error, std::string_view setting, std::string_view value = {},
                            std::uint32_t max = 0) {
        out.error = error;
        out.setting = setting;
        out.value = value;
        out.max = max;
        return out;
    };
    NodeOverride entry;
    const auto names = settings_of(entry);
    std::array<bool, 5> given{};
    for (;;)
    {
        const auto comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        if (item.empty())
        {
            return failed(NodeOverrideTextError::EmptySetting, {});
        }
        const auto colon = item.find(':');
        const std::string_view name = item.substr(0, colon);
        std::size_t index = 0;
        while (index < names.size() && names[index].name != name)
        {
            ++index;
        }
        // An unknown name first: "adress" without a value is a wrong name, not a missing value.
        if (index == names.size())
        {
            return failed(NodeOverrideTextError::UnknownSetting, name);
        }
        if (colon == std::string_view::npos)
        {
            return failed(NodeOverrideTextError::NoValue, name);
        }
        if (given[index])
        {
            return failed(NodeOverrideTextError::Twice, name);
        }
        const std::string_view value = item.substr(colon + 1);
        const auto parsed = number(value);
        if (!parsed)
        {
            return failed(NodeOverrideTextError::NotANumber, name, value);
        }
        if (*parsed > names[index].max)
        {
            return failed(NodeOverrideTextError::OutOfRange, name, value, names[index].max);
        }
        given[index] = true;
        set(entry, index, *parsed);
        if (comma == std::string_view::npos)
        {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    out.entry = entry;
    return out;
}

} // namespace vehicle_sil::j1939
