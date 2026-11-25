// SPDX-License-Identifier: MIT
//
// The launch_run scenarios about the participants' processes: the run of the example, a
// participant that fails, hangs or leaves a process behind, an interrupt, a time-out, the
// command line:
//
//   docs         the run of the example and its output in docs/simulation-run.md (text and
//                JSON, byte for byte but for the version; also without Dashboard), the
//                participants' lines on stderr, --log-dir
//   check        a configuration with an error: nothing is started (exit 1)
//   crash        a required ECU's process ends while the simulation runs: aborted (exit 1)
//   step         a required ECU's step throws: aborted (exit 1)
//   optional     a participant with required: false fails: a warning (exit 0; --strict 1)
//   not_joined   a required participant never joins: the start times out (exit 1)
//   quitter      a required participant ends before the start: nothing waits for the time-out
//   hang         ECUs that do not end after the simulation: stopped, and killed (exit 1)
//   grandchild   an ECU leaves a process running: the launcher ends it (a warning)
//   path         a bare executable name found on PATH; the working directory of the participants
//   interrupt    the launcher is interrupted (POSIX SIGINT, Windows Ctrl+Break): it stops the
//                simulation in order and exits 130; no participant is left
//   interrupt_twice  a second interrupt ends the waiting for a participant that does not end
//   interrupt_ending an interrupt while the launcher waits for the participants after the
//                simulation: it stops them at once and exits 130
//   ended_early  a participant stops the simulation long before stop_at_ms: a note
//   timeout      an ECU hangs in a step: --timeout-ms aborts the run
//   no_console   (Windows) the launcher without a console ends a participant at once
//   killed       the launcher dies (SIGKILL / TerminateProcess): no participant is left
//   usage        wrong command lines (exit 4), also with --json
//   disabled     participants left out (enabled: false, --disable): not started, not waited for,
//                no log; --enable takes one in again; a name the file does not have (exit 4)
#include "launch_run_support.hpp"

/// How the launcher asks a participant to stop, as the result names it.
const std::string kStop =
#ifdef _WIN32
    "Ctrl+Break";
#else
    "SIGTERM";
#endif

/// The copies in docs/howto/*.md: the block after each line "<!-- vehicle_sil_launch {command} ... -->"
/// there is the block after the same line in a document directly in `docs` (which this test compares
/// with the output).
void howto_copies_agree(const fs::path& docs, const std::string& command)
{
    const std::string head = "<!-- vehicle_sil_launch " + command + " ";
    const fs::path howto = docs / "howto";
    std::size_t copies = 0;
    for (const auto& entry : fs::directory_iterator(howto))
    {
        if (entry.path().extension() != ".md")
        {
            continue;
        }
        std::istringstream in(lf(read_text(entry.path())));
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind(head, 0) == 0 && line.size() > 4 && line.compare(line.size() - 4, 4, " -->") == 0)
            {
                const fs::path original = marked_file(docs, line);
                expect_text(block_after(entry.path(), line), block_after(original, line),
                            entry.path().generic_string() + ": the copy after " + line +
                                " (copy the block after the same line in " + original.filename().string() + ")");
                ++copies;
            }
        }
    }
    require(copies > 0, "no copy of a vehicle_sil_launch " + command + " example in " + howto.string());
}

void docs(const Options& o)
{
    const fs::path dir = example_copy(o);
    const Captured text = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "1000"}, dir);
    expect_code(text, 0, "run simulation.yaml");
    expect_text(text.standard_output, block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml -->"),
                "run simulation.yaml");
    const std::string err = lf(text.standard_error);
    require(contains(err, "\n[Dashboard] t=0 ms engine speed 0 rpm (0 messages)\n"),
            "the participants' lines on stderr after [<name>]:\n" + err);
    require(err.rfind("[vehicle_sil_launch] registry at silkit://127.0.0.1:", 0) == 0,
            "the first line on stderr is where the registry listens:\n" + err);
    require(contains(err, "\n[vehicle_sil_launch] simulation running\n"), "the start on stderr:\n" + err);

    const Captured without = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "1000", "--disable", "Dashboard"}, dir);
    expect_code(without, 0, "run simulation.yaml --disable Dashboard");
    expect_text(without.standard_output,
                block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --disable Dashboard -->"),
                "run simulation.yaml --disable Dashboard");

    const Captured json = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "1000", "--json"}, dir);
    expect_code(json, 0, "run simulation.yaml --json");
    // Byte for byte: "\n" also on Windows.
    expect_text(without_timing(without_versions(json.standard_output)),
                without_versions(block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --json -->")),
                "run simulation.yaml --json");
    // registry_uri is where the registry listened (the port it took), as the "registry at" line says.
    const std::string at = "[vehicle_sil_launch] registry at ";
    const auto atLine = json.standard_error.find(at);
    require(atLine != std::string::npos, "run --json says where the registry is on stderr:\n" + json.standard_error);
    const std::string listened = json.standard_error.substr(atLine + at.size(), json.standard_error.find_first_of("\r\n", atLine) - atLine - at.size());
    require(listened.rfind("silkit://127.0.0.1:", 0) == 0 && listened != "silkit://127.0.0.1:0" &&
                contains(json.standard_output, "  \"registry_uri\": \"" + listened + "\",\n"),
            "registry_uri of run --json is " + listened + ":\n" + json.standard_output);
    // As fast as possible: the time as it went, never late, never paused.
    require(contains(json.standard_output, "  \"timing\": {\n    \"speed\": \"max\",\n    \"time_step_ms\": 5,\n") &&
                contains(json.standard_output, "    \"sim_ms\": 1000,\n") &&
                contains(json.standard_output, "    \"paced_steps\": 0,\n    \"late_steps\": 0,\n    \"max_late_ms\": 0,\n"
                                               "    \"slipped_ms\": 0,\n    \"late_ms\": 0,\n    \"paused_ms\": 0\n  },\n"),
            "the timing of a run as fast as possible:\n" + json.standard_output);
    // With a speed: the line of the time, as the manual shows it (the numbers that depend on the
    // machine as <...>). The controller steps every 5 ms then, as Dashboard does: 200 steps.
    const Captured paced = launch(o, {"run", "simulation.yaml", "--stop-at-ms", "1000", "--speed", "4"}, dir);
    expect_code(paced, 0, "run simulation.yaml --speed 4");
    const auto timeAt = paced.standard_output.find("\ntime: ");
    require(timeAt != std::string::npos, "no line of the time:\n" + paced.standard_output);
    std::string timeLine = paced.standard_output.substr(timeAt + 1, paced.standard_output.find('\n', timeAt + 1) - timeAt);
    for (const auto& [before, name] : std::vector<std::pair<std::string, std::string>>{
             {"ran at ", "<actual_speed>"}, {"real time; ", "<late_steps>"}, {"the latest by ", "<max_late_ms>"}, {"ms), ", "<slipped_ms>"}})
    {
        const auto from = timeLine.find(before);
        require(from != std::string::npos, "the line of the time has no '" + before + "': " + timeLine);
        timeLine.replace(from + before.size(), timeLine.find(' ', from + before.size()) - from - before.size(), name);
    }
    expect_text(timeLine, block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --speed 4: the line of the time -->"),
                "the line of the time");
    howto_copies_agree(o.docs, "run");

    const fs::path logs = o.work_dir / "logs";
    const Captured logged = launch(o, {"run", "--json", "--log-dir", shown_path(logs), "--stop-at-ms", "1000", "simulation.yaml"}, dir);
    expect_code(logged, 0, "run --log-dir");
    // The JSON names each participant's file.
    require(contains(logged.standard_output, "      \"log\": \"" + shown_path(logs / "Dashboard.log") + "\",\n"),
            "the JSON names Dashboard's log:\n" + logged.standard_output);
    const std::string dashboard = lf(read_text(logs / "Dashboard.log"));
    require(dashboard.rfind("t=0 ms engine speed 0 rpm (0 messages)\n", 0) == 0,
            "Dashboard.log has the lines without [Dashboard]:\n" + dashboard);
    require(fs::exists(logs / "Engine.log"), "--log-dir makes a file for every participant (Engine.log)");
}

void check(const Options& o)
{
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 2\n"
                                         "networks: []\n"
                                         "participants:\n"
                                         "  - name: Logger\n"
                                         "    command: [" + helper(o) + ", --pid-file, logger.pid]\n"
                                         "    describe: false\n"
                                         "    requried: false\n");
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 1, "run with an error in the file");
    expect_text(run.standard_output,
                "sim.yaml: 1 participant, 0 networks\n"
                "\n"
                "participant  step  networks\n"
                "Logger       -     not checked (describe: false)\n"
                "\n"
                "error: sim.yaml:7: participant 'Logger': unknown key 'requried': did you mean 'required'? (the keys "
                "here: name, command, step_ms, describe, required, enabled, manual, role, variables, network_names, j1939_nodes) [unknown-key]\n"
                "note: sim.yaml:4: participant 'Logger': not checked (describe: false): vehicle_sil_launch cannot "
                "compare its name, step and networks with this file [not-checked]\n"
                "note: sim.yaml:5: participant 'Logger': the command has no {registry_uri}, so the executable is not told "
                "where the simulation's registry is: if it needs it, add its option for the registry URI with "
                "\"{registry_uri}\" as the value [no-registry-uri]\n"
                "\n"
                "nothing was started: the check found errors (vehicle_sil_launch check sim.yaml shows the same)\n"
                "\n"
                "failed: 1 error, 0 warnings, 2 notes\n",
                "run with an error in the file");
    require(!fs::exists(dir / "logger.pid"), "nothing was started");
    require(!contains(run.standard_error, "[vehicle_sil_launch] registry"), "no registry was started:\n" + run.standard_error);
}

const std::string kTwoEcusTable = "participant  step   networks\n";

void crash(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "pid exit-at-ms 200 7"}, {"Brake", "pid"}});
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 1, "run with a crashing ECU");
    expect_text(run.standard_output,
                "sim.yaml: 2 participants, 0 networks\n"
                "\n"
                "participant  step   networks\n"
                "Engine       10 ms  -\n"
                "Brake        10 ms  -\n"
                "\n"
                "run: the simulation was aborted: participant 'Engine' failed\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       exit code 7\n"
                "Brake        yes       exit code 0\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "error: sim.yaml:5: participant 'Engine': it failed while the simulation was running, and the launcher "
                "aborted the simulation: its process exited with code 7; the lines [Engine] above show what it wrote [participant-failed]\n"
                "\n"
                "failed: 1 error, 0 warnings, 0 notes\n",
                "run with a crashing ECU");
    require(all_gone(dir, {"Engine.pid", "Brake.pid"}, 10s), "every participant has ended");
}

void step(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "throw-at-ms 200"}});
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 1, "run with an ECU whose step fails");
    expect_text(run_part(run.standard_output),
                "run: the simulation was aborted: participant 'Engine' failed\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       exit code 1\n"
                "Brake        yes       exit code 0\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "error: sim.yaml:5: participant 'Engine': it failed while the simulation was running, and the launcher "
                "aborted the simulation: its process exited with code 1; the lines [Engine] above show what it wrote [participant-failed]\n"
                "\n"
                "failed: 1 error, 0 warnings, 0 notes\n",
                "run with an ECU whose step fails");
    require(contains(lf(run.standard_error), "[Engine] error: Engine: "), "the ECU's own message after [Engine]:\n" + run.standard_error);
}

void optional(const Options& o)
{
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 2\n"
                                         "# run-option --stop-at-ms 1000\n"
                                         "networks: []\n"
                                         "participants:\n"
                                         "  - name: Engine\n"
                                         "    command: [" + ecu(o) + "]\n"
                                         "  - name: Logger\n"
                                         "    command: [" + helper(o) + ", --err, \"cannot open the log\\n\", --exit, \"5\", --uri, \"{registry_uri}\"]\n"
                                         "    describe: false\n"
                                         "    required: false\n");
    const std::string expected = "run: the simulation ran to stop_at_ms (1000 ms) and ended\n"
                                 "\n"
                                 "participant  required  ended\n"
                                 "Engine       yes       exit code 0\n"
                                 "Logger       no        exit code 5\n"
                                 "\n"
                                 "traffic: no network in use\n"
                                 "\n"
                                 "warning: sim.yaml:7: participant 'Logger': its process exited with code 5; the lines "
                                 "[Logger] above show what it wrote [exit-code]\n"
                                 "\n";
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 0, "run with a failing optional participant");
    expect_text(run_part(run.standard_output), expected + "passed: 0 errors, 1 warning, 1 note\n",
                "run with a failing optional participant");
    require(contains(lf(run.standard_error), "\n[Logger] cannot open the log\n"), "the participant's line:\n" + run.standard_error);
    const Captured strict = launch(o, {"run", "--strict", "sim.yaml"}, dir);
    expect_code(strict, 1, "run --strict with a failing optional participant");
    expect_text(run_part(strict.standard_output), expected + "failed: 0 errors, 1 warning, 1 note (--strict)\n",
                "run --strict with a failing optional participant");
}

void not_joined(const Options& o)
{
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through (the participants are ended at once)");
    }
    // 1.5 s for Engine to join is ample; Ghost never joins.
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 2\n"
                                         "# run-option --stop-at-ms 1000\n"
                                         "# run-option --participant-start-timeout-ms 1500\n"
                                         "networks: []\n"
                                         "participants:\n"
                                         "  - name: Engine\n"
                                         "    command: [" + ecu(o) + "]\n"
                                         "  - name: Ghost\n"
                                         "    command: [" + helper(o) + ", --on-stop, \"9\", --pid-file, ghost.pid, --sleep-ms, \"600000\", --uri, \"{registry_uri}\"]\n"
                                         "    describe: false\n",
                                      {{"Engine", "print waiting-for-the-start"}});
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 1, "run with a participant that never joins");
    expect_text(run_part(run.standard_output),
                "run: the simulation did not start\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       stopped (" + kStop + "), exit code 130\n"
                "Ghost        yes       stopped (" + kStop + "), exit code 9\n"
                "\n"
                "error: sim.yaml:8: participant 'Ghost': it did not join the simulation within 1500 ms "
                "(start_timeout_ms), although its process was running: it must connect to the registry given with "
                "{registry_uri} as the participant 'Ghost' with a coordinated lifecycle (or set required: false); if it "
                "is only slow, raise start_timeout_ms [not-joined]\n"
                "\n"
                "failed: 1 error, 0 warnings, 1 note\n",
                "run with a participant that never joins");
    // The ECU waits longer than the launcher (start_timeout_ms + stop_timeout_ms + 10 s), so that the
    // launcher, not the ECU's own limit, ends the start and names who did not join.
    require(contains(lf(run.standard_error), " --start-timeout-ms 21500\n"), "the ECU's limit:\n" + run.standard_error);
    // What the ECU wrote to stdout before it was stopped (without a flush) is not lost.
    require(contains(lf(run.standard_error), "\n[Engine] waiting-for-the-start\n"),
            "the ECU's unflushed line:\n" + run.standard_error);
    // ecu_main ends at once on a stop request before the start, with its own message.
    require(contains(lf(run.standard_error), "[Engine] error: Engine: interrupted (" + kStop + ") before the simulation started"),
            "the ECU's message:\n" + run.standard_error);
    require(all_gone(dir, {"ghost.pid"}, 10s), "Ghost has ended");
}

void quitter(const Options& o)
{
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through (the participants are ended at once)");
    }
    // The start would wait 10 minutes: the launcher must not wait for it once Quitter has ended.
    // Engine is only the other side: a helper that sleeps and ends with 130 on a stop request. It does not
    // join the simulation, so no wait inside SIL Kit can keep the request from reaching it (under the thread
    // sanitizer a signal is held back while a thread sleeps in such a wait); that ecu_main ends at once on a
    // stop request before the start is checked by not_joined.
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 2\n"
                                         "# run-option --stop-at-ms 1000\n"
                                         "# run-option --participant-start-timeout-ms 600000\n"
                                         "networks: []\n"
                                         "participants:\n"
                                         "  - name: Engine\n"
                                         "    command: [" + helper(o) + ", --sleep-ms, \"600000\", --on-stop, \"130\", --uri, \"{registry_uri}\"]\n"
                                         "    describe: false\n"
                                         "  - name: Quitter\n"
                                         "    command: [" + helper(o) + ", --err, \"no configuration\\n\", --exit, \"2\", --uri, \"{registry_uri}\"]\n"
                                         "    describe: false\n");
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 1, "run with a participant that ends before the start");
    // Engine is told to stop right after it was started. On a slow machine that is before it installs its handler,
    // and it ends by the signal itself (on Windows, by Ctrl+Break with the code 0xC000013A). Both endings are right:
    // this scenario is about Quitter.
    std::string shown = run_part(run.standard_output);
    for (const std::string& by_signal : {"stopped (" + kStop + "), signal " + kStop + "\n",
                                         "stopped (" + kStop + "), exit code 0xC000013A (Windows ended it on a Ctrl+C, a "
                                         "Ctrl+Break or a closed console)\n"})
    {
        if (const auto at = shown.find(by_signal); at != std::string::npos)
        {
            shown.replace(at, by_signal.size(), "stopped (" + kStop + "), exit code 130\n");
        }
    }
    try
    {
        expect_text(shown,
                    "run: the simulation did not start\n"
                    "\n"
                    "participant  required  ended\n"
                    "Engine       yes       stopped (" + kStop + "), exit code 130\n"
                    "Quitter      yes       exit code 2\n"
                    "\n"
                    "error: sim.yaml:9: participant 'Quitter': it ended before the simulation started: its process exited "
                    "with code 2, so the simulation could not start; the lines [Quitter] above show what it wrote [ended-before-start]\n"
                    "\n"
                    "failed: 1 error, 0 warnings, 2 notes\n",
                    "run with a participant that ends before the start");
    }
    catch (const std::runtime_error& e)
    {
        // Show what the participants wrote: it tells how Engine ended.
        throw std::runtime_error(std::string(e.what()) + "\n--- stderr ---\n" + run.standard_error);
    }
}

void hang(const Options& o)
{
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through (the participants are ended at once)");
    }
    // Both hang after the simulation for ever: the time is not a guess about how fast they get
    // there. After the stop request, Stuck has to leave ecu_main and exit with 9 before the
    // second period ends (else it is killed too): scaled, as that takes longer on a slow machine.
    const std::string stop_ms = std::to_string(scaled_timeout(300ms).count());
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000\n"
                                      "# run-option --participant-stop-timeout-ms " + stop_ms + "\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Stuck\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Stuck", "pid hang-after-end stop-exit 9"}, {"Deaf", "pid hang-after-end ignore-stop"}});
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 1, "run with ECUs that do not end");
    expect_text(run_part(run.standard_output),
                "run: the simulation ran to stop_at_ms (1000 ms) and ended\n"
                "\n"
                "participant  required  ended\n"
                "Stuck        yes       stopped (" + kStop + "), exit code 9\n"
                "Deaf         yes       killed\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "error: sim.yaml:6: participant 'Stuck': it was still running " + stop_ms + " ms (stop_timeout_ms) after the "
                "simulation ended: the launcher asked it to stop (" + kStop + "), and its process exited with code 9; a "
                "participant should end when the simulation ends (the lines [Stuck] above show what it wrote) [did-not-end]\n"
                "error: sim.yaml:8: participant 'Deaf': it was still running " + stop_ms + " ms (stop_timeout_ms) after the "
                "simulation ended: the launcher asked it to stop (" + kStop + "), and as it did not end within " + stop_ms + " ms "
                "either, ended it with the processes it started; a participant should end when the simulation ends (the "
                "lines [Deaf] above show what it wrote) [did-not-end]\n"
                "\n"
                "failed: 2 errors, 0 warnings, 0 notes\n",
                "run with ECUs that do not end");
    require(all_gone(dir, {"Stuck.pid", "Deaf.pid"}, 10s), "both have ended");
}

void grandchild(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "grandchild " + shown_path(o.helper)}});
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    const long sleeper = read_pid((dir / "Engine-grandchild.pid").string(), 10s);
    const bool gone = sleeper > 0 && wait_gone(sleeper, 10s);
    if (sleeper > 0 && !gone)
    {
        kill_one(sleeper);
    }
    expect_code(run, 0, "run with an ECU that leaves a process");
    expect_text(run_part(run.standard_output),
                "run: the simulation ran to stop_at_ms (1000 ms) and ended\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       exit code 0\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "warning: sim.yaml:5: participant 'Engine': processes it started were still running after it had ended: "
                "the launcher ended them; end them in the participant before it exits [left-processes]\n"
                "\n"
                "passed: 0 errors, 1 warning, 0 notes\n",
                "run with an ECU that leaves a process");
    const std::string shown = "\n--- output ---\n" + run.standard_output + "--- errors ---\n" + run.standard_error;
    require(sleeper > 0, "the ECU started its process (Engine-grandchild.pid)" + shown);
    require(gone, "the process the ECU left has ended (" + std::to_string(sleeper) + ")" + shown);
}

void path(const Options& o)
{
    // A bare name runs from PATH (here: only the helper's directory); the participants run in the
    // directory of the configuration file.
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 2\n"
                                         "# run-option --stop-at-ms 1000\n"
                                         "networks: []\n"
                                         "participants:\n"
                                         "  - name: Engine\n"
                                         "    command: [" + ecu(o) + "]\n"
                                         "  - name: Where\n"
                                         "    command: [" + o.helper.stem().string() + ", --pwd, --uri, \"{registry_uri}\"]\n"
                                         "    describe: false\n"
                                         "    required: false\n");
    const fs::path elsewhere = o.work_dir / "elsewhere";
    fs::create_directories(elsewhere);
    // In front of the PATH there is (Windows finds the launcher's DLLs on it).
    const char* old_path = std::getenv("PATH");
#ifdef _WIN32
    const std::string path = o.helper.parent_path().string() + ";" + (old_path != nullptr ? old_path : "");
    _putenv_s("PATH", path.c_str());
#else
    const std::string path = o.helper.parent_path().string() + ":" + (old_path != nullptr ? old_path : "");
    ::setenv("PATH", path.c_str(), 1);
#endif
    const Captured run = launch(o, {"run", shown_path(dir / "sim.yaml")}, elsewhere);
    expect_code(run, 0, "run with a participant on PATH");
    const std::string out = run.standard_output;
    require(contains(out, "note: " + shown_path(dir / "sim.yaml") + ":8: participant 'Where': " + o.helper.stem().string() +
                              " is not in the directory of the configuration file; it runs " + shown_path(o.helper) +
                              " (found on PATH) [found-on-path]\n"),
            "the note names where it runs from:\n" + out);
    require(contains(out, "Where        no        exit code 0\n"), "it ran:\n" + out);
    const std::string err = lf(run.standard_error);
    // check runs --describe in the directory of the configuration file too: the helper writes a
    // description from a file there (a relative path).
    write_file(dir / "check.yaml", "vehicle_sil_simulation: 2\n"
                                   "networks: []\n"
                                   "participants:\n"
                                   "  - name: Described\n"
                                   "    command: [" + helper(o) + ", --out-file, described.json]\n");
    write_file(dir / "described.json", "{\"vehicle_sil_ecu\": 1, \"vehicle_sil\": \"x\", \"name\": \"Described\", "
                                       "\"step_ns\": 10000000, \"networks\": [], \"variables\": []}\n");
    const Captured checked = launch(o, {"check", shown_path(dir / "check.yaml")}, elsewhere);
    expect_code(checked, 0, "check with --describe in the directory of the file");
    const auto at = err.find("[Where] ");
    require(at != std::string::npos, "its line:\n" + err);
    const std::string where = err.substr(at + 8, err.find('\n', at) - at - 8);
    require(fs::equivalent(fs::path(where), dir), "it ran in " + where + ", not in the directory of the configuration file");
}

void interrupt(const Options& o)
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
    // No stop_at_ms: the simulation runs until it is interrupted.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "pid"}, {"Brake", "pid"}});
    const auto [status, out] = run_until_running(o, dir, [](vehicle_sil::process::Process& launcher) {
#ifdef _WIN32
        require(launcher.request_stop(), "Ctrl+Break could not be sent");
#else
        ::kill(static_cast<pid_t>(launcher.id()), SIGINT);
#endif
    });
    const bool gone = all_gone(dir, {"Engine.pid", "Brake.pid"}, 30s);
    require(status.has_value() && status->code == 130 && !status->signal,
            "the launcher ends with 130 when interrupted, not " + (status ? std::to_string(status->code) : std::string("running")));
    expect_text(run_part(out),
                "run: interrupted (" + by + "): the simulation was stopped\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       exit code 0\n"
                "Brake        yes       exit code 0\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "note: sim.yaml: interrupted (" + by + "): the launcher stopped the simulation and the participants [interrupted]\n"
                "\n"
                "interrupted: 0 errors, 0 warnings, 1 note\n",
                "run interrupted");
    require(gone, "no participant is left");
}

void interrupt_twice(const Options& o)
{
#ifdef _WIN32
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through");
    }
#endif
    // Deaf does not end after the simulation and ignores the stop request; stop_timeout_ms would
    // wait 10 minutes twice. A second interrupt ends the waiting.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --participant-stop-timeout-ms 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Deaf", "pid hang-after-end ignore-stop"}});
    std::mutex mutex;
    std::condition_variable changed;
    std::string out;
    std::string err;
    vehicle_sil::process::ProcessOptions options;
    options.working_directory = dir;
    options.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
        const std::lock_guard lock(mutex);
        (stream == vehicle_sil::process::Stream::Output ? out : err) += std::string(line) + "\n";
        changed.notify_all();
    };
    vehicle_sil::process::Process launcher(o.launch, fixture_run_arguments(dir, {"run", "sim.yaml"}), std::move(options));
    const auto wait_for_line = [&](const std::string& line) {
        std::unique_lock lock(mutex);
        // Only a last line against a hang.
        require(changed.wait_for(lock, scaled_timeout(120s), [&] { return contains(err, line); }), "no line " + line + " in:\n" + err);
    };
    const auto interrupt_it = [&] {
#ifdef _WIN32
        require(launcher.request_stop(), "Ctrl+Break could not be sent");
#else
        ::kill(static_cast<pid_t>(launcher.id()), SIGINT);
#endif
    };
    wait_for_line("[vehicle_sil_launch] simulation running\n");
    interrupt_it();
    wait_for_line("): the simulation was stopped\n");
    interrupt_it();
    const auto status = launcher.wait_for(scaled_timeout(120s));
    launcher.finish_output(10s);
    const bool gone = all_gone(dir, {"Deaf.pid"}, 30s);
    require(status.has_value() && status->code == 130, "the launcher ends with 130 after the second interrupt");
    const std::lock_guard lock(mutex);
    require(contains(out, "Deaf         yes       killed\n"), "Deaf was killed at once:\n" + out);
    require(gone, "Deaf has ended");
}

/// Asks the process `pid` to stop as the launcher would (POSIX SIGTERM; Windows Ctrl+Break to its
/// process group: the launcher starts each participant as the leader of one).
void ask_to_stop(long pid)
{
#ifdef _WIN32
    require(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(pid)) != 0, "Ctrl+Break could not be sent");
#else
    ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
}

void ended_early(const Options& o)
{
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through");
    }
    // stop_at_ms is far away; Engine stops the simulation (a stop request makes ecu_main stop it).
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000000000000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "pid"}});
    const auto [status, out] = run_until_running(o, dir, [&dir](vehicle_sil::process::Process&) {
        ask_to_stop(read_pid((dir / "Engine.pid").string(), 60s));
    });
    require(status.has_value() && status->code == 1, "the launcher ends with 1 (Engine's exit code)");
    expect_text(run_part(out),
                "run: the simulation ran until a participant ended it\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       exit code 130\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "error: sim.yaml:5: participant 'Engine': its process exited with code 130; the lines [Engine] above show "
                "what it wrote [exit-code]\n"
                "note: sim.yaml: the simulation ended before stop_at_ms (1000000000000 ms): a participant stopped it; if "
                "that is not expected, the lines above show which one [ended-before-stop-at]\n"
                "\n"
                "failed: 1 error, 0 warnings, 1 note\n",
                "run that a participant ended early");
}

void interrupt_ending(const Options& o)
{
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through");
    }
#ifdef _WIN32
    const std::string by = "Ctrl+Break";
#else
    const std::string by = "SIGINT";
#endif
    // Stuck does not end after the simulation; the launcher would wait 10 minutes for it.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000\n"
                                      "# run-option --participant-stop-timeout-ms 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Stuck\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Stuck", "pid hang-after-end stop-exit 9"}});
    std::mutex mutex;
    std::condition_variable changed;
    std::string out;
    std::string err;
    vehicle_sil::process::ProcessOptions options;
    options.working_directory = dir;
    options.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
        const std::lock_guard lock(mutex);
        (stream == vehicle_sil::process::Stream::Output ? out : err) += std::string(line) + "\n";
        changed.notify_all();
    };
    vehicle_sil::process::Process launcher(o.launch, fixture_run_arguments(dir, {"run", "sim.yaml"}), std::move(options));
    {
        std::unique_lock lock(mutex);
        // Only a last line against a hang.
        require(changed.wait_for(lock, scaled_timeout(120s), [&] { return contains(err, "[vehicle_sil_launch] the simulation ended\n"); }),
                "the simulation did not end:\n" + err);
    }
    wait_hanging(dir, "Stuck");
#ifdef _WIN32
    require(launcher.request_stop(), "Ctrl+Break could not be sent");
#else
    ::kill(static_cast<pid_t>(launcher.id()), SIGINT);
#endif
    const auto status = launcher.wait_for(scaled_timeout(120s));
    launcher.finish_output(10s);
    const bool gone = all_gone(dir, {"Stuck.pid"}, 30s);
    require(status.has_value() && status->code == 130, "the launcher ends with 130 when interrupted while it waits");
    const std::lock_guard lock(mutex);
    expect_text(run_part(out),
                "run: the simulation ran to stop_at_ms (1000 ms) and ended\n"
                "\n"
                "participant  required  ended\n"
                "Stuck        yes       stopped (" + kStop + "), exit code 9\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "note: sim.yaml: interrupted (" + by + "): the launcher stopped the participants instead of waiting for "
                "them to end [interrupted]\n"
                "\n"
                "interrupted: 0 errors, 0 warnings, 1 note\n",
                "run interrupted while it waits for the participants");
    require(gone, "Stuck has ended");
}

void timeout(const Options& o)
{
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through");
    }
    // Hang never returns from its step at 200 ms: virtual time stops for everyone, and only the
    // wall-clock limit ends the run.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000\n"
                                      "# run-option --participant-stop-timeout-ms 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Hang\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Hang", "pid hang-at-ms 200"}});
    const Captured run = launch(o, {"run", "--timeout-ms", "1500", "sim.yaml"}, dir);
    expect_code(run, 1, "run with an ECU that hangs in a step");
    expect_text(run_part(run.standard_output),
                "run: the simulation was aborted: it did not end within --timeout-ms (1500 ms)\n"
                "\n"
                "participant  required  ended\n"
                "Hang         yes       killed\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "error: sim.yaml: the simulation did not end within 1500 ms of wall-clock time (--timeout-ms), so the "
                "launcher aborted it: a participant may hang in a step (the lines above show what the participants "
                "wrote); if it is only slow, raise --timeout-ms [timeout]\n"
                "error: sim.yaml:6: participant 'Hang': it was still running 300 ms (stop_timeout_ms) after the "
                "simulation ended: the launcher asked it to stop (" + kStop + "), and as it did not end within 300 ms "
                "either, ended it with the processes it started; a participant should end when the simulation ends (the "
                "lines [Hang] above show what it wrote) [did-not-end]\n"
                "\n"
                "failed: 2 errors, 0 warnings, 0 notes\n",
                "run with an ECU that hangs in a step");
    require(all_gone(dir, {"Hang.pid"}, 10s), "Hang has ended");
}

void no_console(const Options& o)
{
#ifdef _WIN32
    // The launcher without a console (as from a service): it cannot send Ctrl+Break, so it ends a
    // participant that does not end at once, and says so.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 1000\n"
                                      "# run-option --participant-stop-timeout-ms 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Deaf", "pid hang-after-end ignore-stop"}});
    vehicle_sil::process::CaptureOptions options;
    options.timeout = scaled_timeout(240s);
    options.working_directory = dir;
    const Captured run = vehicle_sil::process::run_captured(
        o.helper, {"--run-detached", shown_path(o.launch), "run", "sim.yaml", "--stop-at-ms", "1000",
                   "--participant-stop-timeout-ms", "300"}, options);
    expect_code(run, 1, "run without a console");
    expect_text(run_part(run.standard_output),
                "run: the simulation ran to stop_at_ms (1000 ms) and ended\n"
                "\n"
                "participant  required  ended\n"
                "Deaf         yes       killed\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "error: sim.yaml:6: participant 'Deaf': it was still running 300 ms (stop_timeout_ms) after the "
                "simulation ended: the launcher could not ask it to stop (no console to send Ctrl+Break through) and "
                "ended it with the processes it started; a participant should end when the simulation ends (the lines "
                "[Deaf] above show what it wrote) [did-not-end]\n"
                "\n"
                "failed: 1 error, 0 warnings, 0 notes\n",
                "run without a console");
    require(all_gone(dir, {"Deaf.pid"}, 10s), "Deaf has ended");
#else
    static_cast<void>(o);
    throw Skip("a launcher without a console is a Windows case");
#endif
}

void killed(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Logger\n"
                                      "    command: [" + helper(o) + ", --pid-file, logger.pid, --ignore-stop, --sleep-ms, \"600000\"]\n"
                                      "    describe: false\n"
                                      "    required: false\n",
                                      {{"Engine", "pid grandchild " + shown_path(o.helper)}});
    std::vector<long> pids;
    const auto result = run_until_running(o, dir, [&](vehicle_sil::process::Process& launcher) {
        for (const char* file : {"Engine.pid", "logger.pid", "Engine-grandchild.pid"})
        {
            pids.push_back(read_pid((dir / file).string(), 60s));
        }
        // The launcher alone, as a crash would end it (not its process group or job).
        kill_one(launcher.id());
    });
    require(result.first.has_value(), "the launcher has ended");
    for (const long pid : pids)
    {
        require(pid > 0, "every participant wrote its process ID");
    }
    // The participants themselves (Engine, Logger, which ignores a stop request) end on every system.
    const bool participantsGone = wait_gone(pids[0], 60s) && wait_gone(pids[1], 60s);
#ifdef _WIN32
    // Windows: the job objects end what the participants started too.
    const bool grandchildGone = wait_gone(pids[2], 60s);
#else
    // Linux: what a participant started itself is not ended (docs/simulation-run.md says so).
    const bool grandchildGone = true;
    log(process_alive(pids[2]) ? "the grandchild was left, as documented (ended by the test)" : "the grandchild ended too");
#endif
    for (const long pid : pids)
    {
        if (process_alive(pid))
        {
            kill_one(pid); // clean up after a failure (and the documented grandchild)
        }
    }
    require(participantsGone, "no participant is left after the launcher died");
    require(grandchildGone, "no process a participant started is left after the launcher died (the job objects end them)");
}

void usage(const Options& o)
{
    const fs::path dir = o.work_dir;
    const Captured none = launch(o, {"run"}, dir);
    expect_code(none, 4, "run without a file");
    require(contains(lf(none.standard_error), "error: run needs the configuration file: vehicle_sil_launch run <file>\n"),
            "the message:\n" + none.standard_error);
    const Captured json = launch(o, {"run", "--json", "--bad"}, dir);
    expect_code(json, 4, "run --json --bad");
    require(contains(json.standard_output, "\"vehicle_sil_run\": 2,\n") && contains(json.standard_output, "\"result\": \"usage\",\n") &&
                contains(json.standard_output, "\"message\": \"unknown option --bad\""),
            "the run's JSON for a usage error:\n" + json.standard_output);
    const Captured logDir = launch(o, {"check", "--log-dir", "x", "sim.yaml"}, dir);
    expect_code(logDir, 4, "check --log-dir");
    require(contains(lf(logDir.standard_error), "error: --log-dir is for run: check starts no participant\n"),
            "the message:\n" + logDir.standard_error);
    const Captured missing = launch(o, {"run", "--json", "missing.yaml"}, dir);
    expect_code(missing, 3, "run missing.yaml");
    require(contains(missing.standard_output, "\"result\": \"unreadable\",\n") &&
                contains(missing.standard_output, "\"simulation\": null,\n"),
            "the run's JSON for a file that cannot be read:\n" + missing.standard_output);
}

/// The ECUs' control agents: who is connected when the simulation starts, in the events, the
/// result and the findings.
void control(const Options& o)
{
    {
        // Engine and Brake offer variables; Tool declares none (it has ecu.connected); Quiet has some and does not offer them
        // (--no-control, which its --describe says too).
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 2\n"
                                          "# run-option --stop-at-ms 200\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Engine\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Brake\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Tool\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Quiet\n"
                                          "    command: [" + ecu(o) + ", --no-control]\n",
                                          {{"Engine", "variables"}, {"Brake", "variables"}, {"Quiet", "variables"}});
        const auto begin = std::chrono::steady_clock::now();
        const Captured run = launch(o, {"serve", "--once", "sim.yaml"}, dir);
        expect_code(run, 0, "run with control agents");
        // Nobody is waited for in vain: the hellos are there when the start looks for them.
        require(std::chrono::steady_clock::now() - begin < 60s, "the run with control agents took too long");
        const std::string out = lf(run.standard_output);
        // ecu.connected, the four variables and the three outputs that the ECU's run adds (peer.*).
        for (const std::string name : {"Engine", "Brake"})
        {
            require(contains(out, "\"name\":\"" + name + "\",\"state\":\"connected\",\"variables\":8}\n"),
                    name + " is connected with its variables:\n" + out);
        }
        // An ECU that declares no variable has ecu.connected (and the outputs peer.*).
        require(contains(out, "\"event\":\"control_link\"") &&
                    contains(out, "\"name\":\"Tool\",\"state\":\"connected\",\"variables\":4}\n"),
                "Tool is connected with ecu.connected:\n" + out);
        require(contains(out, "\"name\":\"Quiet\",\"state\":\"not_offered\"}\n"), "Quiet does not offer its variables:\n" + out);
        // After everyone joined, before the simulation runs.
        const auto first = out.find("{\"event\":\"control_link\"");
        const auto last = out.rfind("{\"event\":\"control_link\"");
        const auto joined = out.rfind("\"state\":\"joined\"", first);
        const auto running = out.find("\"phase\":\"running\"");
        require(first != std::string::npos && joined != std::string::npos && running != std::string::npos && joined < first &&
                    last < running && out.find("\"state\":\"joined\"", first) == std::string::npos,
                "the control events come between the last joined and running:\n" + out);
        require(contains(out, "\"log\":null,\"control_link\":{\"state\":\"connected\",\"variables\":8,\"version\":null,"
                              "\"late_messages\":0}},{\"name\":\"Brake\"") &&
                    contains(out, "\"log\":null,\"control_link\":{\"state\":\"not_offered\",\"variables\":null,\"version\":null,"
                                  "\"late_messages\":0}}],\"outside\":[],\"traffic\":{\"before_ms\":"),
                "the result has each participant's control link:\n" + out);
        // The check notes the variables that are not offered, and says what each executable offers.
        require(contains(out, "\"code\":\"control-link-not-offered\"") &&
                    contains(out, "participant 'Quiet': it describes variables, but does not offer them to the simulation") &&
                    contains(out, "\"control\":{\"transport\":\"silkit\",\"wire_version\":2,\"wire_revision\":1}") && contains(out, "\"control\":null") &&
                    !contains(out, "control-link-no-hello") && !contains(out, "control-link-version"),
                "the check notes what is not offered, and nothing else is found:\n" + out);
        log("CHECK control agents in the events and the result -> ok");
    }
    {
        // An ECU whose --describe offers its variables and whose run does not (it runs with
        // --no-control behind the launcher's back): the start waits for its hello in vain, and
        // says so.
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 2\n"
                                          "# run-option --stop-at-ms 200\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Engine\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Engine", "variables"}});
        write_file(dir / "test-ecu-no-control", "\n");
        const Captured run = launch(o, {"run", "sim.yaml", "--json"}, dir);
        fs::remove(dir / "test-ecu-no-control");
        expect_code(run, 0, "run with an ECU that does not say hello");
        const std::string out = lf(run.standard_output);
        require(contains(out, "        \"state\": \"no_hello\",\n") && contains(out, "\"code\": \"control-link-no-hello\"") &&
                    contains(out, "participant 'Engine': it offers its variables (--describe), but its control agent did not "
                                  "say hello within 5000 ms before the simulation started") &&
                    contains(out, "  \"warnings\": 1,\n") && contains(out, "  \"simulation\": \"reached_stop_at\",\n"),
                "the ECU without a hello is a warning, and the simulation runs:\n" + out);
        log("CHECK an ECU that does not say hello -> ok");
    }
}

void disabled(const Options& o)
{
    // Ghost's executable does not exist, and it is required: started or waited for, it would fail
    // the run.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 2\n"
                                      "# run-option --stop-at-ms 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "    enabled: false\n"
                                      "  - name: Ghost\n"
                                      "    command: [missing/ghost]\n"
                                      "    enabled: false\n",
                                      {{"Engine", "pid"}, {"Brake", "pid"}});
    const fs::path logs = o.work_dir / "disabled-logs";
    const Captured run = launch(o, {"run", "sim.yaml", "--log-dir", shown_path(logs)}, dir);
    expect_code(run, 0, "run with disabled participants");
    expect_text(run.standard_output,
                "sim.yaml: 3 participants (2 disabled), 0 networks\n"
                "\n"
                "participant  step   networks\n"
                "Engine       10 ms  -\n"
                "Brake        -      disabled\n"
                "Ghost        -      disabled\n"
                "\n"
                "note: sim.yaml:7: participant 'Brake': disabled (enabled: false, or --disable): it is neither checked nor "
                "started, and the other findings are about the simulation without it [participant-disabled]\n"
                "note: sim.yaml:10: participant 'Ghost': disabled (enabled: false, or --disable): it is neither checked nor "
                "started, and the other findings are about the simulation without it [participant-disabled]\n"
                "\n"
                "run: the simulation ran to stop_at_ms (300 ms) and ended\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       exit code 0\n"
                "Brake        -         not started (disabled)\n"
                "Ghost        -         not started (disabled)\n"
                "\n"
                "traffic: no network in use\n"
                "\n"
                "passed: 0 errors, 0 warnings, 2 notes\n",
                "run with disabled participants");
    const std::string err = lf(run.standard_error);
    require(contains(err, "\n[vehicle_sil_launch] not starting Brake: disabled\n") &&
                contains(err, "\n[vehicle_sil_launch] not starting Ghost: disabled\n"),
            "the launcher says whom it does not start:\n" + err);
    require(fs::exists(dir / "Engine.pid") && !fs::exists(dir / "Brake.pid"), "Engine ran, Brake did not");
    require(fs::exists(logs / "Engine.log") && !fs::exists(logs / "Brake.log") && !fs::exists(logs / "Ghost.log"),
            "no log for a participant that was not started");
    fs::remove(dir / "Engine.pid");

    // The command line swaps them: Brake runs, Engine does not.
    const Captured swapped = launch(o, {"run", "--json", "--disable", "Engine", "--enable", "Brake", "sim.yaml"}, dir);
    expect_code(swapped, 0, "run --disable Engine --enable Brake");
    const std::string json = swapped.standard_output;
    require(contains(json, "      \"name\": \"Engine\",\n      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n      \"for_network\": null,\n"
                           "      \"ended\": \"not_started\",\n      \"exit_code\": null,\n      \"signal\": null,\n"
                           "      \"left_processes\": false,\n      \"log\": null,\n      \"control_link\": {\n"
                           "        \"state\": \"none\",\n") &&
                contains(json, "      \"name\": \"Brake\",\n      \"required\": true,\n      \"enabled\": true,\n      \"manual\": false,\n      \"for_network\": null,\n"
                               "      \"ended\": \"exited\",\n      \"exit_code\": 0,\n") &&
                contains(json, "        \"enabled\": false,\n        \"manual\": false,\n        \"role\": \"ecu\",\n        \"step_ms\": null,\n        \"status\": \"disabled\",\n"),
            "the run's JSON for --disable Engine --enable Brake:\n" + json);
    require(fs::exists(dir / "Brake.pid") && !fs::exists(dir / "Engine.pid"), "Brake ran, Engine did not");

    // A name the file does not have: nothing is started.
    fs::remove(dir / "Brake.pid");
    const Captured typo = launch(o, {"run", "sim.yaml", "--enable", "Brak"}, dir);
    expect_code(typo, 4, "run --enable Brak");
    require(lf(typo.standard_error).rfind("error: --enable Brak: the file has no participant with this name (did you mean "
                                          "'Brake'?); its participants are Engine, Brake, Ghost\nusage: ", 0) == 0 &&
                typo.standard_output.empty(),
            "run --enable Brak wrote:\n" + typo.standard_output + typo.standard_error);
    // The options may come before the command.
    const Captured typoJson = launch(o, {"--json", "--disable", "Engine", "run", "sim.yaml", "--enable", "Engine"}, dir);
    expect_code(typoJson, 4, "run --json --disable Engine --enable Engine");
    require(contains(typoJson.standard_output, "\"vehicle_sil_run\": 2,\n") &&
                contains(typoJson.standard_output, "\"result\": \"usage\",\n") &&
                contains(typoJson.standard_output, "\"message\": \"--disable and --enable both name Engine: give one of them\""),
            "the run's JSON for --disable and --enable of one name:\n" + typoJson.standard_output);
    require(!fs::exists(dir / "Engine.pid") && !fs::exists(dir / "Brake.pid"), "nothing was started after a usage error");

    // A run that fails with a disabled participant: it stays not started, with no finding.
    const fs::path failing = scenario_dir(o,
                                          "vehicle_sil_simulation: 2\n"
                                          "# run-option --stop-at-ms 1000\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Engine\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Brake\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "    enabled: false\n",
                                          {{"Engine", "exit-at-ms 200 7"}, {"Brake", "pid"}});
    const Captured crashed = launch(o, {"run", "--json", "sim.yaml"}, failing);
    expect_code(crashed, 1, "run with a crashing ECU and a disabled one");
    require(contains(crashed.standard_output, "  \"result\": \"failed\",\n") &&
                contains(crashed.standard_output, "      \"name\": \"Brake\",\n      \"required\": true,\n      \"enabled\": "
                                                  "false,\n      \"manual\": false,\n      \"for_network\": null,\n      \"ended\": \"not_started\",\n      \"exit_code\": null,\n"
                                                  "      \"signal\": null,\n      \"left_processes\": false,\n") &&
                !contains(crashed.standard_output.substr(0, crashed.standard_output.find("  \"check\": {")),
                          "\"participant\": \"Brake\""),
            "the run's JSON with a crashing ECU and a disabled one:\n" + crashed.standard_output);
    require(!fs::exists(failing / "Brake.pid"), "Brake was not started");

    // --strict: leaving Dashboard out of the example gives a warning (one participant on
    // J1939_CAN), so nothing is started.
    const Captured strict = launch(o, {"run", "--strict", "simulation.yaml", "--stop-at-ms", "1000", "--disable", "Dashboard"}, o.example_dir);
    expect_code(strict, 1, "run --strict simulation.yaml --disable Dashboard");
    require(contains(strict.standard_output, "\nnothing was started: the check found warnings, and --strict fails on them") &&
                !contains(strict.standard_error, "[vehicle_sil_launch] registry"),
            "run --strict simulation.yaml --disable Dashboard wrote:\n" + strict.standard_output);
}
