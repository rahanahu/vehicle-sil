// SPDX-License-Identifier: MIT
//
// plan_run(), planned_outcome(): what is settled about a run before it begins. Pure.
#include "vehicle_sil/launch/run_plan.hpp"

#include "vehicle_sil/launch/scenario_run.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::launch {

RunPlan plan_run(const CheckReport& check, std::filesystem::path base, const RunOptions& options)
{
    if (!check.config)
    {
        throw std::invalid_argument("plan_run: the check has no configuration (CheckReport::config is empty); call it only "
                                    "with a check that passed (CheckReport::passed())");
    }
    RunPlan plan;
    plan.check = detail::with_bridges(check);
    plan.base = std::move(base);
    const SimulationConfig& config = plan.config();
    // 1 with the time lock, whatever was asked for (the check refuses another --speed).
    const std::optional<double> speed = planned_speed(plan.check.time_lock, options.speed, config.speed);
    // With a scenario the step does not follow the speed: the same steps at every speed.
    plan.timing = detail::plan_time_step(config, plan.check, speed, plan.scenario() != nullptr);
    plan.timing.speed = speed;
    plan.timing.coarse_step_checked = std::any_of(plan.check.findings.begin(), plan.check.findings.end(),
                                                  [](const Finding& f) { return f.code == "coarse-time-step"; });
    plan.timing.late_tolerance_ms = late_tolerance_ms(config);
    plan.strict = options.check.strict;
    plan.holds_before_start = options.start_paused && options.control_endpoint != nullptr;
    plan.log_dir = options.log_dir;
    plan.timeline = options.timeline;
    plan.timeline_shown = options.timeline_shown;
    plan.poll = options.poll;
    plan.outside_lost_after = options.outside_lost_after;
    plan.timeout = options.timeout;
    plan.control_hello_timeout = options.control_hello_timeout;
    return plan;
}

RunOutcome planned_outcome(const RunPlan& plan)
{
    const SimulationConfig& config = plan.config();
    const CheckReport& check = plan.check;
    RunOutcome outcome;
    outcome.start_timeout_ms = config.start_timeout_ms;
    outcome.stop_timeout_ms = config.stop_timeout_ms;
    outcome.control_hello_timeout_ms = plan.control_hello_timeout.count();
    outcome.stop_at_ms = config.stop_at_ms;
    outcome.time_lock = check.time_lock;
    outcome.real_time_peers = detail::real_time_peers(config, check);
    if (const Scenario* scenario = plan.scenario(); scenario != nullptr)
    {
        outcome.scenario = scenario_not_run(*scenario);
        outcome.scenario->file = check.scenario->file;
    }
    outcome.timing = plan.timing;
    if (plan.timeout)
    {
        outcome.timeout_ms = plan.timeout->count();
    }
    for (const auto& p : config.participants)
    {
        ParticipantRun run;
        run.name = p.name;
        run.required = p.required;
        run.describe = p.describe;
        run.enabled = p.enabled;
        run.manual = p.manual;
        run.for_network = p.for_network;
        run.line = p.line;
        outcome.participants.push_back(std::move(run));
    }
    for (const NetworkOutside& o : check.outside)
    {
        outcome.outside.push_back(RunOutside{o.network, o.target, o.participant, std::nullopt, std::nullopt, o.tool});
    }
    return outcome;
}

} // namespace vehicle_sil::launch
