// SPDX-License-Identifier: MIT
//
// The check's output: for people (format_text()) and as JSON (format_json()), and the pieces of
// them that the run's output and the events reuse (check_format.hpp). Pure functions.
#include "vehicle_sil/launch/check.hpp"

#include "check_format.hpp"
#include "check_words.hpp"
#include "vehicle_sil/ecu/detail/json_writer.hpp"

#include <system_error>
#include <cctype>
#include <cmath>
#include <charconv>
#include <algorithm>
#include <sstream>

namespace vehicle_sil::launch {

namespace {

using detail::hex2;
using detail::step_text;

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

/// A network of an ECU as a table cell: "J1939_CAN (j1939 0x00 0x80)", "Body (lin_master)".
std::string network_cell(const NetworkDescription& network)
{
    std::string out = network.name + " (" + to_string(network.use);
    if (network.use == NetworkUse::J1939)
    {
        if (network.nodes.empty())
        {
            out += ", no node";
        }
        for (const auto& node : network.nodes)
        {
            out += " " + (node.address == j1939::kNullAddress ? std::string("any") : hex2(node.address));
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
bool is_on(const ParticipantConfig& participant, const ParticipantResult& result, const NetworkConfig& network)
{
    return participant.enabled && result.status == ParticipantStatus::Described && result.ecu &&
           std::any_of(result.ecu->description.networks.begin(), result.ecu->description.networks.end(),
                       [&](const NetworkDescription& n) {
                           return n.name == network.name && (!network.valid || n.kind() == network.kind);
                       });
}

const char* severity_name(Severity severity)
{
    return severity == Severity::Error ? "error" : severity == Severity::Warning ? "warning" : "note";
}

const char* status_name(ParticipantStatus status)
{
    switch (status)
    {
    case ParticipantStatus::Described:
        return "described";
    case ParticipantStatus::NotChecked:
        return "not_checked";
    case ParticipantStatus::Failed:
        return "failed";
    case ParticipantStatus::Skipped:
        return "skipped";
    case ParticipantStatus::Disabled:
        return "disabled";
    }
    return "skipped";
}

} // namespace

std::string detail::check_text_body(const CheckReport& report)
{
    std::string out;
    if (report.config)
    {
        const SimulationConfig& config = *report.config;
        const auto disabled = static_cast<std::size_t>(std::count_if(
            config.participants.begin(), config.participants.end(), [](const ParticipantConfig& p) { return !p.enabled; }));
        out += report.file + (config.variant.empty() ? std::string{} : " (variant " + config.variant + ")") + ": " +
               count_text(config.participants.size(), "participant", "participants") +
               (disabled > 0 ? " (" + std::to_string(disabled) + " disabled)" : std::string{}) + ", " +
               count_text(config.networks.size(), "network", "networks") + "\n\n";
        std::vector<std::vector<std::string>> rows{{"participant", "step", "networks"}};
        for (std::size_t i = 0; i < config.participants.size(); ++i)
        {
            const ParticipantConfig& p = config.participants[i];
            const ParticipantResult result = i < report.participants.size() ? report.participants[i] : ParticipantResult{};
            std::vector<std::string> row{p.name.empty() ? "participants[" + std::to_string(i) + "]" : p.name};
            if (result.status == ParticipantStatus::Described && result.ecu)
            {
                const EcuDescription& ecu = result.ecu->description;
                row.push_back(ecu.step ? step_text(*ecu.step) : "real time");
                std::string cells;
                for (const auto& network : ecu.networks)
                {
                    cells += (cells.empty() ? "" : ", ") + network_cell(network);
                }
                row.push_back(cells.empty() ? "-" : cells);
            }
            else
            {
                row.push_back("-");
                row.push_back(result.status == ParticipantStatus::NotChecked ? "not checked (describe: false)"
                              : result.status == ParticipantStatus::Failed   ? "no description (see the errors)"
                              : result.status == ParticipantStatus::Disabled ? "disabled"
                                                                             : "not read (see the errors)");
            }
            rows.push_back(std::move(row));
        }
        if (!config.participants.empty())
        {
            out += table(rows) + "\n";
        }
        rows = {{"network", "kind", "participants"}};
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
        }
        if (!config.networks.empty())
        {
            out += table(rows) + "\n";
        }
    }
    if (report.scenario && report.config)
    {
        out += "scenario " + report.scenario->file + (report.scenario->name.empty() ? "" : " (" + report.scenario->name + ")") +
               ": " + count_text(report.scenario->steps, "step", "steps") +
               (report.scenario->skipped > 0 ? " (" + std::to_string(report.scenario->skipped) + " skipped)" : std::string{}) + "\n";
        if (!report.scenario->params.empty())
        {
            std::string params;
            for (const ParamValue& param : report.scenario->params)
            {
                params += (params.empty() ? "" : ", ") + param.name + " = " + param.text + " (" + param_source_text(param.source) + ")";
            }
            out += "parameters: " + params + "\n";
        }
        out += "\n";
    }
    out += detail::findings_text(report.file, report.findings);
    return out;
}

std::string detail::findings_text(const std::string& file, const std::vector<Finding>& findings)
{
    std::string out;
    for (const auto& finding : findings)
    {
        out += std::string(severity_name(finding.severity)) + ": " + (finding.file.empty() ? file : finding.file) +
               (finding.line > 0 ? ":" + std::to_string(finding.line) : std::string{}) + ": " + finding.message + "\n";
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

const char* detail::severity_text(Severity severity) { return severity_name(severity); }

std::string format_text(const CheckReport& report)
{
    std::string out = detail::check_text_body(report);
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

void detail::write_finding_members(vehicle_sil::detail::JsonWriter& w, const std::string& file, const Finding& finding)
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

void detail::write_params_json(vehicle_sil::detail::JsonWriter& w, std::string_view key, const std::vector<ParamValue>& params)
{
    w.begin_array(key);
    for (const ParamValue& param : params)
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
        w.string("from", param_source_name(param.source));
        w.end_object();
    }
    w.end_array();
}

void detail::write_findings_json(vehicle_sil::detail::JsonWriter& w, const std::string& file, const std::vector<Finding>& findings)
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

std::string format_json(const CheckReport& report)
{
    vehicle_sil::detail::JsonWriter w;
    detail::write_check_json(w, {}, report);
    std::string out = w.take();
    out += '\n';
    return out;
}

void detail::write_check_json(vehicle_sil::detail::JsonWriter& w, std::string_view member, const CheckReport& report)
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
            const ParticipantConfig& p = report.config->participants[i];
            const ParticipantResult result = i < report.participants.size() ? report.participants[i] : ParticipantResult{};
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
            optional_integer("step_ms", p.step_ms);
            w.string("status", status_name(result.status));
            if (!result.ecu)
            {
                w.null("reported");
            }
            else
            {
                const EcuDescription& ecu = result.ecu->description;
                w.begin_object("reported");
                optional_text("vehicle_sil", result.ecu->vehicle_sil_version);
                optional_text("name", ecu.name);
                optional_integer("step_ns", ecu.step ? std::optional<std::int64_t>(ecu.step->count()) : std::nullopt);
                w.begin_array("networks");
                for (const auto& network : ecu.networks)
                {
                    w.begin_object();
                    w.string("name", network.name);
                    w.string("kind", to_string(network.kind()));
                    w.string("use", to_string(network.use));
                    w.begin_array("nodes");
                    for (const auto& node : network.nodes)
                    {
                        w.begin_object();
                        w.integer("address", node.address);
                        optional_text("j1939_name", node.name ? vehicle_sil::detail::hex64(node.name->value()) : "");
                        w.boolean("arbitrary_address_capable", node.arbitrary_address_capable());
                        w.end_object();
                    }
                    w.end_array();
                    optional_text("mac", network.mac ? network.mac->to_string() : "");
                    w.end_object();
                }
                w.end_array();
                w.end_object();
            }
            if (result.ecu && result.ecu->variables_reported)
            {
                // As the simulation sees them (the run's hub applies the same map).
                vehicle_sil::detail::write_variables(w, "variables", seen_variables(p, *result.ecu).schema());
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
            if (result.ecu && result.ecu->description.control)
            {
                w.begin_object("control");
                w.string("transport", result.ecu->description.control->transport);
                w.integer("wire_version", result.ecu->description.control->wire_version);
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
            w.end_object();
        }
    }
    w.end_array();
    detail::write_findings_json(w, report.file, report.findings);
    if (report.scenario)
    {
        // Only with --scenario: a result without one is as it was before scenarios.
        w.begin_object("scenario");
        w.string("file", report.scenario->file);
        optional_text("name", report.scenario->name);
        w.integer("steps", static_cast<std::int64_t>(report.scenario->steps));
        // Each only if the scenario has it.
        if (report.scenario->only)
        {
            w.begin_array("only");
            for (const std::string& name : *report.scenario->only)
            {
                w.string({}, name);
            }
            w.end_array();
        }
        if (!report.scenario->params.empty())
        {
            write_params_json(w, "params", report.scenario->params);
        }
        if (report.scenario->skipped > 0)
        {
            w.integer("skipped_steps", static_cast<std::int64_t>(report.scenario->skipped));
        }
        w.end_object();
    }
    w.end_object();
}

} // namespace vehicle_sil::launch
