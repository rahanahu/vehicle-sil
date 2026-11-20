// SPDX-License-Identifier: MIT
//
// check_file(): the steps of the check. Reads the configuration (config.cpp), runs --describe of
// each enabled participant (or takes its saved output: --descriptions) and reads its output
// (describe_read.cpp), compares the descriptions with the file (check_rules.cpp), reads the
// scenario (scenario.cpp). What it needs from the computer (the files, the executables and their
// output, the environment) it gets from probe.cpp, in the order of the steps; the executables only
// through Executables. The outputs are in check_format.cpp; the counts of the report in
// check_report.cpp.
#include "format/exit_codes.hpp"

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/config/time_plan.hpp"
#include "vehicle_sil/config/detail/seen_ecu.hpp"

#include "vehicle_sil/core/registry_uri.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

namespace {

/// The names of the networks in `outside`.
std::vector<std::string> outside_names(const std::vector<config::NetworkOutside>& outside)
{
    std::vector<std::string> names;
    for (const config::NetworkOutside& network : outside)
    {
        names.push_back(network.network);
    }
    return names;
}

/// Empties NetworkOutside::arguments of a network that an error about connecting it is about: a
/// command that a run cannot use is not shown.
void drop_unusable_commands(config::CheckReport& report)
{
    for (config::NetworkOutside& outside : report.outside)
    {
        if (std::any_of(report.findings.begin(), report.findings.end(), [&](const config::Finding& f) {
                return f.severity == config::Severity::Error && f.network == outside.network && f.code.rfind("connect-", 0) == 0;
            }))
        {
            outside.arguments.reset();
        }
    }
}

/// What the check asks of executables: each participant's --describe, and the bridges of --connect
/// (looked for and asked --describe). Made once, from the request: with saved descriptions
/// (RunHost::descriptions) neither run_tool() nor bridge_tools() is called to make it, so such a
/// check has nothing that could start a process.
struct Executables
{
    DescribeSource describe;
    /// Empty: --connect is checked for its form and its names only (check_connections() without tools).
    std::optional<config::ConnectTools> bridges;
    /// RunHost::descriptions could not be used: why (the note descriptions-not-used).
    std::string unusable{};
};

Executables executables_for(const RunHost& host, const std::filesystem::path& directory)
{
    if (host.descriptions.empty())
    {
        return Executables{describe_by_running(host.describe_timeout, directory),
                           bridge_tools(host.tool_directory, host.describe_timeout), {}};
    }
    SavedDescriptions saved = read_saved_descriptions(process::path_from_utf8(host.descriptions));
    return Executables{describe_from_saved(std::move(saved.entries)), std::nullopt, std::move(saved.problem)};
}

} // namespace

int check_exit_code(const config::CheckReport& report) { return exit_code_of(report); }

namespace {

/// The note for the params of a request file that nothing uses: one file serves several
/// scenarios, so they are left alone.
[[nodiscard]] config::Finding unused_request_params(const std::string& file, const std::vector<std::string>& names, const std::string& why)
{
    std::string list;
    for (const std::string& name : names)
    {
        list += (list.empty() ? "" : ", ") + name;
    }
    return config::Finding{config::Severity::Note, "request-params-unused",
                   std::string("params ") + list + " in " + file + " " + (names.size() == 1 ? "is" : "are") +
                       " not used: " + why + "; they are ignored",
                   0, "", ""};
}

/// --scenario: reads the scenario file and checks it against the participants of `report`.
/// @returns the usage errors that only the scenario can tell (ScenarioRead::usage_errors); then
/// `report` has no scenario.
[[nodiscard]] std::vector<config::Finding> check_scenario_file(Checked& report, const RunIntent& intent, const std::string& named,
                                                       bool params_for_many,
                                                       const config::RequestOrigin& origin)
{
    scenario::ScenarioCheck check;
    check.file = named;
    const auto add = [&](config::Finding finding) {
        finding.file = check.file;
        report.findings.push_back(std::move(finding));
    };
    const std::filesystem::path path = process::path_from_utf8(check.file);
    std::string text;
    const std::string problem = read_scenario_file(path, text);
    if (!problem.empty())
    {
        add(config::Finding{config::Severity::Error, "file", problem, 0, "", ""});
    }
    else
    {
        scenario::ScenarioContext context;
        context.variant = report.config->variant;
        context.variants = report.variants;
        context.params = report.config->params;
        context.params.insert(context.params.end(), intent.params.begin(), intent.params.end());
        context.params_for_many = params_for_many;
        if (origin.has("scenario"))
        {
            context.request_file = origin.file; // the file's own scenario: its params must fit it
        }
        context.read_file = files_beside(path);
        scenario::ScenarioRead read = scenario::read_scenario(text, scenario::scenario_catalog(report), context);
        check.name = std::move(read.name);
        check.steps = read.steps;
        check.scenario = std::move(read.scenario);
        check.params = std::move(read.params);
        check.only = std::move(read.only);
        check.skipped = read.skipped;
        check.unchecked_expects = read.unchecked_expects;
        check.not_for_variant = read.not_for_variant;
        if (!read.usage_errors.empty())
        {
            return std::move(read.usage_errors);
        }
        if (!read.unused_request_params.empty())
        {
            report.findings.push_back(unused_request_params(origin.file, read.unused_request_params,
                                                            "the scenario has no parameter of that name"));
        }
        for (config::Finding& finding : read.findings)
        {
            add(std::move(finding));
        }
        if (check.scenario)
        {
            // What the run will judge with: how often the ECUs it reads send their values.
            for (config::Finding& finding : scenario::scenario_samples_findings(report, *check.scenario, version()))
            {
                add(std::move(finding));
            }
        }
    }
    report.scenario = std::move(check);
    return {};
}

/// check --compare-commands: compares what runs of `text` with what runs of the configuration file
/// `other` (read as the configuration file is, from the working directory): CheckReport::commands
/// and its note.
void compare_commands_with(Checked& report, std::string_view text, const std::string& other)
{
    std::string other_text;
    if (const std::string problem = read_config_file(process::path_from_utf8(other), other_text); !problem.empty())
    {
        report.commands = config::CommandsCompared::NotCompared;
        report.findings.push_back(config::commands_not_compared(other, problem));
        return;
    }
    config::CommandsComparison compared = config::compare_commands(text, other_text, other);
    report.commands = compared.result;
    if (compared.note)
    {
        report.findings.push_back(std::move(*compared.note));
    }
}

} // namespace

Checked check_file(const RunRequest& request, const CheckOptions& options,
                       const std::function<void(const config::TimeLock& lock)>& on_time_lock)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    const std::filesystem::path path = process::path_from_utf8(host.config);
    config::EnabledOverrides overrides;
    overrides.disable = intent.disable;
    overrides.enable = intent.enable;
    overrides.manual = intent.manual;
    overrides.variant = intent.variant;
    overrides.origin = request.origin;
    // The speed the run was asked for; a test's is its default, which is not judged.
    const std::optional<std::optional<double>> run_speed =
        options.fastest_by_default ? std::optional<std::optional<double>>{} : intent.speed;
    // What of the request's connect counts for this file: known once it is read.
    std::vector<config::ConnectRequest> connect;
    Checked report;
    report.file = host.config;
    report.strict = intent.strict;
    // Known without the file.
    if (auto invalid = validate_timing(request); !invalid.empty())
    {
        report.usage = true;
        report.findings = std::move(invalid);
        return report;
    }
    if (auto conflicts = config::override_conflicts(overrides); !conflicts.empty())
    {
        report.usage = true;
        report.findings = std::move(conflicts);
        return report;
    }
    std::string text;
    if (const std::string problem = host.config_from_stdin ? read_config_stdin(text) : read_config_file(path, text);
        !problem.empty())
    {
        report.findings.push_back(config::Finding{config::Severity::Error, "file", problem, 0, "", ""});
        if (!host.compare_commands.empty())
        {
            report.commands = config::CommandsCompared::NotCompared;
            report.findings.push_back(config::commands_not_compared(host.compare_commands, config::kCheckedTextUnreadable));
        }
        return report;
    }
    config::ConfigRead read = config::read_config(text, overrides);
    if (read.config)
    {
        for (const config::VariantConfig& variant : read.config->variants)
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
    // It only reads: also for file_only, and whatever the rest of the check finds.
    if (!host.compare_commands.empty())
    {
        compare_commands_with(report, text, host.compare_commands);
    }
    if (report.config)
    {
        config::ConfigConnect counted = config::connect_for_config(*report.config, intent.connect, request.origin);
        connect = std::move(counted.connect);
        report.findings.insert(report.findings.end(), counted.notes.begin(), counted.notes.end());
        // The file and the request decide it: before any executable is asked.
        report.time_lock = config::time_lock(*report.config, connect, intent.time_policy);
        const std::vector<config::Finding> time =
            config::time_lock_findings(report.time_lock, run_speed,
                               std::optional<double>{}, intent.start_paused,
                               request.origin);
        report.findings.insert(report.findings.end(), time.begin(), time.end());
        if (on_time_lock)
        {
            on_time_lock(report.time_lock);
        }
    }
    if (report.config && options.file_only && !connect.empty())
    {
        // Only the form and the names: no tool is looked for.
        config::ConnectCheck checked = config::check_connections(*report.config, {}, connect, nullptr);
        report.findings.insert(report.findings.end(), checked.findings.begin(), checked.findings.end());
        report.outside = std::move(checked.outside);
        drop_unusable_commands(report);
    }
    if (!report.config || options.file_only)
    {
        return report;
    }
    const std::optional<std::string> listen = environment_value(kListenOnNetworkVariable);
    if (auto ignored = config::inherited_listen_variable(*report.config, listen ? listen->c_str() : nullptr))
    {
        report.findings.push_back(std::move(*ignored));
    }
    const std::filesystem::path base = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    // As vehicle_sil_launch run starts them: in the directory of the configuration file.
    const Executables executables = executables_for(host, std::filesystem::absolute(base));
    if (!executables.unusable.empty())
    {
        report.findings.push_back(config::Finding{config::Severity::Note, "descriptions-not-used",
                                          "the saved descriptions (--descriptions) are not used: " + executables.unusable +
                                              "; no participant is described: check without --descriptions to describe "
                                              "them, with --write-descriptions to save them again",
                                          0, "", ""});
    }
    // What --write-descriptions saves: each output that was read.
    std::vector<SavedDescription> described;
    for (const config::ParticipantConfig& p : report.config->participants)
    {
        config::ParticipantResult result;
        // Disabled whatever its entry's errors (they are findings).
        if (!p.enabled)
        {
            result.status = config::ParticipantStatus::Disabled;
            report.participants.push_back(result);
            continue;
        }
        if (!p.valid)
        {
            report.participants.push_back(result);
            continue;
        }
        // "{{word}}" in the path is the literal "{word}" (a valid entry has no other "{word}").
        const std::string written = config::expand_placeholders({p.command.front()}, "", "").front();
        const FoundExecutable found = find_executable(written, base);
        const std::filesystem::path& executable = found.path;
        if (auto bad = config::executable_finding(p, executable, executable_status(executable)))
        {
            report.findings.push_back(std::move(*bad));
            result.status = config::ParticipantStatus::Failed;
            report.participants.push_back(result);
            continue;
        }
        result.executable = executable;
        if (found.on_path)
        {
            report.findings.push_back(config::Finding{config::Severity::Note, "found-on-path",
                                              "participant '" + p.name + "': " + written +
                                                  " is not in the directory of the configuration file; it runs " +
                                                  process::shown_path(executable) + " (found on PATH)",
                                              p.command_line, p.name, ""});
        }
        if (!p.describe)
        {
            result.status = config::ParticipantStatus::NotChecked;
            report.participants.push_back(result);
            continue;
        }
        // "{{word}}" stands for "{word}" here too (an ecu_main command has no placeholders).
        std::vector<std::string> arguments =
            config::expand_placeholders(std::vector<std::string>(p.command.begin() + 1, p.command.end()), "", "");
        arguments.insert(arguments.end(), {"--describe", "--name", p.name});
        // --write-descriptions: the executable as it was before --describe ran, so that one built
        // again meanwhile does not get the output of the one before.
        const std::optional<ExecutableStamp> before =
            host.write_descriptions.empty() ? std::nullopt : executable_stamp(executable);
        const std::optional<config::DescribeRun> run = executables.describe(p, executable, arguments);
        if (!run)
        {
            result.status = config::ParticipantStatus::NotDescribed;
            report.participants.push_back(std::move(result));
            continue;
        }
        config::DescribeOutcome outcome = config::interpret_describe(p, *run);
        if (outcome.error)
        {
            report.findings.push_back(std::move(*outcome.error));
            result.status = config::ParticipantStatus::Failed;
        }
        else
        {
            result.status = config::ParticipantStatus::Described;
            result.ecu = std::move(outcome.ecu);
            if (before && before == executable_stamp(executable))
            {
                SavedDescription& saved = described.emplace_back();
                saved.name = p.name;
                saved.command = p.command;
                saved.executable = *before;
                saved.output = run->standard_output;
            }
        }
        report.participants.push_back(std::move(result));
    }
    if (!host.write_descriptions.empty())
    {
        if (const std::string problem =
                write_saved_descriptions(process::path_from_utf8(host.write_descriptions), described);
            !problem.empty())
        {
            report.findings.push_back(config::Finding{config::Severity::Error, "descriptions-write",
                                              "cannot write the descriptions to " + host.write_descriptions +
                                                  " (--write-descriptions): " + problem +
                                                  "; the rest of this check is not affected: name a file in a directory "
                                                  "that exists and may be written",
                                              0, "", ""});
        }
    }
    if (!connect.empty())
    {
        config::ConnectCheck checked = config::check_connections(*report.config, report.participants, connect,
                                                 executables.bridges ? &*executables.bridges : nullptr);
        report.findings.insert(report.findings.end(), checked.findings.begin(), checked.findings.end());
        report.outside = std::move(checked.outside);
    }
    const std::vector<std::string> outside = outside_names(report.outside);
    auto compared = config::check_participants(*report.config, report.participants, version(), outside);
    report.findings.insert(report.findings.end(), compared.begin(), compared.end());
    drop_unusable_commands(report);
    report.run_timing = config::detail::plan_time_step(*report.config, report, intent.stop_at_ms);
    report.run_timing->speed = config::planned_speed(report.time_lock, run_speed, {});
    report.run_timing->late_tolerance_ms = intent.late_tolerance_ms.value_or(config::kDefaultLateToleranceMs);
    report.stop_at_ms = intent.stop_at_ms;
    report.start_timeout_ms = host.start_timeout_ms;
    report.stop_timeout_ms = host.stop_timeout_ms;
    report.checked_speed = run_speed;
    report.checked_start_paused = intent.start_paused;
    if (intent.stop_at_ms && *intent.stop_at_ms % report.run_timing->time_step_ms != 0)
    {
        report.findings.push_back(config::Finding{config::Severity::Error, "stop-not-on-step",
            "stop_at_ms (" + std::to_string(*intent.stop_at_ms) + ") is not a multiple of time_step_ms (" +
                std::to_string(report.run_timing->time_step_ms) + ")", 0, "", ""});
    }
    if (auto f = config::detail::coarse_time_step(*report.run_timing))
    {
        report.findings.push_back(std::move(*f));
    }
    if (intent.scenario && !intent.scenario->empty())
    {
        // A request file's scenario is beside the configuration, as the file's own paths are.
        std::string named = *intent.scenario;
        if (request.origin.has("scenario") && process::path_from_utf8(named).is_relative() && !path.parent_path().empty())
        {
            named = process::shown_path(path.parent_path() / process::path_from_utf8(named));
        }
        if (auto wrong = check_scenario_file(report, intent, named, options.params_for_many, request.origin); !wrong.empty())
        {
            // As for the other usage errors: the findings alone, and no configuration.
            Checked usage;
            usage.file = std::move(report.file);
            usage.strict = report.strict;
            usage.variants = std::move(report.variants);
            usage.usage = true;
            usage.findings = std::move(wrong);
            return usage;
        }
    }
    else
    {
        std::vector<std::string> unused;
        for (const config::ParamValue& param : intent.params)
        {
            if (param.source == config::ParamSource::RequestFile)
            {
                unused.push_back(param.name);
            }
        }
        if (!unused.empty())
        {
            report.findings.push_back(unused_request_params(request.origin.file, unused, "this run has no scenario"));
        }
    }
    config::sort_findings(report.findings);
    return report;
}

} // namespace vehicle_sil::launch
