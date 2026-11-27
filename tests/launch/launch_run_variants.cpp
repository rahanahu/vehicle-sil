// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about variable maps, variants of a configuration, templates/cases,
// and vehicle_sil_launch test:
//
//   template_cases_matrix           multi-ECU glitch + shared template + Japanese/space case names,
//                                   repeatable --case filter, template dither failure, variants
//                                   only/absent/scale/nested-template matrix, and run_alone() re-run
//   template_csv_determinism        case-bound CSV points in templates with variant scale, dither,
//                                   and --speed 0.5 / --speed max / slow-ms virtual-time determinism
//   template_equivalence_and_serve  cases + hex param + --param override, direct vs template+case
//                                   equivalence on Engine+Dashboard, and serve --once template events
//   test_command  vehicle_sil_launch test: every variant with every scenario of a directory, one
//                run after the other; skipped, failed and passed runs in one table and as JSON;
//                --variant, --param, --fail-fast, --strict, and what ends it before its first run
//   test_interrupt  an interrupt during a test: the current run is stopped, the rest is not run
#include "launch_run_support.hpp"

/// A scenario through a participant's variable map: the names and values are the map's, and a
/// write takes effect in the same step as without a map.
void variable_map(const Options& o)
{
    const std::string head = "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n";
    const std::string body = "  - name: Body\n    command: [" + ecu(o) + "]\n";
    const std::string door = "  - name: Door\n    command: [" + ecu(o) + "]\n";
    const std::string mapped = head + body +
                               "    variables:\n"
                               "      body.fill: { from: level, scale: 0.01, unit: ratio }\n"
                               "      changed_ms: level_changed_at_ms\n"
                               "      body.heater: { absent: true }\n" +
                               door +
                               "    variables:\n"
                               "      drive.mode: { from: mode, map: { standby: 0, cruise: 2 } }\n";
    const std::vector<std::pair<std::string, std::string>> behaviours{{"Body", "scenario-variables"}, {"Door", "scenario-variables"}};
    const auto steps = [](const std::string& fill, const std::string& changed, const std::string& mode, double unit) {
        const auto n = [unit](double percent) {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", percent * unit);
            return std::string(text);
        };
        return "vehicle_sil_scenario: 1\n"
               "name: fill\n"
               "steps:\n"
               "  - set: { Body." + fill + ": " + n(5) + ", Door." + mode + " }\n"
               "  - expect: { Body." + changed + ": 0, Door.mode_now: auto, Body." + fill + ": { near: " + n(5) + ", tolerance: 0.000001 } }\n"
               "  - at_ms: 30\n"
               "    set: { Body." + fill + ": " + n(6) + " }\n"
               "  - expect: { Body." + changed + ": 30 }\n"
               "  - at_ms: 70\n"
               "    set: { Body." + fill + ": " + n(100) + " }\n"
               "  - expect: { Body." + changed + ": 70, Body." + fill + ": " + n(100) + " }\n"
               "  - ramp: { Body." + fill + ": { to: 0, over_ms: 30 } }\n"
               "  - expect: { Body." + fill + ": 0 }\n";
    };
    const auto run = [&](const fs::path& dir, const std::string& what) {
        const Captured r = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, dir);
        expect_code(r, 0, what);
        const std::string out = lf(r.standard_output);
        require(!contains(out, "variable-map"), what + ": every mapping applies:\n" + out);
        return scenario_json(out);
    };
    // Without a map, in the executable's names and values.
    fs::path dir = scenario_dir(o, head + body + door, behaviours);
    write_file(dir / "s.yaml", steps("level", "level_changed_at_ms", "mode: auto", 1.0));
    std::string plain = run(dir, "without a map");
    require(contains(plain, "\"status\": \"passed\""), "the scenario passes without a map:\n" + plain);

    // With the map: the same steps at the same times, in the seen names and values.
    dir = scenario_dir(o, mapped, behaviours);
    write_file(dir / "s.yaml", steps("body.fill", "changed_ms", "drive.mode: cruise", 0.01));
    const std::string seen = run(dir, "with a map");
    require(contains(seen, "\"status\": \"passed\""), "the scenario passes with the map:\n" + seen);
    const auto times = [](const std::string& json) {
        std::string out;
        for (std::size_t at = json.find("_ms\": "); at != std::string::npos; at = json.find("_ms\": ", at + 1))
        {
            out += json.substr(at, json.find('\n', at) - at) + " ";
        }
        return out;
    };
    require(!times(seen).empty(), "the result has the steps' times:\n" + seen);
    expect_text(times(seen), times(plain), "the times of the steps with and without the map");
    log("CHECK a scenario through a variable map: the same steps at the same times -> ok");

    // The check shows what the simulation sees, and both names of what is mapped.
    const Captured json = launch(o, {"check", "sim.yaml", "--json"}, dir);
    expect_code(json, 0, "check --json");
    const std::string report = lf(json.standard_output);
    require(contains(report, "\"name\": \"body.fill\",\n          \"type\": \"real\",\n          \"causality\": \"input\",\n"
                             "          \"unit\": \"ratio\",\n          \"min\": 0,\n          \"max\": 1,") &&
                contains(report, "\"name\": \"body.fill\",\n          \"line\": 7,\n          \"from\": \"level\",\n"
                                 "          \"absent\": false,\n          \"scale\": 0.01,\n          \"offset\": 0,") &&
                contains(report, "\"name\": \"body.heater\",\n          \"line\": 9,\n          \"from\": null,\n"
                                 "          \"absent\": true,") &&
                contains(report, "\"name\": \"drive.mode\",\n          \"type\": \"enum\","),
            "check --json has the seen variables and the map:\n" + report);

    // The executable's own names are not names of the simulation any more; a value is checked in
    // the seen range and against the map's names.
    write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\nsteps:\n"
                               "  - set: { Body.level: 5 }\n"
                               "  - set: { Body.body.fill: 1.5 }\n"
                               "  - set: { Door.drive.mode: auto }\n"
                               "  - set: { Body.body.heater: 1 }\n");
    const Captured wrong = launch(o, {"check", "sim.yaml", "--scenario", "s.yaml"}, dir);
    expect_code(wrong, 1, "check --scenario with the executable's names");
    const std::string said = lf(wrong.standard_output);
    require(contains(said, "s.yaml:3: 'Body.level': participant 'Body' has no variable 'level'") &&
                contains(said, "s.yaml:4: ") && contains(said, "Body.body.fill") &&
                contains(said, "s.yaml:5: ") && contains(said, "standby (0), cruise (2)") &&
                contains(said, "note: s.yaml:6: the step is skipped (set Body.body.heater): Body.body.heater is absent: "
                               "participant 'Body' does not have it in this configuration (absent: true in its variables)"),
            "the scenario's check uses the seen variables:\n" + said);

    // A mapping that does not fit the executable is an error of the check, and nothing runs.
    write_file(dir / "sim.yaml", head + body + "    variables:\n      body.fill: { from: levle, scale: 0.01 }\n" + door);
    const Captured unfit = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(unfit, 1, "run with a mapping that does not fit");
    require(contains(lf(unfit.standard_output),
                     "error: sim.yaml:7: participant 'Body': variables: body.fill: the executable has no variable 'levle': "
                     "did you mean 'level'? Correct the name, or write { absent: true } if this executable does not have "
                     "the variable [variable-map-unknown]\n"),
            "the mapping that does not fit:\n" + lf(unfit.standard_output) + lf(unfit.standard_error));
    log("CHECK a mapping that does not fit stops the run before it starts -> ok");

    // A participant that does not describe itself cannot be compared before the run: what does
    // not fit is said when its agent has connected (a line of the run), and with the result.
    write_file(dir / "sim.yaml", std::string("vehicle_sil_simulation: 2\n# run-option --stop-at-ms 100\nnetworks: []\nparticipants:\n") +
                                     "  - name: Body\n    command: [" + ecu(o) +
                                     ", --registry-uri, \"{registry_uri}\", --name, \"{name}\"]\n    describe: false\n"
                                     "    variables:\n      body.fill: { from: levle, scale: 0.01 }\n"
                                     "      changed_ms: level_changed_at_ms\n" +
                                     door);
    const Captured late = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(late, 1, "run with a mapping that the agent's variables do not fit");
    const std::string said_late = lf(late.standard_error);
    const std::string result_late = lf(late.standard_output);
    // The launcher does not wait for the agent of a participant with describe: false: the line is
    // said at the start if the agent is connected by then, and else after the simulation.
    require(contains(said_late,
                     "[vehicle_sil_launch] participant 'Body': variables: 1 of its mappings do not fit the variables its "
                     "control agent announced and are not applied: body.fill: the executable has no variable 'levle'. In "
                     "this run those variables are read under the executable's names and values, and a write to a name of "
                     "these mappings is refused\n") ||
                contains(said_late,
                         "[vehicle_sil_launch] participant 'Body': variables: 1 of its mappings do not fit the variables its "
                         "control agent announced, which connected after the simulation had started, and were not applied: "
                         "body.fill: the executable has no variable 'levle'. In this run those variables were read under the "
                         "executable's names and values, and a write to a name of these mappings was refused\n"),
            "the line of the run about the mapping:\n" + said_late);
    require(contains(result_late, "note: sim.yaml:9: participant 'Body': variables is not compared for a participant with "
                                  "describe: false") &&
                contains(result_late, "error: sim.yaml:5: participant 'Body': variables: 1 of its mappings do not fit the "
                                      "variables its control agent announced and were not applied") &&
                contains(result_late, "failed: 1 error, 0 warnings, 2 notes\n"),
            "the result about the mapping:\n" + result_late);
    log("CHECK a mapping that does not fit an undescribed participant is said in the run and fails it -> ok");
}

/// Variants of one configuration: the chosen one is laid over the common part (the variable map,
/// enabled, the parameters, the command), a scenario's ${name} takes its value from the scenario,
/// the configuration, the variant and --param in that order, and a step that uses a variable the
/// variant does not have, or of a participant the variant leaves out, is skipped and said so.
void variants(const Options& o)
{
    const std::string config = "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                               "  - name: Body\n    command: [" + ecu(o) + "]\n"
                               "    variables:\n"
                               "      body.fill: level\n"
                               "      body.heater: { absent: true }\n"
                               "  - name: Door\n    command: [" + ecu(o) + "]\n"
                               "params: { fill: 5, wait: 30 }\n"
                               "variants:\n"
                               "  small: {}\n"
                               "  medium:\n"
                               "    participants:\n"
                               "      Body: { variables: { body.heater: ticks } }\n"
                               "      Door: { enabled: false }\n"
                               "    params: { fill: 7 }\n"
                               "  broken:\n"
                               "    participants:\n"
                               "      Body:\n"
                               "        command: [./no_such_ecu]\n";
    const fs::path dir = scenario_dir(o, config, {{"Body", "scenario-variables"}, {"Door", "scenario-variables"}});
    write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\n"
                               "name: variants\n"
                               "params: { fill: 1, extra: 3 }\n"
                               "steps:\n"
                               "  - set: { Body.body.fill: \"${fill}\" }\n"
                               "  - after_ms: ${wait}\n"
                               "    expect: { Body.body.fill: \"${fill}\" }\n"
                               "  - expect: { Body.body.heater: { ge: 1 } }\n"
                               "    within_ms: 100\n"
                               "  - after_ms: 10\n"
                               "    expect: { Body.level_changed_at_ms: 0 }\n");

    // No variant chosen: a usage error that lists them, also for a program.
    const Captured none = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, dir);
    expect_code(none, 4, "run without --variant");
    require(contains(lf(none.standard_error), "error: the file defines variants, and a check or a run is of one of them: give "
                                              "--variant <name> with one of small, medium, broken (none of them is the default)\n"),
            "the usage error lists the variants:\n" + lf(none.standard_error));
    require(contains(lf(none.standard_output), "\"result\": \"usage\"") &&
                contains(lf(none.standard_output), "\"variant\": null,\n    \"variants\": [\n      \"small\",\n      \"medium\",\n      \"broken\"\n    ],"),
            "the usage result lists the variants:\n" + lf(none.standard_output));
    const Captured typo = launch(o, {"check", "sim.yaml", "--variant", "smal"}, dir);
    expect_code(typo, 4, "check with an unknown variant");
    require(contains(lf(typo.standard_error), "error: --variant smal: the file has no variant with this name (did you mean "
                                              "'small'?); its variants are small, medium, broken\n"),
            "the unknown variant:\n" + lf(typo.standard_error));
    // Also when the file has errors: the common part alone is not checked in its place.
    write_file(dir / "bad.yaml", config + "  wrong: { step_ms: 1 }\n");
    const Captured both = launch(o, {"check", "bad.yaml", "--variant", "smal", "--json"}, dir);
    expect_code(both, 4, "check with an unknown variant of a file with errors");
    require(contains(lf(both.standard_error), "error: --variant smal: the file has no variant with this name (did you mean "
                                              "'small'?); its variants are small, medium, broken, wrong; the file has errors "
                                              "too, which a check with one of these variants reports\n") &&
                contains(lf(both.standard_output), "\"result\": \"usage\""),
            "the unknown variant of a file with errors:\n" + lf(both.standard_error) + lf(both.standard_output));
    expect_code(launch(o, {"check", "bad.yaml", "--variant", "small"}, dir), 1, "check a variant of a file with errors");
    log("CHECK a file with variants needs --variant, and the error lists them -> ok");

    // small: the common part. Body has no heater: its step is skipped (it waits, so it takes no
    // time, and has no time at which it was decided), and the scenario passes, saying so.
    const Captured small = launch(o, {"run", "sim.yaml", "--variant", "small", "--scenario", "s.yaml", "--json"}, dir);
    expect_code(small, 0, "run --variant small");
    const std::string smallOut = lf(small.standard_output);
    const std::string smallScenario = scenario_json(smallOut);
    require(contains(smallScenario, "\"status\": \"passed\",") && contains(smallScenario, "\"skipped_steps\": 1,") &&
                contains(smallScenario, "\"text\": \"set Body.body.fill = 5\",") &&
                contains(smallScenario, "\"text\": \"expect Body.body.heater (within 100 ms)\",\n        \"status\": \"skipped\",\n"
                                        "        \"start_ms\": 30,\n        \"at_ms\": null,\n        \"end_ms\": 30,\n"
                                        "        \"code\": \"scenario-skipped\",\n"
                                        "        \"message\": \"Body.body.heater is absent: participant 'Body' does not have it in "
                                        "the variant small (absent: true in its variables)\"") &&
                contains(smallScenario, "\"status\": \"passed\",\n        \"start_ms\": 40,"),
            "the scenario of the variant small:\n" + smallScenario);
    require(contains(smallOut, "\"variant\": \"small\",\n    \"variants\": [") &&
                contains(smallOut, "\"params\": [\n      {\n        \"name\": \"fill\",\n        \"value\": 5,\n"
                                   "        \"from\": \"configuration\"\n      },") &&
                contains(smallOut, "\"code\": \"scenario-step-skipped\","),
            "the check of the variant small has the variant, the parameters and the note:\n" + smallOut);
    log("CHECK the common part as a variant: the absent variable's step is skipped and reported -> ok");

    // The same for people, and as events.
    const Captured text = launch(o, {"run", "sim.yaml", "--variant", "small", "--scenario", "s.yaml"}, dir);
    expect_code(text, 0, "run --variant small for people");
    const std::string shown = lf(text.standard_output);
    require(contains(shown, "expect Body.body.heater (within 100 ms)  skipped  -\n"), "the table has no time for the skipped step:\n" + shown);
    require(contains(shown, "sim.yaml (variant small): 2 participants, 0 networks\n") &&
                contains(shown, "scenario s.yaml (variants): 4 steps (1 skipped)\n"
                                "parameters: fill = 5 (the configuration's params), extra = 3 (the scenario's params), wait = 30 "
                                "(the configuration's params)\n") &&
                contains(shown, "scenario s.yaml (variants): passed, 3 of 4 steps passed, 1 skipped\n") &&
                contains(shown, "line 8 skipped: Body.body.heater is absent: participant 'Body' does not have it in the variant "
                                "small (absent: true in its variables)\n"),
            "the result for people:\n" + shown);
    require(contains(lf(text.standard_error), "scenario: line 8: expect Body.body.heater (within 100 ms): skipped "
                                              "(Body.body.heater is absent: participant 'Body' does not have it in the variant "
                                              "small (absent: true in its variables))\n"),
            "the run's line about the skipped step:\n" + lf(text.standard_error));
    const Captured events = launch(o, {"serve", "--once", "sim.yaml", "--variant", "small", "--scenario", "s.yaml"}, dir);
    expect_code(events, 0, "serve --once --variant small");
    require(contains(lf(events.standard_output), "\"index\":3,\"line\":8,\"kind\":\"expect\",\"text\":\"expect Body.body.heater (within 100 ms)\","
                                                 "\"status\":\"skipped\",\"start_ms\":30,\"at_ms\":null,\"end_ms\":30,"
                                                 "\"code\":\"scenario-skipped\","),
            "the event of the skipped step:\n" + lf(events.standard_output));
    log("CHECK the skipped step in the text, the run's lines and the events -> ok");

    // medium: the heater is there (another variable of the executable), Door is left out, and
    // the variant's parameter replaces the common one; --param replaces both.
    const Captured medium = launch(o, {"run", "sim.yaml", "--variant", "medium", "--scenario", "s.yaml", "--json"}, dir);
    expect_code(medium, 0, "run --variant medium");
    const std::string mediumOut = lf(medium.standard_output);
    const std::string mediumScenario = scenario_json(mediumOut);
    require(contains(mediumScenario, "\"status\": \"passed\",") && !contains(mediumScenario, "skipped") &&
                contains(mediumScenario, "\"text\": \"set Body.body.fill = 7\",") &&
                contains(mediumScenario, "\"text\": \"expect Body.body.heater ge 1 (within 100 ms)\",\n        \"status\": \"passed\","),
            "the scenario of the variant medium:\n" + mediumScenario);
    require(contains(mediumOut, "\"name\": \"Door\",\n      \"required\": true,\n      \"enabled\": false,") &&
                contains(mediumOut, "\"name\": \"fill\",\n        \"value\": 7,\n        \"from\": \"variant\""),
            "the variant medium leaves Door out and has its parameter:\n" + mediumOut);
    const Captured given = launch(o, {"run", "sim.yaml", "--variant", "medium", "--scenario", "s.yaml", "--param", "fill=9",
                                      "--param", "wait=50", "--json"}, dir);
    expect_code(given, 0, "run --param");
    const std::string givenOut = lf(given.standard_output);
    require(contains(scenario_json(givenOut), "\"text\": \"set Body.body.fill = 9\",") &&
                contains(scenario_json(givenOut), "\"text\": \"expect Body.body.fill eq 9\",\n        \"status\": \"passed\",\n"
                                                  "        \"start_ms\": 50,") &&
                contains(givenOut, "\"name\": \"fill\",\n          \"value\": 9,\n          \"from\": \"command_line\""),
            "--param replaces the variant's parameter:\n" + givenOut);
    log("CHECK a variant's variable map, enabled and params, and --param over them -> ok");

    // A --param the scenario does not have, and one without a scenario.
    const Captured unknown = launch(o, {"check", "sim.yaml", "--variant", "small", "--scenario", "s.yaml", "--param", "fil=9"}, dir);
    expect_code(unknown, 4, "check with a --param the scenario does not have");
    require(contains(lf(unknown.standard_error), "error: --param fil=9: the scenario has no parameter fil (did you mean "
                                                 "fill?): its parameters are fill, extra, wait; remove the option or correct "
                                                 "the name\n"),
            "the unknown parameter:\n" + lf(unknown.standard_error));
    const Captured unknownRun = launch(o, {"run", "sim.yaml", "--variant", "small", "--scenario", "s.yaml", "--param", "fil=9", "--json"}, dir);
    expect_code(unknownRun, 4, "run with a --param the scenario does not have");
    require(contains(lf(unknownRun.standard_output), "\"result\": \"usage\"") && !contains(lf(unknownRun.standard_output), "\"run\": {"),
            "the unknown parameter starts nothing:\n" + lf(unknownRun.standard_output));
    const Captured alone = launch(o, {"check", "sim.yaml", "--variant", "small", "--param", "fill=9"}, dir);
    expect_code(alone, 4, "check --param without --scenario");
    require(contains(lf(alone.standard_error), "error: --param sets a parameter of a scenario: give --scenario <file> too, or "
                                               "leave --param out\n"),
            "--param without a scenario:\n" + lf(alone.standard_error));
    const Captured form = launch(o, {"check", "sim.yaml", "--variant", "small", "--scenario", "s.yaml", "--param", "fill"}, dir);
    expect_code(form, 4, "check --param without a value");
    require(contains(lf(form.standard_error), "error: --param fill: write --param <name>=<value> (the name as the scenario has it "
                                              "in ${<name>}: letters, digits and _), e.g. --param idle_rpm=650\n"),
            "--param without a value:\n" + lf(form.standard_error));

    // The variant's command is the one that is checked and started.
    const Captured broken = launch(o, {"run", "sim.yaml", "--variant", "broken"}, dir);
    expect_code(broken, 1, "run --variant broken");
    require(contains(lf(broken.standard_output), "error: sim.yaml:22: participant 'Body': ") &&
                contains(lf(broken.standard_output), "no_such_ecu") &&
                contains(lf(broken.standard_output), "(vehicle_sil_launch check --variant broken sim.yaml shows the same)"),
            "the variant's command is checked:\n" + lf(broken.standard_output));

    // A scenario for other variants only is not run for this one.
    write_file(dir / "only.yaml", "vehicle_sil_scenario: 1\nonly: [medium]\nsteps:\n  - expect: { Body.body.heater: { ge: 0 } }\n");
    const Captured other = launch(o, {"run", "sim.yaml", "--variant", "small", "--scenario", "only.yaml"}, dir);
    expect_code(other, 1, "run a scenario that is only for another variant");
    require(contains(lf(other.standard_output), "error: only.yaml:2: the scenario is only for the variant medium (its only), not "
                                                "for small: run it with --variant medium, or add small to only [scenario-variant]\n"),
            "the scenario that is not for the variant:\n" + lf(other.standard_output));
    const Captured mine = launch(o, {"check", "sim.yaml", "--variant", "medium", "--scenario", "only.yaml", "--json"}, dir);
    expect_code(mine, 0, "check a scenario that is only for this variant");
    require(contains(lf(mine.standard_output), "\"steps\": 1,\n    \"only\": [\n      \"medium\"\n    ],\n"),
            "the check's JSON has only:\n" + lf(mine.standard_output));
    log("CHECK --param's errors, a variant's command, and only -> ok");

    // A participant the variant leaves out: the steps that use it are skipped, with the time
    // their text says (hold_for_ms), so the step after them starts when it does in small.
    write_file(dir / "door.yaml", "vehicle_sil_scenario: 1\n"
                                  "steps:\n"
                                  "  - set: { Body.body.fill: 5 }\n"
                                  "  - expect: { Door.level: 0 }\n"
                                  "    hold_for_ms: 60\n"
                                  "  - expect: { Body.body.fill: 5 }\n");
    const Captured without = launch(o, {"run", "sim.yaml", "--variant", "medium", "--scenario", "door.yaml", "--json"}, dir);
    expect_code(without, 0, "run a scenario that uses a participant the variant leaves out");
    const std::string withoutScenario = scenario_json(lf(without.standard_output));
    require(contains(withoutScenario, "\"text\": \"expect Door.level (for 60 ms)\",\n        \"status\": \"skipped\",\n"
                                      "        \"start_ms\": 0,\n        \"at_ms\": null,\n        \"end_ms\": 60,\n"
                                      "        \"code\": \"scenario-skipped\",\n"
                                      "        \"message\": \"participant 'Door' is not in the variant medium (enabled: false in "
                                      "that variant)\"") &&
                contains(withoutScenario, "\"text\": \"expect Body.body.fill eq 5\",\n        \"status\": \"passed\",\n"
                                          "        \"start_ms\": 60,") &&
                !contains(lf(without.standard_output), "--enable"),
            "the scenario of the variant medium, which has no Door:\n" + lf(without.standard_output));
    const Captured with = launch(o, {"run", "sim.yaml", "--variant", "small", "--scenario", "door.yaml", "--json"}, dir);
    expect_code(with, 0, "run the same scenario with the participant");
    require(contains(scenario_json(lf(with.standard_output)), "\"text\": \"expect Body.body.fill eq 5\",\n        \"status\": \"passed\",\n"
                                                              "        \"start_ms\": 60,") &&
                !contains(scenario_json(lf(with.standard_output)), "skipped"),
            "the same scenario in the variant small:\n" + lf(with.standard_output));
    // Left out by the command line: an error as before, which does not speak of the variant.
    const Captured disabled = launch(o, {"check", "sim.yaml", "--variant", "small", "--disable", "Door", "--scenario", "door.yaml"}, dir);
    expect_code(disabled, 1, "check a scenario that uses a participant that --disable leaves out");
    require(contains(lf(disabled.standard_output), "error: door.yaml:4: 'Door.level': participant 'Door' is left out of this run "
                                                   "(enabled: false, or --disable): take it in (--enable Door), or run a scenario "
                                                   "that does not use it [scenario-variable]\n"),
            "the participant that --disable leaves out:\n" + lf(disabled.standard_output));
    log("CHECK a participant the variant leaves out: its steps are skipped and take their time -> ok");

    // Nothing left that checks: a warning, which --strict does not let pass.
    write_file(dir / "heater.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - set: { Body.body.fill: 5 }\n"
                                    "  - expect: { Body.body.heater: { ge: 0 } }\n");
    const Captured hollow = launch(o, {"check", "sim.yaml", "--variant", "small", "--scenario", "heater.yaml"}, dir);
    expect_code(hollow, 0, "check a scenario whose checks are all skipped");
    require(contains(lf(hollow.standard_output),
                     "warning: heater.yaml: the scenario checks nothing in the variant small: its only step that checks (expect or "
                     "wait_until, line 4) is skipped, so it passes whatever the ECUs do (the notes say why each is skipped). If the "
                     "scenario is not for this variant, list the variants it is for in the scenario (only: [medium, broken]); if it "
                     "is, map what the steps look at to what the variant has (variables in the variant, "
                     "docs/simulation-config.md) [scenario-nothing-checked]\n"),
            "the warning about a scenario that checks nothing:\n" + lf(hollow.standard_output));
    expect_code(launch(o, {"check", "sim.yaml", "--variant", "small", "--scenario", "heater.yaml", "--strict"}, dir), 1,
                "check --strict a scenario whose checks are all skipped");
    const Captured strict = launch(o, {"run", "sim.yaml", "--variant", "small", "--scenario", "heater.yaml", "--strict", "--json"}, dir);
    expect_code(strict, 1, "run --strict a scenario whose checks are all skipped");
    require(contains(lf(strict.standard_output), "\"code\": \"scenario-nothing-checked\",") && !contains(lf(strict.standard_output), "\"status\": \"passed\""),
            "run --strict does not run a scenario that checks nothing:\n" + lf(strict.standard_output));
    expect_code(launch(o, {"run", "sim.yaml", "--variant", "small", "--scenario", "heater.yaml"}, dir), 0,
                "run a scenario whose checks are all skipped");
    log("CHECK a scenario whose checks are all skipped: a warning, and --strict fails -> ok");
}

void test_command(const Options& o)
{
    const std::string participants = "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                                     "  - name: Body\n    command: [" + ecu(o) + "]\n"
                                     "    variables:\n"
                                     "      body.fill: level\n"
                                     "      body.heater: { absent: true }\n";
    const fs::path dir = scenario_dir(o, participants + "params: { fill: 5 }\n"
                                                        "variants:\n"
                                                        "  small: {}\n"
                                                        "  medium:\n"
                                                        "    participants:\n"
                                                        "      Body: { variables: { body.heater: ticks } }\n"
                                                        "    params: { fill: 7 }\n",
                                      {{"Body", "scenario-variables"}});
    write_file(dir / "plain.yaml", participants);
    // Written in another order than their names; what is no scenario file is left alone.
    fs::remove_all(dir / "scn");
    fs::create_directories(dir / "scn" / "more");
    write_file(dir / "scn" / "c_fails.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - set: { Body.body.fill: 5 }\n"
                                             "  - after_ms: 20\n    expect: { Body.body.fill: 6 }\n");
    write_file(dir / "scn" / "b_fill.yaml", "vehicle_sil_scenario: 1\nparams: { fill: 1 }\nsteps:\n"
                                            "  - set: { Body.body.fill: \"${fill}\" }\n"
                                            "  - after_ms: 20\n    expect: { Body.body.fill: \"${fill}\" }\n"
                                            "  - expect: { Body.body.heater: { ge: 1 } }\n");
    write_file(dir / "scn" / "a_only.yaml", "vehicle_sil_scenario: 1\nonly: [medium]\nsteps:\n"
                                            "  - after_ms: 20\n    expect: { Body.body.heater: { ge: 1 } }\n");
    write_file(dir / "scn" / "notes.txt", "not a scenario\n");
    write_file(dir / "scn" / "more" / "d.yaml", "not a scenario either\n");

    // Every variant with every scenario of the directory, in the order of the names.
    const Captured all = launch(o, {"test", "sim.yaml", "--scenarios", "scn"}, dir);
    expect_code(all, 1, "test with a scenario that fails");
    const std::string failure = "line 4: expect Body.body.fill eq 6: ";
    const std::string table = lf(all.standard_output);
    require(contains(table, "variant  scenario          result\n"
                            "small    scn/a_only.yaml   skipped  the scenario is only for medium (its only)\n"
                            "small    scn/b_fill.yaml   passed   1 of 3 steps was skipped\n"
                            "small    scn/c_fails.yaml  FAILED   " + failure) &&
                contains(table, "\nmedium   scn/a_only.yaml   passed\n"
                                "medium   scn/b_fill.yaml   passed\n"
                                "medium   scn/c_fails.yaml  FAILED   " + failure) &&
                contains(table, "\n\nto see a failed run in full, run it alone: vehicle_sil_launch run sim.yaml --speed max --variant "
                                "small --scenario scn/c_fails.yaml\n\n"
                                "FAILED: 6 runs, 3 passed, 2 failed, 1 skipped\n"),
            "the table of the test:\n" + table);
    const std::string said = lf(all.standard_error);
    require(contains(said, "[vehicle_sil_launch] test 1 of 6: variant small, scenario scn/a_only.yaml\n"
                           "[vehicle_sil_launch] test 1 of 6: variant small, scenario scn/a_only.yaml: skipped (the scenario is only "
                           "for medium (its only))\n"
                           "[vehicle_sil_launch] test 2 of 6: variant small, scenario scn/b_fill.yaml\n") &&
                contains(said, "[vehicle_sil_launch] test 6 of 6: variant medium, scenario scn/c_fails.yaml: FAILED (" + failure),
            "the lines of the test:\n" + said);
    log("CHECK test: every variant with every scenario of a directory, in the order of the names -> ok");

    // The same as JSON: the counts, and each run as run --json has it (nothing for a skipped one).
    const Captured json = launch(o, {"test", "sim.yaml", "--scenarios", "scn", "--json"}, dir);
    expect_code(json, 1, "test --json");
    const std::string out = lf(json.standard_output);
    require(out.rfind("{\n  \"vehicle_sil_test\": 1,\n  \"vehicle_sil\": \"", 0) == 0 &&
                contains(out, "  \"file\": \"sim.yaml\",\n  \"result\": \"failed\",\n  \"strict\": false,\n"
                              "  \"variants\": [\n    \"small\",\n    \"medium\"\n  ],\n"
                              "  \"scenarios\": [\n    \"scn/a_only.yaml\",\n    \"scn/b_fill.yaml\",\n    \"scn/c_fails.yaml\"\n  ],\n"
                              "  \"passed\": 3,\n  \"failed\": 2,\n  \"skipped\": 1,\n  \"interrupted\": 0,\n  \"not_run\": 0,\n"
                              "  \"runs\": [\n    {\n      \"variant\": \"small\",\n      \"scenario\": \"scn/a_only.yaml\",\n"
                              "      \"result\": \"skipped\",\n      \"reason\": \"the scenario is only for medium (its only)\",\n"
                              "      \"run\": null\n    },\n    {\n      \"variant\": \"small\",\n"
                              "      \"scenario\": \"scn/b_fill.yaml\",\n      \"result\": \"passed\",\n"
                              "      \"reason\": \"1 of 3 steps was skipped\",\n      \"run\": {\n"
                              "        \"vehicle_sil_run\": 2,\n") &&
                contains(out, "        \"file\": \"sim.yaml\",\n        \"result\": \"passed\",\n") &&
                contains(out, "          \"file\": \"scn/c_fails.yaml\",\n") &&
                contains(out, "\n  ],\n  \"findings\": []\n}\n"),
            "the JSON of the test:\n" + out);
    log("CHECK test --json: the counts and every run as run --json writes it -> ok");

    // --variant chooses, files come in the order given, and a --param is for the scenarios that have it.
    const Captured chosen = launch(o, {"test", "sim.yaml", "--variant", "medium", "--param", "fill=9", "--scenarios",
                                       "scn/b_fill.yaml", "scn/a_only.yaml", "--json"}, dir);
    expect_code(chosen, 0, "test --variant medium with two files and --param");
    const std::string chosenOut = lf(chosen.standard_output);
    require(contains(chosenOut, "  \"variants\": [\n    \"medium\"\n  ],\n  \"scenarios\": [\n    \"scn/b_fill.yaml\",\n"
                                "    \"scn/a_only.yaml\"\n  ],\n  \"passed\": 2,\n  \"failed\": 0,\n") &&
                contains(chosenOut, "\"text\": \"set Body.body.fill = 9\",") && !contains(chosenOut, "\"variant\": \"small\""),
            "the chosen variant, the files in the order given and the parameter:\n" + chosenOut);
    const Captured typo = launch(o, {"test", "sim.yaml", "--param", "fil=9", "--scenarios", "scn"}, dir);
    expect_code(typo, 4, "test with a --param no scenario has");
    require(contains(lf(typo.standard_error), "error: --param fil=9: none of the 3 scenarios has a parameter fil (did you mean "
                                              "fill?): their parameters are fill; remove the option or correct the name\n") &&
                typo.standard_output.empty() && !contains(lf(typo.standard_error), "test 1 of"),
            "the parameter no scenario has:\n" + lf(typo.standard_error));
    log("CHECK test: --variant, files in the order given, --param for the scenarios that have it -> ok");

    // --fail-fast leaves out what comes after the first failure.
    const Captured fast = launch(o, {"test", "--fail-fast", "--scenarios", "scn", "--", "sim.yaml"}, dir);
    expect_code(fast, 1, "test --fail-fast");
    require(contains(lf(fast.standard_output), "medium   scn/a_only.yaml   not run  a run before it failed (--fail-fast)\n") &&
                contains(lf(fast.standard_output), "\nFAILED: 6 runs, 1 passed, 1 failed, 1 skipped, 3 not run\n") &&
                !contains(lf(fast.standard_error), "test 4 of 6"),
            "--fail-fast:\n" + lf(fast.standard_output));

    // A file without variants: no column for them. --strict fails on the warning of a scenario
    // that checks nothing here, and the others still run.
    write_file(dir / "heater.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - expect: { Body.body.heater: { ge: 0 } }\n");
    const Captured plain = launch(o, {"test", "plain.yaml", "--scenarios", "scn/b_fill.yaml", "heater.yaml"}, dir);
    expect_code(plain, 0, "test a file without variants");
    require(contains(lf(plain.standard_output), "scenario         result\n"
                                                "scn/b_fill.yaml  passed  1 of 3 steps was skipped\n"
                                                "heater.yaml      passed  1 of 1 steps was skipped, 1 warning\n"
                                                "\n"
                                                "passed: 2 runs, 2 passed, 0 failed, 0 skipped\n"),
            "the table without variants:\n" + lf(plain.standard_output));
    const Captured strict = launch(o, {"test", "plain.yaml", "--strict", "--scenarios", "heater.yaml", "scn/b_fill.yaml"}, dir);
    expect_code(strict, 1, "test --strict");
    require(contains(lf(strict.standard_output), "heater.yaml      FAILED  the scenario checks nothing in this configuration") &&
                contains(lf(strict.standard_output), "(a warning, and --strict fails on it)") &&
                contains(lf(strict.standard_output), "\nFAILED: 2 runs, 1 passed, 1 failed, 0 skipped (--strict)\n"),
            "--strict:\n" + lf(strict.standard_output));
    log("CHECK test: --fail-fast, a file without variants, --strict -> ok");

    // The command of one failed run alone has the options of the test that the run takes, quoted
    // for a shell: a --param only for a scenario that has it. It is the same run: it fails as the
    // test's did (here by --strict, on the warning), where the bare command would pass.
    fs::create_directories(dir / "more scn");
    write_file(dir / "more scn" / "heat it.yaml", "vehicle_sil_scenario: 1\nparams: { fill: 1 }\nsteps:\n"
                                                    "  - set: { Body.body.fill: \"${fill}\" }\n"
                                                    "  - expect: { Body.body.heater: { ge: 0 } }\n");
    const Captured again = launch(o, {"test", "plain.yaml", "--strict", "--timeout-ms", "60000", "--param", "fill=4 5", "--scenarios",
                                      "more scn"}, dir);
    expect_code(again, 1, "test --strict with a scenario in a directory with a space");
    require(contains(lf(again.standard_output),
                     "\nto see a failed run in full, run it alone: vehicle_sil_launch run plain.yaml --speed max --scenario "
                     "'more scn/heat it.yaml' --param 'fill=4 5' --strict --timeout-ms 60000\n\n"),
            "the command of the failed run alone:\n" + lf(again.standard_output));
    const Captured alone = launch(o, {"run", "plain.yaml", "--speed", "max", "--scenario", "more scn/heat it.yaml", "--param",
                                      "fill=4 5", "--strict", "--timeout-ms", "60000"}, dir);
    expect_code(alone, 1, "the failed run of a test, alone with the options of the test");
    log("CHECK test: the command of one failed run alone has the options of the test -> ok");

    // Every run skipped (the chosen variant, and the scenario's only): nothing was checked. It
    // says so and passes; --strict fails.
    const Captured nothing = launch(o, {"test", "sim.yaml", "--variant", "small", "--scenarios", "scn/a_only.yaml"}, dir);
    expect_code(nothing, 0, "test in which every run is skipped");
    require(contains(lf(nothing.standard_output),
                     "small    scn/a_only.yaml  skipped  the scenario is only for medium (its only)\n"
                     "\n"
                     "warning: sim.yaml: nothing was run: the 1 run was skipped, because no scenario is for the variant small "
                     "(the only of each scenario; --variant chose what runs). Run a variant the scenarios are for, or correct "
                     "their only. The test checked nothing: it passes (--strict would fail on that) [test-nothing-run]\n"
                     "\n"
                     "passed: 1 run, 0 passed, 0 failed, 1 skipped\n"),
            "a test in which every run is skipped:\n" + lf(nothing.standard_output));
    const Captured nothingStrict = launch(o, {"test", "sim.yaml", "--variant", "small", "--strict", "--json", "--scenarios",
                                              "scn/a_only.yaml"}, dir);
    expect_code(nothingStrict, 1, "test --strict in which every run is skipped");
    require(contains(lf(nothingStrict.standard_output), "  \"result\": \"failed\",\n") &&
                contains(lf(nothingStrict.standard_output), "      \"code\": \"test-nothing-run\",\n"),
            "a --strict test in which every run is skipped:\n" + lf(nothingStrict.standard_output));
    log("CHECK test: every run skipped -> a warning, and a failure with --strict -> ok");

    // What ends a test before its first run.
    const auto refused = [&](const std::vector<std::string>& args, const std::string& message, const std::string& what) {
        const Captured run = launch(o, args, dir);
        expect_code(run, 4, what);
        require(contains(lf(run.standard_error), "error: " + message + "\n") && !contains(lf(run.standard_error), "test 1 of"),
                what + ":\n" + lf(run.standard_error));
    };
    refused({"test", "sim.yaml"},
            "test needs the scenarios to run: give --scenarios <directory> (every .yaml, .yml and .json file directly in it) or "
            "--scenarios <file>... after the configuration file",
            "test without --scenarios");
    refused({"test", "--scenarios", "scn", "sim.yaml"},
            "test needs the configuration file: vehicle_sil_launch test <file> --scenarios <directory or files> (the file comes "
            "before --scenarios, which takes every path after it; or last, after --)",
            "test with the file after --scenarios");
    refused({"test", "sim.yaml", "--scenarios", "scn", "nowhere"},
            "--scenarios nowhere: there is no such file or directory: check the path (it is relative to the current directory, "
            "as the configuration file is)",
            "test with a scenario that is not there");
    fs::create_directories(dir / "none");
    refused({"test", "sim.yaml", "--scenarios", "none"},
            "--scenarios none: the directory has no scenario file (a file directly in it whose name ends in .yaml, .yml or "
            ".json; subdirectories are not searched): name the directory that holds the scenarios, or the files themselves",
            "test with a directory without scenarios");
    refused({"test", "sim.yaml", "--variant", "medium", "--variant", "smal", "--scenarios", "scn"},
            "--variant smal: the file has no variant with this name (did you mean 'small'?); its variants are small, medium",
            "test with a variant the file does not have");
    refused({"test", "plain.yaml", "--variant", "small", "--scenarios", "scn"},
            "--variant small: the file defines no variants: leave --variant out, or add variants to the file "
            "(docs/simulation-config.md)",
            "test --variant with a file without variants");
    refused({"test", "sim.yaml", "--scenario", "scn/b_fill.yaml"},
            "--scenario is for check and run: test takes its scenarios with --scenarios <directory or files>",
            "test --scenario");
    refused({"test", "sim.yaml", "--speed", "2", "--scenarios", "scn"},
            "--speed is for run, not for test (which runs one simulation after the other, each as fast as possible, or at speed 1 if a role: bridge participant connects it outside the simulation): to use "
            "it, run one scenario with vehicle_sil_launch run --scenario <file>",
            "test --speed");
    // A participant that someone starts by hand: refused before the first run (each run would
    // wait for a person); left out with --disable, the test runs.
    write_file(dir / "manual.yaml", participants + "  - name: Hil\n    command: [" + ecu(o) + "]\n    manual: true\n");
    refused({"test", "manual.yaml", "--scenarios", "scn/b_fill.yaml"},
            "participant 'Hil' is manual (manual: true in the file): someone starts it by hand, and test starts every run by "
            "itself, so each run would wait for it: leave it out with --disable Hil (a scenario with a step that uses its "
            "variables then fails its check), or run one scenario with it yourself: vehicle_sil_launch run <file> --scenario "
            "<file>",
            "test with a manual participant");
    const Captured unmanned = launch(o, {"test", "manual.yaml", "--disable", "Hil", "--scenarios", "scn/b_fill.yaml"}, dir);
    expect_code(unmanned, 0, "test with the manual participant left out");
    const Captured usage = launch(o, {"test", "sim.yaml", "--json"}, dir);
    expect_code(usage, 4, "test --json without --scenarios");
    require(lf(usage.standard_output).rfind("{\n  \"vehicle_sil_test\": 1,\n", 0) == 0 &&
                contains(lf(usage.standard_output), "  \"file\": \"sim.yaml\",\n  \"result\": \"usage\",\n") &&
                contains(lf(usage.standard_output), "\"runs\": [],\n  \"findings\": [\n    {\n      \"severity\": \"error\",\n"
                                                    "      \"code\": \"usage\",\n"),
            "the JSON of a usage error of test:\n" + lf(usage.standard_output));
    const Captured unreadable = launch(o, {"test", "nowhere.yaml", "--scenarios", "scn"}, dir);
    expect_code(unreadable, 3, "test with a file that is not there");
    expect_text(lf(unreadable.standard_output),
                "error: nowhere.yaml: the file does not exist: check the path [file]\n\ncannot read nowhere.yaml: nothing was run\n",
                "test with a file that is not there");

    // Named cases with Unicode + single-dash (-10C), --case filtering, template failure provenance & single-case re-run.
    fs::create_directories(dir / "scn_cases" / "templates");
    write_file(dir / "scn_cases" / "templates" / "fill_check.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  target: { type: integer }\n"
               "  wait_ms: { type: integer, default: 20 }\n"
               "  label: { type: string, default: check }\n"
               "steps:\n"
               "  - name: \"${label}: set fill\"\n"
               "    set: { Body.body.fill: \"${target}\" }\n"
               "  - name: \"${label}: check fill\"\n"
               "    after_ms: \"${wait_ms}\"\n"
               "    expect: { Body.body.fill: \"${target}\" }\n");
    write_file(dir / "scn_cases" / "load_cases.yaml",
               "vehicle_sil_scenario: 1\n"
               "name: load cases\n"
               "params: { target: 5, expected_fill: 5 }\n"
               "cases:\n"
               "  \"-10C\": { params: { target: 4 } }\n"
               "  定格: { only: [medium], params: { target: 7 } }\n"
               "steps:\n"
               "  - template: templates/fill_check.yaml\n"
               "    params: { target: \"${target}\", label: \"run\" }\n"
               "  - name: verify target\n"
               "    expect: { Body.body.fill: \"${expected_fill}\" }\n");

    // Passing case alone via --case -10C + --param expected_fill=4 (single-dash case name works end-to-end).
    const Captured singleCase = launch(o,
                                       {"run", "sim.yaml", "--variant", "small", "--scenario", "scn_cases/load_cases.yaml",
                                        "--case", "-10C", "--param", "expected_fill=4", "--json"},
                                       dir);
    expect_code(singleCase, 0, "run single case -10C");
    require(contains(lf(singleCase.standard_output), "\"case\": \"-10C\",") &&
                contains(lf(singleCase.standard_output), "\"from\": \"case\""),
            "single case JSON:\n" + lf(singleCase.standard_output));

    // Batch test --scenarios <dir> --case -10C where -10C fails (target 4 != expected_fill 5) and prints run_alone() with --case -10C.
    const Captured caseTest = launch(o, {"test", "sim.yaml", "--variant", "small", "--scenarios", "scn_cases", "--case", "-10C"}, dir);
    expect_code(caseTest, 1, "test --scenarios dir --case -10C");
    require(contains(lf(caseTest.standard_output), "to see a failed run in full, run it alone: vehicle_sil_launch run sim.yaml "
                                                   "--speed max --variant small --scenario scn_cases/load_cases.yaml --case -10C"),
            "run_alone with --case -10C:\n" + lf(caseTest.standard_output));

    // Omitting --case runs -10C and skips 定格 (only: [medium]) on small, with "case": "定格" after "run": null in JSON.
    const Captured caseAllSmall = launch(o, {"test", "sim.yaml", "--variant", "small", "--scenarios", "scn_cases", "--json"}, dir);
    expect_code(caseAllSmall, 1, "test --scenarios dir --variant small --json");
    require(contains(lf(caseAllSmall.standard_output), "\"result\": \"skipped\",\n      \"reason\": \"the case is only for medium (its only)\",\n      \"run\": null,\n      \"case\": \"定格\""),
            "skipped case in JSON:\n" + lf(caseAllSmall.standard_output));

    // Single-scenario unknown --case reports "the scenario has no case with this name" (exit 4).
    const Captured unknownCaseCli = launch(
        o, {"test", "sim.yaml", "--variant", "small", "--scenarios", "scn_cases/load_cases.yaml", "--case", "定格x"}, dir);
    expect_code(unknownCaseCli, 4, "test single-scenario unknown --case");
    require(contains(lf(unknownCaseCli.standard_error),
                     "--case 定格x: the scenario has no case with this name (did you mean '定格'?)"),
            "single-scenario unknown --case stderr:\n" + lf(unknownCaseCli.standard_error));

    // Broken cases: {} fails test loudly (exit 1), never exits 0.
    fs::create_directories(dir / "bad_cases");
    write_file(dir / "bad_cases" / "empty.yaml", "vehicle_sil_scenario: 1\ncases: {}\nsteps:\n  - set: { Body.body.fill: 1 }\n");
    const Captured emptyCasesCli = launch(
        o, {"test", "sim.yaml", "--variant", "small", "--scenarios", "bad_cases/empty.yaml"}, dir);
    expect_code(emptyCasesCli, 1, "test broken cases: {}");
    require(contains(lf(emptyCasesCli.standard_output), "cases must be a non-empty mapping") &&
                contains(lf(emptyCasesCli.standard_output), "FAILED: "),
            "broken cases: {} stdout:\n" + lf(emptyCasesCli.standard_output));
    log("CHECK test: what ends it before its first run (usage, an unreadable file) -> ok");
}

/// vehicle_sil_launch test, interrupted: the current run is stopped in order, the rest is not
/// run, and the table says so.
void test_interrupt(const Options& o)
{
#ifdef _WIN32
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through");
    }
    const std::string by = "Ctrl+Break";
#else
    const std::string by = "SIGINT";
#endif
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                                      "  - name: Body\n    command: [" + ecu(o) + "]\n",
                                      {{"Body", "scenario-variables"}});
    // Far too long to end by itself, also as fast as possible.
    write_file(dir / "long.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - at_ms: 100000000000\n    expect: { Body.ticks: { ge: 0 } }\n");
    write_file(dir / "short.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - at_ms: 20\n    expect: { Body.ticks: { ge: 0 } }\n");
    const auto [status, out] = run_until_running(
        o, dir,
        [](vehicle_sil::process::Process& launcher) {
#ifdef _WIN32
            require(launcher.request_stop(), "Ctrl+Break could not be sent");
#else
            ::kill(static_cast<pid_t>(launcher.id()), SIGINT);
#endif
        },
        {"test", "sim.yaml", "--scenarios", "long.yaml", "short.yaml"});
    require(status.has_value() && status->code == 130 && !status->signal,
            "the launcher ends with 130 when a test is interrupted, not " + (status ? std::to_string(status->code) : std::string("running")));
    expect_text(lf(out),
                "scenario    result\n"
                "long.yaml   interrupted  by " + by + "\n"
                "short.yaml  not run      the test was interrupted before it\n"
                "\n"
                "interrupted: 2 runs, 0 passed, 0 failed, 0 skipped, 1 interrupted, 1 not run\n",
                "test interrupted");
}

namespace {

/// Replaces the top-level `"file": "<from>"` field in a timeline JSON string for cross-config comparison.
std::string replace_top_file(std::string text, const std::string& from, const std::string& to)
{
    const std::string needle = "\"file\": \"" + from + "\"";
    const auto pos = text.find(needle);
    require(pos != std::string::npos, "missing top-level file field " + needle);
    text.replace(pos, needle.size(), "\"file\": \"" + to + "\"");
    return text;
}

/// Extracts the `"series": [...]` section from a pretty-printed timeline JSON.
std::string timeline_series_json(const std::string& json)
{
    const auto start = json.find("  \"series\": [");
    require(start != std::string::npos, "timeline JSON has no series array:\n" + json);
    const auto end = json.find("\n  \"lines\": [", start);
    require(end != std::string::npos, "timeline JSON has no lines array after series:\n" + json);
    return json.substr(start, end - start);
}

/// Extracts the `"over_at_ms": ...` line from a scenario JSON fragment.
std::string over_at_ms_line(const std::string& scen_json)
{
    const auto pos = scen_json.find("\"over_at_ms\": ");
    require(pos != std::string::npos, "scenario JSON has no over_at_ms:\n" + scen_json);
    const auto nl = scen_json.find('\n', pos);
    return scen_json.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
}

/// Extracts per-step timing and action fields `(kind, text, start_ms, at_ms, end_ms)` from scenario JSON.
std::string scenario_step_summary(const std::string& scen_json)
{
    std::string out;
    std::size_t pos = 0;
    while ((pos = scen_json.find("\"kind\": ", pos)) != std::string::npos)
    {
        for (const char* key : {"\"kind\": ", "\"text\": ", "\"start_ms\": ", "\"at_ms\": ", "\"end_ms\": "})
        {
            const auto k = scen_json.find(key, pos);
            require(k != std::string::npos, std::string("missing ") + key + " in scenario JSON:\n" + scen_json);
            const auto nl = scen_json.find('\n', k);
            out += scen_json.substr(k, nl == std::string::npos ? std::string::npos : nl - k) + "\n";
            pos = nl == std::string::npos ? scen_json.size() : nl + 1;
        }
    }
    require(!out.empty(), "scenario JSON has no steps:\n" + scen_json);
    return out;
}

} // namespace

/// Multi-ECU template + Japanese/space case names, repeatable --case filtering, template dither
/// failure, variant calibration matrix (only, absent: true, scale, nested templates), and
/// run_alone() single-case re-run verification.
void template_cases_matrix(const Options& o)
{
    // Part A: Three ECUs (including glitch 150 on Engine), shared power-on template, and Japanese/space cases.
    const std::string multi_ecu =
        "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
        "  - name: Power\n    command: [" + ecu(o) + "]\n"
        "  - name: Engine\n    command: [" + ecu(o) + "]\n"
        "  - name: Body\n    command: [" + ecu(o) + "]\n";
    const fs::path dir_a = scenario_dir(
        o, multi_ecu,
        {{"Power", "scenario-variables"}, {"Engine", "scenario-variables glitch 150"}, {"Body", "scenario-variables"}});
    fs::create_directories(dir_a / "scenarios" / "templates");
    write_file(dir_a / "scenarios" / "templates" / "power_on_neutral.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "name: Power-on, comms establish, neutral check\n"
               "parameters:\n"
               "  comms_timeout_ms: { type: integer, default: 2000 }\n"
               "  neutral_mode: { type: string, default: auto, values: [off, auto] }\n"
               "steps:\n"
               "  - name: 電源ON\n"
               "    set: { Power.mode: on, Body.mode: \"${neutral_mode}\" }\n"
               "  - name: 通信確立待ち\n"
               "    wait_until: { Engine.ticks: { ge: 12 } }\n"
               "    timeout_ms: \"${comms_timeout_ms}\"\n"
               "  - name: レバー中立確認\n"
               "    after_ms: 50\n"
               "    expect: { Body.mode_now: \"${neutral_mode}\" }\n"
               "    within_ms: 500\n");
    write_file(dir_a / "scenarios" / "normal.yaml",
               "vehicle_sil_scenario: 1\n"
               "name: 正常系走行\n"
               "steps:\n"
               "  - name: 初期化（電源ON〜通信確立〜レバー中立）\n"
               "    template: templates/power_on_neutral.yaml\n"
               "  - name: 負荷印加\n"
               "    set: { Body.level: 40 }\n");
    write_file(dir_a / "scenarios" / "comms_loss.yaml",
               "vehicle_sil_scenario: 1\n"
               "name: 通信断リカバリ\n"
               "steps:\n"
               "  - name: 初期化（電源ON〜通信確立〜レバー中立）\n"
               "    template: templates/power_on_neutral.yaml\n"
               "  - name: 一時通信断を許容し復帰確認\n"
               "    wait_until: { Engine.healthy: true }\n"
               "    timeout_ms: 400\n");
    write_file(dir_a / "scenarios" / "load_variation.yaml",
               "vehicle_sil_scenario: 1\n"
               "name: 負荷変動試験\n"
               "params:\n"
               "  boot_timeout_ms: 2000\n"
               "  load_pct: 50\n"
               "cases:\n"
               "  無負荷:\n"
               "    params: { boot_timeout_ms: 2000, load_pct: 0 }\n"
               "  定格:\n"
               "    params: { boot_timeout_ms: 2000, load_pct: 50 }\n"
               "  \"上限 直前\":\n"
               "    params: { boot_timeout_ms: 50, load_pct: 95 }\n"
               "steps:\n"
               "  - name: 初期化（電源ON〜通信確立〜レバー中立）\n"
               "    template: templates/power_on_neutral.yaml\n"
               "    params: { comms_timeout_ms: \"${boot_timeout_ms}\" }\n"
               "  - name: 負荷設定\n"
               "    set: { Body.level: \"${load_pct}\" }\n");

    const Captured all_json = launch(o, {"test", "sim.yaml", "--scenarios", "scenarios/", "--json"}, dir_a);
    expect_code(all_json, 1, "multi-ECU batch test with shared template and Japanese/space cases");
    const std::string all_out = lf(all_json.standard_output);
    require(contains(all_out, "\"passed\": 4,\n  \"failed\": 1,\n  \"skipped\": 0,") &&
                contains(all_out, "\"result\": \"failed\",") && contains(all_out, "\"case\": \"上限 直前\""),
            "multi-ECU batch JSON summary:\n" + all_out);

    // Repeatable --case filter selects only 無負荷 and "上限 直前", and text output includes template file:line + run_alone().
    const Captured filtered = launch(
        o, {"test", "sim.yaml", "--scenarios", "scenarios/load_variation.yaml", "--case", "無負荷", "--case", "上限 直前"}, dir_a);
    expect_code(filtered, 1, "test with repeatable --case filter including space/Japanese case");
    const std::string filt_out = lf(filtered.standard_output);
    require(contains(filt_out, "無負荷") && contains(filt_out, "上限 直前") && !contains(filt_out, "定格") &&
                contains(filt_out, "通信確立待ち") &&
                contains(filt_out, "scenarios/templates/power_on_neutral.yaml:") &&
                contains(filt_out,
                         "to see a failed run in full, run it alone: vehicle_sil_launch run sim.yaml --speed max "
                         "--scenario scenarios/load_variation.yaml --case '上限 直前'") &&
                contains(filt_out, "FAILED: 2 runs, 1 passed, 1 failed, 0 skipped"),
            "repeatable --case filtered text output:\n" + filt_out);

    // Reproduce the failed case alone using the options printed by run_alone().
    const Captured rerun = launch(
        o, {"run", "sim.yaml", "--speed", "max", "--scenario", "scenarios/load_variation.yaml", "--case", "上限 直前"}, dir_a);
    expect_code(rerun, 1, "re-run failed Japanese/space case alone");
    const std::string rerun_all = lf(rerun.standard_output) + "\n" + lf(rerun.standard_error);
    require(contains(rerun_all, "上限 直前") && contains(rerun_all, "通信確立待ち") &&
                contains(rerun_all, "初期化（電源ON〜通信確立〜レバー中立）") &&
                contains(rerun_all, "scenarios/templates/power_on_neutral.yaml:9") &&
                contains(rerun_all, "scenarios/load_variation.yaml:"),
            "single-case failure attribution and call stack:\n" + rerun_all);
    log("CHECK multi-ECU glitch + shared template + Japanese/space --case filter & run_alone() -> ok");

    // Part B: Variant calibration matrix (only, absent: true, scale, nested templates) + template dither failure.
    const fs::path dir_b = o.work_dir / "matrix";
    fs::create_directories(dir_b / "scenarios" / "templates");
    write_file(dir_b / "Body.behaviour", "scenario-variables\n");
    write_file(dir_b / "sim.yaml",
               "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
               "  - name: Body\n    command: [" + ecu(o) + "]\n"
               "variants:\n"
               "  prototype:\n"
               "    params: { cal_target: 0.4, min_ticks: 1 }\n"
               "    participants:\n"
               "      Body:\n"
               "        variables:\n"
               "          heater: { absent: true }\n"
               "          throttle: { from: level, scale: 0.01 }\n"
               "  production:\n"
               "    params: { cal_target: 60, min_ticks: 9999 }\n"
               "    participants:\n"
               "      Body:\n"
               "        variables:\n"
               "          heater: { from: ticks }\n"
               "          throttle: { from: level, scale: 1.0 }\n");
    write_file(dir_b / "scenarios" / "templates" / "heater_check.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  min_heater: { type: integer }\n"
               "steps:\n"
               "  - expect: { Body.heater: { ge: \"${min_heater}\" } }\n"
               "    within_ms: 100\n");
    write_file(dir_b / "scenarios" / "templates" / "init.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  target: { type: number }\n"
               "  min_heater: { type: integer }\n"
               "steps:\n"
               "  - set: { Body.throttle: \"${target}\" }\n"
               "  - template: heater_check.yaml\n"
               "    params: { min_heater: \"${min_heater}\" }\n");
    write_file(dir_b / "scenarios" / "calib.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { cal_target: 20, min_heater: 1 }\n"
               "cases:\n"
               "  nominal: { params: { min_heater: 1 } }\n"
               "  cold_soak:\n"
               "    only: [production]\n"
               "    params: { cal_target: 75, min_heater: 2 }\n"
               "steps:\n"
               "  - template: templates/init.yaml\n"
               "    params: { target: \"${cal_target}\", min_heater: \"${min_heater}\" }\n");

    const Captured calib = launch(o, {"test", "sim.yaml", "--scenarios", "scenarios/calib.yaml", "--json"}, dir_b);
    expect_code(calib, 0, "calibration matrix with only, absent: true, scale, and nested templates");
    const std::string calib_out = lf(calib.standard_output);
    require(contains(calib_out, "\"passed\": 3,\n  \"failed\": 0,\n  \"skipped\": 1,") &&
                contains(calib_out, "\"variant\": \"prototype\",\n      \"scenario\": \"scenarios/calib.yaml\",\n"
                                    "      \"result\": \"skipped\",\n      \"reason\": \"the case is only for production (its only)\",\n"
                                    "      \"run\": null,\n      \"case\": \"cold_soak\"") &&
                contains(calib_out, "\"status\": \"skipped\",") &&
                contains(calib_out, "scenarios/templates/heater_check.yaml"),
            "calibration matrix JSON:\n" + calib_out);

    // Template dither step + failing expect under (variant=production, case=定格) + run_alone() re-run.
    write_file(dir_b / "scenarios" / "templates" / "dither_check.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  min_ticks: { type: integer }\n"
               "steps:\n"
               "  - dither: { Body.throttle: { triangle: { amplitude: 2, period_ms: 80 } } }\n"
               "  - name: Tolerance check\n"
               "    expect: { Body.heater: { ge: \"${min_ticks}\" } }\n"
               "    within_ms: 30\n");
    write_file(dir_b / "scenarios" / "dither_fail.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { min_ticks: 1 }\n"
               "cases:\n"
               "  無負荷: {}\n"
               "  定格: {}\n"
               "steps:\n"
               "  - template: templates/dither_check.yaml\n"
               "    params: { min_ticks: \"${min_ticks}\" }\n");
    const Captured dither_test = launch(
        o, {"test", "sim.yaml", "--variant", "production", "--case", "定格", "--scenarios", "scenarios/dither_fail.yaml"}, dir_b);
    expect_code(dither_test, 1, "template dither failure under variant + Japanese case");
    const std::string dither_out = lf(dither_test.standard_output);
    require(contains(dither_out, "scenarios/templates/dither_check.yaml:") &&
                contains(dither_out,
                         "to see a failed run in full, run it alone: vehicle_sil_launch run sim.yaml --speed max "
                         "--variant production --scenario scenarios/dither_fail.yaml --case '定格'"),
            "template dither failure output:\n" + dither_out);
    const Captured dither_alone = launch(
        o, {"run", "sim.yaml", "--speed", "max", "--variant", "production", "--scenario", "scenarios/dither_fail.yaml", "--case", "定格"},
        dir_b);
    expect_code(dither_alone, 1, "re-run template dither failure alone");
    require(contains(lf(dither_alone.standard_output), "定格") &&
                contains(lf(dither_alone.standard_output), "scenarios/templates/dither_check.yaml:6"),
            "template dither run_alone output:\n" + lf(dither_alone.standard_output));
    log("CHECK calibration matrix (only, absent, scale, nested templates) and template dither failure -> ok");
}

/// Case-bound CSV points inside templates with variant scale, dither sweep, and virtual-time
/// determinism across --speed 0.5, --speed max, and slow-ms 5.
void template_csv_determinism(const Options& o)
{
    const fs::path dir = o.work_dir / "csv_det";
    fs::create_directories(dir / "scenarios" / "templates" / "traces");
    write_file(dir / "Body.behaviour", "scenario-variables\n");
    write_file(dir / "sim_scaled.yaml",
               "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
               "  - name: Body\n    command: [" + ecu(o) + "]\n"
               "variants:\n"
               "  scaled:\n"
               "    participants:\n"
               "      Body:\n"
               "        variables:\n"
               "          fill: { from: level, scale: 0.01 }\n"
               "  raw:\n"
               "    participants:\n"
               "      Body:\n"
               "        variables:\n"
               "          fill: { from: level, scale: 1.0 }\n");
    write_file(dir / "scenarios" / "templates" / "traces" / "low.csv", "ms,val\n0,0.1\n40,0.3\n");
    write_file(dir / "scenarios" / "templates" / "traces" / "high.csv", "ms,val\n0,0.4\n40,0.8\n");
    write_file(dir / "scenarios" / "templates" / "csv_wave.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  profile: { type: string }\n"
               "steps:\n"
               "  - wave:\n"
               "      Body.fill:\n"
               "        points: { file: \"traces/${profile}.csv\", value: val }\n");
    write_file(dir / "scenarios" / "sweep.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { profile: low }\n"
               "cases:\n"
               "  low_case: { params: { profile: low } }\n"
               "  high_case: { params: { profile: high } }\n"
               "steps:\n"
               "  - template: templates/csv_wave.yaml\n"
               "    params: { profile: \"${profile}\" }\n");

    const Captured scaled_res = launch(
        o, {"test", "sim_scaled.yaml", "--variant", "scaled", "--scenarios", "scenarios/sweep.yaml", "--json"}, dir);
    expect_code(scaled_res, 0, "template CSV points bound by cases with variant scale");
    require(contains(lf(scaled_res.standard_output), "\"passed\": 2,\n  \"failed\": 0,\n  \"skipped\": 0,"),
            "scaled CSV cases JSON:\n" + lf(scaled_res.standard_output));

    // Three CSV trace cases + template dither + --speed 0.5 / --speed max / slow-ms 5 timeline determinism.
    write_file(dir / "scenarios" / "templates" / "traces" / "city.csv", "ms,throttle\n0,10\n40,30\n");
    write_file(dir / "scenarios" / "templates" / "traces" / "highway.csv", "ms,throttle\n0,40\n40,70\n");
    write_file(dir / "scenarios" / "templates" / "traces" / "hill_climb.csv", "ms,throttle\n0,60\n40,90\n");
    write_file(dir / "scenarios" / "templates" / "drive_cycle.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  trace_csv: { type: string }\n"
               "  dither_amp: { type: number }\n"
               "  target_ticks: { type: integer }\n"
               "steps:\n"
               "  - dither: { Body.level: { triangle: { amplitude: \"${dither_amp}\", period_ms: 80 } } }\n"
               "  - wave:\n"
               "      Body.level:\n"
               "        points: { file: \"${trace_csv}\", value: throttle }\n"
               "  - wait_until: { Body.ticks: { ge: \"${target_ticks}\" } }\n"
               "    timeout_ms: 200\n"
               "  - expect: { Body.ticks: { ge: \"${target_ticks}\" } }\n"
               "    hold_for_ms: 20\n");
    write_file(dir / "scenarios" / "endurance.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { trace_csv: \"traces/city.csv\", dither_amp: 1.5, target_ticks: 2 }\n"
               "cases:\n"
               "  city: { params: { trace_csv: \"traces/city.csv\", dither_amp: 1.0, target_ticks: 2 } }\n"
               "  highway: { params: { trace_csv: \"traces/highway.csv\", dither_amp: 1.5, target_ticks: 2 } }\n"
               "  hill_climb: { params: { trace_csv: \"traces/hill_climb.csv\", dither_amp: 2.0, target_ticks: 2 } }\n"
               "steps:\n"
               "  - template: templates/drive_cycle.yaml\n"
               "    params: { trace_csv: \"${trace_csv}\", dither_amp: \"${dither_amp}\", target_ticks: \"${target_ticks}\" }\n");
    write_file(dir / "sim_fast.yaml",
               "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
               "  - name: Body\n    command: [" + ecu(o) + "]\n");

    const Captured sweep = launch(o, {"test", "sim_fast.yaml", "--scenarios", "scenarios/endurance.yaml", "--json"}, dir);
    expect_code(sweep, 0, "three CSV trace cases with template dither");
    const std::string sweep_out = lf(sweep.standard_output);
    const auto p_city = sweep_out.find("\"case\": \"city\"");
    const auto p_hwy = sweep_out.find("\"case\": \"highway\"");
    const auto p_hill = sweep_out.find("\"case\": \"hill_climb\"");
    require(contains(sweep_out, "\"passed\": 3,\n  \"failed\": 0,\n  \"skipped\": 0,") &&
                p_city != std::string::npos && p_hwy != std::string::npos && p_hill != std::string::npos &&
                p_city < p_hwy && p_hwy < p_hill,
            "endurance cases order and pass counts:\n" + sweep_out);

    expect_code(launch(o, {"run", "sim_fast.yaml", "--scenario", "scenarios/endurance.yaml", "--case", "highway",
                           "--speed", "0.5", "--timeline", "tl_05.json"}, dir),
                0, "endurance highway --speed 0.5");
    expect_code(launch(o, {"run", "sim_fast.yaml", "--scenario", "scenarios/endurance.yaml", "--case", "highway",
                           "--speed", "max", "--timeline", "tl_max.json"}, dir),
                0, "endurance highway --speed max");
    const std::string tl_05 = lf(read_text(dir / "tl_05.json"));
    const std::string tl_max = lf(read_text(dir / "tl_max.json"));
    expect_text(tl_05, tl_max, "timeline JSON across --speed 0.5 and --speed max");

    // Same run with slow-ms 5 ECU behaviour in a separate directory: identical timeline after normalizing file name.
    const fs::path dir_slow = o.work_dir / "csv_det_slow";
    fs::create_directories(dir_slow / "scenarios" / "templates" / "traces");
    write_file(dir_slow / "Body.behaviour", "scenario-variables slow-ms 5\n");
    write_file(dir_slow / "sim_slow.yaml",
               "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
               "  - name: Body\n    command: [" + ecu(o) + "]\n");
    for (const char* name : {"city.csv", "highway.csv", "hill_climb.csv"})
    {
        fs::copy_file(dir / "scenarios" / "templates" / "traces" / name,
                      dir_slow / "scenarios" / "templates" / "traces" / name,
                      fs::copy_options::overwrite_existing);
    }
    fs::copy_file(dir / "scenarios" / "templates" / "drive_cycle.yaml",
                  dir_slow / "scenarios" / "templates" / "drive_cycle.yaml", fs::copy_options::overwrite_existing);
    fs::copy_file(dir / "scenarios" / "endurance.yaml", dir_slow / "scenarios" / "endurance.yaml",
                  fs::copy_options::overwrite_existing);
    expect_code(launch(o, {"run", "sim_slow.yaml", "--scenario", "scenarios/endurance.yaml", "--case", "highway",
                           "--speed", "max", "--timeline", "tl_slow.json"}, dir_slow),
                0, "endurance highway with slow-ms 5");
    const std::string tl_slow = replace_top_file(lf(read_text(dir_slow / "tl_slow.json")), "sim_slow.yaml", "sim_fast.yaml");
    expect_text(tl_slow, tl_max, "timeline JSON across fast ECU and slow-ms 5 ECU");
    log("CHECK case-bound CSV points, variant scale, and virtual-time determinism (--speed 0.5 / max / slow-ms 5) -> ok");
}

/// Cases with hex integer params + --param CLI string override, direct vs template+case equivalence
/// on real Engine+Dashboard ECUs, and serve --once Events v4 provenance for template steps.
void template_equivalence_and_serve(const Options& o)
{
    // Part A: cases + hex integer param (0x10 -> 16) + --param string override + serve --once Events v4 provenance.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                                      "  - name: Body\n    command: [" + ecu(o) + "]\n",
                                      {{"Body", "scenario-variables"}});
    fs::create_directories(dir / "templates");
    write_file(dir / "templates" / "apply.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  mode_name: { type: string }\n"
               "  hex_val: { type: integer }\n"
               "steps:\n"
               "  - set: { Body.mode: \"${mode_name}\", Body.level: \"${hex_val}\" }\n"
               "  - expect: { Body.mode_now: auto }\n"
               "    within_ms: 100\n");
    write_file(dir / "hex_cases.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { mode_name: off, hex_val: 0x10 }\n"
               "cases:\n"
               "  c1: { params: { mode_name: off, hex_val: 0x10 } }\n"
               "steps:\n"
               "  - template: templates/apply.yaml\n"
               "    params: { mode_name: \"${mode_name}\", hex_val: \"${hex_val}\" }\n");
    const Captured hex_run = launch(
        o, {"run", "sim.yaml", "--scenario", "hex_cases.yaml", "--case", "c1", "--param", "mode_name=auto", "--json"}, dir);
    expect_code(hex_run, 0, "run cases + hex param + --param string override");
    const std::string hex_scn = scenario_json(lf(hex_run.standard_output));
    require(contains(hex_scn, "\"case\": \"c1\",") && contains(hex_scn, "Body.level = 16") &&
                contains(hex_scn, "Body.mode = auto"),
            "hex + --param override scenario JSON:\n" + hex_scn);

    // Interpolated Japanese step name in template: run --json with --case and serve --once Events v4 without case.
    write_file(dir / "templates" / "stage.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  stage: { type: string }\n"
               "steps:\n"
               "  - name: \"確認 (${stage})\"\n"
               "    set: { Body.level: 30 }\n");
    write_file(dir / "stage_cased.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { stage: base }\n"
               "cases:\n"
               "  定格: { params: { stage: \"定格\" } }\n"
               "steps:\n"
               "  - name: Run stage\n"
               "    template: templates/stage.yaml\n"
               "    params: { stage: \"${stage}\" }\n");
    const Captured cased_run = launch(
        o, {"run", "sim.yaml", "--scenario", "stage_cased.yaml", "--case", "定格", "--json"}, dir);
    expect_code(cased_run, 0, "run --json with Japanese case and interpolated template step name");
    const std::string cased_scn = scenario_json(lf(cased_run.standard_output));
    require(contains(cased_scn, "\"case\": \"定格\",") && contains(cased_scn, "\"name\": \"確認 (定格)\",") &&
                contains(cased_scn, "\"file\": \"templates/stage.yaml\",") && contains(cased_scn, "\"call_stack\": ["),
            "cased template run JSON:\n" + cased_scn);

    write_file(dir / "stage_caseless.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { stage: base }\n"
               "steps:\n"
               "  - name: Run stage\n"
               "    template: templates/stage.yaml\n"
               "    params: { stage: \"${stage}\" }\n");
    const Captured served = launch(
        o, {"serve", "--once", "sim.yaml", "--scenario", "stage_caseless.yaml", "--param", "stage=定格", "--speed", "max"},
        dir);
    expect_code(served, 0, "serve --once with caseless template scenario and params");
    const std::vector<std::string> evts = check_events(lf(served.standard_output));
    const auto step_evts = events_with(
        evts, {"\"event\":\"scenario_step\"", "\"name\":\"確認 (定格)\"", "\"file\":\"templates/stage.yaml\"", "\"call_stack\":["});
    const auto result_evts = events_with(evts, {"\"event\":\"result\"", "\"name\":\"確認 (定格)\""});
    require(!step_evts.empty() && !result_evts.empty(),
            "serve --once Events v4 template provenance:\n" + lf(served.standard_output));
    log("CHECK cases + hex param + --param override and serve --once Events v4 template provenance -> ok");

    // Part B: Direct scenario vs Template + --case equivalence on real Engine + Dashboard ECUs.
    const fs::path ex = example_copy(o);
    fs::create_directories(ex / "scenarios" / "templates");
    write_file(ex / "scenarios" / "templates" / "throttle_check.yaml",
               "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  throttle: { type: integer }\n"
               "  min_speed: { type: integer }\n"
               "steps:\n"
               "  - set: { Engine.engine.throttle: \"${throttle}\" }\n"
               "  - wait_until: { Engine.engine.speed: { ge: \"${min_speed}\" } }\n"
               "    timeout_ms: 500\n"
               "  - after_ms: 50\n"
               "    expect: { Dashboard.dashboard.engine_speed: { ge: \"${min_speed}\" } }\n"
               "    within_ms: 200\n");
    write_file(ex / "scenarios" / "direct.yaml",
               "vehicle_sil_scenario: 1\n"
               "steps:\n"
               "  - set: { Engine.engine.throttle: 50 }\n"
               "  - wait_until: { Engine.engine.speed: { ge: 1700 } }\n"
               "    timeout_ms: 500\n"
               "  - after_ms: 50\n"
               "    expect: { Dashboard.dashboard.engine_speed: { ge: 1700 } }\n"
               "    within_ms: 200\n");
    write_file(ex / "scenarios" / "templated.yaml",
               "vehicle_sil_scenario: 1\n"
               "params: { throttle: 50, min_speed: 1700 }\n"
               "cases:\n"
               "  rated: { params: { throttle: 50, min_speed: 1700 } }\n"
               "steps:\n"
               "  - template: templates/throttle_check.yaml\n"
               "    params: { throttle: \"${throttle}\", min_speed: \"${min_speed}\" }\n");

    const Captured r_direct = launch(
        o, {"run", "simulation.yaml", "--speed", "max", "--scenario", "scenarios/direct.yaml", "--json", "--timeline", "tl_direct.json"},
        ex);
    expect_code(r_direct, 0, "direct Engine+Dashboard scenario");
    const Captured r_tpl = launch(
        o,
        {"run", "simulation.yaml", "--speed", "max", "--scenario", "scenarios/templated.yaml", "--case", "rated", "--json",
         "--timeline", "tl_tpl.json"},
        ex);
    expect_code(r_tpl, 0, "template + --case Engine+Dashboard scenario");
    const std::string direct_scn = scenario_json(lf(r_direct.standard_output));
    const std::string tpl_scn = scenario_json(lf(r_tpl.standard_output));
    expect_text(over_at_ms_line(tpl_scn), over_at_ms_line(direct_scn), "direct vs template+case over_at_ms");
    expect_text(scenario_step_summary(tpl_scn), scenario_step_summary(direct_scn), "direct vs template+case steps");
    expect_text(timeline_series_json(lf(read_text(ex / "tl_tpl.json"))),
                timeline_series_json(lf(read_text(ex / "tl_direct.json"))),
                "direct vs template+case timeline series");
    log("CHECK direct vs template + --case equivalence on Engine+Dashboard -> ok");
}
