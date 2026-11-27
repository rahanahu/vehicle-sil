// SPDX-License-Identifier: MIT
#include "vehicle_sil/scenario/scenario.hpp"

#include "scenario_reader.hpp"
#include "vehicle_sil/config/detail/yaml_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace vehicle_sil::scenario::reader {

namespace {

constexpr const char* kDocs = "docs/scenario.md";

bool is_unicode_whitespace(std::uint32_t cp) noexcept
{
    // Matches Python 3 re \s (all codepoints rejected by \S).
    return cp == 0x0020 || (cp >= 0x0009 && cp <= 0x000D) || (cp >= 0x001C && cp <= 0x001F) || cp == 0x0085 ||
           cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool has_non_whitespace_utf8(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size();)
    {
        const auto b0 = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        if (b0 < 0x80)
        {
            cp = b0;
        }
        else if ((b0 & 0xE0) == 0xC0 && i + 1 < text.size())
        {
            cp = (static_cast<std::uint32_t>(b0 & 0x1F) << 6) |
                 static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3F);
            len = 2;
        }
        else if ((b0 & 0xF0) == 0xE0 && i + 2 < text.size())
        {
            cp = (static_cast<std::uint32_t>(b0 & 0x0F) << 12) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                 static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3F);
            len = 3;
        }
        else if ((b0 & 0xF8) == 0xF0 && i + 3 < text.size())
        {
            cp = (static_cast<std::uint32_t>(b0 & 0x07) << 18) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
                 static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 3]) & 0x3F);
            len = 4;
        }
        else
        {
            return false;
        }
        if (!is_unicode_whitespace(cp))
        {
            return true;
        }
        i += len;
    }
    return false;
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
    const ryml::id_type root = doc().root();
    if (root == ryml::NONE || !tree().is_map(root))
    {
        error("not-a-scenario", root == ryml::NONE ? 0 : doc().line(root),
              std::string("the file is not a scenario: it must be a mapping with vehicle_sil_scenario: 1 and steps (") +
                  kDocs + ")");
        out_.params_incomplete = true;
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
        out_.params_incomplete = true;
        return;
    }
    if (const ryml::id_type second = doc().second_document(); second != ryml::NONE)
    {
        error("unsupported-yaml", doc().line(second),
              "a second document (after ---): a file has one scenario; remove the others or put them in files of "
              "their own");
    }
    bool hasVersion = false;
    bool hasTemplateVersion = false;
    ryml::id_type paramsNode = ryml::NONE;
    ryml::id_type onlyNode = ryml::NONE;
    ryml::id_type stepsNode = ryml::NONE;
    ryml::id_type inputsNode = ryml::NONE;
    ryml::id_type endMsNode = ryml::NONE;
    std::map<std::string, std::size_t> seen;
    for (auto child = tree().first_child(root); child != ryml::NONE; child = tree().next_sibling(child))
    {
        const std::string key = config::detail::to_string(tree().key(child));
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
        else if (key == "vehicle_sil_scenario_template")
        {
            hasTemplateVersion = true;
        }
        else if (key == "vehicle_sil_scenario")
        {
            hasVersion = true;
            std::int64_t version = 0;
            if (!plain_integer(child, version))
            {
                error("format-version", doc().line(child),
                      "vehicle_sil_scenario must be the format version " + std::to_string(kScenarioFormat) + ", not " +
                          shown_value(child) + ": write vehicle_sil_scenario: " + std::to_string(kScenarioFormat));
            }
            else if (version != kScenarioFormat)
            {
                out_.findings.clear();
                error("format-version", doc().line(child),
                      "vehicle_sil_scenario is " + std::to_string(version) + ", but this vehicle_sil_launch reads format " +
                          std::to_string(kScenarioFormat) + " only: use the vehicle-sil that the file was written for");
                out_.params_incomplete = true;
                return;
            }
        }
        else if (key == "name")
        {
            if (!is_scalar(child) || is_null(child))
            {
                error("value", doc().line(child), "name must be a text (what the scenario is about), not " + shown_value(child));
            }
            else
            {
                scenario_.name = config::detail::to_string(tree().val(child));
                out_.name = scenario_.name;
            }
        }
        else if (key == "samples")
        {
            const std::string word = is_scalar(child) && !is_null(child) ? config::detail::to_string(tree().val(child)) : std::string{};
            if (word == scenario_samples_name(ScenarioSamples::EveryStep))
            {
                scenario_.samples = ScenarioSamples::EveryStep;
            }
            else if (word == scenario_samples_name(ScenarioSamples::AsSent))
            {
                scenario_.samples = ScenarioSamples::AsSent;
            }
            else
            {
                error("value", doc().line(child),
                      "samples must be every_step or as_sent, not " + shown_value(child) +
                          ": every_step (the default) makes the ECUs whose values the scenario reads send them after "
                          "every step of theirs; as_sent takes the values as often as each ECU sends them");
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
        else if (key == "end_ms")
        {
            endMsNode = child;
            scenario_.end_ms = millis(child, "end_ms", 0, "the virtual time when the scenario ends in milliseconds");
            out_.end_ms = scenario_.end_ms;
        }
        else
        {
            unknown_key(child, key, "", kTopKeys);
        }
    }
    if (hasTemplateVersion && !hasVersion)
    {
        out_.findings.clear();
        error("not-a-scenario", doc().line(root),
              "this file is a scenario template (vehicle_sil_scenario_template: 1), not a runnable scenario: call it from "
              "a scenario's steps with '- template: ...'");
        out_.params_incomplete = true;
        return;
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
    if (hasSteps && !out_.not_for_variant && resolve_step_params(stepsNode) && inputsOk)
    {
        if (inputsNode != ryml::NONE)
        {
            inputs_written_ = true;
            read_inputs(inputsNode);
        }
        declared_ = dithers_.size(); // (the steps' dithers come after those of "inputs")
        read_steps(stepsNode);
        if (scenario_.end_ms && *scenario_.end_ms < min_end_ms_)
        {
            error("scenario-time", doc().line(endMsNode),
                  "end_ms is " + std::to_string(*scenario_.end_ms) + ", before " +
                      (end_known_ ? "the end of the last step (" : "the earliest possible end of the steps (") +
                      std::to_string(min_end_ms_) + " ms): raise end_ms or shorten the steps");
        }
        check_dithers();
        if (caller_template_params_incomplete_)
        {
            out_.params_incomplete = true;
        }
        else
        {
            check_given_params();
        }
        check_something_is_checked();
    }
    else
    {
        out_.params_incomplete = true;
        if (hasSteps && tree().is_seq(stepsNode))
        {
            out_.steps = static_cast<std::size_t>(tree().num_children(stepsNode));
        }
    }
    for (const Known& known : scope().params)
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
    if (std::none_of(out_.findings.begin(), out_.findings.end(), [](const config::Finding& f) { return f.severity == config::Severity::Error; }))
    {
        out_.scenario = std::move(scenario_);
    }
}

void Reader::take_params(ryml::id_type node)
{
    if (node != ryml::NONE)
    {
        for (config::ParamValue& param : read_params(node, "", config::ParamSource::Scenario))
        {
            scope().params.push_back(Known{std::move(param), true, false});
        }
    }
    for (const config::ParamValue& param : context_.params)
    {
        const auto same =
            std::find_if(scope().params.begin(), scope().params.end(), [&param](const Known& k) { return k.value.name == param.name; });
        if (same == scope().params.end())
        {
            scope().params.push_back(Known{param, false, false});
        }
        else
        {
            same->value = param;
        }
    }
}

void Reader::report_undefined_param(const std::string& name, std::size_t line)
{
    if (!scope().undefined.insert(name).second)
    {
        return; // said once
    }
    std::vector<std::string> names;
    for (const Known& k : scope().params)
    {
        names.push_back(k.value.name);
    }
    const std::string guess = config::detail::closest(name, names);
    const std::string how =
        scopes_.size() <= 1
            ? ": give it a default in the scenario (params: { " + name + ": <value> }), or a value in the "
              "configuration's params, in a variant's params or with --param " + name + "=<value>"
            : ": declare it in the template's parameters (" + name + ": { type: ... }) and pass it from the caller's params";
    error("scenario-param", line,
          "${" + name + "} has no value" + (guess.empty() ? "" : " (did you mean ${" + guess + "}?)") + how);
}

bool Reader::resolve_params(ryml::id_type node)
{
    bool ok = true;
    if (tree().has_key(node))
    {
        const std::string key = config::detail::to_string(tree().key(node));
        if (key.find("${") != std::string::npos)
        {
            error("scenario-param", doc().line(node),
                  "'" + key + "': a parameter (${...}) stands for a value, not for a key or for a variable's name: write "
                              "the name out");
            ok = false;
        }
    }
    if (is_scalar(node))
    {
        const std::string text = is_null(node) ? std::string{} : config::detail::to_string(tree().val(node));
        if (text.find("${") == std::string::npos)
        {
            return ok;
        }
        const std::string name = text.size() > 3 && text.rfind("${", 0) == 0 && text.back() == '}' ? text.substr(2, text.size() - 3) : std::string{};
        if (!config::is_param_name(name))
        {
            error("scenario-param", doc().line(node),
                  "'" + text + "': a parameter is written ${name} (letters, digits and _) and is the whole value: it "
                               "cannot be a part of a text, and there are no expressions; work the value out and give it "
                               "as a parameter of its own");
            return false;
        }
        const auto known = std::find_if(scope().params.begin(), scope().params.end(), [&name](const Known& k) { return k.value.name == name; });
        if (known == scope().params.end())
        {
            report_undefined_param(name, doc().line(node));
            return false;
        }
        known->used = true;
        scope().substituted.emplace(node, static_cast<std::size_t>(known - scope().params.begin()));
        return ok;
    }
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        ok = resolve_params(child) && ok;
    }
    return ok;
}

bool Reader::resolve_step_params(ryml::id_type steps_node)
{
    if (!tree().is_seq(steps_node))
    {
        return resolve_params(steps_node);
    }
    bool ok = true;
    const auto check_key = [&](ryml::id_type node) {
        if (tree().has_key(node))
        {
            const std::string key = config::detail::to_string(tree().key(node));
            if (key.find("${") != std::string::npos)
            {
                error("scenario-param", doc().line(node),
                      "'" + key + "': a parameter (${...}) stands for a value, not for a key or for a variable's name: write "
                                  "the name out");
                ok = false;
            }
        }
    };
    for (auto step = tree().first_child(steps_node); step != ryml::NONE; step = tree().next_sibling(step))
    {
        if (!tree().is_map(step))
        {
            ok = resolve_params(step) && ok;
            continue;
        }
        for (auto field = tree().first_child(step); field != ryml::NONE; field = tree().next_sibling(field))
        {
            const std::string fieldKey = tree().has_key(field) ? config::detail::to_string(tree().key(field)) : std::string{};
            if (fieldKey == "template" || fieldKey == "params")
            {
                check_key(field);
                continue;
            }
            if (fieldKey == "name" && is_scalar(field))
            {
                ok = resolve_interpolated_string(field) && ok;
            }
            else if (fieldKey == "wave" && tree().is_map(field))
            {
                check_key(field);
                for (auto varNode = tree().first_child(field); varNode != ryml::NONE; varNode = tree().next_sibling(varNode))
                {
                    if (!tree().is_map(varNode))
                    {
                        ok = resolve_params(varNode) && ok;
                        continue;
                    }
                    check_key(varNode);
                    for (auto part = tree().first_child(varNode); part != ryml::NONE; part = tree().next_sibling(part))
                    {
                        const std::string partKey = tree().has_key(part) ? config::detail::to_string(tree().key(part)) : std::string{};
                        if (partKey == "points" && tree().is_map(part))
                        {
                            check_key(part);
                            for (auto pf = tree().first_child(part); pf != ryml::NONE; pf = tree().next_sibling(pf))
                            {
                                const std::string pfKey = tree().has_key(pf) ? config::detail::to_string(tree().key(pf)) : std::string{};
                                if ((pfKey == "file" || pfKey == "value" || pfKey == "time") && is_scalar(pf))
                                {
                                    check_key(pf);
                                    ok = resolve_interpolated_string(pf) && ok;
                                }
                                else
                                {
                                    ok = resolve_params(pf) && ok;
                                }
                            }
                        }
                        else
                        {
                            ok = resolve_params(part) && ok;
                        }
                    }
                }
            }
            else
            {
                ok = resolve_params(field) && ok;
            }
        }
    }
    return ok;
}

bool Reader::resolve_interpolated_string_text(const std::string& text, std::size_t line, std::string& out)
{
    out.clear();
    if (text.find("${") == std::string::npos)
    {
        out = text;
        return true;
    }
    bool ok = true;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const std::size_t open = text.find("${", pos);
        if (open == std::string::npos)
        {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, open - pos);
        const std::size_t close = text.find('}', open + 2);
        if (close == std::string::npos)
        {
            error("scenario-param", line,
                  "'" + text + "': unclosed '${' in text: write a parameter as ${name} (letters, digits and _)");
            return false;
        }
        const std::string name = text.substr(open + 2, close - (open + 2));
        if (!config::is_param_name(name))
        {
            error("scenario-param", line,
                  "'" + text + "': '${" + name + "}' is not a parameter: write ${name} with letters, digits and _ (not a digit first)");
            return false;
        }
        const auto known = std::find_if(scope().params.begin(), scope().params.end(), [&name](const Known& k) { return k.value.name == name; });
        if (known == scope().params.end())
        {
            report_undefined_param(name, line);
            ok = false;
        }
        else
        {
            known->used = true;
            out += known->value.text;
        }
        pos = close + 1;
    }
    return ok;
}

bool Reader::resolve_interpolated_string(ryml::id_type node)
{
    if (!is_scalar(node) || is_null(node))
    {
        return true;
    }
    const std::string text = config::detail::to_string(tree().val(node));
    if (text.find("${") == std::string::npos)
    {
        return true;
    }
    std::string out;
    if (!resolve_interpolated_string_text(text, doc().line(node), out))
    {
        return false;
    }
    scope().interpolated.emplace(node, std::move(out));
    return true;
}

void Reader::check_given_params()
{
    if (context_.params_for_many)
    {
        return;
    }
    std::vector<std::string> names;
    for (const Known& k : scope().params)
    {
        if (k.declared || k.used)
        {
            names.push_back(k.value.name);
        }
    }
    for (const Known& k : scope().params)
    {
        if (k.value.source == config::ParamSource::RequestFile && !k.declared && !k.used)
        {
            if (context_.request_file.empty())
            {
                out_.unused_request_params.push_back(k.value.name);
                continue;
            }
            const std::string guess = config::detail::closest(k.value.name, names);
            // The file's mistake, and the file is for this scenario: a usage error like --param.
            out_.usage_errors.push_back(config::Finding{
                config::Severity::Error, "usage",
                  "params " + k.value.name + " in " + context_.request_file + ": the scenario has no parameter " +
                      k.value.name + (guess.empty() ? "" : " (did you mean " + guess + "?)") + ": " +
                      (names.empty() ? std::string("it uses none (a step uses one as ${name})")
                                     : "its parameters are " + config::detail::join(names)) +
                      "; remove it from params in " + context_.request_file + " or correct the name",
                0, "", ""});
        }
        if (k.value.source == config::ParamSource::CommandLine && !k.declared && !k.used)
        {
            const std::string guess = config::detail::closest(k.value.name, names);
            // The command line's mistake, not the file's: a usage error like --disable <no such name>.
            out_.usage_errors.push_back(config::Finding{
                config::Severity::Error, "usage",
                  "--param " + k.value.name + "=" + k.value.text + ": the scenario has no parameter " + k.value.name +
                      (guess.empty() ? "" : " (did you mean " + guess + "?)") + ": " +
                      (names.empty() ? std::string("it uses none (a step uses one as ${name})")
                                     : "its parameters are " + config::detail::join(names)) +
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
    for (const CheckRef& ref : skipped_checks_)
    {
        lines.push_back(short_loc(ref.file, ref.line));
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
    const std::size_t unchecked = unchecked_checks_;
    const std::string oneLoc = one ? loc_text(skipped_checks_.front().file, skipped_checks_.front().line) : std::string{};
    const std::string what =
        unchecked == 0 ? (one ? "its only step that checks (expect or wait_until, " + oneLoc + ") is skipped"
                              : "all its " + std::to_string(lines.size()) + " steps that check (expect or wait_until, lines " +
                                    config::detail::join(lines) + ") are skipped")
        : one ? "its only check (an expect beside a ramp or a wave, " + oneLoc + ") is not checked"
              : "all its " + std::to_string(lines.size()) + " checks (expect, wait_until, or an expect beside a ramp or a wave; lines " +
                    config::detail::join(lines) + ") are " + (unchecked == lines.size() ? "not checked" : "skipped or not checked");
    add(config::Severity::Warning, "scenario-nothing-checked", 0,
        "the scenario checks nothing " + where + ": " + what + ", so it passes whatever the ECUs do (the notes say why " +
            (unchecked == 0 ? "each is skipped). " : "for each). ") +
            (context_.variant.empty()
                 ? std::string("If the configuration has what the steps look at under other names, map them in the participant's variables "
                               "(docs/simulation-config.md); else do not run this scenario with it")
                 : "If the scenario is not for this variant, list the variants it is for in the scenario (only: [" +
                       (others.empty() ? std::string("<variant>") : config::detail::join(others)) +
                       "]); if it is, map what the steps look at to what the variant has (variables in the variant, "
                       "docs/simulation-config.md)"));
}

const config::ParamValue* Reader::param_at(ryml::id_type node) const
{
    const auto it = scope().substituted.find(node);
    return it == scope().substituted.end() ? nullptr : &scope().params[it->second].value;
}

std::string Reader::text_of(ryml::id_type node) const
{
    if (const auto it = scope().interpolated.find(node); it != scope().interpolated.end())
    {
        return it->second;
    }
    const config::ParamValue* param = param_at(node);
    return param != nullptr ? param->text : config::detail::to_string(tree().val(node));
}

bool Reader::quoted(ryml::id_type node) const
{
    if (scope().interpolated.count(node) > 0)
    {
        return true;
    }
    const config::ParamValue* param = param_at(node);
    return param != nullptr ? param->quoted : is_scalar(node) && tree().is_val_quoted(node);
}

bool Reader::plain_integer(ryml::id_type node, std::int64_t& value) const
{
    const config::ParamValue* param = param_at(node);
    return param != nullptr ? !param->quoted && read_integer(param->text, value) : config::detail::YamlReader::plain_integer(node, value);
}

bool Reader::plain_number(ryml::id_type node, double& value) const
{
    const config::ParamValue* param = param_at(node);
    return param != nullptr ? !param->quoted && read_number(param->text, value) : config::detail::YamlReader::plain_number(node, value);
}

std::string Reader::shown_value(ryml::id_type node) const
{
    if (const auto it = scope().interpolated.find(node); it != scope().interpolated.end())
    {
        return "the text \"" + it->second + "\"";
    }
    const config::ParamValue* param = param_at(node);
    if (param == nullptr)
    {
        return config::detail::YamlReader::shown_value(node);
    }
    return (param->quoted ? "the text \"" + param->text + "\"" : "'" + param->text + "'") + " (the parameter " + param->name +
           ", from " + config::param_source_text(param->source) + ")";
}

void Reader::read_only(ryml::id_type node)
{
    const std::size_t line = doc().line(node);
    if (!tree().is_seq(node) || !tree().has_children(node))
    {
        error("value", line,
              "only must be a list of the variants the scenario is for (only: [small, medium]), not " +
                  (tree().is_seq(node) ? std::string("an empty list") : shown_value(node)) +
                  "; remove it for a scenario for every variant");
        return;
    }
    std::vector<std::string> names;
    for (auto item = tree().first_child(node); item != ryml::NONE; item = tree().next_sibling(item))
    {
        if (!is_scalar(item) || is_null(item) || tree().val(item).empty())
        {
            error("value", doc().line(item), "only must list variants by name (only: [small, medium]), not " + shown_value(item));
            return;
        }
        const std::string name = config::detail::to_string(tree().val(item));
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            names.push_back(name);
        }
    }
    out_.only = names;
    if (context_.variants.empty())
    {
        error("scenario-variant", line,
              "only names " + config::detail::join(names) + ", but the configuration defines no variants: remove only, or add variants "
              "to the configuration (docs/simulation-config.md)");
        return;
    }
    for (const std::string& name : names)
    {
        if (std::find(context_.variants.begin(), context_.variants.end(), name) == context_.variants.end())
        {
            const std::string guess = config::detail::closest(name, context_.variants);
            error("scenario-variant", line,
                  "only names the variant '" + name + "', which the configuration does not define" +
                      (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; its variants are " +
                      config::detail::join(context_.variants));
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
              "the scenario is only for the variant" + std::string(names.size() == 1 ? " " : "s ") + config::detail::join(names) +
                  " (its only), not for " + context_.variant + ": run it with " + config::detail::or_list(options) + ", or add " +
                  context_.variant + " to only");
        out_.not_for_variant = true;
    }
}

std::optional<std::int64_t> Reader::millis(ryml::id_type node, const std::string& key, std::int64_t least, const std::string& meaning)
{
    std::int64_t value = 0;
    if (!plain_integer(node, value))
    {
        error("value", doc().line(node),
              key + " must be a whole number (" + meaning + "), not " + shown_value(node) +
                  (quoted(node) ? ": write the number without quotes" : ""));
        return std::nullopt;
    }
    if (value < least || value > config::kMaxMillis)
    {
        error("value", doc().line(node),
              key + " is " + std::to_string(value) + ", but it must be from " + std::to_string(least) + " to " +
                  std::to_string(config::kMaxMillis) + " (" + meaning + ")");
        return std::nullopt;
    }
    return value;
}

void Reader::read_steps(ryml::id_type node, std::optional<InheritedTiming> first_timing, bool* aborted)
{
    if (!tree().is_seq(node) || !tree().has_children(node))
    {
        error("value", doc().line(node),
              "steps must be a list with at least one step (- set: ..., - expect: ...), not " +
                  (tree().is_seq(node) ? std::string("an empty list") : shown_value(node)));
        return;
    }
    bool first = true;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        if (scopes_.size() > 1)
        {
            if (expanded_steps_ >= kMaxExpandedSteps)
            {
                if (!too_many_expanded_steps_)
                {
                    too_many_expanded_steps_ = true;
                    error("scenario-template-steps", doc().line(child),
                          "template expansion exceeded the maximum of " + std::to_string(kMaxExpandedSteps) +
                              " steps: reduce template nesting or step counts");
                }
                caller_template_params_incomplete_ = true;
                end_known_ = false;
                if (aborted != nullptr)
                {
                    *aborted = true;
                }
                return;
            }
            ++expanded_steps_;
        }
        read_step(child, first ? first_timing : std::nullopt);
        first = false;
        if (too_many_expanded_steps_)
        {
            caller_template_params_incomplete_ = true;
            if (aborted != nullptr)
            {
                *aborted = true;
            }
            return;
        }
    }
}

void Reader::read_step(ryml::id_type node, std::optional<InheritedTiming> inherited)
{
    const std::size_t line = doc().line(node);
    if (!tree().is_map(node))
    {
        ++out_.steps;
        error("value", line, "a step must be a mapping with one of " + config::detail::join(kActionKeys) + ", not " + shown_value(node));
        end_known_ = false; // what it would have been is not known
        return;
    }
    ScenarioStep step;
    step.file = current_file();
    step.line = line;
    step.call_stack = active_templates_;
    absent_.clear();
    step_bases_.clear();
    step_dithers_.clear();
    std::map<std::string, std::size_t> seen;
    std::map<std::string, ryml::id_type> nodes;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        const std::string key = config::detail::to_string(tree().key(child));
        if (!first_occurrence(seen, key, child))
        {
            if (key == "template" || key == "params")
            {
                caller_template_params_incomplete_ = true;
            }
            continue;
        }
        if (std::find(kStepKeys.begin(), kStepKeys.end(), key) == kStepKeys.end())
        {
            if (key == "parameters")
            {
                error("unknown-key", doc().line(child),
                      "unknown key 'parameters': pass template arguments with 'params:' (parameters: declares formal parameters inside a template file)");
            }
            else
            {
                unknown_key(child, key, "", kStepKeys);
            }
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
        ++out_.steps;
        if (nodes.count("template") > 0 || nodes.count("params") > 0)
        {
            caller_template_params_incomplete_ = true;
        }
        error("value", line,
              actions.empty() ? "the step does nothing: give it one of " + config::detail::join(kActionKeys)
                              : "the step has " + config::detail::join(actions) + ": a step does one thing; make a step of each");
        end_known_ = false;
        return;
    }
    const std::string& action = actions.front();
    const bool isTemplate = action == "template";
    if (!isTemplate)
    {
        ++out_.steps;
        step.kind = action == "set"      ? StepKind::Set
                    : action == "ramp"   ? StepKind::Ramp
                    : action == "wave"   ? StepKind::Wave
                    : action == "dither" ? StepKind::Dither
                    : action == "expect" ? StepKind::Expect
                                         : StepKind::WaitUntil;
    }

    if (nodes.count("name") > 0)
    {
        const ryml::id_type nameNode = nodes.at("name");
        if (!is_scalar(nameNode) || is_null(nameNode))
        {
            error("value", doc().line(nameNode), "step name must be a non-empty text, not " + shown_value(nameNode));
        }
        else
        {
            const auto it = scope().interpolated.find(nameNode);
            std::string resolved = it != scope().interpolated.end() ? it->second : config::detail::to_string(tree().val(nameNode));
            const auto control = std::find_if(resolved.begin(), resolved.end(), [](char c) {
                return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
            });
            if (control != resolved.end() || !vehicle_sil::detail::is_utf8(resolved))
            {
                char code[8];
                std::snprintf(code, sizeof(code), "%02X", control != resolved.end() ? static_cast<unsigned char>(*control) : 0);
                error("value", doc().line(nameNode),
                      std::string("step name must be text on one line") +
                          (control != resolved.end() ? std::string(" (it has the control character U+00") + code + ")"
                                                     : std::string(" (it is not UTF-8)")) +
                          ": write the name without it");
            }
            else if (!has_non_whitespace_utf8(resolved))
            {
                error("value", doc().line(nameNode), "step name must not be empty or whitespace-only");
            }
            else
            {
                step.name = std::move(resolved);
            }
        }
    }

    // The keys that go with the action.
    const auto only_with = [&](const std::string& key, bool allowed, const std::string& with) {
        if (nodes.count(key) > 0 && !allowed)
        {
            error("value", doc().line(nodes.at(key)), key + " goes with " + with + ", not with " + action + ": remove it");
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
                error("value", doc().line(nodes.at(key)),
                      key + " does not go with an expect beside a " + action + ": that expect must be true all through the " +
                          action + ", from its time to its end; remove " + key + ", or make the expect a step of its own after the " +
                          action);
                nodes.erase(key);
            }
        }
    }
    only_with("timeout_ms", !isTemplate && step.kind == StepKind::WaitUntil, "wait_until");
    only_with("within_ms", !isTemplate && step.kind == StepKind::Expect, "expect");
    only_with("hold_for_ms", !isTemplate && step.kind == StepKind::Expect, "expect");
    only_with("undersampled", !isTemplate && (step.kind == StepKind::Wave || step.kind == StepKind::Dither), "wave and dither");
    if (!isTemplate && nodes.count("params") > 0)
    {
        caller_template_params_incomplete_ = true;
    }
    only_with("params", isTemplate, "template");
    bool undersampledOk = false;
    if (nodes.count("undersampled") > 0)
    {
        const ryml::id_type said = nodes.at("undersampled");
        undersampledOk = is_scalar(said) && !is_null(said) && text_of(said) == "ok";
        if (!undersampledOk)
        {
            error("value", doc().line(said),
                  std::string("undersampled must be ok (run the step's ") + (step.kind == StepKind::Dither ? "dithers" : "waves") +
                      " as they are, also where they change faster than their ECU steps), not " + shown_value(said) + ": write undersampled: ok, or remove it");
        }
    }

    // The time.
    std::optional<std::int64_t> time; // as far as it is known before the run
    const bool hasAt = nodes.count("at_ms") > 0;
    const bool hasAfter = nodes.count("after_ms") > 0;
    bool validCallTime = true;
    if (hasAt && hasAfter)
    {
        error("value", doc().line(nodes.at("after_ms")),
              "the step has at_ms and after_ms: give it one time, at_ms (the virtual time) or after_ms (from the end of "
              "the step before it)");
        validCallTime = false;
    }
    else if (hasAt)
    {
        const auto ownAt = millis(nodes.at("at_ms"), "at_ms", 0, "the virtual time of the step in milliseconds");
        const std::string templateHint =
            scopes_.size() > 1
                ? " (inside a template, at_ms is still the scenario's absolute time; use after_ms for time from the previous step)"
                : "";
        if (!ownAt)
        {
            validCallTime = false;
        }
        else if (inherited && inherited->at_ms && *ownAt < *inherited->at_ms)
        {
            error("scenario-time", doc().line(nodes.at("at_ms")),
                  "at_ms is " + std::to_string(*ownAt) + ", before the template call's at_ms (" +
                      std::to_string(*inherited->at_ms) + " ms)" + templateHint);
            validCallTime = false;
        }
        else if (!end_known_)
        {
            if (wait_line_ > 0)
            {
                error("scenario-time", doc().line(nodes.at("at_ms")),
                      "at_ms after a step that waits (" + loc_text(wait_file_, wait_line_) +
                          "): when that step ends is not known before the run; give this step after_ms (from the end of "
                          "the step before it) instead" +
                          templateHint);
            }
            validCallTime = false;
        }
        else
        {
            const std::int64_t prevEnd = end_ms_ + (inherited ? inherited->after_ms : 0);
            if (*ownAt < prevEnd)
            {
                error("scenario-time", doc().line(nodes.at("at_ms")),
                      "at_ms is " + std::to_string(*ownAt) + ", before the end of the step before it (" +
                          std::to_string(prevEnd) + " ms): the steps run in the order of the file, and time does not go "
                                                    "back; reorder the steps or raise at_ms" +
                          templateHint);
                validCallTime = false;
            }
            else
            {
                step.at_ms = ownAt;
                time = *ownAt;
            }
        }
    }
    else
    {
        std::int64_t ownAfter = 0;
        if (hasAfter)
        {
            ownAfter = millis(nodes.at("after_ms"), "after_ms", 0,
                              "milliseconds of virtual time from the end of the step before it")
                           .value_or(0);
        }
        if (inherited && inherited->at_ms)
        {
            step.at_ms = *inherited->at_ms + ownAfter;
            if (end_known_)
            {
                time = *step.at_ms;
            }
        }
        else
        {
            step.after_ms = (inherited ? inherited->after_ms : 0) + ownAfter;
            if (end_known_)
            {
                time = end_ms_ + step.after_ms;
            }
        }
    }

    if (isTemplate)
    {
        std::optional<InheritedTiming> callTiming;
        if (validCallTime)
        {
            callTiming = InheritedTiming{step.at_ms, step.after_ms};
        }
        read_template_step(nodes, step.name, callTiming, validCallTime, line);
        return;
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
            error("value", doc().line(nodes.at("hold_for_ms")),
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
        checks_.push_back(CheckRef{step.file, line});
    }
    if (unchecked_)
    {
        step.expect_skipped = skip_reason();
        step.conditions.clear();
        skipped_checks_.push_back(CheckRef{step.file, line});
        ++unchecked_checks_;
        ++out_.unchecked_expects;
        add(config::Severity::Note, "scenario-step-skipped", step.line,
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
            skipped_checks_.push_back(CheckRef{step.file, line});
        }
    }
    // The latest and earliest times this step can end.
    min_end_ms_ = (time ? *time : min_end_ms_ + step.after_ms) + length;
    latest_ms_ = (time ? *time : latest_ms_ + step.after_ms) + length + step.within_ms.value_or(0);
    if (latest_ms_ > config::kMaxMillis && !too_long_)
    {
        too_long_ = true;
        error("scenario-time", line,
              "the scenario can last " + std::to_string(latest_ms_) + " ms up to the end of this step, more than the longest "
              "time (" + std::to_string(config::kMaxMillis) + " ms): shorten the times (after_ms, over_ms, for_ms, hold_for_ms, the "
              "timeouts) of the steps up to here");
    }
    if (step.waits() || step.kind == StepKind::WaitUntil)
    {
        if (end_known_)
        {
            wait_file_ = current_file();
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
    add(config::Severity::Note, "scenario-step-skipped", step.line,
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
        out = (leftOut.size() == 1 ? "participant " + leftOut.front() + " is" : "the participants " + config::detail::join(leftOut) + " are") +
              " not in the variant " + context_.variant + " (enabled: false in that variant)";
    }
    if (!names.empty())
    {
        out += (out.empty() ? "" : "; ") + config::detail::join(names) + (one ? " is absent: participant " : " are absent: ") +
               (one ? owners.front() + " does not have it" : "the participants " + config::detail::join(owners) + " do not have them") +
               (context_.variant.empty() ? " in this configuration" : " in the variant " + context_.variant) +
               " (absent: true in " + (one ? "its" : "their") + " variables)";
    }
    return out;
}

void Reader::read_set(ryml::id_type node, ScenarioStep& step, const std::optional<std::int64_t>& time)
{
    if (!tree().is_map(node) || !tree().has_children(node))
    {
        error("value", doc().line(node),
              "set must be a mapping of variables to values ({ " + example_variable() + ": 1 }), not " + shown_value(node));
        return;
    }
    std::map<std::string, std::size_t> seen;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        if (!first_occurrence(seen, config::detail::to_string(tree().key(child)), child, "set: "))
        {
            continue;
        }
        const auto v = variable(child);
        if (!v || !writable(*v, doc().line(child), time))
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
    if (!tree().is_map(node))
    {
        return 0;
    }
    for (auto part = tree().first_child(node); part != ryml::NONE; part = tree().next_sibling(part))
    {
        if (config::detail::to_string(tree().key(part)) == "over_ms")
        {
            return millis(part, "over_ms", 1, "milliseconds of virtual time the ramp takes").value_or(0);
        }
    }
    return 0;
}

std::int64_t Reader::read_ramp(ryml::id_type node, ScenarioStep& step, const std::optional<std::int64_t>& time)
{
    std::int64_t longest = 0;
    if (!tree().is_map(node) || !tree().has_children(node))
    {
        error("value", doc().line(node),
              "ramp must be a mapping of variables to { to: <value>, over_ms: <milliseconds> }, not " + shown_value(node));
        return longest;
    }
    std::map<std::string, std::size_t> seen;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        const std::string name = config::detail::to_string(tree().key(child));
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
        const std::size_t line = doc().line(child);
        bool good = writable(*v, line, time, "ramp");
        const ScenarioVariable& var = scenario_.variables[*v];
        if (good && var.type != control::ValueType::Integer && var.type != control::ValueType::Real)
        {
            error("scenario-value", line,
                  "ramp needs a number, and " + var.text + " is " + (var.type == control::ValueType::Bool ? "a bool" : "an enum") +
                      ": use set");
            good = false;
        }
        if (!tree().is_map(child))
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
        for (auto part = tree().first_child(child); part != ryml::NONE; part = tree().next_sibling(part))
        {
            const std::string key = config::detail::to_string(tree().key(part));
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
                    error("scenario-value", doc().line(part),
                          key + " of the ramp of " + var.text + " must be a number, not " + shown_value(part) + hex_hint(part, var.type));
                    good = false;
                }
                else if (!in_range(d, *v))
                {
                    error("scenario-value", doc().line(part),
                          key + " of the ramp of " + var.text + " is " + written(part, d) + ", outside its range (" +
                              range_text(*info_of(*v)) + ")");
                    good = false;
                }
                else if (key == "to")
                {
                    to = d;
                    note_base(*v, d, doc().line(part), true);
                }
                else
                {
                    ramp.base = d;
                    note_base(*v, d, doc().line(part), false);
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
    if (!tree().is_map(node) || !tree().has_children(node))
    {
        error("value", doc().line(node),
              action + " must be a mapping of variables to what they must be ({ " + example_variable() +
                  ": { ge: 1 } }), not " + shown_value(node));
        return;
    }
    std::map<std::string, std::size_t> seen;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        const std::string name = config::detail::to_string(tree().key(child));
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
        if (tree().is_map(child))
        {
            std::optional<double> tolerance;
            std::size_t toleranceLine = 0;
            bool hasNear = false;
            std::map<std::string, std::size_t> keys;
            for (auto part = tree().first_child(child); part != ryml::NONE; part = tree().next_sibling(part))
            {
                const std::string key = config::detail::to_string(tree().key(part));
                if (!first_occurrence(keys, key, part, name + ": "))
                {
                    continue;
                }
                if (key == "tolerance")
                {
                    double d = 0;
                    if (!plain_number(part, d) || d < 0)
                    {
                        error("value", doc().line(part), "tolerance must be a number that is not negative, not " + shown_value(part));
                        good = false;
                    }
                    tolerance = d;
                    toleranceLine = doc().line(part);
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
                    error("scenario-value", doc().line(part),
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
                error("value", hasNear ? doc().line(child) : toleranceLine,
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
                error("value", doc().line(child),
                      name + ": nothing to compare with: give one of " + config::detail::join(kCompareKeys) + ", or the value itself");
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

} // namespace vehicle_sil::scenario::reader

namespace vehicle_sil::scenario {

using reader::as_number;
using reader::kDocs;
using reader::number_text;
using reader::Reader;

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
        line.file = input.file;
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

const char* scenario_samples_name(ScenarioSamples samples)
{
    return samples == ScenarioSamples::AsSent ? "as_sent" : "every_step";
}

std::vector<bool> participants_read(const Scenario& scenario)
{
    std::vector<bool> out(scenario.participants.size(), false);
    // Per variable: an earlier step of the scenario puts a value on it (set, ramp, wave), which is
    // where a motion without a base starts, so that no sample is read. A step the run skips has
    // no writes or motions here, so it does not count.
    std::vector<bool> put(scenario.variables.size(), false);
    const auto reads = [&](std::size_t variable) { out[scenario.variables[variable].participant] = true; };
    for (const ScenarioStep& step : scenario.steps)
    {
        for (const ScenarioCondition& condition : step.conditions)
        {
            reads(condition.variable);
        }
        for (const ScenarioMotion& motion : step.motions)
        {
            if (!motion.base && motion.shape && motion.shape->uses_base() && !put[motion.variable])
            {
                reads(motion.variable);
            }
        }
        // After the step: the motions of one step begin together, so none starts from another's.
        for (const ScenarioWrite& write : step.writes)
        {
            put[write.variable] = true;
        }
        for (const ScenarioMotion& motion : step.motions)
        {
            put[motion.variable] = true;
        }
    }
    return out;
}

ScenarioRead read_scenario(std::string_view text, const ScenarioCatalog& catalog, const ScenarioContext& context)
{
    ScenarioRead out;
    if (text.substr(0, 2) == "\xFF\xFE" || text.substr(0, 2) == "\xFE\xFF" || text.find('\0') != std::string_view::npos)
    {
        out.findings.push_back(config::Finding{config::Severity::Error, "encoding",
                                       "the file is not UTF-8 text (it looks like UTF-16, or has NUL bytes): save it as UTF-8", 0,
                                       "", ""});
        out.params_incomplete = true;
        return out;
    }
    try
    {
        const config::detail::YamlDocument doc(text);
        if (doc.empty())
        {
            out.findings.push_back(config::Finding{config::Severity::Error, "not-a-scenario",
                                           std::string("the file is empty: write vehicle_sil_scenario: 1 and steps (") + kDocs + ")",
                                           0, "", ""});
            out.params_incomplete = true;
            return out;
        }
        Reader(doc, catalog, context, out).read();
    }
    catch (const config::detail::YamlSyntaxError& e)
    {
        out.findings.clear();
        // "{ ge: ${idle_rpm} }" is what one writes first, and YAML does not take it.
        const bool param = e.context().find("${") != std::string::npos;
        out.findings.push_back(config::Finding{config::Severity::Error, "syntax",
                                       "syntax error: " + e.message() +
                                           (param ? ": inside { ... } or [ ... ], write a parameter in quotes (\"${name}\")" : ""),
                                       e.line(), "", "", e.context()});
        out.params_incomplete = true;
    }
    return out;
}

} // namespace vehicle_sil::scenario