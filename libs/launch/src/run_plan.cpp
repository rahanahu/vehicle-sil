// SPDX-License-Identifier: MIT
//
// plan_run(), planned_outcome(): what is settled about a run before it begins. Pure.
#include "vehicle_sil/launch/run_plan.hpp"

#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run_detail.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::launch {

RunPlan plan_run(const Checked& check, std::filesystem::path base, const RunRequest& request, const RunLimits& limits,
                 bool control_server)
{
    if (!check.config)
    {
        throw std::invalid_argument("plan_run: the check has no configuration (CheckReport::config is empty); call it only "
                                    "with a check that passed (CheckReport::passed())");
    }
    RunPlan plan;
    plan.check = detail::with_bridges(check);
    plan.scenario_check = check.scenario;
    // with_bridges() adds one participant for each network outside, in their order, after those of
    // the file.
    plan.members.resize(check.config->participants.size());
    for (const NetworkOutside& outside : check.outside)
    {
        plan.members.emplace_back().outside = outside;
    }
    plan.base = std::move(base);
    // The file's participants only: a bridge of --connect runs in real time, the gate does not move it.
    const SimulationConfig& config = *check.config;
    // 1 with the time lock, whatever was asked for (the check refuses another --speed).
    const std::optional<double> speed = planned_speed(plan.check.time_lock, request.intent.speed, config.speed);
    // With a scenario the step does not follow the speed: the same steps at every speed.
    plan.timing = detail::plan_time_step(config, check, speed, plan.scenario() != nullptr);
    plan.timing.speed = speed;
    plan.timing.coarse_step_checked = std::any_of(plan.check.findings.begin(), plan.check.findings.end(),
                                                  [](const Finding& f) { return f.code == "coarse-time-step"; });
    plan.timing.late_tolerance_ms = late_tolerance_ms(config);
    plan.strict = request.intent.strict;
    plan.holds_before_start = request.intent.start_paused && control_server;
    plan.log_dir = request.host.log_dir;
    plan.timeline = process::path_from_utf8(request.host.timeline);
    plan.timeline_shown = request.host.timeline;
    plan.poll = limits.poll;
    plan.outside_lost_after = limits.outside_lost_after;
    plan.timeout = request.host.timeout;
    plan.control_hello_timeout = limits.control_hello_timeout;
    return plan;
}

RunOutcome planned_outcome(const RunPlan& plan)
{
    const SimulationConfig& config = plan.config();
    const CheckReport& check = plan.check;
    RunOutcome outcome;
    outcome.start.timeout_ms = config.start_timeout_ms;
    outcome.end.stop_timeout_ms = config.stop_timeout_ms;
    outcome.start.control_hello_timeout_ms = plan.control_hello_timeout.count();
    outcome.end.stop_at_ms = config.stop_at_ms;
    outcome.time_lock = check.time_lock;
    outcome.real_time_peers = detail::real_time_peers(config, check);
    if (const Scenario* scenario = plan.scenario(); scenario != nullptr)
    {
        outcome.scenario.result = scenario_not_run(*scenario);
        outcome.scenario.result->file = plan.scenario_check->file;
    }
    outcome.timing = plan.timing;
    if (plan.timeout)
    {
        outcome.end.timeout_ms = plan.timeout->count();
    }
    for (std::size_t i = 0; i < config.participants.size(); ++i)
    {
        const ParticipantConfig& p = config.participants[i];
        // Made in place: MinGW's GCC took the fields of a local that was then moved for uninitialized
        // (-Wmaybe-uninitialized), although every one has its initializer.
        ParticipantRun& run = outcome.participants.emplace_back();
        run.name = p.name;
        run.required = p.required;
        run.describe = p.describe;
        run.enabled = p.enabled;
        run.manual = p.manual;
        if (i < plan.members.size() && plan.members[i].outside)
        {
            run.for_network = plan.members[i].outside->network;
        }
        run.line = p.line;
    }
    for (const NetworkOutside& o : check.outside)
    {
        outcome.outside.push_back(RunOutside{o.network, o.target, o.participant, std::nullopt, std::nullopt, o.tool});
    }
    return outcome;
}

} // namespace vehicle_sil::launch
