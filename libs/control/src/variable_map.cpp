// SPDX-License-Identifier: MIT
#include "vehicle_sil/control/variable_map.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>
#include <limits>
#include <set>
#include <utility>

namespace vehicle_sil::control {

namespace {

constexpr double kTwoPow63 = 9223372036854775808.0;

/// The shortest text that reads back to `value` (the same on every platform and in every locale).
std::string number_text(double value)
{
    std::array<char, 32> text{};
    const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? std::string(text.data(), result.ptr) : std::string("?");
}

std::string type_text(ValueType type)
{
    switch (type)
    {
    case ValueType::Bool: return "a bool";
    case ValueType::Integer: return "an integer";
    case ValueType::Real: return "a real";
    case ValueType::Enum: return "an enum";
    }
    return "a real";
}

std::string items_text(const std::vector<EnumItem>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += (out.empty() ? "" : ", ") + item.name + " (" + std::to_string(item.value) + ")";
    }
    return out;
}

/// What is wrong with the mapping itself, or "".
std::string form_error(const VariableMapping& m)
{
    if (m.absent)
    {
        return !m.from.empty() || m.linear() || m.map || m.unit ? "absent: true takes no other key: remove the others, or absent"
                                                               : "";
    }
    if (m.from.empty())
    {
        return "it does not name the executable's variable: add from: <the executable's name of it>";
    }
    if (m.linear() && m.map)
    {
        return "scale and offset cannot be combined with map: use one of the two";
    }
    if ((m.scale && (!std::isfinite(*m.scale) || *m.scale == 0.0)) || (m.offset && !std::isfinite(*m.offset)))
    {
        return "scale must be a number other than 0 and offset a number (the value seen is the executable's value * "
               "scale + offset)";
    }
    if (m.map)
    {
        if (m.map->empty())
        {
            return "map is empty: give the names of the values ({ off: 0, on: 1 })";
        }
        for (std::size_t i = 0; i < m.map->size(); ++i)
        {
            for (std::size_t k = 0; k < i; ++k)
            {
                if ((*m.map)[k].value == (*m.map)[i].value || (*m.map)[k].name == (*m.map)[i].name)
                {
                    return "map has " + (*m.map)[k].name + " (" + std::to_string((*m.map)[k].value) + ") and " +
                           (*m.map)[i].name + " (" + std::to_string((*m.map)[i].value) +
                           "): give each value one name and each name one value";
                }
            }
        }
    }
    return "";
}

/// A value of `map` that the variable cannot take, or "".
std::string value_error(const VariableMapping& m, const VariableInfo& info)
{
    for (const auto& item : *m.map)
    {
        const auto v = static_cast<double>(item.value);
        const bool in_range = !(info.min && v < *info.min) && !(info.max && v > *info.max);
        const bool an_item = info.type != ValueType::Enum ||
                             std::any_of(info.items.begin(), info.items.end(),
                                         [&](const EnumItem& e) { return e.value == item.value; });
        if (in_range && an_item)
        {
            continue;
        }
        std::string allowed;
        if (info.type == ValueType::Enum)
        {
            allowed = "its values are " + items_text(info.items);
        }
        else
        {
            allowed = "its range is " + (info.min ? number_text(*info.min) : std::string("any")) + " to " +
                      (info.max ? number_text(*info.max) : std::string("any"));
        }
        return "map gives " + item.name + " the value " + std::to_string(item.value) + ", which the executable's '" +
               m.from + "' cannot take (" + allowed + "): correct the value";
    }
    return "";
}

} // namespace

MappedSchema MappedSchema::make(const std::vector<VariableInfo>& actual, const VariableMap& map)
{
    MappedSchema out;
    const auto problem = [&](std::size_t entry, MapProblemKind kind, const std::string& text) {
        out.problems_.push_back(MapProblem{entry, kind, map[entry].name + ": " + text});
    };
    std::map<std::string, std::size_t, std::less<>> index;
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        index.emplace(actual[i].name, i);
    }
    constexpr auto kNone = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> entry_of(actual.size(), kNone); // variable -> the mapping that applies
    for (std::size_t e = 0; e < map.size(); ++e)
    {
        const VariableMapping& m = map[e];
        if (const std::string error = form_error(m); !error.empty())
        {
            problem(e, MapProblemKind::Form, error);
            continue;
        }
        if (m.absent)
        {
            if (index.count(m.name) > 0)
            {
                problem(e, MapProblemKind::Present,
                        "it says absent: true, but the executable has a variable '" + m.name +
                            "': remove this entry (a variable that is not listed is seen under its own name)");
            }
            continue;
        }
        const auto it = index.find(m.from);
        if (it == index.end())
        {
            problem(e, MapProblemKind::Unknown, "the executable has no variable '" + m.from + "'");
            continue;
        }
        const VariableInfo& info = actual[it->second];
        if (entry_of[it->second] != kNone)
        {
            problem(e, MapProblemKind::Collision,
                    "the executable's '" + m.from + "' is mapped twice (also as " + map[entry_of[it->second]].name +
                        "): keep one of the two");
            continue;
        }
        if (m.linear() && info.type != ValueType::Integer && info.type != ValueType::Real)
        {
            problem(e, MapProblemKind::Type,
                    "scale and offset need a number, and the executable's '" + m.from + "' is " + type_text(info.type) +
                        ": remove them" + (info.type == ValueType::Enum ? " (map gives its values other names)" : ""));
            continue;
        }
        if (m.map && info.type != ValueType::Integer && info.type != ValueType::Enum)
        {
            problem(e, MapProblemKind::Type,
                    "map names the values of an integer or an enum, and the executable's '" + m.from + "' is " +
                        type_text(info.type) + ": remove map");
            continue;
        }
        if (m.map)
        {
            if (const std::string error = value_error(m, info); !error.empty())
            {
                problem(e, MapProblemKind::BadValue, error);
                continue;
            }
        }
        entry_of[it->second] = e;
    }
    // The seen names must be unique. A mapping whose name is taken is left out, which gives its
    // variable back the executable's name: that may take the name of another mapping, so again
    // until nothing changes (mappings only go, so it ends).
    for (bool changed = true; changed;)
    {
        changed = false;
        std::set<std::string, std::less<>> taken;
        for (std::size_t i = 0; i < actual.size(); ++i)
        {
            if (entry_of[i] == kNone)
            {
                taken.insert(actual[i].name);
            }
        }
        for (std::size_t e = 0; e < map.size(); ++e)
        {
            const auto i = static_cast<std::size_t>(std::find(entry_of.begin(), entry_of.end(), e) - entry_of.begin());
            if (i == actual.size() || taken.insert(map[e].name).second)
            {
                continue;
            }
            problem(e, MapProblemKind::Collision,
                    "another variable of the executable is seen under the name '" + map[e].name +
                        "' already: choose another name here, or map that variable to another name too");
            entry_of[i] = kNone;
            changed = true;
            break;
        }
    }
    std::sort(out.problems_.begin(), out.problems_.end(),
              [](const MapProblem& a, const MapProblem& b) { return a.entry < b.entry; });

    out.schema_ = actual;
    out.sources_.resize(actual.size());
    out.rules_.resize(actual.size());
    out.actual_names_.reserve(actual.size());
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        out.actual_names_.push_back(actual[i].name);
        if (entry_of[i] == kNone)
        {
            continue;
        }
        const VariableMapping& m = map[entry_of[i]];
        VariableInfo& info = out.schema_[i];
        Rule& rule = out.rules_[i];
        out.sources_[i] = actual[i].name;
        out.seen_index_.emplace(m.name, i);
        if (m.name != actual[i].name)
        {
            out.actual_index_.emplace(actual[i].name, i);
        }
        info.name = m.name;
        if (m.linear())
        {
            rule.convert = Convert::Linear;
            rule.scale = m.scale.value_or(1.0);
            rule.offset = m.offset.value_or(0.0);
            rule.integer = actual[i].type == ValueType::Integer;
            rule.min = actual[i].min;
            rule.max = actual[i].max;
            const auto seen = [&](const std::optional<double>& v) {
                return v ? std::optional<double>(*v * rule.scale + rule.offset) : std::nullopt;
            };
            info.type = ValueType::Real;
            info.min = seen(rule.scale > 0 ? actual[i].min : actual[i].max);
            info.max = seen(rule.scale > 0 ? actual[i].max : actual[i].min);
            if (info.step)
            {
                info.step = *info.step * std::fabs(rule.scale);
            }
            info.unit.clear();
            out.linear_.push_back(i);
        }
        else if (m.map)
        {
            rule.convert = Convert::Items;
            info.type = ValueType::Enum;
            info.items = *m.map;
        }
        if (m.unit)
        {
            info.unit = *m.unit;
        }
    }
    return out;
}

void MappedSchema::to_seen(Snapshot& sample) const
{
    if (sample.values.size() != schema_.size())
    {
        return;
    }
    for (const std::size_t i : linear_)
    {
        Value& value = sample.values[i];
        const Rule& rule = rules_[i];
        if (const auto* integer = std::get_if<std::int64_t>(&value))
        {
            value = static_cast<double>(*integer) * rule.scale + rule.offset;
        }
        else if (const auto* real = std::get_if<double>(&value))
        {
            value = *real * rule.scale + rule.offset;
        }
    }
}

std::optional<std::string> MappedSchema::to_actual(Write& write) const
{
    const auto it = seen_index_.find(write.name);
    if (it == seen_index_.end())
    {
        return std::nullopt;
    }
    const std::size_t i = it->second;
    const Rule& rule = rules_[i];
    const auto fail = [&](const std::string& why) { return std::optional<std::string>(schema_[i].name + ": " + why); };
    switch (rule.convert)
    {
    case Convert::None:
        break;
    case Convert::Items:
    {
        // Only what the map decides; the agent checks the rest (the type of the value).
        std::optional<std::int64_t> v;
        if (const auto* integer = std::get_if<std::int64_t>(&write.value))
        {
            v = *integer;
        }
        else if (const auto* real = std::get_if<double>(&write.value);
                 real != nullptr && *real == std::trunc(*real) && *real >= -kTwoPow63 && *real < kTwoPow63)
        {
            v = static_cast<std::int64_t>(*real);
        }
        if (v && std::none_of(schema_[i].items.begin(), schema_[i].items.end(),
                              [&](const EnumItem& item) { return item.value == *v; }))
        {
            return fail(std::to_string(*v) + " is not one of its values");
        }
        break;
    }
    case Convert::Linear:
    {
        if (std::holds_alternative<bool>(write.value))
        {
            return fail("needs a number");
        }
        const auto* integer = std::get_if<std::int64_t>(&write.value);
        const double seen = integer != nullptr ? static_cast<double>(*integer) : std::get<double>(write.value);
        if (!std::isfinite(seen))
        {
            return fail("needs a finite number");
        }
        // The range is compared in the executable's values: the seen ends of the range are
        // products, and may be off the exact end by a rounding.
        double actual = (seen - rule.offset) / rule.scale;
        const std::string out_of_range = number_text(seen) + " is out of range";
        if (rule.integer)
        {
            actual = std::nearbyint(actual);
            if (!std::isfinite(actual) || actual < -kTwoPow63 || actual >= kTwoPow63 ||
                (rule.min && actual < std::ceil(*rule.min)) || (rule.max && actual > std::floor(*rule.max)))
            {
                return fail(out_of_range);
            }
            write.value = static_cast<std::int64_t>(actual);
        }
        else
        {
            if (!std::isfinite(actual))
            {
                return fail(out_of_range);
            }
            // A value at an end of the range may come out just beyond it: it is that end.
            const auto slack = [](double bound) { return 1e-9 * std::max(1.0, std::fabs(bound)); };
            if (rule.min && actual < *rule.min)
            {
                if (*rule.min - actual > slack(*rule.min))
                {
                    return fail(out_of_range);
                }
                actual = *rule.min;
            }
            if (rule.max && actual > *rule.max)
            {
                if (actual - *rule.max > slack(*rule.max))
                {
                    return fail(out_of_range);
                }
                actual = *rule.max;
            }
            write.value = actual;
        }
        break;
    }
    }
    write.name = actual_names_[i];
    return std::nullopt;
}

std::string MappedSchema::reason_to_seen(std::string reason) const
{
    const auto colon = reason.find(": ");
    if (colon == std::string::npos)
    {
        return reason;
    }
    const auto it = actual_index_.find(std::string_view(reason).substr(0, colon));
    if (it == actual_index_.end())
    {
        return reason;
    }
    return schema_[it->second].name + " (the executable's " + actual_names_[it->second] + ")" + reason.substr(colon);
}

} // namespace vehicle_sil::control
