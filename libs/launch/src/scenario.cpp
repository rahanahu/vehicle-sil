// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/scenario.hpp"

#include "vehicle_sil/launch/check.hpp"
#include "scenario_reader.hpp"
#include "seen_ecu.hpp"
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

namespace vehicle_sil::launch::scenario_reader {

namespace {

constexpr const char* kDocs = "docs/scenario.md";

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

} // namespace

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

void Reader::read()
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
    ryml::id_type inputsNode = ryml::NONE;
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
        else if (key == "inputs")
        {
            inputsNode = child;
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
    // The inputs before the steps, wherever the file has them: what they declare is there
    // from the start. (Both are looked through for parameters before either is read.)
    const bool inputsOk = inputsNode == ryml::NONE || out_.not_for_variant || resolve_params(inputsNode);
    if (hasSteps)
    {
        if (!out_.not_for_variant && resolve_params(stepsNode) && inputsOk)
        {
            if (inputsNode != ryml::NONE)
            {
                inputs_written_ = true;
                read_inputs(inputsNode);
            }
            declared_ = dithers_.size(); // (the steps' dithers come after those of "inputs")
            read_steps(stepsNode);
            check_dithers();
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
              "the file has no steps: add steps, a list of what to do in order (set, ramp, wave, dither, wait_until, expect)");
    }
    if (std::none_of(out_.findings.begin(), out_.findings.end(), [](const Finding& f) { return f.severity == Severity::Error; }))
    {
        out_.scenario = std::move(scenario_);
    }
}

void Reader::take_params(ryml::id_type node)
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

bool Reader::resolve_params(ryml::id_type node)
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

void Reader::check_given_params()
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

void Reader::check_something_is_checked()
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

const ParamValue* Reader::param_at(ryml::id_type node) const
{
    const auto it = substituted_.find(node);
    return it == substituted_.end() ? nullptr : &params_[it->second].value;
}

std::string Reader::text_of(ryml::id_type node) const
{
    const ParamValue* param = param_at(node);
    return param != nullptr ? param->text : detail::to_string(tree_.val(node));
}

bool Reader::quoted(ryml::id_type node) const
{
    const ParamValue* param = param_at(node);
    return param != nullptr ? param->quoted : is_scalar(node) && tree_.is_val_quoted(node);
}

bool Reader::plain_integer(ryml::id_type node, std::int64_t& value) const
{
    const ParamValue* param = param_at(node);
    return param != nullptr ? !param->quoted && read_integer(param->text, value) : YamlReader::plain_integer(node, value);
}

bool Reader::plain_number(ryml::id_type node, double& value) const
{
    const ParamValue* param = param_at(node);
    return param != nullptr ? !param->quoted && read_number(param->text, value) : YamlReader::plain_number(node, value);
}

std::string Reader::shown_value(ryml::id_type node) const
{
    const ParamValue* param = param_at(node);
    if (param == nullptr)
    {
        return YamlReader::shown_value(node);
    }
    return (param->quoted ? "the text \"" + param->text + "\"" : "'" + param->text + "'") + " (the parameter " + param->name +
           ", from " + param_source_text(param->source) + ")";
}

void Reader::read_only(ryml::id_type node)
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

std::optional<std::int64_t> Reader::millis(ryml::id_type node, const std::string& key, std::int64_t least, const std::string& meaning)
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

void Reader::read_steps(ryml::id_type node)
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

void Reader::read_step(ryml::id_type node)
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
    step_bases_.clear();
    step_dithers_.clear();
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
                : action == "dither" ? StepKind::Dither
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
    only_with("undersampled", step.kind == StepKind::Wave || step.kind == StepKind::Dither, "wave and dither");
    bool undersampledOk = false;
    if (nodes.count("undersampled") > 0)
    {
        const ryml::id_type said = nodes.at("undersampled");
        undersampledOk = is_scalar(said) && !is_null(said) && text_of(said) == "ok";
        if (!undersampledOk)
        {
            error("value", doc_.line(said),
                  std::string("undersampled must be ok (run the step's ") + (step.kind == StepKind::Dither ? "dithers" : "waves") +
                      " as they are, also where they change faster than their ECU steps), not " + shown_value(said) + ": write undersampled: ok, or remove it");
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
    case StepKind::Dither: read_dither(nodes.at("dither"), step, undersampledOk); break;
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
    if (step.skipped.empty())
    {
        note_lines(step, time);
    }
    scenario_.steps.push_back(std::move(step));
}

void Reader::skip(ScenarioStep& step)
{
    step.skipped = skip_reason();
    step.writes.clear();
    step.motions.clear();
    step.overlays.clear();
    step.conditions.clear();
    ++out_.skipped;
    add(Severity::Note, "scenario-step-skipped", step.line,
        "the step is skipped (" + step.text + "): " + step.skipped +
            "; the other steps run, also those that would need what this one does");
}

std::string Reader::skip_reason() const
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

void Reader::read_set(ryml::id_type node, ScenarioStep& step, const std::optional<std::int64_t>& time)
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
            const ScenarioVariable& var = scenario_.variables[*v];
            step.writes.push_back(ScenarioWrite{*v, *value, var.text + " = " + value_text(*value, &var)});
        }
    }
}

std::int64_t Reader::skipped_over_ms(ryml::id_type node)
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

std::int64_t Reader::read_ramp(ryml::id_type node, ScenarioStep& step, const std::optional<std::int64_t>& time)
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
        ScenarioMotion ramp;
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
                    note_base(*v, d, doc_.line(part), true);
                }
                else
                {
                    ramp.base = d;
                    note_base(*v, d, doc_.line(part), false);
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
            ramp.described.type = "ramp";
            ramp.described.values = {{"to", to}, {"over_ms", ramp.length_ms}};
            ramp.text = var.text + (ramp.base ? " from " + number_text(*ramp.base) : "") + " to " + number_text(to) + " over " +
                        std::to_string(ramp.length_ms) + " ms";
            step.motions.push_back(std::move(ramp));
        }
    }
    return longest;
}

void Reader::read_conditions(ryml::id_type node, ScenarioStep& step, const std::string& action)
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

std::optional<Compare> Reader::compare_of(const std::string& key)
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

std::string Reader::condition_text(const ScenarioVariable& var, const ScenarioCondition& condition)
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

std::string Reader::step_text(const ScenarioStep& step) const
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
        out += sep() + (w.text.empty() ? var.text + " = " + value_text(w.value, &var) : w.text);
    }
    for (const auto& r : step.motions)
    {
        out += sep() + r.text;
    }
    for (const auto& o : step.overlays)
    {
        out += sep() + o.text;
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

} // namespace vehicle_sil::launch::scenario_reader

namespace vehicle_sil::launch {

using scenario_reader::as_number;
using scenario_reader::kDocs;
using scenario_reader::number_text;
using scenario_reader::Reader;

const char* step_kind_name(StepKind kind)
{
    switch (kind)
    {
    case StepKind::Set: return "set";
    case StepKind::Ramp: return "ramp";
    case StepKind::Wave: return "wave";
    case StepKind::Expect: return "expect";
    case StepKind::WaitUntil: return "wait_until";
    case StepKind::Dither: return "dither";
    }
    return "set";
}

const char* line_time_source_name(ScenarioLineText::TimeSource source)
{
    switch (source)
    {
    case ScenarioLineText::TimeSource::Text: break;
    case ScenarioLineText::TimeSource::AfterWait: return "after_wait";
    case ScenarioLineText::TimeSource::FirstWrite: return "first_write";
    }
    return "text";
}

std::vector<ScenarioLineText> scenario_inputs_text(const Scenario& scenario)
{
    std::vector<ScenarioLineText> out;
    for (const ScenarioOverlay& input : scenario.inputs)
    {
        ScenarioLineText line;
        line.variable = scenario.variables[input.variable].text;
        line.kind = "dither";
        line.line = input.line;
        line.time_source = ScenarioLineText::TimeSource::FirstWrite;
        line.text = input.text;
        line.shape = input.described;
        out.push_back(std::move(line));
    }
    return out;
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
            const SeenEcu d(p, *result->ecu);
            // As the run's hub shows them.
            auto seen = std::make_shared<const control::MappedSchema>(d.variables());
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
            if (!d.step())
            {
                c.unusable = "runs in real time (it has no step): a scenario needs the virtual time of its steps";
            }
            else if (d.has_variables() && !d.control())
            {
                c.unusable = "does not offer its variables to the simulation (--no-control, or an executable of an earlier "
                             "vehicle-sil): build it with this vehicle-sil and run it without --no-control";
            }
            else
            {
                c.step = *d.step();
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