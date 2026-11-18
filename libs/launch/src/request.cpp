// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/request.hpp"

#include <algorithm>

namespace vehicle_sil::launch {
namespace {

Finding usage(std::string message)
{
    Finding f;
    f.code = "usage";
    f.message = std::move(message);
    return f;
}

bool has(const std::vector<std::string>& names, const std::string& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

/// Adds the names of `more` that are not in `names` yet and not in `against` or `against2`.
bool add_names(std::vector<std::string>& names, const std::vector<std::string>& more, const std::vector<std::string>& against,
               const std::vector<std::string>& against2 = {})
{
    bool added = false;
    for (const std::string& name : more)
    {
        if (!has(names, name) && !has(against, name) && !has(against2, name))
        {
            names.push_back(name);
            added = true;
        }
    }
    return added;
}

} // namespace

std::vector<Finding> validate_request(const RunRequest& request, RequestCommand command)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    const RequestOrigin& origin = request.origin;
    const std::string scenario = origin.named("scenario", "--scenario", "scenario");
    const std::string paused = origin.named("start_paused", "--start-paused", "start_paused");
    std::vector<Finding> out;
    // A test takes the scenarios of its own list: a parameter is for each of them.
    if (command != RequestCommand::Test && !intent.params.empty() && !intent.scenario)
    {
        const bool fromFile = origin.has("params") && std::none_of(intent.params.begin(), intent.params.end(), [](const ParamValue& p) {
            return p.source == ParamSource::CommandLine;
        });
        out.push_back(usage(fromFile ? "params in " + origin.file +
                                           " sets parameters of a scenario: give --scenario <file> (or scenario in " +
                                           origin.file + ") too, or take params out of " + origin.file
                                     : std::string("--param sets a parameter of a scenario: give --scenario <file> too, or "
                                                   "leave --param out")));
    }
    if (!host.timeline.empty() && !intent.scenario)
    {
        out.push_back(usage("--timeline writes what a scenario did over the time: give --scenario <file> too, or leave "
                            "--timeline out"));
    }
    if (intent.scenario && intent.start_paused)
    {
        // Also with a control server: its clients cannot move the time during a scenario, so they
        // could not end the hold.
        out.push_back(usage(scenario + " cannot be used with " + paused +
                            ": the scenario writes its values for 0 before "
                            "the first step and runs by itself; pause and step it with --stdin-commands if needed"));
    }
    // A check starts nothing: it has neither of the two options, and nothing waits.
    if (command != RequestCommand::Check && intent.start_paused && !host.stdin_commands && !host.control)
    {
        out.push_back(usage(paused + " needs --stdin-commands or --control: without commands nothing could make the "
                                     "simulation go on (resume, step)"));
    }
    return out;
}

std::vector<ConnectRequest> merge_connect(std::span<const ConnectRequest> arguments, std::span<const ConnectRequest> file,
                                          std::vector<std::string>* from_file)
{
    std::vector<ConnectRequest> out(arguments.begin(), arguments.end());
    for (const ConnectRequest& written : file)
    {
        const bool given = std::any_of(arguments.begin(), arguments.end(),
                                       [&](const ConnectRequest& a) { return a.network == written.network; });
        if (given || written.target == kNoTarget)
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
    RequestOrigin& origin = out.origin;
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
    bool params = false;
    for (const ParamValue& param : written.params)
    {
        if (std::none_of(given.params.begin(), given.params.end(), [&](const ParamValue& p) { return p.name == param.name; }))
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
    if (add_names(intent.disable, written.disable, given.enable, given.manual))
    {
        origin.fields.emplace_back("disable");
    }
    if (add_names(intent.enable, written.enable, given.disable))
    {
        origin.fields.emplace_back("enable");
    }
    if (add_names(intent.manual, written.manual, given.disable))
    {
        origin.fields.emplace_back("manual");
    }
    return out;
}

} // namespace vehicle_sil::launch
