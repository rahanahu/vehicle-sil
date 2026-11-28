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
#include <limits>
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

int param_source_rank(config::ParamSource source) noexcept
{
    switch (source)
    {
    case config::ParamSource::Scenario: return 0;
    case config::ParamSource::Configuration: return 1;
    case config::ParamSource::Variant: return 2;
    case config::ParamSource::Case: return 3;
    case config::ParamSource::RequestFile: return 4;
    case config::ParamSource::CommandLine: return 5;
    case config::ParamSource::TemplateDefault: return 6;
    case config::ParamSource::TemplateCall: return 7;
    }
    return 0;
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
    ryml::id_type casesNode = ryml::NONE;
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
        else if (key == "cases")
        {
            casesNode = child;
            cases_written_ = true;
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
    if (!hasVersion)
    {
        error("missing-key", 0,
              "the file has no vehicle_sil_scenario (the format version): add vehicle_sil_scenario: " +
                  std::to_string(kScenarioFormat));
    }
    const bool hasSteps = stepsNode != ryml::NONE;
    if (!hasSteps)
    {
        error("missing-key", 0,
              "the file has no steps: add steps, a list of what to do in order (set, ramp, wave, dither, wait_until, expect)");
    }
    if (casesNode != ryml::NONE)
    {
        read_cases(casesNode);
        if (cases_incomplete_)
        {
            out_.params_incomplete = true;
        }
    }
    const bool hasTopLevelErrors =
        !hasVersion || !hasSteps ||
        std::any_of(out_.findings.begin(), out_.findings.end(),
                    [](const config::Finding& f) { return f.severity == config::Severity::Error; });
    bool skipReading = false;
    const CaseDef* chosenCase = resolve_case_selection(casesNode, hasTopLevelErrors, skipReading);
    // The steps last, wherever the file has them: they use the parameters, and are read only
    // for a variant the scenario is for.
    take_params(paramsNode, chosenCase != nullptr ? &chosenCase->params : nullptr);
    resolve_only_and_case(onlyNode, chosenCase);
    // The inputs before the steps, wherever the file has them: what they declare is there
    // from the start. (Both are looked through for parameters before either is read.)
    const bool inputsOk = inputsNode == ryml::NONE || out_.not_for_variant || skipReading || resolve_params(inputsNode);
    if (hasSteps && !skipReading && !out_.not_for_variant && resolve_step_params(stepsNode) && inputsOk)
    {
        if (inputsNode != ryml::NONE)
        {
            inputs_written_ = true;
            read_inputs(inputsNode);
        }
        declared_ = dithers_.size(); // (the steps' dithers come after those of "inputs")
        collect_cli_string_params(stepsNode);
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
        if (hasSteps && !out_.not_for_variant && (!skipReading || !cases_.empty()))
        {
            scan_metadata_only(inputsNode, stepsNode);
        }
    }
    std::set<std::string> emittedMeta;
    const auto push_meta = [&](const std::string& name) {
        if (!emittedMeta.insert(name).second)
        {
            return;
        }
        const auto it = param_constraints_.find(name);
        if (it == param_constraints_.end() || !it->second.template_type || it->second.conflict_reported)
        {
            return;
        }
        const ParamConstraint& c = it->second;
        ScenarioParamMeta meta;
        meta.name = name;
        meta.type = param_type_name(*c.template_type);
        if (c.has_values)
        {
            meta.values.reserve(c.allowed_values.size());
            for (const config::ParamValue& v : c.allowed_values)
            {
                meta.values.push_back(v.text);
            }
        }
        meta.description = c.description;
        out_.param_meta.push_back(std::move(meta));
    };
    for (Known& known : scope().params)
    {
        if (known.declared || known.used)
        {
            if (known.value.from_cli)
            {
                const auto cit = param_constraints_.find(known.value.name);
                if (cit != param_constraints_.end() && cit->second.template_type &&
                    *cit->second.template_type == ParamType::String && !cit->second.conflict_reported)
                {
                    known.value.quoted = true;
                }
            }
            push_meta(known.value.name);
            out_.params.push_back(known.value);
        }
    }
    for (const std::string& name : param_constraint_order_)
    {
        push_meta(name);
    }
    if (!skipReading &&
        std::none_of(out_.findings.begin(), out_.findings.end(), [](const config::Finding& f) { return f.severity == config::Severity::Error; }))
    {
        scenario_.params = out_.params;
        out_.scenario = std::move(scenario_);
    }
}

void Reader::take_params(ryml::id_type node, const std::vector<config::ParamValue>* case_params)
{
    if (node != ryml::NONE)
    {
        for (config::ParamValue& param : read_params(node, "", config::ParamSource::Scenario))
        {
            const std::string rootName = param.name;
            scope().params.push_back(Known{std::move(param), true, false, rootName});
        }
    }
    const auto merge_param = [this](const config::ParamValue& param) {
        const auto same =
            std::find_if(scope().params.begin(), scope().params.end(), [&param](const Known& k) { return k.value.name == param.name; });
        if (same == scope().params.end())
        {
            scope().params.push_back(Known{param, false, false, param.name});
        }
        else if (param_source_rank(param.source) >= param_source_rank(same->value.source))
        {
            same->value = param;
        }
    };
    for (const config::ParamValue& param : context_.params)
    {
        merge_param(param);
    }
    if (case_params != nullptr)
    {
        for (const config::ParamValue& param : *case_params)
        {
            merge_param(param);
        }
    }
}

void Reader::report_undefined_param(const std::string& name, std::size_t line)
{
    if (scopes_.size() <= 1)
    {
        add_missing_param(name);
    }
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
            else if ((fieldKey == "expect" || fieldKey == "wait_until") && tree().is_map(field))
            {
                check_key(field);
                const auto check_var_ref = [&](ryml::id_type varNode) {
                    check_key(varNode);
                    if (is_scalar(varNode) && !is_null(varNode))
                    {
                        const std::string raw = config::detail::to_string(tree().val(varNode));
                        if (raw.find("${") != std::string::npos)
                        {
                            error("scenario-param", doc().line(varNode),
                                  "'" + raw + "': a parameter (${...}) stands for a value, not for a variable's name: write "
                                              "the name out");
                            ok = false;
                        }
                    }
                };
                for (auto cond = tree().first_child(field); cond != ryml::NONE; cond = tree().next_sibling(cond))
                {
                    if (!tree().is_map(cond))
                    {
                        ok = resolve_params(cond) && ok;
                        continue;
                    }
                    check_key(cond);
                    for (auto part = tree().first_child(cond); part != ryml::NONE; part = tree().next_sibling(part))
                    {
                        const std::string partKey = tree().has_key(part) ? config::detail::to_string(tree().key(part)) : std::string{};
                        if (partKey == "variable")
                        {
                            check_var_ref(part);
                        }
                        else if (compare_of(partKey).has_value() && tree().is_map(part))
                        {
                            check_key(part);
                            for (auto sub = tree().first_child(part); sub != ryml::NONE; sub = tree().next_sibling(sub))
                            {
                                const std::string subKey = tree().has_key(sub) ? config::detail::to_string(tree().key(sub)) : std::string{};
                                if (subKey == "variable")
                                {
                                    check_var_ref(sub);
                                }
                                else
                                {
                                    ok = resolve_params(sub) && ok;
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
    if (out_.not_for_variant)
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
    for (const CaseDef& c : cases_)
    {
        for (const config::ParamValue& cp : c.params)
        {
            if (std::find(names.begin(), names.end(), cp.name) != names.end())
            {
                continue;
            }
            const std::string guess = config::detail::closest(cp.name, names);
            error("scenario-param", cp.line,
                  "case '" + c.name + "': parameter '" + cp.name + "' is not declared in params or used in steps" +
                      (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + ": " +
                      (names.empty() ? std::string("the scenario uses no parameters (a step uses one as ${name})")
                                     : "its parameters are " + config::detail::join(names)) +
                      "; remove it or declare it in params");
        }
    }
    if (context_.params_for_many)
    {
        return;
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

void Reader::read_cases(ryml::id_type node)
{
    const std::size_t line = doc().line(node);
    if (!tree().is_map(node) || !tree().has_children(node))
    {
        error("value", line,
              "cases must be a non-empty mapping of case names to { params, only }, not " +
                  (tree().is_map(node) ? std::string("an empty mapping") : shown_value(node)));
        cases_incomplete_ = true;
        return;
    }
    std::map<std::string, std::size_t> seen;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        CaseDef def;
        def.name = config::detail::to_string(tree().key(child));
        def.line = doc().line(child);
        if (!first_occurrence(seen, def.name, child, "cases: "))
        {
            cases_incomplete_ = true;
            continue;
        }
        const auto control = std::find_if(def.name.begin(), def.name.end(), [](char c) {
            return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
        });
        if (def.name.empty() || def.name.rfind("--", 0) == 0 || control != def.name.end() ||
            !vehicle_sil::detail::is_utf8(def.name))
        {
            error("value", def.line,
                  "cases: a case name must be non-empty UTF-8 text on one line that does not start with '--', not '" +
                      def.name + "'");
            cases_incomplete_ = true;
            continue;
        }
        if (!tree().is_map(child))
        {
            error("value", def.line,
                  "case '" + def.name + "' must be a mapping ({ params: ..., only: ... } or {}), not " + shown_value(child));
            cases_incomplete_ = true;
        }
        else
        {
            std::map<std::string, std::size_t> caseSeen;
            for (auto prop = tree().first_child(child); prop != ryml::NONE; prop = tree().next_sibling(prop))
            {
                const std::string propKey = config::detail::to_string(tree().key(prop));
                const std::string prefix = "case '" + def.name + "': ";
                if (!first_occurrence(caseSeen, propKey, prop, prefix))
                {
                    continue;
                }
                if (propKey == "params")
                {
                    const std::size_t beforeFindings = out_.findings.size();
                    def.params = read_params(prop, prefix, config::ParamSource::Case);
                    if (std::any_of(out_.findings.begin() + static_cast<std::ptrdiff_t>(beforeFindings),
                                    out_.findings.end(),
                                    [](const config::Finding& f) { return f.severity == config::Severity::Error; }))
                    {
                        cases_incomplete_ = true;
                    }
                }
                else if (propKey == "only")
                {
                    def.only_line = doc().line(prop);
                    def.only = read_only_list(prop, prefix);
                }
                else
                {
                    unknown_key(prop, propKey, prefix, kCaseKeys);
                }
            }
        }
        out_.cases.push_back(def.name);
        scenario_.cases.push_back(def.name);
        cases_.push_back(std::move(def));
    }
}

const Reader::CaseDef* Reader::resolve_case_selection(ryml::id_type casesNode, bool hasTopLevelErrors, bool& skipReading)
{
    skipReading = false;
    if (casesNode != ryml::NONE)
    {
        if (cases_.empty())
        {
            skipReading = true;
            out_.params_incomplete = true;
            return nullptr;
        }
        if (!context_.case_name)
        {
            out_.usage_errors.push_back(config::Finding{
                config::Severity::Error, "usage",
                "the scenario defines cases, and a check or a run is of one of them: give --case <name> with one of " +
                    config::detail::join(out_.cases) + " (none of them is the default)",
                0, "", ""});
            skipReading = true;
            out_.params_incomplete = true;
            return nullptr;
        }
        const auto it = std::find_if(cases_.begin(), cases_.end(),
                                     [this](const CaseDef& c) { return c.name == *context_.case_name; });
        if (it == cases_.end())
        {
            const std::string guess = config::detail::closest(*context_.case_name, out_.cases);
            const std::string caseRef = context_.case_request_file.empty()
                                            ? "--case " + *context_.case_name
                                            : "case " + *context_.case_name + " in " + context_.case_request_file;
            out_.usage_errors.push_back(config::Finding{
                config::Severity::Error, "usage",
                caseRef + ": the scenario has no case with this name" +
                    (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; its cases are " +
                    config::detail::join(out_.cases) +
                    (hasTopLevelErrors ? "; the file has errors too, which a check with one of these cases reports" : ""),
                0, "", ""});
            skipReading = true;
            out_.params_incomplete = true;
            return nullptr;
        }
        out_.case_name = it->name;
        scenario_.case_name = it->name;
        return &*it;
    }
    if (context_.case_name && !hasTopLevelErrors)
    {
        const std::string caseRef = context_.case_request_file.empty()
                                        ? "--case " + *context_.case_name
                                        : "case " + *context_.case_name + " in " + context_.case_request_file;
        const std::string leaveOut = context_.case_request_file.empty()
                                         ? "leave --case out"
                                         : "remove case from " + context_.case_request_file;
        out_.usage_errors.push_back(config::Finding{
            config::Severity::Error, "usage",
            caseRef + ": the scenario has no cases: " + leaveOut + ", or add cases to the scenario",
            0, "", ""});
        skipReading = true;
        out_.params_incomplete = true;
    }
    return nullptr;
}

std::optional<std::vector<std::string>> Reader::read_only_list(ryml::id_type node, const std::string& prefix)
{
    const std::size_t line = doc().line(node);
    if (!tree().is_seq(node) || !tree().has_children(node))
    {
        error("value", line,
              prefix + "only must be a list of the variants " +
                  (prefix.empty() ? "the scenario" : "the case") + " is for (only: [small, medium]), not " +
                  (tree().is_seq(node) ? std::string("an empty list") : shown_value(node)) +
                  "; remove it for " + (prefix.empty() ? "a scenario for every variant" : "every variant"));
        return std::nullopt;
    }
    std::vector<std::string> names;
    for (auto item = tree().first_child(node); item != ryml::NONE; item = tree().next_sibling(item))
    {
        if (!is_scalar(item) || is_null(item) || tree().val(item).empty())
        {
            error("value", doc().line(item),
                  prefix + "only must list variants by name (only: [small, medium]), not " + shown_value(item));
            return std::nullopt;
        }
        const std::string name = config::detail::to_string(tree().val(item));
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            names.push_back(name);
        }
    }
    return names;
}

void Reader::resolve_only_and_case(ryml::id_type onlyNode, const CaseDef* chosenCase)
{
    std::optional<std::vector<std::string>> scenarioOnly;
    const std::size_t scenarioOnlyLine = onlyNode != ryml::NONE ? doc().line(onlyNode) : 0;
    if (onlyNode != ryml::NONE)
    {
        scenarioOnly = read_only_list(onlyNode, "");
    }
    out_.case_details.clear();
    out_.case_details.reserve(cases_.size());
    for (const CaseDef& c : cases_)
    {
        ScenarioCaseDetail detail;
        detail.name = c.name;
        if (scenarioOnly && c.only)
        {
            std::vector<std::string> inter;
            for (const std::string& v : *scenarioOnly)
            {
                if (std::find(c.only->begin(), c.only->end(), v) != c.only->end())
                {
                    inter.push_back(v);
                }
            }
            detail.only = std::move(inter);
        }
        else if (c.only)
        {
            detail.only = c.only;
        }
        else if (scenarioOnly)
        {
            detail.only = scenarioOnly;
        }
        out_.case_details.push_back(std::move(detail));
    }
    const bool hasCaseOnly = chosenCase != nullptr && chosenCase->only.has_value();
    if (!scenarioOnly && !hasCaseOnly)
    {
        // Still validate other cases' raw `only` against `context_.variants` below if any case defines `only`.
        bool anyOtherOnly = false;
        for (const CaseDef& c : cases_)
        {
            if (c.only)
            {
                anyOtherOnly = true;
                break;
            }
        }
        if (!anyOtherOnly)
        {
            return;
        }
    }
    if (context_.variants.empty())
    {
        if (scenarioOnly)
        {
            out_.only = scenarioOnly;
            error("scenario-variant", scenarioOnlyLine,
                  "only names " + config::detail::join(*scenarioOnly) +
                      ", but the configuration defines no variants: remove only, or add variants to the configuration "
                      "(docs/simulation-config.md)");
        }
        for (const CaseDef& c : cases_)
        {
            if (c.only)
            {
                if (!out_.only && chosenCase == &c)
                {
                    out_.only = c.only;
                }
                error("scenario-variant", c.only_line,
                      "case '" + c.name + "': only names " + config::detail::join(*c.only) +
                          ", but the configuration defines no variants: remove only, or add variants to the configuration "
                          "(docs/simulation-config.md)");
            }
        }
        return;
    }
    if (scenarioOnly)
    {
        for (const std::string& name : *scenarioOnly)
        {
            if (std::find(context_.variants.begin(), context_.variants.end(), name) == context_.variants.end())
            {
                const std::string guess = config::detail::closest(name, context_.variants);
                error("scenario-variant", scenarioOnlyLine,
                      "only names the variant '" + name + "', which the configuration does not define" +
                          (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; its variants are " +
                          config::detail::join(context_.variants));
            }
        }
    }
    for (const CaseDef& c : cases_)
    {
        if (c.only)
        {
            for (const std::string& name : *c.only)
            {
                if (std::find(context_.variants.begin(), context_.variants.end(), name) == context_.variants.end())
                {
                    const std::string guess = config::detail::closest(name, context_.variants);
                    error("scenario-variant", c.only_line,
                          "case '" + c.name + "': only names the variant '" + name + "', which the configuration does not define" +
                              (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; its variants are " +
                              config::detail::join(context_.variants));
                }
            }
        }
    }
    std::optional<std::vector<std::string>> effectiveOnly;
    std::size_t effectiveLine = scenarioOnlyLine;
    if (scenarioOnly && hasCaseOnly)
    {
        std::vector<std::string> inter;
        for (const std::string& v : *scenarioOnly)
        {
            if (std::find(chosenCase->only->begin(), chosenCase->only->end(), v) != chosenCase->only->end())
            {
                inter.push_back(v);
            }
        }
        if (inter.empty())
        {
            out_.only = std::vector<std::string>{};
            error("scenario-variant", chosenCase->only_line,
                  "case '" + chosenCase->name + "': its only (" + config::detail::join(*chosenCase->only) +
                      ") and the scenario's only (" + config::detail::join(*scenarioOnly) +
                      ") have no variant in common: add a shared variant or widen one of them");
            // out_.not_for_variant remains false per Contract 4 so test.cpp fails loudly rather than skipping.
            return;
        }
        effectiveOnly = std::move(inter);
        effectiveLine = chosenCase->only_line;
    }
    else if (hasCaseOnly)
    {
        effectiveOnly = chosenCase->only;
        effectiveLine = chosenCase->only_line;
    }
    else if (scenarioOnly)
    {
        effectiveOnly = scenarioOnly;
        effectiveLine = scenarioOnlyLine;
    }
    if (!effectiveOnly)
    {
        return;
    }
    out_.only = effectiveOnly;
    if (!context_.variant.empty() &&
        std::find(effectiveOnly->begin(), effectiveOnly->end(), context_.variant) == effectiveOnly->end())
    {
        std::vector<std::string> options;
        for (const std::string& name : *effectiveOnly)
        {
            options.push_back("--variant " + name);
        }
        if (hasCaseOnly)
        {
            error("scenario-variant", effectiveLine,
                  (scenarioOnly ? "case '" + chosenCase->name + "' with the scenario's only" : "case '" + chosenCase->name + "'") +
                      " is only for the variant" + std::string(effectiveOnly->size() == 1 ? " " : "s ") +
                      config::detail::join(*effectiveOnly) + " (its only), not for " + context_.variant + ": run it with " +
                      config::detail::or_list(options) + ", or add " + context_.variant + " to only");
        }
        else
        {
            error("scenario-variant", effectiveLine,
                  "the scenario is only for the variant" + std::string(effectiveOnly->size() == 1 ? " " : "s ") +
                      config::detail::join(*effectiveOnly) + " (its only), not for " + context_.variant + ": run it with " +
                      config::detail::or_list(options) + ", or add " + context_.variant + " to only");
        }
        out_.not_for_variant = true;
    }
}

std::optional<std::int64_t> Reader::millis(ryml::id_type node, const std::string& key, std::int64_t least, const std::string& meaning)
{
    record_direct_param_usage(node, ParamType::Integer);
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
    StepReadState state;
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
    state.beside = beside;
    switch (step.kind)
    {
    case StepKind::Set: read_set(nodes.at("set"), step, state, time); break;
    case StepKind::Ramp: length = read_ramp(nodes.at("ramp"), step, state, time); break;
    case StepKind::Wave: length = read_wave(nodes.at("wave"), step, state, time, undersampledOk); break;
    case StepKind::Dither: read_dither(nodes.at("dither"), step, state, undersampledOk); break;
    case StepKind::Expect:
    case StepKind::WaitUntil:
        read_conditions(nodes.at(action), step, state, action);
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
    state.motion_absent = state.absent.size();
    if (beside)
    {
        read_conditions(nodes.at("expect"), step, state, "expect");
    }
    // Only the expect uses what is absent: the ramp or wave runs, unchecked.
    state.unchecked = beside && state.motion_absent == 0 && !state.absent.empty();
    step.text = step_text(step, state);
    const bool checks = beside || step.kind == StepKind::Expect || step.kind == StepKind::WaitUntil;
    if (checks)
    {
        checks_.push_back(CheckRef{step.file, line});
    }
    if (state.unchecked)
    {
        step.expect_skipped = skip_reason(state.absent);
        step.conditions.clear();
        skipped_checks_.push_back(CheckRef{step.file, line});
        ++unchecked_checks_;
        ++out_.unchecked_expects;
        add(config::Severity::Note, "scenario-step-skipped", step.line,
            "the " + std::string(step_kind_name(step.kind)) + " runs; its expect is not checked (" + step.text +
                "): " + step.expect_skipped);
    }
    else if (!state.absent.empty())
    {
        // It takes the time its text says (length), as in a configuration that has the
        // variables: the times of the steps after it do not depend on what is skipped.
        skip(step, state);
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
        note_lines(step, state, time);
    }
    check_same_time_order(step, time, length);
    scenario_.steps.push_back(std::move(step));
}

void Reader::check_same_time_order(const ScenarioStep& step, std::optional<std::int64_t> time, std::int64_t length)
{
    bool sameGroup = false;
    if (time)
    {
        sameGroup = same_time_active_ && same_time_at_ && *same_time_at_ == *time;
        if (!sameGroup)
        {
            same_time_reads_.clear();
            same_time_warned_.clear();
            same_time_at_ = *time;
            same_time_active_ = true;
        }
    }
    else if (!end_known_ && step.after_ms == 0 && same_time_active_ && !same_time_at_)
    {
        sameGroup = true;
    }
    else
    {
        same_time_reads_.clear();
        same_time_warned_.clear();
        same_time_at_.reset();
        same_time_active_ = !end_known_;
    }

    if (step.skipped.empty())
    {
        // Check writes against earlier reads in the same virtual-time group before adding this
        // step's own conditions, so `ramp`/`wave` with an `expect` beside it does not warn on itself.
        const auto check_write = [&](std::size_t var, std::size_t writeLine) {
            const auto it = same_time_reads_.find(var);
            if (it == same_time_reads_.end() || !same_time_warned_.insert(var).second)
            {
                return;
            }
            const std::string& varText = scenario_.variables[var].text;
            const std::string atText = time ? " (" + std::to_string(*time) + " ms)" : std::string{};
            const std::string readLoc = loc_text(it->second.file, it->second.line);
            add(config::Severity::Warning, "scenario-same-time-order", writeLine > 0 ? writeLine : step.line,
                "'" + varText + "' is written here at the same virtual time" + atText + " as an earlier expect (" +
                    readLoc +
                    "): writes at time T are applied before conditions at time T are evaluated, so the expect sees "
                    "the written value; put the write first or add after_ms/at_ms to separate them",
                scenario_.participants[scenario_.variables[var].participant].name);
        };
        for (const ScenarioWrite& w : step.writes)
        {
            check_write(w.variable, w.line);
        }
        for (const ScenarioMotion& m : step.motions)
        {
            // Motions without an explicit base whose value at t=0 equals the base (such as a ramp
            // without from: or a sine/triangle/sawtooth/lag wave without around:) continue smoothly
            // from the current value at T, so an earlier expect at T does not observe a jump.
            const bool preservesValueAtStart =
                m.shape && m.shape->uses_base() && !m.base.has_value() &&
                m.shape->value(SimDuration{}, 0.0) == 0.0 && m.shape->value(SimDuration{}, 1.0) == 1.0;
            if (!preservesValueAtStart)
            {
                check_write(m.variable, m.line);
            }
        }
        for (const ScenarioOverlay& o : step.overlays)
        {
            check_write(o.variable, o.line);
        }

        const auto note_read = [&](std::size_t var, std::size_t readLine) {
            if (same_time_reads_.find(var) == same_time_reads_.end())
            {
                same_time_reads_.emplace(var, SameTimeRead{step.file, readLine > 0 ? readLine : step.line});
            }
        };
        for (const ScenarioCondition& cond : step.conditions)
        {
            note_read(cond.variable, cond.line);
            for (const ScenarioComparison& cmp : cond.all)
            {
                if (cmp.rhs_variable)
                {
                    note_read(*cmp.rhs_variable, cond.line);
                }
            }
        }
    }

    const std::int64_t stepDuration = !step.skipped.empty() ? step.skipped_ms : length;
    if (stepDuration > 0 || step.waits() || step.kind == StepKind::WaitUntil)
    {
        same_time_reads_.clear();
        same_time_warned_.clear();
        same_time_at_.reset();
        same_time_active_ = false;
    }
}

void Reader::skip(ScenarioStep& step, const StepReadState& state)
{
    step.skipped = skip_reason(state.absent);
    step.writes.clear();
    step.motions.clear();
    step.overlays.clear();
    step.conditions.clear();
    ++out_.skipped;
    add(config::Severity::Note, "scenario-step-skipped", step.line,
        "the step is skipped (" + step.text + "): " + step.skipped +
            "; the other steps run, also those that would need what this one does");
}

std::string Reader::skip_reason(const std::vector<std::string>& absent) const
{
    std::vector<std::string> names;
    std::vector<std::string> owners;
    std::vector<std::string> leftOut; // the participants the variant leaves out, once each
    for (const std::string& name : absent)
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

void Reader::read_set(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::optional<std::int64_t>& time)
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
        const auto v = variable(child, state.absent);
        if (!v || !writable(*v, doc().line(child), time))
        {
            continue;
        }
        if (const auto value = value_of(child, *v, "the value", true, &state))
        {
            const ScenarioVariable& var = scenario_.variables[*v];
            step.writes.push_back(ScenarioWrite{*v, *value, var.text + " = " + value_text(*value, &var), doc().line(child)});
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

std::int64_t Reader::read_ramp(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::optional<std::int64_t>& time)
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
        const std::size_t absentBefore = state.absent.size();
        const auto v = variable(child, state.absent);
        if (!v)
        {
            if (state.absent.size() > absentBefore)
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
        ramp.line = line;
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
                record_direct_param_usage(part, ParamType::Number);
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
                    note_base(state, *v, d, doc().line(part), true);
                }
                else
                {
                    ramp.base = d;
                    note_base(state, *v, d, doc().line(part), false);
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

bool Reader::read_rhs_variable(ryml::id_type var_node, std::size_t lhs_variable, const std::string& what,
                               const std::string& prefix, StepReadState& state, std::optional<std::size_t>& rhs_out,
                               std::string& rhs_text_out)
{
    rhs_out.reset();
    rhs_text_out.clear();
    const std::size_t varLine = doc().line(var_node);
    if (!is_scalar(var_node) || is_null(var_node))
    {
        error("value", varLine,
              prefix + "variable must be <participant>.<variable> (e.g. " + example_variable() + "), not " +
                  shown_value(var_node));
        return false;
    }
    const std::string rawText = config::detail::to_string(tree().val(var_node));
    const bool isQuoted = tree().is_val_quoted(var_node);
    if (param_at(var_node) != nullptr || rawText.find("${") != std::string::npos)
    {
        error("scenario-param", varLine,
              "'" + rawText + "': a parameter (${...}) stands for a value, not for a variable's name: write the name out");
        return false;
    }
    if (!isQuoted && is_yaml12_non_string_literal(rawText))
    {
        error("value", varLine,
              prefix + "variable must be <participant>.<variable> (e.g. " + example_variable() + "), not " +
                  shown_value(var_node));
        return false;
    }
    const std::size_t absentBefore = state.absent.size();
    const auto rhs = variable_by_name(rawText, varLine, state.absent);
    if (!rhs)
    {
        if (state.absent.size() > absentBefore)
        {
            rhs_text_out = rawText;
            return true;
        }
        return !check_catalog_;
    }
    const ScenarioVariable& lhsVar = scenario_.variables[lhs_variable];
    const ScenarioVariable& rhsVar = scenario_.variables[*rhs];
    const std::string pre = what + " of " + lhsVar.text + " (" + type_name(lhsVar.type) + ")";
    if (*rhs == lhs_variable)
    {
        error("scenario-value", varLine,
              pre + " compares " + lhsVar.text + " with itself: name another variable, or write a constant value");
        return false;
    }
    const bool lhsNumeric = lhsVar.type == control::ValueType::Integer || lhsVar.type == control::ValueType::Real;
    const bool rhsNumeric = rhsVar.type == control::ValueType::Integer || rhsVar.type == control::ValueType::Real;
    if (lhsNumeric != rhsNumeric || (!lhsNumeric && lhsVar.type != rhsVar.type))
    {
        error("scenario-value", varLine,
              pre + " cannot compare with " + rhsVar.text + " (" + type_name(rhsVar.type) + "): " +
                  (lhsNumeric ? "an integer or real variable can be compared only with another integer or real variable"
                              : lhsVar.type == control::ValueType::Bool
                                    ? "a bool variable can be compared only with another bool variable"
                                    : "an enum variable can be compared only with another enum variable with the same items"));
        return false;
    }
    if (lhsVar.type == control::ValueType::Enum && !enum_items_match(lhsVar.items, rhsVar.items))
    {
        error("scenario-value", varLine,
              pre + " cannot compare with " + rhsVar.text + ": their enum items do not match (" + lhsVar.text + " has " +
                  items_text(lhsVar.items) + "; " + rhsVar.text + " has " + items_text(rhsVar.items) +
                  "): if their item values match, align item names or shared subsets with variables.<name>.map in the configuration file (docs/simulation-config.md)");
        return false;
    }
    rhs_out = *rhs;
    rhs_text_out = rhsVar.text;
    return true;
}

bool Reader::read_variable_ref_map(ryml::id_type map_node, std::size_t lhs_variable, Compare op, const std::string& lhs_name,
                                   const std::string& key, StepReadState& state, std::optional<std::size_t>& rhs_out,
                                   std::string& rhs_text_out)
{
    rhs_out.reset();
    rhs_text_out.clear();
    const ScenarioVariable& var = scenario_.variables[lhs_variable];
    if (!tree().has_children(map_node))
    {
        error("value", doc().line(map_node),
              key + " of " + var.text + " (" + type_name(var.type) +
                  ") must be a value or { variable: <participant>.<variable> }, not an empty mapping");
        return false;
    }
    ryml::id_type varNode = ryml::NONE;
    ryml::id_type nestedTolNode = ryml::NONE;
    bool ok = true;
    std::map<std::string, std::size_t> refKeys;
    const std::string prefix = lhs_name + ": " + key + ": ";
    for (auto refChild = tree().first_child(map_node); refChild != ryml::NONE; refChild = tree().next_sibling(refChild))
    {
        const std::string refKey = config::detail::to_string(tree().key(refChild));
        if (!first_occurrence(refKeys, refKey, refChild, prefix))
        {
            ok = false;
            continue;
        }
        if (refKey == "variable")
        {
            varNode = refChild;
        }
        else if (refKey == "tolerance" && op == Compare::Near)
        {
            nestedTolNode = refChild;
            ok = false;
        }
        else
        {
            unknown_key(refChild, refKey, prefix, kVariableRefKeys);
            ok = false;
        }
    }
    if (nestedTolNode != ryml::NONE)
    {
        const std::string shownVar = varNode != ryml::NONE && is_scalar(varNode) && !is_null(varNode)
                                         ? config::detail::to_string(tree().val(varNode))
                                         : "<participant>.<variable>";
        const std::string shownTol = is_scalar(nestedTolNode) && !is_null(nestedTolNode) ? text_of(nestedTolNode) : "<number>";
        error("unknown-key", doc().line(nestedTolNode),
              prefix + "tolerance goes beside near, not inside { variable: ... }: write { near: { variable: " + shownVar +
                  " }, tolerance: " + shownTol + " }");
    }
    if (varNode == ryml::NONE)
    {
        return false;
    }
    return read_rhs_variable(varNode, lhs_variable, key, prefix, state, rhs_out, rhs_text_out) && ok;
}

void Reader::read_conditions(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::string& action)
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
        const std::size_t absentBeforeLhs = state.absent.size();
        const auto v = variable(child, state.absent);
        if (!v)
        {
            for (std::size_t i = absentBeforeLhs; i < state.absent.size(); ++i)
            {
                state.condition_bare_absent.insert(state.absent[i]);
            }
            continue;
        }
        const control::ValueType lhsType = scenario_.variables[*v].type;
        const std::string lhsText = scenario_.variables[*v].text;
        ScenarioCondition condition;
        condition.variable = *v;
        condition.line = doc().line(child);
        std::vector<std::string> rhsTexts;
        std::vector<std::string> condAbsent;
        bool good = true;
        if (tree().is_map(child))
        {
            bool hasDirectVariable = false;
            for (auto part = tree().first_child(child); part != ryml::NONE; part = tree().next_sibling(part))
            {
                if (config::detail::to_string(tree().key(part)) == "variable")
                {
                    hasDirectVariable = true;
                    break;
                }
            }
            if (hasDirectVariable)
            {
                ryml::id_type varPart = ryml::NONE;
                ryml::id_type tolPart = ryml::NONE;
                std::vector<std::string> opKeys;
                std::map<std::string, std::size_t> keys;
                for (auto part = tree().first_child(child); part != ryml::NONE; part = tree().next_sibling(part))
                {
                    const std::string key = config::detail::to_string(tree().key(part));
                    if (!first_occurrence(keys, key, part, name + ": "))
                    {
                        good = false;
                        continue;
                    }
                    if (key == "variable")
                    {
                        varPart = part;
                    }
                    else if (key == "tolerance")
                    {
                        tolPart = part;
                    }
                    else if (compare_of(key).has_value())
                    {
                        opKeys.push_back(key);
                    }
                    else
                    {
                        unknown_key(part, key, name + ": ", kVariableRefKeys);
                        good = false;
                    }
                }
                const std::string shownVar = varPart != ryml::NONE && is_scalar(varPart) && !is_null(varPart)
                                                 ? config::detail::to_string(tree().val(varPart))
                                                 : "<participant>.<variable>";
                if (tolPart != ryml::NONE && opKeys.empty())
                {
                    const std::string shownTol = is_scalar(tolPart) && !is_null(tolPart) ? text_of(tolPart) : "<number>";
                    error("value", doc().line(tolPart),
                          name + ": variable and tolerance cannot be combined directly: write { near: { variable: " +
                              shownVar + " }, tolerance: " + shownTol + " }");
                    good = false;
                }
                else if (!opKeys.empty())
                {
                    error("value", doc().line(child),
                          name + ": variable cannot be combined with " + config::detail::join(opKeys) +
                              " in the same mapping: write { eq: { variable: " + shownVar + " }, ... }");
                    good = false;
                }
                else if (good && varPart != ryml::NONE)
                {
                    std::optional<std::size_t> rhsIdx;
                    std::string rhsText;
                    const std::size_t absentBefore = state.absent.size();
                    if (read_rhs_variable(varPart, *v, "the value", name + ": ", state, rhsIdx, rhsText))
                    {
                        condition.all.push_back(ScenarioComparison{Compare::Eq, control::Value{}, 0.0, rhsIdx});
                        rhsTexts.push_back(rhsText);
                        for (std::size_t i = absentBefore; i < state.absent.size(); ++i)
                        {
                            condAbsent.push_back(state.absent[i]);
                        }
                    }
                    else
                    {
                        good = false;
                    }
                }
            }
            else
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
                        record_direct_param_usage(part, ParamType::Number);
                        double d = 0;
                        if (!plain_number(part, d) || d < 0)
                        {
                            error("value", doc().line(part),
                                  "tolerance must be a number that is not negative, not " + shown_value(part));
                            good = false;
                        }
                        tolerance = d;
                        toleranceLine = doc().line(part);
                        continue;
                    }
                    const auto op = compare_of(key);
                    if (!op)
                    {
                        if (config::detail::closest(key, kConditionKeys) == "variable")
                        {
                            error("unknown-key", doc().line(part),
                                  name + ": unknown key '" + key +
                                      "': did you mean 'variable'? (write { variable: <participant>.<variable> }, or use one "
                                      "of the keys here: " +
                                      config::detail::join(kCompareKeys) + ")");
                        }
                        else
                        {
                            unknown_key(part, key, name + ": ", kCompareKeys);
                        }
                        good = false;
                        continue;
                    }
                    hasNear = hasNear || *op == Compare::Near;
                    const bool ordered = *op != Compare::Eq && *op != Compare::Ne;
                    if (ordered && (lhsType == control::ValueType::Bool || lhsType == control::ValueType::Enum))
                    {
                        error("scenario-value", doc().line(part),
                              key + " compares numbers, and " + lhsText + " is " +
                                  (lhsType == control::ValueType::Bool ? "a bool" : "an enum") + ": use eq or ne");
                        good = false;
                        continue;
                    }
                    if (tree().is_map(part))
                    {
                        std::optional<std::size_t> rhsIdx;
                        std::string rhsText;
                        const std::size_t absentBefore = state.absent.size();
                        if (!read_variable_ref_map(part, *v, *op, name, key, state, rhsIdx, rhsText))
                        {
                            good = false;
                            continue;
                        }
                        condition.all.push_back(ScenarioComparison{*op, control::Value{}, 0.0, rhsIdx});
                        rhsTexts.push_back(rhsText);
                        for (std::size_t i = absentBefore; i < state.absent.size(); ++i)
                        {
                            condAbsent.push_back(state.absent[i]);
                        }
                    }
                    else
                    {
                        const auto value = value_of(part, *v, key, false);
                        if (!value)
                        {
                            good = false;
                            continue;
                        }
                        condition.all.push_back(ScenarioComparison{*op, *value, 0.0});
                        rhsTexts.emplace_back();
                    }
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
        }
        else if (const auto value = value_of(child, *v, "the value", false))
        {
            condition.all.push_back(ScenarioComparison{Compare::Eq, *value, 0.0});
            rhsTexts.emplace_back();
        }
        else
        {
            good = false;
        }
        if (good)
        {
            for (const std::string& absName : condAbsent)
            {
                state.condition_formatted_absent.insert(absName);
            }
            condition.text = condition_text(scenario_.variables[*v], condition, rhsTexts);
            step.conditions.push_back(std::move(condition));
        }
        else
        {
            for (const std::string& absName : condAbsent)
            {
                state.condition_bare_absent.insert(absName);
            }
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

std::string Reader::condition_text(const ScenarioVariable& var, const ScenarioCondition& condition,
                                   const std::vector<std::string>& rhs_texts)
{
    std::string out = var.text;
    bool first = true;
    for (std::size_t i = 0; i < condition.all.size(); ++i)
    {
        const auto& c = condition.all[i];
        const std::string rhs = i < rhs_texts.size() && !rhs_texts[i].empty() ? rhs_texts[i] : value_text(c.value, &var);
        out += std::string(first ? " " : ", ") + compare_name(c.op) + " " + rhs +
               (c.op == Compare::Near ? " (tolerance " + number_text(c.tolerance) + ")" : "");
        first = false;
    }
    return out;
}

std::string Reader::step_text(const ScenarioStep& step, const StepReadState& state) const
{
    std::string out = step_kind_name(step.kind);
    bool first = true;
    const auto sep = [&] { return std::exchange(first, false) ? " " : "; "; };
    // The variables that are absent, by name alone (what they would be is not read), once each.
    const auto absent = [&](std::size_t from, std::size_t to, bool in_conditions) {
        for (std::size_t i = from; i < to; ++i)
        {
            const std::string& name = state.absent[i];
            if (in_conditions && state.condition_formatted_absent.count(name) > 0 &&
                state.condition_bare_absent.count(name) == 0)
            {
                continue;
            }
            if (std::find(state.absent.begin() + static_cast<std::ptrdiff_t>(from),
                          state.absent.begin() + static_cast<std::ptrdiff_t>(i), name) ==
                state.absent.begin() + static_cast<std::ptrdiff_t>(i))
            {
                out += sep() + name;
            }
        }
    };
    const std::size_t motion = state.beside ? state.motion_absent : state.absent.size();
    const bool standaloneCond = !state.beside && (step.kind == StepKind::Expect || step.kind == StepKind::WaitUntil);
    absent(0, motion, standaloneCond);
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
    if (state.beside)
    {
        // "ramp Engine.engine.throttle to 80 over 1000 ms, expecting Engine.engine.speed le 3000"
        out += state.unchecked ? ", expect not checked:" : ", expecting";
        first = true;
        absent(motion, state.absent.size(), true);
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

bool enum_items_match(const std::vector<control::EnumItem>& a, const std::vector<control::EnumItem>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    const auto by_key = [](const control::EnumItem& x, const control::EnumItem& y) {
        return x.value != y.value ? x.value < y.value : x.name < y.name;
    };
    std::vector<control::EnumItem> sa = a;
    std::vector<control::EnumItem> sb = b;
    std::sort(sa.begin(), sa.end(), by_key);
    std::sort(sb.begin(), sb.end(), by_key);
    for (std::size_t i = 0; i < sa.size(); ++i)
    {
        if (sa[i].value != sb[i].value || sa[i].name != sb[i].name)
        {
            return false;
        }
    }
    return true;
}

namespace {

constexpr double kTwoPow63 = 9223372036854775808.0;
constexpr double kTwoPow64 = 18446744073709551616.0;

struct Uint128
{
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    [[nodiscard]] bool operator<=(const Uint128& o) const noexcept { return hi != o.hi ? hi < o.hi : lo <= o.lo; }
};

/// Exact integer value of `x` where 2^63 <= x < 2^128 (already an integer in IEEE-754 double).
Uint128 uint128_from_large_double(double x) noexcept
{
    int exp = 0;
    const double mant = std::frexp(x, &exp); // 0.5 <= mant < 1.0, 64 <= exp <= 128
    const auto sig = static_cast<std::uint64_t>(std::ldexp(mant, 53));
    const int shift = exp - 53; // 11 <= shift <= 75
    if (shift < 64)
    {
        return Uint128{sig >> (64 - shift), sig << shift};
    }
    return Uint128{sig << (shift - 64), 0};
}

Uint128 uint128_add64(Uint128 a, std::uint64_t b) noexcept
{
    const std::uint64_t lo = a.lo + b;
    return Uint128{a.hi + (lo < a.lo ? 1u : 0u), lo};
}

Uint128 uint128_sub64(Uint128 a, std::uint64_t b) noexcept
{
    return Uint128{a.hi - (a.lo < b ? 1u : 0u), a.lo - b};
}

/// Exact comparison of `i` and finite `d`: -1 if i < d, 0 if i == d, +1 if i > d.
int compare_int_double(std::int64_t i, double d) noexcept
{
    if (d < -kTwoPow63)
    {
        return 1;
    }
    if (d >= kTwoPow63)
    {
        return -1;
    }
    double ipart = 0.0;
    const double fpart = std::modf(d, &ipart);
    const auto di = static_cast<std::int64_t>(ipart);
    if (i < di)
    {
        return -1;
    }
    if (i > di)
    {
        return 1;
    }
    return fpart > 0.0 ? -1 : fpart < 0.0 ? 1 : 0;
}

/// Exact check of a + b >= 1.0 for 0 <= a < 1 and 0 <= b < 1 without rounding error (Sterbenz).
bool sum_at_least_one(double a, double b) noexcept
{
    const double hi = std::max(a, b);
    const double lo = std::min(a, b);
    if (hi < 0.5)
    {
        return false;
    }
    return lo >= (1.0 - hi);
}

bool uint64_within_tolerance(std::uint64_t diff, double tolerance) noexcept
{
    if (tolerance >= kTwoPow64)
    {
        return true;
    }
    return diff <= static_cast<std::uint64_t>(std::floor(tolerance));
}

/// Exact check of |i - d| <= tolerance for finite `d` and finite `tolerance` >= 0, without long double.
bool int_double_near(std::int64_t i, double d, double tolerance) noexcept
{
    if (d >= -kTwoPow63 && d < kTwoPow63)
    {
        if (tolerance >= kTwoPow64)
        {
            return true;
        }
        double t_ipart_d = 0.0;
        const double t_fpart = std::modf(tolerance, &t_ipart_d);
        const auto t_ipart = static_cast<std::uint64_t>(t_ipart_d);

        double d_ipart_d = 0.0;
        const double d_fpart = std::modf(d, &d_ipart_d);
        const auto di = static_cast<std::int64_t>(d_ipart_d);

        if (i == di)
        {
            return t_ipart >= 1 || std::fabs(d_fpart) <= t_fpart;
        }
        if (i > di)
        {
            const std::uint64_t u = static_cast<std::uint64_t>(i) - static_cast<std::uint64_t>(di);
            if (d_fpart <= 0.0)
            {
                const double af = -d_fpart;
                return u != t_ipart ? u < t_ipart : af <= t_fpart;
            }
            if (u <= t_ipart)
            {
                return true;
            }
            return u - t_ipart == 1 && sum_at_least_one(d_fpart, t_fpart);
        }
        const std::uint64_t u = static_cast<std::uint64_t>(di) - static_cast<std::uint64_t>(i);
        if (d_fpart >= 0.0)
        {
            return u != t_ipart ? u < t_ipart : d_fpart <= t_fpart;
        }
        if (u <= t_ipart)
        {
            return true;
        }
        return u - t_ipart == 1 && sum_at_least_one(-d_fpart, t_fpart);
    }

    const double kTwoPow128 = std::ldexp(1.0, 128);
    const double abs_d = std::fabs(d);
    if (abs_d >= kTwoPow128)
    {
        if (tolerance != abs_d)
        {
            return tolerance > abs_d;
        }
        return d > 0.0 ? i >= 0 : i <= 0;
    }
    if (tolerance >= kTwoPow128)
    {
        return true;
    }
    Uint128 diff = uint128_from_large_double(abs_d);
    if (d > 0.0)
    {
        diff = i >= 0 ? uint128_sub64(diff, static_cast<std::uint64_t>(i))
                      : uint128_add64(diff, std::uint64_t{0} - static_cast<std::uint64_t>(i));
    }
    else
    {
        diff = i <= 0 ? uint128_sub64(diff, std::uint64_t{0} - static_cast<std::uint64_t>(i))
                      : uint128_add64(diff, static_cast<std::uint64_t>(i));
    }
    const double fl_t = std::floor(tolerance);
    const Uint128 tol_u =
        fl_t < kTwoPow64 ? Uint128{0, static_cast<std::uint64_t>(fl_t)} : uint128_from_large_double(fl_t);
    return diff <= tol_u;
}

} // namespace

bool holds(Compare op, const control::Value& lhs, const control::Value& rhs, double tolerance)
{
    if (const auto* lb = std::get_if<bool>(&lhs))
    {
        const auto* rb = std::get_if<bool>(&rhs);
        if (rb == nullptr)
        {
            return false;
        }
        return op == Compare::Eq ? *lb == *rb : op == Compare::Ne ? *lb != *rb : false;
    }
    if (std::holds_alternative<bool>(rhs))
    {
        return false;
    }
    if (const auto* ld = std::get_if<double>(&lhs); ld != nullptr && !std::isfinite(*ld))
    {
        return false;
    }
    if (const auto* rd = std::get_if<double>(&rhs); rd != nullptr && !std::isfinite(*rd))
    {
        return false;
    }
    if (op == Compare::Near && (!std::isfinite(tolerance) || tolerance < 0.0))
    {
        return false;
    }

    const auto* li = std::get_if<std::int64_t>(&lhs);
    const auto* ri = std::get_if<std::int64_t>(&rhs);
    if (li != nullptr && ri != nullptr)
    {
        switch (op)
        {
        case Compare::Eq: return *li == *ri;
        case Compare::Ne: return *li != *ri;
        case Compare::Ge: return *li >= *ri;
        case Compare::Le: return *li <= *ri;
        case Compare::Gt: return *li > *ri;
        case Compare::Lt: return *li < *ri;
        case Compare::Near:
        {
            const std::uint64_t diff = *li >= *ri
                                           ? static_cast<std::uint64_t>(*li) - static_cast<std::uint64_t>(*ri)
                                           : static_cast<std::uint64_t>(*ri) - static_cast<std::uint64_t>(*li);
            return uint64_within_tolerance(diff, tolerance);
        }
        }
        return false;
    }

    if (li != nullptr || ri != nullptr)
    {
        if (op == Compare::Near)
        {
            return li != nullptr ? int_double_near(*li, std::get<double>(rhs), tolerance)
                                 : int_double_near(*ri, std::get<double>(lhs), tolerance);
        }
        const int cmp =
            li != nullptr ? compare_int_double(*li, std::get<double>(rhs)) : -compare_int_double(*ri, std::get<double>(lhs));
        switch (op)
        {
        case Compare::Eq: return cmp == 0;
        case Compare::Ne: return cmp != 0;
        case Compare::Ge: return cmp >= 0;
        case Compare::Le: return cmp <= 0;
        case Compare::Gt: return cmp > 0;
        case Compare::Lt: return cmp < 0;
        case Compare::Near: break;
        }
        return false;
    }

    const double x = std::get<double>(lhs);
    const double y = std::get<double>(rhs);
    switch (op)
    {
    case Compare::Eq: return x == y;
    case Compare::Ne: return x != y;
    case Compare::Ge: return x >= y;
    case Compare::Le: return x <= y;
    case Compare::Gt: return x > y;
    case Compare::Lt: return x < y;
    case Compare::Near: return std::fabs(x - y) <= tolerance;
    }
    return false;
}

bool holds(const ScenarioComparison& comparison, const control::Value& value)
{
    return holds(comparison.op, value, comparison.value, comparison.tolerance);
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
            for (const ScenarioComparison& comparison : condition.all)
            {
                if (comparison.rhs_variable)
                {
                    reads(*comparison.rhs_variable);
                }
            }
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

ScenarioInspection inspect_scenario(std::string_view text, const ScenarioContext& context)
{
    ScenarioInspection out;
    if (text.substr(0, 2) == "\xFF\xFE" || text.substr(0, 2) == "\xFE\xFF" || text.find('\0') != std::string_view::npos)
    {
        out.findings.push_back(config::Finding{config::Severity::Error, "encoding",
                                               "the file is not UTF-8 text (it looks like UTF-16, or has NUL bytes): save it as UTF-8",
                                               0, "", ""});
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
        reader::SharedReadCache sharedCache;
        ScenarioContext baseCtx = context;
        baseCtx.case_name.reset();
        baseCtx.params_for_many = true;
        ScenarioRead firstRead;
        Reader firstReader(doc, {}, baseCtx, firstRead, false, &sharedCache);
        firstReader.read();
        out.has_cases = firstReader.has_cases();
        out.cases = firstRead.cases;
        if (!out.has_cases)
        {
            out.params_incomplete = firstRead.params_incomplete;
            for (const config::ParamValue& p : firstRead.params)
            {
                if (std::find(out.param_names.begin(), out.param_names.end(), p.name) == out.param_names.end())
                {
                    out.param_names.push_back(p.name);
                }
            }
            out.findings = std::move(firstRead.findings);
            return out;
        }
        if (out.cases.empty() || firstReader.cases_incomplete())
        {
            out.params_incomplete = true;
            out.findings = std::move(firstRead.findings);
            return out;
        }
        std::vector<std::string> targets;
        if (context.case_name)
        {
            if (std::find(out.cases.begin(), out.cases.end(), *context.case_name) != out.cases.end())
            {
                targets.push_back(*context.case_name);
            }
        }
        else
        {
            targets = out.cases;
        }
        const auto has_err = [](const std::vector<config::Finding>& fs) {
            return std::any_of(fs.begin(), fs.end(),
                               [](const config::Finding& f) { return f.severity == config::Severity::Error; });
        };
        for (const std::string& caseName : targets)
        {
            ScenarioContext caseCtx = context;
            caseCtx.case_name = caseName;
            caseCtx.params_for_many = true;
            ScenarioRead caseRead;
            Reader(doc, {}, caseCtx, caseRead, false, &sharedCache).read();
            out.params_incomplete = out.params_incomplete || caseRead.params_incomplete;
            CaseInspection caseInfo;
            caseInfo.name = caseName;
            caseInfo.params_incomplete = caseRead.params_incomplete;
            caseInfo.has_error = has_err(caseRead.findings);
            for (const config::ParamValue& p : caseRead.params)
            {
                if (std::find(caseInfo.param_names.begin(), caseInfo.param_names.end(), p.name) == caseInfo.param_names.end())
                {
                    caseInfo.param_names.push_back(p.name);
                }
                if (std::find(out.param_names.begin(), out.param_names.end(), p.name) == out.param_names.end())
                {
                    out.param_names.push_back(p.name);
                }
            }
            out.case_inspections.push_back(std::move(caseInfo));
            if (out.findings.empty() || (!has_err(out.findings) && has_err(caseRead.findings)))
            {
                out.findings = std::move(caseRead.findings);
            }
        }
        if (out.findings.empty())
        {
            out.findings = std::move(firstRead.findings);
        }
    }
    catch (const config::detail::YamlSyntaxError& e)
    {
        out.findings.clear();
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
