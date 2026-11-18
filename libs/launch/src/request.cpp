// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/request.hpp"

#include "vehicle_sil/process/paths.hpp"

namespace vehicle_sil::launch {
namespace {

Finding usage(std::string message)
{
    Finding f;
    f.code = "usage";
    f.message = std::move(message);
    return f;
}

} // namespace

std::vector<Finding> validate_request(const RunRequest& request, RequestCommand command)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    std::vector<Finding> out;
    // A test takes the scenarios of its own list: a parameter is for each of them.
    if (command != RequestCommand::Test && !intent.params.empty() && !intent.scenario)
    {
        out.push_back(usage("--param sets a parameter of a scenario: give --scenario <file> too, or leave --param out"));
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
        out.push_back(usage("--scenario cannot be used with --start-paused: the scenario writes its values for 0 before "
                            "the first step and runs by itself; pause and step it with --stdin-commands if needed"));
    }
    if (intent.start_paused && !host.stdin_commands && !host.control)
    {
        out.push_back(usage("--start-paused needs --stdin-commands or --control: without commands nothing could make the "
                            "simulation go on (resume, step)"));
    }
    return out;
}

CheckOptions check_options_of(const RunRequest& request)
{
    const RunIntent& intent = request.intent;
    const RunHost& host = request.host;
    CheckOptions out;
    out.describe_timeout = host.describe_timeout;
    out.strict = intent.strict;
    out.overrides.variant = intent.variant;
    out.overrides.disable = intent.disable;
    out.overrides.enable = intent.enable;
    out.overrides.manual = intent.manual;
    out.params = intent.params;
    out.scenario_shown = intent.scenario.value_or("");
    out.scenario = process::path_from_utf8(out.scenario_shown);
    out.connect = intent.connect;
    out.tool_directory = host.tool_directory;
    out.time_policy = intent.time_policy;
    return out;
}

} // namespace vehicle_sil::launch
