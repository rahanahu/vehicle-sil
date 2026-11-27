// SPDX-License-Identifier: MIT
//
// The scenario's reader (scenario_reader.hpp): parameterized step templates, scoped path
// resolution, lexical parameter binding, and call_stack provenance formatting.
#include "scenario_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::scenario {

std::string format_call_stack_text(const std::vector<SourceFrame>& stack, std::string_view line_prefix)
{
    std::string detail;
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
    {
        if (!detail.empty())
        {
            detail += '\n';
        }
        detail += line_prefix;
        detail += "called from: ";
        detail += it->file.empty() ? "line " + std::to_string(it->line) : it->file + ":" + std::to_string(it->line);
        std::string meta;
        if (!it->name.empty())
        {
            meta += "step '" + it->name + "'";
        }
        for (const SourceParam& p : it->params)
        {
            if (!meta.empty())
            {
                meta += ", ";
            }
            meta += p.name + " = " + (p.quoted ? "\"" + p.text + "\"" : p.text);
        }
        if (!meta.empty())
        {
            detail += " (" + meta + ")";
        }
    }
    return detail;
}

} // namespace vehicle_sil::scenario

namespace vehicle_sil::scenario::reader {

namespace {

bool is_whole_param_ref(std::string_view text, std::string& name)
{
    if (text.size() > 3 && text.rfind("${", 0) == 0 && text.back() == '}')
    {
        const std::string_view inner = text.substr(2, text.size() - 3);
        if (config::is_param_name(inner))
        {
            name = std::string(inner);
            return true;
        }
    }
    return false;
}

std::string summarize_template_error(const std::vector<config::Finding>& findings, std::size_t from)
{
    for (std::size_t i = from; i < findings.size(); ++i)
    {
        if (findings[i].severity == config::Severity::Error)
        {
            const config::Finding& f = findings[i];
            std::string out = "is invalid (";
            if (!f.code.empty())
            {
                out += f.code;
                if (f.line > 0)
                {
                    out += " at line " + std::to_string(f.line);
                }
                out += ": ";
            }
            out += f.message + ")";
            return out;
        }
    }
    return "is invalid";
}

} // namespace

std::string Reader::format_call_stack(const std::vector<SourceFrame>& stack)
{
    return format_call_stack_text(stack);
}

std::string Reader::shown_path_for(const std::string& normalized_rel) const
{
    return scenario_dir_prefix_.empty() ? normalized_rel : scenario_dir_prefix_ + "/" + normalized_rel;
}

std::optional<std::string> Reader::resolve_scoped_path(const std::string& dir_rel, const std::string& raw,
                                                       std::string& normalized_out, bool is_template)
{
    static const std::string kOnlyBelow = "a scenario reads only files in its own directory and below it";
    const std::string example = is_template ? "templates/power_on.yaml" : "traces/drive.csv";
    const std::string what = is_template ? "the template file" : "the CSV file";
    if (raw.empty())
    {
        return "is empty: give the path of " + what + " from the scenario file's directory (" + example + ")";
    }
    for (const char c : raw)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F)
        {
            return "has a control character: write a valid file path (" + example + ")";
        }
    }
    if (raw.find('\\') != std::string::npos)
    {
        return "has '\\': write '/' between directories (" + example + "), which every OS reads";
    }
    if (raw.front() == '/' ||
        (raw.size() >= 2 && raw[1] == ':' && std::isalpha(static_cast<unsigned char>(raw[0])) != 0))
    {
        return "is an absolute path: give the path from the scenario file's directory (" + example + "); " + kOnlyBelow;
    }

    std::vector<std::string> parts;
    if (!dir_rel.empty())
    {
        for (std::size_t at = 0; at <= dir_rel.size();)
        {
            const std::size_t end = std::min(dir_rel.find('/', at), dir_rel.size());
            if (end > at)
            {
                parts.push_back(dir_rel.substr(at, end - at));
            }
            at = end + 1;
        }
    }

    bool lastWasDir = false;
    for (std::size_t at = 0; at <= raw.size();)
    {
        const std::size_t end = std::min(raw.find('/', at), raw.size());
        const std::string_view seg(raw.data() + at, end - at);
        if (seg.empty())
        {
            return "is not a valid file path: write '/' between directory names without '//' or trailing '/'";
        }
        if (seg == ".")
        {
            lastWasDir = true;
        }
        else if (seg == "..")
        {
            if (dir_rel.empty() || parts.empty())
            {
                return "goes up a directory (..): " + kOnlyBelow + "; put the file there";
            }
            parts.pop_back();
            lastWasDir = true;
        }
        else
        {
            parts.emplace_back(seg);
            lastWasDir = false;
        }
        at = end + 1;
    }
    if (parts.empty() || lastWasDir)
    {
        return "must name a file, not a directory (" + example + ")";
    }

    normalized_out.clear();
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            normalized_out += '/';
        }
        normalized_out += parts[i];
    }
    return std::nullopt;
}

const char* Reader::param_type_name(ParamType type)
{
    switch (type)
    {
    case ParamType::Integer: return "integer";
    case ParamType::Number: return "number";
    case ParamType::Boolean: return "boolean";
    case ParamType::String: return "string";
    }
    return "string";
}

bool Reader::is_yaml12_non_string_literal(std::string_view text)
{
    // Matches YAML 1.2 Core Schema (CoreLoader in tests/schema/test_schemas.py):
    // null, bool, int (decimal, 0o..., 0x...), float (decimal/exponent, .inf, .nan).
    if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL" ||
        text == "true" || text == "True" || text == "TRUE" || text == "false" || text == "False" || text == "FALSE")
    {
        return true;
    }
    if (text == ".nan" || text == ".NaN" || text == ".NAN")
    {
        return true;
    }
    std::size_t i = 0;
    if (text[i] == '+' || text[i] == '-')
    {
        ++i;
    }
    if (i == text.size())
    {
        return false;
    }
    const std::string_view body = text.substr(i);
    if (body == ".inf" || body == ".Inf" || body == ".INF")
    {
        return true;
    }
    // 0o[0-7]+ or 0x[0-9a-fA-F]+ (CoreLoader allows without sign)
    if (i == 0 && text.size() > 2 && text[0] == '0' && text[1] == 'o')
    {
        return std::all_of(text.begin() + 2, text.end(), [](char c) { return c >= '0' && c <= '7'; });
    }
    if (i == 0 && text.size() > 2 && text[0] == '0' && text[1] == 'x')
    {
        return std::all_of(text.begin() + 2, text.end(),
                           [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    }
    // int: [-+]?[0-9]+
    if (std::all_of(body.begin(), body.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
    {
        return true;
    }
    // float: [-+]?(.[0-9]+|[0-9]+(.[0-9]*)?)([eE][-+]?[0-9]+)?
    std::size_t p = 0;
    if (body[p] == '.')
    {
        ++p;
        if (p == body.size() || std::isdigit(static_cast<unsigned char>(body[p])) == 0)
        {
            return false;
        }
        while (p < body.size() && std::isdigit(static_cast<unsigned char>(body[p])) != 0)
        {
            ++p;
        }
    }
    else if (std::isdigit(static_cast<unsigned char>(body[p])) != 0)
    {
        while (p < body.size() && std::isdigit(static_cast<unsigned char>(body[p])) != 0)
        {
            ++p;
        }
        if (p < body.size() && body[p] == '.')
        {
            ++p;
            while (p < body.size() && std::isdigit(static_cast<unsigned char>(body[p])) != 0)
            {
                ++p;
            }
        }
    }
    else
    {
        return false;
    }
    if (p < body.size())
    {
        if (body[p] != 'e' && body[p] != 'E')
        {
            return false;
        }
        ++p;
        if (p < body.size() && (body[p] == '+' || body[p] == '-'))
        {
            ++p;
        }
        if (p == body.size() || std::isdigit(static_cast<unsigned char>(body[p])) == 0)
        {
            return false;
        }
        while (p < body.size() && std::isdigit(static_cast<unsigned char>(body[p])) != 0)
        {
            ++p;
        }
    }
    return p == body.size();
}

bool Reader::validate_param_value(config::ParamValue& val, ParamType type, std::size_t line, const std::string& label)
{
    const std::string shown = val.quoted ? "the text \"" + val.text + "\"" : "'" + val.text + "'";
    switch (type)
    {
    case ParamType::Integer:
    {
        std::int64_t i = 0;
        const Hex hex = val.quoted ? Hex{} : read_hex(val.text);
        if (!val.quoted && (read_integer(val.text, i) || hex.kind == Hex::Kind::Read))
        {
            val.from_cli = false;
            return true;
        }
        std::string hint;
        if (val.quoted)
        {
            hint = ": write the integer without quotes";
        }
        else if (hex.kind == Hex::Kind::Negative)
        {
            hint = ": write a negative number in decimal (-" + std::to_string(hex.magnitude) + ")";
        }
        else if (hex.kind == Hex::Kind::TooLarge)
        {
            hint = ": it is more than an integer holds (at most 0x7FFFFFFFFFFFFFFF)";
        }
        error("scenario-param", line, label + " must be a whole number (type: integer), not " + shown + hint);
        return false;
    }
    case ParamType::Number:
    {
        double d = 0;
        if (!val.quoted && read_number(val.text, d))
        {
            val.from_cli = false;
            return true;
        }
        const Hex hex = val.quoted ? Hex{} : read_hex(val.text);
        std::string hint;
        if (val.quoted)
        {
            hint = ": write the number without quotes";
        }
        else if (hex.kind == Hex::Kind::Read)
        {
            hint = ": hexadecimal (0x...) is for type: integer only; write " + std::to_string(hex.magnitude);
        }
        error("scenario-param", line, label + " must be a number (type: number), not " + shown + hint);
        return false;
    }
    case ParamType::Boolean:
        if (!val.quoted && (val.text == "true" || val.text == "false"))
        {
            val.from_cli = false;
            return true;
        }
        error("scenario-param", line,
              label + " must be true or false (type: boolean), not " + shown +
                  (val.quoted ? ": write true or false without quotes" : ""));
        return false;
    case ParamType::String:
        if (val.quoted || val.from_cli || !is_yaml12_non_string_literal(val.text))
        {
            if (val.from_cli)
            {
                val.quoted = true;
                val.from_cli = false;
            }
            return true;
        }
        error("scenario-param", line,
              label + " must be a text (type: string), not " + shown + ": put numbers, booleans, and null in quotes (\"" +
                  val.text + "\")");
        return false;
    }
    return false;
}

bool Reader::param_values_equal(ParamType type, const config::ParamValue& a, const config::ParamValue& b)
{
    switch (type)
    {
    case ParamType::Integer:
    {
        const auto parse_int = [](const std::string& s) -> std::int64_t {
            std::int64_t i = 0;
            if (read_integer(s, i))
            {
                return i;
            }
            return static_cast<std::int64_t>(read_hex(s).magnitude);
        };
        return parse_int(a.text) == parse_int(b.text);
    }
    case ParamType::Number:
    {
        double da = 0;
        double db = 0;
        return read_number(a.text, da) && read_number(b.text, db) && da == db;
    }
    case ParamType::Boolean:
    case ParamType::String:
        return a.text == b.text;
    }
    return false;
}

bool Reader::parse_template_parameters(ryml::id_type node, std::vector<ParameterDef>& out)
{
    if (!tree().is_map(node))
    {
        error("value", doc().line(node),
              "parameters must be a mapping of parameter names to definitions ({ timeout_ms: { type: integer, default: 1000 } }), not " +
                  shown_value(node));
        return false;
    }
    bool ok = true;
    std::map<std::string, std::size_t> seenParams;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        const std::string paramName = config::detail::to_string(tree().key(child));
        const std::size_t line = doc().line(child);
        if (!first_occurrence(seenParams, paramName, child, "parameters: "))
        {
            ok = false;
            continue;
        }
        if (!config::is_param_name(paramName))
        {
            error("value", line,
                  "parameters: '" + paramName +
                      "' cannot be a parameter's name: use letters, digits and _ (not a digit first), as a scenario writes it in ${...}");
            ok = false;
            continue;
        }
        if (!tree().is_map(child) || !tree().has_children(child))
        {
            error("value", line,
                  "parameter '" + paramName + "' must be a mapping with type (" + config::detail::join(kParameterTypes) +
                      "), not " + (tree().is_map(child) ? std::string("an empty mapping") : shown_value(child)));
            ok = false;
            continue;
        }
        ParameterDef def;
        def.name = paramName;
        def.line = line;
        bool hasType = false;
        bool typeOk = false;
        ryml::id_type defaultNode = ryml::NONE;
        ryml::id_type valuesNode = ryml::NONE;
        std::map<std::string, std::size_t> seenKeys;
        for (auto part = tree().first_child(child); part != ryml::NONE; part = tree().next_sibling(part))
        {
            const std::string key = config::detail::to_string(tree().key(part));
            if (!first_occurrence(seenKeys, key, part, "parameter '" + paramName + "': "))
            {
                ok = false;
                continue;
            }
            if (key == "type")
            {
                hasType = true;
                if (!is_scalar(part) || is_null(part))
                {
                    error("value", doc().line(part),
                          "type of parameter '" + paramName + "' must be one of " + config::detail::join(kParameterTypes) +
                              ", not " + shown_value(part));
                    ok = false;
                }
                else
                {
                    const std::string t = config::detail::to_string(tree().val(part));
                    if (t == "integer")
                    {
                        def.type = ParamType::Integer;
                        typeOk = true;
                    }
                    else if (t == "number")
                    {
                        def.type = ParamType::Number;
                        typeOk = true;
                    }
                    else if (t == "boolean")
                    {
                        def.type = ParamType::Boolean;
                        typeOk = true;
                    }
                    else if (t == "string")
                    {
                        def.type = ParamType::String;
                        typeOk = true;
                    }
                    else
                    {
                        error("value", doc().line(part),
                              "type of parameter '" + paramName + "' must be one of " + config::detail::join(kParameterTypes) +
                                  ", not " + shown_value(part));
                        ok = false;
                    }
                }
            }
            else if (key == "default")
            {
                defaultNode = part;
            }
            else if (key == "values")
            {
                valuesNode = part;
            }
            else if (key == "description")
            {
                if (!is_scalar(part) || is_null(part) ||
                    (!tree().is_val_quoted(part) &&
                     is_yaml12_non_string_literal(config::detail::to_string(tree().val(part)))))
                {
                    error("value", doc().line(part),
                          "description of parameter '" + paramName + "' must be a text, not " + shown_value(part) +
                              (!is_scalar(part) || is_null(part)
                                   ? ""
                                   : ": put numbers, booleans, and null in quotes (\"" +
                                         config::detail::to_string(tree().val(part)) + "\")"));
                    ok = false;
                }
                else
                {
                    def.description = config::detail::to_string(tree().val(part));
                }
            }
            else
            {
                unknown_key(part, key, "parameter '" + paramName + "': ", kParameterDefKeys);
                ok = false;
            }
        }
        if (!hasType)
        {
            error("missing-key", line,
                  "parameter '" + paramName + "' needs type (" + config::detail::join(kParameterTypes) + ")");
            ok = false;
        }
        if (!typeOk)
        {
            continue;
        }
        bool valuesOk = true;
        if (valuesNode != ryml::NONE)
        {
            if (!tree().is_seq(valuesNode) || !tree().has_children(valuesNode))
            {
                error("value", doc().line(valuesNode),
                      "values of parameter '" + paramName + "' must be a non-empty list, not " +
                          (tree().is_seq(valuesNode) ? std::string("an empty list") : shown_value(valuesNode)));
                valuesOk = false;
                ok = false;
            }
            else
            {
                for (auto item = tree().first_child(valuesNode); item != ryml::NONE; item = tree().next_sibling(item))
                {
                    const std::size_t itemLine = doc().line(item);
                    if (!is_scalar(item) || is_null(item))
                    {
                        error("value", itemLine,
                              "each item in values of parameter '" + paramName + "' must be a scalar, not " + shown_value(item));
                        valuesOk = false;
                        ok = false;
                        continue;
                    }
                    const std::string text = config::detail::to_string(tree().val(item));
                    if (text.find("${") != std::string::npos)
                    {
                        error("scenario-param", itemLine,
                              "values of parameter '" + paramName + "' cannot use another parameter (${...}): write the value out");
                        valuesOk = false;
                        ok = false;
                        continue;
                    }
                    config::ParamValue pv{paramName, text, tree().is_val_quoted(item), config::ParamSource::TemplateDefault, itemLine, false};
                    if (!validate_param_value(pv, def.type, itemLine, "item in values of parameter '" + paramName + "'"))
                    {
                        valuesOk = false;
                        ok = false;
                        continue;
                    }
                    def.allowed_values.push_back(std::move(pv));
                }
            }
        }
        if (defaultNode != ryml::NONE)
        {
            const std::size_t defLine = doc().line(defaultNode);
            if (!is_scalar(defaultNode) || is_null(defaultNode))
            {
                error("value", defLine,
                      "default of parameter '" + paramName + "' must be a scalar, not " + shown_value(defaultNode));
                ok = false;
            }
            else
            {
                const std::string text = config::detail::to_string(tree().val(defaultNode));
                if (text.find("${") != std::string::npos)
                {
                    error("scenario-param", defLine,
                          "default of parameter '" + paramName + "' cannot use another parameter (${...}): write the value out");
                    ok = false;
                }
                else
                {
                    config::ParamValue pv{paramName, text, tree().is_val_quoted(defaultNode),
                                          config::ParamSource::TemplateDefault, defLine, false};
                    if (!validate_param_value(pv, def.type, defLine, "default of parameter '" + paramName + "'"))
                    {
                        ok = false;
                    }
                    else if (valuesOk && !def.allowed_values.empty() &&
                             std::none_of(def.allowed_values.begin(), def.allowed_values.end(),
                                          [&](const config::ParamValue& allowed) {
                                              return param_values_equal(def.type, pv, allowed);
                                          }))
                    {
                        error("scenario-param", defLine,
                              "default of parameter '" + paramName + "' is not one of its allowed values");
                        ok = false;
                    }
                    else
                    {
                        def.default_value = std::move(pv);
                    }
                }
            }
        }
        out.push_back(std::move(def));
    }
    return ok;
}

const CachedTemplate* Reader::load_template(const std::string& normalized_rel, std::size_t call_line)
{
    if (const auto it = cache_.templates.find(normalized_rel); it != cache_.templates.end())
    {
        if (!it->second.read_error.empty())
        {
            error("scenario-template-file", call_line, "template '" + normalized_rel + "' " + it->second.read_error);
            return nullptr;
        }
        if (!it->second.valid)
        {
            const std::string summary = it->second.invalid_summary.empty() ? std::string("is invalid") : it->second.invalid_summary;
            error("scenario-template-file", call_line, "template '" + normalized_rel + "' " + summary);
            return nullptr;
        }
        return &it->second;
    }

    CachedTemplate& tpl = cache_.templates[normalized_rel];
    tpl.shown_file = shown_path_for(normalized_rel);
    const auto slash = normalized_rel.rfind('/');
    tpl.dir_rel = slash == std::string::npos ? std::string{} : normalized_rel.substr(0, slash);

    const auto& readerFn = context_.read_template ? context_.read_template : context_.read_file;
    if (!readerFn)
    {
        tpl.read_error =
            "cannot be read here: templates are read with the scenario file (check --scenario, run --scenario)";
        error("scenario-template-file", call_line, "template '" + normalized_rel + "' " + tpl.read_error);
        return nullptr;
    }
    const auto bytes = readerFn(normalized_rel, tpl.read_error);
    if (!bytes)
    {
        if (tpl.read_error.empty())
        {
            tpl.read_error = "could not be read";
        }
        error("scenario-template-file", call_line, "template '" + normalized_rel + "' " + tpl.read_error);
        return nullptr;
    }
    const std::size_t loadFindingsStart = findings_.size();
    if (bytes->substr(0, 2) == "\xFF\xFE" || bytes->substr(0, 2) == "\xFE\xFF" ||
        bytes->find('\0') != std::string::npos)
    {
        add_at(config::Severity::Error, "encoding", tpl.shown_file, 0,
               "the file is not UTF-8 text (it looks like UTF-16, or has NUL bytes): save it as UTF-8", "", "",
               format_call_stack(active_templates_));
        tpl.invalid_summary = summarize_template_error(findings_, loadFindingsStart);
        return nullptr;
    }
    try
    {
        tpl.doc = std::make_unique<config::detail::YamlDocument>(*bytes);
    }
    catch (const config::detail::YamlSyntaxError& e)
    {
        std::string detail = e.context();
        const std::string stack = format_call_stack(active_templates_);
        if (!stack.empty())
        {
            if (!detail.empty())
            {
                detail += '\n';
            }
            detail += stack;
        }
        add_at(config::Severity::Error, "syntax", tpl.shown_file, e.line(),
               "syntax error: " + e.message() +
                   (e.context().find("${") != std::string::npos
                        ? ": inside { ... } or [ ... ], write a parameter in quotes (\"${name}\")"
                        : ""),
               "", "", std::move(detail));
        tpl.invalid_summary = summarize_template_error(findings_, loadFindingsStart);
        return nullptr;
    }
    if (tpl.doc->empty())
    {
        add_at(config::Severity::Error, "not-a-template", tpl.shown_file, 0,
               "the template file is empty: write vehicle_sil_scenario_template: 1 and steps", "", "",
               format_call_stack(active_templates_));
        tpl.invalid_summary = summarize_template_error(findings_, loadFindingsStart);
        return nullptr;
    }

    const std::size_t findingsBefore = findings_.size();
    scopes_.push_back(EvalScope{tpl.doc.get(), tpl.shown_file, tpl.dir_rel});
    ScopeGuard popValidationScope([this]() { scopes_.pop_back(); });

    const ryml::id_type root = doc().root();
    if (root == ryml::NONE || !tree().is_map(root))
    {
        error("not-a-template", root == ryml::NONE ? 0 : doc().line(root),
              "the file is not a scenario template: it must be a mapping with vehicle_sil_scenario_template: 1 and steps");
        tpl.invalid_summary = summarize_template_error(findings_, findingsBefore);
        return nullptr;
    }
    std::vector<std::size_t> refs;
    find_references(root, refs);
    if (!refs.empty())
    {
        for (const std::size_t line : refs)
        {
            error("unsupported-yaml", line,
                  "anchors and aliases (&name, *name, <<) are not read: write the values out (a value that starts with * or & must be in quotes)");
        }
        tpl.invalid_summary = summarize_template_error(findings_, findingsBefore);
        return nullptr;
    }
    if (const ryml::id_type second = doc().second_document(); second != ryml::NONE)
    {
        error("unsupported-yaml", doc().line(second),
              "a second document (after ---): a template file has one document; remove the others or put them in files of their own");
    }

    bool hasVersion = false;
    bool hasScenarioVersion = false;
    ryml::id_type paramsNode = ryml::NONE;
    ryml::id_type stepsNode = ryml::NONE;
    std::map<std::string, std::size_t> seen;
    for (auto child = tree().first_child(root); child != ryml::NONE; child = tree().next_sibling(child))
    {
        const std::string key = config::detail::to_string(tree().key(child));
        if (!first_occurrence(seen, key, child))
        {
            continue;
        }
        if (key == "vehicle_sil_scenario_template")
        {
            hasVersion = true;
            std::int64_t version = 0;
            if (!plain_integer(child, version))
            {
                error("format-version", doc().line(child),
                      "vehicle_sil_scenario_template must be the format version " +
                          std::to_string(kScenarioTemplateFormat) + ", not " + shown_value(child) +
                          ": write vehicle_sil_scenario_template: " + std::to_string(kScenarioTemplateFormat));
            }
            else if (version != kScenarioTemplateFormat)
            {
                findings_.erase(findings_.begin() + static_cast<std::ptrdiff_t>(findingsBefore), findings_.end());
                error("format-version", doc().line(child),
                      "vehicle_sil_scenario_template is " + std::to_string(version) +
                          ", but this vehicle_sil_launch reads format " + std::to_string(kScenarioTemplateFormat) +
                          " only: use the vehicle-sil that the file was written for");
                tpl.invalid_summary = summarize_template_error(findings_, findingsBefore);
                return nullptr;
            }
        }
        else if (key == "vehicle_sil_scenario")
        {
            hasScenarioVersion = true;
        }
        else if (key == "name")
        {
            if (!is_scalar(child) || is_null(child))
            {
                error("value", doc().line(child),
                      "name must be a text (what the template is about), not " + shown_value(child));
            }
        }
        else if (key == "parameters")
        {
            paramsNode = child;
        }
        else if (key == "steps")
        {
            stepsNode = child;
        }
        else
        {
            unknown_key(child, key, "", kTemplateTopKeys);
        }
    }

    if (hasScenarioVersion && !hasVersion)
    {
        findings_.erase(findings_.begin() + static_cast<std::ptrdiff_t>(findingsBefore), findings_.end());
        error("not-a-template", doc().line(root),
              "this file is a runnable scenario (vehicle_sil_scenario: 1), not a scenario template: put shared steps in a file with vehicle_sil_scenario_template: 1");
        tpl.invalid_summary = summarize_template_error(findings_, findingsBefore);
        return nullptr;
    }
    if (!hasVersion)
    {
        error("missing-key", 0,
              "the template file has no vehicle_sil_scenario_template (the format version): add vehicle_sil_scenario_template: " +
                  std::to_string(kScenarioTemplateFormat));
    }
    if (paramsNode != ryml::NONE)
    {
        parse_template_parameters(paramsNode, tpl.parameters);
    }
    if (stepsNode == ryml::NONE)
    {
        error("missing-key", 0,
              "the template file has no steps: add steps, a list of what to do in order");
    }
    else if (!tree().is_seq(stepsNode) || !tree().has_children(stepsNode))
    {
        error("value", doc().line(stepsNode),
              "steps must be a list with at least one step (- set: ..., - expect: ...), not " +
                  (tree().is_seq(stepsNode) ? std::string("an empty list") : shown_value(stepsNode)));
    }
    else
    {
        tpl.steps_node = stepsNode;
    }

    const bool hasErrors = std::any_of(findings_.begin() + static_cast<std::ptrdiff_t>(findingsBefore), findings_.end(),
                                       [](const config::Finding& f) { return f.severity == config::Severity::Error; });
    tpl.valid = !hasErrors;
    if (!tpl.valid)
    {
        tpl.invalid_summary = summarize_template_error(findings_, findingsBefore);
    }
    return tpl.valid ? &tpl : nullptr;
}

bool Reader::read_template_call_params(ryml::id_type params_node, const CachedTemplate& tpl, std::size_t call_line,
                                       std::vector<Known>& callee_params, std::vector<SourceParam>& frame_params)
{
    bool ok = true;
    struct PassedArg
    {
        config::ParamValue value;
        std::optional<std::string> root_param{};
    };
    std::map<std::string, PassedArg> passed;
    std::set<std::string> mentioned;
    if (params_node != ryml::NONE)
    {
        if (!tree().is_map(params_node))
        {
            error("value", doc().line(params_node),
                  "params must be a mapping of parameter names to values, not " + shown_value(params_node));
            return false;
        }
        std::vector<std::string> declaredNames;
        declaredNames.reserve(tpl.parameters.size());
        for (const ParameterDef& def : tpl.parameters)
        {
            declaredNames.push_back(def.name);
        }
        std::map<std::string, std::size_t> seen;
        for (auto child = tree().first_child(params_node); child != ryml::NONE; child = tree().next_sibling(child))
        {
            const std::string argName = config::detail::to_string(tree().key(child));
            const std::size_t line = doc().line(child);
            if (!first_occurrence(seen, argName, child, "params: "))
            {
                ok = false;
                continue;
            }
            if (!config::is_param_name(argName))
            {
                error("value", line,
                      "params: '" + argName +
                          "' cannot be a parameter's name: use letters, digits and _ (not a digit first), as a scenario writes it in ${...}");
                ok = false;
                continue;
            }
            const auto defIt = std::find_if(tpl.parameters.begin(), tpl.parameters.end(),
                                            [&](const ParameterDef& d) { return d.name == argName; });
            if (defIt == tpl.parameters.end())
            {
                const std::string guess = config::detail::closest(argName, declaredNames);
                error("scenario-param", line,
                      "unknown template parameter '" + argName + "'" +
                          (guess.empty() ? "" : " (did you mean '" + guess + "'?)") +
                          (declaredNames.empty() ? ": the template declares no parameters"
                                                 : ": its parameters are " + config::detail::join(declaredNames)));
                ok = false;
                continue;
            }
            mentioned.insert(argName);
            if (!is_scalar(child) || is_null(child))
            {
                error("value", line,
                      "params: " + argName + " must be one value (a number, true or false, or a text), not " + shown_value(child));
                ok = false;
                continue;
            }
            const std::string rawText = config::detail::to_string(tree().val(child));
            const bool rawQuoted = tree().is_val_quoted(child);
            config::ParamValue val;
            val.name = argName;
            val.source = config::ParamSource::TemplateCall;
            val.line = line;
            std::optional<std::string> forwardedRoot;

            std::string refName;
            if (rawText.find("${") != std::string::npos)
            {
                if (is_whole_param_ref(rawText, refName))
                {
                    const auto known = std::find_if(scope().params.begin(), scope().params.end(),
                                                    [&](const Known& k) { return k.value.name == refName; });
                    if (known == scope().params.end())
                    {
                        report_undefined_param(refName, line);
                        if (scopes_.size() == 1)
                        {
                            record_template_param_constraint(refName, *defIt, line, true);
                        }
                        ok = false;
                        continue;
                    }
                    known->used = true;
                    forwardedRoot = known->root_param;
                    if (forwardedRoot)
                    {
                        record_template_param_constraint(*forwardedRoot, *defIt, line, true);
                    }
                    val.text = known->value.text;
                    val.quoted = known->value.quoted;
                    val.from_cli = known->value.from_cli;
                }
                else if (defIt->type != ParamType::String)
                {
                    error("scenario-param", line,
                          "'" + rawText + "': parameter '" + argName + "' has type " + param_type_name(defIt->type) +
                              ", so a parameter reference must be the whole value (${name}), not part of a text");
                    ok = false;
                    continue;
                }
                else
                {
                    std::string interpolated;
                    if (!resolve_interpolated_string_text(rawText, line, interpolated))
                    {
                        ok = false;
                        continue;
                    }
                    val.text = std::move(interpolated);
                    val.quoted = true;
                    val.from_cli = false;
                }
            }
            else
            {
                val.text = rawText;
                val.quoted = rawQuoted;
                val.from_cli = false;
            }

            if (!validate_param_value(val, defIt->type, line, "parameter '" + argName + "'"))
            {
                ok = false;
                continue;
            }
            if (!defIt->allowed_values.empty() &&
                std::none_of(defIt->allowed_values.begin(), defIt->allowed_values.end(),
                             [&](const config::ParamValue& allowed) {
                                 return param_values_equal(defIt->type, val, allowed);
                             }))
            {
                std::vector<std::string> shownAllowed;
                shownAllowed.reserve(defIt->allowed_values.size());
                for (const config::ParamValue& allowed : defIt->allowed_values)
                {
                    shownAllowed.push_back(allowed.quoted ? "\"" + allowed.text + "\"" : allowed.text);
                }
                error("scenario-param", line,
                      "parameter '" + argName + "' must be one of " + config::detail::join(shownAllowed) + ", not " +
                          (val.quoted ? "\"" + val.text + "\"" : val.text));
                ok = false;
                continue;
            }
            passed.emplace(argName, PassedArg{std::move(val), std::move(forwardedRoot)});
        }
    }

    for (const ParameterDef& def : tpl.parameters)
    {
        if (const auto it = passed.find(def.name); it != passed.end())
        {
            callee_params.push_back(Known{it->second.value, true, false, it->second.root_param});
            frame_params.push_back(SourceParam{it->second.value.name, it->second.value.text, it->second.value.quoted,
                                               it->second.value.source});
        }
        else if (def.default_value)
        {
            callee_params.push_back(Known{*def.default_value, true, false, std::nullopt});
            frame_params.push_back(SourceParam{def.default_value->name, def.default_value->text,
                                               def.default_value->quoted, def.default_value->source});
        }
        else if (mentioned.count(def.name) == 0)
        {
            if (scopes_.size() == 1)
            {
                add_missing_param(def.name);
            }
            if (ok)
            {
                error("scenario-param", call_line,
                      "missing required template parameter '" + def.name + "' (type: " + param_type_name(def.type) +
                          "): pass it under params: { " + def.name + ": <value> }");
                ok = false;
            }
        }
    }
    return ok;
}

void Reader::read_template_step(const std::map<std::string, ryml::id_type>& nodes, const std::string& step_name,
                                std::optional<InheritedTiming> inherited, bool valid_call_time, std::size_t step_line)
{
    const ryml::id_type tplNode = nodes.at("template");
    const std::size_t tplLine = doc().line(tplNode);
    if (!is_scalar(tplNode) || is_null(tplNode))
    {
        error("value", tplLine, "template must be a template file path (templates/power_on.yaml), not " + shown_value(tplNode));
        caller_template_params_incomplete_ = true;
        end_known_ = false;
        return;
    }
    const std::string rawPath = config::detail::to_string(tree().val(tplNode));
    if (rawPath.find("${") != std::string::npos)
    {
        error("scenario-param", tplLine,
              "template path '" + rawPath + "' cannot use a parameter (${...}): write the template file path out");
        caller_template_params_incomplete_ = true;
        end_known_ = false;
        return;
    }
    std::string normalizedRel;
    if (const auto wrong = resolve_scoped_path(scope().dir_rel, rawPath, normalizedRel, true))
    {
        error("scenario-template-file", tplLine, "template '" + rawPath + "' " + *wrong);
        caller_template_params_incomplete_ = true;
        end_known_ = false;
        return;
    }
    if (const auto cycleIt = std::find(active_rel_paths_.begin(), active_rel_paths_.end(), normalizedRel);
        cycleIt != active_rel_paths_.end())
    {
        std::string chain;
        const std::size_t startIdx = static_cast<std::size_t>(cycleIt - active_rel_paths_.begin());
        for (std::size_t i = startIdx; i < active_templates_.size(); ++i)
        {
            if (!chain.empty())
            {
                chain += " -> ";
            }
            chain += loc_text(active_templates_[i].file, active_templates_[i].line);
        }
        if (!chain.empty())
        {
            chain += " -> ";
        }
        chain += loc_text(scope().file, step_line) + " (" + shown_path_for(normalizedRel) + ")";
        error("scenario-template-cycle", tplLine,
              "template call forms a cycle: " + chain + ": remove the recursive template call");
        caller_template_params_incomplete_ = true;
        end_known_ = false;
        return;
    }
    if (active_templates_.size() >= kMaxTemplateDepth)
    {
        error("scenario-template-depth", tplLine,
              "template nesting exceeds the maximum depth of " + std::to_string(kMaxTemplateDepth) +
                  ": flatten nested template calls");
        caller_template_params_incomplete_ = true;
        end_known_ = false;
        return;
    }

    SourceFrame frame;
    frame.file = scope().file;
    frame.line = step_line;
    frame.name = step_name;

    const CachedTemplate* tpl = nullptr;
    {
        active_templates_.push_back(frame);
        ScopeGuard popLoadFrame([this]() { active_templates_.pop_back(); });
        tpl = load_template(normalizedRel, tplLine);
    }
    if (tpl == nullptr)
    {
        caller_template_params_incomplete_ = true;
        end_known_ = false;
        return;
    }

    const ryml::id_type paramsNode = nodes.count("params") > 0 ? nodes.at("params") : ryml::NONE;
    std::vector<Known> calleeParams;
    std::vector<SourceParam> frameParams;
    if (!read_template_call_params(paramsNode, *tpl, step_line, calleeParams, frameParams))
    {
        caller_template_params_incomplete_ = true;
        end_known_ = false;
        return;
    }

    if (!valid_call_time)
    {
        end_known_ = false;
    }

    frame.params = std::move(frameParams);

    active_templates_.push_back(std::move(frame));
    active_rel_paths_.push_back(normalizedRel);
    EvalScope calleeScope;
    calleeScope.doc = tpl->doc.get();
    calleeScope.file = tpl->shown_file;
    calleeScope.dir_rel = tpl->dir_rel;
    calleeScope.params = std::move(calleeParams);
    scopes_.push_back(std::move(calleeScope));
    ScopeGuard popTemplate([this]() {
        scopes_.pop_back();
        active_rel_paths_.pop_back();
        active_templates_.pop_back();
    });

    if (!resolve_step_params(tpl->steps_node))
    {
        end_known_ = false;
        return;
    }
    bool aborted = false;
    read_steps(tpl->steps_node, inherited, &aborted);
    if (aborted)
    {
        end_known_ = false;
    }
}

std::optional<ParamType> Reader::join_param_types(ParamType a, ParamType b) noexcept
{
    if (a == b)
    {
        return a;
    }
    if ((a == ParamType::Integer && b == ParamType::Number) || (a == ParamType::Number && b == ParamType::Integer))
    {
        return ParamType::Integer;
    }
    return std::nullopt;
}

bool Reader::value_fits_type_quiet(const config::ParamValue& pv, ParamType type)
{
    switch (type)
    {
    case ParamType::Integer:
    {
        std::int64_t i = 0;
        return !pv.quoted && (read_integer(pv.text, i) || read_hex(pv.text).kind == Hex::Kind::Read);
    }
    case ParamType::Number:
    {
        double d = 0;
        return !pv.quoted && read_number(pv.text, d);
    }
    case ParamType::Boolean:
        return !pv.quoted && (pv.text == "true" || pv.text == "false");
    case ParamType::String:
        return pv.quoted || pv.from_cli || !is_yaml12_non_string_literal(pv.text);
    }
    return false;
}

void Reader::add_missing_param(const std::string& name)
{
    if (std::find(out_.missing_params.begin(), out_.missing_params.end(), name) == out_.missing_params.end())
    {
        out_.missing_params.push_back(name);
    }
}

void Reader::record_template_param_constraint(const std::string& root_name, const ParameterDef& def, std::size_t line,
                                              bool emit_errors)
{
    auto [it, inserted] = param_constraints_.try_emplace(root_name);
    if (inserted)
    {
        param_constraint_order_.push_back(root_name);
    }
    ParamConstraint& c = it->second;
    if (c.description.empty() && !def.description.empty())
    {
        c.description = def.description;
    }
    if (!c.effective_type)
    {
        c.effective_type = def.type;
        c.first_origin = loc_text(current_file(), line);
    }
    else
    {
        const auto joined = join_param_types(*c.effective_type, def.type);
        if (!joined)
        {
            if (emit_errors && !c.conflict_reported)
            {
                c.conflict_reported = true;
                error("scenario-param", line,
                      "parameter '" + root_name + "' has incompatible types: used as " +
                          param_type_name(*c.effective_type) + " (" + c.first_origin + ") and as " +
                          param_type_name(def.type) + ": use separate parameters or align the types");
            }
            return;
        }
        c.effective_type = *joined;
    }
    if (!c.template_type)
    {
        c.template_type = def.type;
    }
    else if (const auto joined = join_param_types(*c.template_type, def.type))
    {
        c.template_type = *joined;
    }
    const ParamType t = *c.template_type;
    if (c.has_values && def.allowed_values.empty())
    {
        std::vector<config::ParamValue> filtered;
        for (const config::ParamValue& v : c.allowed_values)
        {
            if (value_fits_type_quiet(v, t))
            {
                filtered.push_back(v);
            }
        }
        c.allowed_values = std::move(filtered);
        if (c.allowed_values.empty() && emit_errors && !c.conflict_reported)
        {
            c.conflict_reported = true;
            error("scenario-param", line,
                  "parameter '" + root_name + "' has no allowed value compatible with type " + param_type_name(t) +
                      ": use separate parameters or align the template parameter constraints");
        }
    }
    else if (!c.has_values && !def.allowed_values.empty())
    {
        c.has_values = true;
        for (const config::ParamValue& v : def.allowed_values)
        {
            if (value_fits_type_quiet(v, t) &&
                std::none_of(c.allowed_values.begin(), c.allowed_values.end(),
                             [&](const config::ParamValue& existing) { return param_values_equal(t, existing, v); }))
            {
                c.allowed_values.push_back(v);
            }
        }
        if (c.allowed_values.empty() && emit_errors && !c.conflict_reported)
        {
            c.conflict_reported = true;
            error("scenario-param", line,
                  "parameter '" + root_name + "' has no allowed value compatible with type " + param_type_name(t) +
                      ": use separate parameters or align the template parameter constraints");
        }
    }
    else if (c.has_values && !def.allowed_values.empty())
    {
        std::vector<config::ParamValue> inter;
        for (const config::ParamValue& a : c.allowed_values)
        {
            const auto match = std::find_if(def.allowed_values.begin(), def.allowed_values.end(),
                                            [&](const config::ParamValue& b) {
                                                if (t == ParamType::Integer)
                                                {
                                                    if (value_fits_type_quiet(a, ParamType::Integer) &&
                                                        value_fits_type_quiet(b, ParamType::Integer))
                                                    {
                                                        return param_values_equal(ParamType::Integer, a, b);
                                                    }
                                                    return param_values_equal(ParamType::Number, a, b) &&
                                                           (value_fits_type_quiet(a, ParamType::Integer) ||
                                                            value_fits_type_quiet(b, ParamType::Integer));
                                                }
                                                return param_values_equal(t, a, b);
                                            });
            if (match != def.allowed_values.end())
            {
                const config::ParamValue& kept =
                    (t == ParamType::Integer && !value_fits_type_quiet(a, ParamType::Integer)) ? *match : a;
                if (std::none_of(inter.begin(), inter.end(),
                                 [&](const config::ParamValue& existing) { return param_values_equal(t, existing, kept); }))
                {
                    inter.push_back(kept);
                }
            }
        }
        c.allowed_values = std::move(inter);
        if (c.allowed_values.empty() && emit_errors && !c.conflict_reported)
        {
            c.conflict_reported = true;
            error("scenario-param", line,
                  "parameter '" + root_name +
                      "' has conflicting allowed values across template calls (their intersection is empty): use separate "
                      "parameters or align the values lists");
        }
    }
}

void Reader::record_direct_param_usage(ryml::id_type node, ParamType usage_type)
{
    const auto it = scope().substituted.find(node);
    if (it == scope().substituted.end())
    {
        return;
    }
    const Known& k = scope().params[it->second];
    if (!k.root_param)
    {
        return;
    }
    auto [cit, inserted] = param_constraints_.try_emplace(*k.root_param);
    if (inserted)
    {
        param_constraint_order_.push_back(*k.root_param);
    }
    ParamConstraint& c = cit->second;
    const std::size_t line = doc().line(node);
    if (!c.effective_type)
    {
        c.effective_type = usage_type;
        c.first_origin = loc_text(current_file(), line);
        return;
    }
    const auto joined = join_param_types(*c.effective_type, usage_type);
    if (!joined)
    {
        if (!c.conflict_reported)
        {
            c.conflict_reported = true;
            error("scenario-param", line,
                  "parameter '" + *k.root_param + "' has incompatible types: used as " +
                      param_type_name(*c.effective_type) + " (" + c.first_origin + ") and as " +
                      param_type_name(usage_type) + ": use separate parameters for text and numeric/boolean values");
        }
        return;
    }
    c.effective_type = *joined;
}

void Reader::note_param_ref_metadata(const std::string& name)
{
    const auto known = std::find_if(scope().params.begin(), scope().params.end(),
                                    [&name](const Known& k) { return k.value.name == name; });
    if (known != scope().params.end())
    {
        known->used = true;
    }
    else if (scopes_.size() == 1)
    {
        add_missing_param(name);
    }
}

void Reader::scan_interpolated_text_refs(const std::string& text)
{
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const std::size_t open = text.find("${", pos);
        if (open == std::string::npos)
        {
            break;
        }
        const std::size_t close = text.find('}', open + 2);
        if (close == std::string::npos)
        {
            break;
        }
        const std::string name = text.substr(open + 2, close - (open + 2));
        if (config::is_param_name(name))
        {
            note_param_ref_metadata(name);
        }
        pos = close + 1;
    }
}

void Reader::scan_scalar_param_refs(ryml::id_type node, bool allow_interpolation)
{
    if (is_scalar(node))
    {
        if (is_null(node))
        {
            return;
        }
        const std::string text = config::detail::to_string(tree().val(node));
        if (text.find("${") == std::string::npos)
        {
            return;
        }
        if (allow_interpolation)
        {
            scan_interpolated_text_refs(text);
        }
        else
        {
            std::string name;
            if (is_whole_param_ref(text, name))
            {
                note_param_ref_metadata(name);
            }
        }
        return;
    }
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        scan_scalar_param_refs(child, allow_interpolation);
    }
}

void Reader::scan_steps_metadata(ryml::id_type steps_node)
{
    if (!tree().is_seq(steps_node))
    {
        return;
    }
    for (auto step = tree().first_child(steps_node); step != ryml::NONE; step = tree().next_sibling(step))
    {
        if (!tree().is_map(step))
        {
            scan_scalar_param_refs(step, false);
            continue;
        }
        ryml::id_type templateNode = ryml::NONE;
        ryml::id_type paramsNode = ryml::NONE;
        for (auto field = tree().first_child(step); field != ryml::NONE; field = tree().next_sibling(field))
        {
            const std::string fieldKey = tree().has_key(field) ? config::detail::to_string(tree().key(field)) : std::string{};
            if (fieldKey == "template")
            {
                templateNode = field;
            }
            else if (fieldKey == "params")
            {
                paramsNode = field;
            }
            else if (fieldKey == "name" && is_scalar(field))
            {
                scan_scalar_param_refs(field, true);
            }
            else if (fieldKey == "wave" && tree().is_map(field))
            {
                for (auto varNode = tree().first_child(field); varNode != ryml::NONE; varNode = tree().next_sibling(varNode))
                {
                    if (!tree().is_map(varNode))
                    {
                        scan_scalar_param_refs(varNode, false);
                        continue;
                    }
                    for (auto part = tree().first_child(varNode); part != ryml::NONE; part = tree().next_sibling(part))
                    {
                        const std::string partKey = tree().has_key(part) ? config::detail::to_string(tree().key(part)) : std::string{};
                        if (partKey == "points" && tree().is_map(part))
                        {
                            for (auto pf = tree().first_child(part); pf != ryml::NONE; pf = tree().next_sibling(pf))
                            {
                                const std::string pfKey = tree().has_key(pf) ? config::detail::to_string(tree().key(pf)) : std::string{};
                                scan_scalar_param_refs(pf, (pfKey == "file" || pfKey == "value" || pfKey == "time") && is_scalar(pf));
                            }
                        }
                        else
                        {
                            scan_scalar_param_refs(part, false);
                        }
                    }
                }
            }
            else
            {
                scan_scalar_param_refs(field, false);
            }
        }
        if (templateNode == ryml::NONE || !is_scalar(templateNode) || is_null(templateNode))
        {
            continue;
        }
        const std::string rawPath = config::detail::to_string(tree().val(templateNode));
        if (rawPath.find("${") != std::string::npos)
        {
            continue;
        }
        std::string normalizedRel;
        if (resolve_scoped_path(scope().dir_rel, rawPath, normalizedRel, true))
        {
            continue;
        }
        if (std::find(active_rel_paths_.begin(), active_rel_paths_.end(), normalizedRel) != active_rel_paths_.end() ||
            active_templates_.size() >= kMaxTemplateDepth)
        {
            continue;
        }
        const CachedTemplate* tpl = nullptr;
        if (const auto it = cache_.templates.find(normalizedRel); it != cache_.templates.end())
        {
            if (!it->second.valid)
            {
                continue;
            }
            tpl = &it->second;
        }
        else
        {
            const std::size_t findingsBefore = findings_.size();
            tpl = load_template(normalizedRel, doc().line(templateNode));
            findings_.resize(findingsBefore);
            if (tpl == nullptr)
            {
                // Do not poison a shared cache with silently discarded errors.
                cache_.templates.erase(normalizedRel);
                continue;
            }
        }
        std::vector<Known> calleeParams;
        std::set<std::string> passedNames;
        if (paramsNode != ryml::NONE && tree().is_map(paramsNode))
        {
            for (auto child = tree().first_child(paramsNode); child != ryml::NONE; child = tree().next_sibling(child))
            {
                const std::string argName = config::detail::to_string(tree().key(child));
                const auto defIt = std::find_if(tpl->parameters.begin(), tpl->parameters.end(),
                                                [&](const ParameterDef& d) { return d.name == argName; });
                if (defIt == tpl->parameters.end())
                {
                    continue;
                }
                passedNames.insert(argName);
                if (!is_scalar(child) || is_null(child))
                {
                    continue;
                }
                const std::string rawText = config::detail::to_string(tree().val(child));
                std::optional<std::string> forwardedRoot;
                if (rawText.find("${") != std::string::npos)
                {
                    std::string refName;
                    if (is_whole_param_ref(rawText, refName))
                    {
                        const auto known = std::find_if(scope().params.begin(), scope().params.end(),
                                                        [&](const Known& k) { return k.value.name == refName; });
                        if (known != scope().params.end())
                        {
                            known->used = true;
                            forwardedRoot = known->root_param;
                        }
                        else if (scopes_.size() == 1)
                        {
                            add_missing_param(refName);
                            forwardedRoot = refName;
                        }
                        if (forwardedRoot)
                        {
                            record_template_param_constraint(*forwardedRoot, *defIt, doc().line(child), false);
                        }
                    }
                    else if (defIt->type == ParamType::String)
                    {
                        scan_interpolated_text_refs(rawText);
                    }
                }
                calleeParams.push_back(Known{config::ParamValue{argName, rawText, tree().is_val_quoted(child),
                                                                config::ParamSource::TemplateCall, doc().line(child), false},
                                             true, false, std::move(forwardedRoot)});
            }
        }
        for (const ParameterDef& def : tpl->parameters)
        {
            if (passedNames.count(def.name) == 0)
            {
                if (def.default_value)
                {
                    calleeParams.push_back(Known{*def.default_value, true, false, std::nullopt});
                }
                else if (scopes_.size() == 1)
                {
                    add_missing_param(def.name);
                }
            }
        }
        SourceFrame frame;
        frame.file = scope().file;
        frame.line = doc().line(step);
        active_templates_.push_back(std::move(frame));
        active_rel_paths_.push_back(normalizedRel);
        EvalScope calleeScope;
        calleeScope.doc = tpl->doc.get();
        calleeScope.file = tpl->shown_file;
        calleeScope.dir_rel = tpl->dir_rel;
        calleeScope.params = std::move(calleeParams);
        scopes_.push_back(std::move(calleeScope));
        ScopeGuard popMeta([this]() {
            scopes_.pop_back();
            active_rel_paths_.pop_back();
            active_templates_.pop_back();
        });
        scan_steps_metadata(tpl->steps_node);
    }
}

void Reader::scan_metadata_only(ryml::id_type inputs_node, ryml::id_type steps_node)
{
    if (inputs_node != ryml::NONE)
    {
        scan_scalar_param_refs(inputs_node, false);
    }
    if (steps_node != ryml::NONE)
    {
        scan_steps_metadata(steps_node);
    }
}

} // namespace vehicle_sil::scenario::reader
