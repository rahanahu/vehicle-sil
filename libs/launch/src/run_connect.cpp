// SPDX-License-Identifier: MIT
//
// run --connect: the bridges that run starts are participants of the run only (with_bridges()).
// They go at the end of the participants, so that everything that is kept per participant (the
// result, the logs, the events, the processes) has them without a list of its own. How they are
// started and when they stop the run: run.cpp, run_participants.cpp, run_start.cpp and
// run_watch.cpp; their findings: run_assess.cpp.
#include "vehicle_sil/launch/run_detail.hpp"

#include <algorithm>
#include <utility>

namespace vehicle_sil::launch::detail {

CheckReport with_bridges(const CheckReport& check)
{
    CheckReport out = check;
    if (!out.config)
    {
        return out;
    }
    for (const NetworkOutside& outside : check.outside)
    {
        const auto network = std::find_if(out.config->networks.begin(), out.config->networks.end(),
                                          [&outside](const NetworkConfig& n) { return n.name == outside.network; });
        ParticipantConfig p;
        p.name = outside.participant;
        p.command = {outside.tool};
        p.describe = true;
        p.required = false;
        p.enabled = true;
        p.role = ParticipantRole::Bridge;
        p.role_written = true;
        p.line = network != out.config->networks.end() ? network->line : 0;
        p.command_line = p.line;
        p.step_line = p.line;
        out.config->participants.push_back(std::move(p));
        ParticipantResult result;
        result.status = ParticipantStatus::NotChecked;
        result.executable = outside.tool_path;
        out.participants.push_back(std::move(result));
    }
    return out;
}

} // namespace vehicle_sil::launch::detail
