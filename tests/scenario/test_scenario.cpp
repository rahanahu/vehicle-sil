// SPDX-License-Identifier: MIT
//
// The scenario file (scenario.hpp: reading and checking, the wording of its errors) and the
// engine that runs a scenario (scenario_run.hpp) against a model of ECUs that steps in virtual
// time: when the writes take effect, that no sample is skipped, the wording of the failures, and
// that the result is the same whenever the samples and results of a step arrive, and the timeline of
// a run (timeline.hpp) that the engine notes against the same model. The timeline file as the
// launcher writes it, compared with the files of tests/launch/timeline, is in
// tests/launch/test_scenario_timeline.cpp.
#include "launch_test_util.hpp"
#include "scenario_model.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/scenario/timeline.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::scenario;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;
using vehicle_sil::config::test::lines;
using namespace vehicle_sil::scenario::test::scenario_model;

TEST_CASE(scenario_file_is_read_with_its_steps_and_what_each_does)
{
    const Scenario s = read("vehicle_sil_scenario: 1\n"
                            "name: all the steps\n"
                            "steps:\n"
                            "  - at_ms: 0\n"
                            "    set: { Body.level: 5, Body.gain: 3, Door.mode: auto, Door.lamp: true }\n"
                            "  - at_ms: 100\n"
                            "    ramp: { Body.ratio: { to: 0.5, over_ms: 40 }, Body.level: { from: 10, to: 20, over_ms: 100 } }\n"
                            "  - wait_until: { Body.body.speed: { ge: 150.5 }, Door.mode: auto }\n"
                            "    timeout_ms: 2000\n"
                            "  - after_ms: 20\n"
                            "    expect: { Body.level: 20, Door.mode: { ne: 0 }, Body.body.speed: { near: 200, tolerance: 0.5 } }\n"
                            "  - expect: { Door.lamp: true }\n"
                            "    within_ms: 50\n"
                            "  - expect: { Body.level: { gt: 1, lt: 30 } }\n"
                            "    hold_for_ms: 30\n");
    CHECK(s.name == "all the steps");
    CHECK_EQ(s.steps.size(), std::size_t{6});
    CHECK_EQ(s.participants.size(), std::size_t{2});
    CHECK(s.participants[0].name == "Body" && s.participants[1].name == "Door");
    std::string texts;
    for (const auto& step : s.steps)
    {
        texts += std::to_string(step.line) + ": " + step.text + "\n";
    }
    check_text(texts, "4: set Body.level = 5; Body.gain = 3; Door.mode = auto (2); Door.lamp = true\n"
                      "6: ramp Body.ratio to 0.5 over 40 ms; Body.level from 10 to 20 over 100 ms\n"
                      "8: wait_until Body.body.speed ge 150.5; Door.mode eq auto (2) (timeout 2000 ms)\n"
                      "10: expect Body.level eq 20; Door.mode ne off (0); Body.body.speed near 200 (tolerance 0.5)\n"
                      "12: expect Door.lamp eq true (within 50 ms)\n"
                      "14: expect Body.level gt 1, lt 30 (for 30 ms)\n");
    CHECK(s.steps[2].waits() && s.steps[4].waits() && !s.steps[5].waits());
    // JSON is read as well.
    const Scenario json = read(R"({"vehicle_sil_scenario": 1, "steps": [{"set": {"Body.level": 1}}]})");
    CHECK(json.steps.at(0).text == "set Body.level = 1");
}

TEST_CASE(scenario_file_errors_say_the_line_what_is_wrong_and_how_to_fix_it)
{
    const auto errors = [](const std::string& text) { return lines(read_scenario(text, catalog()).findings); };
    // The form.
    check_text(errors(""), "E 0 not-a-scenario: the file is empty: write vehicle_sil_scenario: 1 and steps (docs/scenario.md)\n");
    check_text(errors("vehicle_sil_scenario: 2\nsteps: []\n"),
               "E 1 format-version: vehicle_sil_scenario is 2, but this vehicle_sil_launch reads format 1 only: use the "
               "vehicle-sil that the file was written for\n");
    check_text(errors("steps:\n  - set: { Body.level: 1 }\nstep: 1\n"),
               "E 3 unknown-key: unknown key 'step': did you mean 'steps'? (the keys here: vehicle_sil_scenario, name, samples, only, params, cases, inputs, end_ms, steps)\n"
               "E 0 missing-key: the file has no vehicle_sil_scenario (the format version): add vehicle_sil_scenario: 1\n");
    check_text(errors(kHead + "  - sets: { Body.level: 1 }\n"),
               "E 3 unknown-key: unknown key 'sets': did you mean 'set'? (the keys here: name, at_ms, after_ms, set, ramp, wave, dither, "
               "wait_until, timeout_ms, expect, within_ms, hold_for_ms, undersampled, template, params)\n"
               "E 3 value: the step does nothing: give it one of set, ramp, wave, dither, wait_until, expect, template\n");
    check_text(errors(kHead + "  - set: { Body.level: 1 }\n    expect: { Body.level: 1 }\n"),
               "E 3 value: the step has set, expect: a step does one thing; make a step of each\n");
    // The variables.
    check_text(errors(kHead + "  - set: { Bdy.level: 1 }\n"),
               "E 3 scenario-variable: 'Bdy.level': the configuration has no participant 'Bdy' (the name ends at the first "
               "'.'): did you mean 'Body.level'?\n");
    check_text(errors(kHead + "  - expect: { Body.body.sped: 1 }\n"),
               "E 3 scenario-variable: 'Body.body.sped': participant 'Body' has no variable 'body.sped': did you mean "
               "'Body.body.speed'? (vehicle_sil_launch check --json lists the variables)\n");
    check_text(errors(kHead + "  - expect: { Spare.level: 1 }\n"),
               "E 3 scenario-variable: 'Spare.level': participant 'Spare' is left out of this run (enabled: false, or --disable)\n");
    // What can be written, and when.
    check_text(errors(kHead + "  - set: { Body.pulse: true }\n"),
               "E 3 scenario-not-writable: Body.pulse is an output of its ECU: it can be read (expect, wait_until), not written; "
               "write one of the ECU's inputs instead\n");
    check_text(errors(kHead + "  - at_ms: 10\n    set: { Body.gain: 2 }\n"),
               "E 4 scenario-not-writable: Body.gain is a parameter: it can be written only before the first step, that is with "
               "set in a step at 0 (at_ms: 0, or the first steps without a time), not at 10 ms\n");
    // The values.
    check_text(errors(kHead + "  - set: { Body.level: 101 }\n"),
               "E 3 scenario-value: the value of Body.level (integer) is 101, outside its range (from 0 to 100)\n");
    check_text(errors(kHead + "  - set: { Body.level: 1.5, Door.lamp: 1, Door.mode: of }\n"),
               "E 3 scenario-value: the value of Body.level (integer) must be a whole number, not '1.5'\n"
               "E 3 scenario-value: the value of Door.lamp (bool) must be true or false, not '1'\n"
               "E 3 scenario-value: the value of Door.mode (enum) must be one of its items, by name or by value, not 'of': did "
               "you mean 'off'? the items are off (0), on (1), auto (2)\n");
    check_text(errors(kHead + "  - ramp: { Door.lamp: { to: 1, over_ms: 10 } }\n"),
               "E 3 scenario-value: ramp needs a number, and Door.lamp is a bool: use set\n");
    check_text(errors(kHead + "  - expect: { Door.mode: { ge: 1 }, Body.level: { near: 5 } }\n"),
               "E 3 scenario-value: ge compares numbers, and Door.mode is an enum: use eq or ne\n"
               "E 3 value: Body.level: near needs tolerance (how far from the value it may be): add tolerance: <number>\n");
    // The times.
    check_text(errors(kHead + "  - wait_until: { Body.level: 1 }\n"),
               "E 3 missing-key: wait_until needs timeout_ms: how long to wait at most, in milliseconds of virtual time (when it "
               "passes, the step fails)\n");
    check_text(errors(kHead + "  - at_ms: 100\n    set: { Body.level: 1 }\n  - at_ms: 50\n    set: { Body.level: 2 }\n"),
               "E 5 scenario-time: at_ms is 50, before the end of the step before it (100 ms): the steps run in the order of the "
               "file, and time does not go back; reorder the steps or raise at_ms\n");
    check_text(errors(kHead + "  - expect: { Body.level: 1 }\n    within_ms: 10\n  - at_ms: 500\n    set: { Body.level: 2 }\n"),
               "E 5 scenario-time: at_ms after a step that waits (line 3): when that step ends is not known before the run; give "
               "this step after_ms (from the end of the step before it) instead\n");
    check_text(errors(kHead + "  - at_ms: 5\n    after_ms: 5\n    set: { Body.level: 2 }\n    timeout_ms: 3\n"),
               "E 6 value: timeout_ms goes with wait_until, not with set: remove it\n"
               "E 4 value: the step has at_ms and after_ms: give it one time, at_ms (the virtual time) or after_ms (from the end "
               "of the step before it)\n");
    // A ramp ends where it says: the step after it may not be earlier.
    check_text(errors(kHead + "  - ramp: { Body.level: { to: 5, over_ms: 100 } }\n  - at_ms: 99\n    expect: { Body.level: 5 }\n"),
               "E 4 scenario-time: at_ms is 99, before the end of the step before it (100 ms): the steps run in the order of the "
               "file, and time does not go back; reorder the steps or raise at_ms\n");
    // The times added up stay within the longest time (said once).
    check_text(errors(kHead + "  - after_ms: 600000000000\n    set: { Body.level: 1 }\n"
                              "  - wait_until: { Body.level: 2 }\n    timeout_ms: 300000000000\n"
                              "  - after_ms: 200000000000\n    set: { Body.level: 3 }\n"
                              "  - after_ms: 200000000000\n    set: { Body.level: 4 }\n"),
               "E 7 scenario-time: the scenario can last 1100000000000 ms up to the end of this step, more than the longest time "
               "(1000000000000 ms): shorten the times (after_ms, over_ms, for_ms, hold_for_ms, the timeouts) of the steps up to here\n");
    // A syntax error has its line.
    const auto syntax = read_scenario("vehicle_sil_scenario: 1\nsteps: [\n", catalog()).findings;
    CHECK_EQ(syntax.size(), std::size_t{1});
    CHECK(syntax.at(0).code == "syntax");
}

TEST_CASE(scenario_writes_take_effect_in_the_step_of_their_time_however_the_steps_interleave)
{
    // Values for 0 (a parameter among them), for a time on the ECUs' grid, for a time between two
    // steps (the next step takes it), for a time that is no step of the controller, and right
    // after a wait. Door steps every 4 ms, the controller every 10 ms.
    const std::string file = kHead + "  - set: { Body.level: 5, Body.gain: 3, Door.level: 1 }\n"
                                     "  - at_ms: 130\n"
                                     "    set: { Body.level: 7 }\n"
                                     "  - at_ms: 135\n"
                                     "    set: { Body.level: 8, Door.level: 2 }\n"
                                     "  - at_ms: 150\n"
                                     "    set: { Body.level: 9 }\n"
                                     "  - at_ms: 150\n"
                                     "    set: { Body.level: 10 }\n"
                                     "  - wait_until: { Body.pulse: true }\n"
                                     "    timeout_ms: 100\n"
                                     "  - set: { Body.level: 11 }\n";
    const Scenario s = read(file, catalog(10ms, 4ms));
    const std::string expected = "Body.level=5 for 0 applied at 0\n"
                                 "Body.gain=3 for 0 applied at 0\n"
                                 "Door.level=1 for 0 applied at 0\n"
                                 "Body.level=7 for 130 applied at 130\n"
                                 "Door.level=2 for 135 applied at 136\n"
                                 "Body.level=8 for 135 applied at 140\n"
                                 "Body.level=9 for 150 applied at 150\n"
                                 "Body.level=10 for 150 applied at 150\n"
                                 // The pulse is in the sample of 200 ms: known at 210, the next step is at 220.
                                 "Body.level=11 for 220 applied at 220\n";
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            world.ecus[1].step = 4ms;
            world.policy = policy;
            world.reverse = reverse;
            const ScenarioRun run = world.run(s);
            if (run.status != ScenarioStatus::Passed)
            {
                throw unit::Failure(name + ": " + text_of(run) + joined(world.log));
            }
            // Per ECU the order is fixed; between the ECUs it is the order of the model's steps.
            std::vector<std::string> got = world.log;
            std::vector<std::string> want;
            for (std::size_t from = 0; from < expected.size();)
            {
                const auto to = expected.find('\n', from);
                want.push_back(expected.substr(from, to - from));
                from = to + 1;
            }
            const auto per_ecu = [](std::vector<std::string> all, const std::string& ecu) {
                all.erase(std::remove_if(all.begin(), all.end(), [&ecu](const std::string& l) { return l.rfind(ecu, 0) != 0; }), all.end());
                return joined(all);
            };
            check_text(per_ecu(got, "Body"), per_ecu(want, "Body"), name.c_str());
            check_text(per_ecu(got, "Door"), per_ecu(want, "Door"), name.c_str());
            CHECK_EQ(world.ecus[0].gain_at_0, std::int64_t{3});
        }
    }
}

TEST_CASE(scenario_writes_of_a_tick_all_go_out_before_the_engine_waits_once)
{
    // Three writes for 50 ms, two to Body and one to Door, go out at the controller's step 40 ms;
    // the engine waits once for all of them (the model tells nothing of the agents' answers
    // before). The two to Body take effect in the order of their steps.
    const std::string file = kHead + "  - set: { Body.level: 5, Door.level: 1 }\n"
                                     "  - at_ms: 50\n"
                                     "    set: { Body.level: 7 }\n"
                                     "  - set: { Door.level: 2 }\n"
                                     "  - set: { Body.level: 8 }\n"
                                     "  - at_ms: 60\n"
                                     "    expect: { Body.level: 8, Door.level: 2 }\n";
    World world;
    const ScenarioRun run = world.run(read(file));
    CHECK(run.status == ScenarioStatus::Passed);
    CHECK(world.settles == std::vector<std::size_t>({1, 3}));
    check_text(joined(world.log), "Body.level=5 for 0 applied at 0\n"
                                  "Door.level=1 for 0 applied at 0\n"
                                  "Body.level=7 for 50 applied at 50\n"
                                  "Body.level=8 for 50 applied at 50\n"
                                  "Door.level=2 for 50 applied at 50\n");
}

TEST_CASE(scenario_write_refused_at_once_fails_the_first_refused_in_the_order_sent)
{
    const std::string file = kHead + "  - set: { Body.level: 5, Door.level: 1 }\n"
                                     "  - at_ms: 50\n"
                                     "    set: { Body.level: 7 }\n"
                                     "  - set: { Door.level: 2 }\n"
                                     "  - set: { Body.level: 8 }\n"
                                     "  - at_ms: 60\n"
                                     "    expect: { Body.level: 8, Door.level: 2 }\n";
    const auto refusing = [](std::vector<std::string> peers) {
        return [peers](const std::string& peer, SimTime at) -> std::optional<control::TargetResult> {
            if (at == SimTime{} + 50ms && std::find(peers.begin(), peers.end(), peer) != peers.end())
            {
                return control::TargetResult{peer, control::TargetStatus::Busy, std::nullopt, false, "the peer has too many commands in flight"};
            }
            return std::nullopt;
        };
    };
    // Door refuses its write for 50 ms. The engine learns it once the three writes of the tick
    // have gone out, and fails Door's step as it would have failed had it waited for each write:
    // the step after it is not run, though its write went out to Body.
    {
        World world;
        world.refuse = refusing({"Door"});
        check_text(text_of(world.run(read(file))),
                   "failed over=40\n"
                   "3 passed start=0 at=0 end=0\n"
                   "4 unfinished start=50 at=- end=-\n"
                   "6 failed start=50 at=50 end=50 scenario-write: the write for 50 ms was not taken by participant 'Door': the "
                   "ECU's agent had too many commands waiting (the peer has too many commands in flight)\n"
                   "7 not_run start=- at=- end=-\n"
                   "8 not_run start=- at=- end=-\n");
        CHECK(world.settles == std::vector<std::size_t>({1, 3}));
        CHECK(std::any_of(world.ecus[0].queue.begin(), world.ecus[0].queue.end(), [](const ModelEcu::Command& c) {
            return c.at == SimTime{} + 50ms && c.writes.size() == 1 && c.writes[0].first == "level" &&
                   std::get<std::int64_t>(c.writes[0].second) == 8;
        }));
    }
    // The hub refuses Door's write itself (no room): that is known at once, the engine sends
    // nothing after it, and the step fails the same way.
    {
        World world;
        world.refuse = refusing({"Door"});
        world.refused_by_hub = true;
        check_text(text_of(world.run(read(file))),
                   "failed over=40\n"
                   "3 passed start=0 at=0 end=0\n"
                   "4 unfinished start=50 at=- end=-\n"
                   "6 failed start=50 at=50 end=50 scenario-write: the write for 50 ms was not taken by participant 'Door': the "
                   "ECU's agent had too many commands waiting (the peer has too many commands in flight)\n"
                   "7 not_run start=- at=- end=-\n"
                   "8 not_run start=- at=- end=-\n");
        CHECK(world.settles == std::vector<std::size_t>({1, 2}));
        CHECK(std::none_of(world.ecus[0].queue.begin(), world.ecus[0].queue.end(), [](const ModelEcu::Command& c) {
            return c.at == SimTime{} + 50ms && c.writes.size() == 1 && std::get<std::int64_t>(c.writes[0].second) == 8;
        }));
    }
    // The result of a write is not known any more when the engine looks (cannot happen with the
    // hub's memory): the step of the first such write fails, as when it is judged.
    {
        World world;
        world.forgets = SimTime{} + 50ms;
        check_text(text_of(world.run(read(file))),
                   "failed over=40\n"
                   "3 passed start=0 at=0 end=0\n"
                   "4 failed start=50 at=50 end=50 scenario-write: the result of the write for 50 ms is not known any more: this "
                   "is an error of vehicle_sil_launch\n"
                   "6 not_run start=- at=- end=-\n"
                   "7 not_run start=- at=- end=-\n"
                   "8 not_run start=- at=- end=-\n");
    }
    // Body refuses too: the first refused write in the order sent is Body's of the step before.
    {
        World world;
        world.refuse = refusing({"Body", "Door"});
        check_text(text_of(world.run(read(file))),
                   "failed over=40\n"
                   "3 passed start=0 at=0 end=0\n"
                   "4 failed start=50 at=50 end=50 scenario-write: the write for 50 ms was not taken by participant 'Body': the "
                   "ECU's agent had too many commands waiting (the peer has too many commands in flight)\n"
                   "6 not_run start=- at=- end=-\n"
                   "7 not_run start=- at=- end=-\n"
                   "8 not_run start=- at=- end=-\n");
    }
    // A ramp that finds no base fails at its first write; a write sent before it in the same
    // tick and refused at once comes first.
    {
        World world;
        world.refuse = [](const std::string& peer, SimTime) -> std::optional<control::TargetResult> {
            return peer == "Door" ? std::optional<control::TargetResult>(control::TargetResult{
                                        peer, control::TargetStatus::Invalid, std::nullopt, false, "level: the simulation has ended"})
                                  : std::nullopt;
        };
        check_text(text_of(world.run(read(kHead + "  - set: { Door.level: 1 }\n"
                                                  "  - ramp: { Body.level: { to: 10, over_ms: 100 } }\n"))),
                   "failed over=0\n"
                   "3 failed start=0 at=0 end=0 scenario-write: the write for 0 ms was not taken by participant 'Door': the ECU's "
                   "agent refused it (level: the simulation has ended)\n"
                   "4 not_run start=- at=- end=-\n");
        CHECK(world.settles == std::vector<std::size_t>({1}));
    }
}

TEST_CASE(scenario_result_is_the_same_whenever_the_samples_and_results_of_a_step_arrive)
{
    const std::string file = kHead + "  - set: { Body.level: 20, Door.mode: on }\n"                      // 3
                                     "  - wait_until: { Body.body.speed: { ge: 200 } }\n"                // 4
                                     "    timeout_ms: 500\n"
                                     "  - expect: { Body.body.speed: 200, Door.mode: on }\n"             // 6
                                     "    hold_for_ms: 40\n"
                                     "  - ramp: { Body.level: { to: 0, over_ms: 50 }, Door.ratio: { from: 1, to: 0, over_ms: 30 } }\n" // 8
                                     "  - expect: { Body.level: 0 }\n"                                   // 9
                                     "  - after_ms: 10\n"                                                // 10
                                     "    expect: { Body.body.speed: { le: 100 } }\n"
                                     "    within_ms: 300\n"
                                     "  - expect: { Door.ratio: { near: 0, tolerance: 0.001 }, Body.level_changed_at_ms: 140 }\n" // 13
                                     "  - after_ms: 5\n"                                                 // 14
                                     "    set: { Door.lamp: true }\n"
                                     "  - after_ms: 15\n"                                                // 16
                                     "    expect: { Door.lamp: true }\n";
    // speed: +50 per 10 ms step from the step at 0: 200 in the sample of 30 ms. Known at 40, so the
    // wait ends at 50; the hold is over [50, 90]; the ramp from 90 to 140 (level 0 written for 140).
    // The speed follows the level down at once (it is 0 in the sample of 140 ms), so the wait from
    // 150 is true at 150: known at 160, it ends at 170. The set for 175 is applied by the step at 180.
    const std::string expected = "passed over=200\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 passed start=0 at=30 end=50\n"
                                 "6 passed start=50 at=90 end=90\n"
                                 "8 passed start=90 at=140 end=140\n"
                                 "9 passed start=140 at=140 end=140\n"
                                 "10 passed start=150 at=150 end=170\n"
                                 "13 passed start=170 at=170 end=170\n"
                                 "14 passed start=175 at=180 end=175\n"
                                 "16 passed start=190 at=190 end=190\n";
    const Scenario s = read(file);
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            world.policy = policy;
            world.reverse = reverse;
            check_text(text_of(world.run(s)), expected, name.c_str());
            CHECK(std::none_of(world.log.begin(), world.log.end(), [](const std::string& l) { return l.find("LATE") != std::string::npos; }));
        }
    }
}

TEST_CASE(scenario_conditions_look_at_every_sample)
{
    // `pulse` is true in one sample only (200 ms). A wait sees it; a hold over it is broken by it,
    // at that sample; an expect at 205 ms sees the latest sample at or before it (the one of 200 ms).
    for (const auto& [name, policy] : policies())
    {
        World wait;
        wait.policy = policy;
        check_text(text_of(wait.run(read(kHead + "  - wait_until: { Body.pulse: true }\n    timeout_ms: 1000\n"
                                                 "  - expect: { Body.pulse: false }\n"))),
                   "passed over=230\n"
                   "3 passed start=0 at=200 end=220\n"
                   "5 passed start=220 at=220 end=220\n",
                   name.c_str());
        World hold;
        hold.policy = policy;
        check_text(text_of(hold.run(read(kHead + "  - at_ms: 150\n    expect: { Body.pulse: false }\n    hold_for_ms: 100\n"))),
                   "failed over=210\n"
                   "3 failed start=150 at=200 end=250 scenario-failed: not true for 100 ms (from 150 ms): at 200 ms Body.pulse eq "
                   "false, but it was true (its sample of 200 ms)\n",
                   name.c_str());
        World between;
        between.policy = policy;
        check_text(text_of(between.run(read(kHead + "  - at_ms: 205\n    expect: { Body.pulse: true }\n"
                                                    "  - at_ms: 210\n    expect: { Body.pulse: false }\n"))),
                   "passed over=220\n"
                   "3 passed start=205 at=205 end=205\n"
                   "5 passed start=210 at=210 end=210\n",
                   name.c_str());
    }
}

TEST_CASE(scenario_samples_of_one_tick_are_looked_at_in_the_order_of_their_times)
{
    // The controller steps every 20 ms, the ECUs sample every 10 ms: two samples of each ECU per
    // tick, and conditions that begin and end between them. Both ECUs have the pulse in their
    // sample of 200 ms only: a condition on both is true there, whichever sample came first.
    const Scenario s = read(kHead + "  - at_ms: 150\n"
                                    "    expect: { Body.pulse: false }\n"
                                    "    hold_for_ms: 40\n"
                                    "  - wait_until: { Body.pulse: true, Door.pulse: true }\n"
                                    "    timeout_ms: 15\n"
                                    "  - expect: { Door.pulse: false, Body.pulse: false }\n"
                                    "    within_ms: 100\n"
                                    "  - after_ms: 21\n"
                                    "    set: { Body.level: 2 }\n"
                                    "  - after_ms: 18\n"
                                    "    expect: { Body.level_changed_at_ms: 310, Body.level: 2 }\n",
                            catalog(10ms, 4ms));
    // The hold ends at 190; the wait from 190 is true at 200 (known at 220, it ends at 240); the
    // pulse is gone in the sample of 240 (known at 260, ends at 280).
    const std::string expected = "passed over=320\n"
                                 "3 passed start=150 at=190 end=190\n"
                                 "6 passed start=190 at=200 end=240\n"
                                 "8 passed start=240 at=240 end=280\n"
                                 "10 passed start=301 at=310 end=301\n"
                                 "12 passed start=319 at=319 end=319\n";
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            world.tick = 20ms;
            world.ecus[1].step = 4ms;
            world.policy = policy;
            world.reverse = reverse;
            check_text(text_of(world.run(s)), expected, name.c_str());
        }
    }
    // The same wait misses the pulse by one sample: it runs out at 195, between two samples of a tick.
    World late;
    late.tick = 20ms;
    check_text(text_of(late.run(read(kHead + "  - at_ms: 180\n    wait_until: { Body.pulse: true, Door.pulse: true }\n    timeout_ms: 15\n"))),
               "failed over=200\n"
               "3 failed start=180 at=195 end=- scenario-timeout: timed out at 195 ms (15 ms after 180 ms): Body.pulse eq true, but "
               "it was false (its sample of 190 ms); Door.pulse eq true, but it was false (its sample of 190 ms)\n");
}

TEST_CASE(scenario_failures_say_the_time_and_the_values)
{
    const auto run = [](const std::string& steps, const std::function<void(World&)>& change = {}) {
        World world;
        if (change)
        {
            change(world);
        }
        return text_of(world.run(read(kHead + steps)));
    };
    // A wait that runs out: when, and what the values were then.
    check_text(run("  - set: { Body.level: 20 }\n"
                   "  - wait_until: { Body.body.speed: { ge: 250 }, Body.level: 20 }\n    timeout_ms: 100\n"
                   "  - set: { Body.level: 0 }\n"),
               "failed over=110\n"
               "3 passed start=0 at=0 end=0\n"
               "4 failed start=0 at=100 end=- scenario-timeout: timed out at 100 ms (100 ms after 0 ms): Body.body.speed ge 250, "
               "but it was 200 (its sample of 100 ms)\n"
               "6 not_run start=- at=- end=-\n");
    // An expectation at one time, and one within a time.
    check_text(run("  - at_ms: 20\n    expect: { Body.level: 1, Body.body.speed: { lt: 10 } }\n"),
               "failed over=30\n"
               "3 failed start=20 at=20 end=20 scenario-failed: not true at 20 ms: Body.level eq 1, but it was 0 (its sample of 20 ms)\n");
    check_text(run("  - expect: { Door.mode: on }\n    within_ms: 25\n"),
               "failed over=30\n"
               "3 failed start=0 at=25 end=- scenario-failed: not true at any time within 25 ms (from 0 ms to 25 ms): at the end "
               "Door.mode eq on (1), but it was off (0) (its sample of 20 ms)\n");
    // Before the first sample there is no value.
    check_text(run("  - expect: { Body.level: 0 }\n    within_ms: 5\n", [](World& w) { w.ecus[0].step = 20ms; w.tick = 5ms; }),
               "passed over=5\n"
               "3 passed start=0 at=0 end=10\n");
    // A write the ECU rejects in its step: said at the controller's step after it, before the
    // expectations of that time are looked at.
    check_text(run("  - at_ms: 50\n    set: { Body.level: 13 }\n  - expect: { Body.level: 13 }\n"),
               "failed over=60\n"
               "3 failed start=50 at=50 end=50 scenario-write: the write for 50 ms was not applied by participant 'Body': the ECU "
               "rejected it (level: a setter threw: 13 is not a level)\n"
               "5 not_run start=- at=- end=-\n");
    // A participant without a link: the first step that uses it says so, before anything runs.
    check_text(run("  - at_ms: 50\n    set: { Body.level: 1 }\n  - expect: { Door.lamp: false }\n",
                   [](World& w) { w.ecus.pop_back(); }),
               "failed over=0\n"
               "3 not_run start=- at=- end=-\n"
               "5 failed start=50 at=50 end=- scenario-failed: Door.lamp cannot be read: participant 'Door' has no control link in "
               "this run: its agent did not connect before the simulation started (the run's control-link findings and its "
               "lines say why)\n");
    // The simulation stops by itself first: what could be decided is, the next step is out of time.
    check_text(run("  - at_ms: 80\n    set: { Body.level: 1 }\n  - at_ms: 90\n    expect: { Body.level: 1 }\n"
                   "  - at_ms: 100\n    expect: { Body.level: 1 }\n  - at_ms: 150\n    set: { Body.level: 2 }\n",
                   [](World& w) { w.stop_at = at(100ms); }),
               "incomplete over=100\n"
               "3 passed start=80 at=80 end=80\n"
               "5 passed start=90 at=90 end=90\n"
               "7 not_run start=- at=- end=-\n"
               "9 not_run start=- at=- end=-\n");
    // The stop arrives before the controller can start the write at 95 ms.
    check_text(run("  - at_ms: 95\n    set: { Body.level: 1 }\n", [](World& w) { w.stop_at = at(100ms); }),
               "incomplete over=100\n"
               "3 not_run start=- at=- end=-\n");
    // A participant failed and ended the run: a step that failed (of that) is not the result.
    {
        World world;
        world.ecus.pop_back();
        const Scenario s = read(kHead + "  - set: { Body.level: 1 }\n  - at_ms: 50\n    set: { Body.level: 13 }\n");
        ScenarioEngine engine(s, 10ms, world);
        engine.begin({EnginePeer{true, model_variables()}});
        // Body goes after the first tick: the result of the write for 0 never comes.
        CHECK(!engine.tick(at(0ms)));
        world.ecus.clear();
        for (SimTime p = at(10ms); !engine.tick(p); p += 10ms)
        {
        }
        CHECK(engine.run().status == ScenarioStatus::Failed);
        engine.superseded();
        CHECK(engine.over());
        check_text(text_of(engine.run()), "incomplete over=-\n"
                                          "3 unfinished start=0 at=- end=-\n"
                                          "4 not_run start=- at=- end=-\n");
    }
    // The simulation ends for another reason: incomplete, and how far it got.
    {
        World world;
        const Scenario s = read(kHead + "  - set: { Body.level: 1 }\n  - wait_until: { Body.level: 2 }\n    timeout_ms: 9000\n");
        check_text(text_of(world.run(s, 300ms)), "incomplete over=-\n"
                                                 "3 passed start=0 at=0 end=0\n"
                                                 "4 unfinished start=0 at=- end=-\n");
    }
}

TEST_CASE(scenario_stop_keeps_writes_that_apply_before_the_boundary)
{
    // One set spans two ECUs. Body can apply both values at 15 ms; Door's next step is the
    // exclusive end at 20 ms. Its write must not suppress Body's command or complete the set.
    const Scenario scenario = read(kHead + "  - at_ms: 15\n"
                                           "    set: { Body.level: 5, Body.lamp: true, Door.level: 9 }\n",
                                   catalog(5ms, 10ms));
    World world;
    world.tick = 5ms;
    world.ecus[0].step = 5ms;
    world.stop_at = at(20ms);
    const ScenarioRun run = world.run(scenario);
    CHECK(run.status == ScenarioStatus::Incomplete);
    CHECK(run.steps[0].status == StepStatus::Unfinished);
    CHECK_EQ(world.ecus[0].level, std::int64_t{5});
    CHECK(world.ecus[0].lamp);
    CHECK_EQ(world.ecus[1].level, std::int64_t{0});
    check_text(joined(world.log), "Body.level=5 for 15 applied at 15\n"
                                  "Body.lamp=true for 15 applied at 15\n");

    World refused;
    refused.tick = 5ms;
    refused.ecus[0].step = 5ms;
    refused.stop_at = at(20ms);
    refused.refuse = [](const std::string& peer, SimTime) -> std::optional<control::TargetResult> {
        if (peer != "Body") return std::nullopt;
        control::TargetResult result;
        result.peer = peer;
        result.status = control::TargetStatus::Invalid;
        result.reason = "refused";
        return result;
    };
    CHECK(refused.run(scenario).status == ScenarioStatus::Failed);
}

TEST_CASE(scenario_stop_does_not_issue_relative_steps_after_a_blocked_step)
{
    const Scenario scenario = read(kHead + "  - at_ms: 100\n"
                                           "    set: { Body.level: 5 }\n"
                                           "  - set: { Body.level: 9 }\n");
    World world;
    world.stop_at = at(100ms);
    const ScenarioRun run = world.run(scenario);
    CHECK(run.status == ScenarioStatus::Incomplete);
    CHECK(run.steps[0].status == StepStatus::NotRun);
    CHECK(run.steps[1].status == StepStatus::NotRun);
    CHECK(world.log.empty());
}

TEST_CASE(scenario_stop_places_only_motions_that_can_write_before_it)
{
    const Scenario scenario = read(kHead + "  - at_ms: 15\n"
                                           "    ramp: { Body.level: { from: 5, to: 10, over_ms: 10 }, "
                                           "Door.level: { from: 7, to: 12, over_ms: 10 } }\n",
                                   catalog(5ms, 10ms));
    TimelineRecord timeline(scenario);
    World world;
    world.record = &timeline;
    world.tick = 5ms;
    world.ecus[0].step = 5ms;
    world.stop_at = at(20ms);
    CHECK(world.run(scenario).status == ScenarioStatus::Incomplete);
    CHECK_EQ(world.ecus[0].level, std::int64_t{5});
    CHECK_EQ(world.ecus[1].level, std::int64_t{0});
    CHECK(timeline.recorded_to() == at(15ms));
    CHECK_EQ(timeline.lines().size(), std::size_t{1});
    CHECK(scenario.variables[timeline.lines()[0].variable].text == "Body.level");
    CHECK(timeline.lines()[0].from == at(15ms));
}

TEST_CASE(scenario_stop_places_only_dithers_that_can_write_before_it)
{
    const Scenario scenario = read(kHead + "  - set: { Body.level: 50, Door.level: 50 }\n"
                                           "  - at_ms: 15\n"
                                           "    dither: { Body.level: { square: { amplitude: 1, period_ms: 20 } }, "
                                           "Door.level: { square: { amplitude: 1, period_ms: 20 } } }\n",
                                   catalog(5ms, 10ms));
    TimelineRecord timeline(scenario);
    World world;
    world.record = &timeline;
    world.tick = 5ms;
    world.ecus[0].step = 5ms;
    world.stop_at = at(20ms);
    const ScenarioRun run = world.run(scenario);
    CHECK(run.status == ScenarioStatus::Incomplete);
    CHECK(run.steps[1].status == StepStatus::Unfinished);
    CHECK_EQ(world.ecus[0].level, std::int64_t{49});
    CHECK_EQ(world.ecus[1].level, std::int64_t{50});
    CHECK(timeline.recorded_to() == at(15ms));
    const auto body = std::count_if(timeline.lines().begin(), timeline.lines().end(), [&](const auto& line) {
        return line.dither && scenario.variables[line.variable].text == "Body.level";
    });
    const auto door = std::count_if(timeline.lines().begin(), timeline.lines().end(), [&](const auto& line) {
        return line.dither && scenario.variables[line.variable].text == "Door.level";
    });
    CHECK_EQ(body, 1);
    CHECK_EQ(door, 0);
}

TEST_CASE(scenario_stop_checks_only_writes_that_can_apply_before_it)
{
    const Scenario scenario = read(kHead + "  - at_ms: 15\n"
                                           "    set: { Body.level: 5, Door.level: 9 }\n",
                                   catalog(5ms, 10ms));
    World limited;
    limited.tick = 5ms;
    limited.ecus[0].step = 5ms;
    limited.ecus.pop_back(); // Door's control link is absent
    limited.stop_at = at(20ms);
    const ScenarioRun cut = limited.run(scenario);
    CHECK(cut.status == ScenarioStatus::Incomplete);
    CHECK_EQ(limited.ecus[0].level, std::int64_t{5});

    World unlimited;
    unlimited.tick = 5ms;
    unlimited.ecus[0].step = 5ms;
    unlimited.ecus.pop_back();
    CHECK(unlimited.run(scenario).status == ScenarioStatus::Failed);
    CHECK(unlimited.log.empty());

    // Read conditions are checked even if the participant has no step before the boundary.
    const Scenario watch = read(kHead + "  - at_ms: 15\n"
                                        "    expect: { Door.level: 0 }\n", catalog(5ms, 10ms));
    World no_link;
    no_link.tick = 5ms;
    no_link.ecus.pop_back();
    no_link.stop_at = at(20ms);
    CHECK(no_link.run(watch).status == ScenarioStatus::Failed);
}

TEST_CASE(scenario_ramp_writes_a_line_in_the_steps_of_its_ecu)
{
    // From what the scenario wrote last (1, in the step before it for the same time: no sample
    // has it yet) to 0 over 16 ms, in Door's 4 ms steps; and an integer from a given value,
    // rounded, whose end (125 ms) is between two steps of its ECU: the step at 130 ms takes it.
    const Scenario s = read(kHead + "  - set: { Door.ratio: 0.5 }\n"
                                    "  - at_ms: 100\n"
                                    "    set: { Door.ratio: 1 }\n"
                                    "  - at_ms: 100\n"
                                    "    ramp: { Door.ratio: { to: 0, over_ms: 16 }, Body.level: { from: 1, to: 4, over_ms: 25 } }\n"
                                    "  - after_ms: 5\n"
                                    "    expect: { Door.ratio: 0, Body.level: 4 }\n",
                            catalog(10ms, 4ms));
    World world;
    world.ecus[1].step = 4ms;
    const ScenarioRun run = world.run(s);
    check_text(text_of(run), "passed over=140\n"
                             "3 passed start=0 at=0 end=0\n"
                             "4 passed start=100 at=100 end=100\n"
                             "6 passed start=100 at=125 end=125\n"
                             "8 passed start=130 at=130 end=130\n");
    check_text(joined(world.log), "Door.ratio=0.5 for 0 applied at 0\n"
                                  "Body.level=1 for 100 applied at 100\n"
                                  "Door.ratio=1 for 100 applied at 100\n"
                                  "Door.ratio=1 for 100 applied at 100\n"
                                  "Door.ratio=0.75 for 104 applied at 104\n"
                                  "Door.ratio=0.5 for 108 applied at 108\n"
                                  "Body.level=2 for 110 applied at 110\n"
                                  "Door.ratio=0.25 for 112 applied at 112\n"
                                  "Door.ratio=0 for 116 applied at 116\n"
                                  "Body.level=3 for 120 applied at 120\n"
                                  "Body.level=4 for 125 applied at 130\n");
    // Without a value to start from, the ramp says so.
    World empty;
    check_text(text_of(empty.run(read(kHead + "  - ramp: { Body.level: { to: 4, over_ms: 20 } }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=20 scenario-write: the ramp of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 0 ms and the scenario has not written it; give the ramp from: <value>\n");
}

namespace {

ParamValue param(std::string name, std::string text, ParamSource source, bool quoted = false)
{
    ParamValue out;
    out.name = std::move(name);
    out.text = std::move(text);
    out.source = source;
    out.quoted = quoted;
    return out;
}

/// "name=text from", one per line.
std::string params_text(const std::vector<ParamValue>& params)
{
    std::string out;
    for (const auto& p : params)
    {
        out += p.name + "=" + (p.quoted ? "\"" + p.text + "\"" : p.text) + " " + param_source_name(p.source) + "\n";
    }
    return out;
}

/// The catalog with Body.boost and Door.boost said to be absent.
ScenarioCatalog catalog_without_boost()
{
    ScenarioCatalog c = catalog();
    c[0].absent = {"boost"};
    c[1].absent = {"boost"};
    return c;
}

} // namespace

TEST_CASE(scenario_parameters_stand_for_values_and_the_later_source_wins)
{
    const std::string text = "vehicle_sil_scenario: 1\n"
                             "params: { level: 5, mode: auto, wait: 100, unused: 1 }\n"
                             "steps:\n"
                             "  - set: { Body.level: \"${level}\", Door.mode: \"${mode}\", Door.lamp: \"${lamp}\" }\n"
                             "  - at_ms: ${wait}\n"
                             "    ramp:\n"
                             "      Body.ratio: { to: \"${ratio}\", over_ms: \"${wait}\" }\n"
                             "  - expect:\n"
                             "      Body.body.speed: { near: \"${level}\", tolerance: \"${ratio}\" }\n"
                             "    within_ms: ${wait}\n";
    ScenarioContext context;
    context.params = {param("level", "7", ParamSource::Configuration), param("lamp", "true", ParamSource::Configuration),
                      param("ratio", "0.25", ParamSource::Configuration), param("elsewhere", "1", ParamSource::Configuration),
                      param("level", "9", ParamSource::Variant), param("wait", "40", ParamSource::CommandLine)};
    const ScenarioRead r = read_scenario(text, catalog(), context);
    check_text(lines(r.findings), "");
    CHECK(r.scenario.has_value());
    std::string texts;
    for (const auto& step : r.scenario->steps)
    {
        texts += std::to_string(step.line) + ": " + step.text + "\n";
    }
    check_text(texts, "4: set Body.level = 9; Door.mode = auto (2); Door.lamp = true\n"
                      "5: ramp Body.ratio to 0.25 over 40 ms\n"
                      "8: expect Body.body.speed near 9 (tolerance 0.25) (within 40 ms)\n");
    CHECK_EQ(*r.scenario->steps[1].at_ms, std::int64_t{40});
    // What the scenario declares, then what it uses besides; not what only the configuration has.
    check_text(params_text(r.params), "level=9 variant\n"
                                      "mode=auto scenario\n"
                                      "wait=40 command_line\n"
                                      "unused=1 scenario\n"
                                      "lamp=true configuration\n"
                                      "ratio=0.25 configuration\n");
    // Without the context the defaults count, and what has none is an error once.
    const ScenarioRead alone = read_scenario(text, catalog());
    CHECK(!alone.scenario.has_value());
    check_text(lines(alone.findings),
               "E 4 scenario-param: ${lamp} has no value: give it a default in the scenario (params: { lamp: <value> }), or a "
               "value in the configuration's params, in a variant's params or with --param lamp=<value>\n"
               "E 7 scenario-param: ${ratio} has no value: give it a default in the scenario (params: { ratio: <value> }), or a "
               "value in the configuration's params, in a variant's params or with --param ratio=<value>\n");
    CHECK_EQ(alone.steps, std::size_t{3});
}

TEST_CASE(scenario_parameter_errors_say_what_to_write)
{
    const auto errors = [](const std::string& text, const ScenarioContext& context = {}) {
        return lines(read_scenario(text, catalog(), context).findings);
    };
    // Only a whole value; not a key, not a part, no expression.
    check_text(errors("vehicle_sil_scenario: 1\n"
                      "params: { n: 5, 2nd: 1, list: [1], other: \"${n}\" }\n"
                      "steps:\n"
                      "  - set: { \"Body.${n}\": 1 }\n"
                      "  - set: { Body.level: \"${n} \" }\n"
                      "  - set: { Body.level: \"${n+1}\" }\n"
                      "  - at_ms: \"${m}\"\n"
                      "    set: { Body.level: \"${m}\" }\n"),
               "E 2 value: params: '2nd' cannot be a parameter's name: use letters, digits and _ (not a digit first), as a "
               "scenario writes it in ${...}\n"
               "E 2 value: params: list must be one value (a number, true or false, or a text), not a list\n"
               "E 2 value: params: other is the text \"${n}\", but a parameter's value cannot use another parameter (${...}): "
               "write the value out\n"
               "E 4 scenario-param: 'Body.${n}': a parameter (${...}) stands for a value, not for a key or for a variable's "
               "name: write the name out\n"
               "E 5 scenario-param: '${n} ': a parameter is written ${name} (letters, digits and _) and is the whole value: it "
               "cannot be a part of a text, and there are no expressions; work the value out and give it as a parameter of "
               "its own\n"
               "E 6 scenario-param: '${n+1}': a parameter is written ${name} (letters, digits and _) and is the whole value: "
               "it cannot be a part of a text, and there are no expressions; work the value out and give it as a parameter "
               "of its own\n"
               "E 7 scenario-param: ${m} has no value (did you mean ${n}?): give it a default in the scenario (params: { m: "
               "<value> }), or a value in the configuration's params, in a variant's params or with --param m=<value>\n");
    // A value that does not fit says where it comes from.
    ScenarioContext context;
    context.params = {param("n", "high", ParamSource::CommandLine), param("t", "10", ParamSource::Variant, true)};
    check_text(errors("vehicle_sil_scenario: 1\n"
                      "params: { n: 5 }\n"
                      "steps:\n"
                      "  - set: { Body.level: \"${n}\" }\n"
                      "  - after_ms: \"${t}\"\n"
                      "    set: { Body.level: 1 }\n",
                      context),
               "E 4 scenario-value: the value of Body.level (integer) must be a whole number, not 'high' (the parameter n, from "
               "--param)\n"
               "E 5 value: after_ms must be a whole number (milliseconds of virtual time from the end of the step before it), "
               "not the text \"10\" (the parameter t, from the variant's params): write the number without quotes\n");
    // A --param that the scenario does not have changes nothing: an error, not silence. It is the
    // command line's (a usage error), not a finding of the file.
    context.params = {param("leve", "1", ParamSource::CommandLine), param("elsewhere", "1", ParamSource::Configuration)};
    const ScenarioRead near = read_scenario("vehicle_sil_scenario: 1\n"
                                            "params: { level: 5 }\n"
                                            "steps:\n"
                                            "  - set: { Body.level: \"${level}\" }\n",
                                            catalog(), context);
    check_text(lines(near.findings), "");
    check_text(lines(near.usage_errors),
               "E 0 usage: --param leve=1: the scenario has no parameter leve (did you mean level?): its parameters "
               "are level; remove the option or correct the name\n");
    const ScenarioRead none = read_scenario(kHead + "  - set: { Body.level: 2 }\n", catalog(), context);
    check_text(lines(none.findings), "");
    check_text(lines(none.usage_errors),
               "E 0 usage: --param leve=1: the scenario has no parameter leve: it uses none (a step uses one as "
               "${name}); remove the option or correct the name\n");
}

TEST_CASE(scenario_values_of_integers_and_enums_may_be_hexadecimal)
{
    // Where a value of an integer or an enum stands; the steps' texts stay decimal.
    ScenarioContext context;
    context.params = {param("code", "0X1f", ParamSource::CommandLine)};
    const ScenarioRead r = read_scenario(
        "vehicle_sil_scenario: 1\n"
        "params: { code: 0, top: 0x64 }\n"
        "steps:\n"
        "  - set: { Body.level: 0x0, Body.gain: 0x7FFFFFFFFFFFFFFF, Door.mode: 0x2, Door.level: \"${top}\" }\n"
        "  - ramp: { Body.level: { from: 0x0A, to: 0X64, over_ms: 100 } }\n"
        "  - expect: { Body.level: 0x64, Door.mode: { ne: 0x0 }, Body.gain: { ge: 0x10, le: 0x7fffffffffffffff, gt: 0xf, lt: 0x7FFFFFFFFFFFFFFF } }\n"
        "  - wait_until: { Body.level: { eq: \"${code}\" } }\n"
        "    timeout_ms: 10\n"
        "  - wave: { Body.level: { square: { low: 0x10, high: 0x20, period_ms: 100 }, for_ms: 200 },\n"
        "            Door.level: { sine: { amplitude: 5, period_ms: 100 }, around: 0x32, for_ms: 100 } }\n"
        "  - wave: { Body.level: { points: [[0, 0x1], [50, 0xa]] }, Door.mode: { points: [[0, 0x1], [10, 0x2]] } }\n",
        catalog(), context);
    check_text(lines(r.findings), "");
    CHECK(r.scenario.has_value());
    if (!r.scenario)
    {
        return;
    }
    std::string texts;
    for (const auto& step : r.scenario->steps)
    {
        texts += step.text + "\n";
    }
    check_text(texts, "set Body.level = 0; Body.gain = 9223372036854775807; Door.mode = auto (2); Door.level = 100\n"
                      "ramp Body.level from 10 to 100 over 100 ms\n"
                      "expect Body.level eq 100; Door.mode ne off (0); Body.gain ge 16, le 9223372036854775807, gt 15, lt "
                      "9223372036854775807\n"
                      "wait_until Body.level eq 31 (timeout 10 ms)\n"
                      "wave Body.level square low 16 high 32 period 100 ms for 200 ms; Door.level sine amplitude 5 period 100 ms "
                      "around 50 for 100 ms\n"
                      "wave Body.level points (2, linear) over 50 ms; Door.mode points (2, hold) over 10 ms\n");

    // What is not read, and what to write instead.
    const auto errors = [](const std::string& text) { return lines(read_scenario(text, catalog()).findings); };
    check_text(errors(kHead + "  - set: { Body.level: 0x65, Door.level: -0x10, Body.gain: \"0x10\", Body.ratio: 0x1 }\n"),
               "E 3 scenario-value: the value of Body.level (integer) is 0x65 (101), outside its range (from 0 to 100)\n"
               "E 3 scenario-value: the value of Door.level (integer) must be a whole number, not '-0x10': write a negative number "
               "in decimal (-16)\n"
               "E 3 scenario-value: the value of Body.gain (integer) must be a whole number, not the text \"0x10\": write the "
               "number without quotes\n"
               "E 3 scenario-value: the value of Body.ratio (real) must be a number, not '0x1': hexadecimal (0x...) is for "
               "integer and enum variables only; write 1\n");
    check_text(errors(kHead + "  - set: { Body.gain: 0x8000000000000000, Door.gain: 0x, Body.level: 0x1G, Door.level: 0o17 }\n"),
               "E 3 scenario-value: the value of Body.gain (integer) must be a whole number, not '0x8000000000000000': it is "
               "more than an integer holds (at most 0x7FFFFFFFFFFFFFFF)\n"
               "E 3 scenario-value: the value of Door.gain (integer) must be a whole number, not '0x'\n"
               "E 3 scenario-value: the value of Body.level (integer) must be a whole number, not '0x1G'\n"
               "E 3 scenario-value: the value of Door.level (integer) must be a whole number, not '0o17'\n");
    check_text(errors(kHead + "  - set: { Door.mode: -0x1, Body.mode: 0x5, Door.lamp: 0x1 }\n"),
               "E 3 scenario-value: the value of Door.mode (enum) must be one of its items, by name or by value, not '-0x1': "
               "write a negative number in decimal (-1); the items are off (0), on (1), auto (2)\n"
               "E 3 scenario-value: the value of Body.mode (enum) must be one of its items, by name or by value, not '0x5': the "
               "items are off (0), on (1), auto (2)\n"
               "E 3 scenario-value: the value of Door.lamp (bool) must be true or false, not '0x1'\n");
    check_text(errors(kHead + "  - ramp: { Body.level: { to: 0x65, over_ms: 10 }, Body.ratio: { to: 0x1, over_ms: 10 } }\n"
                              "  - expect: { Body.level: { ge: -0x1 }, Body.body.speed: { le: 0x10 } }\n"
                              "  - wave: { Body.level: { pulse: { value: 0x65, width_ms: 10 } } }\n"),
               "E 3 scenario-value: to of the ramp of Body.level is 0x65 (101), outside its range (from 0 to 100)\n"
               "E 3 scenario-value: to of the ramp of Body.ratio must be a number, not '0x1': hexadecimal (0x...) is for "
               "integer and enum variables only; write 1\n"
               "E 4 scenario-value: ge of Body.level (integer) must be a whole number, not '-0x1': write a negative number in "
               "decimal (-1)\n"
               "E 4 scenario-value: le of Body.body.speed (real) must be a number, not '0x10': hexadecimal (0x...) is for "
               "integer and enum variables only; write 16\n"
               "E 5 scenario-value: value of the pulse of Body.level is 0x65 (101), outside its range (from 0 to 100)\n");
}

TEST_CASE(scenario_only_names_the_variants_it_is_for)
{
    const std::string text = "vehicle_sil_scenario: 1\n"
                             "only: [small, medium]\n"
                             "steps:\n"
                             "  - set: { Body.nothing: 1 }\n";
    ScenarioContext context;
    context.variants = {"small", "medium", "large"};
    context.variant = "large";
    // Not for this variant: said, and the steps are not checked (they may not fit it).
    const ScenarioRead other = read_scenario(text, catalog(), context);
    CHECK(other.not_for_variant && !other.scenario.has_value());
    CHECK(other.only == std::vector<std::string>({"small", "medium"}));
    CHECK_EQ(other.steps, std::size_t{1});
    check_text(lines(other.findings),
               "E 2 scenario-variant: the scenario is only for the variants small, medium (its only), not for large: run it "
               "with --variant small or --variant medium, or add large to only\n");
    // For this variant: read as any other.
    context.variant = "medium";
    const ScenarioRead mine = read_scenario(text, catalog(), context);
    CHECK(!mine.not_for_variant);
    CHECK(lines(mine.findings).find("E 4 scenario-variable: 'Body.nothing'") == 0);
    // The names must be variants of the configuration.
    context.variants = {"small", "large"};
    context.variant = "small";
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nonly: [small, medum, larg]\nsteps:\n  - set: { Body.level: 1 }\n", catalog(), context).findings),
               "E 2 scenario-variant: only names the variant 'medum', which the configuration does not define; its variants are "
               "small, large\n"
               "E 2 scenario-variant: only names the variant 'larg', which the configuration does not define (did you mean "
               "'large'?); its variants are small, large\n");
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nonly: [small]\nsteps:\n  - set: { Body.level: 1 }\n", catalog()).findings),
               "E 2 scenario-variant: only names small, but the configuration defines no variants: remove only, or add variants "
               "to the configuration (docs/simulation-config.md)\n");
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nonly: small\nsteps:\n  - set: { Body.level: 1 }\n", catalog(), context).findings),
               "E 2 value: only must be a list of the variants the scenario is for (only: [small, medium]), not 'small'; remove "
               "it for a scenario for every variant\n");
}

TEST_CASE(scenario_steps_that_use_an_absent_variable_are_skipped_and_the_others_run)
{
    ScenarioContext context;
    context.variant = "small";
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                 "  - after_ms: 30\n"
                                                 "    set: { Body.boost: 2, Door.level: 7 }\n"
                                                 "  - after_ms: 20\n"
                                                 "    wait_until: { Body.boost: { ge: 1 }, Door.boost: 1 }\n"
                                                 "    timeout_ms: 500\n"
                                                 "  - after_ms: 10\n"
                                                 "    ramp: { Body.boost: { to: 3, over_ms: 100 } }\n"
                                                 "  - after_ms: 10\n"
                                                 "    expect: { Body.level: 5, Door.level: 0 }\n",
                                         catalog_without_boost(), context);
    check_text(lines(r.findings),
               "N 4 scenario-step-skipped: the step is skipped (set Body.boost; Door.level = 7): Body.boost is absent: participant "
               "'Body' does not have it in the variant small (absent: true in its variables); the other steps run, also "
               "those that would need what this one does\n"
               "N 6 scenario-step-skipped: the step is skipped (wait_until Body.boost; Door.boost (timeout 500 ms)): Body.boost, "
               "Door.boost are absent: the participants 'Body', 'Door' do not have them in the variant small (absent: true in "
               "their variables); the other steps run, also those that would need what this one does\n"
               "N 9 scenario-step-skipped: the step is skipped (ramp Body.boost): Body.boost is absent: participant 'Body' does "
               "not have it in the variant small (absent: true in its variables); the other steps run, also those that would "
               "need what this one does\n");
    CHECK(r.scenario.has_value());
    CHECK_EQ(r.skipped, std::size_t{3});
    // A skipped step writes nothing (Door.level stays 0) and takes the time its text says: the
    // ramp's 100 ms, nothing for the set and for the wait. It has no time that decided it.
    World world;
    std::vector<std::string> events;
    const ScenarioRun run = world.run(*r.scenario);
    check_text(text_of(run),
               "passed over=180\n"
               "3 passed start=0 at=0 end=0\n"
               "4 skipped start=30 at=- end=30 scenario-skipped: Body.boost is absent: participant 'Body' does not have it in "
               "the variant small (absent: true in its variables)\n"
               "6 skipped start=50 at=- end=50 scenario-skipped: Body.boost, Door.boost are absent: the participants 'Body', "
               "'Door' do not have them in the variant small (absent: true in their variables)\n"
               "9 skipped start=60 at=- end=160 scenario-skipped: Body.boost is absent: participant 'Body' does not have it in "
               "the variant small (absent: true in its variables)\n"
               "11 passed start=170 at=170 end=170\n");
    CHECK_EQ(run.skipped(), std::size_t{3});
    check_text(joined(world.log), "Body.level=5 for 0 applied at 0\n");
    // A variable that is not said to be absent is an error as before.
    CHECK(lines(read_scenario(kHead + "  - set: { Body.boost: 2 }\n", catalog()).findings).find("E 3 scenario-variable: 'Body.boost': participant 'Body' has no variable 'boost'") == 0);
    // Only skipped steps: the scenario passes, and says that it looked at nothing.
    const ScenarioRead none = read_scenario(kHead + "  - set: { Body.boost: 2 }\n", catalog_without_boost());
    CHECK(none.scenario.has_value());
    World empty;
    const ScenarioRun nothing = empty.run(*none.scenario);
    check_text(text_of(nothing), "passed over=0\n"
                                 "3 skipped start=0 at=- end=0 scenario-skipped: Body.boost is absent: participant 'Body' does not "
                                 "have it in this configuration (absent: true in its variables)\n");
    // A failure before a skipped step leaves it skipped; the simulation's end does not fail it.
    World stopped;
    stopped.stop_at = at(40ms);
    const ScenarioRead late = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                    "  - at_ms: 100\n"
                                                    "    set: { Body.boost: 2 }\n"
                                                    "  - at_ms: 200\n"
                                                    "    expect: { Body.level: 5 }\n",
                                            catalog_without_boost());
    CHECK(late.scenario.has_value());
    const ScenarioRun cut = stopped.run(*late.scenario);
    CHECK(cut.status == ScenarioStatus::Incomplete);
    CHECK(cut.steps[1].status == StepStatus::Skipped && cut.steps[2].status == StepStatus::NotRun);
}

TEST_CASE(scenario_stop_does_not_extend_a_passed_run_for_a_skipped_suffix)
{
    const Scenario scenario = read(kHead + "  - set: { Body.level: 5 }\n"
                                           "  - at_ms: 100\n"
                                           "    set: { Body.boost: 2 }\n"
                                           "  - after_ms: 20\n"
                                           "    expect: { Body.boost: 1 }\n", catalog_without_boost());
    World limited;
    limited.stop_at = at(40ms);
    std::vector<ScenarioStepRun> notices;
    limited.on_step = [&](const ScenarioStepRun& step) {
        if (step.status == StepStatus::Skipped) notices.push_back(step);
    };
    const ScenarioRun cut = limited.run(scenario);
    CHECK(cut.status == ScenarioStatus::Passed);
    CHECK(cut.steps[0].status == StepStatus::Passed);
    CHECK(cut.steps[1].status == StepStatus::Skipped && cut.steps[2].status == StepStatus::Skipped);
    CHECK(cut.steps[1].start == at(100ms) && cut.steps[1].end == at(100ms));
    CHECK(cut.steps[2].start == at(120ms) && cut.steps[2].end == at(120ms));
    CHECK_EQ(notices.size(), std::size_t{2});
    CHECK(notices[0].start == cut.steps[1].start && notices[0].end == cut.steps[1].end && !notices[0].at);
    CHECK(notices[1].start == cut.steps[2].start && notices[1].end == cut.steps[2].end && !notices[1].at);
    CHECK_EQ(limited.ecus[0].level, std::int64_t{5});

    World unlimited;
    const ScenarioRun all = unlimited.run(scenario);
    CHECK(all.status == ScenarioStatus::Passed);
    CHECK(all.steps[1].start == at(100ms) && all.steps[2].start == at(120ms));

    // The known skipped steps are announced even if the earlier write later fails.
    const Scenario failure = read(kHead + "  - set: { Body.level: 13 }\n"
                                          "  - at_ms: 100\n"
                                          "    set: { Body.boost: 2 }\n", catalog_without_boost());
    World failed;
    failed.stop_at = at(40ms);
    std::vector<ScenarioStepRun> failed_notices;
    failed.on_step = [&](const ScenarioStepRun& step) {
        if (step.status == StepStatus::Skipped) failed_notices.push_back(step);
    };
    const ScenarioRun result = failed.run(failure);
    CHECK(result.status == ScenarioStatus::Failed);
    CHECK_EQ(failed_notices.size(), std::size_t{1});
    CHECK(failed_notices[0].start == at(100ms) && failed_notices[0].end == at(100ms));
}

/// The errors alone (lines()), without the notes and the warnings.
std::string errors_only(const std::vector<Finding>& findings)
{
    std::vector<Finding> errors;
    std::copy_if(findings.begin(), findings.end(), std::back_inserter(errors), [](const Finding& f) { return f.severity == Severity::Error; });
    return lines(errors);
}

/// The starts of the steps that are not skipped, "line:start_ms" each.
std::string starts_of(const ScenarioRun& run)
{
    std::string out;
    for (const ScenarioStepRun& step : run.steps)
    {
        if (step.status != StepStatus::Skipped)
        {
            out += std::to_string(step.line) + ":" + time_text(step.start) + " ";
        }
    }
    return out;
}

TEST_CASE(scenario_times_are_the_same_whether_a_step_with_a_known_length_is_skipped_or_not)
{
    // The ramp's over_ms and the expect's hold_for_ms are times of the text: a configuration
    // that skips the steps gives the steps after them the times of one that runs them.
    const std::string text = kHead + "  - set: { Body.level: 0 }\n"
                                     "  - ramp: { Body.boost: { to: 3, over_ms: 100 }, Body.ratio: { to: 1, over_ms: 40 } }\n"
                                     "  - after_ms: 10\n"
                                     "    expect: { Body.boost: { ge: 0 } }\n"
                                     "    hold_for_ms: 200\n"
                                     "  - expect: { Body.pulse: false }\n"
                                     "  - at_ms: 400\n"
                                     "    set: { Body.lamp: true }\n";
    ScenarioCatalog with = catalog();
    auto& boost = with[0].variables.emplace_back();
    boost.name = "boost";
    boost.type = control::ValueType::Integer;
    boost.causality = control::Causality::Input;
    const ScenarioRead there = read_scenario(text, with);
    const ScenarioRead absent = read_scenario(text, catalog_without_boost());
    CHECK(there.scenario.has_value() && absent.scenario.has_value());
    CHECK_EQ(there.skipped, std::size_t{0});
    CHECK_EQ(absent.skipped, std::size_t{2});
    // The mixed ramp is skipped whole, with its longest over_ms, the absent variable's.
    CHECK_EQ(absent.scenario->steps[1].skipped_ms, std::int64_t{100});
    CHECK_EQ(absent.scenario->steps[2].skipped_ms, std::int64_t{200});
    World a;
    const ScenarioRun skipped = a.run(*absent.scenario);
    CHECK(skipped.status == ScenarioStatus::Passed);
    check_text(starts_of(skipped), "3:0 8:310 9:400 ");
    CHECK(skipped.steps[1].start == at(0ms) && skipped.steps[1].end == at(100ms) && !skipped.steps[1].at);
    CHECK(skipped.steps[2].start == at(110ms) && skipped.steps[2].end == at(310ms) && !skipped.steps[2].at);
    // Where time would go back is the same in both: at_ms inside the skipped ramp.
    const std::string back = kHead + "  - ramp: { Body.boost: { to: 3, over_ms: 100 } }\n"
                                     "  - at_ms: 50\n"
                                     "    set: { Body.lamp: true }\n";
    const std::string wrong = "E 4 scenario-time: at_ms is 50, before the end of the step before it (100 ms): the steps run in "
                              "the order of the file, and time does not go back; reorder the steps or raise at_ms\n";
    check_text(errors_only(read_scenario(back, with).findings), wrong);
    check_text(errors_only(read_scenario(back, catalog_without_boost()).findings), wrong);
    // A skipped step that waits takes no time (when it would end is not in the text), and the
    // text is read as if it waited: no at_ms after it.
    const ScenarioRead waits = read_scenario(kHead + "  - expect: { Body.boost: 1 }\n"
                                                     "    within_ms: 300\n"
                                                     "  - after_ms: 20\n"
                                                     "    expect: { Body.pulse: false }\n",
                                             catalog_without_boost());
    CHECK(waits.scenario.has_value());
    CHECK_EQ(waits.scenario->steps[0].skipped_ms, std::int64_t{0});
    World w;
    check_text(starts_of(w.run(*waits.scenario)), "5:20 ");
    CHECK(errors_only(read_scenario(kHead + "  - expect: { Body.boost: 1 }\n"
                                            "    within_ms: 300\n"
                                            "  - at_ms: 400\n"
                                            "    expect: { Body.pulse: false }\n",
                                    catalog_without_boost())
                          .findings)
              .find("E 5 scenario-time: at_ms after a step that waits (line 3)") == 0);
    // The over_ms of an absent variable is read like any other time; its to and from are not.
    check_text(errors_only(read_scenario(kHead + "  - ramp: { Body.boost: { to: high, over_ms: soon } }\n", catalog_without_boost()).findings),
               "E 3 value: over_ms must be a whole number (milliseconds of virtual time the ramp takes), not 'soon'\n");
}

TEST_CASE(scenario_steps_that_use_a_participant_the_variant_leaves_out_are_skipped)
{
    // Door is not in the variant (its enabled: false there): like an absent variable, whatever
    // the variable's name. Spare is left out in another way (--disable): an error as before.
    ScenarioCatalog c = catalog_without_boost();
    c[1] = CatalogParticipant{"Door", "", SimDuration{}, {}};
    c[1].not_in_variant = true;
    ScenarioContext context;
    context.variants = {"small", "medium"};
    context.variant = "medium";
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                 "  - set: { Door.level: 7 }\n"
                                                 "  - expect: { Door.anything: 1, Door.level: 7, Body.boost: 1 }\n"
                                                 "    hold_for_ms: 50\n"
                                                 "  - expect: { Body.level: 5 }\n",
                                         c, context);
    check_text(lines(r.findings),
               "N 4 scenario-step-skipped: the step is skipped (set Door.level): participant 'Door' is not in the variant medium "
               "(enabled: false in that variant); the other steps run, also those that would need what this one does\n"
               "N 5 scenario-step-skipped: the step is skipped (expect Door.anything; Door.level; Body.boost (for 50 ms)): "
               "participant 'Door' is not in the variant medium (enabled: false in that variant); Body.boost is absent: "
               "participant 'Body' does not have it in the variant medium (absent: true in its variables); the other steps "
               "run, also those that would need what this one does\n");
    CHECK(r.scenario.has_value());
    World world;
    const ScenarioRun run = world.run(*r.scenario);
    CHECK(run.status == ScenarioStatus::Passed);
    check_text(starts_of(run), "3:0 7:50 ");
    CHECK(run.steps[1].message == "participant 'Door' is not in the variant medium (enabled: false in that variant)");
    // The other ways of being left out do not say "variant", and do not skip.
    check_text(lines(read_scenario(kHead + "  - set: { Spare.level: 1 }\n", c, context).findings),
               "E 3 scenario-variable: 'Spare.level': participant 'Spare' is left out of this run (enabled: false, or --disable)\n");
}

TEST_CASE(scenario_that_checks_nothing_after_the_skips_gets_a_warning)
{
    ScenarioContext context;
    context.variants = {"small", "medium", "large"};
    context.variant = "small";
    // Every step that checks is skipped; the set that is left checks nothing.
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                 "  - expect: { Body.boost: 1 }\n"
                                                 "  - wait_until: { Body.boost: 2 }\n"
                                                 "    timeout_ms: 100\n",
                                         catalog_without_boost(), context);
    CHECK(r.scenario.has_value());
    std::string warnings;
    for (const Finding& f : r.findings)
    {
        if (f.severity == Severity::Warning)
        {
            warnings += lines({f});
        }
    }
    check_text(warnings,
               "W 0 scenario-nothing-checked: the scenario checks nothing in the variant small: all its 2 steps that check "
               "(expect or wait_until, lines 4, 5) are skipped, so it passes whatever the ECUs do (the notes say why each is "
               "skipped). If the scenario is not for this variant, list the variants it is for in the scenario (only: [medium, "
               "large]); if it is, map what the steps look at to what the variant has (variables in the variant, "
               "docs/simulation-config.md)\n");
    // Without variants: no "only" to write.
    const ScenarioRead plain = read_scenario(kHead + "  - expect: { Body.boost: 1 }\n", catalog_without_boost());
    check_text(lines(plain.findings).substr(lines(plain.findings).find("W 0")),
               "W 0 scenario-nothing-checked: the scenario checks nothing in this configuration: its only step that checks "
               "(expect or wait_until, line 3) is skipped, so it passes whatever the ECUs do (the notes say why each is "
               "skipped). If the configuration has what the steps look at under other names, map them in the participant's "
               "variables (docs/simulation-config.md); else do not run this scenario with it\n");
    // One step that checks is left: no warning. Nor for a scenario that never checked anything.
    const auto warned = [&](const std::string& steps) {
        const ScenarioRead read = read_scenario(kHead + steps, catalog_without_boost(), context);
        return std::any_of(read.findings.begin(), read.findings.end(), [](const Finding& f) { return f.severity != Severity::Note; });
    };
    CHECK(!warned("  - expect: { Body.boost: 1 }\n  - expect: { Body.level: 0 }\n"));
    CHECK(!warned("  - set: { Body.boost: 1 }\n  - set: { Body.level: 1 }\n"));
    CHECK(!warned("  - set: { Body.boost: 1 }\n"));
}

TEST_CASE(scenario_parameter_without_quotes_in_braces_gets_a_hint)
{
    // YAML does not take "${" in { ... } without quotes: the syntax error says what to write.
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: ${n} }\n", catalog());
    CHECK_EQ(r.findings.size(), std::size_t{1});
    CHECK(r.findings[0].code == "syntax" && r.findings[0].line == 3);
    CHECK(r.findings[0].message.find(": inside { ... } or [ ... ], write a parameter in quotes (\"${name}\")") != std::string::npos);
}

// --- wave ---

namespace {

/// The findings of a scenario's steps (after kHead), with Body and Door stepping as given.
std::string wave_findings(const std::string& steps, SimDuration body_step = 10ms, SimDuration door_step = 10ms)
{
    return lines(read_scenario(kHead + steps, catalog(body_step, door_step)).findings);
}

/// What was applied to one variable, "value@time" each ("Body.level": "10@0 12@10 ").
std::string applied(const World& world, const std::string& variable)
{
    std::string out;
    for (const std::string& entry : world.log)
    {
        const std::string head = variable + "=";
        if (entry.rfind(head, 0) == 0)
        {
            const std::size_t forAt = entry.find(" for ");
            out += entry.substr(head.size(), forAt - head.size()) + "@" + entry.substr(entry.rfind(' ') + 1) + " ";
        }
    }
    return out;
}

} // namespace

TEST_CASE(scenario_wave_is_read_with_its_shape_and_how_long_it_lasts)
{
    const Scenario s = read(
        kHead + "  - wave: { Body.ratio: { sine: { amplitude: 0.2, period_ms: 200 }, around: 0.5, for_ms: 1000 } }\n"
                "  - wave: { Body.level: { triangle: { amplitude: 5, frequency_hz: 2.5, phase_deg: 90 }, around: 50, for_ms: 300 },\n"
                "            Door.level: { sawtooth: { amplitude: 5, period_ms: 100, falling: true }, around: 50, for_ms: 250 } }\n"
                "  - wave: { Body.level: { square: { low: 10, high: 20, period_ms: 100, duty: 30 }, for_ms: 200 } }\n"
                "  - wave: { Body.level: { square: { amplitude: 3, period_ms: 100 }, around: 9, for_ms: 200 }, Door.lamp: { square: { period_ms: 40 }, for_ms: 80 } }\n"
                "  - wave: { Door.lamp: { pulse: { width_ms: 30 } }, Body.level: { pulse: { value: 7, width_ms: 50 } } }\n"
                "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 20, seed: 7 }, around: 0.5, for_ms: 100 } }\n"
                "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 20, seed: 7, distribution: gaussian }, for_ms: 100 } }\n"
                "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 1, to_hz: 5, sweep: log }, around: 0.5, for_ms: 2000 } }\n"
                "  - wave: { Body.level: { lag: { from: 0, to: 80, tau_ms: 200, settle: true }, for_ms: 1000 } }\n"
                "  - wave: { Door.lamp: { chatter: { to: true, bounces: 3, interval_ms: 10 } }, Body.level: { chatter: { from: 0, to: 1, interval_ms: 20, seed: 4 }, for_ms: 100 } }\n"
                "  - wave: { Body.level: { points: [[0, 0], [100, 50], [400, 20]] }, Door.mode: { points: [[0, auto], [50, off], [80, 1]] },\n"
                "            Door.lamp: { points: [[10, true], [30, false]], interpolate: hold } }\n"
                "  - after_ms: 5\n"
                "    expect: { Body.level: 20 }\n");
    std::string texts;
    for (const auto& step : s.steps)
    {
        texts += std::to_string(step.line) + ": " + step.text + "\n";
    }
    check_text(texts,
               "3: wave Body.ratio sine amplitude 0.2 period 200 ms around 0.5 for 1000 ms\n"
               "4: wave Body.level triangle amplitude 5 frequency 2.5 Hz phase 90 deg around 50 for 300 ms; Door.level sawtooth "
               "amplitude 5 period 100 ms falling around 50 for 250 ms\n"
               "6: wave Body.level square low 10 high 20 period 100 ms duty 30 % for 200 ms\n"
               "7: wave Body.level square amplitude 3 period 100 ms around 9 for 200 ms; Door.lamp square period 40 ms for 80 ms\n"
               "8: wave Door.lamp pulse true width 30 ms; Body.level pulse 7 width 50 ms\n"
               "9: wave Body.ratio noise amplitude 0.1 hold 20 ms seed 7 around 0.5 for 100 ms\n"
               "10: wave Body.ratio noise amplitude 0.1 hold 20 ms seed 7 gaussian for 100 ms\n"
               "11: wave Body.ratio chirp amplitude 0.1 from 1 Hz to 5 Hz log around 0.5 for 2000 ms\n"
               "12: wave Body.level lag from 0 to 80 tau 200 ms settle for 1000 ms\n"
               "13: wave Door.lamp chatter to true 3 bounces interval 10 ms; Body.level chatter from 0 to 1 interval 20 ms seed 4 for 100 ms\n"
               "14: wave Body.level points (3, linear) over 400 ms; Door.mode points (3, hold) over 80 ms; Door.lamp points (2, hold) "
               "over 30 ms\n"
               "16: expect Body.level eq 20\n");
    CHECK(s.steps[0].kind == StepKind::Wave);
    CHECK(std::string(step_kind_name(StepKind::Wave)) == "wave");
    // Each step lasts as long as its longest wave: the last expect is 5 ms after the points end.
    World world;
    const ScenarioRun run = world.run(s, 10s);
    CHECK(run.status == ScenarioStatus::Passed);
    check_text(starts_of(run), "3:0 4:1000 6:1300 7:1500 8:1700 9:1750 10:1850 11:1950 12:3950 13:4950 14:5050 16:5455 ");
}

TEST_CASE(scenario_wave_writes_its_shape_in_the_steps_of_its_ecu_and_returns_to_the_middle)
{
    // A triangle of 80 ms around 10 for 100 ms (not a whole number of periods): a value per step
    // of Body from the time alone, rounded for the integer, and at the end the middle again.
    // Door's bool is high for a quarter of each 40 ms and low at the end.
    const Scenario s = read(kHead + "  - at_ms: 100\n"
                                    "    wave: { Body.level: { triangle: { amplitude: 4, period_ms: 80 }, around: 10, for_ms: 100 },\n"
                                    "            Door.lamp: { square: { period_ms: 40, duty: 25 }, for_ms: 80 } }\n"
                                    "  - expect: { Body.level: 10, Door.lamp: false }\n");
    World world;
    const ScenarioRun run = world.run(s);
    check_text(text_of(run), "passed over=210\n"
                             "3 passed start=100 at=200 end=200\n"
                             "6 passed start=200 at=200 end=200\n");
    check_text(applied(world, "Body.level"), "10@100 12@110 14@120 12@130 10@140 8@150 6@160 8@170 10@180 12@190 10@200 ");
    check_text(applied(world, "Door.lamp"), "true@100 false@110 false@120 false@130 true@140 false@150 false@160 false@170 false@180 ");
}

TEST_CASE(scenario_wave_without_around_swings_around_the_value_the_variable_has)
{
    // After a ramp to 80: around 80, and 80 again at the end. Then around what the wave left.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n"
                                    "  - ramp: { Body.level: { to: 80, over_ms: 20 } }\n"
                                    "  - wave: { Body.level: { sawtooth: { amplitude: 10, period_ms: 40 }, for_ms: 60 } }\n"
                                    "  - wave: { Body.level: { square: { amplitude: 5, period_ms: 20 }, for_ms: 30 } }\n");
    World world;
    const ScenarioRun run = world.run(s);
    check_text(text_of(run), "passed over=120\n"
                             "3 passed start=0 at=0 end=0\n"
                             "4 passed start=0 at=20 end=20\n"
                             "5 passed start=20 at=80 end=80\n"
                             "6 passed start=80 at=110 end=110\n");
    check_text(applied(world, "Body.level"), "20@0 20@0 50@10 80@20 "
                                             "70@20 75@30 80@40 85@50 70@60 75@70 80@80 "
                                             "85@80 75@90 85@100 80@110 ");
    // Without a value to swing around, the wave says so, and what to write.
    World empty;
    check_text(text_of(empty.run(read(kHead + "  - wave: { Body.level: { sine: { amplitude: 4, period_ms: 80 }, for_ms: 20 } }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=20 scenario-write: the wave of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 0 ms and the scenario has not written it; give the wave around: <value>\n");
    // A pulse returns to the value the variable has: without one, it says how to give it one.
    World bare;
    check_text(text_of(bare.run(read(kHead + "  - wave: { Body.level: { pulse: { value: 9, width_ms: 20 } } }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=20 scenario-write: the wave of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 0 ms and the scenario has not written it; set the variable in a step before the pulse (set: { "
               "Body.level: <value> })\n");
    // A shape that has its values itself needs none.
    World own;
    CHECK(own.run(read(kHead + "  - wave: { Body.level: { square: { low: 1, high: 2, period_ms: 20 }, for_ms: 20 }, Door.lamp: { pulse: { width_ms: 10 } },\n"
                               "            Door.level: { points: [[0, 3], [20, 5]] }, Body.lamp: { chatter: { to: true, bounces: 1, interval_ms: 10 } } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(own, "Body.level"), "2@0 1@10 1@20 ");
    check_text(applied(own, "Door.lamp"), "true@0 false@10 ");
    check_text(applied(own, "Door.level"), "3@0 4@10 5@20 ");
    check_text(applied(own, "Body.lamp"), "true@0 false@10 true@20 ");
}

TEST_CASE(scenario_ramp_and_wave_start_from_what_the_scenario_wrote_whatever_the_samples_since_say)
{
    // The ECU writes 60 to its own input at 30 ms, and its samples say so. The ramp without from
    // still starts from the 20 the scenario wrote, and the wave without around swings around the
    // 80 the ramp left.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n"
                                    "  - at_ms: 50\n"
                                    "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"
                                    "  - at_ms: 100\n"
                                    "    wave: { Body.level: { square: { amplitude: 5, period_ms: 20 }, for_ms: 20 } }\n");
    World world;
    world.ecus[0].own_level = std::pair{at(30ms), std::int64_t{60}};
    World later;
    later.ecus[0].own_level = std::pair{at(80ms), std::int64_t{60}};
    for (World* w : {&world, &later})
    {
        CHECK(w->run(s).status == ScenarioStatus::Passed);
        check_text(applied(*w, "Body.level"), "20@0 20@50 50@60 80@70 85@100 75@110 80@120 ");
    }
    // Before the scenario has written the variable, the sample is all there is: from the ECU's 60.
    World first;
    first.ecus[0].own_level = std::pair{at(30ms), std::int64_t{60}};
    CHECK(first.run(read(kHead + "  - at_ms: 50\n"
                                 "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(first, "Body.level"), "60@50 70@60 80@70 ");
}

TEST_CASE(scenario_wave_shapes_that_go_somewhere_stay_there)
{
    // A lag stays where it has come to (or at `to` with settle), a chatter at `to`, points at the
    // last of them; a pulse returns. (The third step lasts 30 ms, as its longest wave.)
    const Scenario s = read(kHead + "  - wave: { Body.level: { lag: { from: 0, to: 100, tau_ms: 20 }, for_ms: 40 },\n"
                                    "            Door.level: { lag: { from: 0, to: 100, tau_ms: 20, settle: true }, for_ms: 40 } }\n"
                                    "  - wave: { Body.level: { chatter: { to: 50, bounces: 2, interval_ms: 10 } } }\n"
                                    "  - wave: { Body.level: { pulse: { value: 9, width_ms: 20 } }, Door.mode: { points: [[0, on], [20, auto], [30, off]] } }\n"
                                    "  - wave: { Body.level: { points: [[20, 60], [40, 0]], interpolate: hold } }\n"
                                    "  - wave: { Body.level: { points: [[20, 60], [40, 10]] } }\n");
    World world;
    const ScenarioRun run = world.run(s);
    CHECK(run.status == ScenarioStatus::Passed);
    // 100 (1 - e^-0.5) = 39.3, (1 - e^-1) = 63.2, (1 - e^-1.5) = 77.7, (1 - e^-2) = 86.5.
    check_text(applied(world, "Body.level"), "0@0 39@10 63@20 78@30 86@40 "
                                             "50@40 86@50 50@60 86@70 50@80 "
                                             "9@80 9@90 50@100 "
                                             "50@110 50@120 60@130 60@140 0@150 "
                                             "0@150 30@160 60@170 35@180 10@190 ");
    check_text(applied(world, "Door.level"), "0@0 39@10 63@20 78@30 100@40 ");
    check_text(applied(world, "Door.mode"), "1@80 1@90 2@100 0@110 ");
}

TEST_CASE(scenario_wave_gives_the_same_value_at_the_same_time_whatever_the_steps_are)
{
    // The same noise and the same chatter to an ECU that steps every 10 ms and to the same ECU
    // stepping every 4 ms, with a controller of 10 ms and of 20 ms: at the times both step (every
    // 20 ms), the same values.
    const std::string text = kHead + "  - wave: { Body.ratio: { noise: { amplitude: 0.4, hold_ms: 20, seed: 11 }, around: 0.5, for_ms: 200 },\n"
                                     "            Body.level: { chatter: { from: 0, to: 9, interval_ms: 20, seed: 3 }, for_ms: 200 } }\n";
    const auto at_shared_times = [](const World& world, const std::string& variable) {
        std::string out;
        for (const std::string& entry : world.log)
        {
            const std::int64_t when = std::stoll(entry.substr(entry.rfind(' ') + 1));
            if (entry.rfind(variable + "=", 0) == 0 && when % 20 == 0)
            {
                out += entry.substr(entry.find('=') + 1) + "\n";
            }
        }
        return out;
    };
    std::string first;
    std::string firstLevel;
    for (const SimDuration step : {SimDuration{10ms}, SimDuration{4ms}})
    {
        for (const SimDuration tick : {SimDuration{10ms}, SimDuration{20ms}})
        {
            const Scenario s = read(text, catalog(step, 10ms));
            World world;
            world.tick = tick;
            world.ecus[0].step = step;
            CHECK(world.run(s).status == ScenarioStatus::Passed);
            const std::string ratio = at_shared_times(world, "Body.ratio");
            const std::string level = at_shared_times(world, "Body.level");
            // A value per stretch of hold_ms: 10 of them, and the middle at the end.
            CHECK_EQ(std::count(ratio.begin(), ratio.end(), '\n'), std::ptrdiff_t{11});
            CHECK(first.empty() || (first == ratio && firstLevel == level));
            first = ratio;
            firstLevel = level;
        }
    }
    // The values are those of the seed and of the variable's name, on every platform (whole
    // numbers alone make them): the first of them, and the middle at the end.
    check_text(first.substr(0, first.find('\n') + 1), "0.4353229041685524 for 0 applied at 0\n");
    CHECK(first.find("0.5 for 200 applied at 200\n") != std::string::npos);
    // Another seed gives other values. So does the same seed for another variable: two inputs
    // with one seed do not move together.
    const Scenario other = read(kHead + "  - wave: { Body.ratio: { noise: { amplitude: 0.4, hold_ms: 20, seed: 12 }, around: 0.5, for_ms: 200 },\n"
                                        "            Door.ratio: { noise: { amplitude: 0.4, hold_ms: 20, seed: 11 }, around: 0.5, for_ms: 200 },\n"
                                        "            Door.level: { chatter: { from: 0, to: 9, interval_ms: 20, seed: 3 }, for_ms: 200 } }\n");
    World world;
    CHECK(world.run(other).status == ScenarioStatus::Passed);
    CHECK(at_shared_times(world, "Body.ratio") != first);
    CHECK(at_shared_times(world, "Door.ratio") != first);
    CHECK(at_shared_times(world, "Door.ratio") != at_shared_times(world, "Body.ratio"));
    CHECK(at_shared_times(world, "Door.level") != firstLevel);
}

TEST_CASE(scenario_wave_rounds_a_half_for_an_integer_the_same_way_whatever_the_last_bits_are)
{
    // A sine of amplitude 1 and 120 ms is a half at 10 ms and at 50 ms, but for the last bits of
    // the sine: an integer gets 1 there (halves go away from zero), not 0 on one platform and 1
    // on another.
    World world;
    CHECK(world.run(read(kHead + "  - set: { Body.level: 0 }\n"
                                 "  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 120 }, for_ms: 60 } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "0@0 0@0 1@10 1@20 1@30 1@40 1@50 0@60 ");
}

TEST_CASE(scenario_pulse_shorter_than_a_step_is_seen_if_a_step_of_the_ecu_is_in_it)
{
    // Body steps at 0, 10, 20, ... ms. A pulse of 5 ms from 107 ms: its value is applied in the
    // step at 110 ms and what follows it in the one at 120 ms, so the ECU has it for one step.
    // From 201 ms: both are applied in the step at 210 ms, where the later one wins. From 305 ms,
    // ending at a step: the same.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n"
                                    "  - at_ms: 107\n    wave: { Body.level: { pulse: { value: 50, width_ms: 5 } } }\n    undersampled: ok\n"
                                    "  - at_ms: 201\n    wave: { Body.level: { pulse: { value: 60, width_ms: 5 } } }\n    undersampled: ok\n"
                                    "  - at_ms: 305\n    wave: { Body.level: { pulse: { value: 70, width_ms: 5 } } }\n    undersampled: ok\n");
    World world;
    CHECK(world.run(s).status == ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "20@0 50@110 20@120 60@210 20@210 70@310 20@310 ");
    // The check says which it is, where the time of the step is known before the run.
    check_text(wave_findings("  - set: { Body.level: 20 }\n"
                             "  - at_ms: 107\n    wave: { Body.level: { pulse: { value: 50, width_ms: 5 } } }\n"
                             "  - at_ms: 201\n    wave: { Body.level: { pulse: { value: 60, width_ms: 5 } } }\n"
                             "  - at_ms: 305\n    wave: { Body.level: { pulse: { value: 70, width_ms: 5 } } }\n"
                             "  - wait_until: { Body.level: 20 }\n    timeout_ms: 100\n"
                             "  - wave: { Body.level: { pulse: { value: 70, width_ms: 5 } } }\n"),
               "W 5 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 ms): "
               "this step begins at 107 ms, so the ECU has the pulse's value for one whole step of 10 ms (the one at 110 ms), not for 5 "
               "ms. Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 7 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 ms): "
               "this step begins at 201 ms, and none of the ECU's steps is from then until the pulse ends (206 ms), so the ECU never gets "
               "the pulse's value. Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 9 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 ms): "
               "this step begins at 305 ms, and none of the ECU's steps is from then until the pulse ends (310 ms), so the ECU never gets "
               "the pulse's value. Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 12 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 "
               "ms): the ECU has the pulse's value for one whole step if one of its steps is from the pulse's beginning until its end, "
               "and never otherwise; when this step begins is not known before the run. Make width_ms 10 or more; to run it as it is "
               "without this message, add undersampled: ok to the step\n");
}

TEST_CASE(scenario_points_that_are_kept_may_jump_and_a_point_the_ecu_never_gets_is_said)
{
    // interpolate: hold is how a value jumps: points 1 ms apart say nothing, as long as each is
    // written once (Body steps every 10 ms; the wave writes at 0, 10, 20, ... ms of the step).
    check_text(wave_findings("  - wave: { Body.level: { points: [[0, 0], [20, 5], [21, 60], [40, 0]], interpolate: hold } }\n"), "");
    World world;
    CHECK(world.run(read(kHead + "  - wave: { Body.level: { points: [[0, 0], [20, 60], [40, 0]], interpolate: hold } }\n")).status ==
          ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "0@0 0@10 60@20 60@30 0@40 ");
    // A point that no write carries; a point before the end whose one write may meet the end's
    // in one step of the ECU: at a time that is known it does (101 ms) or does not (100 ms), and
    // after a wait that is not known.
    check_text(wave_findings("  - wave: { Body.level: { points: [[0, 0], [21, 5], [25, 6], [40, 0]], interpolate: hold } }\n"
                             "  - at_ms: 100\n    wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"
                             "  - at_ms: 201\n    wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"
                             "  - wait_until: { Body.level: 6 }\n    timeout_ms: 100\n"
                             "  - wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"),
               "W 3 scenario-wave-undersampled: the points of Body.level keep point 2 from 21 ms to 25 ms, and participant 'Body' takes "
               "its inputs once per step of 10 ms: the wave writes a value every 10 ms from the step's beginning, and none of those "
               "times is in that span, so the value of point 2 is never applied. Put the points at multiples of 10 ms; to run it as it "
               "is without this message, add undersampled: ok to the step\n"
               "W 7 scenario-wave-undersampled: the points of Body.level keep point 2 from 12 ms to 25 ms, where they end, and participant "
               "'Body' takes its inputs once per step of 10 ms: this step begins at 201 ms, and none of the ECU's steps is from 221 ms "
               "(when the value of point 2 is written) until the points end (226 ms), so that value is never applied. Put the points at "
               "multiples of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 10 scenario-wave-undersampled: the points of Body.level keep point 2 from 12 ms to 25 ms, where they end, and "
               "participant 'Body' takes its inputs once per step of 10 ms: the value of point 2 is applied only if one of the ECU's "
               "steps is from 20 ms to 25 ms after this step begins, and when it begins is not known before the run. Put the points at "
               "multiples of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n");
    // The span of a point ends where the next begins: a write at that time carries the next.
    check_text(wave_findings("  - wave: { Body.level: { points: [[0, 0], [15, 5], [20, 6], [40, 0]], interpolate: hold } }\n"),
               "W 3 scenario-wave-undersampled: the points of Body.level keep point 2 from 15 ms to 20 ms, and participant 'Body' takes "
               "its inputs once per step of 10 ms: the wave writes a value every 10 ms from the step's beginning, and none of those "
               "times is in that span, so the value of point 2 is never applied. Put the points at multiples of 10 ms; to run it as it "
               "is without this message, add undersampled: ok to the step\n");
    // What the check says of 201 ms is what happens: 5 and 6 are both applied at 230 ms.
    World late;
    CHECK(late.run(read(kHead + "  - at_ms: 201\n    wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"
                                "    undersampled: ok\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(late, "Body.level"), "0@210 0@220 5@230 6@230 ");
}

TEST_CASE(scenario_wave_takes_parameters_where_a_number_stands)
{
    ScenarioContext context;
    context.params = {param("amp", "3", ParamSource::CommandLine), param("period", "80", ParamSource::CommandLine),
                      param("top", "9", ParamSource::CommandLine)};
    const ScenarioRead r = read_scenario(
        kHead + "  - wave: { Body.level: { triangle: { amplitude: \"${amp}\", period_ms: \"${period}\" }, around: 20, for_ms: \"${period}\" } }\n"
                "  - wave: { Body.level: { points: [[0, 1], [20, \"${top}\"]] } }\n",
        catalog(), context);
    check_text(lines(r.findings), "");
    check_text(r.scenario->steps[0].text + "\n" + r.scenario->steps[1].text + "\n",
               "wave Body.level triangle amplitude 3 period 80 ms around 20 for 80 ms\n"
               "wave Body.level points (2, linear) over 20 ms\n");
    World world;
    CHECK(world.run(*r.scenario).status == ScenarioStatus::Passed);
    // (21.5 and 18.5 are rounded away from zero, as a ramp's values are.)
    check_text(applied(world, "Body.level"), "20@0 22@10 23@20 22@30 20@40 19@50 17@60 19@70 20@80 1@80 5@90 9@100 ");
    // A parameter that does not fit says where it comes from.
    context.params[0] = param("amp", "big", ParamSource::CommandLine);
    check_text(lines(read_scenario(kHead + "  - wave: { Body.level: { sine: { amplitude: \"${amp}\", period_ms: 80 }, for_ms: 80 } }\n",
                                   catalog(), context)
                         .findings),
               "E 3 value: amplitude of the sine of Body.level must be a number (how far the value goes above and below the middle), "
               "not 'big' (the parameter amp, from --param)\n");
}

TEST_CASE(scenario_wave_errors_say_what_is_wrong_and_what_to_write)
{
    // The form.
    check_text(wave_findings("  - wave: 5\n"),
               "E 3 value: wave must be a mapping of variables to a shape and how long it lasts ({ Body.level: { sine: { amplitude: 1, "
               "period_ms: 100 }, for_ms: 1000 } }), not '5'\n");
    check_text(wave_findings("  - wave: { Body.level: { for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 100 }, square: { period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sinus: { amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 100 } } }\n"
                             "  - wave: { Body.level: { sine: 5, for_ms: 100 } }\n"),
               "E 3 value: wave of Body.level names no shape: give it one of sine, triangle, sawtooth, square, pulse, noise, chirp, lag, "
               "chatter, points\n"
               "E 4 value: wave of Body.level has sine, square: a variable takes one shape in a step; make a step of each\n"
               "E 5 unknown-key: wave of Body.level: unknown key 'sinus': did you mean 'sine'? (the keys here: sine, triangle, sawtooth, "
               "square, pulse, noise, chirp, lag, chatter, points, around, for_ms, interpolate)\n"
               "E 5 value: wave of Body.level names no shape: give it one of sine, triangle, sawtooth, square, pulse, noise, chirp, lag, "
               "chatter, points\n"
               "E 6 missing-key: wave of Body.level needs for_ms: how long the sine lasts, in milliseconds of virtual time\n"
               "E 7 value: the sine of Body.level must be a mapping of its values ({ amplitude: 1, period_ms: 100 }), not '5'\n");
    // What cannot run: a period or a width of 0, no seed, times that do not go up.
    check_text(wave_findings("  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 0 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, frequency_hz: -2 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, frequency_hz: 5, period_ms: 200 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: -1 }, for_ms: 0 } }\n"
                             "  - wave: { Body.level: { pulse: { value: 1, width_ms: 0 } } }\n"
                             "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 10 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 1], [50, 2], [50, 3], [40, 4]] } }\n"
                             "  - wave: { Body.level: { points: [[0, 1]] } }\n"
                             "  - wave: { Body.level: { points: [[0, 1, 2], 5] } }\n"
                             "  - wave: { Body.level: { square: { low: 1, high: 2, period_ms: 100, duty: 101 }, for_ms: 100 } }\n"),
               "E 3 value: period_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time of one period)\n"
               "E 4 value: frequency_hz of the sine of Body.level is -2, but it must be from 1e-09 to 1e+09 (periods per second of virtual "
               "time)\n"
               "E 5 value: the sine of Body.level has period_ms and frequency_hz: give one of them (frequency_hz: 50 is period_ms: 20)\n"
               "E 6 value: amplitude of the sine of Body.level is -1, but it must be from 0 (how far the value goes above and below the "
               "middle)\n"
               "E 6 missing-key: the sine of Body.level needs period_ms or frequency_hz: how long one period is, or how many there are in "
               "a second\n"
               "E 6 value: for_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time the wave takes)\n"
               "E 7 value: width_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time the pulse lasts)\n"
               "E 8 missing-key: the noise of Body.ratio needs seed: a whole number that picks the values; the same seed gives the same "
               "values in every run, so that a failure can be run again\n"
               "E 9 value: the times of the points of Body.level must go up: point 3 is at 50 ms, not after point 2 (50 ms); for a "
               "value that jumps, write interpolate: hold beside the points (each value is then kept until the next point)\n"
               "E 9 value: the times of the points of Body.level must go up: point 4 is at 40 ms, not after point 3 (50 ms)\n"
               "E 10 value: the points of Body.level end at 0 ms: a wave takes time; give a later point, or use set for a value at one "
               "time\n"
               "E 11 value: point 1 of the points of Body.level must be [<milliseconds since the step began>, <value>], not a list\n"
               "E 11 value: point 2 of the points of Body.level must be [<milliseconds since the step began>, <value>], not '5'\n"
               "E 12 value: duty of the square of Body.level is 101, but it must be from 0 to 100 (the percent of each period for which it "
               "is high)\n");
    // A shape that does not fit the variable's type.
    check_text(wave_findings("  - wave: { Door.lamp: { sine: { amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Door.mode: { square: { period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Door.lamp: { square: { low: 0, high: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Door.lamp: { points: [[0, true], [50, false]], interpolate: linear } }\n"
                             "  - wave: { Door.mode: { points: [[0, auto], [50, open]] } }\n"
                             "  - wave: { Door.lamp: { chatter: { to: 1, bounces: 2, interval_ms: 10 } } }\n"
                             "  - wave: { Body.level_changed_at_ms: { pulse: { value: 1, width_ms: 20 } } }\n"
                             "  - wave: { Body.gain: { pulse: { value: 1, width_ms: 20 } } }\n"),
               "E 3 scenario-value: sine needs a number, and Door.lamp is a bool: for a bool use square, pulse, chatter or points\n"
               "E 4 scenario-value: square needs a number, and Door.mode is an enum: for an enum use points, or set\n"
               "E 5 value: low does not go with the square of Door.lamp: the square of a bool is false and true; remove it\n"
               "E 5 value: high does not go with the square of Door.lamp: the square of a bool is false and true; remove it\n"
               "E 6 scenario-value: interpolate: linear needs a number, and Door.lamp is a bool: write interpolate: hold (each value is "
               "kept until the next point), or leave it out\n"
               "E 7 scenario-value: the value of point 2 of the points of Door.mode (enum) must be one of its items, by name or by value, "
               "not 'open': the items are off (0), on (1), auto (2)\n"
               "E 8 scenario-value: to of the chatter of Door.lamp (bool) must be true or false, not '1'\n"
               "E 9 scenario-not-writable: Body.level_changed_at_ms is an output of its ECU: it can be read (expect, wait_until), not "
               "written; write one of the ECU's inputs instead\n"
               "E 10 scenario-not-writable: Body.gain is a parameter: it can be written only before the first step, that is with set in a "
               "step at 0 (at_ms: 0, or the first steps without a time), not with wave\n");
    // Outside the range, as far as the text says where the wave goes.
    check_text(wave_findings("  - wave: { Body.level: { sine: { amplitude: 5, period_ms: 100 }, around: 98, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { square: { low: -1, high: 101, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 5, period_ms: 100 }, around: 120, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 0], [100, 500]] } }\n"
                             "  - wave: { Body.level: { lag: { to: 101, tau_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { noise: { amplitude: 50, hold_ms: 10, seed: 1, distribution: gaussian }, around: 50, for_ms: 100 } }\n"),
               "E 3 scenario-value: the sine of Body.level goes from 93 to 103 (around 98, amplitude 5), outside its range (from 0 to "
               "100): lower the amplitude, or move around\n"
               "E 4 scenario-value: low of the square of Body.level is -1, outside its range (from 0 to 100)\n"
               "E 4 scenario-value: high of the square of Body.level is 101, outside its range (from 0 to 100)\n"
               "E 5 scenario-value: around of the sine of Body.level is 120, outside its range (from 0 to 100)\n"
               "E 6 scenario-value: the value of point 2 of the points of Body.level is 500, outside its range (from 0 to 100)\n"
               "E 7 scenario-value: to of the lag of Body.level is 101, outside its range (from 0 to 100)\n");
    // A chirp may begin or end at 0 Hz on a linear sweep, not on a log one, and it has to move.
    check_text(wave_findings("  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 0, to_hz: 5 }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 0, to_hz: 5, sweep: log }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 5, to_hz: 0, sweep: log }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 0, to_hz: 0 }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: -1, to_hz: 5 }, around: 0.5, for_ms: 1000 } }\n"),
               "E 4 value: from_hz of the chirp of Body.ratio is 0, but a log sweep changes the frequency by the same factor every "
               "second, which neither begins nor ends at 0 Hz: give a frequency above 0, or write sweep: linear\n"
               "E 5 value: to_hz of the chirp of Body.ratio is 0, but a log sweep changes the frequency by the same factor every "
               "second, which neither begins nor ends at 0 Hz: give a frequency above 0, or write sweep: linear\n"
               "E 6 value: the chirp of Body.ratio goes from 0 Hz to 0 Hz, which does not move: give from_hz or to_hz a frequency above "
               "0\n"
               "E 7 value: from_hz of the chirp of Body.ratio is -1, but it must be from 0 to 1e+09 (the frequency at the beginning)\n");
    // Keys that do not go together.
    check_text(wave_findings("  - wave: { Body.level: { pulse: { value: 1, width_ms: 20 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 1], [20, 2]], for_ms: 100, around: 5 } }\n"
                             "  - wave: { Body.level: { square: { low: 1, high: 2, amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { square: { low: 1, high: 2, period_ms: 100 }, around: 5, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { lag: { to: 1, tau_ms: 100 }, around: 5, for_ms: 100, interpolate: hold } }\n"
                             "  - wave: { Body.level: { chatter: { to: 1, bounces: 2, interval_ms: 10 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { chatter: { to: 1, interval_ms: 10 } } }\n"
                             "  - set: { Body.level: 1 }\n"
                             "    undersampled: ok\n"
                             "  - wave: { Body.level: { pulse: { value: 1, width_ms: 20 } } }\n"
                             "    undersampled: yes\n"
                             "  - wave: { Body.level: { pulse: { value: 1, width_ms: 20 }, around: 5 } }\n"),
               "E 3 value: for_ms does not go with pulse: a pulse lasts its width_ms; remove it\n"
               "E 4 value: for_ms does not go with points: the points end at the last of them; remove it\n"
               "E 4 value: around does not go with the points of Body.level: the points are the values themselves (for where it begins, "
               "start with a point at 0: [0, <value>]); remove it\n"
               "E 5 value: the square of Body.level has amplitude and low: give low and high (its two values), or amplitude (so far above "
               "and below the middle)\n"
               "E 6 value: around does not go with the square of Body.level: low and high are the values themselves; remove it\n"
               "E 7 value: interpolate goes with points, not with lag: remove it\n"
               "E 7 value: around does not go with the lag of Body.level: the value it starts from is its from (lag: { from: <value>, ... "
               "}); remove it\n"
               "E 8 value: the chatter of Body.level has bounces and for_ms: give one of them (bounces: how often it falls back; for_ms: "
               "how long it bounces)\n"
               "E 9 missing-key: the chatter of Body.level needs bounces (how often it falls back before it rests) or for_ms (how long it "
               "bounces)\n"
               "E 11 value: undersampled goes with wave and dither, not with set: remove it\n"
               "E 13 value: undersampled must be ok (run the step's waves as they are, also where they change faster than their ECU "
               "steps), not 'yes': write undersampled: ok, or remove it\n"
               "E 14 value: around does not go with the pulse of Body.level: a pulse returns to the value the variable has when the "
               "pulse begins (for another value after it, write a set in the next step); remove it\n");
    // A wave ends where it says: the step after it may not be earlier.
    check_text(wave_findings("  - wave: { Body.level: { pulse: { value: 1, width_ms: 100 } } }\n  - at_ms: 99\n    expect: { Body.level: 5 }\n"),
               "E 4 scenario-time: at_ms is 99, before the end of the step before it (100 ms): the steps run in the order of the file, "
               "and time does not go back; reorder the steps or raise at_ms\n");
}

TEST_CASE(scenario_wave_that_its_ecu_cannot_see_gets_a_warning_that_says_what_the_ecu_sees)
{
    // Body steps every 10 ms. A period under two steps; a smooth shape with few steps a period.
    check_text(wave_findings("  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 5 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { triangle: { amplitude: 0.1, frequency_hz: 40 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 80 }, around: 0.5, for_ms: 100 } }\n"),
               "W 3 scenario-wave-undersampled: the sine of Body.ratio has a period of 5 ms, but participant 'Body' takes its inputs once "
               "per step of 10 ms: with fewer than two values per period the ECU sees a slower shape or a constant, not this one. Make "
               "the period 80 ms or more (8 values per period); to run it as it is without this message, add undersampled: ok to the "
               "step\n"
               "N 4 scenario-wave-coarse: the triangle of Body.ratio has a period of 25 ms, and participant 'Body' takes its inputs once "
               "per step of 10 ms: the ECU sees 2.5 values per period, so it can miss the highest and the lowest. A period of 80 ms or "
               "more gives it 8; to run it as it is without this message, add undersampled: ok to the step\n");
    // A period of two steps: a sine and a triangle that begin at their middle are there at every
    // step of the ECU, which so sees a constant. Begun elsewhere they are two values (a note), and
    // a sawtooth is two values wherever it begins. When the scenario's step begins does not matter.
    check_text(wave_findings("  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 20 }, around: 0.5, for_ms: 100 } }\n"
                             "  - at_ms: 203\n"
                             "    wave: { Body.ratio: { triangle: { amplitude: 0.1, frequency_hz: 50, phase_deg: 180 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 20, phase_deg: 90 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { sawtooth: { amplitude: 0.1, period_ms: 20 }, around: 0.5, for_ms: 100 } }\n"),
               "W 3 scenario-wave-undersampled: the sine of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs "
               "once per step of 10 ms: at each of those times the sine is at its middle, so the ECU sees a constant, not the sine. Make "
               "the period 80 ms or more (8 values per period); to run it as it is without this message, add undersampled: ok to the "
               "step\n"
               "W 5 scenario-wave-undersampled: the triangle of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs "
               "once per step of 10 ms: at each of those times the triangle is at its middle, so the ECU sees a constant, not the "
               "triangle. Make the period 80 ms or more (8 values per period); to run it as it is without this message, add "
               "undersampled: ok to the step\n"
               "N 6 scenario-wave-coarse: the sine of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs once per "
               "step of 10 ms: the ECU sees 2 values per period, so it can miss the highest and the lowest. A period of 80 ms or more "
               "gives it 8; to run it as it is without this message, add undersampled: ok to the step\n"
               "N 7 scenario-wave-coarse: the sawtooth of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs once "
               "per step of 10 ms: the ECU sees 2 values per period, so it can miss the highest and the lowest. A period of 80 ms or "
               "more gives it 8; to run it as it is without this message, add undersampled: ok to the step\n");
    // What the check says is what is written: the middle at every step of the ECU.
    {
        World constant;
        CHECK(constant.run(read(kHead + "  - at_ms: 203\n"
                                        "    wave: { Body.level: { sine: { amplitude: 9, period_ms: 20 }, around: 50, for_ms: 40 } }\n"
                                        "    undersampled: ok\n"))
                  .status == ScenarioStatus::Passed);
        check_text(applied(constant, "Body.level"), "50@210 50@220 50@230 50@240 50@250 ");
    }
    // A square whose high time is not a number of steps: the duty the ECU sees.
    check_text(wave_findings("  - wave: { Door.lamp: { square: { period_ms: 30, duty: 25 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 25 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 40, duty: 10 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 40, duty: 25 }, for_ms: 300 },\n"
                             "            Body.lamp: { square: { period_ms: 40, duty: 0 }, for_ms: 300 }, Body.level: { square: { low: 0, high: 1, period_ms: 20, duty: 100 }, for_ms: 300 } }\n"
                             // Begun 5 ms into its period, the 15 ms that are high hold one step of the ECU, not two.
                             "  - wave: { Door.lamp: { square: { period_ms: 40, duty: 37.5, phase_deg: 45 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 15 }, for_ms: 300 } }\n"),
               "W 3 scenario-wave-duty: the square of Door.lamp is high for 7.5 ms of each period of 30 ms (duty 25 %), but participant "
               "'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (33.3 %). Make the high time "
               "(period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 4 scenario-wave-duty: the square of Door.lamp has a period of 25 ms, which is not a multiple of the step of participant "
               "'Door' (10 ms): the ECU sees high and low times that change from period to period. Make the period a multiple of 10 ms; "
               "to run it as it is without this message, add undersampled: ok to the step\n"
               "W 5 scenario-wave-duty: the square of Door.lamp is high for 4 ms of each period of 40 ms (duty 10 %), but participant "
               "'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (25 %). Make the high time "
               "(period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 8 scenario-wave-duty: the square of Door.lamp is high for 15 ms of each period of 40 ms (duty 37.5 %), but participant "
               "'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (25 %). Make the high time "
               "(period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 9 scenario-wave-undersampled: the square of Door.lamp has a period of 15 ms, but participant 'Door' takes its inputs "
               "once per step of 10 ms: with fewer than two values per period the ECU sees a slower shape or a constant, not this one. "
               "Make the period 20 ms or more; to run it as it is without this message, add undersampled: ok to the step\n");
    // What is shorter than a step: a pulse, the hold of a noise, the gap of two points, the
    // interval of a chatter (half of it with a seed); and the top of a chirp.
    check_text(wave_findings("  - wave: { Door.lamp: { pulse: { width_ms: 3 } } }\n"
                             "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 2, seed: 1 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 0], [20, 5], [25, 6], [28, 7], [100, 0]] } }\n"
                             "  - wave: { Door.lamp: { chatter: { to: true, bounces: 3, interval_ms: 2 } } }\n"
                             "  - wave: { Door.lamp: { chatter: { to: true, bounces: 3, interval_ms: 15, seed: 1 } } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 1, to_hz: 200 }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 20, to_hz: 1 }, around: 0.5, for_ms: 1000 } }\n"),
               "W 3 scenario-wave-undersampled: the pulse of Door.lamp is 3 ms wide, shorter than the step of participant 'Door' (10 ms): "
               "this step begins at 0 ms, so the ECU has the pulse's value for one whole step of 10 ms (the one at 0 ms), not for 3 ms. "
               "Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 4 scenario-wave-undersampled: the noise of Body.ratio changes every 2 ms, faster than the step of participant 'Body' "
               "(10 ms): the ECU gets only the value at the time of each of its steps; the values between are never applied. Make "
               "hold_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 5 scenario-wave-undersampled: the points of Body.level are as little as 3 ms apart (at 25 ms and 28 ms), closer "
               "than the step of participant 'Body' (10 ms): the ECU gets only the value at the time of each of its steps, so it does not see every "
               "point. Put the points 10 ms or more apart; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 6 scenario-wave-undersampled: the chatter of Door.lamp changes every 2 ms, faster than the step of participant 'Door' "
               "(10 ms): the ECU gets only the value at the time of each of its steps, so it sees some of the bounces or none. Make "
               "interval_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 7 scenario-wave-undersampled: the chatter of Door.lamp changes as little as 7.5 ms apart (interval_ms 15 with a seed: a "
               "half to one and a half intervals), closer than the step of participant 'Door' (10 ms): the ECU gets only the value at the "
               "time of each of its steps, so it sees some of the bounces or none. Make interval_ms 20 or more; to run it as it is "
               "without this message, add undersampled: ok to the step\n"
               "W 8 scenario-wave-undersampled: the chirp of Body.ratio goes up to 200 Hz (a period of 5 ms), but participant 'Body' takes "
               "its inputs once per step of 10 ms: above 50 Hz (two steps per period) the ECU sees a slower shape, not the sweep. Keep "
               "from_hz and to_hz at 50 or less; to run it as it is without this message, add undersampled: ok to the step\n"
               "N 9 scenario-wave-coarse: the chirp of Body.ratio goes up to 20 Hz (a period of 50 ms), and participant 'Body' takes its "
               "inputs once per step of 10 ms: at that frequency the ECU sees 5 values per period, so it can miss the highest and the "
               "lowest. Up to 12.5 Hz it gets 8 or more; to run it as it is without this message, add undersampled: ok to the step\n");
    // The step of the ECU decides: the same waves for an ECU of 1 ms say nothing.
    check_text(wave_findings("  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 8 }, around: 0.5, for_ms: 100 }, Door.lamp: { pulse: { width_ms: 3 } } }\n",
                             1ms, 1ms),
               "");
    // undersampled: ok on the step: its waves run as they are, without a word; and they do run.
    const std::string quiet = "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 5 }, around: 0.5, for_ms: 20 }, Door.lamp: { square: { period_ms: 30, duty: 25 }, for_ms: 60 } }\n"
                              "    undersampled: ok\n";
    check_text(wave_findings(quiet), "");
    World world;
    CHECK(world.run(read(kHead + quiet)).status == ScenarioStatus::Passed);
    // A warning does not keep the scenario from running: it is read.
    CHECK(read_scenario(kHead + "  - wave: { Door.lamp: { pulse: { width_ms: 3 } } }\n", catalog()).scenario.has_value());
    // A participant whose step is not known: said, not guessed.
    ScenarioCatalog unknown = catalog();
    unknown[0].step = SimDuration{};
    check_text(lines(read_scenario(kHead + "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 5 }, around: 0.5, for_ms: 20 } }\n", unknown).findings),
               "N 3 scenario-wave-not-checked: whether participant 'Body' can see the sine of Body.ratio as it is written is not "
               "checked: the participant's step is not known\n");
}

TEST_CASE(scenario_times_are_the_same_whether_a_wave_is_skipped_or_not)
{
    // What a wave takes is in its text (for_ms, a pulse's width, the last point, the bounces): a
    // configuration without the variable gives the steps after it the same times.
    const std::string text = kHead + "  - set: { Body.level: 0 }\n"
                                     "  - wave: { Body.boost: { sine: { amplitude: 1, period_ms: 100 }, around: 5, for_ms: 100 }, Body.ratio: { pulse: { value: 1, width_ms: 40 } } }\n"
                                     "  - wave: { Body.boost: { pulse: { value: 1, width_ms: 30 } } }\n"
                                     "  - wave: { Body.boost: { points: [[0, 1], [20, 2], [70, 3]] } }\n"
                                     "  - wave: { Body.boost: { chatter: { to: 1, bounces: 3, interval_ms: 10 } } }\n"
                                     "  - set: { Body.lamp: true }\n";
    ScenarioCatalog with = catalog();
    auto& boost = with[0].variables.emplace_back();
    boost.name = "boost";
    boost.type = control::ValueType::Integer;
    boost.causality = control::Causality::Input;
    const ScenarioRead there = read_scenario(text, with);
    const ScenarioRead absent = read_scenario(text, catalog_without_boost());
    CHECK(there.scenario.has_value() && absent.scenario.has_value());
    CHECK_EQ(absent.skipped, std::size_t{4});
    CHECK_EQ(absent.scenario->steps[1].skipped_ms, std::int64_t{100});
    CHECK_EQ(absent.scenario->steps[2].skipped_ms, std::int64_t{30});
    CHECK_EQ(absent.scenario->steps[3].skipped_ms, std::int64_t{70});
    CHECK_EQ(absent.scenario->steps[4].skipped_ms, std::int64_t{60});
    check_text(absent.scenario->steps[1].text + "\n", "wave Body.boost; Body.ratio pulse 1 width 40 ms\n");
    World a;
    const ScenarioRun skipped = a.run(*absent.scenario);
    CHECK(skipped.status == ScenarioStatus::Passed);
    check_text(starts_of(skipped), "3:0 8:260 ");
    CHECK(skipped.steps[4].start == at(200ms) && skipped.steps[4].end == at(260ms) && !skipped.steps[4].at);
    // Nothing of a skipped step is written, also not the wave of the variable that is there.
    check_text(applied(a, "Body.ratio"), "");
}

// --- points from a CSV file ---


TEST_CASE(scenario_points_from_a_file_write_what_the_same_points_in_the_text_write)
{
    // The same points in the text and in two files: a byte order mark, CRLF, a blank line, a name
    // in quotes, spaces around a cell, hexadecimal, an enum by name and by value, a bool as 0, the
    // times in seconds in a column that is not the first, one file for two variables.
    const std::string first = "  - set: { Body.ratio: 0, Door.lamp: false }\n";
    const std::string text =
        kHead + first +
        "  - wave: { Body.level: { points: [[0, 0], [50, 25.5], [80, 0x3C], [400, 20]] }, Door.mode: { points: [[0, auto], [50, off], [80, 1], [400, on]] },\n"
        "            Door.lamp: { points: [[10, true], [30, false]] },\n"
        "            Body.ratio: { points: [[10, 0.25], [30, 1]] } }\n"
        "  - expect: { Body.level: 20, Door.mode: on, Door.lamp: false, Body.ratio: 1 }\n";
    const std::string fromFiles =
        kHead + first +
        "  - wave: { Body.level: { points: { file: traces/drive.csv, value: level } }, Door.mode: { points: { file: traces/drive.csv, value: mode, time: time_ms } },\n"
        "            Door.lamp: { points: { file: lamp.csv, value: lamp, time: t, time_unit: s } },\n"
        "            Body.ratio: { points: { file: lamp.csv, value: ratio, time: t, time_unit: s } } }\n"
        "  - expect: { Body.level: 20, Door.mode: on, Door.lamp: false, Body.ratio: 1 }\n";
    const auto reads = std::make_shared<std::map<std::string, int>>();
    const ScenarioContext context = with_files({{"traces/drive.csv", "\xEF\xBB\xBF\"time_ms\", level ,mode\r\n0,0,auto\r\n\r\n50,25.5,off\r\n"
                                                                     "80,0x3C,1\r\n400,20,\"on\"\r\n"},
                                                {"lamp.csv", "lamp,t,ratio\ntrue,0.01,0.25\n0,0.03,1\n"}},
                                               reads);
    const ScenarioRead read = read_scenario(fromFiles, catalog(), context);
    check_text(lines(read.findings), "");
    CHECK(read.scenario.has_value());
    check_text(read.scenario->steps[1].text + "\n",
               "wave Body.level points from traces/drive.csv column level (4, linear) over 400 ms; Door.mode points from "
               "traces/drive.csv column mode (4, hold) over 400 ms; Door.lamp points from lamp.csv column lamp (2, hold) over 30 ms; "
               "Body.ratio points from lamp.csv column ratio (2, linear) over 30 ms\n");
    // Each file is read once.
    CHECK(*reads == (std::map<std::string, int>{{"lamp.csv", 1}, {"traces/drive.csv", 1}}));
    World inText;
    World inFiles;
    const ScenarioRun a = inText.run(read_scenario(text, catalog()).scenario.value());
    const ScenarioRun b = inFiles.run(*read.scenario);
    CHECK(a.status == ScenarioStatus::Passed && b.status == ScenarioStatus::Passed);
    check_text(text_of(b), text_of(a));
    for (const char* variable : {"Body.level", "Door.mode", "Door.lamp", "Body.ratio"})
    {
        check_text(applied(inFiles, variable), applied(inText, variable));
    }
    // 25.5 at 50 ms, 60 at 80 ms, then 1.25 less every 10 ms to 20 at 400 ms (rounded for the integer).
    check_text(applied(inFiles, "Body.level"), "0@0 5@10 10@20 15@30 20@40 26@50 37@60 49@70 60@80 59@90 58@100 56@110 55@120 "
                                               "54@130 53@140 51@150 50@160 49@170 48@180 46@190 45@200 44@210 43@220 41@230 40@240 "
                                               "39@250 38@260 36@270 35@280 34@290 33@300 31@310 30@320 29@330 28@340 26@350 25@360 "
                                               "24@370 23@380 21@390 20@400 ");
}

TEST_CASE(scenario_points_file_errors_say_the_file_the_line_and_the_column)
{
    std::string big = "time,level\n";
    for (int i = 0; i <= 100'000; ++i)
    {
        big += std::to_string(i) + ",1\n";
    }
    const ScenarioContext context =
        with_files({{"ok.csv", "time,level\n0,1\n50,2\n"},
                    {"twice.csv", "time,level,level\n0,1,2\n10,1,2\n"},
                    {"cells.csv", "time,level\n0,1\n10,this-is-not-a-number-at-all\n"},
                    {"times.csv", "time,level\n0,1\n12.5,2\n"},
                    {"seconds.csv", "time,level\n0,1\nsoon,2\n"},
                    {"negative.csv", "time,level\n-5,1\n10,2\n"},
                    {"order.csv", "time,level\n0,1\n50,2\n50,3\n"},
                    {"range.csv", "time,level\n0,1\n10,500\n"},
                    {"empty.csv", "\xEF\xBB\xBF\r\n  \r\n"},
                    {"header.csv", "time,level\r\n"},
                    {"ragged.csv", "time,level\n0,1\n10\n"},
                    {"quote.csv", "time,level\n0,\"1\n"},
                    {"utf16.csv", std::string("\xFF\xFEt\0", 4)},
                    {"door.csv", "time,lamp,mode\n0,yes,open\n10,1,1\n"},
                    {"big.csv", big}});
    const auto findings = [&](const std::string& steps, const ScenarioContext& c) { return lines(read_scenario(kHead + steps, catalog(), c).findings); };
    // The path: from the scenario's directory, below it, with '/'.
    check_text(findings("  - wave: { Body.level: { points: { file: /data/ok.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: 'C:/data/ok.csv', value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: traces/../../ok.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: 'traces\\ok.csv', value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: none.csv, value: level } } }\n",
                        context),
               "E 3 scenario-points-file: the points of Body.level: the file '/data/ok.csv' is an absolute path: give the path from the "
               "scenario file's directory (traces/drive.csv); a scenario reads only files in its own directory and below it\n"
               "E 4 scenario-points-file: the points of Body.level: the file 'C:/data/ok.csv' is an absolute path: give the path from the "
               "scenario file's directory (traces/drive.csv); a scenario reads only files in its own directory and below it\n"
               "E 5 scenario-points-file: the points of Body.level: the file 'traces/../../ok.csv' goes up a directory (..): a scenario "
               "reads only files in its own directory and below it; put the file there\n"
               "E 6 scenario-points-file: the points of Body.level: the file 'traces\\ok.csv' has '\\': write '/' between directories "
               "(traces/drive.csv), which every OS reads\n"
               "E 7 scenario-points-file: the points of Body.level: none.csv does not exist\n");
    // The keys.
    check_text(findings("  - wave: { Body.level: { points: { file: ok.csv } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: level, unit: s } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: level, time_unit: min } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: [level] } } }\n"
                        "  - wave: { Body.level: { points: 5 } }\n",
                        context),
               "E 3 missing-key: the points of Body.level from a file need value: the name of the column of the values (points: { file: "
               "traces/drive.csv, value: throttle })\n"
               "E 4 unknown-key: the points of Body.level: unknown key 'unit': remove it, or use one of the keys here (file, value, time, "
               "time_unit)\n"
               "E 5 value: time_unit of the points of Body.level must be ms or s, not 'min'\n"
               "E 6 value: value of the points of Body.level must be the name of the column of the values, not a list\n"
               "E 7 value: points of Body.level must be a list of [<milliseconds since the step began>, <value>] ([[0, 0], [100, 50]]), "
               "or a CSV file and its column (points: { file: traces/drive.csv, value: throttle }), not '5'\n");
    // The columns.
    check_text(findings("  - wave: { Body.level: { points: { file: ok.csv, value: levl } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: level, time: when } } }\n"
                        "  - wave: { Body.level: { points: { file: twice.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: time } } }\n",
                        context),
               "E 3 scenario-points-file: the points of Body.level: ok.csv has no column 'levl' on its first line (line 1): did you mean "
               "'level'?; its columns are 'time', 'level'\n"
               "E 4 scenario-points-file: the points of Body.level: ok.csv has no column 'when' on its first line (line 1); its columns "
               "are 'time', 'level'\n"
               "E 5 scenario-points-file: the points of Body.level: twice.csv has 2 columns named 'level' (columns 2 and 3): give its "
               "columns different names on its first line\n"
               "E 6 scenario-points-file: the points of Body.level: value names the column of the times ('time', the first column): name "
               "the column of the values, or the column of the times with time\n");
    // The cells: the first wrong one, cut short.
    check_text(findings("  - wave: { Body.level: { points: { file: cells.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: times.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: seconds.csv, value: level, time_unit: s } } }\n"
                        "  - wave: { Body.level: { points: { file: negative.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: order.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: range.csv, value: level } } }\n"
                        "  - wave: { Door.lamp: { points: { file: door.csv, value: lamp } } }\n"
                        "  - wave: { Door.mode: { points: { file: door.csv, value: mode } } }\n",
                        context),
               "E 3 scenario-value: the value on line 3 of cells.csv (column 'level') for Body.level (integer) must be a number, not "
               "'this-is-not-a-number...'\n"
               "E 4 value: the time on line 3 of times.csv (column 'time') is 12.5 ms: the times of points are whole milliseconds; round "
               "them in the file\n"
               "E 5 value: the time on line 3 of seconds.csv (column 'time') must be a number of seconds since the step began, not 'soon'\n"
               "E 6 value: the time on line 2 of negative.csv (column 'time') is -5 ms, but it must be from 0 to 1000000000000 "
               "(milliseconds of virtual time since the step began)\n"
               "E 7 value: the times of the points of Body.level must go up: line 4 of order.csv is at 50 ms, not after line 3 (50 ms); "
               "for a value that jumps, write interpolate: hold beside the points (each value is then kept until the next point)\n"
               "E 8 scenario-value: the value on line 3 of range.csv (column 'level') for Body.level (integer) is 500, outside its range "
               "(from 0 to 100)\n"
               "E 9 scenario-value: the value on line 2 of door.csv (column 'lamp') for Door.lamp (bool) must be true, false, 1 or 0, "
               "not 'yes'\n"
               "E 10 scenario-value: the value on line 2 of door.csv (column 'mode') for Door.mode (enum) must be one of its items, by "
               "name or by value, not 'open': the items are off (0), on (1), auto (2)\n");
    // The file as a whole.
    check_text(findings("  - wave: { Body.level: { points: { file: empty.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: header.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: ragged.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: quote.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: utf16.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: big.csv, value: level } } }\n",
                        context),
               "E 3 scenario-points-file: the points of Body.level: empty.csv is empty: its first line names the columns (time,throttle), "
               "and each line below it gives a time and the values\n"
               "E 4 scenario-points-file: the points of Body.level: header.csv has no lines of values below the names of its columns "
               "(line 1): give one line for each point (0,50)\n"
               "E 5 scenario-points-file: the points of Body.level: ragged.csv has 1 cell on line 3, and its first line names 2 columns: "
               "give every line one cell for each column\n"
               "E 6 scenario-points-file: the points of Body.level: quote.csv has a cell in quotes on line 2 whose closing quote is not on "
               "the line: end a cell in quotes with a quote before the next ',' (write \"\" for a quote in it)\n"
               "E 7 scenario-points-file: the points of Body.level: utf16.csv is not UTF-8 text (it looks like UTF-16, or has bytes that "
               "are not UTF-8): save it as CSV in UTF-8\n"
               "E 8 scenario-points-file: the points of Body.level: big.csv has more than 100000 lines of values (one point each): keep "
               "fewer points, or split it into steps of their own\n");
    // Without a way to read files (read_scenario() without ScenarioContext::read_file).
    check_text(findings("  - wave: { Body.level: { points: { file: ok.csv, value: level } } }\n", ScenarioContext{}),
               "E 3 scenario-points-file: the points of Body.level: ok.csv is not read here: points from a file are read with the "
               "scenario file (check --scenario, run --scenario)\n");
    // A parameter may name the file and the column.
    check_text(findings("  - wave: { Body.level: { points: { file: \"${trace}\", value: \"${column}\" } } }\n"
                        "params: { trace: ok.csv, column: level }\n",
                        context),
               "");
}

TEST_CASE(scenario_points_file_of_a_skipped_wave_gives_its_length)
{
    // A skipped step lasts until the time on the last line of its file, as it would if it ran.
    const std::string text = kHead + "  - wave: { Body.boost: { points: { file: boost.csv, value: b, time: t } } }\n"
                                     "  - wave: { Body.boost: { points: { file: boost.csv, value: b, time: s, time_unit: s } } }\n"
                                     "  - wave: { Body.boost: { points: { file: none.csv, value: b } } }\n";
    const ScenarioRead absent =
        read_scenario(text, catalog_without_boost(), with_files({{"boost.csv", "b,t,s\n1,0,0\n2,70,0.25\n"}}));
    CHECK(absent.scenario.has_value());
    CHECK_EQ(absent.skipped, std::size_t{3});
    CHECK_EQ(absent.scenario->steps[0].skipped_ms, std::int64_t{70});
    CHECK_EQ(absent.scenario->steps[1].skipped_ms, std::int64_t{250});
    CHECK_EQ(absent.scenario->steps[2].skipped_ms, std::int64_t{0});
}

// --- an expect beside a ramp or a wave ---

TEST_CASE(scenario_expect_beside_a_ramp_is_read_with_the_ramp)
{
    const Scenario s = read(kHead + "  - set: { Body.level: 0 }\n"
                                    "  - ramp: { Body.level: { to: 10, over_ms: 100 } }\n"
                                    "    expect: { Body.level: { le: 10 }, Body.lamp: false }\n"
                                    "  - wave: { Door.ratio: { sine: { amplitude: 0.1, period_ms: 100 }, around: 0.5, for_ms: 200 } }\n"
                                    "    expect: { Door.ratio: { ge: 0.3 } }\n");
    CHECK_EQ(s.steps.size(), std::size_t{3});
    CHECK(s.steps[1].kind == StepKind::Ramp && s.steps[1].conditions.size() == 2 && s.steps[1].motions.size() == 1);
    CHECK(s.steps[2].kind == StepKind::Wave && s.steps[2].conditions.size() == 1);
    check_text(s.steps[1].text + "\n" + s.steps[2].text + "\n",
               "ramp Body.level to 10 over 100 ms, expecting Body.level le 10; Body.lamp eq false\n"
               "wave Door.ratio sine amplitude 0.1 period 100 ms around 0.5 for 200 ms, expecting Door.ratio ge 0.3\n");
    // It must be true all through the ramp: no within_ms or hold_for_ms; and only beside a ramp
    // or a wave.
    check_text(errors_only(read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                                 "    expect: { Body.level: { le: 10 } }\n"
                                                 "    within_ms: 50\n"
                                                 "  - wave: { Body.level: { pulse: { value: 1, width_ms: 50 } } }\n"
                                                 "    expect: { Body.level: { le: 10 } }\n"
                                                 "    hold_for_ms: 50\n"
                                                 "  - set: { Body.level: 1 }\n"
                                                 "    expect: { Body.level: 1 }\n",
                                         catalog())
                               .findings),
               "E 5 value: within_ms does not go with an expect beside a ramp: that expect must be true all through the ramp, "
               "from its time to its end; remove within_ms, or make the expect a step of its own after the ramp\n"
               "E 8 value: hold_for_ms does not go with an expect beside a wave: that expect must be true all through the "
               "wave, from its time to its end; remove hold_for_ms, or make the expect a step of its own after the wave\n"
               "E 9 value: the step has set, expect: a step does one thing; make a step of each\n");
}

namespace {

/// Runs `s` with every policy() and both orders of the ECUs, and returns its one result (text_of):
/// a test fails if two of them differ.
std::string one_result(const Scenario& s, const std::function<void(World&)>& setup = {})
{
    std::optional<std::string> first;
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            if (setup)
            {
                setup(world);
            }
            world.policy = policy;
            world.reverse = reverse;
            const std::string got = text_of(world.run(s));
            if (!first)
            {
                first = got;
            }
            check_text(got, *first, (name + (reverse ? ", the ECUs in reverse order" : "")).c_str());
        }
    }
    return *first;
}

} // namespace

TEST_CASE(scenario_expect_beside_a_ramp_holds_from_its_time_to_its_end_and_not_after)
{
    // Body.level goes 0, 1, ..., 9 at 100, 110, ..., 190 and 10 at 200, the end: each sample of
    // Body (every 10 ms) has the value written for its time. The set at 210 is after the end.
    const auto run = [](const std::string& expect) {
        return one_result(read(kHead + "  - set: { Body.level: 0 }\n"
                                       "  - at_ms: 100\n"
                                       "    ramp: { Body.level: { to: 10, over_ms: 100 } }\n"
                                       "    expect: { Body.level: " + expect + " }\n"
                                       "  - after_ms: 10\n"
                                       "    set: { Body.level: 50 }\n"));
    };
    // True at T, at every sample in (T, E] and not looked at after E.
    check_text(run("{ le: 10 }"), "passed over=220\n"
                                  "3 passed start=0 at=0 end=0\n"
                                  "4 passed start=100 at=200 end=200\n"
                                  "7 passed start=210 at=210 end=210\n");
    // The sample at T is looked at (the step at T applied the first write).
    check_text(run("{ ge: 1 }"), "failed over=110\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 failed start=100 at=100 end=200 scenario-failed: not true during the ramp (from 100 ms to 200 "
                                 "ms): at 100 ms Body.level ge 1, but it was 0 (its sample of 100 ms)\n"
                                 "7 not_run start=- at=- end=-\n");
    // And the sample at E, the last one of the ramp.
    check_text(run("{ le: 9 }"), "failed over=210\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 failed start=100 at=200 end=200 scenario-failed: not true during the ramp (from 100 ms to 200 "
                                 "ms): at 200 ms Body.level le 9, but it was 10 (its sample of 200 ms)\n"
                                 "7 unfinished start=210 at=- end=-\n");
}

TEST_CASE(scenario_expect_beside_a_ramp_between_the_steps_of_its_ecu_looks_from_the_first_write_to_the_end_value)
{
    // Door steps every 30 ms (0, 30, 60, ...), the controller every 10 ms. The ramp from 40 ms
    // writes 0 for 40 (applied at 60), 3 for 70 (at 90) and its end, 5 for 100, at 120. The
    // expect judges the samples of 60, 90 and 120: not that of 30 ms (50, before the ramp), and
    // the end value although E (100 ms) is between two steps.
    const auto run = [](const std::string& expect) {
        return one_result(read(kHead + "  - set: { Door.level: 50 }\n"
                                       "  - at_ms: 40\n"
                                       "    ramp: { Door.level: { from: 0, to: 5, over_ms: 60 } }\n"
                                       "    expect: { Door.level: " + expect + " }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; });
    };
    check_text(run("{ le: 5 }"), "passed over=130\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 passed start=40 at=100 end=100\n");
    check_text(run("{ le: 4 }"), "failed over=130\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 failed start=40 at=120 end=100 scenario-failed: not true during the ramp (from 40 ms to 120 "
                                 "ms, where its end (100 ms) takes effect): at 120 ms Door.level le 4, but it was 5 (its sample "
                                 "of 120 ms)\n");
    // An expect of another ECU is judged up to the same time: Body's samples of 40 to 120 ms.
    check_text(one_result(read(kHead + "  - set: { Door.level: 50 }\n"
                                       "  - at_ms: 40\n"
                                       "    ramp: { Door.level: { from: 0, to: 5, over_ms: 60 } }\n"
                                       "    expect: { Body.body.speed: { lt: 0 } }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; }),
               "failed over=50\n"
               "3 passed start=0 at=0 end=0\n"
               "4 failed start=40 at=40 end=100 scenario-failed: not true during the ramp (from 40 ms to 120 ms, where its end "
               "(100 ms) takes effect): at 40 ms Body.body.speed lt 0, but it was 0 (its sample of 40 ms)\n");
    // A variable whose ECU sends no sample in that time was not looked at: the step fails. Door
    // samples at 60 and 90; the ramp of Body from 61 ms to 71 ms looks until 80 ms.
    check_text(one_result(read(kHead + "  - at_ms: 61\n"
                                       "    ramp: { Body.level: { from: 0, to: 5, over_ms: 10 } }\n"
                                       "    expect: { Door.level: 0 }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; }),
               "failed over=90\n"
               "3 failed start=61 at=80 end=71 scenario-failed: not checked during the ramp (from 61 ms to 80 ms, where its end "
               "(71 ms) takes effect): no sample of Door.level in that time (its ECU sends one after each of its steps; with samples: as_sent, every sample_period); make the "
               "ramp longer, or check in an expect step with within_ms after it\n");
}

TEST_CASE(scenario_expect_beside_a_motion_decides_the_step_with_its_writes)
{
    // The step passes when both its writes and its expect have: a failing expect after it is
    // still looked at, and an expect that fails at the end fails a ramp that is the last step.
    check_text(one_result(read(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                       "    expect: { Body.level: { le: 10 } }\n"
                                       "  - expect: { Body.level: 0 }\n")),
               "failed over=110\n"
               "3 passed start=0 at=100 end=100\n"
               "5 failed start=100 at=100 end=100 scenario-failed: not true at 100 ms: Body.level eq 0, but it was 10 (its "
               "sample of 100 ms)\n");
    check_text(one_result(read(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                       "    expect: { Body.level: { lt: 10 } }\n")),
               "failed over=110\n"
               "3 failed start=0 at=100 end=100 scenario-failed: not true during the ramp (from 0 ms to 100 ms): at 100 ms "
               "Body.level lt 10, but it was 10 (its sample of 100 ms)\n");
    // The writes decide it too: Door steps every 30 ms, so the end of its ramp (100 ms) takes
    // effect at 120 ms and is judged at 130 ms, after the expect on Body closed (at 120 ms, seen
    // at 130 ms). Door rejects that last write (13): the step fails, though its expect held.
    check_text(one_result(read(kHead + "  - ramp: { Door.level: { from: 0, to: 13, over_ms: 100 } }\n"
                                       "    expect: { Body.level: 0 }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; }),
               "failed over=130\n"
               "3 failed start=0 at=100 end=100 scenario-write: the write for 100 ms was not applied by participant 'Door': "
               "the ECU rejected it (level: a setter threw: 13 is not a level)\n");
}

TEST_CASE(scenario_expect_beside_a_wave_fails_with_what_the_launcher_learns_first)
{
    // The wave around 90 writes 102 for 110 ms, which the agent refuses at once (out of range);
    // the expect is already false at 100 ms. With a tick of 10 ms the write for 110 ms goes out
    // (and is refused) at 100 ms, before the sample of 100 ms is looked at (at 110 ms); with a
    // tick of 5 ms that sample is looked at first, at 105 ms.
    const auto run = [](SimDuration tick) {
        return one_result(read(kHead + "  - set: { Body.level: 90 }\n"
                                       "  - at_ms: 100\n"
                                       "    wave: { Body.level: { sine: { amplitude: 20, period_ms: 100 }, for_ms: 100 } }\n"
                                       "    expect: { Body.level: { le: 80 } }\n"),
                          [tick](World& w) { w.tick = tick; });
    };
    check_text(run(10ms), "failed over=100\n"
                          "3 passed start=0 at=0 end=0\n"
                          "4 failed start=100 at=110 end=200 scenario-write: the write for 110 ms was not taken by participant "
                          "'Body': the ECU's agent refused it (level: out of range)\n");
    check_text(run(5ms), "failed over=105\n"
                         "3 passed start=0 at=0 end=0\n"
                         "4 failed start=100 at=100 end=200 scenario-failed: not true during the wave (from 100 ms to 200 ms): at "
                         "100 ms Body.level le 80, but it was 90 (its sample of 100 ms)\n");
    // A variable of the expect without a link fails the step as a read, not as a write.
    World alone;
    alone.ecus.pop_back();
    check_text(text_of(alone.run(read(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                              "    expect: { Door.lamp: false }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=- scenario-failed: Door.lamp cannot be read: participant 'Door' has no control "
               "link in this run: its agent did not connect before the simulation started (the run's control-link findings "
               "and its lines say why)\n");
}

TEST_CASE(scenario_expect_beside_a_ramp_that_uses_an_absent_variable_is_skipped_and_the_ramp_runs)
{
    ScenarioContext context;
    context.variants = {"small", "large"};
    context.variant = "small";
    const ScenarioRead r = read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 2, over_ms: 20 } }\n"
                                                 "    expect: { Body.boost: 0, Body.level: { le: 2 } }\n"
                                                 "  - ramp: { Body.boost: { to: 3, over_ms: 50 } }\n"
                                                 "    expect: { Body.level: 2 }\n"
                                                 "  - set: { Body.lamp: true }\n",
                                         catalog_without_boost(), context);
    check_text(lines(r.findings),
               "N 3 scenario-step-skipped: the ramp runs; its expect is not checked (ramp Body.level from 0 to 2 over 20 ms, "
               "expect not checked: Body.boost; Body.level le 2): Body.boost is absent: participant 'Body' does not have it in "
               "the variant small (absent: true in its variables)\n"
               "N 5 scenario-step-skipped: the step is skipped (ramp Body.boost, expecting Body.level eq 2): Body.boost is "
               "absent: participant 'Body' does not have it in the variant small (absent: true in its variables); the other "
               "steps run, also those that would need what this one does\n"
               "W 0 scenario-nothing-checked: the scenario checks nothing in the variant small: all its 2 checks (expect, "
               "wait_until, or an expect beside a ramp or a wave; lines 3, 5) are skipped or not checked, so it passes whatever "
               "the ECUs do (the notes say why for each). If the scenario is not for this variant, list the variants it is for "
               "in the scenario (only: [large]); if it is, map what the steps look at to what the variant has (variables in "
               "the variant, docs/simulation-config.md)\n");
    CHECK(r.scenario.has_value());
    CHECK_EQ(r.skipped, std::size_t{1});
    CHECK_EQ(r.unchecked_expects, std::size_t{1});
    CHECK(r.scenario->steps[0].conditions.empty() && r.scenario->steps[0].skipped.empty());
    check_text(r.scenario->steps[0].expect_skipped,
               "Body.boost is absent: participant 'Body' does not have it in the variant small (absent: true in its variables)");
    // The first ramp writes, unchecked; the second is skipped and takes its 50 ms.
    World world;
    const ScenarioRun run = world.run(*r.scenario);
    check_text(text_of(run), "passed over=80\n"
                             "3 passed start=0 at=20 end=20\n"
                             "5 skipped start=20 at=- end=70 scenario-skipped: Body.boost is absent: participant 'Body' does "
                             "not have it in the variant small (absent: true in its variables)\n"
                             "7 passed start=70 at=70 end=70\n");
    check_text(applied(world, "Body.level"), "0@0 1@10 2@20 ");
    CHECK(run.steps[0].expect_skipped == r.scenario->steps[0].expect_skipped);
    CHECK(run.steps[1].expect_skipped.empty());
    CHECK_EQ(run.unchecked_expects(), std::size_t{1});
    CHECK_EQ(run.skipped(), std::size_t{1});
    // When the expect beside is the only check and is not checked, the warning says so.
    const ScenarioRead only = read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 2, over_ms: 20 } }\n"
                                                    "    expect: { Body.boost: 0 }\n",
                                            catalog_without_boost());
    CHECK(lines(only.findings).find("W 0 scenario-nothing-checked: the scenario checks nothing in this configuration: its only "
                                    "check (an expect beside a ramp or a wave, line 3) is not checked, so it passes whatever the "
                                    "ECUs do (the notes say why for each). ") != std::string::npos);
    // A scenario with another check left gets no warning.
    const ScenarioRead checked = read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 2, over_ms: 20 } }\n"
                                                       "    expect: { Body.boost: 0 }\n"
                                                       "  - expect: { Body.level: 2 }\n",
                                               catalog_without_boost(), context);
    CHECK(std::none_of(checked.findings.begin(), checked.findings.end(), [](const Finding& f) { return f.severity != Severity::Note; }));
}

// --- the timeline of a run (timeline.hpp) ---

TEST_CASE(scenario_motion_starts_from_where_the_one_before_it_left_the_variable)
{
    // An integer. The lag ends at 77.7, which the ECU gets as 78; the ramp after it starts from
    // that 78, not from 77.7: halfway to 77 it writes 77.5, which is 78 (from 77.7 it would be 77).
    const Scenario rounded = read(kHead + "  - wave: { Body.level: { lag: { from: 0, to: 100, tau_ms: 20 }, for_ms: 30 } }\n"
                                          "  - ramp: { Body.level: { to: 77, over_ms: 20 } }\n");
    World world;
    CHECK(world.run(rounded).status == ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "0@0 39@10 63@20 78@30 78@30 78@40 77@50 ");
    // A ramp that ends between two steps of its ECU (25 ms, steps of 10 ms): the one after it
    // starts from its end value (80), not from the last value on a step (68).
    const Scenario between = read(kHead + "  - set: { Body.level: 20 }\n"
                                          "  - ramp: { Body.level: { to: 80, over_ms: 25 } }\n"
                                          "  - ramp: { Body.level: { to: 0, over_ms: 20 } }\n");
    World off;
    CHECK(off.run(between).status == ScenarioStatus::Passed);
    check_text(applied(off, "Body.level"), "20@0 20@0 44@10 68@20 80@30 80@30 40@40 0@50 ");
}

TEST_CASE(scenario_first_motion_without_a_base_takes_the_sample_the_controllers_step_has_let_it_see)
{
    // A ramp at 25 ms that does not say where it starts, of a variable the scenario has not
    // written: its base is a sample. With a controller that steps every 10 or 20 ms the samples
    // of 0 and 10 ms were looked at before the writes for 25 ms went out. With one that steps
    // every 30 ms those writes go out at 0 ms, before any sample: the same scenario fails. (The
    // message says the way out: give it from.)
    const Scenario s = read(kHead + "  - at_ms: 25\n"
                                    "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n");
    for (const SimDuration tick : {SimDuration{10ms}, SimDuration{20ms}})
    {
        World world;
        world.tick = tick;
        CHECK(world.run(s).status == ScenarioStatus::Passed);
        check_text(applied(world, "Body.level"), "0@30 40@40 80@50 ");
    }
    World coarse;
    coarse.tick = 30ms;
    check_text(text_of(coarse.run(s)),
               "failed over=0\n"
               "3 failed start=25 at=25 end=45 scenario-write: the ramp of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 25 ms and the scenario has not written it; give the ramp from: <value>\n");
    // Where it turns: the writes for the times up to the controller's first step (30 ms, that
    // time too) go out at 0 ms. A ramp at 30 ms fails as well; one at 40 ms goes out at 30 ms, with
    // the samples of 0 to 20 ms looked at.
    World edge;
    edge.tick = 30ms;
    check_text(text_of(edge.run(read(kHead + "  - at_ms: 30\n"
                                             "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"))),
               "failed over=0\n"
               "3 failed start=30 at=30 end=50 scenario-write: the ramp of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 30 ms and the scenario has not written it; give the ramp from: <value>\n");
    World after;
    after.tick = 30ms;
    CHECK(after.run(read(kHead + "  - at_ms: 40\n"
                                 "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(after, "Body.level"), "0@40 40@50 80@60 ");
}

// --- dither: a shape on top of an input ---


TEST_CASE(scenario_dither_is_read_from_the_inputs_and_from_its_step)
{
    const ScenarioRead r = read_scenario(kDitherHead + "inputs:\n"
                                                       "  Body.ratio: { dither: { noise: { amplitude: 0.05, hold_ms: 10, seed: 7 } } }\n"
                                                       "  Door.level:\n"
                                                       "    dither: { triangle: { amplitude: 2, period_ms: 80 } }\n"
                                                       "steps:\n"
                                                       "  - set: { Body.ratio: 0.5, Door.level: 5 }\n"
                                                       "  - after_ms: 100\n"
                                                       "    dither: { Body.ratio: none, Door.level: { square: { amplitude: 1, frequency_hz: 25, duty: 25 } } }\n",
                                         catalog());
    check_text(lines(r.findings), "");
    if (!r.scenario)
    {
        throw unit::Failure("no scenario");
    }
    const Scenario& s = *r.scenario;
    // The inputs come first among the variables, wherever a step uses them.
    CHECK(s.inputs.size() == 2 && s.variables.size() == 2);
    if (s.inputs.size() != 2 || s.steps.size() != 2 || s.steps[1].overlays.size() != 2)
    {
        throw unit::Failure("the scenario was not read as written");
    }
    CHECK(s.variables[0].text == "Body.ratio" && s.variables[1].text == "Door.level");
    CHECK(s.inputs[0].variable == 0 && s.inputs[0].line == 3 && s.inputs[0].shape != nullptr);
    CHECK(s.inputs[0].text == "Body.ratio noise amplitude 0.05 hold 10 ms seed 7");
    CHECK(s.inputs[1].variable == 1 && s.inputs[1].line == 4); // (the entry's line, not the line of its dither)
    CHECK(s.inputs[1].text == "Door.level triangle amplitude 2 period 80 ms");
    CHECK(s.inputs[1].described.type == "triangle" && s.inputs[1].described.values.size() == 2);
    CHECK(s.steps[1].kind == StepKind::Dither && std::string(step_kind_name(s.steps[1].kind)) == "dither");
    check_text(s.steps[1].text, "dither Body.ratio none; Door.level square amplitude 1 frequency 25 Hz duty 25 %");
    CHECK(s.steps[1].overlays[0].shape == nullptr && s.steps[1].overlays[0].described.type.empty());
    CHECK(s.steps[1].overlays[1].shape != nullptr && s.steps[1].overlays[1].described.type == "square");
    // A dither step takes no time: the next step counts from its time.
    const Scenario timed = read(kHead + "  - at_ms: 50\n    dither: { Body.level: none }\n  - after_ms: 10\n    set: { Body.level: 1 }\n");
    World world;
    check_text(text_of(world.run(timed)), "passed over=70\n"
                                          "3 passed start=50 at=50 end=50\n"
                                          "5 passed start=60 at=60 end=60\n");
    // A number of the shape may be a parameter, as in a wave.
    const Scenario withParam = read(kDitherHead + "params: { swing: 3 }\ninputs:\n  Body.level: { dither: { triangle: { amplitude: \"${swing}\", period_ms: 40 } } }\n"
                                                  "steps:\n  - set: { Body.level: 50 }\n");
    CHECK(withParam.inputs.size() == 1 && withParam.inputs[0].text == "Body.level triangle amplitude 3 period 40 ms");
}

TEST_CASE(scenario_dither_adds_to_whatever_the_scenario_puts_on_the_input)
{
    // A triangle of 2 every 40 ms on Body.level: 0, +2, 0, -2 at the steps of Body. It is on the
    // value of the set, on the ramp (which starts from the 50 the set left, not from a dithered
    // value) and on the ramp's end value; none takes it off with one write of the value alone;
    // a dither put later is at the place of its period that the time says (250 ms: the top).
    const Scenario s = read(kDitherHead + "inputs:\n"
                                          "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
                                          "steps:\n"
                                          "  - set: { Body.level: 50 }\n"
                                          "  - at_ms: 100\n"
                                          "    ramp: { Body.level: { to: 80, over_ms: 60 } }\n"
                                          "  - at_ms: 200\n"
                                          "    dither: { Body.level: none }\n"
                                          "  - at_ms: 250\n"
                                          "    dither: { Body.level: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
                                          "  - at_ms: 280\n"
                                          "    expect: { Body.level: 80 }\n");
    for (const auto& [name, policy] : policies())
    {
        for (const SimDuration tick : {5ms, 10ms, 30ms})
        {
            World world;
            world.tick = tick;
            world.policy = policy;
            const ScenarioRun run = world.run(s);
            const std::string where = name + ", tick " + std::to_string(tick.count());
            check_text(written_to(world, "Body.level", 280ms),
                       "50@0>0 52@10>10 50@20>20 48@30>30 50@40>40 52@50>50 50@60>60 48@70>70 50@80>80 52@90>90 "
                       "50@100>100 53@110>110 60@120>120 67@130>130 70@140>140 73@150>150 80@160>160 "
                       "82@170>170 80@180>180 78@190>190 80@200>200 "
                       "82@250>250 80@260>260 78@270>270 80@280>280 ",
                       where.c_str());
            check_text(text_of(run).substr(text_of(run).find('\n') + 1), "5 passed start=0 at=0 end=0\n"
                                                                          "6 passed start=100 at=160 end=160\n"
                                                                          "8 passed start=200 at=200 end=200\n"
                                                                          "10 passed start=250 at=250 end=250\n"
                                                                          "12 passed start=280 at=280 end=280\n",
                       where.c_str());
        }
    }
}

TEST_CASE(scenario_dither_writes_once_per_step_of_its_ecu_where_no_step_writes_and_no_motion_runs)
{
    // The values and the times of the writes do not depend on the controller's step or on when
    // the samples arrive, also in a stretch after a step that waits: the dither is a function
    // of the virtual time. Body.level: a ramp that begins and ends between two steps of Body
    // (107 ms to 193 ms). Body.ratio: set once, with a noise. Door.level has no dither.
    const std::string steps = "steps:\n"
                              "  - set: { Body.level: 20, Body.ratio: 0.5, Door.level: 7 }\n"
                              "  - at_ms: 107\n"
                              "    ramp: { Body.level: { to: 60, over_ms: 86 }, Door.level: { to: 9, over_ms: 86 } }\n"
                              "  - wait_until: { Body.body.speed: { ge: 615 } }\n"
                              "    timeout_ms: 2000\n"
                              "  - expect: { Body.level: { ge: 50 } }\n"
                              "    hold_for_ms: 100\n";
    const Scenario s = read(kDitherHead + "inputs:\n"
                                          "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
                                          "  Body.ratio: { dither: { noise: { amplitude: 0.05, hold_ms: 10, seed: 7 } } }\n" +
                            steps);
    const Scenario plain = read(kDitherHead + steps);
    std::string first;
    std::string firstRatio;
    std::string door;
    for (const auto& [name, policy] : policies())
    {
        for (const SimDuration tick : {5ms, 10ms, 30ms})
        {
            const std::string where = name + ", tick " + std::to_string(tick.count());
            World world;
            world.tick = tick;
            world.policy = policy;
            const ScenarioRun run = world.run(s);
            check_text(scenario_status_name(run.status), "passed", where.c_str());
            // The wait became true at a sample, whose time is the same in every run; the stretch
            // after it is 100 ms of the dither alone.
            const SimDuration waited = *run.steps[2].at - SimTime{};
            check_text(std::to_string(waited.count() / 1000000), "210", where.c_str()); // (the speed follows the level: 620 with the 62 of 210 ms)
            const std::string level = written_to(world, "Body.level", waited + 100ms);
            const std::string ratio = written_to(world, "Body.ratio", waited + 100ms);
            if (first.empty())
            {
                first = level;
                firstRatio = ratio;
            }
            check_text(level, first, where.c_str());
            check_text(ratio, firstRatio, where.c_str());
            // One write per step of the ECU, at the step's own time, but for: the ramp's writes,
            // which keep their own times (107, 117, ...: each is applied by the next step), and
            // its end value at 193 ms, which the step at 200 ms applies before the dither's write
            // of 200 ms.
            check_text(steps_without_one_write(world, "Body.level", 0ms, waited + 100ms, 10ms), "200:2 ", where.c_str());
            check_text(steps_without_one_write(world, "Body.ratio", 0ms, waited + 100ms, 10ms), "", where.c_str());
            // An input without a dither is written as in a scenario without any: the same writes.
            World other;
            other.tick = tick;
            other.policy = policy;
            other.run(plain); // (its wait runs out: without the dither the speed stays below 615)
            check_text(written_to(world, "Door.level", 5s), written_to(other, "Door.level", 5s), where.c_str());
            if (door.empty())
            {
                door = written_to(world, "Door.level", 5s);
            }
            check_text(written_to(world, "Door.level", 5s), door, where.c_str());
            // The ramp writes at the same times with and without the dither.
            const auto times = [](const std::string& writes) {
                std::string out;
                for (std::size_t at = writes.find('@'); at != std::string::npos; at = writes.find('@', at + 1))
                {
                    out += writes.substr(at, writes.find(' ', at) - at) + " ";
                }
                return out;
            };
            const std::string ramp = times(written_to(other, "Body.level", 193ms));
            check_text(ramp, "@0>0 @107>110 @117>120 @127>130 @137>140 @147>150 @157>160 @167>170 @177>180 @187>190 @193>200 ", where.c_str());
            CHECK(times(level).find(" @100>100 " + ramp.substr(ramp.find("@107")) + "@200>200 ") != std::string::npos);
        }
    }
    check_text(first.substr(0, first.find("@200>200")),
               "20@0>0 22@10>10 20@20>20 18@30>30 20@40>40 22@50>50 20@60>60 18@70>70 20@80>80 22@90>90 20@100>100 "
               "19@107>110 24@117>120 31@127>130 35@137>140 37@147>150 43@157>160 49@167>170 53@177>180 56@187>190 59@193>200 60");
    check_text(door, "7@0>0 7@107>110 7@117>120 7@127>130 8@137>140 8@147>150 8@157>160 8@167>170 9@177>180 9@187>190 9@193>200 ");
}

TEST_CASE(scenario_dither_is_added_before_an_integer_is_rounded)
{
    // Body.level goes from 10 to 11 in 100 ms. At 40 ms the ramp is at 10.4, which is written as
    // 10; the triangle of 0.2 is at its top there (a quarter of 160 ms), and 10.6 is written as
    // 11. Added to the rounded 10 it would stay 10.
    const std::string ramp = "steps:\n  - ramp: { Body.level: { from: 10, to: 11, over_ms: 100 } }\n";
    World plain;
    plain.run(read(kDitherHead + ramp));
    check_text(written_to(plain, "Body.level", 100ms), "10@0>0 10@10>10 10@20>20 10@30>30 10@40>40 11@50>50 11@60>60 11@70>70 11@80>80 11@90>90 11@100>100 ");
    World world;
    world.run(read(kDitherHead + "inputs:\n  Body.level: { dither: { triangle: { amplitude: 0.2, period_ms: 160 } } }\n" + ramp));
    check_text(written_to(world, "Body.level", 100ms), "10@0>0 10@10>10 10@20>20 10@30>30 11@40>40 11@50>50 11@60>60 11@70>70 11@80>80 11@90>90 11@100>100 ");
    // A real input gets the sum as it is: the same bits as a wave around the same value from 0.
    World real;
    real.run(read(kDitherHead + "inputs:\n  Body.ratio: { dither: { triangle: { amplitude: 0.07, period_ms: 40, phase_deg: 30 } } }\n"
                                "steps:\n  - set: { Body.ratio: 0.3 }\n  - at_ms: 100\n    expect: { Body.ratio: { ge: 0 } }\n"));
    World wave;
    wave.run(read(kDitherHead + "steps:\n  - wave: { Body.ratio: { triangle: { amplitude: 0.07, period_ms: 40, phase_deg: 30 }, around: 0.3, for_ms: 200 } }\n"));
    check_text(written_to(real, "Body.ratio", 100ms), written_to(wave, "Body.ratio", 100ms));
    CHECK(written_to(real, "Body.ratio", 100ms).find("@100>100") != std::string::npos);
}

TEST_CASE(scenario_dither_waits_for_the_first_value_and_a_dither_step_writes_once)
{
    // A declared dither rides from the first thing the scenario puts on the input: nothing is
    // written before the set at 30 ms.
    {
        World world;
        const ScenarioRun run = world.run(read(kDitherHead + "inputs:\n  Body.level: { dither: { square: { amplitude: 1, period_ms: 20 } } }\n"
                                                             "steps:\n  - at_ms: 30\n    set: { Body.level: 50 }\n"
                                                             "  - at_ms: 60\n    expect: { Body.level: 51 }\n"));
        check_text(written_to(world, "Body.level", 60ms), "49@30>30 51@40>40 49@50>50 51@60>60 ");
        check_text(scenario_status_name(run.status), "passed");
    }
    // A dither step on an input without a value writes nothing and passes at its time, whatever
    // the controller's step is; the dither is there when the set comes. A set and a dither step
    // at one time: both write, each the set's value with the dither that is put at that time
    // (the square is low at 50 ms). none writes the value alone, once.
    const Scenario s = read(kHead + "  - at_ms: 20\n"
                                    "    dither: { Body.level: { square: { amplitude: 1, period_ms: 20 } }, Door.level: none }\n"
                                    "  - at_ms: 30\n"
                                    "    set: { Body.level: 50 }\n"
                                    "  - at_ms: 50\n"
                                    "    set: { Body.level: 70 }\n"
                                    "  - at_ms: 50\n"
                                    "    dither: { Body.level: { square: { amplitude: 5, period_ms: 20 } } }\n"
                                    "  - at_ms: 80\n"
                                    "    dither: { Body.level: none }\n"
                                    "  - at_ms: 120\n"
                                    "    expect: { Body.level: 70 }\n");
    for (const SimDuration tick : {5ms, 10ms, 30ms})
    {
        World world;
        world.tick = tick;
        const ScenarioRun run = world.run(s);
        const std::string where = "tick " + std::to_string(tick.count());
        check_text(text_of(run).substr(text_of(run).find('\n') + 1), "3 passed start=20 at=20 end=20\n"
                                                                      "5 passed start=30 at=30 end=30\n"
                                                                      "7 passed start=50 at=50 end=50\n"
                                                                      "9 passed start=50 at=50 end=50\n"
                                                                      "11 passed start=80 at=80 end=80\n"
                                                                      "13 passed start=120 at=120 end=120\n",
                   where.c_str());
        check_text(written_to(world, "Body.level", 120ms), "49@30>30 51@40>40 65@50>50 65@50>50 75@60>60 65@70>70 70@80>80 ", where.c_str());
        check_text(written_to(world, "Door.level", 120ms), "", where.c_str());
    }
}

TEST_CASE(scenario_dither_write_that_is_refused_says_the_input_only_if_it_is_known_and_its_range_only_if_that_is_the_reason)
{
    const std::string head = kDitherHead + "inputs:\n"
                                           "  Body.level: { dither: { square: { amplitude: 2, period_ms: 100, phase_deg: 180 } } }\n";
    const std::string steps = "steps:\n"
                              "  - set: { Body.level: 99, Body.ratio: 0.5 }\n"
                              "  - at_ms: 100\n"
                              "    expect: { Body.level: 97 }\n";
    const auto failure = [](const ScenarioRun& run) {
        // (Of the step that failed, whichever it is: the first that was not decided.)
        for (const ScenarioStepRun& step : run.steps)
        {
            if (step.status == StepStatus::Failed)
            {
                return step.code + ": " + step.message + "\n";
            }
        }
        return std::string("no step failed\n");
    };
    // The write of 50 ms is 101, beyond the range, but the agent has no room for the command:
    // that is what the message says, not the range (lowering the amplitude would not help).
    {
        World world;
        world.refuse = [](const std::string& peer, SimTime at) -> std::optional<control::TargetResult> {
            if (peer == "Body" && at == SimTime{} + 50ms)
            {
                return control::TargetResult{peer, control::TargetStatus::Busy, std::nullopt, false, "the peer has too many commands in flight"};
            }
            return std::nullopt;
        };
        check_text(failure(world.run(read(head + steps))),
                   "scenario-write: the dither of Body.level (line 3) wrote 101 at 50 ms, and participant 'Body' did not take the write: "
                   "the ECU's agent had too many commands waiting (the peer has too many commands in flight)\n");
    }
    // A value within the range that the agent refuses for a reason of its own: status and reason.
    {
        World world;
        world.refuse = [](const std::string& peer, SimTime at) -> std::optional<control::TargetResult> {
            if (peer == "Body" && at == SimTime{} + 10ms)
            {
                return control::TargetResult{peer, control::TargetStatus::Invalid, std::nullopt, false, "level: the simulation has ended"};
            }
            return std::nullopt;
        };
        check_text(failure(world.run(read(head + steps))),
                   "scenario-write: the dither of Body.level (line 3) wrote 97 at 10 ms, and participant 'Body' did not take the write: "
                   "the ECU's agent refused it (level: the simulation has ended)\n");
    }
    // Two dithered inputs of Body in one command, and the agent says which one it refuses (the
    // result points at its position in the command): that input and its range, though it is the
    // second of the command.
    {
        World world;
        const ScenarioRun run = world.run(read(head + "  Body.ratio: { dither: { square: { amplitude: 0.1, period_ms: 20 } } }\n" + steps));
        check_text(failure(run), "scenario-write: the dither of Body.level (line 3) wrote 101 at 50 ms, above its range 0 to 100 (level: out of "
                                 "range): lower its amplitude, or keep the value further from the end of the range; the dither of 1 other "
                                 "input of that participant was in the same command; none of the 2 writes of that command was applied\n");
    }
}

TEST_CASE(scenario_dither_errors_say_what_is_wrong_and_what_to_write)
{
    const auto errors = [](const std::string& text) { return lines(read_scenario(kDitherHead + text, catalog()).findings); };
    const std::string steps = "steps:\n  - set: { Body.level: 1 }\n";
    // What a wave has and a dither cannot have.
    check_text(errors("inputs:\n"
                      "  Body.level: { dither: { triangle: { amplitude: 1, period_ms: 100 }, for_ms: 500 } }\n"
                      "  Body.ratio: { dither: { sine: { amplitude: 0.1, period_ms: 100 }, around: 0.5 } }\n"
                      "  Door.level: { dither: { pulse: { value: 1, width_ms: 50 } } }\n"
                      "  Door.ratio: { dither: { square: { low: 0, high: 1, period_ms: 100 } } }\n" +
                      steps),
               "E 3 value: the dither of Body.level has for_ms: a dither has no end, it stays until a dither step changes it; remove "
               "for_ms (dither: { Body.level: none } takes it off)\n"
               "E 4 value: the dither of Body.ratio has around: a dither swings around the value the scenario puts on the input (set, "
               "ramp, wave); remove around\n"
               "E 5 value: pulse cannot be a dither (it has a length): use sine, triangle, sawtooth, square with amplitude or noise, or "
               "write it as a wave step\n"
               "E 6 value: the dither of Door.ratio is a square with low: a dither swings around the value of the input, it has no "
               "values of its own; give the square amplitude instead (square: { amplitude: 1, period_ms: 100 })\n");
    // The variable: a number, an input, one the configuration has.
    check_text(errors("inputs:\n"
                      "  Body.lamp: { dither: { square: { period_ms: 100 } } }\n"
                      "  Body.mode: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n"
                      "  Body.gain: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n"
                      "  Body.pulse: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n"
                      "  Body.levle: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n" +
                      steps),
               "E 3 scenario-value: Body.lamp is a bool: a dither needs a number (an integer or a real input); remove it\n"
               "E 4 scenario-value: Body.mode is an enum: a dither needs a number (an integer or a real input); remove it\n"
               "E 5 scenario-not-writable: Body.gain is a parameter: it can be written only before the first step, that is with set in "
               "a step at 0 (at_ms: 0, or the first steps without a time), not with a dither\n"
               "E 6 scenario-not-writable: Body.pulse is an output of its ECU: it can be read (expect, wait_until), not written; write "
               "one of the ECU's inputs instead\n"
               "E 7 scenario-variable: 'Body.levle': participant 'Body' has no variable 'levle': did you mean 'Body.level'? "
               "(vehicle_sil_launch check --json lists the variables)\n");
    // The form of inputs, and of an entry (which has a dither; any other key is unknown).
    check_text(errors("inputs: [Body.level]\n" + steps),
               "E 2 value: inputs must be a mapping of inputs to what is on top of them ({ Body.level: { dither: { triangle: { "
               "amplitude: 1, period_ms: 100 } } } }), not a list\n");
    check_text(errors("inputs:\n"
                      "  Body.level: { dither: { triangle: { amplitude: 1, period_ms: 100 } }, clip: true }\n"
                      "  Body.ratio: { undersampled: ok }\n"
                      "  Door.level: { dither: none }\n"
                      "  Door.ratio: { dither: { triangle: { amplitude: 0.1, period_ms: 100 } }, undersampled: yes }\n"
                      "  Door.level: { dither: { noise: { amplitude: 1, hold_ms: 10, seed: 1 } } }\n" +
                      steps),
               "E 3 unknown-key: inputs: Body.level: unknown key 'clip': remove it, or use one of the keys here (dither, undersampled)\n"
               "E 4 missing-key: inputs: Body.ratio has no dither: say what is on top of the input (dither: { triangle: { amplitude: "
               "1, period_ms: 100 } }), or remove the entry\n"
               "E 5 value: the dither of Door.level in inputs is none: an input without a dither needs no entry; remove it\n"
               "E 6 value: undersampled must be ok (run the input's dither as it is, also where it changes faster than its ECU steps), "
               "not 'yes': write undersampled: ok, or remove it\n"
               // (Door.ratio's dither is read, and nothing puts a value on Door.ratio.)
               "W 6 scenario-dither-unused: the dither of Door.ratio is never written: no step that runs puts a value on Door.ratio "
               "(set, ramp or wave), and a dither is added to what the steps put; add a set of Door.ratio, or remove the entry\n"
               "E 7 duplicate-key: inputs: 'Door.level' appears twice (first at line 5); keep one of them\n");
    // The step: none or a shape, one shape, the shape's own keys.
    check_text(errors("steps:\n"
                      "  - dither: { Body.level: off }\n"
                      "  - dither: { Body.level: false, Body.ratio: \"none\" }\n" // (none in quotes is none: JSON has no other way)
                      "  - dither: Body.level\n"
                      "  - dither: { Body.level: { } }\n"
                      "  - dither: { Body.level: { sine: { amplitude: 1, period_ms: 100 }, noise: { amplitude: 1, hold_ms: 10, seed: 1 } } }\n"
                      "  - dither: { Body.level: { triangle: { amplitude: 1 } } }\n"
                      "  - dither: { Body.level: { triangle: { amplitude: 1, period_ms: 100, duty: 10 } } }\n"
                      "  - dither: { Body.level: { wobble: { amplitude: 1 } } }\n"
                      "  - dither: { Body.level: none }\n"
                      "    hold_for_ms: 10\n"),
               "E 3 value: the dither of Body.level must be none (which takes the dither off) or a shape ({ triangle: { amplitude: 1, "
               "period_ms: 100 } }), not 'off': write none, or a shape\n"
               "E 4 value: the dither of Body.level must be none (which takes the dither off) or a shape ({ triangle: { amplitude: 1, "
               "period_ms: 100 } }), not 'false': write none, or a shape\n"
               "E 5 value: dither must be a mapping of inputs to a shape or none ({ Body.level: { triangle: { amplitude: 1, period_ms: "
               "100 } } }, or { Body.level: none } to take a dither off), not 'Body.level'\n"
               "E 6 value: the dither of Body.level names no shape: give it one of sine, triangle, sawtooth, square, noise\n"
               "E 7 value: the dither of Body.level has sine, noise: an input has one dither; keep one of them\n"
               "E 8 missing-key: the triangle dither of Body.level needs period_ms or frequency_hz: how long one period is, or how many "
               "there are in a second\n"
               "E 9 unknown-key: the triangle dither of Body.level: unknown key 'duty': remove it, or use one of the keys here "
               "(amplitude, period_ms, frequency_hz, phase_deg)\n"
               "E 10 unknown-key: dither of Body.level: unknown key 'wobble': remove it, or use one of the keys here (sine, triangle, "
               "sawtooth, square, noise)\n"
               "E 12 value: hold_for_ms goes with expect, not with dither: remove it\n");
    // A parameter cannot stand for none or for the shape.
    check_text(errors("params: { what: none }\nsteps:\n  - dither: { Body.level: \"${what}\" }\n"),
               "E 4 value: the dither of Body.level is the parameter what: a parameter cannot stand for none or for a shape; write none "
               "or the shape here (its numbers can be parameters)\n");
}

TEST_CASE(scenario_dither_of_a_variable_the_configuration_does_not_have_is_skipped)
{
    // A dither step that uses an absent variable is skipped whole, as a set is; a declared
    // dither of one is left out, and a note says so. The other inputs keep theirs.
    ScenarioContext context;
    context.variant = "small";
    const ScenarioRead r = read_scenario(kDitherHead + "inputs:\n"
                                                       "  Body.boost: { dither: { triangle: { amplitude: 1, period_ms: 40 } } }\n"
                                                       "  Body.level: { dither: { square: { amplitude: 1, period_ms: 20 } } }\n"
                                                       "steps:\n"
                                                       "  - set: { Body.level: 5 }\n"
                                                       "  - at_ms: 20\n"
                                                       "    dither: { Body.boost: none, Body.level: none }\n"
                                                       "  - at_ms: 40\n"
                                                       "    expect: { Body.level: 6 }\n",
                                         catalog_without_boost(), context);
    check_text(lines(r.findings),
               "N 3 scenario-input-skipped: the dither of Body.boost is not used: Body.boost is absent: participant 'Body' does not "
               "have it in the variant small (absent: true in its variables); the steps that write Body.boost are skipped too\n"
               "N 7 scenario-step-skipped: the step is skipped (dither Body.boost; Body.level none): Body.boost is absent: participant "
               "'Body' does not have it in the variant small (absent: true in its variables); the other steps run, also those that "
               "would need what this one does\n");
    if (!r.scenario)
    {
        throw unit::Failure("no scenario");
    }
    CHECK(r.scenario->inputs.size() == 1 && r.skipped == 1);
    World world;
    const ScenarioRun run = world.run(*r.scenario);
    check_text(text_of(run), "passed over=50\n"
                             "6 passed start=0 at=0 end=0\n"
                             "7 skipped start=20 at=- end=20 scenario-skipped: Body.boost is absent: participant 'Body' does not have it in "
                             "the variant small (absent: true in its variables)\n"
                             "9 passed start=40 at=40 end=40\n");
    // The skipped step took nothing off: the dither goes on.
    check_text(written_to(world, "Body.level", 40ms), "6@0>0 4@10>10 6@20>20 4@30>30 6@40>40 ");
}

TEST_CASE(scenario_dither_that_can_leave_the_range_or_is_never_written_gets_a_warning)
{
    const auto findings = [](const std::string& text, const ScenarioCatalog& c = catalog()) {
        return lines(read_scenario(kDitherHead + text, c).findings);
    };
    const auto estimate = [](const std::string& variable) {
        return ". This is an estimate from the text (the dither that is on when the step comes, not the times): if it is right, the run "
               "fails at the write that is outside. Lower the amplitude, keep the values of " + variable +
               " further from the ends of its range, or take the dither off before them (dither: { " + variable + ": none })\n";
    };
    // The values the text puts on the input while the dither is on it, with the dither's
    // amplitude on both sides: a set's value, the ends of a ramp, where a wave around a value
    // goes. One finding per dither. A value that is only known in the run is not looked at.
    check_text(findings("inputs:\n"
                        "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
                        "  Body.ratio: { dither: { noise: { amplitude: 0.1, hold_ms: 10, seed: 1, distribution: gaussian } } }\n"
                        "  Door.level: { dither: { square: { amplitude: 3, period_ms: 80 } } }\n"
                        "steps:\n"
                        "  - set: { Body.level: 50, Body.ratio: 0.8, Door.level: 50 }\n"
                        "  - ramp: { Body.level: { to: 99, over_ms: 100 } }\n"
                        "  - wave: { Door.level: { sine: { amplitude: 5, period_ms: 80 }, around: 7, for_ms: 80 } }\n"
                        "  - dither: { Door.level: { triangle: { amplitude: 1, period_ms: 80 } } }\n"),
               "W 3 scenario-dither-range: the dither of Body.level may take it out of its range (from 0 to 100): on the 99 of line 8, "
               "the values written go from 97 to 101" + estimate("Body.level") +
               "W 4 scenario-dither-range: the dither of Body.ratio may take it out of its range (from 0 to 1): on the 0.8 of line 7, "
               "the values written go about from 0.5 to 1.1 (three times the amplitude of a gaussian noise, which is its standard "
               "deviation)" + estimate("Body.ratio") +
               "W 5 scenario-dither-range: the dither of Door.level may take it out of its range (from 0 to 100): on the 2 of line 9, "
               "the values written go from -1 to 5" + estimate("Door.level"));
    // Which dither is on which value, by the order of the steps: a dither that was taken off is
    // not on the value that comes after, and one that a step puts is on the value the input was
    // left at, not on those before. Nothing here leaves the range, and nothing is said.
    check_text(findings("inputs:\n"
                        "  Body.level: { dither: { triangle: { amplitude: 5, period_ms: 80 } } }\n"
                        "steps:\n"
                        "  - set: { Body.level: 50 }\n"
                        "  - at_ms: 200\n"
                        "    dither: { Body.level: none }\n"
                        "  - at_ms: 300\n"
                        "    set: { Body.level: 100 }\n"
                        "  - at_ms: 400\n"
                        "    set: { Body.level: 60 }\n"
                        "  - at_ms: 400\n"
                        "    dither: { Body.level: { triangle: { amplitude: 3, period_ms: 80 } } }\n"),
               "");
    // A dither that a step puts while the input stands near an end; and every value a dither may
    // leave the range on is in its one finding (the first with its numbers, the others counted).
    check_text(findings("inputs:\n"
                        "  Body.ratio: { dither: { triangle: { amplitude: 0.1, period_ms: 80 } } }\n"
                        "steps:\n"
                        "  - set: { Body.level: 98, Body.ratio: 0.95 }\n"
                        "  - dither: { Body.level: { triangle: { amplitude: 5, period_ms: 80 } } }\n"
                        "  - ramp: { Body.ratio: { from: 0.05, to: 0.5, over_ms: 100 } }\n"
                        "  - set: { Body.ratio: 1 }\n"),
               "W 3 scenario-dither-range: the dither of Body.ratio may take it out of its range (from 0 to 1): on the 0.95 of line 5, "
               "the values written go from 0.85 to 1.05; so on 2 other values (lines 7, 8)" + estimate("Body.ratio") +
               "W 6 scenario-dither-range: the dither of Body.level may take it out of its range (from 0 to 100): on the 98 of line 5, "
               "the values written go from 93 to 103" + estimate("Body.level"));
    // An integer is rounded as the run rounds it. 100 with a dither of 0.4 is written as 100:
    // nothing about the range, and the finding about the amplitude does not say "0.5 or more",
    // which would leave the range there. With 0.5 it is written as 101, and that is said.
    check_text(findings("inputs:\n  Body.level: { dither: { triangle: { amplitude: 0.4, period_ms: 80 } } }\n"
                        "steps:\n  - set: { Body.level: 100 }\n"),
               "W 3 scenario-wave-undersampled: the triangle dither of Body.level has an amplitude of 0.4, but Body.level is an integer: "
               "on a whole value every sum is rounded back to that value, so the ECU sees no dither there (only where a ramp or a wave "
               "is between two whole values). An amplitude of 0.5 or more would show, but on the 100 of line 5 it would leave the range "
               "(from 0 to 100): keep that value further from the end of the range first, or remove the dither; to run it as it is "
               "without this message, add undersampled: ok to the input's entry in inputs\n");
    check_text(findings("inputs:\n  Body.level: { dither: { triangle: { amplitude: 0.5, period_ms: 80 } } }\n"
                        "steps:\n  - set: { Body.level: 100 }\n"),
               "W 3 scenario-dither-range: the dither of Body.level may take it out of its range (from 0 to 100): on the 100 of line 5, "
               "the values written go from 100 to 101" + estimate("Body.level"));
    // What stays a false alarm (and fails check --strict): the estimate puts the ends of the
    // dither on the ends of a wave under it, whenever they are. Here the triangle is at its
    // bottom when the sine is at its top: no write is above 0.98, and the run passes.
    const std::string opposite = "inputs:\n  Body.ratio: { dither: { triangle: { amplitude: 0.02, period_ms: 80, phase_deg: 180 } } }\n"
                                 "steps:\n  - wave: { Body.ratio: { sine: { amplitude: 0.03, period_ms: 80 }, around: 0.96, for_ms: 80 } }\n"
                                 "  - expect: { Body.ratio: { ge: 0.9 } }\n";
    check_text(findings(opposite),
               "W 3 scenario-dither-range: the dither of Body.ratio may take it out of its range (from 0 to 1): on the 0.99 of line 5, "
               "the values written go from 0.97 to 1.01" + estimate("Body.ratio"));
    World passes;
    check_text(scenario_status_name(passes.run(read(kDitherHead + opposite)).status), "passed");
    // Within the range: nothing. The run refuses what is outside all the same (another test).
    check_text(findings("inputs:\n  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
                        "steps:\n  - set: { Body.level: 98 }\n  - ramp: { Body.level: { to: 2, over_ms: 100 } }\n"),
               "");
    // A declared dither of an input that no step puts a value on is never written. A dither step
    // is no value, and a step that this configuration skips does not count.
    ScenarioContext context;
    context.variant = "small";
    check_text(lines(read_scenario(kDitherHead + "inputs:\n"
                                                 "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
                                                 "  Door.level: { dither: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
                                                 "steps:\n"
                                                 "  - set: { Body.boost: 1, Door.level: 5 }\n"
                                                 "  - dither: { Body.level: none }\n"
                                                 "  - expect: { Body.level: 0 }\n",
                                   catalog_without_boost(), context)
                         .findings),
               "W 3 scenario-dither-unused: the dither of Body.level is never written: no step that runs puts a value on Body.level "
               "(set, ramp or wave), and a dither is added to what the steps put; add a set of Body.level, or remove the entry\n"
               "W 4 scenario-dither-unused: the dither of Door.level is never written: no step that runs puts a value on Door.level "
               "(set, ramp or wave), and a dither is added to what the steps put; add a set of Door.level, or remove the entry\n"
               "N 6 scenario-step-skipped: the step is skipped (set Body.boost; Door.level = 5): Body.boost is absent: participant "
               "'Body' does not have it in the variant small (absent: true in its variables); the other steps run, also those that "
               "would need what this one does\n");
    // What the ECU's step makes of a dither is said as for a wave, and how to run it as it is
    // says where to write that: the input's entry, or the step. An integer rounds a dither of
    // less than a half away on a whole value.
    check_text(findings("inputs:\n"
                        "  Body.ratio: { dither: { sine: { amplitude: 0.1, period_ms: 15 } } }\n"
                        "  Body.level: { dither: { triangle: { amplitude: 0.2, period_ms: 80 } } }\n"
                        "steps:\n"
                        "  - set: { Body.ratio: 0.5, Body.level: 50 }\n"
                        "  - dither: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 5, seed: 1 } } }\n"),
               "W 3 scenario-wave-undersampled: the sine dither of Body.ratio has a period of 15 ms, but participant 'Body' takes its "
               "inputs once per step of 10 ms: with fewer than two values per period the ECU sees a slower shape or a constant, not this "
               "one. Make the period 80 ms or more (8 values per period); to run it as it is without this message, add undersampled: ok "
               "to the input's entry in inputs\n"
               "W 4 scenario-wave-undersampled: the triangle dither of Body.level has an amplitude of 0.2, but Body.level is an integer: "
               "on a whole value every sum is rounded back to that value, so the ECU sees no dither there (only where a ramp or a wave "
               "is between two whole values). Make the amplitude 0.5 or more; to run it as it is without this message, add "
               "undersampled: ok to the input's entry in inputs\n"
               "W 7 scenario-wave-undersampled: the noise dither of Body.ratio changes every 5 ms, faster than the step of participant "
               "'Body' (10 ms): the ECU gets only the value at the time of each of its steps; the values between are never applied. Make "
               "hold_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n");
    check_text(findings("inputs:\n"
                        "  Body.ratio: { dither: { sine: { amplitude: 0.1, period_ms: 15 } }, undersampled: ok }\n"
                        "  Body.level: { dither: { triangle: { amplitude: 0.2, period_ms: 80 } }, undersampled: ok }\n"
                        "steps:\n"
                        "  - set: { Body.ratio: 0.5, Body.level: 50 }\n"
                        "  - dither: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 5, seed: 1 } } }\n"
                        "    undersampled: ok\n"),
               "");
}

TEST_CASE(scenario_with_a_dither_lists_what_its_text_puts_on_the_inputs)
{
    const auto listed = [](const Scenario& s) {
        std::string out;
        for (const ScenarioLineText& line : s.lines)
        {
            out += line.variable + " " + line.kind + " step " + (line.step ? std::to_string(*line.step) : "-") + " " +
                   (line.from_ms ? std::to_string(*line.from_ms) : "?") + "-" + (line.to_ms ? std::to_string(*line.to_ms) : "?") + " " +
                   line_time_source_name(line.time_source) + " line " + std::to_string(line.line) +
                   (line.after_wait > 0 ? " after " + std::to_string(line.after_wait) : "") + (line.shape ? " [" + line.shape->type + "]" : "") +
                   ": " + line.text + "\n";
        }
        return out;
    };
    // The declared dithers first (no step; they begin with the first write of their input, which
    // the text does not say), then what each step that runs puts on an input that has a dither
    // at some time, in the order of the file. A time after a step that waits is not known before
    // the run. Door.lamp has no dither: its set is not in the list.
    ScenarioContext context;
    context.variant = "small";
    const ScenarioRead r = read_scenario(kDitherHead + "inputs:\n"
                                                       "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
                                                       "  Body.boost: { dither: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
                                                       "steps:\n"
                                                       "  - set: { Body.level: 50, Door.lamp: true }\n"
                                                       "  - at_ms: 100\n"
                                                       "    ramp: { Body.level: { to: 80, over_ms: 60 } }\n"
                                                       "  - set: { Body.boost: 1 }\n"
                                                       "  - wait_until: { Body.level: { ge: 0 } }\n"
                                                       "    timeout_ms: 100\n"
                                                       "  - dither: { Body.level: none }\n"
                                                       "  - after_ms: 10\n"
                                                       "    wave: { Body.level: { pulse: { value: 10, width_ms: 20 } } }\n",
                                         catalog_without_boost(), context);
    if (!r.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(r.findings));
    }
    CHECK(r.scenario->has_dither);
    // (The texts are those of a timeline file's lines: with the variable.)
    check_text(listed(*r.scenario), "Body.level dither step - ?-? first_write line 3 [triangle]: Body.level triangle amplitude 2 period 80 ms\n"
                                    "Body.level set step 1 0-0 text line 6: Body.level = 50\n"
                                    "Body.level ramp step 2 100-160 text line 7 [ramp]: Body.level to 80 over 60 ms\n"
                                    "Body.level dither step 5 ?-? after_wait line 12 after 10: Body.level none\n"
                                    "Body.level wave step 6 ?-? after_wait line 13 after 10 [pulse]: Body.level pulse 10 width 20 ms\n");
    // The run's result has the same, and what "inputs" declares (of the variables there are).
    const ScenarioRun before = scenario_not_run(*r.scenario);
    CHECK(before.has_dither && before.lines.size() == 5 && before.inputs.size() == 1);
    CHECK(before.inputs[0].variable == "Body.level" && before.inputs[0].line == 3 &&
          before.inputs[0].text == "Body.level triangle amplitude 2 period 80 ms" && before.inputs[0].shape.has_value());
    // A scenario without a dither has no list: its outputs are as they were. A dither step alone is one.
    const Scenario plain = read(kHead + "  - set: { Body.level: 50 }\n");
    CHECK(!plain.has_dither && plain.lines.empty() && !scenario_not_run(plain).has_dither);
    const Scenario stepOnly = read(kHead + "  - dither: { Body.level: none }\n");
    CHECK(stepOnly.has_dither && stepOnly.lines.size() == 1 && stepOnly.inputs.empty());
}

TEST_CASE(scenario_dither_writes_of_no_step_do_not_decide_a_step)
{
    // One step moves two inputs: Body.level for 20 ms, Door.level for 200 ms. From 30 ms the
    // dither alone writes Body.level, while the step is still under way: those writes are no
    // step's, and the step passes when its own last write is applied, at 200 ms.
    const Scenario s = read(kDitherHead + "inputs:\n"
                                          "  Body.level: { dither: { square: { amplitude: 1, period_ms: 20 } } }\n"
                                          "steps:\n"
                                          "  - ramp: { Body.level: { from: 10, to: 50, over_ms: 20 }, Door.level: { from: 20, to: 40, over_ms: 200 } }\n");
    for (const SimDuration tick : {10ms, 30ms})
    {
        World world;
        world.tick = tick;
        const ScenarioRun run = world.run(s);
        check_text(text_of(run), "passed over=210\n5 passed start=0 at=200 end=200\n", ("tick " + std::to_string(tick.count())).c_str());
        check_text(written_to(world, "Body.level", 60ms), "11@0>0 29@10>10 51@20>20 49@30>30 51@40>40 49@50>50 51@60>60 ");
    }
}

TEST_CASE(scenario_end_ms_validation_and_runtime_settlement)
{
    // 1. Reader validation: negative, above max, and < static_end_ms fail; valid end_ms succeeds.
    {
        const ScenarioRead neg = read_scenario(kHead + "  - set: { Body.level: 10 }\n", catalog());
        CHECK(neg.scenario.has_value());
        const ScenarioRead badNeg = read_scenario("vehicle_sil_scenario: 1\nend_ms: -1\nsteps:\n  - set: { Body.level: 10 }\n", catalog());
        CHECK(!badNeg.scenario.has_value());
        CHECK(!badNeg.findings.empty() && badNeg.findings[0].code == "value");

        const ScenarioRead badMax = read_scenario("vehicle_sil_scenario: 1\nend_ms: 1000000000001\nsteps:\n  - set: { Body.level: 10 }\n", catalog());
        CHECK(!badMax.scenario.has_value());
        CHECK(!badMax.findings.empty() && badMax.findings[0].code == "value");

        const ScenarioRead badEarly = read_scenario("vehicle_sil_scenario: 1\nend_ms: 40\nsteps:\n"
                                                    "  - at_ms: 50\n    set: { Body.level: 10 }\n",
                                                    catalog());
        CHECK(!badEarly.scenario.has_value());
        CHECK(!badEarly.findings.empty() && badEarly.findings[0].code == "scenario-time");

        // wait_until / within_ms have unknown static end, yet static lower bound min_end_ms_ still
        // rejects end_ms smaller than the earliest possible end across steps (including after_ms).
        const ScenarioRead waitTooEarly = read_scenario("vehicle_sil_scenario: 1\nend_ms: 5\nsteps:\n"
                                                        "  - at_ms: 100\n    set: { Body.level: 10 }\n"
                                                        "  - wait_until: { Body.body.speed: { ge: 10 } }\n    timeout_ms: 200\n",
                                                        catalog());
        CHECK(!waitTooEarly.scenario.has_value());
        CHECK(!waitTooEarly.findings.empty() && waitTooEarly.findings[0].code == "scenario-time");

        const ScenarioRead afterWaitTooEarly = read_scenario("vehicle_sil_scenario: 1\nend_ms: 40\nsteps:\n"
                                                             "  - at_ms: 10\n    set: { Body.level: 10 }\n"
                                                             "  - wait_until: { Body.body.speed: { ge: 10 } }\n    timeout_ms: 200\n"
                                                             "  - after_ms: 50\n    set: { Body.level: 20 }\n",
                                                             catalog());
        CHECK(!afterWaitTooEarly.scenario.has_value());
        CHECK(!afterWaitTooEarly.findings.empty() && afterWaitTooEarly.findings[0].code == "scenario-time");

        const ScenarioRead withinTooEarly = read_scenario("vehicle_sil_scenario: 1\nend_ms: 20\nsteps:\n"
                                                          "  - at_ms: 50\n    expect: { Body.level: 0 }\n    within_ms: 100\n",
                                                          catalog());
        CHECK(!withinTooEarly.scenario.has_value());
        CHECK(!withinTooEarly.findings.empty() && withinTooEarly.findings[0].code == "scenario-time");

        const ScenarioRead waitExact = read_scenario("vehicle_sil_scenario: 1\nend_ms: 10\nsteps:\n"
                                                     "  - at_ms: 10\n    set: { Body.level: 10 }\n"
                                                     "  - wait_until: { Body.body.speed: { ge: 10 } }\n    timeout_ms: 200\n",
                                                     catalog());
        CHECK(waitExact.scenario.has_value() && waitExact.scenario->end_ms == std::optional<std::int64_t>(10));

        const ScenarioRead withinExact = read_scenario("vehicle_sil_scenario: 1\nend_ms: 50\nsteps:\n"
                                                       "  - at_ms: 50\n    expect: { Body.level: 0 }\n    within_ms: 100\n",
                                                       catalog());
        CHECK(withinExact.scenario.has_value() && withinExact.scenario->end_ms == std::optional<std::int64_t>(50));

        const ScenarioRead waitOk = read_scenario("vehicle_sil_scenario: 1\nend_ms: 40\nsteps:\n"
                                                  "  - at_ms: 10\n    set: { Body.level: 10 }\n"
                                                  "  - wait_until: { Body.body.speed: { ge: 10 } }\n    timeout_ms: 200\n",
                                                  catalog());
        CHECK(waitOk.scenario.has_value() && waitOk.scenario->end_ms == std::optional<std::int64_t>(40));

        const ScenarioRead expectOnly = read_scenario("vehicle_sil_scenario: 1\nend_ms: 50\nsteps:\n"
                                                      "  - expect: { Body.level: 0 }\n",
                                                      catalog());
        CHECK(expectOnly.scenario.has_value() && expectOnly.scenario->end_ms == std::optional<std::int64_t>(50));
    }

    // 2. end_ms == last.at_ms settles without advancing an extra tick across all timing policies.
    {
        const Scenario s = read("vehicle_sil_scenario: 1\nname: exact end\nend_ms: 50\nsteps:\n"
                                "  - at_ms: 50\n    set: { Body.level: 77 }\n");
        CHECK(s.end_ms == std::optional<std::int64_t>(50));
        for (const auto& [name, policy] : policies())
        {
            World world;
            world.tick = 10ms;
            world.policy = policy;
            const ScenarioRun run = world.run(s);
            CHECK(run.status == ScenarioStatus::Passed);
            CHECK(run.end_ms == std::optional<std::int64_t>(50));
            CHECK(run.over_at == std::optional<SimTime>(at(50ms)));
            check_text(applied(world, "Body.level"), "77@50 ", name.c_str());
        }
    }

    // 3. Misaligned ECU step > end_ms (write at 20 ms, Body step 15 ms -> applied at 30 ms):
    // can_settle_at waits until P >= applied_step (30 ms) so it never deadlocks.
    {
        const Scenario s = read("vehicle_sil_scenario: 1\nend_ms: 20\nsteps:\n"
                                "  - at_ms: 20\n    set: { Body.level: 33 }\n");
        World world;
        world.tick = 10ms;
        world.ecus[0].step = 15ms;
        const ScenarioRun run = world.run(s);
        CHECK(run.status == ScenarioStatus::Passed);
        CHECK(run.over_at == std::optional<SimTime>(at(30ms)));
        check_text(applied(world, "Body.level"), "33@30 ");
    }

    // 4. end_ms > last.at_ms with dither and timeline: holds until end_ms, records samples up to
    // end_ms, and emits no unjudged dither grid writes at t >= end_ms.
    {
        const Scenario s = read(kDitherHead + "end_ms: 60\n"
                                              "inputs:\n"
                                              "  Body.level: { dither: { square: { amplitude: 1, period_ms: 20 } } }\n"
                                              "steps:\n"
                                              "  - at_ms: 0\n"
                                              "    set: { Body.level: 50 }\n"
                                              "  - at_ms: 20\n"
                                              "    expect: { Body.level: 51 }\n");
        World world;
        world.tick = 10ms;
        TimelineRecord record(s);
        world.record = &record;
        const ScenarioRun run = world.run(s);
        CHECK(run.status == ScenarioStatus::Passed);
        CHECK(run.over_at == std::optional<SimTime>(at(60ms)));
        CHECK(record.recorded_to() == std::optional<SimTime>(at(60ms)));
        // Grid writes stop strictly before end_ms (last grid write is at 50 ms, NOT 60 ms).
        check_text(written_to(world, "Body.level", 80ms), "51@0>0 49@10>10 51@20>20 49@30>30 51@40>40 49@50>50 ");
    }

    // 5. Programmatic 0-step scenario with end_ms holds until end_ms; expect-only at 0ms with
    // end_ms holds until end_ms too.
    {
        Scenario zero;
        zero.end_ms = 40;
        World w0;
        w0.tick = 10ms;
        const ScenarioRun r0 = w0.run(zero);
        CHECK(r0.status == ScenarioStatus::Passed);
        CHECK(r0.over_at == std::optional<SimTime>(at(40ms)));

        const Scenario expOnly = read("vehicle_sil_scenario: 1\nend_ms: 40\nsteps:\n"
                                      "  - at_ms: 0\n    expect: { Body.level: 0 }\n");
        World w1;
        w1.tick = 10ms;
        const ScenarioRun r1 = w1.run(expOnly);
        CHECK(r1.status == ScenarioStatus::Passed);
        CHECK(r1.over_at == std::optional<SimTime>(at(40ms)));
    }

    // 6. stop_at_ms < end_ms ends incomplete even if all steps ran; stop_at_ms == end_ms passes if
    // all steps completed (with no suppressed write at stop_at_ms).
    {
        const Scenario s = read("vehicle_sil_scenario: 1\nend_ms: 60\nsteps:\n"
                                "  - at_ms: 10\n    expect: { Body.level: 0 }\n");
        World early;
        early.tick = 10ms;
        early.stop_at = at(40ms);
        const ScenarioRun rEarly = early.run(s);
        CHECK(rEarly.status == ScenarioStatus::Incomplete);

        World exact;
        exact.tick = 10ms;
        exact.stop_at = at(60ms);
        const ScenarioRun rExact = exact.run(s);
        CHECK(rExact.status == ScenarioStatus::Passed);
        CHECK(rExact.over_at == std::optional<SimTime>(at(60ms)));
    }

    // 7. Dither grid write failure after all steps have already Passed (in (last_step_at, end_ms]):
    // fails at scenario level without mutating any Passed step or emitting a contradictory step event.
    {
        // 7a. Refused at once (taken_at_once): 97 +/- 5 -> 92 at 0 ms, 102 at 40 ms (out of range).
        const Scenario s = read(kDitherHead + "end_ms: 80\n"
                                              "inputs:\n"
                                              "  Body.level: { dither: { square: { amplitude: 5, period_ms: 80, phase_deg: 180 } } }\n"
                                              "steps:\n"
                                              "  - at_ms: 0\n"
                                              "    set: { Body.level: 97 }\n"
                                              "  - at_ms: 0\n"
                                              "    expect: { Body.level: 92 }\n");
        World world;
        world.tick = 10ms;
        TimelineRecord record(s);
        world.record = &record;
        std::vector<std::pair<std::size_t, StepStatus>> events;
        world.on_step = [&](const ScenarioStepRun& step) { events.emplace_back(step.index, step.status); };
        const ScenarioRun run = world.run(s);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.code == "scenario-write");
        CHECK(run.message.find("wrote 102 at 40 ms") != std::string::npos);
        CHECK(run.failed_at == std::optional<SimTime>(at(40ms)));
        CHECK(run.failed_file.empty());
        CHECK_EQ(run.failed_line, std::size_t{4});
        CHECK(run.failed_name.empty());
        CHECK(run.failed_call_stack.empty());
        CHECK(run.over_at == std::optional<SimTime>(at(30ms)));
        CHECK_EQ(run.steps.size(), std::size_t{2});
        CHECK(run.steps[0].status == StepStatus::Passed);
        CHECK(run.steps[1].status == StepStatus::Passed);
        CHECK(run.steps[1].code.empty());
        CHECK(run.steps[1].message.empty());
        CHECK(!std::any_of(events.begin(), events.end(), [](const auto& ev) { return ev.second == StepStatus::Failed; }));
        CHECK(record.recorded_to() == std::optional<SimTime>(at(40ms)));
        const auto& series = record.series();
        const auto written_it = std::find_if(series.begin(), series.end(), [](const auto& sr) { return !sr.sampled; });
        CHECK(written_it != series.end());
        if (written_it != series.end())
        {
            CHECK(std::any_of(written_it->written.begin(), written_it->written.end(),
                              [](const auto& p) { return p.at == at(40ms) && p.refused; }));
        }
    }
    {
        // 7b. Rejected in ECU step (judge): 12 +/- 1 -> 11 at 0 ms, 13 at 40 ms (setter throws).
        const Scenario s = read(kDitherHead + "end_ms: 80\n"
                                              "inputs:\n"
                                              "  Body.level: { dither: { square: { amplitude: 1, period_ms: 80, phase_deg: 180 } } }\n"
                                              "steps:\n"
                                              "  - at_ms: 0\n"
                                              "    set: { Body.level: 12 }\n"
                                              "  - at_ms: 0\n"
                                              "    expect: { Body.level: 11 }\n");
        World world;
        world.tick = 10ms;
        std::vector<std::pair<std::size_t, StepStatus>> events;
        world.on_step = [&](const ScenarioStepRun& step) { events.emplace_back(step.index, step.status); };
        const ScenarioRun run = world.run(s);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.code == "scenario-write");
        CHECK(run.message.find("wrote 13 at 40 ms") != std::string::npos);
        CHECK(run.failed_at == std::optional<SimTime>(at(40ms)));
        CHECK(run.over_at == std::optional<SimTime>(at(50ms)));
        CHECK(run.steps[0].status == StepStatus::Passed);
        CHECK(run.steps[1].status == StepStatus::Passed);
        CHECK(!std::any_of(events.begin(), events.end(), [](const auto& ev) { return ev.second == StepStatus::Failed; }));
    }
    {
        // 7c. Post-pass fail_grid from a named dither: step inside a template preserves failed_file,
        // failed_line, failed_name, and failed_call_stack.
        std::map<std::string, std::string> files;
        files["templates/dither_step.yaml"] =
            "vehicle_sil_scenario_template: 1\n"
            "parameters:\n"
            "  amp: { type: integer }\n"
            "steps:\n"
            "  - name: \"dither inject ${amp}\"\n"
            "    at_ms: 0\n"
            "    dither: { Body.level: { square: { amplitude: \"${amp}\", period_ms: 80, phase_deg: 180 } } }\n";
        const std::string rootText =
            kDitherHead +
            "end_ms: 80\n"
            "steps:\n"
            "  - name: call dither\n"
            "    template: templates/dither_step.yaml\n"
            "    params: { amp: 5 }\n"
            "  - at_ms: 0\n"
            "    set: { Body.level: 97 }\n"
            "  - at_ms: 0\n"
            "    expect: { Body.level: 92 }\n";
        ScenarioContext ctx;
        ctx.read_template = [&](const std::string& p, std::string& prob) -> std::optional<std::string> {
            const auto it = files.find(p);
            if (it != files.end()) return it->second;
            prob = "missing";
            return std::nullopt;
        };
        const ScenarioRead r = read_scenario(rootText, catalog(), ctx);
        CHECK(r.scenario.has_value());
        World world;
        world.tick = 10ms;
        const ScenarioRun run = world.run(*r.scenario);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.code == "scenario-write");
        CHECK(run.failed_at == std::optional<SimTime>(at(40ms)));
        CHECK(run.failed_file == "templates/dither_step.yaml");
        CHECK_EQ(run.failed_line, std::size_t{5});
        CHECK(run.failed_name == "dither inject 5");
        CHECK_EQ(run.failed_call_stack.size(), std::size_t{1});
        CHECK(run.failed_call_stack[0].file.empty());
        CHECK_EQ(run.failed_call_stack[0].line, std::size_t{4});
        CHECK(run.failed_call_stack[0].name == "call dither");
    }
}

TEST_CASE(scenario_step_names_work_on_every_step_kind_with_interpolation)
{
    ScenarioCatalog cat = catalog();
    cat[0].absent.push_back("absent");
    ScenarioContext ctx;
    ctx.params = {ParamValue{"load_pct", "50", false, ParamSource::CommandLine},
                  ParamValue{"phase", "定格", true, ParamSource::Configuration},
                  ParamValue{"unused_top", "ignored", true, ParamSource::Scenario}};
    const std::string yaml =
        "vehicle_sil_scenario: 1\n"
        "name: \"literal ${unused}\"\n"
        "params: { rpm: 1800, label: \"待機\", n: 5 }\n"
        "steps:\n"
        "  - name: \"電源ON ${load_pct}%\"\n"
        "    set: { Body.level: 50, Body.ratio: 0.2 }\n"
        "  - name: \"${phase}: ${rpm} rpm\"\n"
        "    ramp: { Body.ratio: { to: 0.5, over_ms: 40 } }\n"
        "    expect: { Body.level: 50 }\n"
        "  - name: 1\n"
        "    wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 100 }, around: 0.5, for_ms: 100 } }\n"
        "  - name: \"$${n} dither\"\n"
        "    dither: { Body.ratio: none }\n"
        "  - name: \"${label}\"\n"
        "    wait_until: { Body.body.speed: { ge: 0 } }\n"
        "    timeout_ms: 100\n"
        "  - name: \"確認\"\n"
        "    expect: { Body.level: 50 }\n"
        "    hold_for_ms: 10\n"
        "  - name: \"skipped step\"\n"
        "    set: { Body.absent: 1 }\n";
    ScenarioRead r = read_scenario(yaml, cat, ctx);
    CHECK(r.usage_errors.empty());
    CHECK(std::none_of(r.findings.begin(), r.findings.end(), [](const Finding& f) { return f.severity == Severity::Error; }));
    CHECK(r.scenario.has_value());
    const Scenario& s = *r.scenario;
    CHECK(s.name == "literal ${unused}");
    CHECK_EQ(s.steps.size(), std::size_t{7});
    CHECK(s.steps[0].name == "電源ON 50%");
    CHECK(s.steps[0].text == "set Body.level = 50; Body.ratio = 0.2");
    CHECK(s.steps[0].file.empty());
    CHECK(s.steps[0].call_stack.empty());
    CHECK(s.steps[1].name == "定格: 1800 rpm");
    CHECK(s.steps[2].name == "1");
    CHECK(s.steps[3].name == "$5 dither");
    CHECK(s.steps[4].name == "待機");
    CHECK(s.steps[5].name == "確認");
    CHECK(s.steps[6].name == "skipped step");
    CHECK(!s.steps[6].skipped.empty());
    // load_pct (CLI) was marked used even though only step name referenced it.
    CHECK(std::any_of(r.params.begin(), r.params.end(), [](const ParamValue& p) { return p.name == "load_pct"; }));

    // JSON input works identically.
    const Scenario json = read(R"({"vehicle_sil_scenario": 1, "steps": [{"name": "json step", "set": {"Body.level": 1}}]})");
    CHECK(json.steps.at(0).name == "json step");

    // scenario_not_run and World::run preserve step names including skipped steps and on_step events.
    const ScenarioRun notRun = scenario_not_run(s);
    CHECK_EQ(notRun.steps.size(), std::size_t{7});
    CHECK(notRun.steps[0].name == "電源ON 50%");
    CHECK(notRun.steps[6].name == "skipped step");

    World world;
    std::vector<std::string> seenNames;
    world.on_step = [&](const ScenarioStepRun& st) { seenNames.push_back(st.name); };
    const ScenarioRun run = world.run(s);
    CHECK(run.status == ScenarioStatus::Passed);
    CHECK(run.steps[0].name == "電源ON 50%");
    CHECK(run.steps[6].name == "skipped step");
    CHECK(std::find(seenNames.begin(), seenNames.end(), "電源ON 50%") != seenNames.end());
    CHECK(std::find(seenNames.begin(), seenNames.end(), "skipped step") != seenNames.end());
}

TEST_CASE(scenario_step_name_errors_reject_empty_whitespace_control_chars_and_bad_params)
{
    const auto errors = [](const std::string& text) { return lines(read_scenario(text, catalog()).findings); };
    CHECK(errors(kHead + "  - name:\n    set: { Body.level: 1 }\n").find("step name must be a non-empty text") != std::string::npos);
    CHECK(errors(kHead + "  - name: null\n    set: { Body.level: 1 }\n").find("step name must be a non-empty text") != std::string::npos);
    CHECK(errors(kHead + "  - name: [a]\n    set: { Body.level: 1 }\n").find("step name must be a non-empty text") != std::string::npos);
    CHECK(errors(kHead + "  - name: { a: 1 }\n    set: { Body.level: 1 }\n").find("step name must be a non-empty text") != std::string::npos);
    CHECK(errors(kHead + "  - name: \"\"\n    set: { Body.level: 1 }\n").find("must not be empty or whitespace-only") != std::string::npos);
    CHECK(errors(kHead + "  - name: \"   \"\n    set: { Body.level: 1 }\n").find("must not be empty or whitespace-only") != std::string::npos);
    CHECK(errors(kHead + "  - name: \"\u3000\"\n    set: { Body.level: 1 }\n").find("must not be empty or whitespace-only") != std::string::npos);
    CHECK(errors(kHead + "  - name: \"\u00a0\"\n    set: { Body.level: 1 }\n").find("must not be empty or whitespace-only") != std::string::npos);
    CHECK(errors("vehicle_sil_scenario: 1\nparams: { blank: \"  \" }\nsteps:\n  - name: \"${blank}\"\n    set: { Body.level: 1 }\n")
              .find("must not be empty or whitespace-only") != std::string::npos);
    CHECK(errors("vehicle_sil_scenario: 1\nparams: { u3000: \"\u3000\" }\nsteps:\n  - name: \"${u3000}\"\n    set: { Body.level: 1 }\n")
              .find("must not be empty or whitespace-only") != std::string::npos);
    const Scenario okUnicodeWs = read(kHead + "  - name: \"a\u3000b\"\n    set: { Body.level: 1 }\n");
    CHECK(okUnicodeWs.steps.at(0).name == "a\u3000b");
    CHECK(errors(kHead + "  - name: \"a\\nb\"\n    set: { Body.level: 1 }\n").find("control character") != std::string::npos);
    CHECK(errors(kHead + "  - name: \"a\\tb\"\n    set: { Body.level: 1 }\n").find("control character") != std::string::npos);
    CHECK(errors(kHead + "  - name: \"a\\x7fb\"\n    set: { Body.level: 1 }\n").find("control character") != std::string::npos);
    CHECK(errors(kHead + "  - name: \"bad\x80\"\n    set: { Body.level: 1 }\n").find("not UTF-8") != std::string::npos);
    CHECK(errors(kHead + "  - name: a\n    name: b\n    set: { Body.level: 1 }\n").find("duplicate-key") != std::string::npos);
    for (const char* bad : {"\"${missing}\"", "\"$${missing}\"", "\"${a+1}\"", "\"${}\"", "\"${unclosed\"", "\"${a${b}}\""})
    {
        CHECK(errors(kHead + "  - name: " + bad + "\n    set: { Body.level: 1 }\n").find("scenario-param") != std::string::npos);
    }
}

TEST_CASE(scenario_step_and_dither_file_provenance_flows_through_runs)
{
    Scenario s = read(kHead + "  - name: direct\n    set: { Body.level: 10 }\n"
                              "  - at_ms: 10\n    expect: { Body.level: 10 }\n");
    s.steps[0].file = "scenarios/templates/init.yaml";
    s.steps[0].line = 7;
    s.steps[0].name = "通信確立";
    s.steps[0].call_stack = {SourceFrame{"scenarios/load.yaml", 20, "初期化",
                                         {SourceParam{"comms_timeout_ms", "50", false, ParamSource::TemplateCall}}}};
    const ScenarioRun notRun = scenario_not_run(s);
    CHECK(notRun.steps[0].file == "scenarios/templates/init.yaml");
    CHECK(notRun.steps[0].name == "通信確立");
    CHECK(notRun.steps[0].call_stack == s.steps[0].call_stack);
    CHECK(notRun.steps[1].file.empty());

    World world;
    std::vector<ScenarioStepRun> observed;
    world.on_step = [&](const ScenarioStepRun& st) { observed.push_back(st); };
    const ScenarioRun run = world.run(s);
    CHECK(run.steps[0].file == "scenarios/templates/init.yaml");
    CHECK(run.steps[0].name == "通信確立");
    CHECK(run.steps[0].call_stack == s.steps[0].call_stack);
    CHECK(!observed.empty());
    CHECK(observed[0].file == "scenarios/templates/init.yaml");
    CHECK(observed[0].call_stack == s.steps[0].call_stack);

    // Dither grid failure formats template file:line when ScenarioOverlay::file is non-empty.
    Scenario ditherFail = read(kDitherHead + "end_ms: 80\n"
                                             "inputs:\n"
                                             "  Body.level: { dither: { square: { amplitude: 5, period_ms: 80, phase_deg: 180 } } }\n"
                                             "steps:\n"
                                             "  - at_ms: 0\n    set: { Body.level: 97 }\n"
                                             "  - at_ms: 0\n    expect: { Body.level: 92 }\n");
    ditherFail.inputs[0].file = "scenarios/templates/dither.yaml";
    ditherFail.inputs[0].line = 5;
    World wFail;
    wFail.tick = 10ms;
    const ScenarioRun rFail = wFail.run(ditherFail);
    CHECK(rFail.status == ScenarioStatus::Failed);
    CHECK(rFail.message.find("(scenarios/templates/dither.yaml:5)") != std::string::npos);

    // ScenarioLineText::file and after_wait_file are populated for inputs and steps.
    ScenarioOverlay ov;
    ov.variable = 0;
    ov.file = "scenarios/templates/dither.yaml";
    ov.line = 12;
    ov.text = "Body.level triangle";
    Scenario lineScenario = read(kHead + "  - set: { Body.level: 10 }\n");
    lineScenario.inputs.push_back(ov);
    const std::vector<ScenarioLineText> inLines = scenario_inputs_text(lineScenario);
    CHECK_EQ(inLines.size(), std::size_t{1});
    CHECK(inLines[0].file == "scenarios/templates/dither.yaml");
    CHECK(inLines[0].line == 12);
}

TEST_CASE(scenario_templates_expand_with_lexical_scoping_quoting_and_provenance)
{
    auto files = std::map<std::string, std::string>{
        {"templates/power_on.yaml",
         "vehicle_sil_scenario_template: 1\n"
         "name: power on\n"
         "parameters:\n"
         "  lvl:\n"
         "    type: integer\n"
         "    default: 10\n"
         "  gear_mode:\n"
         "    type: string\n"
         "    default: off\n"
         "  flag:\n"
         "    type: boolean\n"
         "    default: true\n"
         "  gain:\n"
         "    type: number\n"
         "    default: 0.5\n"
         "  label:\n"
         "    type: string\n"
         "    default: default_lbl\n"
         "steps:\n"
         "  - name: \"${label}: set\"\n"
         "    set: { Body.level: \"${lvl}\", Door.mode: \"${gear_mode}\", Door.lamp: \"${flag}\", Body.ratio: \"${gain}\" }\n"
         "  - after_ms: 20\n"
         "    expect: { Body.level: \"${lvl}\" }\n"},
        {"templates/nested_outer.yaml",
         "vehicle_sil_scenario_template: 1\n"
         "parameters:\n"
         "  inner_lvl: { type: integer }\n"
         "  outer_tag: { type: string }\n"
         "  csv_path: { type: string, default: ../traces/curve.csv }\n"
         "steps:\n"
         "  - name: \"outer ${outer_tag}\"\n"
         "    after_ms: 10\n"
         "    template: ../common/inner.yaml\n"
         "    params:\n"
         "      lvl: \"${inner_lvl}\"\n"
         "      gear_mode: \"${outer_tag}\"\n"
         "      label: \"nested_${outer_tag}\"\n"
         "  - after_ms: 5\n"
         "    wave: { Body.level: { points: { file: \"${csv_path}\", value: level } } }\n"},
        {"common/inner.yaml",
         "vehicle_sil_scenario_template: 1\n"
         "parameters:\n"
         "  lvl: { type: integer }\n"
         "  gear_mode: { type: string }\n"
         "  label: { type: string }\n"
         "steps:\n"
         "  - name: \"${label}\"\n"
         "    after_ms: 15\n"
         "    set: { Body.level: \"${lvl}\", Door.mode: \"${gear_mode}\" }\n"},
        {"traces/curve.csv", "time,level\n0,10\n30,20\n"},
    };
    ScenarioCatalog cat = catalog();
    // Add an enum item named "1" (with numeric value 99) to Door.mode to verify from_cli -> quoted normalization.
    cat[1].variables[2].items.push_back(control::EnumItem{"1", 99});

    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/main.yaml";
    ctx.read_template = [&](const std::string& p, std::string& prob) -> std::optional<std::string> {
        if (const auto it = files.find(p); it != files.end())
        {
            return it->second;
        }
        prob = "does not exist";
        return std::nullopt;
    };
    ctx.read_file = ctx.read_template;
    ctx.params = {
        ParamValue{"cli_hex", "0x10", false, ParamSource::CommandLine, 0, true},
        ParamValue{"cli_enum_name", "1", false, ParamSource::CommandLine, 0, true},
        ParamValue{"cli_tag", "007", false, ParamSource::CommandLine, 0, true},
    };

    const std::string yaml =
        "vehicle_sil_scenario: 1\n"
        "params: { outer_lvl: 25, gain_val: 0.25 }\n"
        "steps:\n"
        "  - name: call one\n"
        "    at_ms: 0\n"
        "    template: templates/power_on.yaml\n"
        "    params:\n"
        "      lvl: \"${cli_hex}\"\n"
        "      gear_mode: \"${cli_enum_name}\"\n"
        "      gain: \"${gain_val}\"\n"
        "      label: \"tag_${cli_tag}\"\n"
        "  - name: call nested\n"
        "    after_ms: 50\n"
        "    template: templates/nested_outer.yaml\n"
        "    params:\n"
        "      inner_lvl: \"${outer_lvl}\"\n"
        "      outer_tag: on\n";
    const ScenarioRead r = read_scenario(yaml, cat, ctx);
    check_text(lines(r.findings), "");
    CHECK(r.usage_errors.empty());
    CHECK(r.scenario.has_value());
    const Scenario& s = *r.scenario;
    CHECK_EQ(s.steps.size(), std::size_t{4});

    // Step 0: from templates/power_on.yaml, at_ms = 0 inherited, cli_hex=0x10 -> 16, cli_enum_name="1" -> item 99!
    CHECK(s.steps[0].file == "scenarios/templates/power_on.yaml");
    CHECK_EQ(s.steps[0].line, std::size_t{20});
    CHECK(s.steps[0].name == "tag_007: set");
    CHECK(s.steps[0].at_ms == std::optional<std::int64_t>{0});
    CHECK(s.steps[0].text == "set Body.level = 16; Door.mode = 1 (99); Door.lamp = true; Body.ratio = 0.25");
    CHECK_EQ(s.steps[0].call_stack.size(), std::size_t{1});
    CHECK(s.steps[0].call_stack[0].file == "scenarios/main.yaml");
    CHECK_EQ(s.steps[0].call_stack[0].line, std::size_t{4});
    CHECK(s.steps[0].call_stack[0].name == "call one");
    CHECK_EQ(s.steps[0].call_stack[0].params.size(), std::size_t{5});
    // cli_enum_name passed via from_cli to type: string normalized quoted=true.
    CHECK(s.steps[0].call_stack[0].params[1].name == "gear_mode");
    CHECK(s.steps[0].call_stack[0].params[1].quoted == true);
    // flag used TemplateDefault.
    CHECK(s.steps[0].call_stack[0].params[2].name == "flag");
    CHECK(s.steps[0].call_stack[0].params[2].source == ParamSource::TemplateDefault);

    // Step 1: after_ms = 20 inside power_on.yaml
    CHECK(s.steps[1].after_ms == 20);
    CHECK(s.steps[1].at_ms == std::nullopt);

    // Step 2: nested 2-deep call: caller after_ms 50 + outer after_ms 10 + inner after_ms 15 = 75!
    CHECK(s.steps[2].file == "scenarios/common/inner.yaml");
    CHECK(s.steps[2].name == "nested_on");
    CHECK_EQ(s.steps[2].after_ms, std::int64_t{75});
    CHECK_EQ(s.steps[2].call_stack.size(), std::size_t{2});
    CHECK(s.steps[2].call_stack[0].file == "scenarios/main.yaml");
    CHECK(s.steps[2].call_stack[0].name == "call nested");
    CHECK(s.steps[2].call_stack[1].file == "scenarios/templates/nested_outer.yaml");
    CHECK(s.steps[2].call_stack[1].name == "outer on");

    // Step 3: wave points file inside template resolved ../traces/curve.csv relative to templates/.
    CHECK(s.steps[3].file == "scenarios/templates/nested_outer.yaml");
    CHECK_EQ(s.steps[3].motions.size(), std::size_t{1});
}

TEST_CASE(scenario_template_scoping_type_checks_and_errors)
{
    std::map<std::string, std::string> files;
    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/s.yaml";
    ctx.read_template = [&](const std::string& p, std::string& prob) -> std::optional<std::string> {
        if (const auto it = files.find(p); it != files.end())
        {
            return it->second;
        }
        prob = "does not exist";
        return std::nullopt;
    };
    ctx.read_file = ctx.read_template;
    const auto run_check = [&](const std::string& yaml) { return read_scenario(yaml, catalog(), ctx); };

    // 1. Lexical isolation: outer scenario/CLI params do NOT leak into templates.
    files["templates/leak.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - set: { Body.level: \"${outer_val}\" }\n";
    {
        ScenarioContext c2 = ctx;
        c2.params = {ParamValue{"outer_val", "10", false, ParamSource::CommandLine, 0, true}};
        const ScenarioRead r = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/leak.yaml\n", catalog(), c2);
        CHECK(lines(r.findings).find("E 3 scenario-param: ${outer_val} has no value") != std::string::npos);
        CHECK(lines(r.findings).find("parameters") != std::string::npos);
        CHECK(!r.params_incomplete);
        CHECK(!r.usage_errors.empty()); // outer_val was never used in caller scope!
    }

    // 2. Template passed as root scenario -> not-a-scenario; scenario passed as template -> not-a-template.
    CHECK(lines(run_check("vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Body.level: 1 }\n").findings)
              .find("not-a-scenario") != std::string::npos);
    files["templates/is_scenario.yaml"] = "vehicle_sil_scenario: 1\nsteps:\n  - set: { Body.level: 1 }\n";
    CHECK(lines(run_check(kHead + "  - template: templates/is_scenario.yaml\n").findings)
              .find("not-a-template") != std::string::npos);

    // 3. Call-site errors: ${...} in template path, parameters: typo hint, params: on non-template step.
    CHECK(lines(run_check(kHead + "  - template: \"templates/${x}.yaml\"\n").findings).find("scenario-param") != std::string::npos);
    CHECK(lines(run_check(kHead + "  - template: templates/leak.yaml\n    parameters: { a: 1 }\n").findings)
              .find("unknown key 'parameters': pass template arguments with 'params:'") != std::string::npos);
    CHECK(lines(run_check(kHead + "  - set: { Body.level: 1 }\n    params: { a: 1 }\n").findings)
              .find("params goes with template, not with set") != std::string::npos);

    // 4. ParameterDef errors: missing type, empty {}, null, default/values mismatch.
    files["templates/bad_defs.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  a: {}\n"
        "  b:\n"
        "  c: { default: 1 }\n"
        "  d: { type: integer, default: 1.5 }\n"
        "  e: { type: integer, values: [1, 2], default: 3 }\n"
        "steps:\n  - set: { Body.level: 1 }\n";
    {
        const std::string errs = lines(run_check(kHead + "  - template: templates/bad_defs.yaml\n").findings);
        CHECK(errs.find("an empty mapping") != std::string::npos);
        CHECK(errs.find("not empty") != std::string::npos);
        CHECK(errs.find("needs type") != std::string::npos);
        CHECK(errs.find("type: integer") != std::string::npos);
        CHECK(errs.find("not one of its allowed values") != std::string::npos);
    }

    // 5. type: string rejects YAML 1.2 non-string literals unless quoted or from_cli; millis rejects 0x10.
    files["templates/typed.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  s: { type: string }\n"
        "  ms: { type: integer, default: 0x10 }\n"
        "steps:\n"
        "  - name: \"${s}\"\n"
        "    after_ms: \"${ms}\"\n"
        "    set: { Body.level: 1 }\n";
    for (const char* badLit : {"123", "0x10", "0o7", "+1", "true", "True", "TRUE", "false", "False", "FALSE", ".inf", ".nan"})
    {
        const std::string errs = lines(run_check(
            std::string("vehicle_sil_scenario: 1\nsteps:\n  - template: templates/typed.yaml\n    params: { s: ") + badLit + ", ms: 10 }\n").findings);
        CHECK(errs.find("scenario-param") != std::string::npos);
    }
    for (const char* nullLit : {"~", "null"})
    {
        const std::string errs = lines(run_check(
            std::string("vehicle_sil_scenario: 1\nsteps:\n  - template: templates/typed.yaml\n    params: { s: ") + nullLit + ", ms: 10 }\n").findings);
        CHECK(errs.find("not empty") != std::string::npos);
    }
    // Hex default 0x10 accepted by type: integer, rejected by millis() (after_ms).
    CHECK(lines(run_check("vehicle_sil_scenario: 1\nsteps:\n  - template: templates/typed.yaml\n    params: { s: ok }\n").findings)
              .find("after_ms must be a whole number") != std::string::npos);

    // 6. Template syntax error attributed to template file:line with Finding::detail call_stack.
    files["templates/bad_syntax.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Body.level: [1 }\n";
    {
        const ScenarioRead r = run_check(kHead + "  - name: bad call\n    template: templates/bad_syntax.yaml\n");
        CHECK(!r.findings.empty());
        CHECK(r.findings[0].code == "syntax");
        CHECK(r.findings[0].file == "scenarios/templates/bad_syntax.yaml");
        CHECK(r.findings[0].detail.find("called from: scenarios/s.yaml:3 (step 'bad call')") != std::string::npos);
    }

    // 7. InheritedTiming at_ms inside template & end_known_ = false on expansion failure.
    files["templates/abs.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - at_ms: 50\n"
        "    set: { Body.level: 1 }\n";
    {
        const std::string errs = lines(run_check(kHead + "  - at_ms: 100\n    template: templates/abs.yaml\n").findings);
        CHECK(errs.find("before the template call's at_ms (100 ms)") != std::string::npos);
        CHECK(errs.find("inside a template, at_ms is still the scenario's absolute time") != std::string::npos);
    }

    // 8. Cycle, depth (> 16), step count (> 10,000), and sandboxed .. path escape.
    files["templates/cyc_a.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - template: cyc_b.yaml\n";
    files["templates/cyc_b.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - template: cyc_a.yaml\n";
    CHECK(lines(run_check(kHead + "  - template: templates/cyc_a.yaml\n").findings).find("scenario-template-cycle") != std::string::npos);

    files["templates/escape.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - template: ../../secret.yaml\n";
    CHECK(lines(run_check(kHead + "  - template: templates/escape.yaml\n").findings).find("scenario-template-file") != std::string::npos);

    files["templates/root_csv_escape.yaml"] =
        "vehicle_sil_scenario_template: 1\nsteps:\n  - wave: { Body.level: { points: { file: ../../secret.csv, value: level } } }\n";
    CHECK(lines(run_check(kHead + "  - template: templates/root_csv_escape.yaml\n").findings).find("scenario-points-file") != std::string::npos);

    // Depth 17 triggers scenario-template-depth.
    for (int d = 1; d <= 17; ++d)
    {
        const std::string next = d < 17 ? "  - template: d" + std::to_string(d + 1) + ".yaml\n" : "  - set: { Body.level: 1 }\n";
        files["templates/d" + std::to_string(d) + ".yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n" + next;
    }
    CHECK(lines(run_check(kHead + "  - template: templates/d1.yaml\n").findings).find("scenario-template-depth") != std::string::npos);

    // Binary tree depth 14 (> 10,000 steps) triggers scenario-template-steps.
    files["templates/leaf.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Body.level: 1 }\n";
    for (int d = 1; d <= 14; ++d)
    {
        const std::string child = d == 14 ? "leaf.yaml" : "b" + std::to_string(d + 1) + ".yaml";
        files["templates/b" + std::to_string(d) + ".yaml"] =
            "vehicle_sil_scenario_template: 1\nsteps:\n  - template: " + child + "\n  - template: " + child + "\n";
    }
    CHECK(lines(run_check(kHead + "  - template: templates/b1.yaml\n").findings).find("scenario-template-steps") != std::string::npos);

    // 9. Dithers inside templates carry Finding::detail call_stack and ScenarioOverlay::file.
    files["templates/dither.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - name: add dither\n"
        "    dither: { Body.level: { square: { amplitude: 5, period_ms: 80 } } }\n"
        "  - set: { Body.level: 98 }\n";
    {
        const ScenarioRead r = run_check(kDitherHead + "steps:\n  - name: call dither\n    template: templates/dither.yaml\n");
        const auto dIt = std::find_if(r.findings.begin(), r.findings.end(),
                                      [](const Finding& f) { return f.code == "scenario-dither-range"; });
        CHECK(dIt != r.findings.end());
        CHECK(dIt->file == "scenarios/templates/dither.yaml");
        CHECK(dIt->detail.find("called from: scenarios/s.yaml:3 (step 'call dither')") != std::string::npos);
    }

    // 10. --param passed to a failing template call surfaces the template error, not a false usage error.
    {
        ScenarioContext c2 = ctx;
        c2.params = {ParamValue{"cli_param", "50", false, ParamSource::CommandLine, 0, true}};

        const ScenarioRead missing = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/missing.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(missing.params_incomplete);
        CHECK(missing.usage_errors.empty());
        CHECK(lines(missing.findings).find("scenario-template-file") != std::string::npos);

        files["templates/cyc_param_a.yaml"] =
            "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer }\nsteps:\n  - template: cyc_param_b.yaml\n";
        files["templates/cyc_param_b.yaml"] =
            "vehicle_sil_scenario_template: 1\nsteps:\n  - template: cyc_param_a.yaml\n    params: { throttle: 10 }\n";
        const ScenarioRead cyclic = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/cyc_param_a.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(cyclic.params_incomplete);
        CHECK(cyclic.usage_errors.empty());
        CHECK(lines(cyclic.findings).find("scenario-template-cycle") != std::string::npos);

        // Also when cyc_a.yaml declares no params and caller passes unknown param referencing ${cli_param},
        // scenario-param is surfaced instead of a false usage error.
        const ScenarioRead unknownArg = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/cyc_a.yaml\n    params: { unknown_arg: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(unknownArg.params_incomplete);
        CHECK(unknownArg.usage_errors.empty());
        CHECK(lines(unknownArg.findings).find("scenario-param") != std::string::npos);

        const ScenarioRead badSyntax = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/bad_syntax.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(badSyntax.params_incomplete);
        CHECK(badSyntax.usage_errors.empty());
        CHECK(lines(badSyntax.findings).find("syntax:") != std::string::npos);

        const ScenarioRead tooManySteps = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/b1.yaml\n  - template: templates/leaf.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(tooManySteps.params_incomplete);
        CHECK(tooManySteps.usage_errors.empty());
        CHECK(lines(tooManySteps.findings).find("scenario-template-steps") != std::string::npos);

        const ScenarioRead mixedAction = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/leaf.yaml\n    set: { Body.level: 1 }\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(mixedAction.params_incomplete);
        CHECK(mixedAction.usage_errors.empty());
        CHECK(lines(mixedAction.findings).find("a step does one thing") != std::string::npos);

        files["templates/empty_steps.yaml"] =
            "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer }\nsteps: []\n";
        const ScenarioRead emptyTplSteps = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/empty_steps.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(emptyTplSteps.params_incomplete);
        CHECK(emptyTplSteps.usage_errors.empty());
        CHECK(lines(emptyTplSteps.findings).find("an empty list") != std::string::npos);

        files["templates/bad_desc_val.yaml"] =
            "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer, description: 123 }\nsteps:\n  - set: { Body.level: 1 }\n";
        const ScenarioRead badTplDesc = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/bad_desc_val.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(badTplDesc.params_incomplete);
        CHECK(badTplDesc.usage_errors.empty());
        CHECK(lines(badTplDesc.findings).find("description of parameter 'throttle' must be a text") != std::string::npos);

        files["templates/missing_type.yaml"] =
            "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { default: 1 }\nsteps:\n  - set: { Body.level: 1 }\n";
        const ScenarioRead missingTplType = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/missing_type.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(missingTplType.params_incomplete);
        CHECK(missingTplType.usage_errors.empty());
        CHECK(lines(missingTplType.findings).find("missing-key:") != std::string::npos);

        files["templates/unknown_tpl_key.yaml"] =
            "vehicle_sil_scenario_template: 1\nonly: [small]\nsteps:\n  - set: { Body.level: 1 }\n";
        const ScenarioRead unknownTplKey = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/unknown_tpl_key.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(unknownTplKey.params_incomplete);
        CHECK(unknownTplKey.usage_errors.empty());
        CHECK(lines(unknownTplKey.findings).find("unknown-key:") != std::string::npos);

        files["templates/dup_tpl_key.yaml"] =
            "vehicle_sil_scenario_template: 1\nname: a\nname: b\nsteps:\n  - set: { Body.level: 1 }\n";
        const ScenarioRead dupTplKey = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/dup_tpl_key.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(dupTplKey.params_incomplete);
        CHECK(dupTplKey.usage_errors.empty());
        CHECK(lines(dupTplKey.findings).find("duplicate-key:") != std::string::npos);

        const ScenarioRead nullTpl = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: null\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(nullTpl.params_incomplete);
        CHECK(nullTpl.usage_errors.empty());
        CHECK(lines(nullTpl.findings).find("template must be a template file path") != std::string::npos);

        const ScenarioRead paramsOnSet = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - set: { Body.level: 1 }\n    params: { throttle: \"${cli_param}\" }\n",
            catalog(), c2);
        CHECK(paramsOnSet.params_incomplete);
        CHECK(paramsOnSet.usage_errors.empty());
        CHECK(lines(paramsOnSet.findings).find("params goes with template") != std::string::npos);

        const ScenarioRead rootSyntax = read_scenario("vehicle_sil_scenario: 1\nsteps:\n  - set: [1\n", catalog(), c2);
        CHECK(rootSyntax.params_incomplete);
        CHECK(rootSyntax.usage_errors.empty());
        CHECK(lines(rootSyntax.findings).find("syntax:") != std::string::npos);

        const ScenarioRead utf16 = read_scenario(std::string("\xFF\xFEvehicle_sil_scenario: 1\n", 26), catalog(), c2);
        CHECK(utf16.params_incomplete);
        CHECK(utf16.usage_errors.empty());
        CHECK(lines(utf16.findings).find("encoding:") != std::string::npos);

        // When load_template() and read_template_call_params() succeed, an error inside the template body
        // or on a non-template step keeps params_incomplete == false so genuinely unused --param is caught.
        files["templates/body_bad_time.yaml"] =
            "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer }\nsteps:\n"
            "  - at_ms: \"bad\"\n    set: { Body.level: \"${throttle}\" }\n";
        const ScenarioRead bodyBadTime = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/body_bad_time.yaml\n    params: { throttle: 10 }\n",
            catalog(), c2);
        CHECK(!bodyBadTime.params_incomplete);
        CHECK(!bodyBadTime.usage_errors.empty());

        const ScenarioRead badTime = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - at_ms: \"bad\"\n    set: { Body.level: 1 }\n", catalog(), c2);
        CHECK(!badTime.params_incomplete);
        CHECK(!badTime.usage_errors.empty());

        files["templates/healthy_param.yaml"] =
            "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer }\nsteps:\n"
            "  - set: { Body.level: \"${throttle}\" }\n";
        const ScenarioRead healthyUnused = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/healthy_param.yaml\n    params: { throttle: 10 }\n",
            catalog(), c2);
        CHECK(!healthyUnused.params_incomplete);
        CHECK(!healthyUnused.usage_errors.empty());
    }

    // 11. ParameterDef.description accepts quoted/unquoted text and rejects unquoted YAML 1.2 non-string literals.
    files["templates/desc_ok.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  a: { type: integer, default: 1, description: \"123\" }\n"
        "  b: { type: integer, default: 2, description: hello world }\n"
        "steps:\n  - set: { Body.level: 1 }\n";
    CHECK(lines(run_check(kHead + "  - template: templates/desc_ok.yaml\n").findings).empty());

    for (const char* badDesc : {"123", "true", "0x10", ".inf", "null", "[]"})
    {
        files["templates/desc_bad.yaml"] =
            std::string("vehicle_sil_scenario_template: 1\nparameters:\n  a: { type: integer, default: 1, description: ") +
            badDesc + " }\nsteps:\n  - set: { Body.level: 1 }\n";
        const std::string errs = lines(run_check(kHead + "  - template: templates/desc_bad.yaml\n").findings);
        CHECK(errs.find("description of parameter 'a' must be a text") != std::string::npos);
    }
}

TEST_CASE(scenario_cases_parsing_precedence_only_intersection_and_inspect)
{
    CHECK(std::string(param_source_name(ParamSource::Case)) == "case");
    CHECK(std::string(param_source_text(ParamSource::Case)) == "the case's params");

    // 1. Parsing valid cases (Unicode, -10C, a--b, empty {}, JSON), preserving file order.
    const std::string multi =
        "vehicle_sil_scenario: 1\n"
        "params: { lvl: 10, ms: 50 }\n"
        "cases:\n"
        "  無負荷: { params: { lvl: 0 } }\n"
        "  定格: {}\n"
        "  \"-10C\": { params: { lvl: 20, ms: 80 } }\n"
        "  \"a--b\": { params: { lvl: 30 } }\n"
        "steps:\n"
        "  - after_ms: ${ms}\n"
        "    set: { Body.level: \"${lvl}\" }\n";
    {
        ScenarioContext ctx;
        ctx.case_name = "無負荷";
        const ScenarioRead r = read_scenario(multi, catalog(), ctx);
        CHECK(r.scenario.has_value());
        CHECK(r.case_name == "無負荷");
        CHECK(r.cases == std::vector<std::string>({"無負荷", "定格", "-10C", "a--b"}));
        CHECK(r.scenario->case_name == "無負荷");
        CHECK(r.scenario->cases == r.cases);
        CHECK(r.params.size() == 2);
        CHECK(r.params[0].text == "0");
        CHECK(r.params[0].source == ParamSource::Case);
        CHECK(r.params[1].text == "50");
        CHECK(r.params[1].source == ParamSource::Scenario);
        CHECK(r.scenario->params == r.params);

        const ScenarioRun notRun = scenario_not_run(*r.scenario);
        CHECK(notRun.case_name == "無負荷");
        CHECK(notRun.params == r.params);
    }
    {
        ScenarioContext ctx;
        ctx.case_name = "-10C";
        const ScenarioRead r = read_scenario(multi, catalog(), ctx);
        CHECK(r.scenario.has_value());
        CHECK(r.case_name == "-10C");
        CHECK(r.params[0].text == "20");
        CHECK(r.params[1].text == "80");
    }
    {
        const std::string json =
            R"({"vehicle_sil_scenario": 1, "params": {"lvl": 1}, "cases": {"first": {}, "second": {"params": {"lvl": 2}}}, "steps": [{"set": {"Body.level": "${lvl}"}}]})";
        ScenarioContext ctx;
        ctx.case_name = "second";
        const ScenarioRead r = read_scenario(json, catalog(), ctx);
        CHECK(r.scenario.has_value());
        CHECK(r.cases == std::vector<std::string>({"first", "second"}));
        CHECK(r.params[0].text == "2");
        CHECK(r.params[0].source == ParamSource::Case);
    }

    // 2. Rejecting malformed cases: empty, non-map, duplicate, ^--, control chars (incl. trailing \n), invalid UTF-8, null CaseDef, unknown key, ${...}.
    const auto err = [](const std::string& text, const std::optional<std::string>& c = std::nullopt) {
        ScenarioContext ctx;
        ctx.case_name = c;
        return lines(read_scenario(text, catalog(), ctx).findings);
    };
    CHECK(err("vehicle_sil_scenario: 1\ncases: {}\nsteps:\n  - set: { Body.level: 1 }\n").find("non-empty mapping") !=
          std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\ncases: 1\nsteps:\n  - set: { Body.level: 1 }\n").find("non-empty mapping") !=
          std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\ncases:\n  a: {}\n  a: {}\nsteps:\n  - set: { Body.level: 1 }\n", "a").find("duplicate-key") !=
          std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\ncases:\n  \"--bad\": {}\nsteps:\n  - set: { Body.level: 1 }\n").find("does not start with '--'") !=
          std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\ncases:\n  \"\": {}\nsteps:\n  - set: { Body.level: 1 }\n").find("non-empty UTF-8") !=
          std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\ncases:\n  \"a\\n\": {}\nsteps:\n  - set: { Body.level: 1 }\n").find("on one line") !=
          std::string::npos);
    CHECK(err(std::string("vehicle_sil_scenario: 1\ncases:\n  \"\xFF\": {}\nsteps:\n  - set: { Body.level: 1 }\n"))
              .find("non-empty UTF-8") != std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\ncases:\n  a:\nsteps:\n  - set: { Body.level: 1 }\n", "a").find("must be a mapping") !=
          std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\ncases:\n  a: { steps: [] }\nsteps:\n  - set: { Body.level: 1 }\n", "a")
              .find("unknown key 'steps'") != std::string::npos);
    CHECK(err("vehicle_sil_scenario: 1\nparams: { y: 1 }\ncases:\n  a: { params: { x: \"${y}\" } }\nsteps:\n  - set: { Body.level: \"${y}\" }\n",
              "a")
              .find("cannot use another parameter") != std::string::npos);

    // 3. Case selection usage errors.
    {
        const ScenarioRead missing = read_scenario(multi, catalog());
        CHECK(!missing.scenario.has_value());
        CHECK(missing.params_incomplete);
        CHECK(!missing.usage_errors.empty());
        CHECK(missing.usage_errors[0].message.find("give --case <name> with one of 無負荷, 定格, -10C, a--b") != std::string::npos);

        ScenarioContext badCtx;
        badCtx.case_name = "定格X";
        const ScenarioRead typo = read_scenario(multi, catalog(), badCtx);
        CHECK(typo.params_incomplete);
        CHECK(!typo.usage_errors.empty());
        CHECK(typo.usage_errors[0].message.find("did you mean '定格'?") != std::string::npos);

        ScenarioContext caselessCtx;
        caselessCtx.case_name = "any";
        const ScenarioRead caseless = read_scenario(kHead + "  - set: { Body.level: 1 }\n", catalog(), caselessCtx);
        CHECK(caseless.params_incomplete);
        CHECK(!caseless.usage_errors.empty());
        CHECK(caseless.usage_errors[0].message.find("the scenario has no cases") != std::string::npos);
    }

    // 4. Rank-merged 6-level outer precedence independent of context.params vector order.
    {
        const std::string text =
            "vehicle_sil_scenario: 1\n"
            "params: { p_scn: 1, p_cfg: 1, p_var: 1, p_case: 1, p_req: 1, p_cli: 1 }\n"
            "cases:\n"
            "  c1:\n"
            "    params: { p_case: 4, p_req: 4, p_cli: 4 }\n"
            "steps:\n"
            "  - set: { Body.level: \"${p_scn}\" }\n"
            "  - set: { Body.level: \"${p_cfg}\" }\n"
            "  - set: { Body.level: \"${p_var}\" }\n"
            "  - set: { Body.level: \"${p_case}\" }\n"
            "  - set: { Body.level: \"${p_req}\" }\n"
            "  - set: { Body.level: \"${p_cli}\" }\n";
        ScenarioContext ctx;
        ctx.case_name = "c1";
        // Intentionally scramble order: CommandLine before RequestFile before Variant before Configuration.
        ctx.params = {
            ParamValue{"p_cli", "6", false, ParamSource::CommandLine, 0, true},
            ParamValue{"p_req", "5", false, ParamSource::RequestFile, 2, false},
            ParamValue{"p_var", "3", false, ParamSource::Variant, 3, false},
            ParamValue{"p_cfg", "2", false, ParamSource::Configuration, 4, false},
            ParamValue{"p_case", "2", false, ParamSource::Variant, 5, false},
            ParamValue{"p_req", "3", false, ParamSource::Variant, 6, false},
            ParamValue{"p_cli", "2", false, ParamSource::Configuration, 7, false},
        };
        const ScenarioRead r = read_scenario(text, catalog(), ctx);
        CHECK(r.scenario.has_value());
        const auto get = [&](const std::string& name) -> const ParamValue* {
            const auto it = std::find_if(r.params.begin(), r.params.end(), [&](const ParamValue& p) { return p.name == name; });
            return it == r.params.end() ? nullptr : &*it;
        };
        CHECK(get("p_scn")->text == "1" && get("p_scn")->source == ParamSource::Scenario);
        CHECK(get("p_cfg")->text == "2" && get("p_cfg")->source == ParamSource::Configuration);
        CHECK(get("p_var")->text == "3" && get("p_var")->source == ParamSource::Variant);
        CHECK(get("p_case")->text == "4" && get("p_case")->source == ParamSource::Case);
        CHECK(get("p_req")->text == "5" && get("p_req")->source == ParamSource::RequestFile);
        CHECK(get("p_cli")->text == "6" && get("p_cli")->source == ParamSource::CommandLine);
    }

    // 5. Template lexical isolation with Case params, and cases.*.params undeclared/unused validation (even with params_for_many).
    {
        std::map<std::string, std::string> files;
        files["templates/t.yaml"] =
            "vehicle_sil_scenario_template: 1\n"
            "parameters:\n  x: { type: integer }\n"
            "steps:\n  - set: { Body.level: \"${x}\" }\n";
        const std::string badCaseParam =
            "vehicle_sil_scenario: 1\n"
            "params: { val: 1 }\n"
            "cases:\n"
            "  ok_case: { params: { val: 5 } }\n"
            "  bad_case: { params: { vl: 9 } }\n"
            "steps:\n"
            "  - template: templates/t.yaml\n"
            "    params: { x: \"${val}\" }\n";
        ScenarioContext ctx;
        ctx.case_name = "ok_case";
        ctx.params_for_many = true;
        ctx.read_template = [&](const std::string& p, std::string& prob) -> std::optional<std::string> {
            const auto it = files.find(p);
            if (it != files.end()) return it->second;
            prob = "missing";
            return std::nullopt;
        };
        const ScenarioRead r = read_scenario(badCaseParam, catalog(), ctx);
        CHECK(!r.scenario.has_value());
        CHECK(lines(r.findings).find("case 'bad_case': parameter 'vl' is not declared in params or used in steps (did you mean 'val'?)") !=
              std::string::npos);

        // Template lexical isolation: case param cannot leak into template without explicit params:.
        files["templates/leak.yaml"] =
            "vehicle_sil_scenario_template: 1\n"
            "steps:\n  - set: { Body.level: \"${val}\" }\n";
        const std::string leakText =
            "vehicle_sil_scenario: 1\n"
            "params: { val: 1 }\n"
            "cases: { c: { params: { val: 7 } } }\n"
            "steps:\n  - template: templates/leak.yaml\n";
        ctx.case_name = "c";
        const ScenarioRead leakRead = read_scenario(leakText, catalog(), ctx);
        CHECK(lines(leakRead.findings).find("${val} has no value") != std::string::npos);
    }

    // 6. only: intersection (Reconciled Contract 4).
    {
        const std::string text =
            "vehicle_sil_scenario: 1\n"
            "only: [small, medium]\n"
            "cases:\n"
            "  c_med: { only: [medium, large], params: { unused_typo: 1 } }\n"
            "  c_empty: { only: [large] }\n"
            "steps:\n"
            "  - set: { Body.level: 1 }\n";
        ScenarioContext ctx;
        ctx.variants = {"small", "medium", "large"};
        ctx.variant = "small";
        ctx.case_name = "c_med";
        // effective_only = [medium]; variant is "small" -> skipped cleanly with errors() == 1, and cases.*.params NOT checked when not_for_variant!
        const ScenarioRead skipped = read_scenario(text, catalog(), ctx);
        CHECK(skipped.not_for_variant);
        CHECK(skipped.only == std::vector<std::string>({"medium"}));
        CHECK(skipped.findings.size() == 1);
        CHECK(skipped.findings[0].code == "scenario-variant");

        // Empty intersection when both non-empty -> not_for_variant == false (fails loudly).
        ctx.case_name = "c_empty";
        const ScenarioRead emptyInter = read_scenario(text, catalog(), ctx);
        CHECK(!emptyInter.not_for_variant);
        CHECK(emptyInter.only == std::vector<std::string>({}));
        CHECK(lines(emptyInter.findings).find("have no variant in common") != std::string::npos);

        // Raw variant typo in unselected case -> errors > 1 so test.cpp fails loudly instead of skipping.
        const std::string typoText =
            "vehicle_sil_scenario: 1\n"
            "only: [small, medium]\n"
            "cases:\n"
            "  c_med: { only: [medium] }\n"
            "  c_typo: { only: [medum] }\n"
            "steps:\n  - set: { Body.level: 1 }\n";
        ctx.case_name = "c_med";
        ctx.variant = "small";
        const ScenarioRead typoRead = read_scenario(typoText, catalog(), ctx);
        CHECK(typoRead.not_for_variant);
        CHECK(typoRead.findings.size() == 2);
        CHECK(lines(typoRead.findings).find("did you mean 'medium'?") != std::string::npos);
    }

    // 7. inspect_scenario().
    {
        ScenarioContext inspectCtx;
        inspectCtx.variants = {"small", "medium"};
        const ScenarioInspection inspMulti = inspect_scenario(multi, inspectCtx);
        CHECK(inspMulti.has_cases);
        CHECK(inspMulti.cases == std::vector<std::string>({"無負荷", "定格", "-10C", "a--b"}));
        CHECK(!inspMulti.params_incomplete);
        CHECK(inspMulti.param_names == std::vector<std::string>({"lvl", "ms"}));
        CHECK(std::none_of(inspMulti.findings.begin(), inspMulti.findings.end(),
                           [](const Finding& f) { return f.severity == Severity::Error; }));

        const ScenarioInspection inspCaseless =
            inspect_scenario("vehicle_sil_scenario: 1\nparams: { p: 1 }\nsteps:\n  - set: { Body.level: \"${p}\" }\n", inspectCtx);
        CHECK(!inspCaseless.has_cases);
        CHECK(inspCaseless.cases.empty());
        CHECK(!inspCaseless.params_incomplete);
        CHECK(inspCaseless.param_names == std::vector<std::string>({"p"}));
        CHECK(inspCaseless.findings.empty());

        const ScenarioInspection inspOnlyOk =
            inspect_scenario("vehicle_sil_scenario: 1\nonly: [medium]\nsteps:\n  - set: { Body.level: 1 }\n", inspectCtx);
        CHECK(inspOnlyOk.findings.empty());

        const ScenarioInspection inspOnlyBad =
            inspect_scenario("vehicle_sil_scenario: 1\nonly: [bogus]\nsteps:\n  - set: { Body.level: 1 }\n", inspectCtx);
        CHECK(std::any_of(inspOnlyBad.findings.begin(), inspOnlyBad.findings.end(),
                          [](const Finding& f) { return f.severity == Severity::Error && f.code == "scenario-variant"; }));

        const ScenarioInspection inspBrokenCases =
            inspect_scenario("vehicle_sil_scenario: 1\ncases: {}\nsteps:\n  - set: { Body.level: 1 }\n", inspectCtx);
        CHECK(inspBrokenCases.has_cases);
        CHECK(inspBrokenCases.cases.empty());
        CHECK(inspBrokenCases.params_incomplete);

        const ScenarioInspection inspSyntax = inspect_scenario("vehicle_sil_scenario: 1\nsteps:\n  - set: [1\n", inspectCtx);
        CHECK(!inspSyntax.has_cases);
        CHECK(inspSyntax.params_incomplete);
    }
}

TEST_CASE(scenario_tier5_adversarial_template_boundaries_and_dithers)
{
    std::map<std::string, std::string> files;
    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/s.yaml";
    ctx.read_template = [&](const std::string& p, std::string& prob) -> std::optional<std::string> {
        if (const auto it = files.find(p); it != files.end())
        {
            return it->second;
        }
        prob = "does not exist";
        return std::nullopt;
    };
    ctx.read_file = ctx.read_template;
    const auto run_check = [&](const std::string& yaml, ScenarioContext custom) { return read_scenario(yaml, catalog(), custom); };

    // Gap 1: Repeated broken template call emits 1 syntax finding (no duplicate/crash) and keeps params_incomplete == true;
    // plus malformed template AST variants (NUL, UTF-16 BOM, comment-only, sequence root, anchor/alias, multi-doc ---).
    files["templates/bad_syntax.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Body.level: [1 }\n";
    {
        ScenarioContext c2 = ctx;
        c2.params = {ParamValue{"cli_param", "50", false, ParamSource::CommandLine, 0, true}};
        const ScenarioRead rep = run_check(
            "vehicle_sil_scenario: 1\nsteps:\n"
            "  - template: templates/bad_syntax.yaml\n    params: { throttle: \"${cli_param}\" }\n"
            "  - template: templates/bad_syntax.yaml\n    params: { throttle: \"${cli_param}\" }\n",
            c2);
        CHECK(rep.params_incomplete);
        CHECK(rep.usage_errors.empty());
        const auto syntaxCount = std::count_if(rep.findings.begin(), rep.findings.end(),
                                               [](const Finding& f) { return f.code == "syntax"; });
        CHECK_EQ(syntaxCount, 1);
    }
    files["templates/nul.yaml"] = std::string("vehicle_sil_scenario_template: 1\nsteps:\0\n", 42);
    CHECK(lines(run_check(kHead + "  - template: templates/nul.yaml\n", ctx).findings).find("encoding:") != std::string::npos);

    files["templates/utf16.yaml"] = std::string("\xFF\xFEvehicle_sil_scenario_template: 1\n", 35);
    CHECK(lines(run_check(kHead + "  - template: templates/utf16.yaml\n", ctx).findings).find("encoding:") != std::string::npos);

    files["templates/comment_only.yaml"] = "# just a comment\n";
    CHECK(lines(run_check(kHead + "  - template: templates/comment_only.yaml\n", ctx).findings).find("not-a-template:") !=
          std::string::npos);

    files["templates/list_root.yaml"] = "- set: { Body.level: 1 }\n";
    CHECK(lines(run_check(kHead + "  - template: templates/list_root.yaml\n", ctx).findings).find("not-a-template:") !=
          std::string::npos);

    files["templates/anchor.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - set: &a { Body.level: 1 }\n"
        "  - set: *a\n";
    CHECK(lines(run_check(kHead + "  - template: templates/anchor.yaml\n", ctx).findings).find("unsupported-yaml:") !=
          std::string::npos);

    files["templates/multi_doc.yaml"] =
        "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Body.level: 1 }\n---\nvehicle_sil_scenario_template: 1\n";
    CHECK(lines(run_check(kHead + "  - template: templates/multi_doc.yaml\n", ctx).findings).find("unsupported-yaml:") !=
          std::string::npos);

    // Gap 2: Exact boundaries kMaxTemplateDepth (16 pass vs 17 fail), kMaxExpandedSteps (10,000 pass vs 10,001 fail),
    // ./self.yaml cycle normalization, and root templates/../templates/leaf.yaml rejection.
    for (int d = 1; d <= 17; ++d)
    {
        const std::string next = d < 17 ? "  - template: depth_" + std::to_string(d + 1) + ".yaml\n" : "  - set: { Body.level: 1 }\n";
        files["templates/depth_" + std::to_string(d) + ".yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n" + next;
    }
    {
        const ScenarioRead depth16 = run_check(kHead + "  - template: templates/depth_2.yaml\n", ctx);
        CHECK(depth16.scenario.has_value());
        CHECK(lines(depth16.findings).empty());
        CHECK_EQ(depth16.scenario->steps.size(), std::size_t{1});

        const ScenarioRead depth17 = run_check(kHead + "  - template: templates/depth_1.yaml\n", ctx);
        CHECK(!depth17.scenario.has_value());
        CHECK(lines(depth17.findings).find("scenario-template-depth:") != std::string::npos);
    }
    {
        std::string hundredSteps = "vehicle_sil_scenario_template: 1\nsteps:\n";
        for (int s = 0; s < 100; ++s)
        {
            hundredSteps += "  - set: { Body.level: 1 }\n";
        }
        files["templates/hundred.yaml"] = hundredSteps;
        files["templates/one.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Body.level: 1 }\n";
        std::string tenK = kHead;
        for (int c = 0; c < 100; ++c)
        {
            tenK += "  - template: templates/hundred.yaml\n";
        }
        const ScenarioRead pass10k = run_check(tenK, ctx);
        CHECK(pass10k.scenario.has_value());
        CHECK_EQ(pass10k.scenario->steps.size(), std::size_t{10000});

        const ScenarioRead fail10001 = run_check(tenK + "  - template: templates/one.yaml\n", ctx);
        CHECK(!fail10001.scenario.has_value());
        CHECK(lines(fail10001.findings).find("scenario-template-steps:") != std::string::npos);
    }
    files["templates/self_dot.yaml"] = "vehicle_sil_scenario_template: 1\nsteps:\n  - template: ./self_dot.yaml\n";
    CHECK(lines(run_check(kHead + "  - template: templates/self_dot.yaml\n", ctx).findings).find("scenario-template-cycle:") !=
          std::string::npos);
    CHECK(lines(run_check(kHead + "  - template: templates/../templates/one.yaml\n", ctx).findings)
              .find("scenario-template-file:") != std::string::npos);

    // Gap 3: InheritedTiming mixed at_ms: 100 -> after_ms: 20 -> at_ms: 110 rejection,
    // and nested wait_until wait_file_ attribution.
    files["templates/mixed_time.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - after_ms: 20\n"
        "    set: { Body.level: 1 }\n"
        "  - at_ms: 110\n"
        "    set: { Body.level: 2 }\n";
    {
        const std::string errs = lines(run_check(kHead + "  - at_ms: 100\n    template: templates/mixed_time.yaml\n", ctx).findings);
        CHECK(errs.find("scenario-time:") != std::string::npos);
        CHECK(errs.find("at_ms is 110, before the end of the step before it (120 ms)") != std::string::npos);
        CHECK(errs.find("inside a template, at_ms is still the scenario's absolute time") != std::string::npos);
    }
    files["templates/wait_outer.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - wait_until: { Engine.speed: 50 }\n"
        "    timeout_ms: 100\n"
        "  - template: wait_inner.yaml\n";
    files["templates/wait_inner.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - at_ms: 10\n"
        "    set: { Body.level: 1 }\n";
    {
        const std::string errs = lines(run_check(kHead + "  - template: templates/wait_outer.yaml\n", ctx).findings);
        CHECK(errs.find("scenario-time:") != std::string::npos);
        CHECK(errs.find("scenarios/templates/wait_outer.yaml:3") != std::string::npos);
    }

    // Gap 4: skipped_wave_ms() with ${param} in points.file inside an absent: true template step + 0-row CSV safe handling.
    files["traces/ok.csv"] = "time,level\n0,10\n50,20\n";
    files["traces/empty_rows.csv"] = "time,level\n";
    files["templates/skip_csv.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  f: { type: string }\n"
        "  end_t: { type: integer }\n"
        "steps:\n"
        "  - wave: { Door.absent: { points: { file: \"${f}\", value: level } } }\n"
        "  - at_ms: \"${end_t}\"\n"
        "    expect: { Body.level: 0 }\n";
    {
        ScenarioCatalog skipCat = catalog();
        skipCat[1].absent.push_back("absent");
        ScenarioContext skipCtx = ctx;
        skipCtx.variant = "small";
        const std::string errs = lines(read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/skip_csv.yaml\n"
            "    params: { f: ../traces/ok.csv, end_t: 40 }\n",
            skipCat, skipCtx).findings);
        CHECK(errs.find("scenario-time:") != std::string::npos);
        CHECK(errs.find("50 ms") != std::string::npos);

        const ScenarioRead okEmpty = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/skip_csv.yaml\n"
            "    params: { f: ../traces/empty_rows.csv, end_t: 40 }\n",
            skipCat, skipCtx);
        CHECK(okEmpty.scenario.has_value());
        CHECK(!okEmpty.scenario->steps[0].skipped.empty());
        CHECK_EQ(okEmpty.scenario->steps[0].skipped_ms, std::int64_t{0});
    }

    // Gap 5: from_cli -> outer type: string -> nested type: integer non-coercion (quoted = true preserved),
    // and param_values_equal() numeric/hex equivalence (1e0 vs values: [1.0], 16 vs values: [0x10]).
    files["templates/need_int.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n  n: { type: integer }\n"
        "steps:\n  - set: { Body.level: \"${n}\" }\n";
    files["templates/str_to_int.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n  s: { type: string }\n"
        "steps:\n"
        "  - template: need_int.yaml\n"
        "    params: { n: \"${s}\" }\n";
    {
        ScenarioContext cliCtx = ctx;
        cliCtx.params = {ParamValue{"cli_num", "1", false, ParamSource::CommandLine, 0, true}};
        const ScenarioRead rejected = run_check(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/str_to_int.yaml\n"
            "    params: { s: \"${cli_num}\" }\n",
            cliCtx);
        CHECK(!rejected.scenario.has_value());
        CHECK(lines(rejected.findings).find("write the integer without quotes") != std::string::npos);
    }
    files["templates/values_eq.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  n: { type: number, values: [1.0, 2.5] }\n"
        "  i: { type: integer, values: [0x10] }\n"
        "steps:\n  - set: { Body.ratio: \"${n}\", Body.level: \"${i}\" }\n";
    {
        const ScenarioRead eqOk = run_check(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/values_eq.yaml\n    params: { n: 1e0, i: 16 }\n",
            ctx);
        CHECK(eqOk.scenario.has_value());
        CHECK(lines(eqOk.findings).empty());
    }

    // Gap 7 + Bug A-3: check_dithers() multi-call template (file, line) formatting.
    files["templates/dither_dup.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n  v: { type: integer }\n"
        "steps:\n  - set: { Body.level: \"${v}\" }\n";
    files["templates/dither_other.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n  v: { type: integer }\n"
        "steps:\n  - set: { Body.level: \"${v}\" }\n";
    {
        const std::string ditherInput =
            "inputs:\n  Body.level: { dither: { square: { amplitude: 5, period_ms: 80 } } }\nsteps:\n";
        const ScenarioRead sameLine = run_check(
            kDitherHead + ditherInput +
                "  - template: templates/dither_dup.yaml\n    params: { v: 98 }\n"
                "  - template: templates/dither_dup.yaml\n    params: { v: 99 }\n",
            ctx);
        const auto it = std::find_if(sameLine.findings.begin(), sameLine.findings.end(),
                                     [](const Finding& f) { return f.code == "scenario-dither-range"; });
        CHECK(it != sameLine.findings.end());
        CHECK(it->message.find("on the 98 of scenarios/templates/dither_dup.yaml:5") != std::string::npos);
        CHECK(it->message.find("; so on 1 other value.") != std::string::npos);
        CHECK(it->message.find("(line scenarios/templates/dither_dup.yaml:5)") == std::string::npos);

        const ScenarioRead diffFile = run_check(
            kDitherHead + ditherInput +
                "  - template: templates/dither_dup.yaml\n    params: { v: 98 }\n"
                "  - template: templates/dither_other.yaml\n    params: { v: 99 }\n",
            ctx);
        const auto it2 = std::find_if(diffFile.findings.begin(), diffFile.findings.end(),
                                      [](const Finding& f) { return f.code == "scenario-dither-range"; });
        CHECK(it2 != diffFile.findings.end());
        CHECK(it2->message.find("; so on 1 other value (scenarios/templates/dither_other.yaml:5).") != std::string::npos);
        CHECK(it2->message.find("(line scenarios/templates/") == std::string::npos);
    }
}

TEST_CASE(scenario_invalid_cached_template_reports_error_at_every_call_site)
{
    std::map<std::string, std::string> files;
    files["templates/bad_syntax.yaml"] = "vehicle_sil_scenario_template: 1\nsteps: [unclosed\n";
    files["templates/bad_schema.yaml"] = "vehicle_sil_scenario_template: 1\nsteps: []\n";
    int readsSyntax = 0;
    int readsSchema = 0;

    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/main.yaml";
    ctx.read_template = [&](const std::string& path, std::string& problem) -> std::optional<std::string> {
        if (path == "templates/bad_syntax.yaml")
        {
            ++readsSyntax;
        }
        else if (path == "templates/bad_schema.yaml")
        {
            ++readsSchema;
        }
        const auto it = files.find(path);
        if (it == files.end())
        {
            problem = "not found";
            return std::nullopt;
        }
        return it->second;
    };

    const std::string syntaxScenario =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - template: templates/bad_syntax.yaml\n"
        "  - template: templates/bad_syntax.yaml\n";
    const ScenarioRead r1 = read_scenario(syntaxScenario, catalog(), ctx);
    CHECK(!r1.scenario.has_value());
    CHECK_EQ(readsSyntax, 1);
    CHECK_EQ(r1.findings.size(), std::size_t{2});
    CHECK(r1.findings[0].code == "syntax");
    CHECK(r1.findings[0].file == "scenarios/templates/bad_syntax.yaml");
    CHECK(r1.findings[1].code == "scenario-template-file");
    CHECK(r1.findings[1].file.empty());
    CHECK_EQ(r1.findings[1].line, std::size_t{4});
    CHECK(r1.findings[1].message.find("templates/bad_syntax.yaml") != std::string::npos);
    CHECK(r1.findings[1].message.find("syntax") != std::string::npos);

    const std::string schemaScenario =
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - template: templates/bad_schema.yaml\n"
        "  - template: templates/bad_schema.yaml\n"
        "  - template: templates/bad_schema.yaml\n";
    const ScenarioRead r2 = read_scenario(schemaScenario, catalog(), ctx);
    CHECK(!r2.scenario.has_value());
    CHECK_EQ(readsSchema, 1);
    CHECK_EQ(r2.findings.size(), std::size_t{3});
    CHECK(r2.findings[0].code == "value");
    CHECK(r2.findings[0].file == "scenarios/templates/bad_schema.yaml");
    CHECK(r2.findings[1].code == "scenario-template-file");
    CHECK_EQ(r2.findings[1].line, std::size_t{4});
    CHECK(r2.findings[2].code == "scenario-template-file");
    CHECK_EQ(r2.findings[2].line, std::size_t{5});
}

TEST_CASE(scenario_template_paths_normalize_windows_backslashes)
{
    ScenarioContext ctx;
    ctx.scenario_file = "scenarios\\nested\\main.yaml";
    ctx.read_template = [](const std::string& path, std::string& problem) -> std::optional<std::string> {
        if (path == "templates/step.yaml")
        {
            return "vehicle_sil_scenario_template: 1\nsteps:\n  - set: { Body.level: 10 }\n";
        }
        problem = "missing";
        return std::nullopt;
    };
    const ScenarioRead r = read_scenario(
        "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/step.yaml\n", catalog(), ctx);
    CHECK(r.scenario.has_value());
    CHECK_EQ(r.scenario->steps.size(), std::size_t{1});
    CHECK(r.scenario->steps[0].file == "scenarios/nested/templates/step.yaml");
    CHECK(r.scenario->steps[0].file.find('\\') == std::string::npos);
}

TEST_CASE(scenario_parameter_type_lattice_metadata_conflicts_and_cli_string_quoting)
{
    std::map<std::string, std::string> files;
    files["templates/num.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  x:\n"
        "    type: number\n"
        "    values: [10, 20, 30]\n"
        "    description: Number constraint\n"
        "steps:\n"
        "  - expect: { Body.body.speed: { ge: \"${x}\" } }\n";
    files["templates/int.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  y:\n"
        "    type: integer\n"
        "    values: [0x14, 30, 40]\n"
        "    description: Integer constraint\n"
        "steps:\n"
        "  - set: { Body.level: \"${y}\" }\n";
    files["templates/disjoint.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  z:\n"
        "    type: integer\n"
        "    values: [50, 60]\n"
        "steps:\n"
        "  - set: { Body.level: \"${z}\" }\n";
    files["templates/str.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  tag:\n"
        "    type: string\n"
        "    description: Tag label\n"
        "steps:\n"
        "  - name: \"Tag ${tag}\"\n"
        "    set: { Body.level: 10 }\n";
    files["templates/bool.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  flag:\n"
        "    type: boolean\n"
        "    description: Enable flag\n"
        "steps:\n"
        "  - set: { Door.lamp: \"${flag}\" }\n";
    files["templates/outer_forward.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  p:\n"
        "    type: number\n"
        "    description: Outer description\n"
        "steps:\n"
        "  - template: int.yaml\n"
        "    params: { y: \"${p}\" }\n";

    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/main.yaml";
    ctx.read_template = [&](const std::string& path, std::string& problem) -> std::optional<std::string> {
        const auto it = files.find(path);
        if (it == files.end())
        {
            problem = "missing";
            return std::nullopt;
        }
        return it->second;
    };

    // 1. Direct untyped root step produces no inferred param_meta.
    {
        const ScenarioRead direct = read_scenario(
            "vehicle_sil_scenario: 1\nparams: { lvl: 20 }\nsteps:\n  - set: { Body.level: \"${lvl}\" }\n",
            catalog(), ctx);
        CHECK(direct.scenario.has_value());
        CHECK(direct.param_meta.empty());
    }

    // 2. number + integer narrows to integer, intersects values ([20, 30]), preserves first description,
    //    and string interpolation in step name adds no type constraint.
    {
        const ScenarioRead narrowed = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: 20 }\n"
            "steps:\n"
            "  - name: \"Run ${p}\"\n"
            "    template: templates/num.yaml\n"
            "    params: { x: \"${p}\" }\n"
            "  - template: templates/int.yaml\n"
            "    params: { y: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(narrowed.scenario.has_value());
        CHECK_EQ(narrowed.param_meta.size(), std::size_t{1});
        CHECK(narrowed.param_meta[0].name == "p");
        CHECK(narrowed.param_meta[0].type == "integer");
        CHECK((narrowed.param_meta[0].values == std::vector<std::string>{"20", "30"}));
        CHECK(narrowed.param_meta[0].description == "Number constraint");
    }

    // 3. Transitive narrowing through nested templates (outer number -> inner integer) canonicalizes hex values to decimal.
    {
        const ScenarioRead nested = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: 30 }\n"
            "steps:\n"
            "  - template: templates/outer_forward.yaml\n"
            "    params: { p: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(nested.scenario.has_value());
        CHECK_EQ(nested.param_meta.size(), std::size_t{1});
        CHECK(nested.param_meta[0].type == "integer");
        CHECK((nested.param_meta[0].values == std::vector<std::string>{"20", "30", "40"}));
        CHECK(nested.param_meta[0].description == "Outer description");
    }

    // 4. Empty values intersection is a static scenario-param error.
    {
        const ScenarioRead emptyInter = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: 20 }\n"
            "steps:\n"
            "  - template: templates/int.yaml\n"
            "    params: { y: \"${p}\" }\n"
            "  - template: templates/disjoint.yaml\n"
            "    params: { z: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(!emptyInter.scenario.has_value());
        CHECK(lines(emptyInter.findings).find("scenario-param:") != std::string::npos);
        CHECK(lines(emptyInter.findings).find("intersection is empty") != std::string::npos);
    }

    // 5. Incompatible types: string + numeric (both template+template and template+direct step with from_cli),
    //    and boolean + numeric -> static scenario-param error.
    {
        ScenarioContext cliCtx = ctx;
        cliCtx.params = {ParamValue{"x", "10", false, ParamSource::CommandLine, 0, true}};
        const ScenarioRead strAndDirect = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - set: { Body.level: \"${x}\" }\n"
            "  - template: templates/str.yaml\n"
            "    params: { tag: \"${x}\" }\n",
            catalog(), cliCtx);
        CHECK(!strAndDirect.scenario.has_value());
        CHECK(lines(strAndDirect.findings).find("incompatible types") != std::string::npos);

        const ScenarioRead strAndBool = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - template: templates/bool.yaml\n"
            "    params: { flag: \"${x}\" }\n"
            "  - template: templates/str.yaml\n"
            "    params: { tag: \"${x}\" }\n",
            catalog(), cliCtx);
        CHECK(!strAndBool.scenario.has_value());
        CHECK(lines(strAndBool.findings).find("incompatible types") != std::string::npos);

        const ScenarioRead boolAndInt = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - template: templates/bool.yaml\n"
            "    params: { flag: \"${x}\" }\n"
            "  - template: templates/int.yaml\n"
            "    params: { y: \"${x}\" }\n",
            catalog(), cliCtx);
        CHECK(!boolAndInt.scenario.has_value());
        CHECK(lines(boolAndInt.findings).find("incompatible types") != std::string::npos);
    }

    // 6. CLI --param p=123 bound only to type: string sets quoted = true on out.params and scenario.params.
    {
        ScenarioContext cliCtx = ctx;
        cliCtx.params = {ParamValue{"p", "123", false, ParamSource::CommandLine, 0, true}};
        const ScenarioRead strOnly = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - name: \"Label ${p}\"\n"
            "    template: templates/str.yaml\n"
            "    params: { tag: \"${p}\" }\n",
            catalog(), cliCtx);
        CHECK(strOnly.scenario.has_value());
        CHECK_EQ(strOnly.params.size(), std::size_t{1});
        CHECK(strOnly.params[0].text == "123");
        CHECK(strOnly.params[0].quoted == true);
        CHECK(strOnly.scenario->params[0].quoted == true);
        CHECK_EQ(strOnly.param_meta.size(), std::size_t{1});
        CHECK(strOnly.param_meta[0].type == "string");
    }

    // 7. Narrowing number values containing float/scientific whole numbers (16.0, 1e1) with unconstrained integer
    //    succeeds in both call orders and canonicalizes values to decimal integers.
    {
        files["templates/num_float_whole.yaml"] =
            "vehicle_sil_scenario_template: 1\n"
            "parameters:\n"
            "  y:\n"
            "    type: number\n"
            "    values: [1.5, 16.0, 1e1]\n"
            "steps:\n"
            "  - expect: { Body.body.speed: { ge: \"${y}\" } }\n";
        files["templates/int_plain.yaml"] =
            "vehicle_sil_scenario_template: 1\n"
            "parameters:\n"
            "  x:\n"
            "    type: integer\n"
            "steps:\n"
            "  - set: { Body.level: \"${x}\" }\n";

        const ScenarioRead numFirst = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: 16 }\n"
            "steps:\n"
            "  - template: templates/num_float_whole.yaml\n"
            "    params: { y: \"${p}\" }\n"
            "  - template: templates/int_plain.yaml\n"
            "    params: { x: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(numFirst.scenario.has_value());
        CHECK_EQ(numFirst.param_meta.size(), std::size_t{1});
        CHECK(numFirst.param_meta[0].type == "integer");
        CHECK((numFirst.param_meta[0].values == std::vector<std::string>{"16", "10"}));

        const ScenarioRead intFirst = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: 10 }\n"
            "steps:\n"
            "  - template: templates/int_plain.yaml\n"
            "    params: { x: \"${p}\" }\n"
            "  - template: templates/num_float_whole.yaml\n"
            "    params: { y: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(intFirst.scenario.has_value());
        CHECK_EQ(intFirst.param_meta.size(), std::size_t{1});
        CHECK(intFirst.param_meta[0].type == "integer");
        CHECK((intFirst.param_meta[0].values == std::vector<std::string>{"16", "10"}));
    }

    // 8. Integer hex values combined with type: number canonicalize to decimal regardless of call order,
    //    and every advertised value is accepted when supplied.
    {
        files["templates/num_plain.yaml"] =
            "vehicle_sil_scenario_template: 1\n"
            "parameters:\n"
            "  x: { type: number }\n"
            "steps:\n"
            "  - expect: { Body.body.speed: { ge: \"${x}\" } }\n";

        const ScenarioRead intThenNumVals = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: 20 }\n"
            "steps:\n"
            "  - template: templates/int.yaml\n"
            "    params: { y: \"${p}\" }\n"
            "  - template: templates/num.yaml\n"
            "    params: { x: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(intThenNumVals.scenario.has_value());
        CHECK_EQ(intThenNumVals.param_meta.size(), std::size_t{1});
        CHECK(intThenNumVals.param_meta[0].type == "integer");
        CHECK((intThenNumVals.param_meta[0].values == std::vector<std::string>{"20", "30"}));

        const ScenarioRead intThenNumPlain = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: 20 }\n"
            "steps:\n"
            "  - template: templates/int.yaml\n"
            "    params: { y: \"${p}\" }\n"
            "  - template: templates/num_plain.yaml\n"
            "    params: { x: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(intThenNumPlain.scenario.has_value());
        CHECK_EQ(intThenNumPlain.param_meta.size(), std::size_t{1});
        CHECK((intThenNumPlain.param_meta[0].values == std::vector<std::string>{"20", "30", "40"}));
    }

    // 9. CLI --param p=123 bound to type: string and a numeric-looking enum item ("123") succeeds in both
    //    step orders and preserves quoted string output, matching YAML params: { p: "123" }.
    {
        ScenarioCatalog weirdCat = catalog();
        control::VariableInfo weirdMode;
        weirdMode.name = "weird_mode";
        weirdMode.type = control::ValueType::Enum;
        weirdMode.causality = control::Causality::Input;
        weirdMode.items = {{"off", 0}, {"123", 1}, {"true", 2}};
        weirdCat[0].variables.push_back(std::move(weirdMode));

        ScenarioContext cliCtx = ctx;
        cliCtx.params = {ParamValue{"p", "123", false, ParamSource::CommandLine, 0, true}};

        const ScenarioRead tplFirst = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: \"off\" }\n"
            "steps:\n"
            "  - template: templates/str.yaml\n"
            "    params: { tag: \"${p}\" }\n"
            "  - set: { Body.weird_mode: \"${p}\" }\n",
            weirdCat, cliCtx);
        CHECK(tplFirst.scenario.has_value());
        CHECK_EQ(tplFirst.params.size(), std::size_t{1});
        CHECK(tplFirst.params[0].text == "123" && tplFirst.params[0].quoted == true);
        CHECK_EQ(tplFirst.param_meta.size(), std::size_t{1});
        CHECK(tplFirst.param_meta[0].type == "string");
        CHECK(std::get<std::int64_t>(tplFirst.scenario->steps[1].writes[0].value) == 1);

        const ScenarioRead setFirst = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: \"off\" }\n"
            "steps:\n"
            "  - set: { Body.weird_mode: \"${p}\" }\n"
            "  - template: templates/str.yaml\n"
            "    params: { tag: \"${p}\" }\n",
            weirdCat, cliCtx);
        CHECK(setFirst.scenario.has_value());
        CHECK_EQ(setFirst.params.size(), std::size_t{1});
        CHECK(setFirst.params[0].text == "123" && setFirst.params[0].quoted == true);
        CHECK_EQ(setFirst.param_meta.size(), std::size_t{1});
        CHECK(setFirst.param_meta[0].type == "string");
        CHECK(std::get<std::int64_t>(setFirst.scenario->steps[0].writes[0].value) == 1);
    }

    // 10. Conflicted parameter types omit first-wins type/values metadata in both full check and
    //     unselected-case lightweight metadata scan.
    {
        const ScenarioRead fullConflict = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { p: \"20\" }\n"
            "steps:\n"
            "  - template: templates/str.yaml\n"
            "    params: { tag: \"${p}\" }\n"
            "  - template: templates/int.yaml\n"
            "    params: { y: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(!fullConflict.scenario.has_value());
        CHECK(fullConflict.param_meta.empty());

        const ScenarioRead unselectedConflict = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "cases:\n"
            "  c1: { params: { p: 20 } }\n"
            "steps:\n"
            "  - template: templates/str.yaml\n"
            "    params: { tag: \"${p}\" }\n"
            "  - template: templates/int.yaml\n"
            "    params: { y: \"${p}\" }\n",
            catalog(), ctx);
        CHECK(!unselectedConflict.scenario.has_value());
        CHECK(unselectedConflict.param_meta.empty());
    }
}

TEST_CASE(scenario_unselected_case_metadata_scan_and_shared_inspect_cache)
{
    int templateReads = 0;
    const std::string tplText =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  lvl:\n"
        "    type: integer\n"
        "    values: [10, 50, 80]\n"
        "    description: Target body level\n"
        "  wait_ms:\n"
        "    type: integer\n"
        "    default: 100\n"
        "    description: Optional wait\n"
        "  extra_req:\n"
        "    type: string\n"
        "steps:\n"
        "  - set: { Body.level: \"${lvl}\" }\n"
        "    after_ms: \"${wait_ms}\"\n";

    ScenarioContext ctx;
    ctx.variants = {"small", "medium", "large"};
    ctx.variant = "small";
    ctx.scenario_file = "scenarios/multi.yaml";
    ctx.read_template = [&](const std::string& path, std::string& problem) -> std::optional<std::string> {
        if (path == "templates/step.yaml")
        {
            ++templateReads;
            return tplText;
        }
        problem = "missing";
        return std::nullopt;
    };

    const std::string scenarioText =
        "vehicle_sil_scenario: 1\n"
        "only: [small, medium]\n"
        "params: { base_p: 10 }\n"
        "cases:\n"
        "  c_all: { params: { lvl: 10, extra: \"a\" } }\n"
        "  c_small: { only: [small], params: { lvl: 50, extra: \"b\" } }\n"
        "  c_disjoint: { only: [large], params: { lvl: 80, extra: \"c\" } }\n"
        "steps:\n"
        "  - set: { Body.level: \"${base_p}\" }\n"
        "  - template: templates/step.yaml\n"
        "    params:\n"
        "      lvl: \"${lvl}\"\n"
        "      extra_req: \"${extra}\"\n";

    // Unselected case: lightweight scan collects case_details, root params, param_meta, and missing_params
    // without mixing in any case's defaults and without reporting template default wait_ms as missing.
    {
        const ScenarioRead unselected = read_scenario(scenarioText, catalog(), ctx);
        CHECK(!unselected.scenario.has_value());
        CHECK(!unselected.usage_errors.empty());
        CHECK_EQ(unselected.case_details.size(), std::size_t{3});
        CHECK(unselected.case_details[0].name == "c_all");
        CHECK(unselected.case_details[0].only == std::optional<std::vector<std::string>>({"small", "medium"}));
        CHECK(unselected.case_details[1].name == "c_small");
        CHECK(unselected.case_details[1].only == std::optional<std::vector<std::string>>({"small"}));
        CHECK(unselected.case_details[2].name == "c_disjoint");
        CHECK(unselected.case_details[2].only.has_value() && unselected.case_details[2].only->empty());
        // Only root param base_p is in params (no case params leaked).
        CHECK_EQ(unselected.params.size(), std::size_t{1});
        CHECK(unselected.params[0].name == "base_p");
        // Missing root refs: lvl, extra (not wait_ms which has a template default).
        CHECK((unselected.missing_params == std::vector<std::string>{"lvl", "extra"}));
        // Template param metadata collected even before case selection.
        CHECK_EQ(unselected.param_meta.size(), std::size_t{2});
        CHECK(unselected.param_meta[0].name == "lvl");
        CHECK(unselected.param_meta[0].type == "integer");
        CHECK((unselected.param_meta[0].values == std::vector<std::string>{"10", "50", "80"}));
        CHECK(unselected.param_meta[0].description == "Target body level");
        CHECK(unselected.param_meta[1].name == "extra");
        CHECK(unselected.param_meta[1].type == "string");
    }

    // Selected case: exact effective values for c_small only, no missing_params.
    {
        ScenarioContext selCtx = ctx;
        selCtx.case_name = "c_small";
        const ScenarioRead selected = read_scenario(scenarioText, catalog(), selCtx);
        CHECK(selected.scenario.has_value());
        CHECK(selected.missing_params.empty());
        CHECK_EQ(selected.params.size(), std::size_t{3});
        CHECK(selected.params[1].name == "lvl" && selected.params[1].text == "50");
        CHECK(selected.params[2].name == "extra" && selected.params[2].text == "b");
    }

    // Omitted required template parameter (no default) is reported in missing_params; template default is not.
    {
        const std::string omittedText =
            "vehicle_sil_scenario: 1\n"
            "cases:\n  c1: {}\n"
            "steps:\n"
            "  - template: templates/step.yaml\n"
            "    params: { lvl: 10 }\n";
        const ScenarioRead omitted = read_scenario(omittedText, catalog(), ctx);
        CHECK((omitted.missing_params == std::vector<std::string>{"extra_req"}));
    }

    // inspect_scenario shares template cache across all cases (template read only once across 3 cases)
    // and records per-case param_names in case_inspections.
    templateReads = 0;
    const ScenarioInspection insp = inspect_scenario(scenarioText, ctx);
    CHECK(insp.has_cases);
    CHECK_EQ(templateReads, 1);
    CHECK_EQ(insp.case_inspections.size(), std::size_t{3});
    CHECK(insp.case_inspections[0].name == "c_all");
    CHECK(!insp.case_inspections[0].params_incomplete);
    CHECK(std::find(insp.case_inspections[0].param_names.begin(), insp.case_inspections[0].param_names.end(), "lvl") !=
          insp.case_inspections[0].param_names.end());
}

TEST_CASE(scenario_template_multiple_calls_isolation_timing_hex_and_quoting)
{
    std::map<std::string, std::string> files;
    files["templates/set_lvl.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  target: { type: integer }\n"
        "steps:\n"
        "  - set: { Body.level: \"${target}\" }\n";
    files["templates/timed.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - set: { Body.level: 40 }\n"
        "  - after_ms: 20\n"
        "    set: { Body.level: 60 }\n";
    files["templates/default_wait.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  timeout_ms: { type: integer, default: 200 }\n"
        "steps:\n"
        "  - wait_until: { Body.body.speed: { ge: 100 } }\n"
        "    timeout_ms: \"${timeout_ms}\"\n";
    files["templates/bad_wave.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "steps:\n"
        "  - wave:\n"
        "      Body.level: { square: { low: 0, high: 999, period_ms: 40 }, for_ms: 80 }\n";
    files["templates/typed_args.yaml"] =
        "vehicle_sil_scenario_template: 1\n"
        "parameters:\n"
        "  i: { type: integer }\n"
        "  n: { type: number }\n"
        "  b: { type: boolean }\n"
        "steps:\n"
        "  - set: { Body.level: \"${i}\", Body.ratio: \"${n}\", Door.lamp: \"${b}\" }\n";

    ScenarioContext ctx;
    ctx.scenario_file = "scenarios/main.yaml";
    ctx.read_template = [&](const std::string& path, std::string& problem) -> std::optional<std::string> {
        const auto it = files.find(path);
        if (it == files.end())
        {
            problem = "missing";
            return std::nullopt;
        }
        return it->second;
    };

    // 1. Multiple invocations of the same template with different arguments do not interfere.
    {
        const ScenarioRead r = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - template: templates/set_lvl.yaml\n"
            "    params: { target: 30 }\n"
            "  - after_ms: 20\n"
            "    template: templates/set_lvl.yaml\n"
            "    params: { target: 70 }\n",
            catalog(), ctx);
        CHECK(r.scenario.has_value());
        CHECK_EQ(r.scenario->steps.size(), std::size_t{2});
        CHECK(std::get<std::int64_t>(r.scenario->steps[0].writes[0].value) == 30);
        CHECK(std::get<std::int64_t>(r.scenario->steps[1].writes[0].value) == 70);
    }

    // 2. Inherited timing equivalence between template call with at_ms and inline steps.
    {
        const ScenarioRead tplRead = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - at_ms: 100\n"
            "    template: templates/timed.yaml\n"
            "  - after_ms: 30\n"
            "    set: { Body.level: 80 }\n",
            catalog(), ctx);
        const ScenarioRead inlineRead = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - at_ms: 100\n"
            "    set: { Body.level: 40 }\n"
            "  - after_ms: 20\n"
            "    set: { Body.level: 60 }\n"
            "  - after_ms: 30\n"
            "    set: { Body.level: 80 }\n",
            catalog(), ctx);
        CHECK(tplRead.scenario.has_value() && inlineRead.scenario.has_value());
        World w1, w2;
        const ScenarioRun runTpl = w1.run(*tplRead.scenario);
        const ScenarioRun runInline = w2.run(*inlineRead.scenario);
        CHECK(runTpl.over_at == runInline.over_at);
        CHECK_EQ(runTpl.steps.size(), runInline.steps.size());
        for (std::size_t i = 0; i < runTpl.steps.size(); ++i)
        {
            CHECK(runTpl.steps[i].start == runInline.steps[i].start);
            CHECK(runTpl.steps[i].at == runInline.steps[i].at);
            CHECK(runTpl.steps[i].end == runInline.steps[i].end);
        }
    }

    // 3. Strict lexical isolation: outer params do not override template default unless passed explicitly.
    {
        const ScenarioRead r = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { timeout_ms: 999 }\n"
            "steps:\n"
            "  - template: templates/default_wait.yaml\n"
            "  - template: templates/default_wait.yaml\n"
            "    params: { timeout_ms: \"${timeout_ms}\" }\n",
            catalog(), ctx);
        CHECK(r.scenario.has_value());
        CHECK_EQ(r.scenario->steps.size(), std::size_t{2});
        CHECK(r.scenario->steps[0].within_ms == std::optional<std::int64_t>(200));
        CHECK(r.scenario->steps[1].within_ms == std::optional<std::int64_t>(999));
    }

    // 4. Mixed direct -> template -> direct steps keep provenance and cumulative times (10, 40, 60).
    {
        const ScenarioRead r = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - at_ms: 10\n"
            "    set: { Body.level: 10 }\n"
            "  - after_ms: 30\n"
            "    template: templates/set_lvl.yaml\n"
            "    params: { target: 40 }\n"
            "  - after_ms: 20\n"
            "    set: { Body.level: 60 }\n",
            catalog(), ctx);
        CHECK(r.scenario.has_value());
        CHECK_EQ(r.scenario->steps.size(), std::size_t{3});
        CHECK(r.scenario->steps[0].file.empty() && r.scenario->steps[0].call_stack.empty());
        CHECK(!r.scenario->steps[1].file.empty() && r.scenario->steps[1].call_stack.size() == 1);
        CHECK(r.scenario->steps[2].file.empty() && r.scenario->steps[2].call_stack.empty());
        World w;
        const ScenarioRun run = w.run(*r.scenario);
        CHECK(run.steps[0].start == std::optional<SimTime>(SimTime{10ms}));
        CHECK(run.steps[1].start == std::optional<SimTime>(SimTime{40ms}));
        CHECK(run.steps[2].start == std::optional<SimTime>(SimTime{60ms}));
    }

    // 5. Wave range error inside template attributes Finding::file and line to template.
    {
        ScenarioContext noPrefix = ctx;
        noPrefix.scenario_file = "main.yaml";
        const ScenarioRead r = read_scenario(
            "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/bad_wave.yaml\n", catalog(), noPrefix);
        CHECK(!r.scenario.has_value());
        CHECK_EQ(r.findings.size(), std::size_t{1});
        CHECK(r.findings[0].file == "templates/bad_wave.yaml");
        CHECK_EQ(r.findings[0].line, std::size_t{4});
        CHECK(r.findings[0].code == "scenario-value");
    }

    // 6. Hex integer param accepted for Body.level, rejected for decimal-only after_ms, timeout_ms, and seed.
    {
        const ScenarioRead okHex = read_scenario(
            "vehicle_sil_scenario: 1\nparams: { h: 0x10 }\nsteps:\n  - set: { Body.level: \"${h}\" }\n",
            catalog(), ctx);
        CHECK(okHex.scenario.has_value());
        CHECK(std::get<std::int64_t>(okHex.scenario->steps[0].writes[0].value) == 16);

        const ScenarioRead badAfter = read_scenario(
            "vehicle_sil_scenario: 1\nparams: { h: 0x10 }\nsteps:\n  - after_ms: \"${h}\"\n    set: { Body.level: 1 }\n",
            catalog(), ctx);
        CHECK(!badAfter.scenario.has_value());
        CHECK(lines(badAfter.findings).find("after_ms must be a whole number") != std::string::npos);

        const ScenarioRead badTimeout = read_scenario(
            "vehicle_sil_scenario: 1\nparams: { h: 0x10 }\nsteps:\n  - wait_until: { Body.body.speed: 10 }\n    timeout_ms: \"${h}\"\n",
            catalog(), ctx);
        CHECK(!badTimeout.scenario.has_value());
        CHECK(lines(badTimeout.findings).find("timeout_ms must be a whole number") != std::string::npos);

        const ScenarioRead badSeed = read_scenario(
            "vehicle_sil_scenario: 1\nparams: { h: 0x10 }\nsteps:\n"
            "  - dither: { Body.level: { noise: { amplitude: 1, hold_ms: 10, seed: \"${h}\" } } }\n",
            catalog(), ctx);
        CHECK(!badSeed.scenario.has_value());
        CHECK(lines(badSeed.findings).find("must be a whole number from 0") != std::string::npos);
    }

    // 7. Numeric and boolean template parameters reject quoted literals while accepting "${ref}" forwarding.
    {
        const ScenarioRead badQuoted = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "steps:\n"
            "  - template: templates/typed_args.yaml\n"
            "    params: { i: \"30\", n: \"0.5\", b: \"false\" }\n",
            catalog(), ctx);
        CHECK(!badQuoted.scenario.has_value());
        CHECK(lines(badQuoted.findings).find("write the integer without quotes") != std::string::npos);
        CHECK(lines(badQuoted.findings).find("write the number without quotes") != std::string::npos);
        CHECK(lines(badQuoted.findings).find("write true or false without quotes") != std::string::npos);

        const ScenarioRead okRef = read_scenario(
            "vehicle_sil_scenario: 1\n"
            "params: { ri: 30, rn: 0.5, rb: false }\n"
            "steps:\n"
            "  - template: templates/typed_args.yaml\n"
            "    params: { i: \"${ri}\", n: \"${rn}\", b: \"${rb}\" }\n",
            catalog(), ctx);
        CHECK(okRef.scenario.has_value());
        CHECK(std::get<std::int64_t>(okRef.scenario->steps[0].writes[0].value) == 30);
    }
}

TEST_CASE(scenario_variable_to_variable_comparisons_syntax_and_static_checks)
{
    // All 7 operators, shorthand equality, mixed constant + variable comparisons, tolerance parameter,
    // expect (at-point, within_ms, hold_for_ms, beside ramp/wave), wait_until, and reordered enum items.
    ScenarioCatalog cat = catalog();
    for (auto& v : cat[1].variables)
    {
        if (v.name == "mode")
        {
            v.items = {{"auto", 2}, {"off", 0}, {"on", 1}}; // same (name, value) pairs in another order
        }
    }
    const Scenario s = read(
        "vehicle_sil_scenario: 1\n"
        "params: { tol: 2.5 }\n"
        "steps:\n"
        "  - expect: { Body.level: { variable: Door.level }, Door.mode: { eq: { variable: Body.mode }, ne: { variable: Body.mode } } }\n"
        "  - expect: { Body.level: { ge: { variable: Door.ratio }, le: 100 } }\n"
        "    hold_for_ms: 20\n"
        "  - expect: { Body.ratio: { gt: { variable: Door.level }, lt: { variable: Door.body.speed } } }\n"
        "    within_ms: 30\n"
        "  - expect: { Body.body.speed: { near: { variable: Door.level }, tolerance: \"${tol}\" } }\n"
        "  - wait_until: { Body.lamp: { ne: { variable: Door.pulse } } }\n"
        "    timeout_ms: 50\n"
        "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 20 } }\n"
        "    expect: { Body.level: { ge: { variable: Door.level } } }\n"
        "  - wave: { Body.level: { pulse: { value: 5, width_ms: 20 } } }\n"
        "    expect: { Body.level: { near: { variable: Door.level }, tolerance: 10 } }\n",
        cat);
    CHECK_EQ(s.steps.size(), std::size_t{7});
    std::string texts;
    for (const auto& step : s.steps)
    {
        texts += step.text + "\n";
    }
    check_text(texts,
               "expect Body.level eq Door.level; Door.mode eq Body.mode, ne Body.mode\n"
               "expect Body.level ge Door.ratio, le 100 (for 20 ms)\n"
               "expect Body.ratio gt Door.level, lt Door.body.speed (within 30 ms)\n"
               "expect Body.body.speed near Door.level (tolerance 2.5)\n"
               "wait_until Body.lamp ne Door.pulse (timeout 50 ms)\n"
               "ramp Body.level from 0 to 10 over 20 ms, expecting Body.level ge Door.level\n"
               "wave Body.level pulse 5 width 20 ms, expecting Body.level near Door.level (tolerance 10)\n");

    const auto errs = [](const std::string& body, const ScenarioCatalog& c = catalog(), const ScenarioContext& ctx = {}) {
        return lines(read_scenario(body, c, ctx).findings);
    };

    // Bare dotted identifier guidance across Integer, Real, Bool, and Enum.
    check_text(errs(kHead + "  - expect: { Body.level: Door.level }\n"),
               "E 3 scenario-value: the value of Body.level (integer) must be a whole number, not 'Door.level'; to compare with "
               "another variable, write { variable: Door.level }\n");
    check_text(errs(kHead + "  - expect: { Body.ratio: Door.ratio }\n"),
               "E 3 scenario-value: the value of Body.ratio (real) must be a number, not 'Door.ratio'; to compare with another "
               "variable, write { variable: Door.ratio }\n");
    check_text(errs(kHead + "  - expect: { Body.lamp: Door.lamp }\n"),
               "E 3 scenario-value: the value of Body.lamp (bool) must be true or false, not 'Door.lamp'; to compare with another "
               "variable, write { variable: Door.lamp }\n");
    check_text(errs(kHead + "  - expect: { Body.mode: Door.mode }\n"),
               "E 3 scenario-value: the value of Body.mode (enum) must be one of its items, by name or by value, not 'Door.mode': the "
               "items are off (0), on (1), auto (2); to compare with another variable, write { variable: Door.mode }\n");

    // Mixing variable and tolerance, mixing variable and operator, and condition key typo suggestion.
    check_text(errs(kHead + "  - expect: { Body.level: { variable: Door.level, tolerance: 5 } }\n"),
               "E 3 value: Body.level: variable and tolerance cannot be combined directly: write { near: { variable: Door.level }, "
               "tolerance: 5 }\n");
    check_text(errs(kHead + "  - expect: { Body.level: { near: { variable: Door.level, tolerance: 5 }, tolerance: 5 } }\n"),
               "E 3 unknown-key: Body.level: near: tolerance goes beside near, not inside { variable: ... }: write { near: { variable: "
               "Door.level }, tolerance: 5 }\n");
    CHECK(errs(kHead + "  - expect: { Body.level: { eq: 5, variable: Door.level } }\n")
              .find("variable cannot be combined with eq") != std::string::npos);
    CHECK(errs(kHead + "  - expect: { Body.level: { variabl: Door.level } }\n")
              .find("did you mean 'variable'?") != std::string::npos);

    // Parameters inside variable: are forbidden and never mark parameters as used or missing.
    {
        ScenarioContext ctx;
        ctx.params = {ParamValue{"rhs", "Door.level", false, ParamSource::CommandLine, 0, true}};
        const ScenarioRead r = read_scenario(kHead + "  - expect: { Body.level: { variable: \"${rhs}\" } }\n", catalog(), ctx);
        CHECK(!r.scenario.has_value());
        CHECK(lines(r.findings).find("E 3 scenario-param: '${rhs}': a parameter (${...}) stands for a value, not for a variable's name") !=
              std::string::npos);
        CHECK(r.params.empty()); // CLI --param rhs is never recorded as a used parameter!

        const ScenarioRead scan =
            read_scenario(kHead + "  - expect: { Body.level: { near: { variable: \"${rhs}\" }, tolerance: \"${tol}\" } }\n", catalog());
        CHECK(std::find(scan.missing_params.begin(), scan.missing_params.end(), "rhs") == scan.missing_params.end());
        CHECK(std::find(scan.missing_params.begin(), scan.missing_params.end(), "tol") != scan.missing_params.end());
    }

    // Self-comparison and type/enum mismatches.
    check_text(errs(kHead + "  - expect: { Body.level: { variable: Body.level } }\n"),
               "E 3 scenario-value: the value of Body.level (integer) compares Body.level with itself: name another variable, or write "
               "a constant value\n");
    CHECK(errs(kHead + "  - expect: { Body.lamp: { eq: { variable: Door.level } } }\n")
              .find("a bool variable can be compared only with another bool variable") != std::string::npos);
    CHECK(errs(kHead + "  - expect: { Body.level: { eq: { variable: Door.mode } } }\n")
              .find("an integer or real variable can be compared only with another integer or real variable") != std::string::npos);
    CHECK(errs(kHead + "  - expect: { Body.lamp: { ge: { variable: Door.lamp } } }\n")
              .find("ge compares numbers, and Body.lamp is a bool: use eq or ne") != std::string::npos);

    ScenarioCatalog badEnum = catalog();
    for (auto& v : badEnum[1].variables)
    {
        if (v.name == "mode")
        {
            v.items = {{"off", 0}, {"on", 10}, {"auto", 2}};
        }
    }
    const std::string enumErr = errs(kHead + "  - expect: { Body.mode: { eq: { variable: Door.mode } } }\n", badEnum);
    CHECK(enumErr.find("their enum items do not match") != std::string::npos);
    CHECK(enumErr.find("variables.<name>.map") != std::string::npos);
}

TEST_CASE(scenario_variable_comparisons_absent_skip_vs_runtime_mismatch)
{
    // 1. Static absent: true and not_in_variant on RHS skip the step (or only expect beside ramp/wave)
    // without false "nothing to compare with" / "tolerance goes with near" errors or duplicate variable names.
    ScenarioCatalog cat = catalog();
    cat[1].absent.push_back("missing");
    CatalogParticipant opt;
    opt.name = "Opt";
    opt.not_in_variant = true;
    cat.push_back(opt);

    ScenarioContext ctx;
    ctx.variant = "small";
    const ScenarioRead r = read_scenario(
        "vehicle_sil_scenario: 1\n"
        "steps:\n"
        "  - set: { Body.level: 10 }\n"
        "  - expect: { Body.level: { near: { variable: Door.missing }, tolerance: 5 } }\n"
        "  - wait_until: { Body.level: { variable: Opt.speed } }\n"
        "    timeout_ms: 50\n"
        "  - ramp: { Body.level: { from: 10, to: 20, over_ms: 20 } }\n"
        "    expect: { Body.level: { near: { variable: Door.missing }, tolerance: 5 } }\n"
        "  - expect: { Body.level: 20 }\n",
        cat, ctx);
    if (!r.scenario)
    {
        throw unit::Failure("unexpected errors:\n" + lines(r.findings));
    }
    CHECK_EQ(r.skipped, std::size_t{2});
    CHECK_EQ(r.unchecked_expects, std::size_t{1});
    check_text(r.scenario->steps[1].text, "expect Body.level near Door.missing (tolerance 5)");
    check_text(r.scenario->steps[2].text, "wait_until Body.level eq Opt.speed (timeout 50 ms)");
    check_text(r.scenario->steps[3].text,
               "ramp Body.level from 10 to 20 over 20 ms, expect not checked: Body.level near Door.missing (tolerance 5)");
    CHECK(!r.scenario->steps[1].skipped.empty());
    CHECK(!r.scenario->steps[2].skipped.empty());
    CHECK(r.scenario->steps[3].skipped.empty() && !r.scenario->steps[3].expect_skipped.empty());
    // Door is never read when its only use was in skipped/unchecked conditions.
    const std::vector<bool> readFlags = participants_read(*r.scenario);
    for (std::size_t i = 0; i < r.scenario->participants.size(); ++i)
    {
        if (r.scenario->participants[i].name == "Door")
        {
            CHECK(!readFlags[i]);
        }
    }

    // 2. When both LHS and RHS are absent, step.text does not duplicate the absent variable names.
    cat[0].absent.push_back("gone");
    const ScenarioRead bothAbsent = read_scenario(
        kHead + "  - set: { Body.level: 1 }\n  - expect: { Body.gone: { variable: Door.missing }, Body.level: { variable: Door.missing } }\n",
        cat, ctx);
    CHECK(bothAbsent.scenario.has_value());
    check_text(bothAbsent.scenario->steps[1].text, "expect Body.gone; Body.level eq Door.missing");

    // 3. Runtime disconnected RHS participant and runtime schema/enum mismatch fail immediately (never skipped).
    const Scenario sNum = read(kHead + "  - expect: { Body.level: { variable: Door.level } }\n");
    {
        World world;
        TimelineRecord record(sNum);
        ScenarioEngine engine(sNum, 10ms, world, {}, &record);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = true;
        peers[0].schema = model_variables();
        peers[1].connected = false; // Door disconnected at runtime
        engine.begin(peers);
        CHECK(engine.tick(at(0ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].status == StepStatus::Failed);
        CHECK(run.steps[0].message.find("participant 'Door' has no control link") != std::string::npos);
        CHECK_EQ(record.failures()[0].value().conditions.size(), std::size_t{1});
        CHECK_EQ(record.failures()[0].value().conditions[0].compared.size(), std::size_t{1});
    }
    const Scenario sEnum = read(kHead + "  - expect: { Body.mode: { eq: { variable: Door.mode } } }\n");
    {
        World world;
        ScenarioEngine engine(sEnum, 10ms, world);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = true;
        peers[0].schema = model_variables();
        peers[1].connected = true;
        peers[1].schema = model_variables();
        for (auto& v : peers[1].schema)
        {
            if (v.name == "mode")
            {
                v.items = {{"off", 0}, {"on", 99}, {"auto", 2}}; // runtime enum item drift!
            }
        }
        engine.begin(peers);
        CHECK(engine.tick(at(0ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].message.find("does not offer the variable 'mode' as --describe said it would") != std::string::npos);
    }
}

TEST_CASE(scenario_holds_exact_numeric_and_non_finite_semantics)
{
    using control::Value;
    constexpr std::int64_t kTwo53 = (std::int64_t{1} << 53);
    constexpr std::int64_t kTwo53Plus1 = kTwo53 + 1;
    constexpr std::int64_t kTwo62Plus1 = (std::int64_t{1} << 62) + 1;
    const double dTwo53 = static_cast<double>(kTwo53);         // exact 9007199254740992.0
    const double dTwo62 = std::ldexp(1.0, 62);                 // exact 4611686018427387904.0
    const double dTwo63 = std::ldexp(1.0, 63);                 // exact 9223372036854775808.0 (> INT64_MAX)
    const double dNegTwo63 = -std::ldexp(1.0, 63);             // exact INT64_MIN

    // 1. int64_t vs int64_t above 2^53 and at 64-bit signed extremes.
    CHECK(!holds(Compare::Eq, Value{kTwo53Plus1}, Value{kTwo53}));
    CHECK(holds(Compare::Ne, Value{kTwo53Plus1}, Value{kTwo53}));
    CHECK(holds(Compare::Gt, Value{kTwo53Plus1}, Value{kTwo53}));
    CHECK(!holds(Compare::Le, Value{kTwo53Plus1}, Value{kTwo53}));
    CHECK(holds(Compare::Near, Value{kTwo62Plus1}, Value{std::int64_t{1} << 62}, 1.0));
    CHECK(!holds(Compare::Near, Value{kTwo62Plus1}, Value{std::int64_t{1} << 62}, 0.5));
    CHECK(!holds(Compare::Near, Value{std::numeric_limits<std::int64_t>::min()},
                 Value{std::numeric_limits<std::int64_t>::max()}, std::ldexp(1.0, 63)));
    CHECK(holds(Compare::Near, Value{std::numeric_limits<std::int64_t>::min()},
                Value{std::numeric_limits<std::int64_t>::max()}, std::ldexp(1.0, 64)));

    // 2. int64_t vs double above 2^53 (2^53 + 1 != double(2^53) in both directions across all 7 ops).
    CHECK(!holds(Compare::Eq, Value{kTwo53Plus1}, Value{dTwo53}));
    CHECK(!holds(Compare::Eq, Value{dTwo53}, Value{kTwo53Plus1}));
    CHECK(holds(Compare::Ne, Value{kTwo53Plus1}, Value{dTwo53}));
    CHECK(holds(Compare::Gt, Value{kTwo53Plus1}, Value{dTwo53}));
    CHECK(holds(Compare::Ge, Value{kTwo53Plus1}, Value{dTwo53}));
    CHECK(!holds(Compare::Lt, Value{kTwo53Plus1}, Value{dTwo53}));
    CHECK(!holds(Compare::Le, Value{kTwo53Plus1}, Value{dTwo53}));
    CHECK(holds(Compare::Lt, Value{dTwo53}, Value{kTwo53Plus1}));
    CHECK(holds(Compare::Near, Value{kTwo62Plus1}, Value{dTwo62}, 1.0));
    CHECK(!holds(Compare::Near, Value{kTwo62Plus1}, Value{dTwo62}, 0.9999999999999999));

    // 3. Boundary at 2^63 and -2^63, and Sterbenz sub-ULP fractional boundary.
    CHECK(holds(Compare::Lt, Value{std::numeric_limits<std::int64_t>::max()}, Value{dTwo63}));
    CHECK(!holds(Compare::Eq, Value{std::numeric_limits<std::int64_t>::max()}, Value{dTwo63}));
    CHECK(holds(Compare::Near, Value{std::numeric_limits<std::int64_t>::max()}, Value{dTwo63}, 1.0));
    CHECK(!holds(Compare::Near, Value{std::numeric_limits<std::int64_t>::max()}, Value{dTwo63}, 0.5));
    CHECK(holds(Compare::Eq, Value{std::numeric_limits<std::int64_t>::min()}, Value{dNegTwo63}));

    // i = 1, d = 2^-54: true |1 - 2^-54| = 1 - 2^-54. With tol = 1 - 2^-53, true tol < |i - d|,
    // even though (2^-54) + (1 - 2^-53) rounds to 1.0 in IEEE-754 double! Sterbenz must reject!
    const double dSmall = std::ldexp(1.0, -54);
    const double tolTight = 1.0 - std::ldexp(1.0, -53);
    CHECK(!holds(Compare::Near, Value{std::int64_t{1}}, Value{dSmall}, tolTight));
    CHECK(holds(Compare::Near, Value{std::int64_t{1}}, Value{std::ldexp(1.0, -53)}, tolTight));
    // Negative fractional d = -1e-20 vs i = 0: std::modf keeps |fpart| = 1e-20 (d - floor(d) would round to 1.0).
    CHECK(holds(Compare::Near, Value{std::int64_t{0}}, Value{-1e-20}, 1e-19));
    CHECK(!holds(Compare::Near, Value{std::int64_t{0}}, Value{-1e-20}, 1e-21));

    // 4. Non-finite values (NaN, +Inf, -Inf) evaluate to false for ALL operators including Ne.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const Compare op : {Compare::Eq, Compare::Ne, Compare::Ge, Compare::Le, Compare::Gt, Compare::Lt, Compare::Near})
    {
        for (const double bad : {nan, inf, -inf})
        {
            CHECK(!holds(op, Value{bad}, Value{0.0}, 10.0));
            CHECK(!holds(op, Value{0.0}, Value{bad}, 10.0));
            CHECK(!holds(op, Value{bad}, Value{std::int64_t{0}}, 10.0));
            CHECK(!holds(op, Value{std::int64_t{0}}, Value{bad}, 10.0));
            CHECK(!holds(op, Value{bad}, Value{bad}, 10.0));
        }
    }

    // 5. In ScenarioEngine: missing RHS sample or NaN on RHS makes even `ne` fail!
    const Scenario sNe = read(kHead + "  - expect: { Body.ratio: { ne: { variable: Door.ratio } } }\n");
    {
        // Door has not sent a sample yet -> `ne` must fail (not pass!).
        World world;
        ScenarioEngine engine(sNe, 10ms, world);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = peers[1].connected = true;
        peers[0].schema = peers[1].schema = model_variables();
        engine.begin(peers);
        auto bodySnap = std::make_shared<control::Snapshot>();
        bodySnap->time = at(0ms);
        bodySnap->sequence = 1;
        bodySnap->values = {Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{false}, Value{0.25},
                            Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{false},           Value{0.0}};
        engine.sample(0, bodySnap);
        CHECK(!engine.tick(at(0ms))); // waits for samples of 0 ms until next controller tick
        CHECK(engine.tick(at(10ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].message.find("Door.ratio had no value yet") != std::string::npos);
    }
    {
        // Door sends NaN -> `ne` must fail and report NaN!
        World world;
        TimelineRecord record(sNe);
        ScenarioEngine engine(sNe, 10ms, world, {}, &record);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = peers[1].connected = true;
        peers[0].schema = peers[1].schema = model_variables();
        engine.begin(peers);
        auto snap0 = std::make_shared<control::Snapshot>();
        snap0->time = at(0ms);
        snap0->sequence = 1;
        snap0->values = {Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{false}, Value{0.25},
                         Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{false},           Value{0.0}};
        auto snap1 = std::make_shared<control::Snapshot>(*snap0);
        snap1->values[4] = Value{nan};
        engine.sample(0, snap0);
        engine.sample(1, snap1);
        CHECK(engine.tick(at(10ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].message.find("while Door.ratio was nan (its sample of 0 ms)") != std::string::npos);
        const auto& fc = record.failures()[0].value().conditions[0];
        CHECK(fc.value == std::optional<Value>(Value{0.25}));
        CHECK(fc.sample == std::optional<SimTime>(at(0ms)));
        CHECK(!fc.compared[0].value.has_value());
        CHECK(!fc.compared[0].sample.has_value());
    }
    {
        // Symmetric: Body (LHS) sends NaN while Door (RHS) sends finite 0.25 -> LHS value & sample are both nullopt.
        World world;
        TimelineRecord record(sNe);
        ScenarioEngine engine(sNe, 10ms, world, {}, &record);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = peers[1].connected = true;
        peers[0].schema = peers[1].schema = model_variables();
        engine.begin(peers);
        auto snap0 = std::make_shared<control::Snapshot>();
        snap0->time = at(0ms);
        snap0->sequence = 1;
        snap0->values = {Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{false}, Value{nan},
                         Value{std::int64_t{0}}, Value{std::int64_t{0}}, Value{false},           Value{0.0}};
        auto snap1 = std::make_shared<control::Snapshot>(*snap0);
        snap1->values[4] = Value{0.25};
        engine.sample(0, snap0);
        engine.sample(1, snap1);
        CHECK(engine.tick(at(10ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        const auto& fc = record.failures()[0].value().conditions[0];
        CHECK(!fc.value.has_value());
        CHECK(!fc.sample.has_value());
        CHECK(fc.compared[0].value == std::optional<Value>(Value{0.25}));
        CHECK(fc.compared[0].sample == std::optional<SimTime>(at(0ms)));
    }
}

TEST_CASE(scenario_variable_comparisons_multirate_batch_latch_and_driven_dither)
{
    // 1. Simultaneous sample batch-latch invariant: at t = 10 ms both Body and Door jump from 10 to 50.
    // During hold_for_ms, `Body.level eq Door.level` must hold at t = 10 ms regardless of whether Body or Door
    // is participant 0 in the catalog/scenario! (Evaluating watches per-sample instead of batch-latching all
    // samples at timestamp t = 10 ms would see 50 != 10 and fail!)
    for (const bool doorFirst : {false, true})
    {
        ScenarioCatalog cat;
        if (doorFirst)
        {
            cat.push_back(CatalogParticipant{"Door", "", 10ms, model_variables()});
            cat.push_back(CatalogParticipant{"Body", "", 10ms, model_variables()});
        }
        else
        {
            cat.push_back(CatalogParticipant{"Body", "", 10ms, model_variables()});
            cat.push_back(CatalogParticipant{"Door", "", 10ms, model_variables()});
        }
        const Scenario s = read(
            kHead + "  - expect: { Body.level: { variable: Door.level } }\n"
                    "    hold_for_ms: 20\n",
            cat);
        World world;
        ScenarioEngine engine(s, 10ms, world);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = peers[1].connected = true;
        peers[0].schema = peers[1].schema = model_variables();
        engine.begin(peers);
        const auto makeSnap = [](SimTime t, std::uint64_t seq, std::int64_t lvl) {
            auto snap = std::make_shared<control::Snapshot>();
            snap->time = t;
            snap->sequence = seq;
            snap->values = {control::Value{lvl},            control::Value{std::int64_t{0}}, control::Value{std::int64_t{0}},
                            control::Value{false},          control::Value{0.0},             control::Value{std::int64_t{0}},
                            control::Value{std::int64_t{0}}, control::Value{false},          control::Value{0.0}};
            return snap;
        };
        engine.sample(0, makeSnap(at(0ms), 1, 10));
        engine.sample(1, makeSnap(at(0ms), 1, 10));
        CHECK(!engine.tick(at(10ms)));
        engine.sample(0, makeSnap(at(10ms), 2, 50));
        engine.sample(1, makeSnap(at(10ms), 2, 50));
        engine.sample(0, makeSnap(at(20ms), 3, 50));
        engine.sample(1, makeSnap(at(20ms), 3, 50));
        CHECK(engine.tick(at(30ms)));
        CHECK(engine.run().status == ScenarioStatus::Passed);
    }

    // 2. Multi-rate (10 ms x 20 ms) and both participant declaration orders across all delivery policies.
    for (const bool swapOrder : {false, true})
    {
        ScenarioCatalog cat;
        if (swapOrder)
        {
            cat.push_back(CatalogParticipant{"Door", "", 20ms, model_variables()});
            cat.push_back(CatalogParticipant{"Body", "", 10ms, model_variables()});
        }
        else
        {
            cat.push_back(CatalogParticipant{"Body", "", 10ms, model_variables()});
            cat.push_back(CatalogParticipant{"Door", "", 20ms, model_variables()});
        }
        const Scenario s = read(
            kHead + "  - set: { Body.level: 20, Door.level: 20 }\n"
                    "  - wait_until: { Body.body.speed: { eq: { variable: Door.body.speed }, ge: 200 } }\n"
                    "    timeout_ms: 200\n"
                    "  - expect: { Body.body.speed: { near: { variable: Door.body.speed }, tolerance: 1 } }\n"
                    "    hold_for_ms: 40\n",
            cat);
        for (const auto& [pname, policy] : policies())
        {
            for (const bool rev : {false, true})
            {
                World world;
                world.ecus[1].step = 20ms;
                world.policy = policy;
                world.reverse = rev;
                const ScenarioRun run = world.run(s);
                if (run.status != ScenarioStatus::Passed)
                {
                    throw unit::Failure(pname + ": " + text_of(run));
                }
            }
        }
    }

    // 3. Scenario-driven Input + dither on RHS compares against the ECU's sampled value, never the scheduled written value.
    // Door has dither on Door.level and also overrides its own level in step 20 ms (`own_level = {20ms, 77}`):
    // scheduled written value for Door.level at 20 ms is 40 + dither (40), while ECU sampled value is 77!
    {
        const Scenario s = read(
            "vehicle_sil_scenario: 1\n"
            "inputs:\n"
            "  Door.level: { dither: { triangle: { amplitude: 4, period_ms: 40 } } }\n"
            "steps:\n"
            "  - at_ms: 0\n"
            "    set: { Body.level: 77, Door.level: 40 }\n"
            "  - at_ms: 20\n"
            "    expect: { Body.level: { variable: Door.level } }\n");
        World world;
        world.ecus[1].own_level = {at(20ms), 77};
        const ScenarioRun run = world.run(s);
        CHECK(run.status == ScenarioStatus::Passed);

        // Conversely, expecting Body.level == 40 (the scheduled written value) via Door.level when Door sampled 77 fails!
        const Scenario sFail = read(
            "vehicle_sil_scenario: 1\n"
            "inputs:\n"
            "  Door.level: { dither: { triangle: { amplitude: 4, period_ms: 40 } } }\n"
            "steps:\n"
            "  - at_ms: 0\n"
            "    set: { Body.level: 40, Door.level: 40 }\n"
            "  - at_ms: 20\n"
            "    expect: { Body.level: { variable: Door.level } }\n");
        World worldFail;
        worldFail.ecus[1].own_level = {at(20ms), 77};
        const ScenarioRun runFail = worldFail.run(sFail);
        CHECK(runFail.status == ScenarioStatus::Failed);
        CHECK(runFail.steps[1].message.find("it was 40 (its sample of 20 ms) while Door.level was 77 (its sample of 20 ms)") !=
              std::string::npos);
    }
}

TEST_CASE(scenario_variable_comparisons_during_boundaries_mixed_and_unseen_rhs)
{
    // 1. Mixed constant + variable comparison in During ([20, 40]):
    // If Body.level > 50 at t = 20 ms BEFORE Door has sent any sample >= 20 ms, `le: 50` fails immediately at 20 ms!
    {
        ScenarioCatalog cat = catalog(10ms, 100ms);
        const Scenario s = read(
            "vehicle_sil_scenario: 1\n"
            "samples: as_sent\n"
            "steps:\n"
            "  - at_ms: 20\n"
            "    ramp: { Body.level: { from: 60, to: 70, over_ms: 20 } }\n"
            "    expect: { Body.level: { ge: { variable: Door.level }, le: 50 } }\n",
            cat);
        World world;
        world.ecus[1].step = 100ms;
        world.ecus[1].level = 10; // Door only has its t = 0 ms sample (< 20 ms)!
        TimelineRecord record(s);
        world.record = &record;
        const ScenarioRun run = world.run(s);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].at == std::optional<SimTime>(at(20ms)));
        CHECK(run.steps[0].message.find("at 20 ms Body.level ge Door.level, le 50, but it was 60 (its sample of 20 ms)") !=
              std::string::npos);
        // Only the constant `le 50` failed (ge Door.level had no >= 20 ms RHS sample yet), so `compared` is empty.
        CHECK(record.failures()[0].value().conditions[0].compared.empty());
    }

    // 2. Symmetric `since = T` and `close(U)` when LHS arrives in [T, U] and RHS has only a stale (< T) sample:
    // Step fails at U naming Door.level once (even when referenced twice), preserving LHS actual value/sample and
    // recording `compared: [{ variable: Door.level, value: null, sample_ms: null }]`.
    {
        ScenarioCatalog cat = catalog(10ms, 100ms);
        const Scenario s = read(
            "vehicle_sil_scenario: 1\n"
            "samples: as_sent\n"
            "steps:\n"
            "  - at_ms: 20\n"
            "    ramp: { Body.level: { from: 10, to: 20, over_ms: 20 } }\n"
            "    expect: { Body.level: { ge: { variable: Door.level }, le: { variable: Door.level } } }\n",
            cat);
        World world;
        world.ecus[1].step = 100ms;
        world.ecus[1].level = 99; // stale t = 0 ms sample on Door (99) must NOT be used at t = 20..40 ms!
        TimelineRecord record(s);
        world.record = &record;
        const ScenarioRun run = world.run(s);
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].at == std::optional<SimTime>(at(40ms)));
        check_text(run.steps[0].message,
                   "not checked during the ramp (from 20 ms to 40 ms): no sample of Door.level in that time (its ECU sends one after "
                   "each of its steps; with samples: as_sent, every sample_period); make the ramp longer, or check in an expect step "
                   "with within_ms after it");
        const auto& failedConds = record.failures()[0].value().conditions;
        CHECK_EQ(failedConds.size(), std::size_t{1});
        // LHS Body.level WAS seen at 40 ms with value 20, while RHS Door.level was unseen in [20, 40]!
        CHECK(failedConds[0].value == std::optional<control::Value>(control::Value{std::int64_t{20}}));
        CHECK(failedConds[0].sample == std::optional<SimTime>(at(40ms)));
        CHECK_EQ(failedConds[0].compared.size(), std::size_t{1});
        CHECK(!failedConds[0].compared[0].value.has_value());
        CHECK(!failedConds[0].compared[0].sample.has_value());
    }

    struct ImmediatePort : ScenarioPort
    {
        std::uint64_t next = 0;
        std::map<std::uint64_t, control::OperationResult> ops;
        std::uint64_t write(std::vector<control::PeerWrite> writes, SimTime time) override
        {
            const std::uint64_t seq = ++next;
            control::OperationResult r;
            r.seq = seq;
            for (const auto& w : writes)
            {
                if (std::none_of(r.targets.begin(), r.targets.end(), [&](const auto& t) { return t.peer == w.peer; }))
                {
                    const auto cnt = static_cast<std::uint32_t>(
                        std::count_if(writes.begin(), writes.end(), [&](const auto& x) { return x.peer == w.peer; }));
                    r.targets.push_back(control::TargetResult{w.peer, control::TargetStatus::Applied, time, false, "", cnt});
                }
            }
            ops[seq] = std::move(r);
            return seq;
        }
        void settle() override {}
        std::optional<control::OperationResult> result(std::uint64_t t) override { return ops.at(t); }
        std::optional<control::OperationResult> wait_done(std::uint64_t t) override { return ops.at(t); }
    };

    const auto makeRatioSnap = [](SimTime t, std::uint64_t seq, std::int64_t lvl, double ratio) {
        auto snap = std::make_shared<control::Snapshot>();
        snap->time = t;
        snap->sequence = seq;
        snap->values = {control::Value{lvl},            control::Value{std::int64_t{0}}, control::Value{std::int64_t{0}},
                        control::Value{false},          control::Value{ratio},           control::Value{std::int64_t{0}},
                        control::Value{std::int64_t{0}}, control::Value{false},          control::Value{0.0}};
        return snap;
    };

    // 3. Non-finite (NaN / Inf) sample inside a multi-rate During ([20, 40]) window under samples: every_step:
    // Both LHS NaN before the slower RHS steps and RHS NaN before the slower LHS steps must fail immediately at 20 ms
    // instead of being overwritten by a later finite sample at 30 ms!
    const double nan = std::numeric_limits<double>::quiet_NaN();
    {
        // LHS (Body, 10 ms) emits NaN at 20 ms and recovers to 1.0 at 30 ms; RHS (Door, 30 ms) steps at 0 ms and 30 ms.
        ScenarioCatalog cat = catalog(10ms, 30ms);
        const Scenario s = read(
            kHead + "  - at_ms: 20\n"
                    "    ramp: { Body.level: { from: 10, to: 20, over_ms: 20 } }\n"
                    "    expect: { Body.ratio: { ne: { variable: Door.ratio } } }\n",
            cat);
        ImmediatePort port;
        TimelineRecord record(s);
        ScenarioEngine engine(s, 10ms, port, {}, &record);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = peers[1].connected = true;
        peers[0].schema = peers[1].schema = model_variables();
        engine.begin(peers);
        engine.sample(0, makeRatioSnap(at(0ms), 1, 0, 1.0));
        engine.sample(1, makeRatioSnap(at(0ms), 1, 0, 2.0));
        CHECK(!engine.tick(at(10ms)));
        engine.sample(0, makeRatioSnap(at(10ms), 2, 0, 1.0));
        CHECK(!engine.tick(at(20ms)));
        engine.sample(0, makeRatioSnap(at(20ms), 3, 10, nan));
        engine.sample(0, makeRatioSnap(at(30ms), 4, 15, 1.0));
        engine.sample(1, makeRatioSnap(at(30ms), 2, 0, 2.0));
        CHECK(engine.tick(at(40ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].at == std::optional<SimTime>(at(20ms)));
        CHECK(run.steps[0].message.find("at 20 ms Body.ratio ne Door.ratio, but it was nan (its sample of 20 ms)") !=
              std::string::npos);
        const auto& fc = record.failures()[0].value().conditions[0];
        CHECK(!fc.value.has_value());
        CHECK(!fc.sample.has_value());
        CHECK_EQ(fc.compared.size(), std::size_t{1});
        CHECK(!fc.compared[0].value.has_value());
        CHECK(!fc.compared[0].sample.has_value());
    }
    {
        // Symmetric: LHS (Body, 30 ms) has no >= 20 ms sample yet when RHS (Door, 10 ms) emits NaN at 20 ms and recovers at 30 ms.
        ScenarioCatalog cat = catalog(30ms, 10ms);
        const Scenario s = read(
            kHead + "  - at_ms: 20\n"
                    "    ramp: { Door.level: { from: 10, to: 20, over_ms: 20 } }\n"
                    "    expect: { Body.ratio: { ne: { variable: Door.ratio } } }\n",
            cat);
        ImmediatePort port;
        TimelineRecord record(s);
        ScenarioEngine engine(s, 10ms, port, {}, &record);
        std::vector<EnginePeer> peers(2);
        peers[0].connected = peers[1].connected = true;
        peers[0].schema = peers[1].schema = model_variables();
        engine.begin(peers);
        const std::size_t bodyIdx = s.participants[0].name == "Body" ? 0 : 1;
        const std::size_t doorIdx = 1 - bodyIdx;
        engine.sample(bodyIdx, makeRatioSnap(at(0ms), 1, 0, 1.0));
        engine.sample(doorIdx, makeRatioSnap(at(0ms), 1, 0, 2.0));
        CHECK(!engine.tick(at(10ms)));
        engine.sample(doorIdx, makeRatioSnap(at(10ms), 2, 0, 2.0));
        CHECK(!engine.tick(at(20ms)));
        engine.sample(doorIdx, makeRatioSnap(at(20ms), 3, 10, nan));
        engine.sample(bodyIdx, makeRatioSnap(at(30ms), 2, 0, 1.0));
        engine.sample(doorIdx, makeRatioSnap(at(30ms), 4, 15, 2.0));
        CHECK(engine.tick(at(40ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].at == std::optional<SimTime>(at(20ms)));
        CHECK(run.steps[0].message.find("it had no sample from 20 ms yet while Door.ratio was nan (its sample of 20 ms)") !=
              std::string::npos);
        const auto& fc = record.failures()[0].value().conditions[0];
        CHECK(!fc.value.has_value());
        CHECK(!fc.sample.has_value());
        CHECK_EQ(fc.compared.size(), std::size_t{1});
        CHECK(!fc.compared[0].value.has_value());
        CHECK(!fc.compared[0].sample.has_value());
    }

    // 4. close(U) with two RHS variables on one condition: when LHS and Fast are both seen and passing in [20, 40]
    // while Slow has no sample in [20, 40], failed.conditions[].compared[] lists ONLY Slow (not Fast)!
    {
        ScenarioCatalog cat;
        cat.push_back(CatalogParticipant{"Body", "", 10ms, model_variables()});
        cat.push_back(CatalogParticipant{"Fast", "", 10ms, model_variables()});
        cat.push_back(CatalogParticipant{"Slow", "", 100ms, model_variables()});
        const Scenario s = read(
            "vehicle_sil_scenario: 1\n"
            "samples: as_sent\n"
            "steps:\n"
            "  - at_ms: 20\n"
            "    ramp: { Body.level: { from: 20, to: 30, over_ms: 20 } }\n"
            "    expect: { Body.level: { ge: { variable: Fast.level }, le: { variable: Slow.level } } }\n",
            cat);
        ImmediatePort port;
        TimelineRecord record(s);
        ScenarioEngine engine(s, 10ms, port, {}, &record);
        std::vector<EnginePeer> peers(3);
        for (auto& p : peers)
        {
            p.connected = true;
            p.schema = model_variables();
        }
        engine.begin(peers);
        engine.sample(0, makeRatioSnap(at(0ms), 1, 20, 0.0));
        engine.sample(1, makeRatioSnap(at(0ms), 1, 10, 0.0));
        engine.sample(2, makeRatioSnap(at(0ms), 1, 50, 0.0)); // Slow seen only at 0 ms (< 20 ms)
        CHECK(!engine.tick(at(10ms)));
        engine.sample(0, makeRatioSnap(at(10ms), 2, 20, 0.0));
        engine.sample(1, makeRatioSnap(at(10ms), 2, 10, 0.0));
        CHECK(!engine.tick(at(20ms)));
        engine.sample(0, makeRatioSnap(at(20ms), 3, 20, 0.0));
        engine.sample(1, makeRatioSnap(at(20ms), 3, 10, 0.0));
        CHECK(!engine.tick(at(30ms)));
        engine.sample(0, makeRatioSnap(at(30ms), 4, 25, 0.0));
        engine.sample(1, makeRatioSnap(at(30ms), 4, 10, 0.0));
        CHECK(!engine.tick(at(40ms)));
        engine.sample(0, makeRatioSnap(at(40ms), 5, 30, 0.0));
        engine.sample(1, makeRatioSnap(at(40ms), 5, 10, 0.0));
        CHECK(engine.tick(at(50ms)));
        const ScenarioRun run = engine.run();
        CHECK(run.status == ScenarioStatus::Failed);
        CHECK(run.steps[0].at == std::optional<SimTime>(at(40ms)));
        CHECK(run.steps[0].message.find("no sample of Slow.level in that time") != std::string::npos);
        CHECK(run.steps[0].message.find("Fast.level") == std::string::npos);
        const auto& fc = record.failures()[0].value().conditions[0];
        CHECK(fc.value == std::optional<control::Value>(control::Value{std::int64_t{30}}));
        CHECK(fc.sample == std::optional<SimTime>(at(40ms)));
        CHECK_EQ(fc.compared.size(), std::size_t{1});
        CHECK(s.variables[fc.compared[0].variable].text == "Slow.level");
        CHECK(!fc.compared[0].value.has_value());
        CHECK(!fc.compared[0].sample.has_value());
    }
}
