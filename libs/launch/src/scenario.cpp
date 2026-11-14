// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/scenario.hpp"

#include "vehicle_sil/launch/check.hpp"
#include "scenario_csv.hpp"
#include "scenario_shapes.hpp"
#include "yaml_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <system_error>
#include <utility>

namespace vehicle_sil::launch {

namespace {

// kTopKeys is what the message of an unknown key shows; the reader takes each key in a branch of its own. A key with
// a new branch must be added here too, or the JSON Schema (schema/) and its test (tests/schema), which compare with
// the list in the message, pass without the key. (The keys of a step are taken from kStepKeys itself.)

const std::vector<std::string> kTopKeys = {"vehicle_sil_scenario", "name", "only", "params", "steps"};
const std::vector<std::string> kStepKeys = {"at_ms",      "after_ms", "set",       "ramp",        "wave",        "wait_until",
                                            "timeout_ms", "expect",   "within_ms", "hold_for_ms", "undersampled"};
const std::vector<std::string> kActionKeys = {"set", "ramp", "wave", "wait_until", "expect"};
const std::vector<std::string> kCompareKeys = {"eq", "ne", "ge", "le", "gt", "lt", "near", "tolerance"};
const std::vector<std::string> kRampKeys = {"to", "over_ms", "from"};
// A wave: the shapes (one per variable) and the keys beside the shape.
const std::vector<std::string> kWaveShapes = {"sine", "triangle", "sawtooth", "square", "pulse",
                                              "noise", "chirp", "lag", "chatter", "points"};
const std::vector<std::string> kWaveKeys = {"sine",  "triangle", "sawtooth", "square", "pulse",  "noise",      "chirp",
                                            "lag",   "chatter",  "points",   "around", "for_ms", "interpolate"};
// The keys of points from a CSV file (points: { file: ..., value: ... }).
const std::vector<std::string> kPointsFileKeys = {"file", "value", "time", "time_unit"};
constexpr const char* kPointsFileExample = "points: { file: traces/drive.csv, value: throttle }";
// How a wave that an ECU cannot see as written is run anyway (in every such message).
constexpr const char* kAsItIs = "; to run it as it is without this message, add undersampled: ok to the step";
/// A smooth shape with fewer steps of its ECU per period is coarse (a note).
constexpr std::int64_t kSmoothSteps = 8;
/// Two values of a shape (as a share of its amplitude) that are this close are the same value.
constexpr double kSameValue = 1e-9;
constexpr std::int64_t kMaxBounces = 1'000'000;

constexpr const char* kDocs = "docs/scenario.md";

using detail::join;

std::string number_text(double value)
{
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

const char* type_name(control::ValueType type)
{
    switch (type)
    {
    case control::ValueType::Bool: return "bool";
    case control::ValueType::Integer: return "integer";
    case control::ValueType::Real: return "real";
    case control::ValueType::Enum: return "enum";
    }
    return "real";
}

const char* compare_name(Compare op)
{
    switch (op)
    {
    case Compare::Eq: return "eq";
    case Compare::Ne: return "ne";
    case Compare::Ge: return "ge";
    case Compare::Le: return "le";
    case Compare::Gt: return "gt";
    case Compare::Lt: return "lt";
    case Compare::Near: return "near";
    }
    return "eq";
}

double as_number(const control::Value& value)
{
    if (const auto* b = std::get_if<bool>(&value))
    {
        return *b ? 1.0 : 0.0;
    }
    if (const auto* i = std::get_if<std::int64_t>(&value))
    {
        return static_cast<double>(*i);
    }
    return std::get<double>(value);
}

std::string items_text(const std::vector<control::EnumItem>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += (out.empty() ? "" : ", ") + item.name + " (" + std::to_string(item.value) + ")";
    }
    return out;
}

class Reader : detail::YamlReader
{
public:
    Reader(const detail::YamlDocument& doc, const ScenarioCatalog& catalog, const ScenarioContext& context, ScenarioRead& out) :
      YamlReader(doc, out.findings), catalog_(catalog), context_(context), out_(out)
    {
    }

    void read()
    {
        const ryml::id_type root = doc_.root();
        if (root == ryml::NONE || !tree_.is_map(root))
        {
            error("not-a-scenario", root == ryml::NONE ? 0 : doc_.line(root),
                  std::string("the file is not a scenario: it must be a mapping with vehicle_sil_scenario: 1 and steps (") +
                      kDocs + ")");
            return;
        }
        std::vector<std::size_t> refs;
        find_references(root, refs);
        if (!refs.empty())
        {
            for (const std::size_t line : refs)
            {
                error("unsupported-yaml", line,
                      "anchors and aliases (&name, *name, <<) are not read: write the values out (a value that "
                      "starts with * or & must be in quotes)");
            }
            return;
        }
        if (const ryml::id_type second = doc_.second_document(); second != ryml::NONE)
        {
            error("unsupported-yaml", doc_.line(second),
                  "a second document (after ---): a file has one scenario; remove the others or put them in files of "
                  "their own");
        }
        bool hasVersion = false;
        ryml::id_type paramsNode = ryml::NONE;
        ryml::id_type onlyNode = ryml::NONE;
        ryml::id_type stepsNode = ryml::NONE;
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(root); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, key, child))
            {
                continue;
            }
            if (key == "params")
            {
                paramsNode = child;
            }
            else if (key == "only")
            {
                onlyNode = child;
            }
            else if (key == "vehicle_sil_scenario")
            {
                hasVersion = true;
                std::int64_t version = 0;
                if (!plain_integer(child, version))
                {
                    error("format-version", doc_.line(child),
                          "vehicle_sil_scenario must be the format version " + std::to_string(kScenarioFormat) + ", not " +
                              shown_value(child) + ": write vehicle_sil_scenario: " + std::to_string(kScenarioFormat));
                }
                else if (version != kScenarioFormat)
                {
                    out_.findings.clear();
                    error("format-version", doc_.line(child),
                          "vehicle_sil_scenario is " + std::to_string(version) + ", but this vehicle_sil_launch reads format " +
                              std::to_string(kScenarioFormat) + " only: use the vehicle-sil that the file was written for");
                    return;
                }
            }
            else if (key == "name")
            {
                if (!is_scalar(child) || is_null(child))
                {
                    error("value", doc_.line(child), "name must be a text (what the scenario is about), not " + shown_value(child));
                }
                else
                {
                    scenario_.name = detail::to_string(tree_.val(child));
                    out_.name = scenario_.name;
                }
            }
            else if (key == "steps")
            {
                stepsNode = child;
            }
            else
            {
                unknown_key(child, key, "", kTopKeys);
            }
        }
        // The steps last, wherever the file has them: they use the parameters, and are read only
        // for a variant the scenario is for.
        take_params(paramsNode);
        if (onlyNode != ryml::NONE)
        {
            read_only(onlyNode);
        }
        const bool hasSteps = stepsNode != ryml::NONE;
        if (hasSteps)
        {
            if (!out_.not_for_variant && resolve_params(stepsNode))
            {
                read_steps(stepsNode);
                check_given_params();
                check_something_is_checked();
            }
            else if (tree_.is_seq(stepsNode))
            {
                out_.steps = static_cast<std::size_t>(tree_.num_children(stepsNode));
            }
        }
        for (const Known& known : params_)
        {
            if (known.declared || known.used)
            {
                out_.params.push_back(known.value);
            }
        }
        if (!hasVersion)
        {
            error("missing-key", 0,
                  "the file has no vehicle_sil_scenario (the format version): add vehicle_sil_scenario: " +
                      std::to_string(kScenarioFormat));
        }
        if (!hasSteps)
        {
            error("missing-key", 0,
                  "the file has no steps: add steps, a list of what to do in order (set, ramp, wave, wait_until, expect)");
        }
        if (std::none_of(out_.findings.begin(), out_.findings.end(), [](const Finding& f) { return f.severity == Severity::Error; }))
        {
            out_.scenario = std::move(scenario_);
        }
    }

private:
    // --- parameters ---

    /// A parameter that has a value: from the scenario's "params" (`declared`) or from outside.
    struct Known
    {
        ParamValue value;
        bool declared = false;
        bool used = false; ///< a step has "${name}"
    };

    /// The scenario's defaults (`node`, or ryml::NONE), then the context's over them.
    void take_params(ryml::id_type node)
    {
        if (node != ryml::NONE)
        {
            for (ParamValue& param : read_params(node, "", ParamSource::Scenario))
            {
                params_.push_back(Known{std::move(param), true, false});
            }
        }
        for (const ParamValue& param : context_.params)
        {
            const auto same = std::find_if(params_.begin(), params_.end(), [&param](const Known& k) { return k.value.name == param.name; });
            if (same == params_.end())
            {
                params_.push_back(Known{param, false, false});
            }
            else
            {
                same->value = param;
            }
        }
    }

    /// Finds every "${name}" under `node` (the steps): as a whole value it stands for the
    /// parameter's value from here on (param_at()); anywhere else, or without a value, it is an
    /// error. false if there was one.
    bool resolve_params(ryml::id_type node)
    {
        bool ok = true;
        if (tree_.has_key(node))
        {
            const std::string key = detail::to_string(tree_.key(node));
            if (key.find("${") != std::string::npos)
            {
                error("scenario-param", doc_.line(node),
                      "'" + key + "': a parameter (${...}) stands for a value, not for a key or for a variable's name: write "
                                  "the name out");
                ok = false;
            }
        }
        if (is_scalar(node))
        {
            const std::string text = is_null(node) ? std::string{} : detail::to_string(tree_.val(node));
            if (text.find("${") == std::string::npos)
            {
                return ok;
            }
            const std::string name = text.size() > 3 && text.rfind("${", 0) == 0 && text.back() == '}' ? text.substr(2, text.size() - 3) : std::string{};
            if (!is_param_name(name))
            {
                error("scenario-param", doc_.line(node),
                      "'" + text + "': a parameter is written ${name} (letters, digits and _) and is the whole value: it "
                                   "cannot be a part of a text, and there are no expressions; work the value out and give it "
                                   "as a parameter of its own");
                return false;
            }
            const auto known = std::find_if(params_.begin(), params_.end(), [&name](const Known& k) { return k.value.name == name; });
            if (known == params_.end())
            {
                if (undefined_.insert(name).second) // said once
                {
                    std::vector<std::string> names;
                    for (const Known& k : params_)
                    {
                        names.push_back(k.value.name);
                    }
                    const std::string guess = detail::closest(name, names);
                    error("scenario-param", doc_.line(node),
                          "${" + name + "} has no value" + (guess.empty() ? "" : " (did you mean ${" + guess + "}?)") +
                              ": give it a default in the scenario (params: { " + name + ": <value> }), or a value in the "
                              "configuration's params, in a variant's params or with --param " + name + "=<value>");
                }
                return false;
            }
            known->used = true;
            substituted_.emplace(node, static_cast<std::size_t>(known - params_.begin()));
            return ok;
        }
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            ok = resolve_params(child) && ok;
        }
        return ok;
    }

    /// A --param that the scenario neither declares nor uses changes nothing: an error.
    void check_given_params()
    {
        if (context_.params_for_many)
        {
            return;
        }
        std::vector<std::string> names;
        for (const Known& k : params_)
        {
            if (k.declared || k.used)
            {
                names.push_back(k.value.name);
            }
        }
        for (const Known& k : params_)
        {
            if (k.value.source == ParamSource::CommandLine && !k.declared && !k.used)
            {
                const std::string guess = detail::closest(k.value.name, names);
                // The command line's mistake, not the file's: a usage error like --disable <no such name>.
                out_.usage_errors.push_back(Finding{
                    Severity::Error, "usage",
                      "--param " + k.value.name + "=" + k.value.text + ": the scenario has no parameter " + k.value.name +
                          (guess.empty() ? "" : " (did you mean " + guess + "?)") + ": " +
                          (names.empty() ? std::string("it uses none (a step uses one as ${name})")
                                         : "its parameters are " + join(names)) +
                          "; remove the option or correct the name",
                    0, "", ""});
            }
        }
    }

    /// A scenario whose steps that check (expect, wait_until) are all skipped passes whatever the
    /// ECUs do: a warning, so that --strict does not take it for a pass. One that never had such
    /// a step (only set and ramp) is what its author wrote, and is left alone.
    void check_something_is_checked()
    {
        if (checks_.empty() || skipped_checks_.size() != checks_.size())
        {
            return;
        }
        std::vector<std::string> lines;
        for (const std::size_t line : skipped_checks_)
        {
            lines.push_back(std::to_string(line));
        }
        std::vector<std::string> others;
        for (const std::string& name : context_.variants)
        {
            if (name != context_.variant)
            {
                others.push_back(name);
            }
        }
        const bool one = lines.size() == 1;
        const std::string where = context_.variant.empty() ? "in this configuration" : "in the variant " + context_.variant;
        // An expect beside a ramp or a wave that is not checked: the step runs, only its check is gone.
        const std::size_t unchecked = unchecked_lines_.size();
        const std::string what =
            unchecked == 0 ? (one ? "its only step that checks (expect or wait_until, line " + lines.front() + ") is skipped"
                                  : "all its " + std::to_string(lines.size()) + " steps that check (expect or wait_until, lines " +
                                        join(lines) + ") are skipped")
            : one ? "its only check (an expect beside a ramp or a wave, line " + lines.front() + ") is not checked"
                  : "all its " + std::to_string(lines.size()) + " checks (expect, wait_until, or an expect beside a ramp or a wave; lines " +
                        join(lines) + ") are " + (unchecked == lines.size() ? "not checked" : "skipped or not checked");
        add(Severity::Warning, "scenario-nothing-checked", 0,
            "the scenario checks nothing " + where + ": " + what + ", so it passes whatever the ECUs do (the notes say why " +
                (unchecked == 0 ? "each is skipped). " : "for each). ") +
                (context_.variant.empty()
                     ? std::string("If the configuration has what the steps look at under other names, map them in the participant's variables "
                                   "(docs/simulation-config.md); else do not run this scenario with it")
                     : "If the scenario is not for this variant, list the variants it is for in the scenario (only: [" +
                           (others.empty() ? std::string("<variant>") : join(others)) +
                           "]); if it is, map what the steps look at to what the variant has (variables in the variant, "
                           "docs/simulation-config.md)"));
    }

    /// The parameter whose value stands at `node`, or null.
    const ParamValue* param_at(ryml::id_type node) const
    {
        const auto it = substituted_.find(node);
        return it == substituted_.end() ? nullptr : &params_[it->second].value;
    }

    // What the steps' values are read with: a "${name}" reads as its parameter's value.
    std::string text_of(ryml::id_type node) const
    {
        const ParamValue* param = param_at(node);
        return param != nullptr ? param->text : detail::to_string(tree_.val(node));
    }
    bool quoted(ryml::id_type node) const
    {
        const ParamValue* param = param_at(node);
        return param != nullptr ? param->quoted : is_scalar(node) && tree_.is_val_quoted(node);
    }
    bool plain_integer(ryml::id_type node, std::int64_t& value) const
    {
        const ParamValue* param = param_at(node);
        return param != nullptr ? !param->quoted && read_integer(param->text, value) : YamlReader::plain_integer(node, value);
    }
    bool plain_number(ryml::id_type node, double& value) const
    {
        const ParamValue* param = param_at(node);
        return param != nullptr ? !param->quoted && read_number(param->text, value) : YamlReader::plain_number(node, value);
    }
    std::string shown_value(ryml::id_type node) const
    {
        const ParamValue* param = param_at(node);
        if (param == nullptr)
        {
            return YamlReader::shown_value(node);
        }
        return (param->quoted ? "the text \"" + param->text + "\"" : "'" + param->text + "'") + " (the parameter " + param->name +
               ", from " + param_source_text(param->source) + ")";
    }

    // --- hexadecimal: the one place a scenario reads it (values of integer and enum variables) ---

    /// The unquoted value at `node` (or its parameter's) as hexadecimal; None if quoted.
    Hex hex_of(ryml::id_type node) const
    {
        if (param_at(node) == nullptr && (!is_scalar(node) || is_null(node)))
        {
            return {};
        }
        return quoted(node) ? Hex{} : read_hex(text_of(node));
    }
    /// A whole number written for a value of an integer or an enum variable: decimal, or
    /// hexadecimal (0x1F, 0X1F) without a sign and without quotes.
    bool whole_number(ryml::id_type node, std::int64_t& value) const
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
    /// A number of the range of the variable, as a ramp or a wave writes it: decimal, or for an
    /// integer variable whole_number().
    bool number_of(ryml::id_type node, std::size_t variable, double& value) const
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
    /// What a message adds when the value at `node` is hexadecimal that the variable of `type`
    /// cannot take: "" otherwise (also for a bool, whose message says true or false).
    std::string hex_hint(ryml::id_type node, control::ValueType type) const
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
    /// A value at `node` as a message shows it: the number, and for hexadecimal also as written
    /// ("0x65 (101)"), as the range beside it is decimal.
    std::string written(ryml::id_type node, double value) const
    {
        return hex_of(node).kind == Hex::Kind::Read ? text_of(node) + " (" + number_text(value) + ")" : number_text(value);
    }

    // --- the variants the scenario is for ---

    void read_only(ryml::id_type node)
    {
        const std::size_t line = doc_.line(node);
        if (!tree_.is_seq(node) || !tree_.has_children(node))
        {
            error("value", line,
                  "only must be a list of the variants the scenario is for (only: [small, medium]), not " +
                      (tree_.is_seq(node) ? std::string("an empty list") : shown_value(node)) +
                      "; remove it for a scenario for every variant");
            return;
        }
        std::vector<std::string> names;
        for (auto item = tree_.first_child(node); item != ryml::NONE; item = tree_.next_sibling(item))
        {
            if (!is_scalar(item) || is_null(item) || tree_.val(item).empty())
            {
                error("value", doc_.line(item), "only must list variants by name (only: [small, medium]), not " + shown_value(item));
                return;
            }
            const std::string name = detail::to_string(tree_.val(item));
            if (std::find(names.begin(), names.end(), name) == names.end())
            {
                names.push_back(name);
            }
        }
        out_.only = names;
        if (context_.variants.empty())
        {
            error("scenario-variant", line,
                  "only names " + join(names) + ", but the configuration defines no variants: remove only, or add variants "
                  "to the configuration (docs/simulation-config.md)");
            return;
        }
        for (const std::string& name : names)
        {
            if (std::find(context_.variants.begin(), context_.variants.end(), name) == context_.variants.end())
            {
                const std::string guess = detail::closest(name, context_.variants);
                error("scenario-variant", line,
                      "only names the variant '" + name + "', which the configuration does not define" +
                          (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; its variants are " +
                          join(context_.variants));
            }
        }
        if (!context_.variant.empty() && std::find(names.begin(), names.end(), context_.variant) == names.end())
        {
            std::vector<std::string> options;
            for (const std::string& name : names)
            {
                options.push_back("--variant " + name);
            }
            error("scenario-variant", line,
                  "the scenario is only for the variant" + std::string(names.size() == 1 ? " " : "s ") + join(names) +
                      " (its only), not for " + context_.variant + ": run it with " + detail::or_list(options) + ", or add " +
                      context_.variant + " to only");
            out_.not_for_variant = true;
        }
    }

    /// A whole number of milliseconds from `least` to kMaxMillis, or empty (and an error).
    std::optional<std::int64_t> millis(ryml::id_type node, const std::string& key, std::int64_t least, const std::string& meaning)
    {
        std::int64_t value = 0;
        if (!plain_integer(node, value))
        {
            error("value", doc_.line(node),
                  key + " must be a whole number (" + meaning + "), not " + shown_value(node) +
                      (quoted(node) ? ": write the number without quotes" : ""));
            return std::nullopt;
        }
        if (value < least || value > kMaxMillis)
        {
            error("value", doc_.line(node),
                  key + " is " + std::to_string(value) + ", but it must be from " + std::to_string(least) + " to " +
                      std::to_string(kMaxMillis) + " (" + meaning + ")");
            return std::nullopt;
        }
        return value;
    }

    // --- variables ---

    const control::VariableInfo* info_of(std::size_t variable) const { return infos_[variable]; }

    /// The index of "<participant>.<name>" in the scenario's variables, or empty (and an error).
    std::optional<std::size_t> variable(ryml::id_type node)
    {
        const std::string text = detail::to_string(tree_.key(node));
        const std::size_t line = doc_.line(node);
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
            const std::string guess = detail::closest(participant, names);
            error("scenario-variable", line,
                  "'" + text + "': the configuration has no participant '" + participant + "' (the name ends at the first '.'): " +
                      (guess.empty() ? "its participants are " + (names.empty() ? std::string("none") : join(names))
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
            const std::string guess = detail::closest(name, names);
            error("scenario-variable", line,
                  "'" + text + "': participant '" + participant + "' has no variable '" + name + "': " +
                      (guess.empty() ? (names.empty() ? std::string("it declares none")
                                                      : "its variables are " + join(names))
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
        scenario_.variables.push_back(std::move(out));
        infos_.push_back(&*v);
        maps_.push_back(p->map.get());
        variable_index_.emplace(text, scenario_.variables.size() - 1);
        return scenario_.variables.size() - 1;
    }

    std::string example_variable() const
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

    /// The value written at `node` as a value of the variable: for a write (`write`: an integer
    /// variable takes whole numbers only, and the range counts) or for a comparison.
    std::optional<control::Value> value_of(ryml::id_type node, std::size_t variable, const std::string& what, bool write)
    {
        const ScenarioVariable& v = scenario_.variables[variable];
        const control::VariableInfo& info = *info_of(variable);
        const std::size_t line = doc_.line(node);
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
                const std::string guess = number || !hint.empty() ? std::string{} : detail::closest(text, names);
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
        return out;
    }

    /// Whether `value` may be written to the variable: as the run's hub decides for a converted
    /// variable (the seen ends of its range are products), else within min and max.
    bool in_range(double value, std::size_t variable) const
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

    static std::string range_text(const control::VariableInfo& info)
    {
        return (info.min ? "from " + number_text(*info.min) : std::string("up")) + (info.max ? " to " + number_text(*info.max) : "") +
               (info.unit.empty() ? "" : " " + info.unit);
    }

    /// Whether the step at `time` (empty: not known before the run) may write the variable.
    bool writable(std::size_t variable, std::size_t line, const std::optional<std::int64_t>& time, const char* over_time = nullptr)
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

    // --- steps ---

    void read_steps(ryml::id_type node)
    {
        if (!tree_.is_seq(node) || !tree_.has_children(node))
        {
            error("value", doc_.line(node),
                  "steps must be a list with at least one step (- set: ..., - expect: ...), not " +
                      (tree_.is_seq(node) ? std::string("an empty list") : shown_value(node)));
            return;
        }
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            ++out_.steps;
            read_step(child);
        }
    }

    void read_step(ryml::id_type node)
    {
        const std::size_t line = doc_.line(node);
        if (!tree_.is_map(node))
        {
            error("value", line, "a step must be a mapping with one of " + join(kActionKeys) + ", not " + shown_value(node));
            end_known_ = false; // what it would have been is not known
            return;
        }
        ScenarioStep step;
        step.line = line;
        absent_.clear();
        std::map<std::string, std::size_t> seen;
        std::map<std::string, ryml::id_type> nodes;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string key = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, key, child))
            {
                continue;
            }
            if (std::find(kStepKeys.begin(), kStepKeys.end(), key) == kStepKeys.end())
            {
                unknown_key(child, key, "", kStepKeys);
                continue;
            }
            nodes.emplace(key, child);
        }
        std::vector<std::string> actions;
        for (const auto& action : kActionKeys)
        {
            if (nodes.count(action) > 0)
            {
                actions.push_back(action);
            }
        }
        // An expect beside a ramp or a wave: true all through it.
        const bool beside = actions.size() == 2 && (actions.front() == "ramp" || actions.front() == "wave") && actions.back() == "expect";
        if (beside)
        {
            actions.pop_back();
        }
        if (actions.size() != 1)
        {
            error("value", line,
                  actions.empty() ? "the step does nothing: give it one of " + join(kActionKeys)
                                  : "the step has " + join(actions) + ": a step does one thing; make a step of each");
            end_known_ = false;
            return;
        }
        const std::string& action = actions.front();
        step.kind = action == "set"      ? StepKind::Set
                    : action == "ramp"   ? StepKind::Ramp
                    : action == "wave"   ? StepKind::Wave
                    : action == "expect" ? StepKind::Expect
                                         : StepKind::WaitUntil;

        // The keys that go with the action.
        const auto only_with = [&](const std::string& key, bool allowed, const std::string& with) {
            if (nodes.count(key) > 0 && !allowed)
            {
                error("value", doc_.line(nodes.at(key)), key + " goes with " + with + ", not with " + action + ": remove it");
                nodes.erase(key);
            }
        };
        if (beside)
        {
            for (const char* name : {"within_ms", "hold_for_ms"})
            {
                const std::string key = name;
                if (nodes.count(key) > 0)
                {
                    error("value", doc_.line(nodes.at(key)),
                          key + " does not go with an expect beside a " + action + ": that expect must be true all through the " +
                              action + ", from its time to its end; remove " + key + ", or make the expect a step of its own after the " +
                              action);
                    nodes.erase(key);
                }
            }
        }
        only_with("timeout_ms", step.kind == StepKind::WaitUntil, "wait_until");
        only_with("within_ms", step.kind == StepKind::Expect, "expect");
        only_with("hold_for_ms", step.kind == StepKind::Expect, "expect");
        only_with("undersampled", step.kind == StepKind::Wave, "wave");
        bool undersampledOk = false;
        if (nodes.count("undersampled") > 0)
        {
            const ryml::id_type said = nodes.at("undersampled");
            undersampledOk = is_scalar(said) && !is_null(said) && text_of(said) == "ok";
            if (!undersampledOk)
            {
                error("value", doc_.line(said),
                      "undersampled must be ok (run the step's waves as they are, also where they change faster than their ECU "
                      "steps), not " + shown_value(said) + ": write undersampled: ok, or remove it");
            }
        }

        // The time.
        std::optional<std::int64_t> time; // as far as it is known before the run
        const bool hasAt = nodes.count("at_ms") > 0;
        const bool hasAfter = nodes.count("after_ms") > 0;
        if (hasAt && hasAfter)
        {
            error("value", doc_.line(nodes.at("after_ms")),
                  "the step has at_ms and after_ms: give it one time, at_ms (the virtual time) or after_ms (from the end of "
                  "the step before it)");
        }
        else if (hasAt)
        {
            step.at_ms = millis(nodes.at("at_ms"), "at_ms", 0, "the virtual time of the step in milliseconds");
            if (step.at_ms)
            {
                if (!end_known_)
                {
                    error("scenario-time", doc_.line(nodes.at("at_ms")),
                          "at_ms after a step that waits (line " + std::to_string(wait_line_) +
                              "): when that step ends is not known before the run; give this step after_ms (from the end of "
                              "the step before it) instead");
                }
                else if (*step.at_ms < end_ms_)
                {
                    error("scenario-time", doc_.line(nodes.at("at_ms")),
                          "at_ms is " + std::to_string(*step.at_ms) + ", before the end of the step before it (" +
                              std::to_string(end_ms_) + " ms): the steps run in the order of the file, and time does not go "
                                                        "back; reorder the steps or raise at_ms");
                }
                else
                {
                    time = *step.at_ms;
                }
            }
        }
        else
        {
            if (hasAfter)
            {
                step.after_ms = millis(nodes.at("after_ms"), "after_ms", 0,
                                       "milliseconds of virtual time from the end of the step before it")
                                    .value_or(0);
            }
            if (end_known_)
            {
                time = end_ms_ + step.after_ms;
            }
        }

        std::int64_t length = 0; // of a step whose end is known before the run
        beside_ = beside;
        switch (step.kind)
        {
        case StepKind::Set: read_set(nodes.at("set"), step, time); break;
        case StepKind::Ramp: length = read_ramp(nodes.at("ramp"), step, time); break;
        case StepKind::Wave: length = read_wave(nodes.at("wave"), step, time, undersampledOk); break;
        case StepKind::Expect:
        case StepKind::WaitUntil:
            read_conditions(nodes.at(action), step, action);
            if (step.kind == StepKind::WaitUntil)
            {
                if (nodes.count("timeout_ms") == 0)
                {
                    error("missing-key", line,
                          "wait_until needs timeout_ms: how long to wait at most, in milliseconds of virtual time (when it "
                          "passes, the step fails)");
                }
                else
                {
                    step.within_ms = millis(nodes.at("timeout_ms"), "timeout_ms", 1, "milliseconds of virtual time to wait at most");
                }
            }
            else if (nodes.count("within_ms") > 0 && nodes.count("hold_for_ms") > 0)
            {
                error("value", doc_.line(nodes.at("hold_for_ms")),
                      "the step has within_ms and hold_for_ms: within_ms says \"true at some time within\", hold_for_ms \"true all "
                      "the time\"; make a step of each");
            }
            else if (nodes.count("within_ms") > 0)
            {
                step.within_ms = millis(nodes.at("within_ms"), "within_ms", 1, "milliseconds of virtual time in which it must become true");
            }
            else if (nodes.count("hold_for_ms") > 0)
            {
                step.hold_for_ms = millis(nodes.at("hold_for_ms"), "hold_for_ms", 1, "milliseconds of virtual time for which it must stay true");
                length = step.hold_for_ms.value_or(0);
            }
            break;
        }
        // The absent variables of the ramp or the wave, before those of the expect beside it.
        motion_absent_ = absent_.size();
        if (beside)
        {
            read_conditions(nodes.at("expect"), step, "expect");
        }
        // Only the expect uses what is absent: the ramp or wave runs, unchecked.
        unchecked_ = beside && motion_absent_ == 0 && !absent_.empty();
        step.text = step_text(step);
        const bool checks = beside || step.kind == StepKind::Expect || step.kind == StepKind::WaitUntil;
        if (checks)
        {
            checks_.push_back(line);
        }
        if (unchecked_)
        {
            step.expect_skipped = skip_reason();
            step.conditions.clear();
            skipped_checks_.push_back(line);
            unchecked_lines_.insert(line);
            ++out_.unchecked_expects;
            add(Severity::Note, "scenario-step-skipped", step.line,
                "the " + std::string(step_kind_name(step.kind)) + " runs; its expect is not checked (" + step.text +
                    "): " + step.expect_skipped);
        }
        else if (!absent_.empty())
        {
            // It takes the time its text says (length), as in a configuration that has the
            // variables: the times of the steps after it do not depend on what is skipped.
            skip(step);
            step.skipped_ms = length; // 0 for a step that waits: when it would end is not in the text
            if (checks)
            {
                skipped_checks_.push_back(line);
            }
        }
        // The latest this step can end: the times must stay within what a time can hold.
        latest_ms_ = (time ? *time : latest_ms_ + step.after_ms) + length + step.within_ms.value_or(0);
        if (latest_ms_ > kMaxMillis && !too_long_)
        {
            too_long_ = true;
            error("scenario-time", line,
                  "the scenario can last " + std::to_string(latest_ms_) + " ms up to the end of this step, more than the longest "
                  "time (" + std::to_string(kMaxMillis) + " ms): shorten the times (after_ms, over_ms, for_ms, hold_for_ms, the "
                  "timeouts) of the steps up to here");
        }
        if (step.waits() || step.kind == StepKind::WaitUntil)
        {
            if (end_known_)
            {
                wait_line_ = line;
            }
            end_known_ = false;
        }
        else if (time)
        {
            end_ms_ = *time + length;
        }
        scenario_.steps.push_back(std::move(step));
    }

    /// The step uses variables that their participants do not have (absent_): it does nothing in
    /// the run, which says so; a note says it before.
    void skip(ScenarioStep& step)
    {
        step.skipped = skip_reason();
        step.writes.clear();
        step.signals.clear();
        step.conditions.clear();
        ++out_.skipped;
        add(Severity::Note, "scenario-step-skipped", step.line,
            "the step is skipped (" + step.text + "): " + step.skipped +
                "; the other steps run, also those that would need what this one does");
    }

    /// Why the variables of absent_ cannot be used: "Body.boost is absent: participant 'Body' does
    /// not have it in this configuration (absent: true in its variables)".
    std::string skip_reason() const
    {
        std::vector<std::string> names;
        std::vector<std::string> owners;
        std::vector<std::string> leftOut; // the participants the variant leaves out, once each
        for (const std::string& name : absent_)
        {
            const std::string& owner = absent_variables_.at(name);
            if (left_out_.count(owner) > 0)
            {
                if (std::find(leftOut.begin(), leftOut.end(), "'" + owner + "'") == leftOut.end())
                {
                    leftOut.push_back("'" + owner + "'");
                }
            }
            else if (std::find(names.begin(), names.end(), name) == names.end())
            {
                names.push_back(name);
                owners.push_back("'" + owner + "'");
            }
        }
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
        const bool one = names.size() == 1;
        std::string out;
        if (!leftOut.empty())
        {
            // Not "take it in (--enable)": that would run a simulation that is none of the variants.
            out = (leftOut.size() == 1 ? "participant " + leftOut.front() + " is" : "the participants " + join(leftOut) + " are") +
                  " not in the variant " + context_.variant + " (enabled: false in that variant)";
        }
        if (!names.empty())
        {
            out += (out.empty() ? "" : "; ") + join(names) + (one ? " is absent: participant " : " are absent: ") +
                   (one ? owners.front() + " does not have it" : "the participants " + join(owners) + " do not have them") +
                   (context_.variant.empty() ? " in this configuration" : " in the variant " + context_.variant) +
                   " (absent: true in " + (one ? "its" : "their") + " variables)";
        }
        return out;
    }

    void read_set(ryml::id_type node, ScenarioStep& step, const std::optional<std::int64_t>& time)
    {
        if (!tree_.is_map(node) || !tree_.has_children(node))
        {
            error("value", doc_.line(node),
                  "set must be a mapping of variables to values ({ " + example_variable() + ": 1 }), not " + shown_value(node));
            return;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            if (!first_occurrence(seen, detail::to_string(tree_.key(child)), child, "set: "))
            {
                continue;
            }
            const auto v = variable(child);
            if (!v || !writable(*v, doc_.line(child), time))
            {
                continue;
            }
            if (const auto value = value_of(child, *v, "the value", true))
            {
                step.writes.push_back(ScenarioWrite{*v, *value});
            }
        }
    }

    /// The over_ms of the ramp of a variable that is absent (`node`: its entry in "ramp"), or 0 if
    /// it has none that can be read. Only this is read of it: it is a time, which needs no type;
    /// "to" and "from" need the variable's.
    std::int64_t skipped_over_ms(ryml::id_type node)
    {
        if (!tree_.is_map(node))
        {
            return 0;
        }
        for (auto part = tree_.first_child(node); part != ryml::NONE; part = tree_.next_sibling(part))
        {
            if (detail::to_string(tree_.key(part)) == "over_ms")
            {
                return millis(part, "over_ms", 1, "milliseconds of virtual time the ramp takes").value_or(0);
            }
        }
        return 0;
    }

    /// @returns the longest over_ms, also of the variables that are absent.
    std::int64_t read_ramp(ryml::id_type node, ScenarioStep& step, const std::optional<std::int64_t>& time)
    {
        std::int64_t longest = 0;
        if (!tree_.is_map(node) || !tree_.has_children(node))
        {
            error("value", doc_.line(node),
                  "ramp must be a mapping of variables to { to: <value>, over_ms: <milliseconds> }, not " + shown_value(node));
            return longest;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string name = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, name, child, "ramp: "))
            {
                continue;
            }
            const std::size_t absentBefore = absent_.size();
            const auto v = variable(child);
            if (!v)
            {
                if (absent_.size() > absentBefore)
                {
                    longest = std::max(longest, skipped_over_ms(child));
                }
                continue;
            }
            const std::size_t line = doc_.line(child);
            bool good = writable(*v, line, time, "ramp");
            const ScenarioVariable& var = scenario_.variables[*v];
            if (good && var.type != control::ValueType::Integer && var.type != control::ValueType::Real)
            {
                error("scenario-value", line,
                      "ramp needs a number, and " + var.text + " is " + (var.type == control::ValueType::Bool ? "a bool" : "an enum") +
                          ": use set");
                good = false;
            }
            if (!tree_.is_map(child))
            {
                error("value", line, "ramp of " + name + " must be { to: <value>, over_ms: <milliseconds> }, not " + shown_value(child));
                continue;
            }
            ScenarioSignal ramp;
            ramp.variable = *v;
            double to = 0.0;
            bool hasTo = false;
            bool hasOver = false;
            std::map<std::string, std::size_t> keys;
            for (auto part = tree_.first_child(child); part != ryml::NONE; part = tree_.next_sibling(part))
            {
                const std::string key = detail::to_string(tree_.key(part));
                if (!first_occurrence(keys, key, part, "ramp of " + name + ": "))
                {
                    continue;
                }
                if (key == "over_ms")
                {
                    if (const auto ms = millis(part, "over_ms", 1, "milliseconds of virtual time the ramp takes"))
                    {
                        ramp.length_ms = *ms;
                        hasOver = true;
                    }
                    else
                    {
                        good = false;
                    }
                }
                else if (key == "to" || key == "from")
                {
                    if (!good)
                    {
                        hasTo = hasTo || key == "to";
                        continue;
                    }
                    // A ramp writes numbers in between: its ends are numbers of the range, also for an integer.
                    double d = 0;
                    if (!number_of(part, *v, d))
                    {
                        error("scenario-value", doc_.line(part),
                              key + " of the ramp of " + var.text + " must be a number, not " + shown_value(part) + hex_hint(part, var.type));
                        good = false;
                    }
                    else if (!in_range(d, *v))
                    {
                        error("scenario-value", doc_.line(part),
                              key + " of the ramp of " + var.text + " is " + written(part, d) + ", outside its range (" +
                                  range_text(*info_of(*v)) + ")");
                        good = false;
                    }
                    else if (key == "to")
                    {
                        to = d;
                    }
                    else
                    {
                        ramp.base = d;
                    }
                    hasTo = hasTo || key == "to";
                }
                else
                {
                    unknown_key(part, key, "ramp of " + name + ": ", kRampKeys);
                }
            }
            if (!hasTo)
            {
                error("missing-key", line, "ramp of " + name + " needs to: the value at the end of the ramp");
            }
            if (keys.count("over_ms") == 0)
            {
                error("missing-key", line,
                      "ramp of " + name + " needs over_ms: how long the ramp takes, in milliseconds of virtual time");
            }
            if (!hasTo || !hasOver)
            {
                continue;
            }
            if (good)
            {
                longest = std::max(longest, ramp.length_ms);
                ramp.shape = detail::ramp_shape(to, std::chrono::milliseconds(ramp.length_ms));
                ramp.hint = "give the ramp from: <value>";
                ramp.text = var.text + (ramp.base ? " from " + number_text(*ramp.base) : "") + " to " + number_text(to) + " over " +
                            std::to_string(ramp.length_ms) + " ms";
                step.signals.push_back(std::move(ramp));
            }
        }
        return longest;
    }

    // --- wave ---

    using Parts = std::map<std::string, ryml::id_type>;

    static std::string span_text(SimDuration d)
    {
        const std::int64_t ns = d.count();
        return (ns % 1'000'000 == 0 ? std::to_string(ns / 1'000'000) : number_text(static_cast<double>(ns) / 1e6)) + " ms";
    }

    /// With one decimal at most: "33.3".
    static std::string rounded_text(double value) { return number_text(std::round(value * 10.0) / 10.0); }

    /// The entries of the mapping `node` by key; a key twice or not in `allowed` is reported and
    /// left out. Empty (and an error) if `node` is not a mapping: `must` says what it must be.
    std::optional<Parts> parts_of(ryml::id_type node, const std::string& what, const std::vector<std::string>& allowed,
                                  const std::string& must)
    {
        if (!tree_.is_map(node))
        {
            error("value", doc_.line(node), what + " must be " + must + ", not " + shown_value(node));
            return std::nullopt;
        }
        Parts out;
        std::map<std::string, std::size_t> seen;
        for (auto part = tree_.first_child(node); part != ryml::NONE; part = tree_.next_sibling(part))
        {
            const std::string key = detail::to_string(tree_.key(part));
            if (!first_occurrence(seen, key, part, what + ": "))
            {
                continue;
            }
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            {
                unknown_key(part, key, what + ": ", allowed);
                continue;
            }
            out.emplace(key, part);
        }
        return out;
    }

    /// A number that is not a value of the variable (an amplitude, a frequency), from `least`
    /// (not `least` itself if `above`) to `most`; `meaning` says what it is.
    std::optional<double> measure(ryml::id_type node, const std::string& label, double least, bool above, double most,
                                  const std::string& meaning)
    {
        double d = 0;
        if (!plain_number(node, d))
        {
            error("value", doc_.line(node),
                  label + " must be a number (" + meaning + "), not " + shown_value(node) +
                      (quoted(node) ? ": write the number without quotes" : ""));
            return std::nullopt;
        }
        if (d < least || (above && d == least) || d > most)
        {
            error("value", doc_.line(node),
                  label + " is " + number_text(d) + ", but it must be " + (above ? "more than " : "from ") + number_text(least) +
                      (std::isfinite(most) ? std::string(above ? " and at most " : " to ") + number_text(most) : std::string{}) + " (" +
                      meaning + ")");
            return std::nullopt;
        }
        return d;
    }

    std::optional<bool> flag(ryml::id_type node, const std::string& label, const std::string& meaning)
    {
        const std::string text = is_scalar(node) && !is_null(node) && !quoted(node) ? text_of(node) : std::string{};
        if (text != "true" && text != "false")
        {
            error("value", doc_.line(node), label + " must be true or false (" + meaning + "), not " + shown_value(node));
            return std::nullopt;
        }
        return text == "true";
    }

    std::optional<std::string> choice(ryml::id_type node, const std::string& label, const std::vector<std::string>& options)
    {
        const std::string text = is_scalar(node) && !is_null(node) ? text_of(node) : std::string{};
        if (std::find(options.begin(), options.end(), text) == options.end())
        {
            error("value", doc_.line(node), label + " must be " + detail::or_list(options) + ", not " + shown_value(node));
            return std::nullopt;
        }
        return text;
    }

    std::optional<std::uint64_t> seed_of(ryml::id_type node, const std::string& label)
    {
        std::int64_t value = 0;
        if (!plain_integer(node, value) || value < 0)
        {
            error("value", doc_.line(node),
                  label + " must be a whole number from 0 (the same seed gives the same values in every run), not " + shown_value(node));
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(value);
    }

    /// A value of the variable that a wave writes as it is (`key` of `what`): for a bool true or
    /// false, for an enum an item, else a number of the range (also for an integer: the wave
    /// rounds what it writes).
    std::optional<double> level(ryml::id_type node, std::size_t variable, const std::string& key, const std::string& what)
    {
        const ScenarioVariable& var = scenario_.variables[variable];
        if (var.type == control::ValueType::Bool || var.type == control::ValueType::Enum)
        {
            const auto value = value_of(node, variable, key + " of " + what, true);
            return value ? std::optional<double>{as_number(*value)} : std::nullopt;
        }
        double d = 0;
        if (!number_of(node, variable, d))
        {
            error("scenario-value", doc_.line(node),
                  key + " of " + what + " of " + var.text + " must be a number, not " + shown_value(node) +
                      (quoted(node) ? ": write the number without quotes" : hex_hint(node, var.type)));
            return std::nullopt;
        }
        if (!in_range(d, variable))
        {
            error("scenario-value", doc_.line(node),
                  key + " of " + what + " of " + var.text + " is " + written(node, d) + ", outside its range (" +
                      range_text(*info_of(variable)) + ")");
            return std::nullopt;
        }
        return d;
    }

    /// period_ms or frequency_hz, and phase_deg, of a shape that repeats. `text`: as the step's
    /// text shows them.
    std::optional<detail::Periodic> periodic_of(const Parts& parts, const std::string& what, std::size_t line, std::string& text)
    {
        detail::Periodic out;
        bool ok = true;
        const bool hasPeriod = parts.count("period_ms") > 0;
        const bool hasFrequency = parts.count("frequency_hz") > 0;
        if (hasPeriod && hasFrequency)
        {
            error("value", doc_.line(parts.at("frequency_hz")),
                  what + " has period_ms and frequency_hz: give one of them (frequency_hz: 50 is period_ms: 20)");
            ok = false;
        }
        else if (hasPeriod)
        {
            if (const auto ms = millis(parts.at("period_ms"), "period_ms", 1, "milliseconds of virtual time of one period"))
            {
                out.period = std::chrono::milliseconds(*ms);
                text += " period " + std::to_string(*ms) + " ms";
            }
            else
            {
                ok = false;
            }
        }
        else if (hasFrequency)
        {
            // A period of a whole number of nanoseconds, at least 1 and within the longest time.
            const double least = 1e3 / static_cast<double>(kMaxMillis);
            if (const auto hz = measure(parts.at("frequency_hz"), "frequency_hz of " + what, least, false, 1e9, "periods per second of virtual time"))
            {
                out.period = SimDuration{std::llround(1e9 / *hz)};
                text += " frequency " + number_text(*hz) + " Hz";
            }
            else
            {
                ok = false;
            }
        }
        else
        {
            error("missing-key", line, what + " needs period_ms or frequency_hz: how long one period is, or how many there are in a second");
            ok = false;
        }
        if (parts.count("phase_deg") > 0)
        {
            if (const auto phase = measure(parts.at("phase_deg"), "phase_deg of " + what, -36000.0, false, 36000.0,
                                           "where in its period the shape begins, in degrees: 360 is a whole period"))
            {
                out.phase_deg = *phase;
                if (*phase != 0.0)
                {
                    text += " phase " + number_text(*phase) + " deg";
                }
            }
            else
            {
                ok = false;
            }
        }
        return ok ? std::optional<detail::Periodic>{out} : std::nullopt;
    }

    /// The step of the variable's ECU, to compare a wave with. Empty if the step's waves are not
    /// compared (undersampled: ok) or the step is not known (a note says so).
    std::optional<SimDuration> sampling(std::size_t variable, const std::string& what, std::size_t line, bool quiet)
    {
        if (quiet)
        {
            return std::nullopt;
        }
        const ScenarioParticipant& p = scenario_.participants[scenario_.variables[variable].participant];
        if (p.step <= SimDuration::zero())
        {
            add(Severity::Note, "scenario-wave-not-checked", line,
                "whether participant '" + p.name + "' can see " + what + " as it is written is not checked: the participant's step "
                "is not known",
                p.name);
            return std::nullopt;
        }
        return p.step;
    }

    std::string takes_inputs(std::size_t variable) const
    {
        const ScenarioParticipant& p = scenario_.participants[scenario_.variables[variable].participant];
        return "participant '" + p.name + "' takes its inputs once per step of " + span_text(p.step);
    }
    std::string step_of(std::size_t variable) const
    {
        const ScenarioParticipant& p = scenario_.participants[scenario_.variables[variable].participant];
        return "the step of participant '" + p.name + "' (" + span_text(p.step) + ")";
    }
    SimDuration step_ns(std::size_t variable) const
    {
        return scenario_.participants[scenario_.variables[variable].participant].step;
    }
    /// For a key in whole milliseconds: the first that is not shorter than `d`.
    static std::string whole_ms(SimDuration d) { return std::to_string((d.count() + 999'999) / 1'000'000); }
    const std::string& owner_of(std::size_t variable) const
    {
        return scenario_.participants[scenario_.variables[variable].participant].name;
    }

    /// A shape that repeats every `period`: too fast for its ECU (a warning), or, if it is smooth
    /// (its values between the corners matter; `factor` is then its value, -1 to 1, at a time
    /// since the step began), all the same at the times the ECU takes it (a warning) or coarse (a
    /// note). @returns whether it warned.
    ///
    /// The wave writes at T, T + step, T + 2 step, ... (T: when the scenario's step begins), and
    /// the ECU applies each in a step of its own, so what the ECU sees does not depend on T.
    bool check_period(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration period,
                      const std::function<double(SimDuration)>& factor = {})
    {
        const auto step = sampling(variable, what, line, quiet);
        if (!step)
        {
            return true;
        }
        const bool smooth = static_cast<bool>(factor);
        // What to make of a smooth shape: a period that shows it. Two values per period show a square.
        const std::string longer = smooth ? "Make the period " + span_text(kSmoothSteps * *step) + " or more (" +
                                                std::to_string(kSmoothSteps) + " values per period)"
                                          : "Make the period " + span_text(2 * *step) + " or more";
        if (period < 2 * *step)
        {
            add(Severity::Warning, "scenario-wave-undersampled", line,
                what + " has a period of " + span_text(period) + ", but " + takes_inputs(variable) +
                    ": with fewer than two values per period the ECU sees a slower shape or a constant, not this one. " + longer +
                    kAsItIs,
                owner_of(variable));
            return true;
        }
        if (smooth && period < kSmoothSteps * *step)
        {
            // The values the ECU takes repeat after `count` of them: if they are few, look at them.
            const std::int64_t count = period.count() / std::gcd(period.count(), step->count());
            if (count <= kSmoothSteps)
            {
                double low = factor(SimDuration::zero());
                double high = low;
                for (std::int64_t k = 1; k < count; ++k)
                {
                    const double value = factor(k * *step);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                if (high - low <= kSameValue)
                {
                    const std::string shape = what.substr(0, what.find(" of "));
                    add(Severity::Warning, "scenario-wave-undersampled", line,
                        what + " has a period of " + span_text(period) + ", and " + takes_inputs(variable) + ": at each of those times " +
                            shape + " is at " + (std::fabs(low) <= kSameValue ? "its middle" : "the same value") +
                            ", so the ECU sees a constant, not " + shape + ". " + longer + kAsItIs,
                        owner_of(variable));
                    return true;
                }
            }
            add(Severity::Note, "scenario-wave-coarse", line,
                what + " has a period of " + span_text(period) + ", and " + takes_inputs(variable) + ": the ECU sees " +
                    rounded_text(static_cast<double>(period.count()) / static_cast<double>(step->count())) +
                    " values per period, so it can miss the highest and the lowest. A period of " +
                    span_text(kSmoothSteps * *step) + " or more gives it " + std::to_string(kSmoothSteps) + kAsItIs,
                owner_of(variable));
        }
        return false;
    }

    /// A square whose edges do not fall on the steps of its ECU: the duty the ECU sees.
    void check_square(std::size_t variable, const std::string& what, std::size_t line, bool quiet, const detail::Periodic& periodic,
                      double duty)
    {
        if (check_period(variable, what, line, quiet, periodic.period))
        {
            return;
        }
        const std::int64_t step = scenario_.participants[scenario_.variables[variable].participant].step.count();
        const std::int64_t period = periodic.period.count();
        if (period % step != 0)
        {
            add(Severity::Warning, "scenario-wave-duty", line,
                what + " has a period of " + span_text(periodic.period) + ", which is not a multiple of " + step_of(variable) +
                    ": the ECU sees high and low times that change from period to period. Make the period a multiple of " +
                    span_text(SimDuration{step}) + kAsItIs,
                owner_of(variable));
            return;
        }
        // The ECU takes the values at 0, step, 2 step, ... of the wave: those of them that are high.
        const std::int64_t high = signal::square_high_time(periodic.period, duty).count();
        const std::int64_t first = signal::time_in_period(SimDuration::zero(), periodic.period, periodic.phase_deg).count() % step;
        const std::int64_t seen = high > first ? (high - first + step - 1) / step * step : 0;
        if (seen != high)
        {
            add(Severity::Warning, "scenario-wave-duty", line,
                what + " is high for " + span_text(SimDuration{high}) + " of each period of " + span_text(periodic.period) + " (duty " +
                    number_text(duty) + " %), but " + takes_inputs(variable) + ": the ECU sees it high for " +
                    span_text(SimDuration{seen}) + " of each period (" +
                    rounded_text(100.0 * static_cast<double>(seen) / static_cast<double>(period)) +
                    " %). Make the high time (period x duty / 100) a multiple of " + span_text(SimDuration{step}) + kAsItIs,
                owner_of(variable));
        }
    }

    /// Something of a wave that lasts `span`, shorter than the step of its ECU: `is` says what it
    /// is and how it compares ("is 3 ms wide, shorter than"), `sees` what the ECU sees of it,
    /// `make` what to change.
    void check_span(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration span, const std::string& is,
                    const std::string& sees, const std::string& make)
    {
        const auto step = sampling(variable, what, line, quiet);
        if (step && span < *step)
        {
            add(Severity::Warning, "scenario-wave-undersampled", line,
                what + " " + is + " " + step_of(variable) + ": " + sees + ". " + make + kAsItIs, owner_of(variable));
        }
    }

    enum class Seen
    {
        Yes,
        No,
        Depends, ///< on when the scenario's step begins, which is not known before the run
    };

    /// Whether the ECU gets a value that a wave has from `from` until `to` (times since the
    /// scenario's step began; `to` is not part of it) and no longer; the wave ends at `length`.
    /// `time`: when the scenario's step begins (ms), if that is known before the run.
    ///
    /// The wave writes at T, T + step, T + 2 step, ... and at T + length (what it leaves), and the
    /// ECU applies each write in its first step at or after the write's time: its steps are at 0,
    /// step, 2 step, .... A value that none of the regular writes carries is never applied. Two
    /// regular writes go to two steps of the ECU; the last regular write and the one of the end
    /// go to the same step, where the end's wins, if no step of the ECU is from the one until the
    /// other. `written`, `ends`: those two times, when that decides (Yes or No with `time`; as
    /// times since the step began for Depends).
    static Seen seen_between(SimDuration step, const std::optional<std::int64_t>& time, SimDuration from, SimDuration to, SimDuration length,
                             SimDuration& written, SimDuration& ends)
    {
        const std::int64_t s = step.count();
        const std::int64_t first = (from.count() + s - 1) / s * s; // the first regular write at or after `from`
        if (first >= to.count())
        {
            return Seen::No;
        }
        if (first + s <= length.count())
        {
            return Seen::Yes; // the write after it is a regular one
        }
        written = SimDuration{first};
        ends = length;
        if (!time)
        {
            return Seen::Depends;
        }
        const SimDuration begins = std::chrono::milliseconds(*time);
        written += begins;
        ends += begins;
        const std::int64_t applied = (written.count() + s - 1) / s * s; // the ECU's first step at or after the write
        return applied < ends.count() ? Seen::Yes : Seen::No;
    }

    /// A pulse that is shorter than the step of its ECU: whether the ECU sees it, as far as that
    /// is known before the run.
    void check_pulse(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration width,
                     const std::optional<std::int64_t>& time)
    {
        const auto step = sampling(variable, what, line, quiet);
        if (!step || width >= *step)
        {
            return;
        }
        SimDuration written{};
        SimDuration ends{};
        std::string sees;
        switch (seen_between(*step, time, SimDuration::zero(), width, width, written, ends))
        {
        case Seen::Yes:
            sees = "this step begins at " + span_text(written) + ", so the ECU has the pulse's value for one whole step of " +
                   span_text(*step) + " (the one at " + span_text(SimDuration{(written.count() + step->count() - 1) / step->count() * step->count()}) +
                   "), not for " + span_text(width);
            break;
        case Seen::No:
            sees = "this step begins at " + span_text(written) + ", and none of the ECU's steps is from then until the pulse ends (" +
                   span_text(ends) + "), so the ECU never gets the pulse's value";
            break;
        case Seen::Depends:
            sees = "the ECU has the pulse's value for one whole step if one of its steps is from the pulse's beginning until its end, "
                   "and never otherwise; when this step begins is not known before the run";
            break;
        }
        add(Severity::Warning, "scenario-wave-undersampled", line,
            what + " is " + span_text(width) + " wide, shorter than " + step_of(variable) + ": " + sees + ". Make width_ms " +
                whole_ms(*step) + " or more" + kAsItIs,
            owner_of(variable));
    }

    /// How long the wave of a variable that is absent takes (`node`: its entry in "wave"), as far
    /// as its text says it without the variable's type: for_ms, a pulse's width_ms, the last time
    /// of points, the bounces of a chatter. 0 if that cannot be read.
    std::int64_t skipped_wave_ms(ryml::id_type node)
    {
        if (!tree_.is_map(node))
        {
            return 0;
        }
        std::int64_t length = 0;
        const auto child = [&](ryml::id_type of, const std::string& key) {
            if (tree_.is_map(of))
            {
                for (auto part = tree_.first_child(of); part != ryml::NONE; part = tree_.next_sibling(part))
                {
                    if (detail::to_string(tree_.key(part)) == key)
                    {
                        return part;
                    }
                }
            }
            return static_cast<ryml::id_type>(ryml::NONE);
        };
        const auto whole = [&](ryml::id_type of) {
            std::int64_t value = 0;
            return of != ryml::NONE && plain_integer(of, value) && value >= 0 && value <= kMaxMillis ? value : std::int64_t{0};
        };
        if (const auto forMs = child(node, "for_ms"); forMs != ryml::NONE)
        {
            return millis(forMs, "for_ms", 1, "milliseconds of virtual time the wave takes").value_or(0);
        }
        if (const auto pulse = child(node, "pulse"); pulse != ryml::NONE)
        {
            length = whole(child(pulse, "width_ms"));
        }
        else if (const auto chatter = child(node, "chatter"); chatter != ryml::NONE)
        {
            const std::int64_t bounces = whole(child(chatter, "bounces"));
            const std::int64_t interval = whole(child(chatter, "interval_ms"));
            length = bounces <= kMaxBounces && interval <= kMaxMillis / (2 * kMaxBounces) ? 2 * bounces * interval : 0;
        }
        else if (const auto points = child(node, "points"); points != ryml::NONE && tree_.is_seq(points) && tree_.has_children(points))
        {
            const ryml::id_type last = tree_.last_child(points);
            length = tree_.is_seq(last) && tree_.has_children(last) ? whole(tree_.first_child(last)) : 0;
        }
        else if (points != ryml::NONE && tree_.is_map(points))
        {
            // The time on the file's last line, if it can be read.
            const auto name = [&](const std::string& key) {
                const auto of = child(points, key);
                return of != ryml::NONE && is_scalar(of) && !is_null(of) ? std::optional<std::string>{text_of(of)} : std::nullopt;
            };
            const auto path = name("file");
            const CsvFile* file = path && !path_problem(*path) ? &csv(*path) : nullptr;
            if (file != nullptr && file->table)
            {
                const auto column = name("time");
                const auto& columns = file->table->columns;
                const auto at = column ? std::find(columns.begin(), columns.end(), *column) : columns.begin();
                double ms = 0;
                const auto end = at == columns.end() ? std::nullopt
                                                     : cell_ms(file->table->rows.back().cells[static_cast<std::size_t>(at - columns.begin())],
                                                               name("time_unit") == std::optional<std::string>{"s"}, ms);
                length = end.value_or(0);
            }
        }
        return length;
    }

    /// @returns the longest length, also of the variables that are absent.
    std::int64_t read_wave(ryml::id_type node, ScenarioStep& step, const std::optional<std::int64_t>& time, bool quiet)
    {
        std::int64_t longest = 0;
        if (!tree_.is_map(node) || !tree_.has_children(node))
        {
            error("value", doc_.line(node),
                  "wave must be a mapping of variables to a shape and how long it lasts ({ " + example_variable() +
                      ": { sine: { amplitude: 1, period_ms: 100 }, for_ms: 1000 } }), not " + shown_value(node));
            return longest;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            if (!first_occurrence(seen, detail::to_string(tree_.key(child)), child, "wave: "))
            {
                continue;
            }
            const std::size_t absentBefore = absent_.size();
            const auto v = variable(child);
            if (!v)
            {
                if (absent_.size() > absentBefore)
                {
                    longest = std::max(longest, skipped_wave_ms(child));
                }
                continue;
            }
            if (auto one = read_wave_of(child, *v, time, quiet))
            {
                longest = std::max(longest, one->length_ms);
                step.signals.push_back(std::move(*one));
            }
        }
        return longest;
    }

    /// The wave of one variable (`node`: its entry in "wave"), or empty (and its errors).
    std::optional<ScenarioSignal> read_wave_of(ryml::id_type node, std::size_t v, const std::optional<std::int64_t>& time, bool quiet)
    {
        const std::size_t line = doc_.line(node);
        const ScenarioVariable& var = scenario_.variables[v];
        const std::string name = var.text;
        bool good = writable(v, line, time, "wave");
        const auto top = parts_of(node, "wave of " + name, kWaveKeys,
                                  "a shape with its values, and how long it lasts ({ sine: { amplitude: 1, period_ms: 100 }, for_ms: 1000 })");
        if (!top)
        {
            return std::nullopt;
        }
        std::vector<std::string> shapes;
        for (const std::string& known : kWaveShapes)
        {
            if (top->count(known) > 0)
            {
                shapes.push_back(known);
            }
        }
        if (shapes.size() != 1)
        {
            error("value", line,
                  shapes.empty() ? "wave of " + name + " names no shape: give it one of " + join(kWaveShapes)
                                 : "wave of " + name + " has " + join(shapes) + ": a variable takes one shape in a step; make a step of each");
            return std::nullopt;
        }
        const std::string& shape = shapes.front();
        const ryml::id_type body = top->at(shape);
        const std::string what = "the " + shape + " of " + name;
        const bool isBool = var.type == control::ValueType::Bool;
        const bool isEnum = var.type == control::ValueType::Enum;
        const bool number = !isBool && !isEnum;
        const bool switching = shape == "square" || shape == "pulse" || shape == "chatter";
        if (!(number || shape == "points" || (isBool && switching)))
        {
            error("scenario-value", line,
                  shape + " needs a number, and " + var.text + " is " +
                      (isBool ? "a bool: for a bool use square, pulse, chatter or points" : "an enum: for an enum use points, or set"));
            return std::nullopt;
        }

        ScenarioSignal out;
        out.variable = v;
        out.hint = "give the wave around: <value>";
        std::string text = name + " " + shape;
        std::optional<std::int64_t> length; // of a shape that says it itself
        bool wantsFor = true;               // for_ms is how long it lasts
        std::string noAround;               // why around does not go with it, or ""
        std::optional<double> swing;        // how far from around it goes at most, if that is known
        // What makes the shape, once everything is read: (around or its other base, the length).
        std::function<std::shared_ptr<const Waveform>(SimDuration)> make;

        if (shape == "points")
        {
            good = read_points(body, top->count("interpolate") > 0 ? top->at("interpolate") : ryml::NONE, v, what, time, quiet, out, text, length, make) && good;
            wantsFor = false;
            noAround = "the points are the values themselves (for where it begins, start with a point at 0: [0, <value>])";
        }
        else
        {
            static const std::map<std::string, std::vector<std::string>> kShapeKeys = {
                {"sine", {"amplitude", "period_ms", "frequency_hz", "phase_deg"}},
                {"triangle", {"amplitude", "period_ms", "frequency_hz", "phase_deg"}},
                {"sawtooth", {"amplitude", "period_ms", "frequency_hz", "phase_deg", "falling"}},
                {"square", {"low", "high", "amplitude", "period_ms", "frequency_hz", "duty", "phase_deg"}},
                {"pulse", {"value", "width_ms"}},
                {"noise", {"amplitude", "hold_ms", "seed", "distribution"}},
                {"chirp", {"amplitude", "from_hz", "to_hz", "sweep"}},
                {"lag", {"to", "tau_ms", "from", "settle"}},
                {"chatter", {"to", "interval_ms", "bounces", "seed", "from"}},
            };
            static const std::map<std::string, std::string> kShapeExample = {
                {"sine", "{ amplitude: 1, period_ms: 100 }"},
                {"triangle", "{ amplitude: 1, period_ms: 100 }"},
                {"sawtooth", "{ amplitude: 1, period_ms: 100 }"},
                {"square", "{ low: 0, high: 1, period_ms: 100 }"},
                {"pulse", "{ value: 1, width_ms: 50 }"},
                {"noise", "{ amplitude: 1, hold_ms: 10, seed: 1 }"},
                {"chirp", "{ amplitude: 1, from_hz: 1, to_hz: 10 }"},
                {"lag", "{ to: 1, tau_ms: 100 }"},
                {"chatter", "{ to: 1, bounces: 3, interval_ms: 10 }"},
            };
            const auto parts = parts_of(body, what, kShapeKeys.at(shape), "a mapping of its values (" + kShapeExample.at(shape) + ")");
            if (!parts)
            {
                return std::nullopt;
            }
            const auto has = [&](const char* key) { return parts->count(key) > 0; };
            // A key the shape cannot do without: `meaning` says what it is.
            const auto need = [&](const char* key, const std::string& meaning) {
                if (!has(key))
                {
                    error("missing-key", line, what + " needs " + key + ": " + meaning);
                    good = false;
                    return false;
                }
                return true;
            };
            const auto amplitude_of = [&](const std::string& meaning) {
                std::optional<double> amplitude;
                if (need("amplitude", meaning))
                {
                    amplitude = measure(parts->at("amplitude"), "amplitude of " + what, 0.0, false, HUGE_VAL, meaning);
                    good = good && amplitude.has_value();
                    text += amplitude ? " amplitude " + number_text(*amplitude) : "";
                }
                return amplitude.value_or(0.0);
            };

            if (shape == "sine" || shape == "triangle" || shape == "sawtooth")
            {
                const double amplitude = amplitude_of("how far the value goes above and below the middle");
                const auto periodic = periodic_of(*parts, what, line, text);
                bool falling = false;
                if (has("falling"))
                {
                    const auto said = flag(parts->at("falling"), "falling of " + what, "true: each period goes down and jumps up");
                    good = good && said.has_value();
                    falling = said.value_or(false);
                    text += falling ? " falling" : "";
                }
                good = good && periodic.has_value();
                swing = amplitude;
                if (periodic)
                {
                    const detail::Periodic p = *periodic;
                    check_period(v, what, line, quiet, p.period, [&](SimDuration t) {
                        return shape == "sine" ? signal::sine(t, p.period, p.phase_deg)
                               : shape == "triangle" ? signal::triangle(t, p.period, p.phase_deg)
                                                     : signal::sawtooth(t, p.period, falling, p.phase_deg);
                    });
                    make = [=](SimDuration) {
                        return shape == "sine" ? detail::sine_shape(amplitude, *periodic)
                               : shape == "triangle" ? detail::triangle_shape(amplitude, *periodic)
                                                     : detail::sawtooth_shape(amplitude, *periodic, falling);
                    };
                }
            }
            else if (shape == "square")
            {
                std::optional<double> low;
                std::optional<double> high;
                std::optional<double> amplitude;
                const bool levels = has("low") || has("high");
                if (isBool)
                {
                    for (const char* key : {"low", "high", "amplitude"})
                    {
                        if (has(key))
                        {
                            error("value", doc_.line(parts->at(key)),
                                  std::string(key) + " does not go with " + what + ": the square of a bool is false and true; remove it");
                            good = false;
                        }
                    }
                    low = 0.0;
                    high = 1.0;
                }
                else if (levels && has("amplitude"))
                {
                    error("value", doc_.line(parts->at("amplitude")),
                          what + " has amplitude and " + (has("low") ? "low" : "high") +
                              ": give low and high (its two values), or amplitude (so far above and below the middle)");
                    good = false;
                }
                else if (levels)
                {
                    if (need("low", "the value while it is low") && need("high", "the value while it is high"))
                    {
                        low = level(parts->at("low"), v, "low", "the square");
                        high = level(parts->at("high"), v, "high", "the square");
                        good = good && low && high;
                        text += low && high ? " low " + number_text(*low) + " high " + number_text(*high) : "";
                    }
                }
                else
                {
                    amplitude = amplitude_of("how far the value goes above and below the middle; or give low and high, its two values");
                    swing = amplitude;
                }
                const auto periodic = periodic_of(*parts, what, line, text);
                good = good && periodic.has_value();
                double duty = 50.0;
                if (has("duty"))
                {
                    const auto said = measure(parts->at("duty"), "duty of " + what, 0.0, false, 100.0,
                                              "the percent of each period for which it is high");
                    good = good && said.has_value();
                    duty = said.value_or(50.0);
                    text += said ? " duty " + number_text(duty) + " %" : "";
                }
                if (!amplitude)
                {
                    noAround = isBool ? "a bool has no middle" : "low and high are the values themselves";
                }
                if (periodic)
                {
                    check_square(v, what, line, quiet, *periodic, duty);
                    if (amplitude)
                    {
                        make = [=](SimDuration) { return detail::square_shape(*amplitude, *periodic, duty); };
                    }
                    else if (low && high)
                    {
                        make = [=](SimDuration) { return detail::square_levels_shape(*low, *high, *periodic, duty); };
                    }
                }
            }
            else if (shape == "pulse")
            {
                std::optional<double> value;
                if (isBool && !has("value"))
                {
                    value = 1.0;
                }
                else if (need("value", "the value during the pulse"))
                {
                    value = level(parts->at("value"), v, "value", "the pulse");
                    good = good && value.has_value();
                }
                text += value ? " " + (isBool ? std::string(*value != 0.0 ? "true" : "false") : number_text(*value)) : "";
                if (need("width_ms", "how long the pulse lasts, in milliseconds of virtual time"))
                {
                    length = millis(parts->at("width_ms"), "width_ms", 1, "milliseconds of virtual time the pulse lasts");
                    good = good && length.has_value();
                    text += length ? " width " + std::to_string(*length) + " ms" : "";
                }
                wantsFor = false;
                if (isBool)
                {
                    noAround = "a bool has no middle (the pulse returns to the other value)";
                    out.base = value.value_or(1.0) != 0.0 ? 0.0 : 1.0;
                }
                else
                {
                    noAround = "a pulse returns to the value the variable has when the pulse begins (for another value after it, "
                               "write a set in the next step)";
                    out.hint = "set the variable in a step before the pulse (set: { " + name + ": <value> })";
                }
                if (length)
                {
                    check_pulse(v, what, line, quiet, std::chrono::milliseconds(*length), time);
                }
                if (value)
                {
                    make = [=](SimDuration) { return detail::pulse_shape(*value); };
                }
            }
            else if (shape == "noise")
            {
                bool gaussian = false;
                if (has("distribution"))
                {
                    const auto said = choice(parts->at("distribution"), "distribution of " + what, {"uniform", "gaussian"});
                    good = good && said.has_value();
                    gaussian = said && *said == "gaussian";
                }
                const double amplitude = amplitude_of(gaussian ? "the standard deviation of the values around the middle"
                                                               : "how far the values go above and below the middle at most");
                std::optional<std::int64_t> hold;
                if (need("hold_ms", "how long each value is kept, in milliseconds of virtual time"))
                {
                    hold = millis(parts->at("hold_ms"), "hold_ms", 1, "milliseconds of virtual time each value is kept");
                    good = good && hold.has_value();
                    text += hold ? " hold " + std::to_string(*hold) + " ms" : "";
                }
                std::optional<std::uint64_t> seed;
                if (need("seed", "a whole number that picks the values; the same seed gives the same values in every run, so that a "
                                 "failure can be run again"))
                {
                    seed = seed_of(parts->at("seed"), "seed of " + what);
                    good = good && seed.has_value();
                    text += seed ? " seed " + std::to_string(*seed) : "";
                }
                text += gaussian ? " gaussian" : "";
                if (!gaussian)
                {
                    swing = amplitude; // a gaussian value has no bound worth checking: the run refuses what is outside
                }
                if (hold && seed)
                {
                    const SimDuration held = std::chrono::milliseconds(*hold);
                    check_span(v, what, line, quiet, held, "changes every " + span_text(held) + ", faster than",
                               "the ECU gets only the value at the time of each of its steps; the values between are never applied",
                               "Make hold_ms " + whole_ms(step_ns(v)) + " or more");
                    // (The variable's name is part of the seed: the same seed for two variables is two sequences.)
                    make = [=, mixed = detail::seed_of_variable(*seed, name)](SimDuration) {
                        return detail::noise_shape(amplitude, held, mixed, gaussian);
                    };
                }
            }
            else if (shape == "chirp")
            {
                const double amplitude = amplitude_of("how far the value goes above and below the middle");
                std::optional<double> fromHz;
                std::optional<double> toHz;
                // (0 Hz is a frequency: a linear sweep can begin or end standing still.)
                if (need("from_hz", "the frequency at the beginning, in periods per second of virtual time"))
                {
                    fromHz = measure(parts->at("from_hz"), "from_hz of " + what, 0.0, false, 1e9, "the frequency at the beginning");
                    good = good && fromHz.has_value();
                }
                if (need("to_hz", "the frequency at the end (after for_ms)"))
                {
                    toHz = measure(parts->at("to_hz"), "to_hz of " + what, 0.0, false, 1e9, "the frequency at the end");
                    good = good && toHz.has_value();
                }
                text += fromHz && toHz ? " from " + number_text(*fromHz) + " Hz to " + number_text(*toHz) + " Hz" : "";
                signal::Sweep sweep = signal::Sweep::Linear;
                if (has("sweep"))
                {
                    const auto said = choice(parts->at("sweep"), "sweep of " + what, {"linear", "log"});
                    good = good && said.has_value();
                    sweep = said && *said == "log" ? signal::Sweep::Log : signal::Sweep::Linear;
                    text += sweep == signal::Sweep::Log ? " log" : "";
                }
                swing = amplitude;
                if (fromHz && toHz && sweep == signal::Sweep::Log && (*fromHz == 0.0 || *toHz == 0.0))
                {
                    const char* key = *fromHz == 0.0 ? "from_hz" : "to_hz";
                    error("value", doc_.line(parts->at(key)),
                          std::string(key) + " of " + what + " is 0, but a log sweep changes the frequency by the same factor every second, "
                          "which neither begins nor ends at 0 Hz: give a frequency above 0, or write sweep: linear");
                    good = false;
                }
                else if (fromHz && toHz && *fromHz == 0.0 && *toHz == 0.0)
                {
                    error("value", doc_.line(parts->at("to_hz")),
                          what + " goes from 0 Hz to 0 Hz, which does not move: give from_hz or to_hz a frequency above 0");
                    good = false;
                }
                else if (fromHz && toHz)
                {
                    check_chirp(v, what, line, quiet, std::max(*fromHz, *toHz));
                    make = [=](SimDuration all) { return detail::chirp_shape(amplitude, *fromHz, *toHz, all, sweep); };
                }
            }
            else if (shape == "lag")
            {
                std::optional<double> to;
                if (need("to", "the value it goes towards"))
                {
                    to = level(parts->at("to"), v, "to", "the lag");
                    good = good && to.has_value();
                }
                if (has("from"))
                {
                    out.base = level(parts->at("from"), v, "from", "the lag");
                    good = good && out.base.has_value();
                    text += out.base ? " from " + number_text(*out.base) : "";
                }
                text += to ? " to " + number_text(*to) : "";
                std::optional<std::int64_t> tau;
                if (need("tau_ms", "its time constant in milliseconds of virtual time: after this long it has gone 63 % of the way"))
                {
                    tau = millis(parts->at("tau_ms"), "tau_ms", 1, "the time constant in milliseconds of virtual time");
                    good = good && tau.has_value();
                    text += tau ? " tau " + std::to_string(*tau) + " ms" : "";
                }
                bool settle = false;
                if (has("settle"))
                {
                    const auto said = flag(parts->at("settle"), "settle of " + what, "true: at the end the value is set to `to`");
                    good = good && said.has_value();
                    settle = said.value_or(false);
                    text += settle ? " settle" : "";
                }
                noAround = "the value it starts from is its from (lag: { from: <value>, ... })";
                out.hint = "give the lag from: <value>";
                if (to && tau)
                {
                    make = [=](SimDuration all) { return detail::lag_shape(*to, std::chrono::milliseconds(*tau), all, settle); };
                }
            }
            else // chatter
            {
                std::optional<double> to;
                if (need("to", "the value it comes to rest at"))
                {
                    to = level(parts->at("to"), v, "to", "the chatter");
                    good = good && to.has_value();
                }
                if (has("from") && isBool)
                {
                    error("value", doc_.line(parts->at("from")),
                          "from does not go with " + what + ": a bool bounces between to and the other value; remove it");
                    good = false;
                }
                else if (has("from"))
                {
                    out.base = level(parts->at("from"), v, "from", "the chatter");
                    good = good && out.base.has_value();
                    text += out.base ? " from " + number_text(*out.base) : "";
                }
                else if (isBool && to)
                {
                    out.base = *to != 0.0 ? 0.0 : 1.0;
                }
                text += to ? " to " + (isBool ? std::string(*to != 0.0 ? "true" : "false") : number_text(*to)) : "";
                std::optional<std::int64_t> interval;
                if (need("interval_ms", "how long it stays at a value between two changes, in milliseconds of virtual time"))
                {
                    interval = millis(parts->at("interval_ms"), "interval_ms", 1, "milliseconds of virtual time between two changes");
                    good = good && interval.has_value();
                }
                std::optional<std::uint64_t> seed;
                if (has("seed"))
                {
                    seed = seed_of(parts->at("seed"), "seed of " + what);
                    good = good && seed.has_value();
                }
                const bool hasFor = top->count("for_ms") > 0;
                if (has("bounces") && hasFor)
                {
                    error("value", doc_.line(top->at("for_ms")),
                          what + " has bounces and for_ms: give one of them (bounces: how often it falls back; for_ms: how long it bounces)");
                    good = false;
                }
                else if (has("bounces"))
                {
                    std::int64_t bounces = 0;
                    if (!plain_integer(parts->at("bounces"), bounces) || bounces < 1 || bounces > kMaxBounces)
                    {
                        error("value", doc_.line(parts->at("bounces")),
                              "bounces of " + what + " must be a whole number from 1 to " + std::to_string(kMaxBounces) +
                                  " (how often it falls back before it rests), not " + shown_value(parts->at("bounces")));
                        good = false;
                    }
                    else if (interval && *interval > kMaxMillis / (2 * bounces))
                    {
                        error("value", doc_.line(parts->at("bounces")),
                              what + " lasts 2 x bounces x interval_ms, more than the longest time (" + std::to_string(kMaxMillis) + " ms): lower them");
                        good = false;
                    }
                    else if (interval)
                    {
                        // To, back, to, back, ...: two stretches per bounce, then it rests.
                        length = 2 * bounces * *interval;
                        text += " " + std::to_string(bounces) + (bounces == 1 ? " bounce" : " bounces");
                    }
                    wantsFor = false;
                }
                else if (!hasFor)
                {
                    error("missing-key", line,
                          what + " needs bounces (how often it falls back before it rests) or for_ms (how long it bounces)");
                    good = false;
                    wantsFor = false;
                }
                text += interval ? " interval " + std::to_string(*interval) + " ms" : "";
                text += seed ? " seed " + std::to_string(*seed) : "";
                noAround = "the value it bounces back to is its from (chatter: { from: <value>, ... }), or the value it has";
                out.hint = "give the chatter from: <value>";
                if (interval)
                {
                    // With a seed two changes are as little as half an interval apart.
                    const SimDuration regular = std::chrono::milliseconds(*interval);
                    const SimDuration apart = seed ? regular / 2 : regular;
                    check_span(v, what, line, quiet, apart,
                               seed ? "changes as little as " + span_text(apart) + " apart (interval_ms " + std::to_string(*interval) +
                                          " with a seed: a half to one and a half intervals), closer than"
                                    : "changes every " + span_text(apart) + ", faster than",
                               "the ECU gets only the value at the time of each of its steps, so it sees some of the bounces or none",
                               "Make interval_ms " + whole_ms(seed ? 2 * step_ns(v) : step_ns(v)) + " or more");
                }
                if (to && interval)
                {
                    const std::optional<std::uint64_t> mixed = seed ? std::optional{detail::seed_of_variable(*seed, name)} : std::nullopt;
                    make = [=](SimDuration) { return detail::chatter_shape(*to, std::chrono::milliseconds(*interval), mixed); };
                }
            }
        }

        // What stands beside the shape.
        if (top->count("interpolate") > 0 && shape != "points")
        {
            error("value", doc_.line(top->at("interpolate")), "interpolate goes with points, not with " + shape + ": remove it");
            good = false;
        }
        if (top->count("for_ms") > 0)
        {
            if (wantsFor)
            {
                length = millis(top->at("for_ms"), "for_ms", 1, "milliseconds of virtual time the wave takes");
                good = good && length.has_value();
            }
            else if (shape != "chatter") // (a chatter with bounces and for_ms: said above)
            {
                error("value", doc_.line(top->at("for_ms")),
                      "for_ms does not go with " + shape + ": " +
                          (shape == "pulse" ? "a pulse lasts its width_ms" : "the points end at the last of them") + "; remove it");
                good = false;
            }
        }
        else if (wantsFor)
        {
            error("missing-key", line, "wave of " + name + " needs for_ms: how long the " + shape + " lasts, in milliseconds of virtual time");
            good = false;
        }
        std::optional<double> around;
        if (top->count("around") > 0)
        {
            if (!noAround.empty())
            {
                error("value", doc_.line(top->at("around")), "around does not go with " + what + ": " + noAround + "; remove it");
                good = false;
            }
            else
            {
                around = level(top->at("around"), v, "around", "the " + shape);
                good = good && around.has_value();
                out.base = around;
            }
        }
        if (good && around && swing && (!in_range(*around - *swing, v) || !in_range(*around + *swing, v)))
        {
            error("scenario-value", line,
                  what + " goes from " + number_text(*around - *swing) + " to " + number_text(*around + *swing) + " (around " +
                      number_text(*around) + ", amplitude " + number_text(*swing) + "), outside its range (" +
                      range_text(*info_of(v)) + "): lower the amplitude, or move around");
            good = false;
        }
        if (!good || !length || !make)
        {
            return std::nullopt;
        }
        out.length_ms = *length;
        out.shape = make(std::chrono::milliseconds(*length));
        out.text = text + (around ? " around " + number_text(*around) : "") +
                   (shape == "pulse" || shape == "points" ? "" : (shape == "chatter" && !wantsFor ? "" : " for " + std::to_string(*length) + " ms"));
        return out;
    }

    /// A chirp whose highest frequency its ECU cannot see (a warning), or sees coarsely (a note).
    void check_chirp(std::size_t variable, const std::string& what, std::size_t line, bool quiet, double top_hz)
    {
        const auto step = sampling(variable, what, line, quiet);
        if (!step)
        {
            return;
        }
        const double period = 1e9 / top_hz; // in nanoseconds
        const double steps = period / static_cast<double>(step->count());
        const auto hz_at = [&](std::int64_t count) { return rounded_text(1e9 / static_cast<double>(count * step->count())); };
        if (steps < 2.0)
        {
            add(Severity::Warning, "scenario-wave-undersampled", line,
                what + " goes up to " + number_text(top_hz) + " Hz (a period of " + rounded_text(period / 1e6) + " ms), but " +
                    takes_inputs(variable) + ": above " + hz_at(2) +
                    " Hz (two steps per period) the ECU sees a slower shape, not the sweep. Keep from_hz and to_hz at " + hz_at(2) +
                    " or less" + kAsItIs,
                owner_of(variable));
        }
        else if (steps < static_cast<double>(kSmoothSteps))
        {
            add(Severity::Note, "scenario-wave-coarse", line,
                what + " goes up to " + number_text(top_hz) + " Hz (a period of " + rounded_text(period / 1e6) + " ms), and " +
                    takes_inputs(variable) + ": at that frequency the ECU sees " + rounded_text(steps) +
                    " values per period, so it can miss the highest and the lowest. Up to " + hz_at(kSmoothSteps) + " Hz it gets " +
                    std::to_string(kSmoothSteps) + " or more" + kAsItIs,
                owner_of(variable));
        }
    }

    /// The points of a wave (`body`: the list; `interpolate`: its node or NONE). @returns whether
    /// they have no error; then `length` and `make` are set.
    bool read_points(ryml::id_type body, ryml::id_type interpolate, std::size_t v, const std::string& what,
                     const std::optional<std::int64_t>& time, bool quiet, ScenarioSignal& out, std::string& text, std::optional<std::int64_t>& length,
                     std::function<std::shared_ptr<const Waveform>(SimDuration)>& make)
    {
        const ScenarioVariable& var = scenario_.variables[v];
        const bool number = var.type == control::ValueType::Integer || var.type == control::ValueType::Real;
        bool good = true;
        bool hold = !number;
        if (interpolate != ryml::NONE)
        {
            const auto said = choice(interpolate, "interpolate of " + what, {"linear", "hold"});
            if (!said)
            {
                good = false;
            }
            else if (*said == "linear" && !number)
            {
                error("scenario-value", doc_.line(interpolate),
                      "interpolate: linear needs a number, and " + var.text + " is " +
                          (var.type == control::ValueType::Bool ? "a bool" : "an enum") +
                          ": write interpolate: hold (each value is kept until the next point), or leave it out");
                good = false;
            }
            else
            {
                hold = *said == "hold";
            }
        }
        std::vector<signal::Point> points;
        std::vector<std::string> names; // of each point, as the messages name it
        std::optional<std::int64_t> before;
        std::optional<std::pair<std::int64_t, std::int64_t>> closest; // the two times that are closest
        // The next point (`after`: how a message names the one before it), or false and an error at `line`.
        const auto add = [&](std::int64_t at, double value, std::string name, const std::string& after, std::size_t line) {
            if (before && at <= *before)
            {
                error("value", line,
                      "the times of " + what + " must go up: " + name + " is at " + std::to_string(at) + " ms, not after " + after + " (" +
                          std::to_string(*before) + " ms)" +
                          (at == *before && !hold ? "; for a value that jumps, write interpolate: hold beside the points (each value is "
                                                    "then kept until the next point)"
                                                  : ""));
                return false;
            }
            if (before && (!closest || at - *before < closest->second - closest->first))
            {
                closest = std::pair{*before, at};
            }
            before = at;
            points.push_back(signal::Point{std::chrono::milliseconds(at), value});
            names.push_back(std::move(name));
            return true;
        };
        std::string source; // " from <file> column <name>" for points from a file
        if (tree_.is_map(body))
        {
            const bool read = file_points(body, v, what, add, source);
            if (!good || !read)
            {
                return false;
            }
            out.hint = "begin the file with a line at 0 ms (0,<value>)";
        }
        else
        {
            if (!tree_.is_seq(body) || !tree_.has_children(body))
            {
                error("value", doc_.line(body),
                      "points of " + var.text + " must be a list of [<milliseconds since the step began>, <value>] ([[0, 0], [100, 50]]), or " +
                          "a CSV file and its column (" + kPointsFileExample + "), not " +
                          (tree_.is_seq(body) ? std::string("an empty list") : shown_value(body)));
                return false;
            }
            std::size_t index = 0;
            for (auto item = tree_.first_child(body); item != ryml::NONE; item = tree_.next_sibling(item))
            {
                ++index;
                const std::string label = "point " + std::to_string(index);
                if (!tree_.is_seq(item) || tree_.num_children(item) != 2)
                {
                    error("value", doc_.line(item),
                          label + " of " + what + " must be [<milliseconds since the step began>, <value>], not " + shown_value(item));
                    good = false;
                    continue;
                }
                const ryml::id_type timeNode = tree_.first_child(item);
                const ryml::id_type valueNode = tree_.last_child(item);
                const auto at = millis(timeNode, "the time of " + label, 0, "milliseconds of virtual time since the step began");
                const auto value = level(valueNode, v, "the value of " + label, "the points");
                if (!at || !value || !add(*at, *value, label, "point " + std::to_string(index - 1), doc_.line(item)))
                {
                    good = false;
                }
            }
            if (!good)
            {
                return false;
            }
            out.hint = "begin the points with one at 0 ms ([0, <value>])";
        }
        if (points.back().time <= SimDuration::zero())
        {
            error("value", doc_.line(body),
                  what + " end at 0 ms: a wave takes time; give a later point, or use set for a value at one time");
            return false;
        }
        length = points.back().time.count() / 1'000'000;
        text += source + " (" + std::to_string(points.size()) + (hold ? ", hold" : ", linear") + ") over " + std::to_string(*length) + " ms";
        if (hold)
        {
            check_held_points(v, what, doc_.line(body), quiet, points, names, time);
        }
        else if (closest)
        {
            // On lines every point is a corner: closer than a step, the ECU misses some of them.
            const SimDuration apart = std::chrono::milliseconds(closest->second - closest->first);
            check_span(v, what, doc_.line(body), quiet, apart,
                       "are as little as " + span_text(apart) + " apart (at " + std::to_string(closest->first) + " ms and " +
                           std::to_string(closest->second) + " ms), closer than",
                       "the ECU gets only the value at the time of each of its steps, so it does not see every point",
                       "Put the points " + span_text(step_ns(v)) + " or more apart");
        }
        make = [points = std::move(points), hold](SimDuration) { return detail::points_shape(points, hold); };
        return true;
    }

    // --- points from a CSV file ---

    /// A file the scenario names, as read once (ScenarioContext::read_file, read_csv()).
    struct CsvFile
    {
        std::optional<detail::CsvTable> table;
        std::string problem; ///< if there is no table: after the file's name
    };

    const CsvFile& csv(const std::string& path)
    {
        if (const auto it = files_.find(path); it != files_.end())
        {
            return it->second;
        }
        CsvFile file;
        if (!context_.read_file)
        {
            file.problem = "is not read here: points from a file are read with the scenario file (check --scenario, run --scenario)";
        }
        else if (const auto text = context_.read_file(path, file.problem))
        {
            file.table = detail::read_csv(*text, file.problem);
        }
        return files_.emplace(path, std::move(file)).first->second;
    }

    /// What is wrong with the path of a points file (after "the file '<path>' "), or empty: it must
    /// lead from the scenario's directory to a file in it or below it, the same on every OS.
    static std::optional<std::string> path_problem(const std::string& path)
    {
        static const std::string kOnlyBelow = "a scenario reads only files in its own directory and below it";
        if (path.empty())
        {
            return "is empty: give the path of the CSV file from the scenario file's directory (traces/drive.csv)";
        }
        if (path.find('\\') != std::string::npos)
        {
            return "has '\\': write '/' between directories (traces/drive.csv), which every OS reads";
        }
        if (path.front() == '/' || (path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0])) != 0))
        {
            return "is an absolute path: give the path from the scenario file's directory (traces/drive.csv); " + kOnlyBelow;
        }
        for (std::size_t at = 0; at <= path.size();)
        {
            const std::size_t end = std::min(path.find('/', at), path.size());
            if (path.compare(at, end - at, "..") == 0)
            {
                return "goes up a directory (..): " + kOnlyBelow + "; put the file there";
            }
            at = end + 1;
        }
        return std::nullopt;
    }

    /// A time cell in milliseconds (`seconds`: the cell is in seconds): empty if it is not a
    /// number (`ms` is then NaN), not a whole number of milliseconds, or not from 0 to kMaxMillis.
    static std::optional<std::int64_t> cell_ms(const std::string& cell, bool seconds, double& ms)
    {
        double read = 0;
        if (!read_number(cell, read))
        {
            ms = std::nan("");
            return std::nullopt;
        }
        ms = seconds ? read * 1000.0 : read;
        const double whole = std::round(ms);
        // A time in seconds is whole milliseconds when it differs from them only in its last bits (0.001 * 1000).
        if (std::abs(ms - whole) > 1e-6 + std::abs(ms) * 4 * DBL_EPSILON)
        {
            return std::nullopt;
        }
        ms = whole;
        if (whole < 0 || whole > static_cast<double>(kMaxMillis))
        {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(whole);
    }

    /// The value of a cell for the variable, as a wave writes it (level()): for a bool true, false,
    /// 1 or 0; for an enum an item by name or by value; else a number of the range (also hexadecimal
    /// for an integer). Empty and an error at `line` if it is not one. `where`: "line 7 of f.csv
    /// (column throttle)".
    std::optional<double> cell_value(const std::string& cell, std::size_t variable, const std::string& where, std::size_t line)
    {
        const ScenarioVariable& var = scenario_.variables[variable];
        const control::VariableInfo& info = *info_of(variable);
        const std::string pre = "the value on " + where + " for " + var.text + " (" + type_name(var.type) + ")";
        const Hex hex = read_hex(cell);
        switch (var.type)
        {
        case control::ValueType::Bool:
            if (cell == "true" || cell == "1" || cell == "false" || cell == "0")
            {
                return cell == "true" || cell == "1" ? 1.0 : 0.0;
            }
            error("scenario-value", line, pre + " must be true, false, 1 or 0, not " + detail::shown_cell(cell));
            return std::nullopt;
        case control::ValueType::Enum:
        {
            std::int64_t i = 0;
            const bool number = read_integer(cell, i) || hex.kind == Hex::Kind::Read;
            if (hex.kind == Hex::Kind::Read)
            {
                i = static_cast<std::int64_t>(hex.magnitude);
            }
            const auto item = std::find_if(info.items.begin(), info.items.end(),
                                           [&](const control::EnumItem& e) { return number ? e.value == i : e.name == cell; });
            if (item != info.items.end())
            {
                return static_cast<double>(item->value);
            }
            error("scenario-value", line,
                  pre + " must be one of its items, by name or by value, not " + detail::shown_cell(cell) + ": the items are " +
                      items_text(info.items));
            return std::nullopt;
        }
        case control::ValueType::Integer:
        case control::ValueType::Real:
            break;
        }
        double d = 0;
        if (!read_number(cell, d))
        {
            if (var.type != control::ValueType::Integer || hex.kind != Hex::Kind::Read)
            {
                error("scenario-value", line, pre + " must be a number, not " + detail::shown_cell(cell));
                return std::nullopt;
            }
            d = static_cast<double>(hex.magnitude);
        }
        if (!in_range(d, variable))
        {
            error("scenario-value", line, pre + " is " + number_text(d) + ", outside its range (" + range_text(info) + ")");
            return std::nullopt;
        }
        return d;
    }

    /// The points of a wave from a CSV file (`body`: { file, value, time, time_unit }), each given
    /// to `add` (read_points()). Stops at the first error. `source`: " from <file> column <name>".
    template <class Add>
    bool file_points(ryml::id_type body, std::size_t v, const std::string& what, const Add& add, std::string& source)
    {
        const auto parts = parts_of(body, what, kPointsFileKeys, std::string("a CSV file and its column (") + kPointsFileExample + ")");
        if (!parts)
        {
            return false;
        }
        bool good = true;
        // The name in `key` (a column, the file), or empty and an error.
        const auto name = [&](const std::string& key, const std::string& meaning) -> std::optional<std::string> {
            if (parts->count(key) == 0)
            {
                return std::nullopt;
            }
            const ryml::id_type node = parts->at(key);
            if (!is_scalar(node) || is_null(node) || text_of(node).empty())
            {
                error("value", doc_.line(node), key + " of " + what + " must be " + meaning + ", not " + shown_value(node));
                good = false;
                return std::nullopt;
            }
            return text_of(node);
        };
        for (const auto& [key, meaning] : {std::pair<std::string, std::string>{"file", "the CSV file, from the scenario file's directory"},
                                           {"value", "the name of the column of the values"}})
        {
            if (parts->count(key) == 0)
            {
                error("missing-key", doc_.line(body), what + " from a file need " + key + ": " + meaning + " (" + kPointsFileExample + ")");
                good = false;
            }
        }
        const auto path = name("file", "the path of the CSV file from the scenario file's directory (traces/drive.csv)");
        const auto valueName = name("value", "the name of the column of the values");
        const auto timeName = name("time", "the name of the column of the times (without it, the first column)");
        bool seconds = false;
        if (parts->count("time_unit") > 0)
        {
            const auto unit = choice(parts->at("time_unit"), "time_unit of " + what, {"ms", "s"});
            good = unit.has_value() && good;
            seconds = unit == std::optional<std::string>{"s"};
        }
        if (!good || !path || !valueName)
        {
            return false;
        }
        const std::size_t line = doc_.line(parts->at("file"));
        if (const auto wrong = path_problem(*path))
        {
            error("scenario-points-file", line, what + ": the file '" + *path + "' " + *wrong);
            return false;
        }
        const CsvFile& file = csv(*path);
        if (!file.table)
        {
            error("scenario-points-file", line, what + ": " + *path + " " + file.problem);
            return false;
        }
        const detail::CsvTable& table = *file.table;
        // The index of a column, or empty and an error.
        const auto column = [&](const std::string& wanted, const std::string& key) -> std::optional<std::size_t> {
            std::vector<std::size_t> found;
            for (std::size_t i = 0; i < table.columns.size(); ++i)
            {
                if (table.columns[i] == wanted)
                {
                    found.push_back(i + 1);
                }
            }
            if (found.size() == 1)
            {
                return found.front() - 1;
            }
            if (found.size() > 1)
            {
                error("scenario-points-file", doc_.line(parts->at(key)),
                      what + ": " + *path + " has " + std::to_string(found.size()) + " columns named " + detail::shown_cell(wanted) +
                          " (columns " + std::to_string(found[0]) + " and " + std::to_string(found[1]) +
                          "): give its columns different names on its first line");
                return std::nullopt;
            }
            std::vector<std::string> shown;
            for (std::size_t i = 0; i < table.columns.size() && i < 10; ++i)
            {
                shown.push_back(detail::shown_cell(table.columns[i]));
            }
            const std::string guess = detail::closest(wanted, table.columns);
            error("scenario-points-file", doc_.line(parts->at(key)),
                  what + ": " + *path + " has no column " + detail::shown_cell(wanted) + " on its first line (line " +
                      std::to_string(table.header_line) + ")" +
                      (guess.empty() ? std::string{} : ": did you mean " + detail::shown_cell(guess) + "?") + "; its columns are " +
                      join(shown) + (table.columns.size() > shown.size() ? " and " + std::to_string(table.columns.size() - shown.size()) + " more" : ""));
            return std::nullopt;
        };
        const auto valueColumn = column(*valueName, "value");
        const auto timeColumn = timeName ? column(*timeName, "time") : std::optional<std::size_t>{0};
        if (!valueColumn || !timeColumn)
        {
            return false;
        }
        const std::string& timeHead = table.columns[*timeColumn];
        if (*valueColumn == *timeColumn)
        {
            error("scenario-points-file", doc_.line(parts->at("value")),
                  what + ": value names the column of the times (" + detail::shown_cell(timeHead) + (timeName ? "" : ", the first column") +
                      "): name the column of the values" + (timeName ? "" : ", or the column of the times with time"));
            return false;
        }
        std::string after;
        for (const detail::CsvRow& row : table.rows)
        {
            const std::string here = "line " + std::to_string(row.line);
            const std::string at = here + " of " + *path;
            const std::string& cell = row.cells[*timeColumn];
            double ms = 0;
            const auto time = cell_ms(cell, seconds, ms);
            if (!time)
            {
                const std::string pre = "the time on " + at + " (column " + detail::shown_cell(timeHead) + ")";
                const std::string unit = seconds ? "seconds" : "milliseconds";
                error("value", line,
                      std::isnan(ms) ? pre + " must be a number of " + unit + " since the step began, not " + detail::shown_cell(cell) +
                                           (seconds || timeName ? "" : " (the times are in the first column unless time names another)")
                      : std::round(ms) != ms
                          ? pre + " is " + number_text(ms) + " ms: the times of points are whole milliseconds; round them in the file"
                          : pre + " is " + number_text(ms) + " ms, but it must be from 0 to " + std::to_string(kMaxMillis) +
                                " (milliseconds of virtual time since the step began)");
                return false;
            }
            const auto value = cell_value(row.cells[*valueColumn], v, at + " (column " + detail::shown_cell(*valueName) + ")", line);
            if (!value || !add(*time, *value, at, after, line))
            {
                return false;
            }
            after = here;
        }
        source = " from " + *path + " column " + *valueName;
        return true;
    }

    /// Points that are kept until the next (interpolate: hold): a jump is what they are for, so
    /// how close two of them are does not matter; one whose value the ECU never gets does. Says
    /// the first such point, or else the first of which that is not known before the run.
    void check_held_points(std::size_t variable, const std::string& what, std::size_t line, bool quiet,
                           const std::vector<signal::Point>& points, const std::vector<std::string>& names,
                           const std::optional<std::int64_t>& time)
    {
        const auto step = sampling(variable, what, line, quiet);
        if (!step)
        {
            return;
        }
        const std::string head = what + " keep ";
        const std::string fix = ". Put the points at multiples of " + span_text(*step) + kAsItIs;
        std::string unknown;
        for (std::size_t i = 0; i + 1 < points.size(); ++i)
        {
            SimDuration written{};
            SimDuration ends{};
            const Seen seen = seen_between(*step, time, points[i].time, points[i + 1].time, points.back().time, written, ends);
            const bool last = i + 2 == points.size();
            const std::string span = names[i] + " from " + span_text(points[i].time) + " to " + span_text(points[i + 1].time) +
                                     (last ? ", where they end" : "") + ", and " + takes_inputs(variable) + ": ";
            if (seen == Seen::No && written == SimDuration::zero() && ends == SimDuration::zero())
            {
                add(Severity::Warning, "scenario-wave-undersampled", line,
                    head + span + "the wave writes a value every " + span_text(*step) + " from the step's beginning, and none of those "
                    "times is in that span, so the value of " + names[i] + " is never applied" + fix,
                    owner_of(variable));
                return;
            }
            if (seen == Seen::No)
            {
                add(Severity::Warning, "scenario-wave-undersampled", line,
                    head + span + "this step begins at " + std::to_string(*time) + " ms, and none of the ECU's steps is from " +
                        span_text(written) + " (when the value of " + names[i] + " is written) until the points end (" +
                        span_text(ends) + "), so that value is never applied" + fix,
                    owner_of(variable));
                return;
            }
            if (seen == Seen::Depends && unknown.empty())
            {
                unknown = head + span + "the value of " + names[i] + " is applied only if one of the ECU's steps is from " +
                          span_text(written) + " to " + span_text(ends) + " after this step begins, and when it begins is not known before the run" + fix;
            }
        }
        if (!unknown.empty())
        {
            add(Severity::Warning, "scenario-wave-undersampled", line, unknown, owner_of(variable));
        }
    }

    void read_conditions(ryml::id_type node, ScenarioStep& step, const std::string& action)
    {
        if (!tree_.is_map(node) || !tree_.has_children(node))
        {
            error("value", doc_.line(node),
                  action + " must be a mapping of variables to what they must be ({ " + example_variable() +
                      ": { ge: 1 } }), not " + shown_value(node));
            return;
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
        {
            const std::string name = detail::to_string(tree_.key(child));
            if (!first_occurrence(seen, name, child, action + ": "))
            {
                continue;
            }
            const auto v = variable(child);
            if (!v)
            {
                continue;
            }
            const ScenarioVariable& var = scenario_.variables[*v];
            ScenarioCondition condition;
            condition.variable = *v;
            bool good = true;
            if (tree_.is_map(child))
            {
                std::optional<double> tolerance;
                std::size_t toleranceLine = 0;
                bool hasNear = false;
                std::map<std::string, std::size_t> keys;
                for (auto part = tree_.first_child(child); part != ryml::NONE; part = tree_.next_sibling(part))
                {
                    const std::string key = detail::to_string(tree_.key(part));
                    if (!first_occurrence(keys, key, part, name + ": "))
                    {
                        continue;
                    }
                    if (key == "tolerance")
                    {
                        double d = 0;
                        if (!plain_number(part, d) || d < 0)
                        {
                            error("value", doc_.line(part), "tolerance must be a number that is not negative, not " + shown_value(part));
                            good = false;
                        }
                        tolerance = d;
                        toleranceLine = doc_.line(part);
                        continue;
                    }
                    const auto op = compare_of(key);
                    if (!op)
                    {
                        unknown_key(part, key, name + ": ", kCompareKeys);
                        good = false;
                        continue;
                    }
                    const bool ordered = *op != Compare::Eq && *op != Compare::Ne;
                    if (ordered && (var.type == control::ValueType::Bool || var.type == control::ValueType::Enum))
                    {
                        error("scenario-value", doc_.line(part),
                              key + " compares numbers, and " + var.text + " is " +
                                  (var.type == control::ValueType::Bool ? "a bool" : "an enum") + ": use eq or ne");
                        good = false;
                        continue;
                    }
                    const auto value = value_of(part, *v, key, false);
                    if (!value)
                    {
                        good = false;
                        continue;
                    }
                    hasNear = hasNear || *op == Compare::Near;
                    condition.all.push_back(ScenarioComparison{*op, *value, 0.0});
                }
                if (hasNear != tolerance.has_value())
                {
                    error("value", hasNear ? doc_.line(child) : toleranceLine,
                          hasNear ? name + ": near needs tolerance (how far from the value it may be): add tolerance: <number>"
                                  : name + ": tolerance goes with near (near: <value>, tolerance: <number>): add near or remove it");
                    good = false;
                }
                for (auto& c : condition.all)
                {
                    if (c.op == Compare::Near)
                    {
                        c.tolerance = tolerance.value_or(0.0);
                    }
                }
                if (good && condition.all.empty())
                {
                    error("value", doc_.line(child),
                          name + ": nothing to compare with: give one of " + join(kCompareKeys) + ", or the value itself");
                    good = false;
                }
            }
            else if (const auto value = value_of(child, *v, "the value", false))
            {
                condition.all.push_back(ScenarioComparison{Compare::Eq, *value, 0.0});
            }
            else
            {
                good = false;
            }
            if (good)
            {
                condition.text = condition_text(var, condition);
                step.conditions.push_back(std::move(condition));
            }
        }
    }

    static std::optional<Compare> compare_of(const std::string& key)
    {
        for (const Compare op : {Compare::Eq, Compare::Ne, Compare::Ge, Compare::Le, Compare::Gt, Compare::Lt, Compare::Near})
        {
            if (key == compare_name(op))
            {
                return op;
            }
        }
        return std::nullopt;
    }

    static std::string condition_text(const ScenarioVariable& var, const ScenarioCondition& condition)
    {
        std::string out = var.text;
        bool first = true;
        for (const auto& c : condition.all)
        {
            out += std::string(first ? " " : ", ") + compare_name(c.op) + " " + value_text(c.value, &var) +
                   (c.op == Compare::Near ? " (tolerance " + number_text(c.tolerance) + ")" : "");
            first = false;
        }
        return out;
    }

    std::string step_text(const ScenarioStep& step) const
    {
        std::string out = step_kind_name(step.kind);
        bool first = true;
        const auto sep = [&] { return std::exchange(first, false) ? " " : "; "; };
        // The variables that are absent, by name alone (what they would be is not read), once each.
        const auto absent = [&](std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; ++i)
            {
                if (std::find(absent_.begin() + static_cast<std::ptrdiff_t>(from), absent_.begin() + static_cast<std::ptrdiff_t>(i), absent_[i]) ==
                    absent_.begin() + static_cast<std::ptrdiff_t>(i))
                {
                    out += sep() + absent_[i];
                }
            }
        };
        const std::size_t motion = beside_ ? motion_absent_ : absent_.size();
        absent(0, motion);
        for (const auto& w : step.writes)
        {
            const ScenarioVariable& var = scenario_.variables[w.variable];
            out += sep() + var.text + " = " + value_text(w.value, &var);
        }
        for (const auto& r : step.signals)
        {
            out += sep() + r.text;
        }
        if (beside_)
        {
            // "ramp Engine.engine.throttle to 80 over 1000 ms, expecting Engine.engine.speed le 3000"
            out += unchecked_ ? ", expect not checked:" : ", expecting";
            first = true;
            absent(motion, absent_.size());
        }
        for (const auto& c : step.conditions)
        {
            out += sep() + c.text;
        }
        if (step.kind == StepKind::WaitUntil && step.within_ms)
        {
            out += " (timeout " + std::to_string(*step.within_ms) + " ms)";
        }
        else if (step.within_ms)
        {
            out += " (within " + std::to_string(*step.within_ms) + " ms)";
        }
        else if (step.hold_for_ms)
        {
            out += " (for " + std::to_string(*step.hold_for_ms) + " ms)";
        }
        return out;
    }

    const ScenarioCatalog& catalog_;
    const ScenarioContext& context_;
    ScenarioRead& out_;
    std::map<std::string, CsvFile> files_; ///< the points files, by the path the scenario writes
    Scenario scenario_;
    std::vector<Known> params_;                         ///< every parameter that has a value
    std::map<ryml::id_type, std::size_t> substituted_;  ///< a value node that is "${name}": its index in params_
    std::set<std::string> undefined_;                   ///< the parameters without a value, said once
    std::map<std::string, std::string> absent_variables_; ///< "<participant>.<name>" that is absent: its participant
    std::vector<std::string> absent_;                   ///< those the step being read uses
    bool beside_ = false;          ///< the step being read is a ramp or a wave with an expect beside it
    bool unchecked_ = false;       ///< and only its expect uses what is absent: the expect is not checked
    std::set<std::size_t> unchecked_lines_; ///< of skipped_checks_, those of an expect beside that is not checked
    std::size_t motion_absent_ = 0; ///< of absent_, those of its ramp or wave (the others: of the expect)
    std::set<std::string> left_out_; ///< the participants of absent_variables_ that the variant leaves out
    std::vector<std::size_t> checks_;         ///< the lines of the steps that check (expect, wait_until, an expect beside)
    std::vector<std::size_t> skipped_checks_; ///< those of them that are skipped
    std::vector<const control::VariableInfo*> infos_; ///< of scenario_.variables, in the catalog
    std::vector<const control::MappedSchema*> maps_;  ///< of scenario_.variables: its participant's map, or nullptr
    std::map<std::string, std::size_t> variable_index_;
    std::map<std::string, std::size_t> bad_variables_;
    /// The end of the step before, in milliseconds, if it is known before the run.
    bool end_known_ = true;
    std::int64_t end_ms_ = 0;
    std::size_t wait_line_ = 0; ///< the first step that waits since the end was known
    /// The latest the step before can end, in milliseconds (a wait counted with its whole time).
    std::int64_t latest_ms_ = 0;
    bool too_long_ = false; ///< said once
};

} // namespace

const char* step_kind_name(StepKind kind)
{
    switch (kind)
    {
    case StepKind::Set: return "set";
    case StepKind::Ramp: return "ramp";
    case StepKind::Wave: return "wave";
    case StepKind::Expect: return "expect";
    case StepKind::WaitUntil: return "wait_until";
    }
    return "set";
}

std::string value_text(const control::Value& value, const ScenarioVariable* variable)
{
    if (const auto* b = std::get_if<bool>(&value))
    {
        return *b ? "true" : "false";
    }
    if (const auto* i = std::get_if<std::int64_t>(&value))
    {
        if (variable != nullptr && variable->type == control::ValueType::Enum)
        {
            for (const auto& item : variable->items)
            {
                if (item.value == *i)
                {
                    return item.name + " (" + std::to_string(*i) + ")";
                }
            }
        }
        return std::to_string(*i);
    }
    return number_text(std::get<double>(value));
}

bool holds(const ScenarioComparison& comparison, const control::Value& value)
{
    // Two integers are compared exactly; anything else as doubles.
    const auto* a = std::get_if<std::int64_t>(&value);
    const auto* b = std::get_if<std::int64_t>(&comparison.value);
    const bool exact = a != nullptr && b != nullptr;
    const double x = as_number(value);
    const double y = as_number(comparison.value);
    switch (comparison.op)
    {
    case Compare::Eq: return exact ? *a == *b : x == y;
    case Compare::Ne: return exact ? *a != *b : x != y;
    case Compare::Ge: return exact ? *a >= *b : x >= y;
    case Compare::Le: return exact ? *a <= *b : x <= y;
    case Compare::Gt: return exact ? *a > *b : x > y;
    case Compare::Lt: return exact ? *a < *b : x < y;
    case Compare::Near: return std::fabs(x - y) <= comparison.tolerance;
    }
    return false;
}

ScenarioCatalog scenario_catalog(const CheckReport& check)
{
    ScenarioCatalog out;
    if (!check.config)
    {
        return out;
    }
    for (std::size_t i = 0; i < check.config->participants.size(); ++i)
    {
        const ParticipantConfig& p = check.config->participants[i];
        CatalogParticipant c;
        c.name = p.name;
        const ParticipantResult* result = i < check.participants.size() ? &check.participants[i] : nullptr;
        if (!p.enabled && p.left_out_by_variant && !check.config->variant.empty())
        {
            c.not_in_variant = true;
        }
        else if (!p.enabled)
        {
            c.unusable = "is left out of this run (enabled: false, or --disable): take it in (--enable " + p.name +
                         "), or run a scenario that does not use it";
        }
        else if (!p.describe)
        {
            c.unusable = "has describe: false, so the launcher does not know its variables: a scenario can use the variables "
                         "of ecu_main executables only";
        }
        else if (result == nullptr || result->status != ParticipantStatus::Described || !result->ecu)
        {
            c.unusable = "could not be described (the errors of the configuration say why), so its variables are not known";
        }
        else
        {
            const EcuDescription& d = result->ecu->description;
            // As the run's hub shows them.
            auto seen = std::make_shared<const control::MappedSchema>(seen_variables(p, *result->ecu));
            c.variables = seen->schema();
            for (const control::VariableMapping& mapping : p.variables)
            {
                if (mapping.absent)
                {
                    c.absent.push_back(mapping.name);
                }
            }
            if (!p.variables.empty())
            {
                c.map = std::move(seen);
            }
            if (!d.step)
            {
                c.unusable = "runs in real time (it has no step): a scenario needs the virtual time of its steps";
            }
            else if (!d.variables.empty() && !d.control)
            {
                c.unusable = "does not offer its variables to the simulation (--no-control, or an executable of an earlier "
                             "vehicle-sil): build it with this vehicle-sil and run it without --no-control";
            }
            else
            {
                c.step = *d.step;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

ScenarioRead read_scenario(std::string_view text, const ScenarioCatalog& catalog, const ScenarioContext& context)
{
    ScenarioRead out;
    if (text.substr(0, 2) == "\xFF\xFE" || text.substr(0, 2) == "\xFE\xFF" || text.find('\0') != std::string_view::npos)
    {
        out.findings.push_back(Finding{Severity::Error, "encoding",
                                       "the file is not UTF-8 text (it looks like UTF-16, or has NUL bytes): save it as UTF-8", 0,
                                       "", ""});
        return out;
    }
    try
    {
        const detail::YamlDocument doc(text);
        if (doc.empty())
        {
            out.findings.push_back(Finding{Severity::Error, "not-a-scenario",
                                           std::string("the file is empty: write vehicle_sil_scenario: 1 and steps (") + kDocs + ")",
                                           0, "", ""});
            return out;
        }
        Reader(doc, catalog, context, out).read();
    }
    catch (const detail::YamlSyntaxError& e)
    {
        out.findings.clear();
        // "{ ge: ${idle_rpm} }" is what one writes first, and YAML does not take it.
        const bool param = e.context().find("${") != std::string::npos;
        out.findings.push_back(Finding{Severity::Error, "syntax",
                                       "syntax error: " + e.message() +
                                           (param ? ": inside { ... } or [ ... ], write a parameter in quotes (\"${name}\")" : ""),
                                       e.line(), "", "", e.context()});
    }
    return out;
}

} // namespace vehicle_sil::launch
