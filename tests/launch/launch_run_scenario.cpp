// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about run --scenario: the example scenarios, the steps (wave, dither,
// expect), the timing of the writes, a scenario that fails:
//
//   scenario     run --scenario with the example of examples/ecu_main and its output in
//                docs/scenario.md (text, byte for byte); the scenario in the JSON and in the events;
//                the examples of scenario.md with mistakes (check) and that fails (text, the
//                scenario of the JSON)
//   live         examples/ecu_main/simulation.live.yaml (speed 1, no stop_at_ms): the check, a run past
//                1000 ms and a stop command
//   scenario_wave  the steps "wave": what the ECU saw of each shape, the same at a speed and with
//                  another controller step, the warning of the check and "undersampled: ok", and
//                  the failure of a wave that leaves the variable's range; the example with waves
//   scenario_dither  a dither on an input (the scenario's "inputs" and the step "dither"): what the
//                  ECU saw, the same with another controller step and at a speed; the failure of a
//                  dither that leaves the variable's range; the example with a dither and its timeline
//   scenario_expect_beside  an expect beside a ramp: it holds, the sample that breaks it, and the
//                note, the line and expect_skipped where its variable is absent
//   scenario_same  one scenario, run as fast as possible, at a speed, with a slow ECU and with a
//                pause in between: the same result each time; every write takes effect in the step
//                of its time (the ECU says in which step it saw each value)
//   scenario_disconnect  the example scenario that takes the engine off the bus (ecu.connected):
//                the dashboard sees the speed stop and come back, at the same times at any speed
//   scenario_each_write  the same value written by two steps of a scenario, at one time and at two
//                times: the ECU's setter is called once for each step; 200 writes for one time to
//                one ECU: all taken, and applied in the order of the steps
//   ethernet     the example of examples/ethernet: two ECU executables on an Ethernet network; the
//                check shows their MAC addresses, and the example scenario passes with the same
//                result at any speed (frames, acknowledgements, ecu.connected)
//   scenario_failures  a wait that times out, a simulation that stops before the scenario is over, an
//                ECU that fails in the middle, a scenario with errors (nothing is started), the
//                command line
#include "launch_run_support.hpp"

void scenario(const Options& o)
{
    const fs::path docs = o.docs / "scenario.md";
    const fs::path dir = o.example_dir;
    const std::vector<std::string> args{"run", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_step.yaml"};
    const Captured text = launch(o, args, dir);
    expect_code(text, 0, "run simulation.yaml --scenario scenarios/throttle_step.yaml");
    expect_text(text.standard_output,
                block_after(docs, "<!-- vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml -->"),
                "run simulation.yaml --scenario scenarios/throttle_step.yaml");
    const std::string err = lf(text.standard_error);
    require(contains(err, "\n[vehicle_sil_launch] scenario: line 10: wait_until Engine.engine.speed ge 1700 (timeout 500 ms): ok at "
                          "330 ms\n") &&
                contains(err, "\n[vehicle_sil_launch] the scenario is over: the simulation was stopped\n"),
            "each step's line on stderr:\n" + err);

    // The JSON: the scenario with every step, and nothing else changes (no finding, passed).
    std::vector<std::string> json_args = args;
    json_args.push_back("--json");
    const Captured json = launch(o, json_args, dir);
    expect_code(json, 0, "run --scenario --json");
    const std::string out = lf(json.standard_output);
    const std::string scenario = scenario_json(out);
    require(contains(scenario, "  \"scenario\": {\n    \"file\": \"scenarios/throttle_step.yaml\",\n    \"name\": \"throttle step\",\n"
                               "    \"status\": \"passed\",\n    \"over_at_ms\": 705,\n    \"steps\": [\n") &&
                contains(scenario, "      {\n        \"index\": 2,\n        \"line\": 10,\n        \"kind\": \"wait_until\",\n"
                                   "        \"text\": \"wait_until Engine.engine.speed ge 1700 (timeout 500 ms)\",\n"
                                   "        \"status\": \"passed\",\n        \"start_ms\": 0,\n        \"at_ms\": 330,\n"
                                   "        \"end_ms\": 340,\n        \"code\": null,\n        \"message\": null\n      },\n"),
            "the scenario in the JSON:\n" + scenario);
    require(contains(out, "\"result\": \"passed\"") && contains(out, "\"simulation\": \"ended\"") &&
                contains(out, "\"sim_ms\": 705,") && contains(out, "\"time_step_ms\": 5,"),
            "the run ends with the scenario, at its last step:\n" + out);

    // The events: each step begins and is decided, in the order of the virtual time.
    std::vector<std::string> event_args = args;
    event_args.push_back("--events");
    const Captured events = launch(o, event_args, dir);
    expect_code(events, 0, "run --scenario --events");
    std::vector<std::string> steps;
    std::istringstream in(lf(events.standard_output));
    for (std::string line; std::getline(in, line);)
    {
        if (contains(line, "\"event\":\"scenario_step\""))
        {
            steps.push_back("line " + value_of(line, "line") + " " + value_of(line, "status") + " " + value_of(line, "at_ms"));
        }
    }
    std::string all;
    for (const auto& step : steps)
    {
        all += step + "\n";
    }
    expect_text(all,
                "line 8 running null\nline 8 passed 0\nline 10 running null\nline 10 passed 330\nline 12 running null\n"
                "line 12 passed 490\nline 15 running null\nline 15 passed 490\nline 17 running null\nline 17 passed 700\n"
                "line 18 running null\nline 18 passed 700\n",
                "the scenario_step events");
    require(contains(events.standard_output, "\"scenario\":{\"file\":\"scenarios/throttle_step.yaml\",\"name\":\"throttle step\","
                                             "\"status\":\"passed\",\"over_at_ms\":705,"),
            "the result event has the scenario:\n" + events.standard_output);

    // check --scenario: the scenario is checked with the configuration, nothing runs.
    const Captured checked = launch(o, {"check", "simulation.yaml", "--scenario", "scenarios/throttle_step.yaml"}, dir);
    expect_code(checked, 0, "check --scenario");
    require(contains(lf(checked.standard_output), "\nscenario scenarios/throttle_step.yaml (throttle step): 6 steps\n\npassed: 0 errors"),
            checked.standard_output);

    // The examples in scenario.md of a scenario with mistakes and of one that fails: the files are
    // the blocks there, and so is what the launcher writes.
    // They go to a copy of the example, not next to the example that works.
    const fs::path copy = example_copy(o);
    write_file(copy / "throttle_mistakes.yaml", block_after(docs, "<!-- throttle_mistakes.yaml -->"));
    const Captured mistakes = launch(o, {"check", "simulation.yaml", "--scenario", "throttle_mistakes.yaml"}, copy);
    expect_code(mistakes, 1, "check --scenario throttle_mistakes.yaml");
    expect_text(mistakes.standard_output,
                block_after(docs, "<!-- vehicle_sil_launch check simulation.yaml --scenario throttle_mistakes.yaml -->"),
                "check simulation.yaml --scenario throttle_mistakes.yaml");
    write_file(copy / "throttle_response.yaml", block_after(docs, "<!-- throttle_response.yaml -->"));
    const Captured failed = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "throttle_response.yaml"}, copy);
    expect_code(failed, 1, "run --scenario throttle_response.yaml");
    expect_text(failed.standard_output,
                block_after(docs, "<!-- vehicle_sil_launch run simulation.yaml --scenario throttle_response.yaml -->"),
                "run simulation.yaml --scenario throttle_response.yaml");
    const Captured failed_json = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "throttle_response.yaml", "--json"}, copy);
    expect_code(failed_json, 1, "run --scenario throttle_response.yaml --json");
    expect_text(scenario_json(lf(failed_json.standard_output)),
                block_after(docs, "<!-- vehicle_sil_launch run simulation.yaml --scenario throttle_response.yaml --json: the scenario -->"),
                "the scenario of run simulation.yaml --scenario throttle_response.yaml --json");
}

void scenario_same(const Options& o)
{
    // Every write is checked by what the ECU saw: the step in which its level changed.
    const std::string file = "vehicle_sil_scenario: 1\n"
                             "name: write times\n"
                             "steps:\n"
                             "  - set: { Body.level: 5, Body.gain: 3, Door.mode: auto }\n"
                             "  - expect: { Body.gain_at_0: 3, Body.level_changed_at_ms: 0, Door.mode_now: auto }\n"
                             "  - at_ms: 30\n"
                             "    set: { Body.level: 6 }\n"
                             "  - expect: { Body.level_changed_at_ms: 30 }\n"
                             "  - at_ms: 70\n"
                             "    set: { Door.level: 9 }\n"
                             "  - at_ms: 70\n"
                             "    expect: { Door.level_changed_at_ms: 70, Body.level_changed_at_ms: 30 }\n"
                             "  - wait_until: { Body.pulse: true }\n"
                             "    timeout_ms: 500\n"
                             "  - set: { Body.level: 7 }\n"
                             "  - expect: { Body.level_changed_at_ms: 220 }\n"
                             "  - ramp: { Door.level: { to: 0, over_ms: 30 } }\n"
                             "  - expect: { Door.level_changed_at_ms: 250, Door.level: 0, Body.ticks: 26 }\n";
    const auto run = [&](const fs::path& dir, std::vector<std::string> more, const std::string& what) {
        std::vector<std::string> args{"run", "sim.yaml", "--scenario", "s.yaml", "--json"};
        args.insert(args.end(), more.begin(), more.end());
        const Captured r = launch(o, args, dir);
        expect_code(r, 0, what);
        const std::string out = lf(r.standard_output);
        require(contains(out, "\"late_messages\": 0") && !contains(out, "control-link-late-values"), what + ": no late message:\n" + out);
        return scenario_json(out);
    };
    const fs::path dir = scenario_ecus(o, "", file);
    const std::string fastest = run(dir, {}, "as fast as possible");
    require(contains(fastest, "\"status\": \"passed\"") && contains(fastest, "\"over_at_ms\": 260,"),
            "the scenario passes, and is over at 260 ms:\n" + fastest);
    // The pulse is in the sample of 200 ms; the launcher knows at 210, the next step is at 220.
    require(contains(fastest, "\"start_ms\": 70,\n        \"at_ms\": 200,\n        \"end_ms\": 220,"), "the wait:\n" + fastest);
    for (int i = 0; i < 3; ++i)
    {
        expect_text(run(dir, {}, "as fast as possible again"), fastest, "the same run again");
    }
    expect_text(run(dir, {"--speed", "4"}, "at four times real time"), fastest, "at a speed");
    expect_text(run(dir, {"--speed", "0.5"}, "at half of real time"), fastest, "at half of real time");
    // An ECU whose every step takes 3 ms of real time, and a stop_at_ms beyond the scenario.
    const fs::path slow = scenario_ecus(o, "# run-option --stop-at-ms 2000\n", file, "slow-ms 3");
    expect_text(run(slow, {}, "with a slow ECU"), fastest, "with a slow ECU");
    expect_text(run(slow, {"--speed", "8"}, "with a slow ECU at a speed"), fastest, "with a slow ECU at a speed");

    // With the time commands: paused in the middle and stepped on, the result is the same. (Slow
    // enough that the pause comes long before the end: 260 ms take 2.6 s.)
    {
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--scenario", "s.yaml", "--speed", "0.1", "sim.yaml"});
        live.wait_for({"\"event\":\"scenario_step\"", "\"line\":6,", "\"status\":\"passed\""});
        live.write("{\"command\":\"pause\"}\n");
        live.wait_paused();
        live.write("{\"command\":\"step\",\"ms\":30}\n");
        live.wait_for({"\"command\":\"step\"", "\"accepted\":true"});
        std::this_thread::sleep_for(100ms);
        live.write("{\"command\":\"speed\",\"value\":\"max\"}\n{\"command\":\"resume\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "the paused run passes");
        const std::string& result = lines.back();
        require(contains(result, "\"scenario\":{\"file\":\"s.yaml\",\"name\":\"write times\",\"status\":\"passed\",\"over_at_ms\":260,") &&
                    contains(result, "\"start_ms\":70,\"at_ms\":200,\"end_ms\":220,"),
                "the same result with a pause and a step in between:\n" + result);
        log("CHECK a pause and a step in a scenario -> ok");
    }
}

void scenario_each_write(const Options& o)
{
    // "request" always reads false, and its setter counts each write: a write of the value the
    // variable has already is not dropped on the way.
    const std::string file = "vehicle_sil_scenario: 1\n"
                             "name: a write each time\n"
                             "steps:\n"
                             "  - set: { Body.request: true }\n"
                             "  - set: { Body.request: true }\n"
                             "  - expect: { Body.requests: 2, Body.request: false }\n"
                             "  - at_ms: 30\n"
                             "    set: { Body.request: true }\n"
                             "  - expect: { Body.requests: 3 }\n"
                             "  - at_ms: 50\n"
                             "    set: { Body.request: true }\n"
                             "  - expect: { Body.requests: 4, Body.request: false }\n"
                             "  - expect: { Body.requests: 4 }\n"
                             "    hold_for_ms: 30\n";
    const fs::path dir = scenario_ecus(o, "", file, "request");
    const Captured r = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, dir);
    expect_code(r, 0, "a scenario that writes the same value four times");
    const std::string scenario = scenario_json(lf(r.standard_output));
    require(contains(scenario, "\"status\": \"passed\"") && !contains(scenario, "\"status\": \"failed\""),
            "each set reaches the setter once:\n" + scenario);
    log("CHECK the same value written by four steps: four setter calls -> ok");

    // 200 writes to one ECU for one time: the launcher sends them without waiting for each and
    // then waits once. Each is taken (fewer than the agent's 256 commands in flight), each reaches
    // the setter, and they take effect in the order of the steps: the last level stays.
    std::string many = "vehicle_sil_scenario: 1\nname: many writes at once\nsteps:\n";
    for (int i = 1; i <= 200; ++i)
    {
        many += "  - set: { Body.request: true, Body.level: " + std::to_string(20 + i % 70) + " }\n";
    }
    many += "  - at_ms: 20\n    expect: { Body.requests: 200, Body.level: 80 }\n";
    const fs::path at_once = scenario_ecus(o, "", many, "request");
    const Captured all = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, at_once);
    expect_code(all, 0, "a scenario with 200 writes for one time");
    const std::string result = scenario_json(lf(all.standard_output));
    require(contains(result, "\"status\": \"passed\"") && !contains(result, "\"status\": \"failed\""),
            "200 writes for one time, all taken and applied in order:\n" + result);
    log("CHECK 200 writes for one time to one ECU: all taken, in the order of the steps -> ok");
}

void scenario_wave(const Options& o)
{
    // What the ECU saw of each wave: the step in which its level changed last, and the level.
    const std::string head = "vehicle_sil_scenario: 1\n"
                             "name: waves\n"
                             "steps:\n"
                             "  - set: { Body.level: 20, Door.mode: off }\n"
                             // 30 at 100 and 110, 20 at 120 and 130, ... and 20 (low) at the end, 200.
                             "  - at_ms: 100\n"
                             "    wave: { Body.level: { square: { low: 20, high: 30, period_ms: 40 }, for_ms: 100 },\n"
                             "            Door.mode: { points: [[0, on], [30, auto], [50, off]] } }\n"
                             "  - expect: { Body.level_changed_at_ms: 200, Body.level: 20, Door.mode_now: off }\n"
                             // Around the 20 it has: 25, 30, 25, 20, 15, and 20 again at the end, 260.
                             "  - wave: { Body.level: { triangle: { amplitude: 10, period_ms: 80 }, for_ms: 60 } }\n"
                             "  - expect: { Body.level_changed_at_ms: 260, Body.level: 20 }\n"
                             // Shorter than the ECU's step, begun at one of its steps: seen for that one step.
                             "  - after_ms: 10\n"
                             "    wave: { Body.level: { pulse: { value: 50, width_ms: 5 } } }\n";
    const std::string tail = "  - after_ms: 5\n"
                             "    expect: { Body.level_changed_at_ms: 280, Body.level: 20, Body.ticks: 29 }\n";
    const auto run = [&](const fs::path& dir, std::vector<std::string> more, const std::string& what) {
        std::vector<std::string> args{"run", "sim.yaml", "--scenario", "s.yaml", "--json"};
        args.insert(args.end(), more.begin(), more.end());
        const Captured r = launch(o, args, dir);
        expect_code(r, 0, what);
        const std::string out = lf(r.standard_output);
        require(contains(out, "\"late_messages\": 0") && !contains(out, "control-link-late-values"), what + ": no late message:\n" + out);
        return scenario_json(out);
    };
    const fs::path dir = scenario_ecus(o, "", head + "    undersampled: ok\n" + tail);
    const std::string fastest = run(dir, {}, "as fast as possible");
    require(contains(fastest, "\"status\": \"passed\"") && contains(fastest, "\"over_at_ms\": 290,"),
            "the scenario passes, and is over at 290 ms:\n" + fastest);
    // A wave in the result: its kind, its text, and the times of a step that lasts.
    require(contains(fastest, "        \"kind\": \"wave\",\n"
                              "        \"text\": \"wave Body.level triangle amplitude 10 period 80 ms for 60 ms\",\n"
                              "        \"status\": \"passed\",\n        \"start_ms\": 200,\n        \"at_ms\": 260,\n"
                              "        \"end_ms\": 260,"),
            "the triangle in the JSON:\n" + fastest);
    // The values depend on the virtual time alone: the same at a speed, and with a controller
    // that steps half and twice as often as the ECUs.
    expect_text(run(dir, {"--speed", "4"}, "at four times real time"), fastest, "at a speed");
    // (Before the next scenario_ecus(): each writes the same directory anew.)
    expect_code(launch(o, {"check", "--strict", "sim.yaml", "--scenario", "s.yaml"}, dir), 0, "check --strict with undersampled: ok");
    for (const char* step : {"5", "20"})
    {
        const fs::path other = scenario_ecus(o, std::string("time_step_ms: ") + step + "\n", head + "    undersampled: ok\n" + tail);
        const std::string result = run(other, {}, std::string("with a controller step of ") + step + " ms");
        require(contains(result, "\"status\": \"passed\""), std::string("with a controller step of ") + step + " ms:\n" + result);
    }

    // Without "undersampled: ok" the check warns, and says what the ECU sees; the run is the same.
    const fs::path warned = scenario_ecus(o, "", head + tail);
    const Captured checked = launch(o, {"check", "sim.yaml", "--scenario", "s.yaml"}, warned);
    expect_code(checked, 0, "check of a wave that is finer than the ECU's step");
    require(contains(lf(checked.standard_output),
                     "\nwarning: s.yaml:12: the pulse of Body.level is 5 ms wide, shorter than the step of "
                     "participant 'Body' (10 ms): this step begins at 270 ms, so the ECU has the pulse's value for one whole step of 10 "
                     "ms (the one at 270 ms), not for 5 ms. Make width_ms 10 or more; to run it as it is without this message, add "
                     "undersampled: ok to the step [scenario-wave-undersampled]\n"),
            "the warning of the check:\n" + lf(checked.standard_output));
    expect_code(launch(o, {"check", "--strict", "sim.yaml", "--scenario", "s.yaml"}, warned), 1, "check --strict of the same");
    const std::string same = run(warned, {}, "with the warning");
    require(contains(same, "\"status\": \"passed\"") && contains(same, "\"over_at_ms\": 290,"), "the run with the warning:\n" + same);

    // A pulse shorter than the ECU's step that begins between two steps: the ECU has it for one
    // step if a step is in it (107 ms to 112 ms: the one at 110 ms), and never gets it otherwise
    // (201 ms to 206 ms: the level stays as it changed last, at 120 ms).
    const fs::path between = scenario_ecus(o, "",
                                           "vehicle_sil_scenario: 1\nsteps:\n"
                                           "  - set: { Body.level: 20 }\n"
                                           "  - at_ms: 107\n    wave: { Body.level: { pulse: { value: 50, width_ms: 5 } } }\n    undersampled: ok\n"
                                           "  - at_ms: 115\n    expect: { Body.level: 50, Body.level_changed_at_ms: 110 }\n"
                                           "  - at_ms: 125\n    expect: { Body.level: 20, Body.level_changed_at_ms: 120 }\n"
                                           "  - at_ms: 201\n    wave: { Body.level: { pulse: { value: 60, width_ms: 5 } } }\n    undersampled: ok\n"
                                           "  - at_ms: 225\n    expect: { Body.level: 20, Body.level_changed_at_ms: 120 }\n");
    const std::string seen = run(between, {}, "pulses that begin between two steps of the ECU");
    require(contains(seen, "\"status\": \"passed\""), "pulses that begin between two steps of the ECU:\n" + seen);

    // A wave that leaves the variable's range fails at the write that does, and says the value's time.
    const fs::path outside = scenario_ecus(o, "",
                                           "vehicle_sil_scenario: 1\nsteps:\n"
                                           "  - set: { Body.level: 95 }\n"
                                           "  - wave: { Body.level: { sawtooth: { amplitude: 10, period_ms: 80 }, for_ms: 80 } }\n");
    const Captured failed = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, outside);
    expect_code(failed, 1, "a wave that leaves the range");
    const std::string why = scenario_json(lf(failed.standard_output));
    // 85 at 0, then 2.5 more per step: 100 at 60 ms, 102.5 (written as 103) at 70 ms.
    require(contains(why, "\"code\": \"scenario-write\"") && contains(why, "the write for 70 ms was not taken by participant 'Body'"),
            "the failure of the wave:\n" + why);

    // The example with waves (examples/ecu_main/scenarios/throttle_wave.yaml) passes, without a finding.
    const Captured example = launch(o, {"run", "--strict", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_wave.yaml"}, o.example_dir);
    expect_code(example, 0, "run --strict simulation.yaml --scenario scenarios/throttle_wave.yaml");
    require(contains(lf(example.standard_output), "\nscenario scenarios/throttle_wave.yaml (throttle wave): passed, 7 of 7 steps passed\n") &&
                contains(lf(example.standard_output), "\npassed: 0 errors, 0 warnings, 0 notes"),
            "the example with waves:\n" + lf(example.standard_output));

    // The example with points from a CSV file (examples/ecu_main/scenarios/throttle_trace.yaml and .csv) passes, without a finding.
    const Captured trace = launch(o, {"run", "--strict", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_trace.yaml"}, o.example_dir);
    expect_code(trace, 0, "run --strict simulation.yaml --scenario scenarios/throttle_trace.yaml");
    require(contains(lf(trace.standard_output), "\nscenario scenarios/throttle_trace.yaml (throttle trace): passed, 4 of 4 steps passed\n") &&
                contains(lf(trace.standard_output), "\npassed: 0 errors, 0 warnings, 0 notes"),
            "the example with points from a file:\n" + lf(trace.standard_output));
}

void scenario_dither(const Options& o)
{
    // A square of 5 on Body.level, high for the first 20 ms of every 40 ms. What the ECU saw: the
    // level, and the step in which it changed last.
    const std::string text = "vehicle_sil_scenario: 1\n"
                             "name: dither\n"
                             "inputs:\n"
                             "  Body.level: { dither: { square: { amplitude: 5, period_ms: 40 } } }\n"
                             "steps:\n"
                             // 25 at 0 and 10, 15 at 20 and 30, ...: low again from 100.
                             "  - set: { Body.level: 20 }\n"
                             "  - at_ms: 100\n"
                             "    expect: { Body.level: 15, Body.level_changed_at_ms: 100 }\n"
                             // A ramp between two steps of the ECU; its end value (60, low: 55) is for
                             // 193 ms, and the step at 200 ms gets it and then the dither's 65.
                             "  - at_ms: 107\n"
                             "    ramp: { Body.level: { to: 60, over_ms: 86 } }\n"
                             "  - at_ms: 215\n"
                             "    expect: { Body.level: 65, Body.level_changed_at_ms: 200 }\n"
                             // Taken off at 240 ms (the level was 55 from 220 ms): one write of 60, and no more.
                             "  - at_ms: 240\n"
                             "    dither: { Body.level: none }\n"
                             "  - at_ms: 300\n"
                             "    expect: { Body.level: 60, Body.level_changed_at_ms: 240 }\n";
    const auto run = [&](const fs::path& dir, std::vector<std::string> more, const std::string& what) {
        std::vector<std::string> args{"run", "sim.yaml", "--scenario", "s.yaml", "--json"};
        args.insert(args.end(), more.begin(), more.end());
        const Captured r = launch(o, args, dir);
        expect_code(r, 0, what);
        const std::string out = lf(r.standard_output);
        require(contains(out, "\"late_messages\": 0") && !contains(out, "control-link-late-values"), what + ": no late message:\n" + out);
        return scenario_json(out);
    };
    const fs::path dir = scenario_ecus(o, "", text);
    const std::string fastest = run(dir, {}, "as fast as possible");
    require(contains(fastest, "\"status\": \"passed\"") && contains(fastest, "\"over_at_ms\": 310,"),
            "the scenario passes, and is over at 310 ms:\n" + fastest);
    require(contains(fastest, "        \"kind\": \"dither\",\n        \"text\": \"dither Body.level none\",\n        \"status\": \"passed\",\n"
                              "        \"start_ms\": 240,\n        \"at_ms\": 240,\n        \"end_ms\": 240,\n"),
            "the dither step in the result:\n" + fastest);
    // The same with a coarser step of the controller, and at a speed.
    const std::string coarse = run(scenario_ecus(o, "time_step_ms: 30\n", text), {}, "with a controller step of 30 ms");
    require(contains(coarse, "\"status\": \"passed\""), "with a controller step of 30 ms:\n" + coarse);
    const std::string paced = run(dir, {"--speed", "4"}, "at four times the speed of a clock");
    require(contains(paced, "\"status\": \"passed\""), "at a speed:\n" + paced);

    // A dither that takes the value out of the variable's range: the ECU's agent refuses the
    // write when it gets it. It is no step's write, so the step that fails is the first that is
    // not decided, and the message says which dither wrote what.
    const fs::path outside = scenario_ecus(o, "",
                                           "vehicle_sil_scenario: 1\n"
                                           "inputs:\n"
                                           "  Body.level: { dither: { square: { amplitude: 5, period_ms: 40, phase_deg: 180 } } }\n"
                                           "steps:\n"
                                           "  - set: { Body.level: 97 }\n"
                                           "  - at_ms: 100\n"
                                           "    expect: { Body.level: 92 }\n");
    const Captured failed = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, outside);
    expect_code(failed, 1, "a dither that leaves the range");
    const std::string why = scenario_json(lf(failed.standard_output));
    require(contains(why, "\"code\": \"scenario-write\"") &&
                contains(why, "the dither of Body.level (line 3) wrote 102 at 20 ms, above its range 0 to 100 (level: 102 is out of range): lower "
                              "its amplitude, or keep the value further from the end of the range") &&
                contains(why, "        \"line\": 5,\n        \"kind\": \"set\",\n        \"text\": \"set Body.level = 97\",\n        \"status\": \"passed\","),
            "the failure of the dither:\n" + why);

    // The example with a dither (examples/ecu_main/scenarios/throttle_dither.yaml) passes, without
    // a finding; its timeline has the dither as a line of no step, and the base of every point.
    const fs::path timeline = dir / "throttle_dither.timeline.json";
    const Captured example = launch(o, {"run", "--strict", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_dither.yaml", "--timeline",
                                        timeline.string()},
                                    o.example_dir);
    expect_code(example, 0, "run --strict simulation.yaml --scenario scenarios/throttle_dither.yaml");
    require(contains(lf(example.standard_output), "\nscenario scenarios/throttle_dither.yaml (throttle dither): passed, 8 of 8 steps passed\n") &&
                contains(lf(example.standard_output), "\npassed: 0 errors, 0 warnings, 0 notes"),
            "the example with a dither:\n" + lf(example.standard_output));
    const std::string file = lf(read_text(timeline));
    require(contains(file, "      \"variable\": \"Engine.engine.throttle\",\n      \"kind\": \"dither\",\n      \"line\": 10,\n"
                           "      \"step\": null,\n      \"from_ms\": 0,\n      \"to_ms\": null,\n      \"base\": null,\n"
                           "      \"base_source\": null,\n      \"shape\": {\n        \"type\": \"triangle\",\n        \"amplitude\": 2,\n"
                           "        \"period_ms\": 80\n      },\n") &&
                contains(file, "      \"columns\": [\"at_ms\", \"value\", \"base\"],\n      \"samples\": null,\n      \"first_ms\": null,\n"
                               "      \"last_ms\": null,\n      \"not_applied_from\": null,\n      \"points\": [\n        [0, 50, 50],\n"
                               "        [10, 51, 50],\n        [20, 52, 50],\n") &&
                contains(file, "      \"shape\": null,\n      \"text\": \"Engine.engine.throttle none\"\n"),
            "the timeline of the example with a dither:\n" + file);

    // The check lists what the text puts on the inputs (the block of docs/scenario.md), and so do
    // the JSON of the check and of the run; a scenario without a dither has no such list.
    const Captured checked = launch(o, {"check", "simulation.yaml", "--scenario", "scenarios/throttle_dither.yaml"}, o.example_dir);
    expect_code(checked, 0, "check simulation.yaml --scenario scenarios/throttle_dither.yaml");
    const std::string shown = lf(checked.standard_output);
    const std::string head = "\nscenario scenarios/throttle_dither.yaml (throttle dither): 8 steps\n\n";
    const auto listAt = shown.find(head);
    require(listAt != std::string::npos, "the check of the example with a dither:\n" + shown);
    const auto listEnd = shown.find("\n\n", listAt + head.size());
    expect_text(shown.substr(listAt + head.size(), listEnd + 1 - listAt - head.size()),
                block_after(o.docs / "scenario.md",
                            "<!-- vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_dither.yaml: the lines -->"),
                "the lines of check --scenario scenarios/throttle_dither.yaml");
    const std::string shape = "\"shape\": {\n          \"type\": \"triangle\",\n          \"amplitude\": 2,\n          \"period_ms\": 80\n"
                              "        },\n";
    // (What a line of the timeline file has too is written as there: the text with the variable,
    // the shape, the step; what the text does not say is null, and time_source says why.)
    const std::string lists = "    \"inputs\": [\n      {\n        \"variable\": \"Engine.engine.throttle\",\n        \"line\": 10,\n"
                              "        \"dither\": {\n          \"shape\": {\n            \"type\": \"triangle\",\n            \"amplitude\": 2,\n"
                              "            \"period_ms\": 80\n          },\n"
                              "          \"text\": \"Engine.engine.throttle triangle amplitude 2 period 80 ms\"\n        }\n      }\n    ],\n"
                              "    \"lines\": [\n      {\n        \"variable\": \"Engine.engine.throttle\",\n        \"kind\": \"dither\",\n"
                              "        \"line\": 10,\n        \"step\": null,\n        \"from_ms\": null,\n        \"to_ms\": null,\n"
                              "        \"time_source\": \"first_write\",\n        \"after_line\": null,\n        " + shape +
                              "        \"text\": \"Engine.engine.throttle triangle amplitude 2 period 80 ms\"\n      },\n";
    const Captured checkedJson = launch(o, {"check", "--json", "simulation.yaml", "--scenario", "scenarios/throttle_dither.yaml"}, o.example_dir);
    expect_code(checkedJson, 0, "check --json --scenario scenarios/throttle_dither.yaml");
    require(contains(lf(checkedJson.standard_output), "    \"steps\": 8,\n" + lists) &&
                contains(lf(checkedJson.standard_output),
                         "        \"kind\": \"ramp\",\n        \"line\": 21,\n        \"step\": 4,\n        \"from_ms\": null,\n"
                         "        \"to_ms\": null,\n        \"time_source\": \"after_wait\",\n        \"after_line\": 16,\n        \"shape\": {\n"
                         "          \"type\": \"ramp\",\n          \"to\": 80,\n          \"over_ms\": 300\n        },\n"
                         "        \"text\": \"Engine.engine.throttle to 80 over 300 ms\"\n") &&
                contains(lf(checkedJson.standard_output),
                         "        \"kind\": \"set\",\n        \"line\": 14,\n        \"step\": 1,\n        \"from_ms\": 0,\n        \"to_ms\": 0,\n"
                         "        \"time_source\": \"text\",\n        \"after_line\": null,\n        \"shape\": null,\n"
                         "        \"text\": \"Engine.engine.throttle = 50\"\n"),
            "the inputs and the lines in check --json:\n" + lf(checkedJson.standard_output));
    const Captured ranJson = launch(o, {"run", "--json", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_dither.yaml"}, o.example_dir);
    expect_code(ranJson, 0, "run --json --scenario scenarios/throttle_dither.yaml");
    require(contains(scenario_json(lf(ranJson.standard_output)), "    ],\n" + lists),
            "the inputs and the lines in the scenario of run --json:\n" + scenario_json(lf(ranJson.standard_output)));
    // The run's output for people begins with the check's part, and has the same list there.
    const Captured ran = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_dither.yaml"}, o.example_dir);
    expect_code(ran, 0, "run --scenario scenarios/throttle_dither.yaml");
    require(contains(lf(ran.standard_output),
                     head + block_after(o.docs / "scenario.md",
                                        "<!-- vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_dither.yaml: the lines -->") +
                         "\n"),
            "the lines in the output of run --scenario scenarios/throttle_dither.yaml:\n" + lf(ran.standard_output));
    // A scenario without a dither has none of it: empty lists in the check, in the run's result
    // and in its events, and no list in the output for people (whose text the test "scenario"
    // compares with docs/scenario.md).
    const std::vector<std::vector<std::string>> without = {
        {"check", "--json", "simulation.yaml", "--scenario", "scenarios/throttle_step.yaml"},
        {"run", "--json", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_step.yaml"},
        {"run", "--events", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_step.yaml"},
        {"run", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/throttle_step.yaml"},
    };
    for (const auto& args : without)
    {
        const Captured plain = launch(o, args, o.example_dir);
        expect_code(plain, 0, args[0] + " " + args[1] + " of a scenario without a dither");
        const std::string out = lf(plain.standard_output);
        // (Keys that only the lists have: other parts of the result have "inputs" and "timing" of their own.)
        require(!contains(out, "\"after_line\"") && !contains(out, "\"dither\"") && !contains(out, "first_write") && !contains(out, "\ninput "),
                "a scenario without a dither has no inputs and no lines (" + args[0] + " " + args[1] + "):\n" + out);
        const bool json = args[1] == "--json";
        const bool events = args[1] == "--events";
        require(!json || contains(out, "\n    \"inputs\": [],\n    \"lines\": []"),
                "the lists are there, empty (" + args[0] + " " + args[1] + "):\n" + out);
        require(!events || contains(out, "\"inputs\":[],\"lines\":[]"),
                "the lists are in the events, empty:\n" + out);
    }
    // A scenario that cannot be read has no lists to give: both are null.
    const fs::path copy = example_copy(o);
    write_file(copy / "unreadable.yaml", "vehicle_sil_scenario: 1\nsteps: [\n");
    const Captured unreadable = launch(o, {"check", "--json", "simulation.yaml", "--scenario", "unreadable.yaml"}, copy);
    expect_code(unreadable, 1, "check --json --scenario unreadable.yaml");
    require(contains(lf(unreadable.standard_output), "\n    \"inputs\": null,\n    \"lines\": null"),
            "a scenario that cannot be read has null for its inputs and lines:\n" + lf(unreadable.standard_output));
}

/// examples/ecu_main/simulation.live.yaml: the example's two ECUs at the speed of a clock, with no
/// stop_at_ms. It passes the check, runs past the 1000 ms at which simulation.yaml ends, never
/// ahead of the clock, and a stop command ends it as a stopped run (0), both ECUs with 0.
void live(const Options& o)
{
    const fs::path dir = o.example_dir;
    // The same simulation as simulation.yaml but for the end and the speed (comments aside).
    const auto settings = [&](const std::string& name) {
        std::istringstream in(lf(read_text(dir / name)));
        std::string all;
        for (std::string line; std::getline(in, line);)
        {
            all += line.rfind('#', 0) == 0 ? "" : line + "\n";
        }
        return all;
    };
    const std::string base = settings("simulation.yaml");
    require(!contains(base, "stop_at_ms:"), "workbench examples keep the stop time in the run");
    require(!contains(settings("simulation.live.yaml"), "speed:"), "live example keeps the speed in the run");
    require(lf(read_text(dir / "simulation.live.dashboard.yaml")) == lf(read_text(dir / "simulation.dashboard.yaml")),
            "simulation.live.dashboard.yaml in the build tree is the copy of simulation.dashboard.yaml");

    const Captured checked = launch(o, {"check", "simulation.live.yaml"}, dir);
    expect_code(checked, 0, "check simulation.live.yaml");
    require(contains(lf(checked.standard_output), "\npassed: 0 errors, 0 warnings, 0 notes\n"),
            "the check of simulation.live.yaml has no finding:\n" + checked.standard_output);

    const auto began = std::chrono::steady_clock::now();
    Live run(o, dir, {"run", "--events", "--stdin-commands", "--speed", "1", "simulation.live.yaml"});
    run.wait_for({"\"phase\":\"running\""});
    // Past the end of simulation.yaml. (Events of the time come at most every 100 ms.)
    std::int64_t reached = 0;
    for (std::size_t n = 1; reached <= 1000; ++n)
    {
        run.wait_for_count({"\"event\":\"time\""}, n);
        reached = std::stoll(value_of(events_with(run.lines(), {"\"event\":\"time\""})[n - 1], "sim_ms"));
    }
    // Speed 1 is a bound on one side: the virtual time is never ahead of the clock.
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    require(reached <= waited, "at speed 1, " + std::to_string(reached) + " ms of virtual time took only " +
                                   std::to_string(waited) + " ms");
    run.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = run.finish();
    require(status.has_value() && status->code == 0 && !status->signal,
            "the launcher ends with 0 when stopped, not " + code_text(status));
    const std::string& result = lines.back();
    require(contains(result, "\"result\":\"stopped\",\"end\":\"stopped\",\"ok\":true,") && contains(result, "\"interrupted_by\":\"stop command\""),
            "the result of the stopped run:\n" + result);
    for (const std::string name : {"Engine", "Dashboard"})
    {
        require(contains(result, "{\"name\":\"" + name + "\",\"required\":true,\"enabled\":true,\"manual\":false,\"for_network\":null,\"ended\":\"exited\","
                                                          "\"exit_code\":0,"),
                name + " ended by itself with 0:\n" + result);
    }
}

void scenario_disconnect(const Options& o)
{
    // Engine off its bus at 300 ms: its last speed is the one of 200 ms, so the dashboard says it is
    // lost after 500 ms (the sample of 510 ms). Back at 520 ms: the speed of 600 ms arrives.
    const auto run = [&](std::vector<std::string> more, const std::string& what) {
        std::vector<std::string> args{"run", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/engine_disconnect.yaml", "--json"};
        args.insert(args.end(), more.begin(), more.end());
        const Captured r = launch(o, args, o.example_dir);
        expect_code(r, 0, what);
        return scenario_json(lf(r.standard_output));
    };
    const std::string fastest = run({}, "as fast as possible");
    require(contains(fastest, "\"status\": \"passed\"") && contains(fastest, "\"over_at_ms\": 610,"),
            "the scenario passes, and is over at 610 ms:\n" + fastest);
    for (const auto& [line, at] : std::vector<std::pair<int, int>>{{9, 300}, {11, 505}, {13, 520}, {14, 605}})
    {
        const std::string expected = "\"at_ms\": " + std::to_string(at) + ",";
        const auto step = fastest.find("\"line\": " + std::to_string(line) + ",");
        const auto decided = step == std::string::npos ? step : fastest.find("\"at_ms\": ", step);
        require(decided != std::string::npos && fastest.compare(decided, expected.size(), expected) == 0,
                "line " + std::to_string(line) + " is decided at " + std::to_string(at) + " ms:\n" + fastest);
    }
    expect_text(run({"--speed", "4"}, "at four times real time"), fastest, "at a speed");
}

void ethernet(const Options& o)
{
    // examples/ethernet is next to examples/ecu_main in the build tree.
    const fs::path dir = o.example_dir / ".." / "ethernet";
    const Captured checked = launch(o, {"check", "simulation.yaml"}, dir);
    expect_code(checked, 0, "check of the Ethernet example");
    expect_text(lf(checked.standard_output),
                "simulation.yaml: 2 participants, 1 network\n"
                "\n"
                "participant  step   networks\n"
                "Sensor       10 ms  ETH (ethernet 02:00:00:00:00:01)\n"
                "Display      5 ms   ETH (ethernet 02:00:00:00:00:02)\n"
                "\n"
                "network  kind      participants\n"
                "ETH      ethernet  Sensor, Display\n"
                "\n"
                "passed: 0 errors, 0 warnings, 0 notes\n",
                "check of the Ethernet example");
    const auto run = [&](std::vector<std::string> more, const std::string& what) {
        std::vector<std::string> args{"run", "simulation.yaml", "--stop-at-ms", "1000", "--scenario", "scenarios/exchange.yaml", "--json"};
        args.insert(args.end(), more.begin(), more.end());
        const Captured r = launch(o, args, dir);
        expect_code(r, 0, what);
        return scenario_json(lf(r.standard_output));
    };
    const std::string fastest = run({}, "as fast as possible");
    require(contains(fastest, "\"status\": \"passed\""), "the scenario passes:\n" + fastest);
    expect_text(run({"--speed", "4"}, "at four times real time"), fastest, "at a speed");
}

/// An expect beside a ramp in a real run: true all through the ramp, the time and the value of the
/// sample that breaks it, and an expect that is not checked where its variable is absent while the
/// ramp runs.
void scenario_expect_beside(const Options& o)
{
    // Body.level goes 20, 21, ... from 100 ms, one more each 10 ms, and is 30 at the end, 200 ms.
    const auto steps = [](const std::string& expect) {
        return "vehicle_sil_scenario: 1\nsteps:\n"
               "  - set: { Body.level: 20 }\n"
               "  - at_ms: 100\n"
               "    ramp: { Body.level: { to: 30, over_ms: 100 } }\n"
               "    expect: { Body.level: " + expect + " }\n"
               "  - expect: { Body.level: 30, Body.level_changed_at_ms: 200 }\n";
    };
    const fs::path dir = scenario_ecus(o, "", steps("{ ge: 20, le: 30 }"));
    const Captured passed = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, dir);
    expect_code(passed, 0, "a ramp with an expect beside it");
    const std::string held = scenario_json(lf(passed.standard_output));
    require(contains(held, "        \"kind\": \"ramp\",\n"
                           "        \"text\": \"ramp Body.level to 30 over 100 ms, expecting Body.level ge 20, le 30\",\n"
                           "        \"status\": \"passed\",\n        \"start_ms\": 100,\n        \"at_ms\": 200,\n"
                           "        \"end_ms\": 200,") &&
                !contains(held, "expect_skipped"),
            "the ramp with its expect in the JSON:\n" + held);

    scenario_ecus(o, "", steps("{ le: 25 }"));
    const Captured broken = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, dir);
    expect_code(broken, 1, "a ramp whose expect breaks");
    const std::string why = scenario_json(lf(broken.standard_output));
    require(contains(why, "\"code\": \"scenario-failed\",\n        \"message\": \"not true during the ramp (from 100 ms to 200 ms): "
                          "at 160 ms Body.level le 25, but it was 26 (its sample of 160 ms)\""),
            "the failure of the expect beside the ramp:\n" + why);

    // Where the variable of the expect is absent, the ramp runs and its expect is not checked.
    write_file(dir / "sim.yaml", "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                                 "  - name: Body\n    command: [" + ecu(o) + "]\n"
                                 "    variables:\n      body.heater: { absent: true }\n"
                                 "  - name: Door\n    command: [" + ecu(o) + "]\n");
    write_file(dir / "s.yaml", steps("{ le: 30 }, Body.body.heater: true"));
    const std::string reason = "Body.body.heater is absent: participant 'Body' does not have it in this configuration (absent: true in "
                               "its variables)";
    const Captured checked = launch(o, {"check", "sim.yaml", "--scenario", "s.yaml"}, dir);
    expect_code(checked, 0, "check of an expect beside a ramp with an absent variable");
    require(contains(lf(checked.standard_output), "\nscenario s.yaml: 3 steps (1 expect not checked)\n") &&
                contains(lf(checked.standard_output), "\nnote: s.yaml:4: the ramp runs; its expect is not checked (ramp Body.level to "
                                                      "30 over 100 ms, expect not checked: Body.body.heater; Body.level le 30): " +
                                                          reason + " [scenario-step-skipped]\n"),
            "the note of the check:\n" + lf(checked.standard_output));
    const Captured text = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml"}, dir);
    expect_code(text, 0, "run of an expect beside a ramp with an absent variable");
    require(contains(lf(text.standard_output), "\nscenario s.yaml: passed, 3 of 3 steps passed, 1 expect not checked\n") &&
                contains(lf(text.standard_output), "\nline 4 expect not checked: " + reason + "\n"),
            "the summary and the line under the table:\n" + lf(text.standard_output));
    const Captured json = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, dir);
    expect_code(json, 0, "run --json of an expect beside a ramp with an absent variable");
    require(contains(scenario_json(lf(json.standard_output)), "        \"code\": null,\n        \"message\": null,\n"
                                                              "        \"expect_skipped\": \"" + reason + "\"\n") &&
                contains(scenario_json(lf(json.standard_output)), "\n    \"unchecked_expects\": 1,\n    \"steps\": [\n"),
            "expect_skipped and unchecked_expects in the JSON:\n" + scenario_json(lf(json.standard_output)));
    // vehicle_sil_launch test does not show such a run as a plain pass.
    fs::create_directories(dir / "scn");
    fs::copy_file(dir / "s.yaml", dir / "scn" / "ramp.yaml", fs::copy_options::overwrite_existing);
    const Captured table = launch(o, {"test", "sim.yaml", "--scenarios", "scn"}, dir);
    expect_code(table, 0, "test with an expect that is not checked");
    require(contains(lf(table.standard_output), "scn/ramp.yaml  passed  1 expect was not checked\n"),
            "the table of the test:\n" + lf(table.standard_output));
    const Captured tested = launch(o, {"test", "sim.yaml", "--scenarios", "scn", "--json"}, dir);
    expect_code(tested, 0, "test --json with an expect that is not checked");
    require(contains(lf(tested.standard_output), "\"reason\": \"1 expect was not checked\"") &&
                contains(lf(tested.standard_output), "\"unchecked_expects\": 1,"),
            "the JSON of the test:\n" + lf(tested.standard_output));
}

void scenario_failures(const Options& o)
{
    const std::string head = "vehicle_sil_scenario: 1\nsteps:\n";
    {
        // A wait that times out: the run fails, and says the line, the time and the value.
        const fs::path dir = scenario_ecus(o, "",
                                           head + "  - set: { Body.level: 5 }\n"
                                                  "  - wait_until: { Body.ticks: { ge: 10 }, Body.level: 6 }\n"
                                                  "    timeout_ms: 100\n"
                                                  "  - set: { Body.level: 7 }\n");
        const Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml"}, dir);
        expect_code(run, 1, "a wait that times out");
        const std::string out = lf(run.standard_output);
        require(contains(out, "run: the simulation ran until the scenario was over (110 ms) and ended\n") &&
                    contains(out, "scenario s.yaml: FAILED, 1 of 3 steps passed\n") &&
                    contains(out, "error: s.yaml:4: wait_until Body.ticks ge 10; Body.level eq 6 (timeout 100 ms): timed out at 100 ms "
                                  "(100 ms after 0 ms): Body.level eq 6, but it was 5 (its sample of 100 ms) [scenario-timeout]\n") &&
                    contains(out, "\nfailed: 1 error, 0 warnings, 0 notes\n"),
                "the failure in the text:\n" + out);
        // The failed step's line while the run lasts: the result, the time and why.
        const std::string err = lf(run.standard_error);
        require(contains(err, "\n[vehicle_sil_launch] scenario: line 4: wait_until Body.ticks ge 10; Body.level eq 6 (timeout 100 ms): "
                              "FAILED at 100 ms (timed out at 100 ms (100 ms after 0 ms): Body.level eq 6, but it was 5 (its sample of "
                              "100 ms))\n"),
                "the failed step's line on stderr:\n" + err);
        const Captured json = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json"}, dir);
        expect_code(json, 1, "a wait that times out, --json");
        const std::string j = lf(json.standard_output);
        require(contains(j, "\"code\": \"scenario-timeout\",\n      \"file\": \"s.yaml\",\n      \"line\": 4,") &&
                    contains(j, "\"status\": \"failed\",\n    \"over_at_ms\": 110,") && contains(j, "\"status\": \"not_run\","),
                "the failure in the JSON:\n" + j);
        // The same at a speed.
        const Captured paced = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--json", "--speed", "2"}, dir);
        expect_code(paced, 1, "a wait that times out, at a speed");
        expect_text(scenario_json(lf(paced.standard_output)), scenario_json(j), "the failed scenario at a speed");
    }
    {
        // stop_at_ms ends the run and leaves the pending check undecided.
        const fs::path dir = scenario_ecus(o, "# run-option --stop-at-ms 100\n",
                                           head + "  - at_ms: 50\n    set: { Body.level: 5 }\n"
                                                  "  - at_ms: 90\n    expect: { Body.level: 5 }\n"
                                                  "  - wait_until: { Body.level: 6 }\n    timeout_ms: 1000\n");
        const Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml"}, dir);
        expect_code(run, 5, "stop_at_ms before the end of the scenario");
        const std::string out = lf(run.standard_output);
        require(contains(out, "run: the simulation ran to stop_at_ms (100 ms) and ended\n") &&
                    contains(out, "scenario s.yaml: incomplete, 2 of 3 steps passed\n") &&
                    contains(out, "the run reached the requested stop_at_ms [scenario-incomplete]"),
                "out of time:\n" + out);
    }
    {
        // An ECU fails in the middle: its failure ends the run as always, and the scenario is incomplete.
        const fs::path dir = scenario_ecus(o, "",
                                           head + "  - set: { Body.level: 5 }\n"
                                                  "  - wait_until: { Door.level: 6 }\n    timeout_ms: 5000\n",
                                           "throw-at-ms 100");
        const Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml"}, dir);
        expect_code(run, 1, "an ECU fails during the scenario");
        const std::string out = lf(run.standard_output);
        require(contains(out, "run: the simulation was aborted: participant 'Body' failed\n") &&
                    contains(out, "scenario s.yaml: incomplete, 1 of 2 steps passed\n") && contains(out, "participant 'Body': it failed while the simulation was running") &&
                    contains(out, "show what it wrote [participant-failed]\n") &&
                    contains(out, "error: s.yaml:4: the scenario did not get to its end: the simulation ended at ") &&
                    contains(out, " when 1 of 2 steps had passed and the step at line 4 (wait_until Door.level eq 6 (timeout 5000 "
                                  "ms)) was not decided yet; the other findings and the lines above say why the simulation ended "
                                  "[scenario-incomplete]\n"),
                "an incomplete scenario:\n" + out);
    }
    {
        // The ECU that a ramp writes to dies (exit code 7) at some real time. As fast as possible,
        // the scenario's next write to it fails before the launcher's look at the processes; at a
        // slow speed, the look comes first. Either way: the participant's failure is the result,
        // with its exit code, and the scenario did not finish.
        const fs::path dir = scenario_ecus(o, "",
                                           head + "  - set: { Body.level: 0 }\n"
                                                  "  - ramp: { Body.level: { to: 100, over_ms: 3000000 } }\n",
                                           "exit-after-wall-ms 300 7");
        for (const std::string speed : {"max", "0.05", "max"})
        {
            const Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--speed", speed}, dir);
            expect_code(run, 1, "the written ECU dies, speed " + speed);
            const std::string out = lf(run.standard_output);
            require(contains(out, "run: the simulation was aborted: participant 'Body' failed\n") &&
                        contains(out, "scenario s.yaml: incomplete, 1 of 2 steps passed\n") &&
                        contains(out, "participant 'Body': it failed while the simulation was running, and the launcher aborted the "
                                      "simulation: its process exited with code 7") &&
                        contains(out, "error: s.yaml:4: the scenario did not get to its end: ") && !contains(out, "scenario-write") &&
                        !contains(out, "was not taken by") && contains(out, "\nfailed: 2 errors, "),
                    "the written ECU dies, speed " + speed + ":\n" + out + run.standard_error);
        }
        log("CHECK a participant's failure comes before the scenario's -> ok");
    }
    {
        // A stop command in the middle: stopped in order; the scenario is incomplete, which is the
        // result (5), and the finding that says where it got to is a note, not an error.
        const fs::path dir = scenario_ecus(o, "",
                                           head + "  - set: { Body.level: 5 }\n"
                                                  "  - wait_until: { Door.level: 6 }\n    timeout_ms: 600000\n");
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--scenario", "s.yaml", "--speed", "1", "sim.yaml"});
        live.wait_for({"\"event\":\"scenario_step\"", "\"line\":4,", "\"status\":\"running\""});
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 5, "a stop command during a scenario: 5, not " + code_text(status));
        const std::string& result = lines.back();
        require(contains(result, "\"result\":\"incomplete\",\"end\":\"stopped\",\"ok\":false,\"message\":\"The run was stopped by a stop request "
                                 "before the scenario reached its end, so the scenario has no verdict\",\"strict\":false,\"errors\":0,") &&
                    contains(result, "\"simulation\":\"interrupted\"") &&
                    contains(result, "{\"severity\":\"note\",\"code\":\"scenario-incomplete\"") &&
                    contains(result, "; the run was stopped by a stop request before the scenario's end\"") &&
                    contains(result, "\"name\":null,\"status\":\"incomplete\",\"over_at_ms\":null,") &&
                    contains(result, "\"status\":\"unfinished\""),
                "the stopped scenario:\n" + result);
        log("CHECK a stop command during a scenario -> ok");
    }
    {
        // An ECU hangs in a step while the scenario writes to it: --timeout-ms ends the run, and
        // the scenario's thread with it.
        const fs::path dir = scenario_ecus(o, "# run-option --participant-stop-timeout-ms 500\n",
                                           head + "  - set: { Body.level: 0 }\n"
                                                  "  - ramp: { Body.level: { to: 100, over_ms: 2000 } }\n",
                                           "hang-at-ms 50");
        const Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--timeout-ms", "1500"}, dir);
        expect_code(run, 1, "--timeout-ms during a scenario");
        const std::string out = lf(run.standard_output);
        require(contains(out, "run: the simulation was aborted: it did not end within --timeout-ms (1500 ms)\n") &&
                    contains(out, "scenario s.yaml: incomplete, 1 of 2 steps passed\n") &&
                    contains(out, "error: s.yaml:4: the scenario did not get to its end: "),
                "--timeout-ms during a scenario:\n" + out);
    }
    {
        // A scenario with errors: the check fails, and nothing is started. check says the same.
        const fs::path dir = scenario_ecus(o, "",
                                           head + "  - set: { Body.levl: 5 }\n"
                                                  "  - at_ms: 10\n    set: { Body.gain: 1, Door.ticks: 2 }\n");
        const std::string errors = "error: s.yaml:3: 'Body.levl': participant 'Body' has no variable 'levl': did you mean 'Body.level'? "
                                   "(vehicle_sil_launch check --json lists the variables) [scenario-variable]\n"
                                   "error: s.yaml:5: Body.gain is a parameter: it can be written only before the first step, that is "
                                   "with set in a step at 0 (at_ms: 0, or the first steps without a time), not at 10 ms [scenario-not-writable]\n"
                                   "error: s.yaml:5: Door.ticks is an output of its ECU: it can be read (expect, wait_until), not "
                                   "written; write one of the ECU's inputs instead [scenario-not-writable]\n";
        const Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml"}, dir);
        expect_code(run, 1, "run with a scenario that has errors");
        require(contains(lf(run.standard_output), errors) && contains(lf(run.standard_output), "nothing was started: the check found errors") &&
                    !contains(run.standard_error, "starting Body"),
                "nothing is started:\n" + run.standard_output + run.standard_error);
        const Captured check = launch(o, {"check", "sim.yaml", "--scenario", "s.yaml"}, dir);
        expect_code(check, 1, "check with a scenario that has errors");
        require(contains(lf(check.standard_output), errors) && contains(lf(check.standard_output), "\nfailed: 3 errors, 0 warnings, 0 notes\n"),
                check.standard_output);
        // A participant that is left out, and a file that is not there.
        write_file(dir / "s.yaml", head + "  - expect: { Door.level: 0 }\n");
        const Captured disabled = launch(o, {"check", "sim.yaml", "--scenario", "s.yaml", "--disable", "Door"}, dir);
        expect_code(disabled, 1, "a scenario that uses a disabled participant");
        require(contains(lf(disabled.standard_output),
                         "error: s.yaml:3: 'Door.level': participant 'Door' is left out of this run (enabled: false, or --disable): "
                         "take it in (--enable Door), or run a scenario that does not use it [scenario-variable]\n"),
                disabled.standard_output);
        const Captured missing = launch(o, {"run", "sim.yaml", "--scenario", "none.yaml", "--json"}, dir);
        expect_code(missing, 1, "a scenario file that does not exist");
        require(contains(lf(missing.standard_output), "\"code\": \"file\",\n        \"file\": \"none.yaml\",") &&
                    contains(lf(missing.standard_output), "the scenario file does not exist: check the path given with --scenario"),
                missing.standard_output);
    }
    {
        // The command line.
        const fs::path dir = scenario_ecus(o, "", head + "  - set: { Body.level: 5 }\n");
        Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--stdin-commands", "--start-paused"}, dir);
        expect_code(run, 4, "--scenario with --start-paused");
        require(contains(run.standard_error, "error: --scenario cannot be used with --start-paused: the scenario writes its values "
                                             "for 0 before the first step and runs by itself; pause and step it with "
                                             "--stdin-commands or from a Control Server client (--control) if needed\n"),
                run.standard_error);
        run = launch(o, {"run", "sim.yaml", "--scenario"}, dir);
        expect_code(run, 4, "--scenario without a file");
        const Captured version = launch(o, {"version", "--json"}, dir);
        require(contains(version.standard_output, "\"scenario\""), "version --json lists the feature scenario");
        // With --control: a build with the control server serves it during the scenario (its
        // clients read and move the time, and do not write; tests/control_server/test_launcher.py),
        // and says so in its features.
        const bool server = contains(version.standard_output, "\"control-server\"");
        require(server == contains(version.standard_output, "\"scenario-control\"") &&
                    server == contains(version.standard_output, "\"scenario-time\""),
                "version --json lists scenario-control and scenario-time exactly with control-server: " +
                    version.standard_output);
        run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--control", "0"}, dir);
        if (server)
        {
            expect_code(run, 0, "--scenario with --control");
            require(contains(run.standard_error, "control server at 127.0.0.1:") && contains(run.standard_error, "scenario: "),
                    run.standard_error);
            run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--control", "0", "--start-paused"}, dir);
            expect_code(run, 4, "--scenario with --control and --start-paused");
            require(contains(run.standard_error, "error: --scenario cannot be used with --start-paused: "), run.standard_error);
        }
        else
        {
            expect_code(run, 4, "--scenario with --control, without the control server");
            require(contains(run.standard_error, "error: --control needs a vehicle_sil_launch built with the control server"),
                    run.standard_error);
        }
    }
}
