// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about run --scenario --timeline: the timeline file of a run:
//
//   timeline     run --scenario --timeline: the file of a run that passes (the same file from a
//                second run and at a speed; the run's own output is what it is without --timeline,
//                but for the file's name), of a run that fails, a file that cannot be opened
//                (nothing starts, exit code 3), runs that stop_at_ms and --timeout-ms end and one
//                that does not start, a file that cannot be written (Linux: /dev/full), the
//                command line
#include "launch_run_support.hpp"

/// vehicle_sil_launch test: every scenario of a directory with every variant, one run after the
/// other; a scenario that is not for a variant is skipped, a failed run does not end the test,
/// and the table and the JSON have every run.
void timeline(const Options& o)
{
    const std::string passing = "vehicle_sil_scenario: 1\n"
                                "name: square wave\n"
                                "steps:\n"
                                "  - set: { Body.level: 20, Door.mode: off }\n"
                                "  - at_ms: 100\n"
                                "    wave: { Body.level: { square: { low: 20, high: 30, period_ms: 40 }, for_ms: 100 } }\n"
                                "    expect: { Body.level: { ge: 20, le: 30 } }\n"
                                "  - expect: { Body.level_changed_at_ms: 200, Door.mode_now: off }\n";
    const fs::path dir = scenario_ecus(o, "", passing);
    const std::vector<std::string> plain{"run", "sim.yaml", "--scenario", "s.yaml"};
    const auto with = [&plain](std::vector<std::string> more) {
        std::vector<std::string> args = plain;
        args.insert(args.end(), more.begin(), more.end());
        return args;
    };
    // The file is written, and said on standard error; the directory is made if it is not there.
    const Captured first = launch(o, with({"--timeline", "out/first.json"}), dir);
    expect_code(first, 0, "run --timeline");
    require(contains(lf(first.standard_error), "\n[vehicle_sil_launch] timeline: out/first.json\n"),
            "the line that says where the timeline is:\n" + lf(first.standard_error));
    const std::string file = read_text(dir / "out" / "first.json");
    require(!contains(file, "\r"), "the timeline has LF line ends on every system");
    require(contains(file, "{\n  \"vehicle_sil_timeline\": 1,\n") && contains(file, "  \"stage\": \"run\",\n  \"file\": \"sim.yaml\",\n") &&
                contains(file, "  \"variant\": null,\n  \"params\": [],\n  \"time_step_ms\": 10,\n") &&
                contains(file, "    \"file\": \"s.yaml\",\n    \"name\": \"square wave\",\n    \"status\": \"passed\",\n"),
            "the head of the timeline:\n" + file);
    // What the scenario wrote (the wave: one point a line, the end value last) and saw.
    require(contains(file, "      \"variable\": \"Body.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
                           "      \"columns\": [\"at_ms\", \"value\"],\n      \"samples\": null,\n      \"first_ms\": null,\n      \"last_ms\": null,\n      \"not_applied_from\": null,\n      \"points\": [\n        [0, 20],\n        [100, 30],\n        [110, 30],\n        [120, 20],\n"),
            "the written values:\n" + file);
    require(contains(file, "      \"variable\": \"Body.level\",\n      \"source\": \"sampled\",\n      \"encoding\": \"changes\",\n"),
            "the sampled values:\n" + file);
    require(contains(file, "        \"mode\": \"during\",\n        \"from_ms\": 100,\n        \"to_ms\": 200,\n"),
            "the watch beside the wave:\n" + file);
    // The lines of the inputs: the wave has its values itself, so no base.
    require(contains(file, "      \"variable\": \"Body.level\",\n      \"kind\": \"wave\",\n      \"line\": 5,\n      \"step\": 2,\n"
                           "      \"from_ms\": 100,\n      \"to_ms\": 200,\n      \"base\": null,\n      \"base_source\": null,\n"
                           "      \"shape\": {\n        \"type\": \"square\",\n        \"low\": 20,\n        \"high\": 30,\n"
                           "        \"period_ms\": 40,\n        \"for_ms\": 100\n      },\n") &&
                contains(file, "      \"lines\": [0, 1],\n") && contains(file, "      \"lines\": [2],\n"),
            "the lines of the inputs:\n" + file);
    // The run's own output is the one without --timeline.
    const Captured without = launch(o, plain, dir);
    expect_code(without, 0, "run without --timeline");
    expect_text(first.standard_output, without.standard_output, "standard output with --timeline");
    require(!contains(lf(without.standard_error), "timeline"), "no word of a timeline without --timeline:\n" + lf(without.standard_error));
    const Captured json = launch(o, with({"--json", "--timeline", "second.json"}), dir);
    expect_code(json, 0, "run --json --timeline");
    require(contains(lf(json.standard_output), "\n    \"lines\": [],\n    \"timeline\": \"second.json\"\n  }\n}\n"),
            "the result names the timeline, last in its scenario:\n" + lf(json.standard_output));
    const Captured jsonWithout = launch(o, with({"--json"}), dir);
    require(contains(lf(jsonWithout.standard_output), "\n    \"lines\": [],\n    \"timeline\": null\n  }\n}\n"),
            "the timeline of the result is null without --timeline:\n" + lf(jsonWithout.standard_output));
    // The same run gives the same file: nothing of the wall clock is in it, also at a speed.
    expect_text(read_text(dir / "second.json"), file, "the timeline of a second run");
    expect_code(launch(o, with({"--speed", "4", "--timeline", "third.json"}), dir), 0, "run --speed 4 --timeline");
    expect_text(read_text(dir / "third.json"), file, "the timeline at a speed");
    // A run that falls behind real time (an ECU that takes 20 ms for a step, at four times real
    // time) says so in its result, and gives the same file: nothing of the pace is in it.
    write_file(dir / "Body.behaviour", "scenario-variables slow-ms 20\n");
    const Captured late = launch(o, with({"--speed", "4", "--timeline", "late.json"}), dir);
    expect_code(late, 0, "a run that falls behind, with --timeline");
    require(contains(lf(late.standard_output), "[behind-real-time]"), "the run fell behind real time:\n" + lf(late.standard_output));
    expect_text(read_text(dir / "late.json"), file, "the timeline of a run that fell behind real time");
    write_file(dir / "Body.behaviour", "scenario-variables\n");
    // With serve --once the line is a log event, and the result names the file.
    const Captured events = launch(o, {"serve", "--once", "sim.yaml", "--scenario", "s.yaml", "--timeline", "fourth.json"}, dir);
    expect_code(events, 0, "serve --once --timeline");
    require(contains(lf(events.standard_output), "\"line\":\"timeline: fourth.json\"") &&
                contains(lf(events.standard_output), "\"timeline\":\"fourth.json\""),
            "the events name the timeline:\n" + lf(events.standard_output));
    expect_text(read_text(dir / "fourth.json"), file, "the timeline with serve --once");

    // A file that cannot be opened: said before anything starts, as a set-up error.
    const Captured closed = launch(o, with({"--timeline", "s.yaml/in-a-file.json"}), dir);
    expect_code(closed, 3, "--timeline in a directory that is a file");
    require(contains(lf(closed.standard_output),
                     "cannot set up the simulation: cannot write the timeline s.yaml/in-a-file.json: check that --timeline names a "
                     "file in a directory you can write to (the file is opened before anything starts, so that a run is not "
                     "lost for it) [setup]"),
            "the set-up error:\n" + lf(closed.standard_output));
    require(!contains(lf(closed.standard_error), "registry at"), "nothing was started:\n" + lf(closed.standard_error));

    // A scenario that fails: the file is written all the same, with what failed as numbers.
    write_file(dir / "s.yaml", "vehicle_sil_scenario: 1\n"
                               "steps:\n"
                               "  - set: { Body.level: 20 }\n"
                               "  - at_ms: 50\n"
                               "    expect: { Body.level: 21 }\n"
                               "  - after_ms: 30\n"
                               "    set: { Body.level: 0 }\n");
    const Captured failing = launch(o, with({"--timeline", "failed.json"}), dir);
    expect_code(failing, 1, "a failing scenario with --timeline");
    const std::string failed = read_text(dir / "failed.json");
    require(contains(failed, "    \"status\": \"failed\",\n") && contains(failed, "    \"recorded_to_ms\": 50,\n"),
            "the failed scenario's timeline ends at the failure:\n" + failed);
    require(contains(failed, "      \"failed\": {\n        \"at_ms\": 50,\n        \"code\": \"scenario-failed\",\n"
                             "        \"conditions\": [\n          {\n            \"condition\": 0,\n            \"value\": 20,\n"
                             "            \"sample_ms\": 50\n          }\n        ],\n        \"writes\": [],\n        \"write\": null\n      }"),
            "what failed, as numbers:\n" + failed);
    require(contains(failed, "      \"code\": \"scenario-failed\",\n      \"file\": \"s.yaml\",\n      \"line\": 4,\n"),
            "the scenario's finding is in the file:\n" + failed);

    {
        // A set of several variables whose second setter throws (the test ECU's level 13): the
        // real agent has applied the write before it, and the file says so. Its mark is on the
        // write that was rejected, not on the one that was applied, and the samples of the same
        // file agree (mode_now has the mode that was written).
        const fs::path partly = scenario_ecus(o, "",
                                              "vehicle_sil_scenario: 1\nsteps:\n"
                                              "  - set: { Body.level: 5 }\n"
                                              "  - at_ms: 20\n"
                                              "    set: { Body.mode: auto, Body.level: 13, Door.level: 4 }\n"
                                              "  - at_ms: 60\n"
                                              "    expect: { Body.mode_now: auto }\n");
        const Captured rejected = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--timeline", "partly.json"}, partly);
        expect_code(rejected, 1, "a set whose second setter throws, with --timeline");
        const std::string text = read_text(partly / "partly.json");
        const auto write = [](const char* variable, const char* value, int point, const char* applied) {
            return std::string("          {\n            \"variable\": \"") + variable + "\",\n            \"at_ms\": 20,\n            \"value\": " +
                   value + ",\n            \"point\": " + std::to_string(point) + ",\n            \"applied\": " + applied + "\n          }";
        };
        require(contains(text, "the ECU rejected it (level: a setter threw: 13 is not a level); the ECU had already applied the write "
                               "before it in that command (Body.mode = auto (2)), and it stays applied\","),
                "the message says which write was applied:\n" + text);
        require(contains(text, "        \"writes\": [\n" + write("Body.mode", "2", 0, "true") + ",\n" + write("Body.level", "13", 1, "false") +
                                   ",\n" + write("Door.level", "4", 0, "true") + "\n        ],\n        \"write\": 1\n      }"),
                "the writes of the failed set, each with whether it was applied:\n" + text);
        const auto series = [&text](const std::string& variable, const std::string& source) {
            const auto from = text.find("      \"variable\": \"" + variable + "\",\n      \"source\": \"" + source + "\",\n");
            const auto to = text.find("\n    }", from);
            require(from != std::string::npos && to != std::string::npos, "no " + source + " series of " + variable + ":\n" + text);
            return text.substr(from, to - from);
        };
        require(contains(series("Body.mode", "written"), "      \"not_applied_from\": null,\n") && contains(series("Body.mode", "written"), "        [20, 2]\n"),
                "the write that was applied has no mark:\n" + text);
        require(contains(series("Body.level", "written"), "      \"not_applied_from\": 1,\n") &&
                    contains(series("Body.level", "written"), "        [0, 5],\n        [20, 13]\n"),
                "the write that was rejected is marked:\n" + text);
        require(contains(series("Body.mode_now", "sampled"), "        [0, 0],\n        [20, 2]\n"),
                "the samples have the mode that was applied:\n" + text);
    }

    // The head of a timeline: its "scenario" object.
    const auto head_of = [&dir](const std::string& name) {
        const std::string text = read_text(dir / name);
        const auto from = text.find("  \"scenario\": {");
        const auto to = text.find("  },\n", from);
        require(from != std::string::npos && to != std::string::npos, name + " has no scenario:\n" + text);
        return text.substr(from, to - from);
    };
    const std::string steps = "vehicle_sil_scenario: 1\nsteps:\n";
    {
        const fs::path stopped = scenario_ecus(o, "# run-option --stop-at-ms 100\n",
                                               steps + "  - at_ms: 50\n    set: { Body.level: 5 }\n"
                                                       "  - wait_until: { Body.level: 6 }\n    timeout_ms: 1000\n");
        expect_code(launch(o, with({"--timeline", "stopped.json"}), stopped), 5, "stop_at_ms with --timeline");
        require(contains(head_of("stopped.json"), "\"status\": \"incomplete\"") &&
                    contains(head_of("stopped.json"), "\"over_at_ms\": 100,"),
                "the timeline's stop boundary:\n" + head_of("stopped.json"));
        require(!contains(read_text(dir / "stopped.json"), "scenario-out-of-time") &&
                    contains(read_text(dir / "stopped.json"), "      \"points\": [\n        [50, 5]\n      ]\n"),
                "the write before the stop:\n" + read_text(dir / "stopped.json"));
    }
    {
        // An ECU hangs and --timeout-ms ends the run: incomplete, recorded to a sample it saw.
        const fs::path hung = scenario_ecus(o, "# run-option --participant-stop-timeout-ms 500\n",
                                            steps + "  - set: { Body.level: 0 }\n"
                                                    "  - ramp: { Body.level: { to: 100, over_ms: 2000 } }\n"
                                                    "    expect: { Body.level: { ge: 0 } }\n",
                                            "hang-at-ms 50");
        expect_code(launch(o, with({"--timeline", "hung.json", "--timeout-ms", "1500"}), hung), 1, "--timeout-ms with --timeline");
        const std::string head = head_of("hung.json");
        require(contains(head, "    \"status\": \"incomplete\",\n    \"over_at_ms\": null,\n    \"recorded_to_ms\": ") &&
                    !contains(head, "\"recorded_to_ms\": null") && contains(read_text(dir / "hung.json"), "        [0, 0],\n"),
                "the timeline of a run that --timeout-ms ended:\n" + read_text(dir / "hung.json"));
    }
    {
        // A participant ends before the start: the scenario did not run, and the file says so.
        const fs::path unstarted = scenario_dir(o, "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                                                   "  - name: Body\n    command: [" + ecu(o) + "]\n"
                                                   "  - name: Door\n    command: [" + ecu(o) + "]\n"
                                                   "  - name: Ghost\n    command: [" + yaml_quote(shown_path(o.launch)) + ", version]\n"
                                                   "    describe: false\n",
                                                {{"Body", "scenario-variables"}, {"Door", "scenario-variables"}});
        write_file(unstarted / "s.yaml", steps + "  - set: { Body.level: 5 }\n");
        expect_code(launch(o, with({"--timeline", "unstarted.json"}), unstarted), 1, "a run that does not start, with --timeline");
        const std::string whole = read_text(dir / "unstarted.json");
        expect_text(head_of("unstarted.json"),
                    "  \"scenario\": {\n    \"file\": \"s.yaml\",\n    \"name\": null,\n    \"status\": \"not_run\",\n"
                    "    \"over_at_ms\": null,\n    \"recorded_to_ms\": null,\n    \"end_ms\": null\n",
                    "the timeline of a run that did not start");
        require(contains(whole, "  \"series\": [],\n") && contains(whole, "      \"status\": \"not_run\",\n"),
                "no series, and the step not run:\n" + whole);
    }
#ifdef __linux__
    {
        // The file can be opened and not written (a full disk): an error of its own after the
        // run, and the scenario's result stands.
        const fs::path full = scenario_ecus(o, "", steps + "  - set: { Body.level: 5 }\n");
        const Captured run = launch(o, with({"--timeline", "/dev/full"}), full);
        expect_code(run, 1, "--timeline /dev/full");
        const std::string out = lf(run.standard_output);
        require(contains(out, "scenario s.yaml: passed, 1 of 1 steps passed\n") &&
                    contains(out, "error: sim.yaml: the timeline /dev/full could not be written: writing ") &&
                    contains(out, " bytes to it failed (is the disk full?); what is in the file is not complete: do not use it. The "
                                  "scenario's result is not changed by this. Make room on the disk, or name another file with "
                                  "--timeline, and run again [timeline-write]\n") &&
                    contains(out, "\nfailed: 1 error, 0 warnings, 0 notes\n"),
                "the timeline that could not be written:\n" + out);
        require(!contains(lf(run.standard_error), "] timeline: "), "no line that says the timeline is there:\n" + lf(run.standard_error));
    }
#endif

    // The command line.
    const Captured alone = launch(o, {"run", "sim.yaml", "--timeline", "x.json"}, dir);
    expect_code(alone, 4, "--timeline without --scenario");
    require(contains(lf(alone.standard_error), "error: --timeline writes what a scenario did over the time: give --scenario <file> "
                                               "too, or leave --timeline out\n"),
            "--timeline without --scenario:\n" + lf(alone.standard_error));
    const Captured checked = launch(o, {"check", "sim.yaml", "--scenario", "s.yaml", "--timeline", "x.json"}, dir);
    expect_code(checked, 4, "check --timeline");
    require(contains(lf(checked.standard_error), "error: --timeline is for run: check starts no participant\n"),
            "check --timeline:\n" + lf(checked.standard_error));
    require(!fs::exists(dir / "x.json"), "a wrong command line writes no file");

    // Extra signal recording (--record / request "record" / serve inline intent.record):
    // 1. Run a Body-only scenario with --record on Door.mode_now (not in scenario), Door.ecu.connected,
    //    and Body.level (written-only in scenario) -> timeline records all of them with recorded: true.
    // 2. Run the same scenario with and without --record under --strict -> identical verdict, steps,
    //    timestamps, and writes (verdict invariance).
    // 3. check/run with --record Body.nonexistent fails pre-run before starting any ECU.
    {
        const std::string bodyOnly = "vehicle_sil_scenario: 1\n"
                                     "name: body only\n"
                                     "steps:\n"
                                     "  - set: { Body.level: 20 }\n"
                                     "  - at_ms: 50\n"
                                     "    expect: { Body.level_changed_at_ms: 0 }\n";
        const fs::path recDir = scenario_ecus(o, "", bodyOnly);

        const Captured baseJson = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--strict", "--json", "--timeline", "base.json"}, recDir);
        expect_code(baseJson, 0, "baseline --strict run");
        const std::string baseTimeline = read_text(recDir / "base.json");
        require(!contains(baseTimeline, "\"recorded\""), "baseline timeline omits recorded:\n" + baseTimeline);

        const Captured recJson =
            launch(o,
                   {"run", "sim.yaml", "--scenario", "s.yaml", "--strict", "--json", "--timeline", "extra.json",
                    "--record", "Door.mode_now", "--record", "Door.ecu.connected", "--record", "Body.level"},
                   recDir);
        expect_code(recJson, 0, "extra --record --strict run");

        // Verdict invariance: comparing run --json output (stripping wall_ms/actual_speed/registry_uri and timeline filename).
        auto normalizeRunJson = [](std::string text, const std::string& tlName) {
            text = without_timing(lf(std::move(text)));
            const std::string from = "\"timeline\": \"" + tlName + "\"";
            const auto pos = text.find(from);
            if (pos != std::string::npos)
            {
                text.replace(pos, from.size(), "\"timeline\": \"<tl>\"");
            }
            return text;
        };
        expect_text(normalizeRunJson(recJson.standard_output, "extra.json"),
                    normalizeRunJson(baseJson.standard_output, "base.json"),
                    "run --json invariant with and without --record under --strict");

        const std::string extraTimeline = read_text(recDir / "extra.json");
        require(contains(extraTimeline, "\"name\": \"Door.mode_now\"") &&
                    contains(extraTimeline, "\"name\": \"Door.ecu.connected\"") &&
                    contains(extraTimeline, "\"written\": false,\n      \"watched\": false,\n      \"recorded\": true") &&
                    contains(extraTimeline, "\"written\": true,\n      \"watched\": false,\n      \"recorded\": true") &&
                    contains(extraTimeline, "\"variable\": \"Door.mode_now\",\n      \"source\": \"sampled\"") &&
                    contains(extraTimeline, "\"variable\": \"Door.ecu.connected\",\n      \"source\": \"sampled\"") &&
                    contains(extraTimeline, "\"variable\": \"Body.level\",\n      \"source\": \"sampled\""),
                "extra recorded signals and series in timeline:\n" + extraTimeline);

        // Request file "record" and serve --once inline intent.record produce the exact same timeline!
        write_file(recDir / "req.json",
                   "{\n"
                   "  \"vehicle_sil_run_request\": 1,\n"
                   "  \"scenario\": \"s.yaml\",\n"
                   "  \"strict\": true,\n"
                   "  \"record\": [\"Door.mode_now\", \"Door.ecu.connected\", \"Body.level\"]\n"
                   "}\n");
        const Captured reqRun = launch(o, {"run", "sim.yaml", "--request", "req.json", "--timeline", "from_req.json"}, recDir);
        expect_code(reqRun, 0, "run --request with record");
        expect_text(read_text(recDir / "from_req.json"), extraTimeline, "timeline from request file record");

        const Captured serveRun = launch(
            o,
            {"serve", "--once", "sim.yaml", "--scenario", "s.yaml", "--strict", "--timeline", "from_serve.json",
             "--record", "Door.mode_now", "--record", "Door.ecu.connected", "--record", "Body.level"},
            recDir);
        expect_code(serveRun, 0, "serve --once with inline intent.record");
        expect_text(read_text(recDir / "from_serve.json"), extraTimeline, "timeline from serve --once inline intent.record");

        // Pre-run validation: check and run fail before starting any ECU on unknown variable.
        const Captured badCheck = launch(o, {"check", "sim.yaml", "--record", "Body.nonexistent"}, recDir);
        expect_code(badCheck, 1, "check --record Body.nonexistent");
        require(contains(lf(badCheck.standard_output), "[record-unknown-variable]"),
                "check reports record-unknown-variable:\n" + lf(badCheck.standard_output));

        const Captured badRun = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml", "--record", "Body.nonexistent"}, recDir);
        expect_code(badRun, 1, "run --record Body.nonexistent");
        require(contains(lf(badRun.standard_output), "[record-unknown-variable]") &&
                    !contains(lf(badRun.standard_error), "registry at"),
                "run fails pre-run before starting any ECU:\n" + lf(badRun.standard_output) + "\n" + lf(badRun.standard_error));
    }

    // Absent or delayed optional (required: false) record-only ECU must NOT wait in expected_agents(),
    // emit control-link-no-hello, or alter scenario pass/fail or exit code (even under --strict).
    {
        const std::string cfg =
            "vehicle_sil_simulation: 2\n"
            "networks: []\n"
            "participants:\n"
            "  - name: Body\n"
            "    command: [" + ecu(o) + "]\n"
            "  - name: OptBridge\n"
            "    role: bridge\n"
            "    required: false\n"
            "    manual: true\n"
            "    command: [" + ecu(o) + "]\n";
        const fs::path optDir =
            scenario_dir(o, cfg, {{"Body", "scenario-variables"}, {"OptBridge", "scenario-variables"}});
        write_file(optDir / "s.yaml",
                   "vehicle_sil_scenario: 1\n"
                   "name: body only with optional bridge\n"
                   "steps:\n"
                   "  - set: { Body.level: 20 }\n"
                   "  - at_ms: 50\n"
                   "    expect: { Body.level_changed_at_ms: 0 }\n");

        const Captured baseRun =
            launch(o,
                   {"run", "sim.yaml", "--scenario", "s.yaml", "--time-policy", "free", "--speed", "max",
                    "--strict", "--json", "--timeline", "base.json"},
                   optDir);
        expect_code(baseRun, 0, "optional record-only ECU: baseline --strict run");

        const Captured recRun =
            launch(o,
                   {"run", "sim.yaml", "--scenario", "s.yaml", "--time-policy", "free", "--speed", "max",
                    "--strict", "--json", "--timeline", "opt.json", "--record", "OptBridge.mode_now"},
                   optDir);
        expect_code(recRun, 0, "optional record-only ECU: --record keeps --strict exit 0");
        require(!contains(recRun.standard_output, "control-link-no-hello"),
                "optional record-only ECU must not emit control-link-no-hello:\n" + recRun.standard_output);
        auto normalizeOptJson = [](std::string text, const std::string& tlName) {
            text = without_timing(lf(std::move(text)));
            const std::string from = "\"timeline\": \"" + tlName + "\"";
            const auto pos = text.find(from);
            if (pos != std::string::npos)
            {
                text.replace(pos, from.size(), "\"timeline\": \"<tl>\"");
            }
            return text;
        };
        expect_text(normalizeOptJson(recRun.standard_output, "opt.json"),
                    normalizeOptJson(baseRun.standard_output, "base.json"),
                    "optional record-only ECU run --json identical with and without --record under --strict");
        const std::string optTl = read_text(optDir / "opt.json");
        require(contains(optTl, "\"name\": \"OptBridge.mode_now\"") && contains(optTl, "\"recorded\": true"),
                "optional record-only ECU timeline declares recorded variable:\n" + optTl);
    }

    // Post-failure tail recording (--record-after-fail-ms / request "record_after_fail_ms" / serve intent):
    // 1. Representative failing scenario on real examples/ecu_main (Engine + Dashboard):
    //    set Engine.engine.throttle = 50; wait_until Engine.engine.speed >= 1700 (timeout_ms: 200);
    //    after_ms: 10 set Engine.engine.throttle = 0 (must NOT run!).
    //    With --record Dashboard.dashboard.engine_speed --record-after-fail-ms 300:
    //    - exit code 1, failed step at_ms == 200, recorded_to_ms == 500
    //    - step 3 not_run, zero writes after failure
    //    - both Engine.engine.speed and Dashboard.dashboard.engine_speed climb past 1700 rpm (to 1800) during tail!
    // 2. Without --record-after-fail-ms: recorded_to_ms == 200 (unchanged).
    // 3. serve --once inline intent produces the exact same timeline.
    // 4. User Stop during a long tail (--speed 1 + stdin stop) stops promptly and preserves Failed verdict!
    // 5. CLI usage errors on invalid values, missing scenario, and unsupported subcommands.
    {
        const fs::path exDir = example_copy(o);
        write_file(exDir / "fail_slow.yaml",
                   "vehicle_sil_scenario: 1\n"
                   "name: slow engine climb\n"
                   "steps:\n"
                   "  - set: { Engine.engine.throttle: 50 }\n"
                   "  - wait_until: { Engine.engine.speed: { ge: 1700 } }\n"
                   "    timeout_ms: 200\n"
                   "  - after_ms: 10\n"
                   "    set: { Engine.engine.throttle: 0 }\n");

        const Captured noTail = launch(
            o,
            {"run", "simulation.yaml", "--scenario", "fail_slow.yaml",
             "--record", "Dashboard.dashboard.engine_speed", "--timeline", "no_tail.json"},
            exDir);
        expect_code(noTail, 1, "failing scenario without --record-after-fail-ms");
        const std::string noTailText = read_text(exDir / "no_tail.json");
        require(contains(noTailText, "    \"status\": \"failed\",\n") &&
                    contains(noTailText, "    \"recorded_to_ms\": 200,\n") &&
                    contains(noTailText, "        \"at_ms\": 200,\n        \"code\": \"scenario-timeout\","),
                "baseline failing timeline without tail:\n" + noTailText);

        const Captured withTail = launch(
            o,
            {"run", "simulation.yaml", "--scenario", "fail_slow.yaml",
             "--record", "Dashboard.dashboard.engine_speed",
             "--record-after-fail-ms", "300", "--timeline", "with_tail.json"},
            exDir);
        expect_code(withTail, 1, "failing scenario with --record-after-fail-ms 300");
        const std::string tailText = read_text(exDir / "with_tail.json");
        require(contains(tailText, "    \"status\": \"failed\",\n") &&
                    contains(tailText, "    \"recorded_to_ms\": 500,\n") &&
                    contains(tailText, "        \"at_ms\": 200,\n        \"code\": \"scenario-timeout\",") &&
                    contains(tailText, "      \"index\": 3,\n      \"line\": 7,\n      \"kind\": \"set\",\n"
                                       "      \"text\": \"set Engine.engine.throttle = 0\",\n      \"status\": \"not_run\","),
                "failing timeline with 300ms tail:\n" + tailText);

        const auto series_of = [](const std::string& text, const std::string& variable, const std::string& source) {
            const auto from = text.find("      \"variable\": \"" + variable + "\",\n      \"source\": \"" + source + "\",\n");
            const auto to = text.find("\n    }", from);
            require(from != std::string::npos && to != std::string::npos, "missing " + source + " series for " + variable + ":\n" + text);
            return text.substr(from, to - from);
        };
        // Zero writes after failure: Engine.engine.throttle has only [0, 50].
        const std::string throttleWritten = series_of(tailText, "Engine.engine.throttle", "written");
        require(contains(throttleWritten, "      \"points\": [\n        [0, 50]\n      ]"),
                "zero writes after failure:\n" + throttleWritten);
        // Engine.engine.speed continues recording up to 500ms and climbs past 1700 rpm to 1800 rpm.
        const std::string engineSpeed = series_of(tailText, "Engine.engine.speed", "sampled");
        require(contains(engineSpeed, "\"last_ms\": 500,") &&
                    contains(engineSpeed, ", 1750]") && contains(engineSpeed, ", 1800]"),
                "Engine.engine.speed post-failure samples up to 500ms climbing past 1700 rpm:\n" + engineSpeed);
        // Dashboard.dashboard.engine_speed (extra recorded signal) continues recording up to 500ms and climbs past 1700 rpm!
        const std::string dashSpeed = series_of(tailText, "Dashboard.dashboard.engine_speed", "sampled");
        require(contains(dashSpeed, "\"last_ms\": 500,") && contains(dashSpeed, ", 1800]"),
                "Dashboard.dashboard.engine_speed post-failure samples up to 500ms climbing past 1700 rpm:\n" + dashSpeed);

        // serve --once inline intent produces the exact same timeline.
        const Captured serveTail = launch(
            o,
            {"serve", "--once", "simulation.yaml", "--scenario", "fail_slow.yaml",
             "--record", "Dashboard.dashboard.engine_speed",
             "--record-after-fail-ms", "300", "--timeline", "serve_tail.json"},
            exDir);
        expect_code(serveTail, 1, "serve --once with record_after_fail_ms 300");
        expect_text(read_text(exDir / "serve_tail.json"), tailText, "serve --once timeline with record_after_fail_ms");

        // User Stop during a long tail (--speed 1 + stdin stop right after failure):
        // stops promptly without waiting out the 10-second tail and preserves Failed verdict!
        {
            std::mutex mu;
            std::condition_variable cv;
            bool sawFailure = false;
            std::string errLog;
            std::string outLog;
            vehicle_sil::process::ProcessOptions procOpts;
            procOpts.working_directory = exDir;
            procOpts.input = true;
            procOpts.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
                const std::lock_guard lock(mu);
                if (stream == vehicle_sil::process::Stream::Error)
                {
                    errLog += std::string(line) + "\n";
                    if (line.find("FAILED at 200 ms") != std::string_view::npos)
                    {
                        sawFailure = true;
                        cv.notify_all();
                    }
                }
                else
                {
                    outLog += std::string(line) + "\n";
                }
            };
            const auto started = std::chrono::steady_clock::now();
            vehicle_sil::process::Process proc(
                o.launch,
                {"run", "simulation.yaml", "--scenario", "fail_slow.yaml",
                 "--speed", "1", "--stdin-commands",
                 "--record-after-fail-ms", "10000", "--timeline", "stop_during_tail.json"},
                std::move(procOpts));
            {
                std::unique_lock lock(mu);
                require(cv.wait_for(lock, scaled_timeout(20s), [&] { return sawFailure; }),
                        "did not observe scenario FAILED line before sending stop:\n" + errLog);
            }
            require(proc.write_input("{\"command\":\"stop\"}\n"), "failed to send stdin stop during tail");
            const auto status = proc.wait_for(scaled_timeout(20s));
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (!status)
            {
                proc.kill_tree();
            }
            proc.finish_output(10s);
            require(status.has_value() && status->code == std::optional<int>(1),
                    "stop during tail must exit with code 1 (Failed):\n" + outLog + "\n" + errLog);
            require(elapsed < scaled_timeout(8s),
                    "stop during 10s tail must finish promptly instead of waiting out the full tail");
            const std::string stopTailText = read_text(exDir / "stop_during_tail.json");
            require(contains(stopTailText, "    \"status\": \"failed\",\n") &&
                        contains(stopTailText, "        \"at_ms\": 200,\n        \"code\": \"scenario-timeout\","),
                    "stop during tail preserves Failed verdict and failed step:\n" + stopTailText);
            const auto recPos = stopTailText.find("    \"recorded_to_ms\": ");
            require(recPos != std::string::npos, "missing recorded_to_ms:\n" + stopTailText);
            const double recTo = std::stod(stopTailText.substr(recPos + 22));
            require(recTo >= 200.0 && recTo < 10000.0,
                    "recorded_to_ms after early stop during tail should be in [200, 10000), got " + std::to_string(recTo));
        }

        // Regression (phys_leak): on default simulation.yaml (time_step_ms=5, Engine step_ms=10),
        // Step 2 (expect at 100ms) fails at over_at_ms=105ms; Step 3 (after_ms: 5, at=105ms,
        // applied_step=110ms > 105ms) must NOT be submitted at tick(100) or applied during the tail!
        {
            write_file(exDir / "phys_leak.yaml",
                       "vehicle_sil_scenario: 1\n"
                       "name: phys_leak\n"
                       "steps:\n"
                       "  - at_ms: 0\n"
                       "    set: { Engine.engine.throttle: 0 }\n"
                       "  - at_ms: 100\n"
                       "    expect: { Engine.engine.speed: { ge: 9999 } }\n"
                       "  - after_ms: 5\n"
                       "    set: { Engine.engine.throttle: 100 }\n");
            const Captured leakRun = launch(
                o,
                {"run", "simulation.yaml", "--scenario", "phys_leak.yaml",
                 "--record", "Engine.engine.throttle",
                 "--record", "Dashboard.dashboard.engine_speed",
                 "--record-after-fail-ms", "400", "--speed", "max",
                 "--timeline", "phys_leak.json"},
                exDir);
            expect_code(leakRun, 1, "phys_leak regression run");
            const std::string leakText = read_text(exDir / "phys_leak.json");
            require(contains(leakText, "    \"status\": \"failed\",\n") &&
                        contains(leakText, "    \"over_at_ms\": 105,\n") &&
                        contains(leakText, "    \"recorded_to_ms\": 500,\n") &&
                        contains(leakText, "\"text\": \"set Engine.engine.throttle = 100\",\n      \"status\": \"not_run\","),
                    "phys_leak timeline status and step 3 not_run:\n" + leakText);
            const std::string thrWritten = series_of(leakText, "Engine.engine.throttle", "written");
            require(contains(thrWritten, "      \"points\": [\n        [0, 0]\n      ]") &&
                        !contains(thrWritten, "[105, 100]"),
                    "phys_leak written throttle must not contain [105, 100]:\n" + thrWritten);
            const std::string thrSampled = series_of(leakText, "Engine.engine.throttle", "sampled");
            require(contains(thrSampled, "      \"points\": [\n        [0, 0]\n      ]") &&
                        !contains(thrSampled, ", 100]"),
                    "phys_leak sampled throttle must stay 0 through tail:\n" + thrSampled);
            const std::string engSpeed = series_of(leakText, "Engine.engine.speed", "sampled");
            require(contains(engSpeed, "\"last_ms\": 500,") &&
                        contains(engSpeed, ", 800]") && !contains(engSpeed, ", 850]"),
                    "phys_leak Engine.engine.speed must remain at idle 800 rpm:\n" + engSpeed);
        }

        // CLI usage errors for --record-after-fail-ms:
        for (const std::vector<std::string>& badArgs : std::vector<std::vector<std::string>>{
                 {"run", "simulation.yaml", "--scenario", "fail_slow.yaml", "--record-after-fail-ms", "0"},
                 {"run", "simulation.yaml", "--scenario", "fail_slow.yaml", "--record-after-fail-ms", "-5"},
                 {"run", "simulation.yaml", "--scenario", "fail_slow.yaml", "--record-after-fail-ms", "600001"},
                 {"run", "simulation.yaml", "--scenario", "fail_slow.yaml", "--record-after-fail-ms", "abc"},
                 {"run", "simulation.yaml", "--record-after-fail-ms", "300"},
                 {"check", "simulation.yaml", "--record-after-fail-ms", "300"},
                 {"test", "simulation.yaml", "--record-after-fail-ms", "300", "--scenarios", "fail_slow.yaml"},
                 {"serve", "simulation.yaml", "--record-after-fail-ms", "300"},
                 {"connections", "simulation.yaml", "--record-after-fail-ms", "300"}})
        {
            const Captured bad = launch(o, badArgs, exDir);
            expect_code(bad, 4, "invalid --record-after-fail-ms usage");
        }
    }
}
