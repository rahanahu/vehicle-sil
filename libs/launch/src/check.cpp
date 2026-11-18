// SPDX-License-Identifier: MIT
//
// check_file(): the steps of the check. Reads the configuration (config.cpp), runs --describe of
// each enabled participant and reads its output (describe_read.cpp), compares the descriptions
// with the file (check_rules.cpp), reads the scenario (scenario.cpp). What it needs from the
// computer (the files, the executables and their output, the environment) it gets from probe.cpp,
// in the order of the steps. The outputs are in check_format.cpp.
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/time_plan.hpp"
#include "seen_ecu.hpp"

#include "vehicle_sil/core/registry_uri.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

namespace {

/// The names of the networks in `outside`.
std::vector<std::string> outside_names(const std::vector<NetworkOutside>& outside)
{
    std::vector<std::string> names;
    for (const NetworkOutside& network : outside)
    {
        names.push_back(network.network);
    }
    return names;
}

/// Empties NetworkOutside::arguments of a network that an error about connecting it is about: a
/// command that a run cannot use is not shown.
void drop_unusable_commands(CheckReport& report)
{
    for (NetworkOutside& outside : report.outside)
    {
        if (std::any_of(report.findings.begin(), report.findings.end(), [&](const Finding& f) {
                return f.severity == Severity::Error && f.network == outside.network && f.code.rfind("connect-", 0) == 0;
            }))
        {
            outside.arguments.reset();
        }
    }
}

} // namespace

ConfigConnect connect_for_config(const SimulationConfig& config, std::span<const ConnectRequest> connect,
                                 const RequestOrigin& origin)
{
    ConfigConnect out;
    for (const ConnectRequest& request : connect)
    {
        const bool fromFile =
            std::find(origin.connected.begin(), origin.connected.end(), request.network) != origin.connected.end();
        const bool known = std::any_of(config.networks.begin(), config.networks.end(),
                                       [&](const NetworkConfig& n) { return n.name == request.network; });
        if (fromFile && !known)
        {
            out.notes.push_back(Finding{Severity::Note, "connect-network-unused",
                                        "the network " + request.network + " of connect in " + origin.file +
                                            " is not in this configuration: it is left out",
                                        0, "", request.network});
            continue;
        }
        out.connect.push_back(request);
    }
    return out;
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

/// --scenario: reads the scenario file and checks it against the participants of `report`.
/// @returns the usage errors that only the scenario can tell (ScenarioRead::usage_errors); then
/// `report` has no scenario.
[[nodiscard]] std::vector<Finding> check_scenario_file(CheckReport& report, const RunIntent& intent, const std::string& named,
                                                       bool params_for_many)
{
    ScenarioCheck check;
    check.file = named;
    const auto add = [&](Finding finding) {
        finding.file = check.file;
        report.findings.push_back(std::move(finding));
    };
    const std::filesystem::path path = process::path_from_utf8(check.file);
    std::string text;
    const std::string problem = read_scenario_file(path, text);
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
        context.params.insert(context.params.end(), intent.params.begin(), intent.params.end());
        context.params_for_many = params_for_many;
        context.read_file = files_beside(path);
        ScenarioRead read = read_scenario(text, scenario_catalog(report), context);
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
        for (Finding& finding : read.findings)
        {
            add(std::move(finding));
        }
    }
    report.scenario = std::move(check);
    return {};
}

} // namespace

CheckReport check_file(const RunRequest& request, const CheckOptions& options,
                       const std::function<void(const TimeLock& lock)>& on_time_lock)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    const std::filesystem::path path = process::path_from_utf8(host.config);
    EnabledOverrides overrides;
    overrides.disable = intent.disable;
    overrides.enable = intent.enable;
    overrides.manual = intent.manual;
    overrides.variant = intent.variant;
    overrides.origin = request.origin;
    // The speed the run was asked for; a test's is its default, which is not judged.
    const std::optional<std::optional<double>> run_speed =
        options.fastest_by_default ? std::optional<std::optional<double>>{} : intent.speed;
    // What of the request's connect counts for this file: known once it is read.
    std::vector<ConnectRequest> connect;
    CheckReport report;
    report.file = host.config;
    report.strict = intent.strict;
    // Known without the file.
    if (auto conflicts = override_conflicts(overrides); !conflicts.empty())
    {
        report.usage = true;
        report.findings = std::move(conflicts);
        return report;
    }
    std::string text;
    if (const std::string problem = read_config_file(path, text); !problem.empty())
    {
        report.findings.push_back(Finding{Severity::Error, "file", problem, 0, "", ""});
        return report;
    }
    ConfigRead read = read_config(text, overrides);
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
    if (report.config)
    {
        ConfigConnect counted = connect_for_config(*report.config, intent.connect, request.origin);
        connect = std::move(counted.connect);
        report.findings.insert(report.findings.end(), counted.notes.begin(), counted.notes.end());
        // The file and the request decide it: before any executable is asked.
        report.time_lock = time_lock(*report.config, connect, intent.time_policy);
        const std::vector<Finding> time =
            time_lock_findings(report.time_lock, run_speed,
                               options.fastest_by_default ? std::optional<double>{} : report.config->speed, intent.start_paused,
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
        ConnectCheck checked = check_connections(*report.config, {}, connect, nullptr);
        report.findings.insert(report.findings.end(), checked.findings.begin(), checked.findings.end());
        report.outside = std::move(checked.outside);
        drop_unusable_commands(report);
    }
    if (!report.config || options.file_only)
    {
        return report;
    }
    const std::optional<std::string> listen = environment_value(kListenOnNetworkVariable);
    if (auto ignored = inherited_listen_variable(*report.config, listen ? listen->c_str() : nullptr))
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
        // As vehicle_sil_launch run starts it: in the directory of the configuration file.
        const DescribeRun run =
            run_tool(executable, p.command.front(), arguments, host.describe_timeout, std::filesystem::absolute(base));
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
    if (!connect.empty())
    {
        const ConnectTools tools = bridge_tools(host.tool_directory, host.describe_timeout);
        ConnectCheck checked = check_connections(*report.config, report.participants, connect, &tools);
        report.findings.insert(report.findings.end(), checked.findings.begin(), checked.findings.end());
        report.outside = std::move(checked.outside);
    }
    const std::vector<std::string> outside = outside_names(report.outside);
    auto compared = check_participants(*report.config, report.participants, version(), outside);
    report.findings.insert(report.findings.end(), compared.begin(), compared.end());
    drop_unusable_commands(report);
    // A run at the file's speed (1 with the time lock) with the file's time_step_ms: known before
    // it runs. A run with a speed of its own (run --speed, test) reports its own afterwards.
    if (const std::optional<double> speed = planned_speed(report.time_lock, {}, report.config->speed);
        !run_speed && !options.fastest_by_default && speed && report.config->time_step_ms)
    {
        RunTiming timing = detail::plan_time_step(*report.config, report, speed);
        timing.speed = speed;
        if (auto f = detail::coarse_time_step(timing))
        {
            report.findings.push_back(std::move(*f));
        }
    }
    if (intent.scenario && !intent.scenario->empty())
    {
        // A request file's scenario is beside the configuration, as the file's own paths are.
        std::string named = *intent.scenario;
        if (request.origin.has("scenario") && process::path_from_utf8(named).is_relative() && !path.parent_path().empty())
        {
            named = process::shown_path(path.parent_path() / process::path_from_utf8(named));
        }
        if (auto wrong = check_scenario_file(report, intent, named, options.params_for_many); !wrong.empty())
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
