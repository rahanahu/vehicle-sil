// SPDX-License-Identifier: MIT
//
// plan_run(), planned_outcome(): what is settled about a run before it begins. Pure.
#include "vehicle_sil/runner/run_plan.hpp"

#include "vehicle_sil/core/registry_uri.hpp"
#include "vehicle_sil/runner/detail/run_detail.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::runner {

RunPlan plan_run(const config::CheckReport& check, const scenario::ScenarioCheck* scenario, std::filesystem::path base,
                 const RunChoices& choices, const RunLimits& limits, bool control_server)
{
    if (!check.config)
    {
        throw std::invalid_argument("plan_run: the check has no configuration (CheckReport::config is empty); call it only "
                                    "with a check that passed (CheckReport::passed())");
    }
    if (!check.passed())
    {
        throw std::invalid_argument("plan_run: the check did not pass; call it only with a passing CheckReport");
    }
    config::TimingValues values;
    values.stop_at_ms = choices.stop_at_ms;
    values.late_tolerance_ms = choices.late_tolerance_ms;
    values.speed = choices.speed;
    values.start_timeout_ms = choices.start_timeout_ms;
    values.stop_timeout_ms = choices.stop_timeout_ms;
    if (choices.timeout)
    {
        values.timeout_ms = choices.timeout->count();
    }
    values.time_step_ms = check.config->time_step_ms;
    if (!config::valid_timing_values(values))
    {
        throw std::invalid_argument("plan_run: timing values are outside their allowed ranges");
    }
    if (check.config->time_step_ms && !config::stop_on_time_step(choices.stop_at_ms, *check.config->time_step_ms))
    {
        throw std::invalid_argument("plan_run: stop_at_ms is not a multiple of time_step_ms");
    }
    if (check.run_timing &&
        (check.stop_at_ms != choices.stop_at_ms || check.checked_speed != choices.speed ||
         check.checked_start_paused != choices.start_paused ||
         check.checked_after_scenario != choices.after_scenario ||
         check.run_timing->late_tolerance_ms != choices.late_tolerance_ms ||
         check.run_timing->speed != config::planned_speed(check.time_lock, choices.speed, {}) ||
         !config::valid_positive_millis(check.run_timing->time_step_ms) ||
         check.start_timeout_ms != choices.start_timeout_ms || check.stop_timeout_ms != choices.stop_timeout_ms))
    {
        throw std::invalid_argument("plan_run: timing choices differ from the checked run; check the request again");
    }
    if (check.run_timing && !config::stop_on_time_step(choices.stop_at_ms, check.run_timing->time_step_ms))
    {
        throw std::invalid_argument("plan_run: checked time step does not divide stop_at_ms");
    }
    if (choices.after_scenario == config::AfterScenario::Pause)
    {
        if (scenario == nullptr || !scenario->scenario)
        {
            throw std::invalid_argument("plan_run: after_scenario=pause requires a valid scenario");
        }
        if (choices.stop_at_ms)
        {
            throw std::invalid_argument("plan_run: after_scenario=pause cannot be combined with stop_at_ms");
        }
        if (check.time_lock.locked)
        {
            throw std::invalid_argument("plan_run: after_scenario=pause cannot be used when time is locked (outside-real-time)");
        }
    }
    if (choices.record_after_fail)
    {
        if (choices.record_after_fail->count() < 1 || choices.record_after_fail->count() > config::kMaxRecordAfterFailMs)
        {
            throw std::invalid_argument("plan_run: record_after_fail must be between 1 and " +
                                        std::to_string(config::kMaxRecordAfterFailMs) + " ms");
        }
        if (scenario == nullptr || !scenario->scenario)
        {
            throw std::invalid_argument("plan_run: record_after_fail requires a valid scenario");
        }
    }
    if (!choices.external_registry_uri.empty())
    {
        if (const std::string err = external_registry_uri_error(choices.external_registry_uri); !err.empty())
        {
            throw std::invalid_argument("plan_run: " + err);
        }
        if (check.config->registry_uri.has_value())
        {
            throw std::invalid_argument("plan_run: external_registry_uri cannot be combined with SimulationConfig::registry_uri");
        }
    }
    RunPlan plan;
    plan.check = detail::with_bridges(check);
    if (scenario != nullptr)
    {
        plan.scenario_check = *scenario;
    }
    // with_bridges() adds one participant for each network outside, in their order, after those of
    // the file.
    plan.members.resize(check.config->participants.size());
    for (const config::NetworkOutside& outside : check.outside)
    {
        plan.members.emplace_back().outside = outside;
    }
    plan.recorded_variables = choices.recorded_variables;
    plan.record_after_fail = choices.record_after_fail;
    if (const scenario::Scenario* read = plan.scenario(); read != nullptr)
    {
        const std::span<const scenario::RecordedVariable> recordedForSampling =
            choices.timeline.empty() ? std::span<const scenario::RecordedVariable>{}
                                     : std::span<const scenario::RecordedVariable>(plan.recorded_variables);
        const std::vector<bool> asked = scenario::sample_every_step_participants(check, *read, recordedForSampling);
        for (std::size_t i = 0; i < asked.size(); ++i)
        {
            plan.members[i].sample_every_step = asked[i];
        }
    }
    else if (!choices.timeline.empty() && !plan.recorded_variables.empty())
    {
        const std::vector<bool> asked = scenario::sample_every_step_participants(check, plan.recorded_variables);
        for (std::size_t i = 0; i < asked.size(); ++i)
        {
            plan.members[i].sample_every_step = asked[i];
        }
    }
    plan.base = std::move(base);
    plan.stop_at_ms = choices.stop_at_ms;
    plan.start_timeout_ms = choices.start_timeout_ms;
    plan.stop_timeout_ms = choices.stop_timeout_ms;
    plan.collect_control_metrics = choices.collect_control_metrics;
    plan.external_registry_uri = choices.external_registry_uri;
    plan.after_scenario = choices.after_scenario;
    if (check.run_timing)
    {
        plan.timing = *check.run_timing;
    }
    else
    {
        plan.timing = config::detail::plan_time_step(*check.config, check, plan.stop_at_ms);
        plan.timing.speed = config::planned_speed(plan.check.time_lock, choices.speed, {});
        plan.timing.late_tolerance_ms = choices.late_tolerance_ms;
    }
    plan.timing.coarse_step_checked = std::any_of(plan.check.findings.begin(), plan.check.findings.end(),
                                                  [](const config::Finding& f) { return f.code == "coarse-time-step"; });
    plan.strict = choices.strict;
    plan.holds_before_start = choices.start_paused && control_server;
    plan.log_dir = choices.log_dir;
    plan.timeline = process::path_from_utf8(choices.timeline);
    plan.timeline_shown = choices.timeline;
    plan.poll = limits.poll;
    plan.outside_lost_after = limits.outside_lost_after;
    plan.timeout = choices.timeout;
    plan.control_hello_timeout = limits.control_hello_timeout;
    plan.ecu_start_margin = limits.ecu_start_margin;
    return plan;
}

RunOutcome planned_outcome(const RunPlan& plan)
{
    const config::SimulationConfig& config = plan.config();
    const config::CheckReport& check = plan.check;
    RunOutcome outcome;
    outcome.start.timeout_ms = plan.start_timeout_ms;
    outcome.end.stop_timeout_ms = plan.stop_timeout_ms;
    outcome.start.control_hello_timeout_ms = plan.control_hello_timeout.count();
    outcome.end.stop_at_ms = plan.stop_at_ms;
    outcome.time_lock = check.time_lock;
    outcome.real_time_peers = config::detail::real_time_peers(config, check);
    if (const scenario::Scenario* scenario = plan.scenario(); scenario != nullptr)
    {
        outcome.scenario.result = scenario::scenario_not_run(*scenario);
        outcome.scenario.result->file = plan.scenario_check->file;
    }
    outcome.timing = plan.timing;
    if (plan.timeout)
    {
        outcome.end.timeout_ms = plan.timeout->count();
    }
    for (std::size_t i = 0; i < config.participants.size(); ++i)
    {
        const config::ParticipantConfig& p = config.participants[i];
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
    for (const config::NetworkOutside& o : check.outside)
    {
        outcome.outside.push_back(RunOutside{o.network, o.target, o.participant, std::nullopt, std::nullopt, o.tool});
    }
    return outcome;
}

} // namespace vehicle_sil::runner
