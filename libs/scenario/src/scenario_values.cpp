// SPDX-License-Identifier: MIT
//
// The scenario's reader (scenario_reader.hpp): variables and their values, hexadecimal included.
#include "scenario_reader.hpp"

namespace vehicle_sil::scenario::reader {

ScenarioShapeText::Value Reader::shape_value(ryml::id_type node, std::size_t variable) const
{
    std::int64_t whole = 0;
    double number = 0;
    if (plain_integer(node, whole))
    {
        return whole;
    }
    if (number_of(node, variable, number))
    {
        return number;
    }
    const std::string text = text_of(node);
    if (!quoted(node) && (text == "true" || text == "false"))
    {
        return text == "true";
    }
    return text;
}

Reader::Hex Reader::hex_of(ryml::id_type node) const
{
    if (param_at(node) == nullptr && (!is_scalar(node) || is_null(node)))
    {
        return {};
    }
    return quoted(node) ? Hex{} : read_hex(text_of(node));
}

bool Reader::whole_number(ryml::id_type node, std::int64_t& value) const
{
    if (plain_integer(node, value))
    {
        return true;
    }
    const Hex hex = hex_of(node);
    if (hex.kind != Hex::Kind::Read)
    {
        return false;
    }
    value = static_cast<std::int64_t>(hex.magnitude);
    return true;
}

bool Reader::number_of(ryml::id_type node, std::size_t variable, double& value) const
{
    std::int64_t i = 0;
    if (plain_number(node, value))
    {
        return true;
    }
    if (scenario_.variables[variable].type != control::ValueType::Integer || !whole_number(node, i))
    {
        return false;
    }
    value = static_cast<double>(i);
    return true;
}

std::string Reader::hex_hint(ryml::id_type node, control::ValueType type) const
{
    const Hex hex = hex_of(node);
    const std::string decimal = std::to_string(hex.magnitude);
    switch (hex.kind)
    {
    case Hex::Kind::None: return {};
    case Hex::Kind::Negative: return ": write a negative number in decimal (-" + decimal + ")";
    case Hex::Kind::TooLarge: return ": it is more than an integer holds (at most 0x7FFFFFFFFFFFFFFF)";
    case Hex::Kind::Read:
        return type == control::ValueType::Real ? ": hexadecimal (0x...) is for integer and enum variables only; write " + decimal
                                                : std::string{};
    }
    return {};
}

std::string Reader::written(ryml::id_type node, double value) const
{
    return hex_of(node).kind == Hex::Kind::Read ? text_of(node) + " (" + number_text(value) + ")" : number_text(value);
}

std::optional<std::size_t> Reader::variable(ryml::id_type node)
{
    const std::string text = config::detail::to_string(tree().key(node));
    const std::size_t line = doc().line(node);
    if (const auto known = variable_index_.find(text); known != variable_index_.end())
    {
        return known->second;
    }
    if (absent_variables_.count(text) > 0)
    {
        absent_.push_back(text);
        return std::nullopt;
    }
    if (bad_variables_.count(text) > 0)
    {
        return std::nullopt; // said once
    }
    bad_variables_.emplace(text, line);
    const auto dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == text.size())
    {
        error("scenario-variable", line,
              "'" + text + "' is not a variable: write <participant>.<variable>, e.g. " + example_variable());
        return std::nullopt;
    }
    const std::string participant = text.substr(0, dot);
    const std::string name = text.substr(dot + 1);
    const auto p = std::find_if(catalog_.begin(), catalog_.end(),
                                [&participant](const CatalogParticipant& c) { return c.name == participant; });
    if (p == catalog_.end())
    {
        std::vector<std::string> names;
        for (const auto& c : catalog_)
        {
            names.push_back(c.name);
        }
        const std::string guess = config::detail::closest(participant, names);
        error("scenario-variable", line,
              "'" + text + "': the configuration has no participant '" + participant + "' (the name ends at the first '.'): " +
                  (guess.empty() ? "its participants are " + (names.empty() ? std::string("none") : config::detail::join(names))
                                 : "did you mean '" + guess + "." + name + "'?"));
        return std::nullopt;
    }
    if (p->not_in_variant)
    {
        // The variant leaves the participant out: as for an absent variable, the step is
        // skipped (read_step()). What the variable would be is not known, so any name passes.
        bad_variables_.erase(text);
        absent_variables_.emplace(text, participant);
        left_out_.insert(participant);
        absent_.push_back(text);
        return std::nullopt;
    }
    if (!p->unusable.empty())
    {
        error("scenario-variable", line, "'" + text + "': participant '" + participant + "' " + p->unusable, participant);
        return std::nullopt;
    }
    const auto v = std::find_if(p->variables.begin(), p->variables.end(),
                                [&name](const control::VariableInfo& i) { return i.name == name; });
    if (v == p->variables.end() && std::find(p->absent.begin(), p->absent.end(), name) != p->absent.end())
    {
        // Said to be missing on purpose: no error, the step is skipped (read_step()).
        bad_variables_.erase(text);
        absent_variables_.emplace(text, participant);
        absent_.push_back(text);
        return std::nullopt;
    }
    if (v == p->variables.end())
    {
        std::vector<std::string> names;
        for (const auto& i : p->variables)
        {
            names.push_back(i.name);
        }
        const std::string guess = config::detail::closest(name, names);
        error("scenario-variable", line,
              "'" + text + "': participant '" + participant + "' has no variable '" + name + "': " +
                  (guess.empty() ? (names.empty() ? std::string("it declares none")
                                                  : "its variables are " + config::detail::join(names))
                                 : "did you mean '" + participant + "." + guess + "'?") +
                  " (vehicle_sil_launch check --json lists the variables)",
              participant);
        return std::nullopt;
    }
    auto sp = std::find_if(scenario_.participants.begin(), scenario_.participants.end(),
                           [&participant](const ScenarioParticipant& s) { return s.name == participant; });
    if (sp == scenario_.participants.end())
    {
        scenario_.participants.push_back(ScenarioParticipant{participant, p->step});
        sp = scenario_.participants.end() - 1;
    }
    ScenarioVariable out;
    out.participant = static_cast<std::size_t>(sp - scenario_.participants.begin());
    out.name = name;
    out.type = v->type;
    out.items = v->items;
    out.text = text;
    out.min = v->min;
    out.max = v->max;
    scenario_.variables.push_back(std::move(out));
    infos_.push_back(&*v);
    maps_.push_back(p->map.get());
    variable_index_.emplace(text, scenario_.variables.size() - 1);
    return scenario_.variables.size() - 1;
}

std::string Reader::example_variable() const
{
    for (const auto& c : catalog_)
    {
        if (c.unusable.empty() && !c.variables.empty())
        {
            return c.name + "." + c.variables.front().name;
        }
    }
    return "Engine.engine.speed";
}

std::optional<control::Value> Reader::value_of(ryml::id_type node, std::size_t variable, const std::string& what, bool write)
{
    const ScenarioVariable& v = scenario_.variables[variable];
    const control::VariableInfo& info = *info_of(variable);
    const std::size_t line = doc().line(node);
    const std::string pre = what + " of " + v.text + " (" + type_name(v.type) + ")";
    if (!is_scalar(node) || is_null(node))
    {
        error("scenario-value", line, pre + " must be a value, not " + shown_value(node));
        return std::nullopt;
    }
    const std::string text = text_of(node);
    const bool quoted = this->quoted(node);
    std::optional<control::Value> out;
    switch (v.type)
    {
    case control::ValueType::Bool:
        if (!quoted && (text == "true" || text == "false"))
        {
            out = control::Value{text == "true"};
        }
        else
        {
            error("scenario-value", line, pre + " must be true or false, not " + shown_value(node));
        }
        break;
    case control::ValueType::Integer:
    {
        std::int64_t i = 0;
        double d = 0;
        if (whole_number(node, i))
        {
            out = control::Value{i};
        }
        else if (!write && plain_number(node, d))
        {
            out = control::Value{d};
        }
        else
        {
            error("scenario-value", line,
                  pre + " must be a whole number, not " + shown_value(node) +
                      (quoted ? ": write the number without quotes" : hex_hint(node, v.type)));
        }
        break;
    }
    case control::ValueType::Real:
    {
        double d = 0;
        if (plain_number(node, d))
        {
            out = control::Value{d};
        }
        else
        {
            error("scenario-value", line,
                  pre + " must be a number, not " + shown_value(node) +
                      (quoted ? ": write the number without quotes" : hex_hint(node, v.type)));
        }
        break;
    }
    case control::ValueType::Enum:
    {
        std::int64_t i = 0;
        const bool number = whole_number(node, i);
        const auto item = std::find_if(info.items.begin(), info.items.end(), [&](const control::EnumItem& e) {
            return number ? e.value == i : e.name == text;
        });
        if (item != info.items.end())
        {
            out = control::Value{item->value};
        }
        else
        {
            std::vector<std::string> names;
            for (const auto& e : info.items)
            {
                names.push_back(e.name);
            }
            const std::string hint = hex_hint(node, v.type);
            const std::string guess = number || !hint.empty() ? std::string{} : config::detail::closest(text, names);
            error("scenario-value", line,
                  pre + " must be one of its items, by name or by value, not " + shown_value(node) +
                      (hint.empty() ? std::string(": ") : hint + "; ") +
                      (guess.empty() ? std::string{} : "did you mean '" + guess + "'? ") + "the items are " +
                      items_text(info.items));
        }
        break;
    }
    }
    if (out && write && v.type != control::ValueType::Bool && v.type != control::ValueType::Enum &&
        !in_range(as_number(*out), variable))
    {
        error("scenario-value", line, pre + " is " + (hex_of(node).kind == Hex::Kind::Read ? written(node, as_number(*out)) : text) +
                                          ", outside its range (" + range_text(info) + ")");
        return std::nullopt;
    }
    if (out && write && (v.type == control::ValueType::Integer || v.type == control::ValueType::Real))
    {
        note_base(variable, as_number(*out), line, true);
    }
    return out;
}

bool Reader::in_range(double value, std::size_t variable) const
{
    const control::VariableInfo& info = *info_of(variable);
    if (maps_[variable] != nullptr)
    {
        if (const auto decided = maps_[variable]->in_range(info.name, value))
        {
            return *decided;
        }
    }
    return !(info.min && value < *info.min) && !(info.max && value > *info.max);
}

std::string Reader::range_text(const control::VariableInfo& info)
{
    return (info.min ? "from " + number_text(*info.min) : std::string("up")) + (info.max ? " to " + number_text(*info.max) : "") +
           (info.unit.empty() ? "" : " " + info.unit);
}

bool Reader::writable(std::size_t variable, std::size_t line, const std::optional<std::int64_t>& time, const char* over_time)
{
    const ScenarioVariable& v = scenario_.variables[variable];
    const control::VariableInfo& info = *info_of(variable);
    if (info.causality == control::Causality::Output)
    {
        error("scenario-not-writable", line,
              v.text + " is an output of its ECU: it can be read (expect, wait_until), not written; write one of the "
                       "ECU's inputs instead");
        return false;
    }
    if (info.causality == control::Causality::Parameter && (over_time != nullptr || !time || *time != 0))
    {
        error("scenario-not-writable", line,
              v.text + " is a parameter: it can be written only before the first step, that is with set in a step at "
                       "0 (at_ms: 0, or the first steps without a time)" +
                  (over_time != nullptr ? std::string(", not with ") + over_time : time ? ", not at " + std::to_string(*time) + " ms" : std::string(", not after a step that waits")));
        return false;
    }
    return true;
}

} // namespace vehicle_sil::scenario::reader
