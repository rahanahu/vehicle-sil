// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/request.hpp"

#include <algorithm>
#include <cmath>

namespace vehicle_sil::launch {
namespace {

config::Finding usage(std::string message)
{
    config::Finding f;
    f.code = "usage";
    f.message = std::move(message);
    return f;
}

bool has(const std::vector<std::string>& names, const std::string& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

/// Adds the names of `more` (the file's list `key`) that are not in `names` yet and not in
/// `against` or `against2`, and notes each in `from_file` as "<key> <name>".
bool add_names(std::vector<std::string>& names, const std::vector<std::string>& more, const std::vector<std::string>& against,
               const std::vector<std::string>& against2, const std::string& key, std::vector<std::string>& from_file)
{
    bool added = false;
    for (const std::string& name : more)
    {
        if (!has(names, name) && !has(against, name) && !has(against2, name))
        {
            names.push_back(name);
            from_file.push_back(key + " " + name);
            added = true;
        }
    }
    return added;
}

} // namespace

std::vector<config::Finding> validate_timing(const RunRequest& request)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    config::TimingValues values;
    values.stop_at_ms = intent.stop_at_ms;
    values.late_tolerance_ms = intent.late_tolerance_ms;
    values.speed = intent.speed;
    values.start_timeout_ms = host.start_timeout_ms;
    values.stop_timeout_ms = host.stop_timeout_ms;
    if (host.timeout)
    {
        values.timeout_ms = host.timeout->count();
    }
    values.describe_timeout_ms = host.describe_timeout.count();
    return config::validate_timing_values(values);
}

std::vector<config::Finding> validate_request(const RunRequest& request, RequestCommand command)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    const config::RequestOrigin& origin = request.origin;
    const std::string scenario = origin.named("scenario", "--scenario", "scenario");
    const std::string paused = origin.named("start_paused", "--start-paused", "start_paused");
    std::vector<config::Finding> out = validate_timing(request);
    // A test takes the scenarios of its own list: a parameter is for each of them.
    // The params of a request file wait for a run with a scenario: one file serves runs with and without.
    const bool typed = std::any_of(intent.params.begin(), intent.params.end(),
                                   [](const config::ParamValue& p) { return p.source != config::ParamSource::RequestFile; });
    if (command != RequestCommand::Test && typed && !intent.scenario)
    {
        out.push_back(usage("--param sets a parameter of a scenario: give --scenario <file> too, or leave --param out"));
    }
    if (!host.timeline.empty() && !intent.scenario)
    {
        out.push_back(usage("--timeline writes what a scenario did over the time: give --scenario <file> too, or leave "
                            "--timeline out"));
    }
    if (!host.control_metrics.empty() && command != RequestCommand::Run)
    {
        out.push_back(usage(command == RequestCommand::Check
                                ? "--control-metrics is for run: check starts no participant"
                                : "--control-metrics is for run, not for test (which runs multiple simulations into one "
                                  "summary): run one simulation with vehicle_sil_launch run --control-metrics <file>"));
    }
    if (intent.scenario && intent.start_paused)
    {
        // A pause that comes before the simulation runs makes a scenario's run stand still at 0 too
        // (the gate's pause, after the start window). What is refused here is the hold before the
        // start (hold_before_start(), which --start-paused with a control server is): where it
        // comes among the scenario's writes for 0 in the start window is not settled.
        out.push_back(usage(scenario + " cannot be used with " + paused +
                            ": the scenario writes its values for 0 before the first step and runs by itself; pause "
                            "and step it with --stdin-commands or from a Control Server client (--control) if needed"));
    }
    // What a check reads instead of the file and of the executables (a run and a test read both
    // themselves, as they start them), and the file it compares the commands with.
    if (command != RequestCommand::Check)
    {
        if (host.config_from_stdin)
        {
            out.push_back(usage("--text-from-stdin is for check: run and test read the configuration file itself, so "
                                "leave it out"));
        }
        if (!host.descriptions.empty())
        {
            out.push_back(usage("--descriptions is for check: run and test run --describe of each participant again "
                                "before they start it, so leave it out"));
        }
        if (!host.write_descriptions.empty())
        {
            out.push_back(usage("--write-descriptions is for check: leave it out, and write the descriptions with "
                                "vehicle_sil_launch check --write-descriptions <file>"));
        }
        if (!host.compare_commands.empty())
        {
            out.push_back(usage("--compare-commands is for check: leave it out, and compare the commands with "
                                "vehicle_sil_launch check --compare-commands <file>"));
        }
    }
    // A check starts nothing: it has neither of the two options, and nothing waits.
    if (command != RequestCommand::Check && intent.start_paused && !host.stdin_commands && !host.control)
    {
        out.push_back(usage(paused + " needs --stdin-commands or --control: without commands nothing could make the "
                                     "simulation go on (resume, step)"));
    }
    if (intent.after_scenario == config::AfterScenario::Pause)
    {
        const std::string after = origin.named("after_scenario", "after_scenario", "after_scenario");
        if (!intent.scenario)
        {
            out.push_back(usage(after + " \"pause\" requires a scenario: give --scenario <file> or \"scenario\" in the "
                                        "request, or use \"stop\""));
        }
        if (intent.stop_at_ms)
        {
            const std::string stopAt = origin.named("stop_at_ms", "--stop-at-ms", "stop_at_ms");
            out.push_back(usage(after + " \"pause\" cannot be used with " + stopAt +
                                ": a paused handover keeps the run open instead of stopping at a stop time"));
        }
        if (command == RequestCommand::Test)
        {
            out.push_back(usage("test does not accept " + after + " \"pause\": test runs scenarios to completion"));
        }
        if (command == RequestCommand::Run && !host.stdin_commands && !host.control)
        {
            out.push_back(usage(after + " \"pause\" needs --stdin-commands or --control: without commands nothing "
                                        "could resume or step the paused simulation"));
        }
    }
    return out;
}

std::vector<config::ConnectRequest> merge_connect(std::span<const config::ConnectRequest> arguments, std::span<const config::ConnectRequest> file,
                                          std::vector<std::string>* from_file)
{
    std::vector<config::ConnectRequest> out(arguments.begin(), arguments.end());
    for (const config::ConnectRequest& written : file)
    {
        const bool given = std::any_of(arguments.begin(), arguments.end(),
                                       [&](const config::ConnectRequest& a) { return a.network == written.network; });
        if (given || written.target == config::kNoTarget)
        {
            continue;
        }
        out.push_back(written);
        if (from_file != nullptr)
        {
            from_file->push_back(written.network);
        }
    }
    return out;
}

RunRequest with_request_file(const RunRequest& arguments, const RequestFile& file, bool time_policy_given)
{
    RunRequest out = arguments;
    RunIntent& intent = out.intent;
    const RunIntent& given = arguments.intent;
    const RunIntent& written = file.intent;
    config::RequestOrigin& origin = out.origin;
    origin = {};
    origin.file = file.shown;
    const auto wrote = [&](const char* key) { return std::find(file.fields.begin(), file.fields.end(), key) != file.fields.end(); };
    if (!given.variant && written.variant)
    {
        intent.variant = written.variant;
        origin.fields.emplace_back("variant");
    }
    if (!given.scenario && written.scenario)
    {
        intent.scenario = written.scenario;
        origin.fields.emplace_back("scenario");
    }
    if (!given.speed && written.speed)
    {
        intent.speed = written.speed;
        origin.fields.emplace_back("speed");
    }
    if (!time_policy_given && wrote("time_policy"))
    {
        intent.time_policy = written.time_policy;
        origin.fields.emplace_back("time_policy");
    }
    if (!given.start_paused && written.start_paused)
    {
        intent.start_paused = true;
        origin.fields.emplace_back("start_paused");
    }
    if (!given.strict && written.strict)
    {
        intent.strict = true;
        origin.fields.emplace_back("strict");
    }
    if (!given.stop_at_ms && written.stop_at_ms)
    {
        intent.stop_at_ms = written.stop_at_ms;
        origin.fields.emplace_back("stop_at_ms");
    }
    if (!given.late_tolerance_ms && written.late_tolerance_ms)
    {
        intent.late_tolerance_ms = written.late_tolerance_ms;
        origin.fields.emplace_back("late_tolerance_ms");
    }
    if (given.after_scenario == config::AfterScenario::Stop && wrote("after_scenario") &&
        written.after_scenario != config::AfterScenario::Stop)
    {
        intent.after_scenario = written.after_scenario;
        origin.fields.emplace_back("after_scenario");
    }
    bool params = false;
    for (const config::ParamValue& param : written.params)
    {
        if (std::none_of(given.params.begin(), given.params.end(), [&](const config::ParamValue& p) { return p.name == param.name; }))
        {
            intent.params.push_back(param);
            params = true;
        }
    }
    if (params)
    {
        origin.fields.emplace_back("params");
    }
    intent.connect = merge_connect(given.connect, written.connect, &origin.connected);
    if (!origin.connected.empty())
    {
        origin.fields.emplace_back("connect");
    }
    if (add_names(intent.disable, written.disable, given.enable, given.manual, "disable", origin.participants))
    {
        origin.fields.emplace_back("disable");
    }
    if (add_names(intent.enable, written.enable, given.disable, {}, "enable", origin.participants))
    {
        origin.fields.emplace_back("enable");
    }
    if (add_names(intent.manual, written.manual, given.disable, {}, "manual", origin.participants))
    {
        origin.fields.emplace_back("manual");
    }
    return out;
}

} // namespace vehicle_sil::launch
