// SPDX-License-Identifier: MIT
//
// The check's output: for people (format_text()) and as JSON (format_json()), and the pieces of
// them that the run's output and the events reuse (check_format.hpp). Pure functions.
#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"

#include "check_format.hpp"
#include "vehicle_sil/config/detail/check_words.hpp"
#include "vehicle_sil/config/detail/seen_ecu.hpp"
#include "vehicle_sil/ecu/describe_json.hpp"
#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <system_error>
#include <cctype>
#include <cmath>
#include <charconv>
#include <algorithm>
#include <sstream>

namespace vehicle_sil::launch {

namespace {

using config::detail::hex2;
using config::detail::step_text;

/// The number of code points of UTF-8 text (its width in a table).
std::size_t width(std::string_view text)
{
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

/// Rows of columns, each column as wide as its widest cell, two spaces between; no trailing
/// spaces.
std::string table(const std::vector<std::vector<std::string>>& rows)
{
    std::vector<std::size_t> widths;
    for (const auto& row : rows)
    {
        widths.resize(std::max(widths.size(), row.size()));
        for (std::size_t i = 0; i < row.size(); ++i)
        {
            widths[i] = std::max(widths[i], width(row[i]));
        }
    }
    std::string out;
    for (const auto& row : rows)
    {
        std::string line;
        for (std::size_t i = 0; i < row.size(); ++i)
        {
            line += row[i];
            if (i + 1 < row.size())
            {
                line.append(widths[i] - width(row[i]) + 2, ' ');
            }
        }
        out += line + "\n";
    }
    return out;
}

std::string count_text(std::size_t n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

/// A network of an ECU as a table cell: "J1939_CAN (j1939 0x00 0x80)", "Body (lin_master)"; one
/// that the entry renames with both names, the simulation's first: "VehicleCAN <- J1939_CAN (j1939 0x00)";
/// a node whose address j1939_nodes changes with both addresses, the simulation's first and no
/// space around the arrow: "J1939_CAN (j1939 0x01<-0x00)" (254: "any").
std::string network_cell(const config::SeenNetwork& seen)
{
    const NetworkDescription& network = seen.network;
    std::string out = network.name + (seen.renamed() ? " <- " + seen.executable_name : std::string{}) + " (" +
                      to_string(network.use);
    if (network.use == NetworkUse::J1939)
    {
        if (network.nodes.empty())
        {
            out += ", no node";
        }
        const auto address = [](j1939::Address a) { return a == j1939::kNullAddress ? std::string("any") : config::detail::hex2(a); };
        for (std::size_t k = 0; k < network.nodes.size(); ++k)
        {
            const j1939::Address now = network.nodes[k].address;
            const j1939::Address coded = k < seen.coded_nodes.size() ? seen.coded_nodes[k].address : now;
            out += " " + address(now) + (now != coded ? "<-" + address(coded) : std::string{});
        }
    }
    if (network.mac)
    {
        out += " " + network.mac->to_string();
    }
    return out + ")";
}

/// true if the participant is enabled and described itself on `network` with its kind (as the rules
/// count; any kind for a network whose kind could not be read).
bool is_on(const config::ParticipantConfig& participant, const config::ParticipantResult& result, const config::NetworkConfig& network)
{
    if (!participant.enabled || result.status != config::ParticipantStatus::Described || !result.ecu)
    {
        return false;
    }
    const std::vector<config::SeenNetwork> seen = config::SeenEcu(participant, *result.ecu).seen_networks();
    return std::any_of(seen.begin(), seen.end(), [&](const config::SeenNetwork& n) {
        return n.network.name == network.name && (!network.valid || n.network.kind() == network.kind);
    });
}

/// " (1 skipped, 1 expect not checked)", or "" if the run skips nothing.
std::string scenario_counts_text(const scenario::ScenarioCheck& scenario)
{
    std::vector<std::string> parts;
    if (scenario.skipped > 0)
    {
        parts.push_back(std::to_string(scenario.skipped) + " skipped");
    }
    if (scenario.unchecked_expects > 0)
    {
        parts.push_back(std::to_string(scenario.unchecked_expects) +
                        (scenario.unchecked_expects == 1 ? " expect not checked" : " expects not checked"));
    }
    std::string out;
    for (const std::string& part : parts)
    {
        out += (out.empty() ? " (" : ", ") + part;
    }
    return out.empty() ? out : out + ")";
}

const char* severity_name(config::Severity severity)
{
    return severity == config::Severity::Error ? "error" : severity == config::Severity::Warning ? "warning" : "note";
}

const char* status_name(config::ParticipantStatus status)
{
    switch (status)
    {
    case config::ParticipantStatus::Described:
        return "described";
    case config::ParticipantStatus::NotChecked:
        return "not_checked";
    case config::ParticipantStatus::NotDescribed:
        return "not_described";
    case config::ParticipantStatus::Failed:
        return "failed";
    case config::ParticipantStatus::Skipped:
        return "skipped";
    case config::ParticipantStatus::Disabled:
        return "disabled";
    }
    return "skipped";
}

} // namespace

std::string detail::check_text_body(const config::CheckReport& report, const scenario::ScenarioCheck* scenario)
{
    std::string out;
    if (report.config)
    {
        const config::SimulationConfig& config = *report.config;
        const auto disabled = static_cast<std::size_t>(std::count_if(
            config.participants.begin(), config.participants.end(), [](const config::ParticipantConfig& p) { return !p.enabled; }));
        out += report.file + (config.variant.empty() ? std::string{} : " (variant " + config.variant + ")") + ": " +
               count_text(config.participants.size(), "participant", "participants") +
               (disabled > 0 ? " (" + std::to_string(disabled) + " disabled)" : std::string{}) + ", " +
               count_text(config.networks.size(), "network", "networks") + "\n\n";
        std::vector<std::vector<std::string>> rows{{"participant", "step", "networks"}};
        for (std::size_t i = 0; i < config.participants.size(); ++i)
        {
            const config::ParticipantConfig& p = config.participants[i];
            const config::ParticipantResult result = i < report.participants.size() ? report.participants[i] : config::ParticipantResult{};
            // A participant that is not an ECU says so after its name: "Spy (role: test)".
            std::vector<std::string> row{(p.name.empty() ? "participants[" + std::to_string(i) + "]" : p.name) +
                                         (p.role == config::ParticipantRole::Ecu ? std::string{}
                                                                         : std::string(" (role: ") + to_string(p.role) + ")")};
            if (result.status == config::ParticipantStatus::Described && result.ecu)
            {
                const config::SeenEcu ecu(p, *result.ecu);
                row.push_back(ecu.step() ? config::detail::step_text(*ecu.step()) : "real time");
                std::string cells;
                for (const config::SeenNetwork& network : ecu.seen_networks())
                {
                    cells += (cells.empty() ? "" : ", ") + network_cell(network);
                }
                row.push_back(cells.empty() ? "-" : cells);
            }
            else
            {
                row.push_back("-");
                row.push_back(result.status == config::ParticipantStatus::NotChecked     ? "not checked (describe: false)"
                              : result.status == config::ParticipantStatus::NotDescribed ? "not described (see the notes)"
                              : result.status == config::ParticipantStatus::Failed       ? "no description (see the errors)"
                              : result.status == config::ParticipantStatus::Disabled     ? "disabled"
                                                                                 : "not read (see the errors)");
            }
            rows.push_back(std::move(row));
        }
        if (!config.participants.empty())
        {
            out += table(rows) + "\n";
        }
        // The column "outside" only with --connect: a report without it is as it was before.
        rows = {{"network", "kind", "participants"}};
        if (!report.outside.empty())
        {
            rows.front().emplace_back("outside");
        }
        for (const auto& network : config.networks)
        {
            std::string users;
            for (std::size_t i = 0; i < config.participants.size() && i < report.participants.size(); ++i)
            {
                if (is_on(config.participants[i], report.participants[i], network))
                {
                    users += (users.empty() ? "" : ", ") + config.participants[i].name;
                }
            }
            rows.push_back({network.name, network.valid ? to_string(network.kind) : "-", users.empty() ? "-" : users});
            if (!report.outside.empty())
            {
                const auto outside = std::find_if(report.outside.begin(), report.outside.end(),
                                                  [&](const config::NetworkOutside& o) { return o.network == network.name; });
                rows.back().push_back(outside != report.outside.end() ? outside->target : "-");
            }
        }
        if (!config.networks.empty())
        {
            out += table(rows) + "\n";
        }
    }
    if (scenario != nullptr && report.config)
    {
        out += "scenario " + scenario->file + (scenario->name.empty() ? "" : " (" + scenario->name + ")") +
               ": " + count_text(scenario->steps, "step", "steps") +
               scenario_counts_text(*scenario) + "\n";
        if (!scenario->params.empty())
        {
            std::string params;
            for (const config::ParamValue& param : scenario->params)
            {
                params += (params.empty() ? "" : ", ") + param.name + " = " + param.text + " (" + config::param_source_text(param.source) + ")";
            }
            out += "parameters: " + params + "\n";
        }
        out += "\n";
        if (scenario->scenario && scenario->scenario->has_dither)
        {
            // Only for a scenario with a dither: another one's output is as it was.
            out += detail::scenario_lines_text(scenario->scenario->lines) + "\n";
        }
    }
    out += detail::findings_text(report.file, report.findings);
    return out;
}

void detail::write_scenario_lines_json(vehicle_sil::json::JsonWriter& w, const std::vector<scenario::ScenarioLineText>& inputs,
                                       const std::vector<scenario::ScenarioLineText>& lines)
{
    const auto number = [&w](std::string_view key, const std::optional<std::int64_t>& n) {
        if (n)
        {
            w.integer(key, *n);
        }
        else
        {
            w.null(key);
        }
    };
    // As a line of a timeline file has it: its type, and the keys of the text with their values.
    const auto shape = [&w](const std::optional<scenario::ScenarioShapeText>& described) {
        if (!described)
        {
            w.null("shape");
            return;
        }
        w.begin_object("shape");
        w.string("type", described->type);
        for (const auto& [key, value] : described->values)
        {
            if (const auto* d = std::get_if<double>(&value))
            {
                w.real(key, *d);
            }
            else if (const auto* i = std::get_if<std::int64_t>(&value))
            {
                w.integer(key, *i);
            }
            else if (const auto* b = std::get_if<bool>(&value))
            {
                w.boolean(key, *b);
            }
            else
            {
                w.string(key, std::get<std::string>(value));
            }
        }
        w.end_object();
    };
    w.begin_array("inputs");
    for (const scenario::ScenarioLineText& input : inputs)
    {
        w.begin_object();
        w.string("variable", input.variable);
        w.integer("line", static_cast<std::int64_t>(input.line));
        w.begin_object("dither");
        shape(input.shape);
        w.string("text", input.text);
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.begin_array("lines");
    for (const scenario::ScenarioLineText& line : lines)
    {
        w.begin_object();
        w.string("variable", line.variable);
        w.string("kind", line.kind);
        w.integer("line", static_cast<std::int64_t>(line.line));
        number("step", line.step ? std::optional<std::int64_t>(static_cast<std::int64_t>(*line.step)) : std::nullopt);
        number("from_ms", line.from_ms);
        number("to_ms", line.to_ms);
        w.string("time_source", scenario::line_time_source_name(line.time_source));
        number("after_line", line.time_source == scenario::ScenarioLineText::TimeSource::AfterWait
                                 ? std::optional<std::int64_t>(static_cast<std::int64_t>(line.after_wait))
                                 : std::nullopt);
        shape(line.shape);
        w.string("text", line.text);
        w.end_object();
    }
    w.end_array();
}

std::string detail::scenario_lines_text(const std::vector<scenario::ScenarioLineText>& lines)
{
    std::vector<std::vector<std::string>> rows{{"input", "from", "to", "what", "line"}};
    for (const scenario::ScenarioLineText& line : lines)
    {
        // A time that the text does not give ("?": after a step that waits; "first write": a
        // declared dither begins with the first write of its input), and no end ("-").
        const auto ms = [](const std::optional<std::int64_t>& t) { return t ? std::to_string(*t) + " ms" : std::string("?"); };
        const bool dither = line.kind == "dither";
        // (The text without the variable, which has its own column.)
        const std::string what = line.text.rfind(line.variable + " ", 0) == 0 ? line.text.substr(line.variable.size() + 1) : line.text;
        rows.push_back({line.variable, line.time_source == scenario::ScenarioLineText::TimeSource::FirstWrite ? "first write" : ms(line.from_ms),
                        dither ? "-" : ms(line.to_ms),
                        line.kind + " " + what +
                            (line.time_source == scenario::ScenarioLineText::TimeSource::AfterWait
                                 ? " (after the wait of line " + std::to_string(line.after_wait) + ")"
                                 : ""),
                        std::to_string(line.line)});
    }
    return table(rows);
}

std::string detail::findings_text(const std::string& file, const std::vector<config::Finding>& findings)
{
    std::string out;
    for (const auto& finding : findings)
    {
        out += std::string(severity_name(finding.severity)) + ": " + (finding.file.empty() ? file : finding.file) +
               (finding.line > 0 ? ":" + std::to_string(finding.line) : std::string{}) + ": " + finding.message +
               (finding.code.empty() ? std::string{} : " [" + finding.code + "]") + "\n";
        if (!finding.detail.empty())
        {
            std::istringstream in(finding.detail);
            std::string line;
            while (std::getline(in, line))
            {
                out += "    " + line + "\n";
            }
        }
    }
    if (!findings.empty())
    {
        out += "\n";
    }
    return out;
}

std::string detail::count_summary(std::size_t errors, std::size_t warnings, std::size_t notes)
{
    return count_text(errors, "error", "errors") + ", " + count_text(warnings, "warning", "warnings") + ", " +
           count_text(notes, "note", "notes");
}

std::string detail::table_text(const std::vector<std::vector<std::string>>& rows) { return table(rows); }

const char* detail::severity_text(config::Severity severity) { return severity_name(severity); }

namespace {

std::string text_of(const config::CheckReport& report, const scenario::ScenarioCheck* scenario)
{
    std::string out = detail::check_text_body(report, scenario);
    const std::string counts = count_text(report.errors(), "error", "errors") + ", " +
                               count_text(report.warnings(), "warning", "warnings") + ", " +
                               count_text(report.notes(), "note", "notes");
    if (!report.config)
    {
        out += "cannot read " + report.file + ": " + counts + "\n";
    }
    else if (report.passed())
    {
        out += "passed: " + counts + "\n";
    }
    else
    {
        out += std::string("failed: ") + counts + (report.errors() == 0 ? " (--strict)" : "") + "\n";
    }
    return out;
}

} // namespace

std::string format_text(const Checked& checked) { return text_of(checked, detail::scenario_of(checked)); }

void detail::write_finding_members(vehicle_sil::json::JsonWriter& w, const std::string& file, const config::Finding& finding)
{
    const auto optional_text = [&w](std::string_view key, const std::string& value) {
        if (value.empty())
        {
            w.null(key);
        }
        else
        {
            w.string(key, value);
        }
    };
    w.string("severity", severity_name(finding.severity));
    w.string("code", finding.code);
    optional_text("file", finding.file.empty() ? file : finding.file);
    if (finding.line > 0)
    {
        w.integer("line", static_cast<std::int64_t>(finding.line));
    }
    else
    {
        w.null("line");
    }
    optional_text("participant", finding.participant);
    optional_text("network", finding.network);
    w.string("message", finding.message);
    optional_text("detail", finding.detail);
}

void detail::write_params_json(vehicle_sil::json::JsonWriter& w, std::string_view key, const std::vector<config::ParamValue>& params)
{
    w.begin_array(key);
    for (const config::ParamValue& param : params)
    {
        w.begin_object();
        w.string("name", param.name);
        // As a scenario reads it: true, false, a whole number, a number, or a text.
        const std::string& text = param.text;
        std::int64_t integer = 0;
        double number = 0;
        const char* end = text.data() + text.size();
        const bool plain = !param.quoted && !text.empty() &&
                           (std::isdigit(static_cast<unsigned char>(text.front())) != 0 || text.front() == '-') &&
                           std::all_of(text.begin(), text.end(), [](char c) {
                               return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+';
                           });
        if (!param.quoted && (text == "true" || text == "false"))
        {
            w.boolean("value", text == "true");
        }
        else if (const auto [ptr, ec] = std::from_chars(text.data(), end, integer); plain && ec == std::errc{} && ptr == end)
        {
            w.integer("value", integer);
        }
        else if (const auto [at, bad] = std::from_chars(text.data(), end, number);
                 plain && bad == std::errc{} && at == end && std::isfinite(number))
        {
            w.real("value", number);
        }
        else
        {
            w.string("value", text);
        }
        w.string("from", config::param_source_name(param.source));
        w.end_object();
    }
    w.end_array();
}

void detail::write_findings_json(vehicle_sil::json::JsonWriter& w, const std::string& file, const std::vector<config::Finding>& findings)
{
    w.begin_array("findings");
    for (const auto& finding : findings)
    {
        w.begin_object();
        write_finding_members(w, file, finding);
        w.end_object();
    }
    w.end_array();
}

namespace {

std::string json_of(const config::CheckReport& report, const scenario::ScenarioCheck* scenario)
{
    vehicle_sil::json::JsonWriter w;
    detail::write_check_json(w, {}, report, scenario);
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace

std::string format_json(const Checked& checked) { return json_of(checked, detail::scenario_of(checked)); }

void detail::write_check_json(vehicle_sil::json::JsonWriter& w, std::string_view member, const config::CheckReport& report,
                              const scenario::ScenarioCheck* scenario)
{
    const auto optional_text = [&w](std::string_view key, const std::string& value) {
        if (value.empty())
        {
            w.null(key);
        }
        else
        {
            w.string(key, value);
        }
    };
    const auto optional_integer = [&w](std::string_view key, const std::optional<std::int64_t>& value) {
        if (value)
        {
            w.integer(key, *value);
        }
        else
        {
            w.null(key);
        }
    };
    w.begin_object(member);
    w.integer("vehicle_sil_check", kCheckFormat);
    w.string("vehicle_sil", version());
    optional_text("file", report.file);
    w.string("result", report.usage     ? "usage"
                       : !report.config ? "unreadable"
                       : report.passed() ? "passed"
                                         : "failed");
    w.boolean("strict", report.strict);
    w.integer("errors", static_cast<std::int64_t>(report.errors()));
    w.integer("warnings", static_cast<std::int64_t>(report.warnings()));
    w.integer("notes", static_cast<std::int64_t>(report.notes()));
    optional_integer("stop_at_ms", report.config ? report.config->stop_at_ms : std::nullopt);
    optional_integer("start_timeout_ms",
                     report.config ? std::optional<std::int64_t>(report.config->start_timeout_ms) : std::nullopt);
    optional_integer("stop_timeout_ms",
                     report.config ? std::optional<std::int64_t>(report.config->stop_timeout_ms) : std::nullopt);
    optional_text("registry_uri", report.config ? report.config->registry_uri.value_or("") : std::string{});
    // check --compare-commands; null without it (also in a run's check, which compares nothing).
    optional_text("commands", report.commands == config::CommandsCompared::Same          ? "same"
                              : report.commands == config::CommandsCompared::Changed     ? "changed"
                              : report.commands == config::CommandsCompared::NotCompared ? "not_compared"
                                                                                         : "");
    if (!report.variants.empty())
    {
        // Only for a file with variants: the chosen one (null with result "usage") and all of them.
        optional_text("variant", report.config ? report.config->variant : std::string{});
        w.begin_array("variants");
        for (const std::string& name : report.variants)
        {
            w.string({}, name);
        }
        w.end_array();
    }
    if (report.config && !report.config->params.empty())
    {
        // Only for a file with params: those of the common part with the variant's over them.
        write_params_json(w, "params", report.config->params);
    }
    w.begin_array("participants");
    if (report.config)
    {
        for (std::size_t i = 0; i < report.config->participants.size(); ++i)
        {
            const config::ParticipantConfig& p = report.config->participants[i];
            const config::ParticipantResult result = i < report.participants.size() ? report.participants[i] : config::ParticipantResult{};
            w.begin_object();
            optional_text("name", p.name);
            w.integer("line", static_cast<std::int64_t>(p.line));
            w.begin_array("command");
            for (const auto& word : p.command)
            {
                w.string({}, word);
            }
            w.end_array();
            w.boolean("describe", p.describe);
            w.boolean("required", p.required);
            w.boolean("enabled", p.enabled);
            w.boolean("manual", p.manual);
            w.string("role", to_string(p.role));
            optional_integer("step_ms", p.step_ms);
            w.string("status", status_name(result.status));
            if (!result.ecu)
            {
                w.null("reported");
            }
            else
            {
                const config::SeenEcu seen(p, *result.ecu);
                const EcuDescription& ecu = seen.as_described();
                w.begin_object("reported");
                optional_text("vehicle_sil", result.ecu->vehicle_sil_version);
                optional_text("name", ecu.name);
                optional_integer("step_ns", ecu.step ? std::optional<std::int64_t>(ecu.step->count()) : std::nullopt);
                detail::write_reported_networks(w, ecu);
                w.end_object();
            }
            if (result.ecu && result.ecu->variables_reported)
            {
                // As the simulation sees them (the run's hub applies the same map).
                vehicle_sil::write_variables(w, "variables", config::SeenEcu(p, *result.ecu).variables().schema());
            }
            else
            {
                w.null("variables");
            }
            // Only for an entry with "variables": the names on both sides.
            if (!p.variables.empty())
            {
                w.begin_array("variable_map");
                for (std::size_t m = 0; m < p.variables.size(); ++m)
                {
                    const control::VariableMapping& mapping = p.variables[m];
                    w.begin_object();
                    w.string("name", mapping.name);
                    w.integer("line", static_cast<std::int64_t>(p.variable_lines[m]));
                    optional_text("from", mapping.from);
                    w.boolean("absent", mapping.absent);
                    w.real("scale", mapping.linear() ? std::optional<double>(mapping.scale.value_or(1.0)) : std::nullopt);
                    w.real("offset", mapping.linear() ? std::optional<double>(mapping.offset.value_or(0.0)) : std::nullopt);
                    if (mapping.unit)
                    {
                        w.string("unit", *mapping.unit);
                    }
                    else
                    {
                        w.null("unit");
                    }
                    if (mapping.map)
                    {
                        w.begin_array("map");
                        for (const auto& item : *mapping.map)
                        {
                            w.begin_object();
                            w.string("name", item.name);
                            w.integer("value", item.value);
                            w.end_object();
                        }
                        w.end_array();
                    }
                    else
                    {
                        w.null("map");
                    }
                    w.end_object();
                }
                w.end_array();
            }
            // For an entry with "network_names": the lines as written (also one that renames
            // nothing), the simulation's name and the executable's; null without.
            if (p.network_names.empty())
            {
                w.null("network_map");
            }
            else
            {
                w.begin_array("network_map");
                for (const config::NetworkName& entry : p.network_names)
                {
                    w.begin_object();
                    w.string("name", entry.name);
                    w.integer("line", static_cast<std::int64_t>(entry.line));
                    w.string("from", entry.from);
                    w.end_object();
                }
                w.end_array();
            }
            // For an entry with "j1939_nodes": the entries as written (also one that changes
            // nothing), and the node each takes with its values in the code and after the entry;
            // null without.
            if (p.j1939_nodes.empty())
            {
                w.null("j1939_nodes");
            }
            else
            {
                const std::vector<config::SeenNetwork> seen = p.enabled && result.status == config::ParticipantStatus::Described && result.ecu
                                                          ? config::SeenEcu(p, *result.ecu).seen_networks()
                                                          : std::vector<config::SeenNetwork>{};
                const auto number = [&w](const char* key, const auto& value) {
                    if (value)
                    {
                        w.integer(key, static_cast<std::int64_t>(*value));
                    }
                    else
                    {
                        w.null(key);
                    }
                };
                w.begin_array("j1939_nodes");
                for (std::size_t e = 0; e < p.j1939_nodes.size(); ++e)
                {
                    const config::J1939NodeEntry& entry = p.j1939_nodes[e];
                    w.begin_object();
                    w.string("network", entry.network);
                    w.integer("line", static_cast<std::int64_t>(entry.line));
                    number("from", entry.node.from);
                    number("address", entry.node.address);
                    number("identity_number", entry.node.identity_number);
                    number("function_instance", entry.node.function_instance);
                    number("ecu_instance", entry.node.ecu_instance);
                    const config::SeenNetwork* on = nullptr;
                    const config::J1939NodeMatch* taken = nullptr;
                    for (const config::SeenNetwork& n : seen)
                    {
                        for (const config::J1939NodeMatch& match : n.j1939_matches)
                        {
                            if (match.entry == e && match.result.match == j1939::NodeOverrideMatch::Applied)
                            {
                                on = &n;
                                taken = &match;
                            }
                        }
                    }
                    if (taken != nullptr)
                    {
                        const J1939NodeDescription& node = on->network.nodes[taken->result.node];
                        w.begin_object("node");
                        w.integer("coded_address", on->coded_nodes[taken->result.node].address);
                        w.integer("address", node.address);
                        if (node.name)
                        {
                            w.string("j1939_name", vehicle_sil::json::hex64(node.name->value()));
                        }
                        else
                        {
                            w.null("j1939_name");
                        }
                        w.boolean("arbitrary_address_capable", node.arbitrary_address_capable());
                        w.end_object();
                    }
                    else
                    {
                        w.null("node");
                    }
                    w.end_object();
                }
                w.end_array();
            }
            if (const std::optional<ControlOffer> offer = result.ecu ? config::SeenEcu(p, *result.ecu).control() : std::nullopt)
            {
                w.begin_object("control");
                w.string("transport", offer->transport);
                w.integer("wire_version", offer->wire_version);
                w.integer("wire_revision", offer->wire_revision);
                w.end_object();
            }
            else
            {
                w.null("control");
            }
            w.end_object();
        }
    }
    w.end_array();
    w.begin_array("networks");
    if (report.config)
    {
        for (const auto& network : report.config->networks)
        {
            w.begin_object();
            w.string("name", network.name);
            optional_text("kind", network.valid ? to_string(network.kind) : "");
            w.integer("line", static_cast<std::int64_t>(network.line));
            w.begin_array("participants");
            for (std::size_t i = 0; i < report.config->participants.size() && i < report.participants.size(); ++i)
            {
                if (is_on(report.config->participants[i], report.participants[i], network))
                {
                    w.string({}, report.config->participants[i].name);
                }
            }
            w.end_array();
            // null on a network that --connect does not connect: no tool is looked for for it, so
            // the output of a check without --connect does not depend on the tools this computer has.
            const auto outside = std::find_if(report.outside.begin(), report.outside.end(),
                                              [&](const config::NetworkOutside& o) { return o.network == network.name; });
            if (outside != report.outside.end())
            {
                w.begin_object("outside");
                w.string("tool", outside->tool);
                optional_text("tool_path", outside->tool_path.empty() ? std::string{} : process::shown_path(outside->tool_path));
                w.string("target", outside->target);
                w.string("participant", outside->participant);
                // null: an error is about connecting the network (a command it would refuse).
                if (outside->arguments)
                {
                    w.begin_array("command");
                    w.string({}, outside->tool);
                    for (const std::string& argument : *outside->arguments)
                    {
                        w.string({}, argument);
                    }
                    w.end_array();
                }
                else
                {
                    w.null("command");
                }
                w.end_object();
            }
            else
            {
                w.null("outside");
            }
            w.end_object();
        }
    }
    w.end_array();
    if (report.config)
    {
        w.begin_object("time_lock");
        w.boolean("locked", report.time_lock.locked);
        w.begin_array("because");
        for (const std::string& name : report.time_lock.because())
        {
            w.string({}, name);
        }
        w.end_array();
        w.end_object();
    }
    else
    {
        w.null("time_lock");
    }
    detail::write_findings_json(w, report.file, report.findings);
    if (scenario != nullptr)
    {
        // Only with --scenario: a result without one is as it was before scenarios.
        w.begin_object("scenario");
        w.string("file", scenario->file);
        optional_text("name", scenario->name);
        w.integer("steps", static_cast<std::int64_t>(scenario->steps));
        // Each only if the scenario has it.
        if (scenario->only)
        {
            w.begin_array("only");
            for (const std::string& name : *scenario->only)
            {
                w.string({}, name);
            }
            w.end_array();
        }
        if (!scenario->params.empty())
        {
            write_params_json(w, "params", scenario->params);
        }
        if (scenario->skipped > 0)
        {
            w.integer("skipped_steps", static_cast<std::int64_t>(scenario->skipped));
        }
        if (scenario->unchecked_expects > 0)
        {
            w.integer("unchecked_expects", static_cast<std::int64_t>(scenario->unchecked_expects));
        }
        if (scenario->scenario)
        {
            // Empty for a scenario without a dither (Scenario::has_dither).
            detail::write_scenario_lines_json(w, scenario::scenario_inputs_text(*scenario->scenario), scenario->scenario->lines);
        }
        else
        {
            // The scenario could not be read.
            w.null("inputs");
            w.null("lines");
        }
        w.end_object();
    }
    w.end_object();
}

} // namespace vehicle_sil::launch
