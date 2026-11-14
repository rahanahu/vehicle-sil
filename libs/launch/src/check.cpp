// SPDX-License-Identifier: MIT
//
// check_file(): the steps of the check. Reads the configuration (config.cpp), runs --describe of
// each enabled participant and reads its output (describe_read.cpp), compares the descriptions
// with the file (check_rules.cpp), reads the scenario (scenario.cpp). The outputs are in
// check_format.cpp.
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/run.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

namespace {

/// The command as shown in messages, on one line: an argument that is empty or has white space,
/// quotes or control characters in quotes, the control characters as \n, \t, \r or \xNN.
std::string command_text(const std::vector<std::string>& words)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const auto& word : words)
    {
        bool quote = word.empty();
        std::string shown;
        for (const char c : word)
        {
            const auto byte = static_cast<unsigned char>(c);
            quote = quote || byte <= ' ' || c == '"' || c == '\'';
            if (c == '\n')
            {
                shown += "\\n";
            }
            else if (c == '\t')
            {
                shown += "\\t";
            }
            else if (c == '\r')
            {
                shown += "\\r";
            }
            else if (byte < ' ' || byte == 0x7F)
            {
                shown += std::string("\\x") + kHex[byte >> 4] + kHex[byte & 0xF];
            }
            else
            {
                shown += c;
            }
        }
        out += (out.empty() ? "" : " ") + (quote ? "'" + shown + "'" : shown);
    }
    return out;
}

} // namespace

std::size_t CheckReport::errors() const
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [](const Finding& f) { return f.severity == Severity::Error; }));
}

std::size_t CheckReport::warnings() const
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [](const Finding& f) { return f.severity == Severity::Warning; }));
}

std::size_t CheckReport::notes() const { return findings.size() - errors() - warnings(); }

bool CheckReport::passed() const
{
    return !usage && config.has_value() && errors() == 0 && (!strict || warnings() == 0);
}

void sort_findings(std::vector<Finding>& findings)
{
    std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
        if (a.severity != b.severity)
        {
            return a.severity < b.severity; // errors, warnings, notes
        }
        if (a.file != b.file)
        {
            return a.file.empty(); // the configuration's, then the scenario's
        }
        return a.line < b.line;
    });
}

namespace {
/// The largest configuration file read.
constexpr std::uintmax_t kMaxConfigBytes = std::uintmax_t{16} << 20;
} // namespace

namespace {

/// --scenario: reads the scenario file and checks it against the participants of `report`.
/// @returns the usage errors that only the scenario can tell (ScenarioRead::usage_errors); then
/// `report` has no scenario.
[[nodiscard]] std::vector<Finding> check_scenario_file(CheckReport& report, const CheckOptions& options)
{
    ScenarioCheck check;
    check.file = options.scenario_shown;
    const auto add = [&](Finding finding) {
        finding.file = check.file;
        report.findings.push_back(std::move(finding));
    };
    const std::filesystem::path& path = options.scenario;
    std::error_code ec;
    std::string problem;
    std::string text;
    if (!std::filesystem::exists(path, ec))
    {
        problem = "the scenario file does not exist: check the path given with --scenario";
    }
    else if (!std::filesystem::is_regular_file(path, ec))
    {
        problem = "this is not a regular file: name the scenario file with --scenario";
    }
    else if (std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    {
        problem = "the file is larger than " + std::to_string(kMaxConfigBytes >> 20) + " MiB, too large for a scenario";
    }
    else if (std::ifstream in(path, std::ios::binary); !in)
    {
        problem = "cannot open the scenario file: check its permissions";
    }
    else
    {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (!problem.empty())
    {
        add(Finding{Severity::Error, "file", problem, 0, "", ""});
    }
    else
    {
        ScenarioContext context;
        context.variant = report.config->variant;
        context.variants = report.variants;
        context.params = report.config->params;
        context.params.insert(context.params.end(), options.params.begin(), options.params.end());
        context.params_for_many = options.params_for_many;
        ScenarioRead read = read_scenario(text, scenario_catalog(report), context);
        check.name = std::move(read.name);
        check.steps = read.steps;
        check.scenario = std::move(read.scenario);
        check.params = std::move(read.params);
        check.only = std::move(read.only);
        check.skipped = read.skipped;
        check.not_for_variant = read.not_for_variant;
        if (!read.usage_errors.empty())
        {
            return std::move(read.usage_errors);
        }
        for (Finding& finding : read.findings)
        {
            add(std::move(finding));
        }
    }
    report.scenario = std::move(check);
    return {};
}

} // namespace

CheckReport check_file(const std::filesystem::path& path, const std::string& shown_as, const CheckOptions& options)
{
    CheckReport report;
    report.file = shown_as;
    report.strict = options.strict;
    // Known without the file.
    if (auto conflicts = override_conflicts(options.overrides); !conflicts.empty())
    {
        report.usage = true;
        report.findings = std::move(conflicts);
        return report;
    }
    std::error_code ec;
    std::string problem;
    std::string text;
    if (!std::filesystem::exists(path, ec))
    {
        problem = "the file does not exist: check the path";
    }
    else if (std::filesystem::is_directory(path, ec))
    {
        problem = "this is a directory: name the configuration file in it";
    }
    else if (!std::filesystem::is_regular_file(path, ec))
    {
        problem = "this is not a regular file (a device or a pipe?): name the configuration file";
    }
    else if (std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    {
        problem = "the file is larger than " + std::to_string(kMaxConfigBytes >> 20) +
                  " MiB, too large for a configuration: name the configuration file";
    }
    else if (std::ifstream in(path, std::ios::binary); !in)
    {
        problem = "cannot open the file: check its permissions";
    }
    else
    {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (!problem.empty())
    {
        report.findings.push_back(Finding{Severity::Error, "file", problem, 0, "", ""});
        return report;
    }
    ConfigRead read = read_config(text, options.overrides);
    if (read.config)
    {
        for (const VariantConfig& variant : read.config->variants)
        {
            report.variants.push_back(variant.name);
        }
    }
    if (!read.override_errors.empty())
    {
        report.usage = true;
        report.findings = std::move(read.override_errors);
        return report;
    }
    report.findings = std::move(read.findings);
    report.config = std::move(read.config);
    if (!report.config || options.file_only)
    {
        return report;
    }
    const std::string listenVariable(kListenOnNetworkVariable);
    if (auto ignored = inherited_listen_variable(*report.config, std::getenv(listenVariable.c_str())))
    {
        report.findings.push_back(std::move(*ignored));
    }
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    for (const ParticipantConfig& p : report.config->participants)
    {
        ParticipantResult result;
        // Disabled whatever its entry's errors (they are findings).
        if (!p.enabled)
        {
            result.status = ParticipantStatus::Disabled;
            report.participants.push_back(result);
            continue;
        }
        if (!p.valid)
        {
            report.participants.push_back(result);
            continue;
        }
        // "{{word}}" in the path is the literal "{word}" (a valid entry has no other "{word}").
        const std::string written = expand_placeholders({p.command.front()}, "", "").front();
        const FoundExecutable found = find_executable(written, base);
        const std::filesystem::path& executable = found.path;
        if (auto bad = check_executable(p, executable))
        {
            report.findings.push_back(std::move(*bad));
            result.status = ParticipantStatus::Failed;
            report.participants.push_back(result);
            continue;
        }
        result.executable = executable;
        if (found.on_path)
        {
            report.findings.push_back(Finding{Severity::Note, "found-on-path",
                                              "participant '" + p.name + "': " + written +
                                                  " is not in the directory of the configuration file; it runs " +
                                                  process::shown_path(executable) + " (found on PATH)",
                                              p.command_line, p.name, ""});
        }
        if (!p.describe)
        {
            result.status = ParticipantStatus::NotChecked;
            report.participants.push_back(result);
            continue;
        }
        // "{{word}}" stands for "{word}" here too (an ecu_main command has no placeholders).
        std::vector<std::string> arguments =
            expand_placeholders(std::vector<std::string>(p.command.begin() + 1, p.command.end()), "", "");
        arguments.insert(arguments.end(), {"--describe", "--name", p.name});
        std::vector<std::string> shown{p.command.front()};
        shown.insert(shown.end(), arguments.begin(), arguments.end());
        DescribeRun run;
        run.command_text = command_text(shown);
        run.timeout = options.describe_timeout;
        try
        {
            // As vehicle_sil_launch run starts it: in the directory of the configuration file.
            process::Captured captured = process::run_captured(
                std::filesystem::absolute(executable), arguments,
                process::CaptureOptions{options.describe_timeout, 1u << 20, std::filesystem::absolute(base)});
            run.exit_code = captured.exit_code;
            run.signal = captured.signal;
            run.output_truncated = captured.output_truncated;
            run.error_truncated = captured.error_truncated;
            run.standard_output = std::move(captured.standard_output);
            run.standard_error = std::move(captured.standard_error);
        }
        catch (const process::StartError& e)
        {
            run.start_error = e.what();
            run.start_refused_by_system = e.cause() == process::StartError::Cause::System;
        }
        catch (const std::exception& e)
        {
            run.start_error = e.what();
            run.start_refused_by_system = true;
        }
        DescribeOutcome outcome = interpret_describe(p, run);
        if (outcome.error)
        {
            report.findings.push_back(std::move(*outcome.error));
            result.status = ParticipantStatus::Failed;
        }
        else
        {
            result.status = ParticipantStatus::Described;
            result.ecu = std::move(outcome.ecu);
        }
        report.participants.push_back(std::move(result));
    }
    auto compared = check_participants(*report.config, report.participants, version());
    report.findings.insert(report.findings.end(), compared.begin(), compared.end());
    // A run at the file's speed with the file's time_step_ms: known before it runs.
    if (report.config->speed && report.config->time_step_ms)
    {
        RunTiming timing = detail::plan_time_step(*report.config, report, report.config->speed);
        timing.speed = report.config->speed;
        if (auto f = detail::coarse_time_step(timing))
        {
            report.findings.push_back(std::move(*f));
        }
    }
    if (!options.scenario.empty())
    {
        if (auto wrong = check_scenario_file(report, options); !wrong.empty())
        {
            // As for the other usage errors: the findings alone, and no configuration.
            CheckReport usage;
            usage.file = std::move(report.file);
            usage.strict = report.strict;
            usage.variants = std::move(report.variants);
            usage.usage = true;
            usage.findings = std::move(wrong);
            return usage;
        }
    }
    sort_findings(report.findings);
    return report;
}

} // namespace vehicle_sil::launch
