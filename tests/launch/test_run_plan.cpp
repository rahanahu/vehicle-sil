// SPDX-License-Identifier: MIT
//
// The plan of a run (launch::plan_run()) and the outcome a run begins with
// (launch::planned_outcome()): values made from a check and the request and the limits. No process, no
// thread.

#include "test_harness.hpp"

#include "vehicle_sil/launch/run_plan.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::launch;
using Speed = std::optional<double>;

/// A check that passed, of ECUs that described themselves with these steps (microseconds).
CheckReport checked(const std::vector<std::int64_t>& steps_us)
{
    CheckReport check;
    check.file = "sim.yaml";
    check.config.emplace();
    for (std::size_t i = 0; i < steps_us.size(); ++i)
    {
        ParticipantConfig p;
        p.name = "Ecu" + std::to_string(i);
        p.line = 10 + i;
        check.config->participants.push_back(p);
        ParticipantResult r;
        r.status = ParticipantStatus::Described;
        r.ecu.emplace();
        r.ecu->description.name = p.name;
        r.ecu->description.step = std::chrono::microseconds(steps_us[i]);
        check.participants.push_back(std::move(r));
    }
    return check;
}

CheckReport with_scenario(CheckReport check)
{
    check.scenario.emplace();
    check.scenario->file = "drive.yaml";
    check.scenario->scenario.emplace();
    return check;
}

struct Timed
{
    std::int64_t step_ms;
    Speed speed;
    friend bool operator==(const Timed&, const Timed&) = default;
};

Timed timed(const CheckReport& check, const RunRequest& request)
{
    const RunPlan plan = plan_run(check, "dir", request);
    return Timed{plan.timing.time_step_ms, plan.timing.speed};
}

RunRequest at_speed(std::optional<Speed> speed)
{
    RunRequest request;
    request.intent.speed = speed;
    return request;
}

} // namespace

TEST_CASE(the_plan_has_the_speed_the_run_starts_at_and_the_step_that_goes_with_it)
{
    const Speed max{};
    CheckReport check = checked({1000, 10000});
    // Nothing asked, nothing written: as fast as possible, in steps of 10 ms.
    CHECK(timed(check, RunRequest{}) == (Timed{10, max}));
    // --speed: the ECUs' steps decide the step.
    CHECK(timed(check, at_speed(Speed(1.0))) == (Timed{1, 1.0}));
    // The file's speed, and --speed in its place (also "max").
    check.config->speed = 0.5;
    CHECK(timed(check, RunRequest{}) == (Timed{1, 0.5}));
    CHECK(timed(check, at_speed(Speed(2.0))) == (Timed{1, 2.0}));
    CHECK(timed(check, at_speed(max)) == (Timed{10, max}));
    // With the time lock: 1, whatever was asked for.
    check.time_lock.locked = true;
    CHECK(timed(check, at_speed(max)) == (Timed{1, 1.0}));
    CHECK(timed(check, at_speed(Speed(4.0))) == (Timed{1, 1.0}));
}

TEST_CASE(with_a_scenario_the_planned_step_is_the_same_at_every_speed)
{
    const CheckReport check = with_scenario(checked({1000, 10000}));
    const Speed max{};
    // The speed stays what was asked for; the step is that of a run with a speed.
    CHECK(timed(check, at_speed(max)) == (Timed{1, max}));
    CHECK(timed(check, at_speed(Speed(1.0))) == (Timed{1, 1.0}));
    CHECK(timed(check, at_speed(Speed(8.0))) == (Timed{1, 8.0}));
    // Without the scenario, the step of a run as fast as possible is the coarse one.
    CHECK(timed(checked({1000, 10000}), at_speed(max)) == (Timed{10, max}));
    const RunPlan plan = plan_run(check, "dir", RunRequest{});
    CHECK(plan.scenario() == &*plan.check.scenario->scenario);
    CHECK(plan_run(checked({1000}), "dir", RunRequest{}).scenario() == nullptr);
}

TEST_CASE(the_plan_holds_before_the_start_only_for_a_paused_start_with_a_control_server)
{
    const CheckReport check = checked({10000});
    RunRequest request;
    CHECK(!plan_run(check, "dir", request).holds_before_start);
    request.intent.start_paused = true;
    CHECK(!plan_run(check, "dir", request).holds_before_start);
    CHECK(plan_run(check, "dir", request, {}, true).holds_before_start);
    request.intent.start_paused = false;
    CHECK(!plan_run(check, "dir", request, {}, true).holds_before_start);
}

TEST_CASE(the_bridges_are_the_last_participants_of_the_plan_and_do_not_count_for_the_step)
{
    CheckReport check = checked({5000});
    check.config->networks.emplace_back();
    check.config->networks.back().name = "CAN1";
    const RunRequest options = at_speed(Speed(1.0));
    const Timed without = timed(check, options);
    CHECK(without == (Timed{5, 1.0}));
    check.outside = {NetworkOutside{"CAN1", "vehicle_sil_vector_xl_bridge", "/t/xl", "virtual:1", "bridge.CAN1", std::nullopt}};
    const RunPlan plan = plan_run(check, "dir", options);
    // A bridge has no description, and still the step is that of the ECUs.
    CHECK((Timed{plan.timing.time_step_ms, plan.timing.speed}) == without);
    CHECK(plan.config().participants.size() == 2 && plan.check.participants.size() == 2);
    if (plan.config().participants.size() != 2)
    {
        return;
    }
    CHECK(plan.config().participants[0].name == "Ecu0" && plan.config().participants[1].name == "bridge.CAN1");
    // The mark of the bridge, with what it connects: one member for each participant.
    CHECK_EQ(plan.members.size(), std::size_t{2});
    if (plan.members.size() != 2)
    {
        return;
    }
    CHECK(!plan.members[0].outside);
    CHECK(plan.members[1].outside && *plan.members[1].outside == check.outside[0]);
    // The check that was given is left as it was.
    CHECK_EQ(check.config->participants.size(), std::size_t{1});
    // The outcome begins with the same participants in the same order, and the network outside.
    const RunOutcome outcome = planned_outcome(plan);
    CHECK(outcome.participants.size() == 2 && outcome.outside.size() == 1);
    if (outcome.participants.size() != 2 || outcome.outside.size() != 1)
    {
        return;
    }
    CHECK(outcome.participants[0].name == "Ecu0" && outcome.participants[0].line == 10 && outcome.participants[0].required);
    CHECK(outcome.participants[1].name == "bridge.CAN1" && outcome.participants[1].for_network == "CAN1" &&
          !outcome.participants[1].required);
    CHECK(outcome.outside[0].network == "CAN1" && outcome.outside[0].target == "virtual:1" &&
          outcome.outside[0].participant == "bridge.CAN1" && outcome.outside[0].tool == "vehicle_sil_vector_xl_bridge");
}

TEST_CASE(a_bridge_of_the_file_counts_for_the_step_as_any_participant_with_a_step)
{
    // role: bridge in the file, not a bridge that --connect adds (the plan does not mark it): the
    // step divides its step too, 5 ms and 2 ms meet every 1 ms. Left out, the step would be 5 ms.
    CheckReport check = checked({5000, 2000});
    check.config->participants[1].role = ParticipantRole::Bridge;
    const RunPlan plan = plan_run(check, "dir", at_speed(Speed(1.0)));
    CHECK(plan.members.size() == 2 && !plan.members[1].outside);
    CHECK((Timed{plan.timing.time_step_ms, plan.timing.speed}) == (Timed{1, 1.0}));
}

TEST_CASE(a_check_without_a_configuration_has_no_plan)
{
    CheckReport check;
    check.file = "sim.yaml";
    bool thrown = false;
    try
    {
        static_cast<void>(plan_run(check, "dir", RunRequest{}));
    }
    catch (const std::invalid_argument& e)
    {
        thrown = std::string(e.what()) == "plan_run: the check has no configuration (CheckReport::config is empty); call it "
                                          "only with a check that passed (CheckReport::passed())";
    }
    CHECK(thrown);
}

TEST_CASE(the_plan_takes_the_limits_and_the_files_as_given_and_the_outcome_begins_with_them)
{
    using namespace std::chrono_literals;
    CheckReport check = with_scenario(checked({10000}));
    check.config->stop_at_ms = 500;
    check.config->start_timeout_ms = 1234;
    check.config->stop_timeout_ms = 2345;
    check.config->late_tolerance_ms = 77;
    RunRequest request;
    RunLimits limits;
    {
        // What the caller did not set: the defaults of RunLimits.
        const RunPlan plan = plan_run(check, "dir", request);
        CHECK(plan.poll == limits.poll && plan.outside_lost_after == kOutsideLostAfter && !plan.timeout &&
              plan.control_hello_timeout == limits.control_hello_timeout && !plan.strict && plan.log_dir.empty() &&
              plan.timeline.empty() && plan.timeline_shown.empty() && !plan.timing.coarse_step_checked);
        CHECK(!planned_outcome(plan).end.timeout_ms);
    }
    request.intent.strict = true;
    request.host.log_dir = "logs";
    request.host.timeline = "out/timeline.json";
    limits.poll = 7ms;
    limits.outside_lost_after = 8ms;
    request.host.timeout = 9000ms;
    limits.control_hello_timeout = 10ms;
    Finding coarse;
    coarse.severity = Severity::Warning;
    coarse.code = "coarse-time-step";
    check.findings.push_back(coarse);
    const RunPlan plan = plan_run(check, "base", request, limits);
    CHECK(plan.base == "base" && plan.strict && plan.log_dir == "logs" && plan.timeline == "out/timeline.json" &&
          plan.timeline_shown == "out/timeline.json");
    CHECK(plan.poll == 7ms && plan.outside_lost_after == 8ms && plan.timeout == std::optional(9000ms) &&
          plan.control_hello_timeout == 10ms);
    CHECK(plan.timing.coarse_step_checked && plan.timing.late_tolerance_ms == 77);
    const RunOutcome outcome = planned_outcome(plan);
    CHECK(outcome.start.timeout_ms == 1234 && outcome.end.stop_timeout_ms == 2345 && outcome.start.control_hello_timeout_ms == 10 &&
          outcome.end.stop_at_ms == std::optional<std::int64_t>(500) && outcome.end.timeout_ms == std::optional<std::int64_t>(9000));
    CHECK(outcome.timing.time_step_ms == plan.timing.time_step_ms && outcome.timing.speed == plan.timing.speed &&
          outcome.timing.coarse_step_checked && outcome.timing.late_tolerance_ms == 77 && outcome.timing.sim_ms == 0);
    // The scenario as not run, with its file; nothing of what a run finds.
    CHECK(outcome.scenario.result.has_value() && outcome.scenario.result->file == "drive.yaml");
    CHECK(outcome.end.simulation == SimulationEnd::NotStarted && outcome.setup.error.empty() && outcome.setup.registry_uri.empty());
    // Without a scenario: none in the outcome.
    CHECK(!planned_outcome(plan_run(checked({10000}), "dir", request, limits)).scenario.result);
}
