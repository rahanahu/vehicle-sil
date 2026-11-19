// SPDX-License-Identifier: MIT
//
// The samples a scenario judges with (its "samples" key): which participants' values a scenario
// reads, what the check says of an executable that cannot be asked for a sample in every step,
// which participants the plan starts with --sample-every-step, and the finding of a run that
// did not begin its scenario because an ECU sends its values less often than its step. No
// process, no thread.

#include "launch_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/runner/detail/run_detail.hpp"
#include "vehicle_sil/runner/run_outcome.hpp"
#include "vehicle_sil/runner/run_plan.hpp"
#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/scenario/scenario_check.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace vehicle_sil::config;
using namespace vehicle_sil::scenario;
using namespace vehicle_sil::runner;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;

const std::string kHead = "vehicle_sil_scenario: 1\nsteps:\n";

std::vector<control::VariableInfo> variables()
{
    control::VariableInfo level;
    level.name = "level";
    level.type = control::ValueType::Integer;
    level.causality = control::Causality::Input;
    control::VariableInfo speed;
    speed.name = "speed";
    speed.type = control::ValueType::Real;
    speed.causality = control::Causality::Output;
    return {level, speed};
}

/// A check that passed of the ECUs Body (step 1 ms) and Door (step 10 ms), each described with
/// `features` (Door: `door_features`).
Checked checked(const std::vector<std::string>& features, const std::vector<std::string>& door_features)
{
    Checked check;
    check.file = "sim.yaml";
    check.config.emplace();
    const auto add = [&check](const std::string& name, SimDuration step, const std::vector<std::string>& takes) {
        ParticipantConfig p;
        p.name = name;
        p.command = {"bin/" + name};
        check.config->participants.push_back(p);
        ParticipantResult r;
        r.status = ParticipantStatus::Described;
        r.ecu.emplace();
        r.ecu->vehicle_sil_version = "0.1.0";
        r.ecu->description.name = name;
        r.ecu->description.step = step;
        r.ecu->description.variables = variables();
        r.ecu->description.control = ControlOffer{"silkit", 2, 1};
        r.ecu->description.features = takes;
        r.ecu->variables_reported = true;
        check.participants.push_back(std::move(r));
    };
    add("Body", 1ms, features);
    add("Door", 10ms, door_features);
    return check;
}

const std::vector<std::string> kTakes{"map-network", "j1939-node", "sample-every-step"};
const std::vector<std::string> kOld{"map-network", "j1939-node"};

Scenario read(const Checked& check, const std::string& text)
{
    ScenarioRead r = read_scenario(text, scenario_catalog(check));
    if (!r.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(r.findings));
    }
    return *r.scenario;
}

std::string flags(const std::vector<bool>& list)
{
    std::string out;
    for (const bool flag : list)
    {
        out += flag ? '1' : '0';
    }
    return out;
}

/// Which of Body and Door the scenario reads a value of, by name.
std::string readers(const Scenario& s)
{
    const std::vector<bool> read = participants_read(s);
    std::string out;
    for (std::size_t i = 0; i < s.participants.size(); ++i)
    {
        if (read[i])
        {
            out += (out.empty() ? "" : ",") + s.participants[i].name;
        }
    }
    return out;
}

} // namespace

TEST_CASE(scenario_samples_is_every_step_unless_the_file_says_as_sent)
{
    const Checked check = checked(kTakes, kTakes);
    const std::string steps = "steps:\n  - set: { Body.level: 1 }\n";
    CHECK(read(check, "vehicle_sil_scenario: 1\n" + steps).samples == ScenarioSamples::EveryStep);
    CHECK(read(check, "vehicle_sil_scenario: 1\nsamples: every_step\n" + steps).samples == ScenarioSamples::EveryStep);
    CHECK(read(check, "vehicle_sil_scenario: 1\nsamples: as_sent\n" + steps).samples == ScenarioSamples::AsSent);
    CHECK(std::string(scenario_samples_name(ScenarioSamples::EveryStep)) == "every_step");
    CHECK(std::string(scenario_samples_name(ScenarioSamples::AsSent)) == "as_sent");
    // Another word, or no word, is an error that names the two.
    const std::string wrong = "samples must be every_step or as_sent, not ";
    const std::string how = ": every_step (the default) makes the ECUs whose values the scenario reads send them after every step of "
                            "theirs; as_sent takes the values as often as each ECU sends them\n";
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nsamples: often\n" + steps, scenario_catalog(check)).findings),
               "E 2 value: " + wrong + "'often'" + how);
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nsamples: [as_sent]\n" + steps, scenario_catalog(check)).findings),
               "E 2 value: " + wrong + "a list" + how);
}

TEST_CASE(scenario_reads_the_participants_of_its_conditions_and_of_the_bases_it_does_not_name)
{
    const Checked check = checked(kTakes, kTakes);
    // Writes alone read nothing.
    check_text(readers(read(check, kHead + "  - set: { Body.level: 1, Door.level: 2 }\n")), "");
    // A condition: expect (at a time, within, held), wait_until, the expect beside a ramp.
    check_text(readers(read(check, kHead + "  - set: { Body.level: 1 }\n  - expect: { Door.speed: 0 }\n")), "Door");
    check_text(readers(read(check, kHead + "  - expect: { Body.speed: 0 }\n    within_ms: 10\n")), "Body");
    check_text(readers(read(check, kHead + "  - expect: { Body.speed: 0 }\n    hold_for_ms: 10\n")), "Body");
    check_text(readers(read(check, kHead + "  - wait_until: { Door.speed: 0 }\n    timeout_ms: 10\n")), "Door");
    check_text(readers(read(check, kHead + "  - ramp: { Body.level: { from: 0, to: 5, over_ms: 50 } }\n    expect: { Door.speed: 0 }\n")),
               "Door");
    // A ramp without from and a wave without around begin at the value the ECU sent.
    check_text(readers(read(check, kHead + "  - ramp: { Door.level: { to: 5, over_ms: 50 } }\n")), "Door");
    check_text(readers(read(check, kHead + "  - ramp: { Door.level: { from: 1, to: 5, over_ms: 50 } }\n")), "");
    check_text(readers(read(check, kHead + "  - wave: { Body.level: { triangle: { amplitude: 5, period_ms: 100 }, for_ms: 300 } }\n")),
               "Body");
    check_text(readers(read(check, kHead + "  - wave: { Body.level: { triangle: { amplitude: 5, period_ms: 100 }, around: 50, for_ms: 300 } }\n")),
               "");
    check_text(readers(read(check, kHead + "  - expect: { Body.speed: 0, Door.speed: 0 }\n")), "Body,Door");
}

TEST_CASE(a_motion_that_starts_from_a_value_the_scenario_wrote_reads_no_sample)
{
    const Checked check = checked(kTakes, kTakes);
    const std::string ramp = "  - ramp: { Door.level: { to: 5, over_ms: 50 } }\n";
    // The variable was written before: a set, a ramp, a wave.
    check_text(readers(read(check, kHead + "  - set: { Door.level: 1 }\n" + ramp)), "");
    check_text(readers(read(check, kHead + "  - ramp: { Door.level: { from: 1, to: 5, over_ms: 50 } }\n" + ramp)), "");
    check_text(readers(read(check, kHead + "  - wave: { Door.level: { triangle: { amplitude: 5, period_ms: 100 }, around: 50, for_ms: 300 } }\n" + ramp)),
               "");
    // Not written yet: the first one reads, the next starts from what the first wrote.
    check_text(readers(read(check, kHead + ramp + ramp)), "Door");
    // Another variable, or the same step: the sample is read.
    check_text(readers(read(check, kHead + "  - set: { Body.level: 1 }\n" + ramp)), "Door");
    check_text(readers(read(check, kHead + "  - ramp: { Door.level: { to: 5, over_ms: 50 }, Body.level: { to: 5, over_ms: 50 } }\n")), "Door,Body");
    // A condition always reads, whatever was written.
    check_text(readers(read(check, kHead + "  - set: { Door.level: 1 }\n  - expect: { Door.level: 1 }\n")), "Door");
    // A step the run skips writes nothing: the ramp after it starts from the sample.
    ScenarioCatalog absent = scenario_catalog(check);
    for (auto& participant : absent)
    {
        if (participant.name == "Body")
        {
            participant.not_in_variant = true;
        }
    }
    ScenarioContext context;
    context.variant = "small";
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: 1, Door.level: 2 }\n" + ramp, absent, context);
    CHECK(r.scenario.has_value());
    CHECK(!r.scenario->steps[0].skipped.empty());
    check_text(readers(*r.scenario), "Door");
}

TEST_CASE(the_run_asks_the_ecus_a_scenario_reads_for_a_sample_in_every_step)
{
    const std::string reads_both = kHead + "  - set: { Body.level: 1 }\n  - expect: { Body.speed: 0, Door.speed: 0 }\n";
    {
        const Checked check = checked(kTakes, kTakes);
        CHECK(flags(sample_every_step_participants(check, read(check, reads_both))) == "11");
        // Only those it reads; a scenario that takes the samples as sent asks no one.
        CHECK(flags(sample_every_step_participants(check, read(check, kHead + "  - set: { Body.level: 1 }\n  - expect: { Door.speed: 0 }\n"))) == "01");
        CHECK(flags(sample_every_step_participants(check, read(check, kHead + "  - set: { Body.level: 1 }\n"))) == "00");
        CHECK(flags(sample_every_step_participants(check, read(check, "vehicle_sil_scenario: 1\nsamples: as_sent\nsteps:\n  - expect: { Body.speed: 0, Door.speed: 0 }\n"))) == "00");
        check_text(lines(scenario_samples_findings(check, read(check, reads_both), "0.2.0")), "");
    }
    // An executable that does not say that it takes the argument is not given it.
    const Checked old = checked(kOld, kTakes);
    const Scenario scenario = read(old, reads_both);
    CHECK(flags(sample_every_step_participants(old, scenario)) == "01");

    // The plan has it per participant, and the arguments end with it.
    Checked with = old;
    with.scenario.emplace();
    with.scenario->file = "s.yaml";
    with.scenario->scenario = scenario;
    const RunPlan plan = plan_run(with, launch::detail::scenario_of(with), "dir", RunChoices{});
    CHECK(!plan.members.at(0).sample_every_step);
    CHECK(plan.members.at(1).sample_every_step);
    CHECK(!plan_run(old, nullptr, "dir", RunChoices{}).members.at(1).sample_every_step); // no scenario: no one
    const SimulationConfig& config = plan.config();
    const std::vector<std::string> without = runner::detail::participant_arguments(config, config.participants[1], "silkit://127.0.0.1:8500", false, false);
    std::vector<std::string> asked = runner::detail::participant_arguments(config, config.participants[1], "silkit://127.0.0.1:8500", false, true);
    CHECK(std::find(without.begin(), without.end(), "--sample-every-step") == without.end());
    CHECK(!asked.empty() && asked.back() == "--sample-every-step");
    asked.pop_back();
    CHECK(asked == without);
    CHECK(ecu_executable::kSampleEveryStepArgument == "--sample-every-step");
    CHECK(ecu_executable::kFeatureSampleEveryStep == "sample-every-step");
}

TEST_CASE(the_check_warns_of_an_ecu_that_a_scenario_reads_and_that_cannot_be_asked_for_every_step)
{
    const std::string reads_body = kHead + "  - set: { Door.level: 1 }\n  - expect: { Body.speed: 0 }\n";
    const Checked old = checked(kOld, kOld);
    // Only the participant whose values are read; Door (written only) gets none.
    check_text(lines(scenario_samples_findings(old, read(old, reads_body), "0.2.0")),
               "W 0 scenario-samples-unsupported: participant 'Body': the scenario reads its values, and its executable does not say "
               "that it takes --sample-every-step (no \"sample-every-step\" in the features of its --describe; it was built with "
               "vehicle-sil 0.1.0), so the run cannot ask it to send its values after every step of its own. If it sends them less "
               "often than that, the run fails before the first step (scenario-not-observed). Build the executable again with "
               "vehicle-sil 0.2.0 or later, or write samples: as_sent at the top of the scenario to judge with the values as the "
               "ECU sends them\n");
    const std::vector<Finding> warned = scenario_samples_findings(old, read(old, reads_body), "0.2.0");
    CHECK(warned.size() == 1 && warned[0].participant == "Body");

    // Without what an executable described, nothing says what it takes: no warning (the run finds
    // out at its start), whatever the status of the participant is.
    Checked unknown = old;
    const Scenario scenario = read(old, reads_body);
    unknown.participants[0].ecu.reset();
    for (const ParticipantStatus status : {ParticipantStatus::Described, ParticipantStatus::NotChecked, ParticipantStatus::Failed})
    {
        unknown.participants[0].status = status;
        check_text(lines(scenario_samples_findings(unknown, scenario, "0.2.0")), "");
        CHECK(flags(sample_every_step_participants(unknown, scenario)) == "00");
    }

    // samples: as_sent: no warning, and a note that says what the scenario then does not see.
    const std::string as_sent = "vehicle_sil_scenario: 1\nsamples: as_sent\nsteps:\n";
    check_text(lines(scenario_samples_findings(old, read(old, as_sent + "  - set: { Door.level: 1 }\n  - expect: { Body.speed: 0 }\n"), "0.2.0")),
               "N 0 scenario-samples-as-sent: samples: as_sent: the scenario judges with the values as often as each ECU sends them "
               "(its sample_period), not after every step of the ECU. A condition does not see what a variable does between two "
               "samples: an expect reads a value up to one sample_period old, and hold_for_ms passes over a change that is shorter "
               "than that. Remove samples (or write samples: every_step) to judge with every step\n");
    // A scenario that reads nothing has nothing to say about samples.
    check_text(lines(scenario_samples_findings(old, read(old, as_sent + "  - set: { Door.level: 1 }\n"), "0.2.0")), "");
}

TEST_CASE(a_scenario_that_was_not_begun_for_its_samples_is_a_failure_with_its_own_code)
{
    const Checked check = checked(kOld, kTakes);
    const Scenario scenario = read(check, kHead + "  - expect: { Body.speed: 0, Door.speed: 0 }\n");
    RunOutcome outcome;
    outcome.scenario.result = scenario_not_run(scenario);
    outcome.scenario.result->file = "s.yaml";
    outcome.scenario.result->status = ScenarioStatus::Failed;
    outcome.scenario.not_observed = {ScenarioNotObserved{"Body", 10ms, 1ms, false}, ScenarioNotObserved{"Door", 50ms, 10ms, true}};
    std::vector<Finding> found;
    for (const Finding& f : assess_run(outcome))
    {
        CHECK(f.code != "scenario-incomplete" && f.code != "scenario-failed");
        if (f.code == "scenario-not-observed")
        {
            found.push_back(f);
        }
    }
    const std::string fix = ", or write samples: as_sent at the top of the scenario to judge with the values as the ECU sends them\n";
    check_text(lines(found),
               "E 0 scenario-not-observed: the scenario was not begun: it reads values of participant 'Body' and judges with every "
               "step of the ECU, and 'Body' sends its values every 10 ms (its sample_period) while its step is 1 ms, so a condition "
               "would not see what a variable does between two samples. Its executable does not take --sample-every-step (no "
               "\"sample-every-step\" in the features of its --describe), so the launcher could not ask for more. Build the executable "
               "again with this vehicle-sil" + fix +
               "E 0 scenario-not-observed: the scenario was not begun: it reads values of participant 'Door' and judges with every "
               "step of the ECU, and 'Door' sends its values every 50 ms (its sample_period) while its step is 10 ms, so a condition "
               "would not see what a variable does between two samples. It was started with --sample-every-step and did not follow "
               "it: an executable that makes its own control agent instead of using the one of ecu_main() must set "
               "AgentOptions::sample_period to 0 itself. Change the executable so" + fix);
    CHECK(found.size() == 2 && found[0].participant == "Body" && found[0].file == "s.yaml" && found[0].severity == Severity::Error);
    CHECK(std::string(scenario_status_name(outcome.scenario.result->status)) == "failed");
}

// A run cannot read or write the variables of an ECU that offers another version (or transport)
// of the control messages: a scenario that uses them is refused by the check, not at its first
// set in the run. One that does not use that ECU still reads.
TEST_CASE(a_scenario_cannot_use_an_ecu_that_offers_another_version_of_the_control_messages)
{
    Checked check = checked(kTakes, kTakes);
    check.participants[0].ecu->description.control = ControlOffer{"silkit", 1, 1};
    check.participants[1].ecu->description.control = ControlOffer{"other", 2, 1};
    const ScenarioRead r =
        read_scenario(kHead + "  - set: { Body.level: 1 }\n  - set: { Door.level: 2 }\n", scenario_catalog(check));
    CHECK(!r.scenario);
    const std::string found = lines(r.findings);
    CHECK(found.find("E 3 scenario-variable: 'Body.level': participant 'Body' offers its variables as silkit messages of "
                     "version 1, and this vehicle_sil_launch speaks version 2, so a run cannot read or write them: the "
                     "ECU is of an older vehicle-sil; build it again") != std::string::npos);
    CHECK(found.find("E 4 scenario-variable: 'Door.level': participant 'Door' offers its variables over the transport "
                     "other, and vehicle_sil_launch run reads and writes variables over silkit only") != std::string::npos);

    Checked door_only = checked(kTakes, kTakes);
    door_only.participants[0].ecu->description.control = ControlOffer{"silkit", 1, 1};
    static_cast<void>(read(door_only, kHead + "  - set: { Door.level: 2 }\n"));
}
