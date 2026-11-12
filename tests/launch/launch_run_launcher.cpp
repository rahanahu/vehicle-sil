// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run as a process, with the ECU executables of examples/ecu_main, an
// ecu_main executable that misbehaves as a file in its working directory says (run_test_ecu.cpp)
// and the helper of tests/process:
//
//   docs         the run of the example and its output in docs/simulation-config.md (text and
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
//   events       run --events: the example of the docs byte for byte (after its placeholders), the
//                whole example's events (seq, order, the result as --json writes it), nothing on stderr
//   events_usage run --events with a wrong command line (also one the check finds) or a file that
//                cannot be read: which events come
//   events_findings a participant leaves the simulation and its process ends later: every finding
//                event is in the result, and none comes twice; the state "failed" comes at once
//   events_stalled a reader that does not read: the participants are not held up; a stop command, a
//                signal and --timeout-ms work all the same; the launcher gives the reader up after
//                its time limit
//   events_stderr the reader reads neither the events nor stderr (one pipe): the launcher ends
//   events_slow  a reader that falls behind and catches up: log lines are dropped and counted
//                (log_dropped), nothing else, and the result comes last
//   events_nonblocking (POSIX) the standard output is a pipe that does not block: nothing is lost
//   events_output (POSIX) the standard output is a file; there is none (exit 4)
//   stdin_stop   run --stdin-commands: a line that is no command, then {"command":"stop"}: stopped in
//                order (130); the end of input after it is no second stop
//   stdin_eof    without --stdin-commands a closed standard input is never read; with it, its end
//                stops the run in order
//   stdin_twice  a second stop command ends the waiting for a participant that does not end
//   stdin_ending a stop command while the launcher waits for the participants after the simulation
//   output_closed the reader of the events goes away (while the simulation runs, while the launcher
//                waits for the start, while it ends the participants): it stops in order, leaves nothing
//   stop_once    the end of input and a closed output after a stop are no second stop
//   stdin_killed the input closes and the launcher is killed at once: no participant is left
//   no_console_stdin (Windows) the launcher without a console is stopped in order by a command
#include "launcher_support.hpp"
#include "spawn_sleeper.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;
using vehicle_sil::process::Captured;
using vehicle_sil::process::shown_path;

namespace {

struct Options
{
    fs::path launch;
    fs::path test_ecu;
    fs::path helper;
    fs::path example_dir; ///< the example's directory in the build tree (simulation.yaml, engine/, dashboard/)
    fs::path docs;
    fs::path work_dir;
    std::string scenario;
};

Options parse(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options o;
    o.launch = args.required("launch");
    o.test_ecu = args.required("test-ecu");
    o.helper = args.required("helper");
    o.example_dir = args.required("example-dir");
    o.docs = args.required("docs");
    o.work_dir = args.required("work-dir");
    o.scenario = args.required("scenario");
    return o;
}

/// The scenario cannot run on this machine: main() exits with 77 (CTest's SKIP_RETURN_CODE).
struct Skip : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& what)
{
    if (!condition)
    {
        throw std::runtime_error(what);
    }
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string read_text(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string lf(std::string text)
{
    std::erase(text, '\r');
    return text;
}

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
    require(static_cast<bool>(out), "cannot write " + path.string());
}

/// A YAML scalar in single quotes (a path with '\' or spaces as it is).
std::string yaml_quote(const std::string& text)
{
    std::string out = "'";
    for (const char c : text)
    {
        out += c == '\'' ? std::string("''") : std::string(1, c);
    }
    return out + "'";
}

/// `text` with every value of a "vehicle_sil" key (a version) replaced by "<version>".
std::string without_versions(std::string text)
{
    const std::string key = "\"vehicle_sil\": \"";
    for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
    {
        const auto from = at + key.size();
        const auto to = text.find('"', from);
        require(to != std::string::npos, "a \"vehicle_sil\" value does not end");
        text.replace(from, to - from, "<version>");
    }
    return text;
}

/// The fenced block after the line `marker` in the Markdown file, without the fences.
std::string block_after(const fs::path& path, const std::string& marker)
{
    const std::string text = lf(read_text(path));
    const auto at = text.find(marker + "\n");
    require(at != std::string::npos, path.string() + " has no line " + marker);
    const auto open = text.find('\n', text.find("```", at)) + 1;
    const auto close = text.find("\n```", open);
    require(close != std::string::npos, path.string() + ": the block after " + marker + " does not end");
    return text.substr(open, close - open + 1);
}

void expect_text(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected)
    {
        throw std::runtime_error(what + " differs\n--- got ---\n" + actual + "--- expected ---\n" + expected + "---");
    }
    log("CHECK " + what + " -> ok");
}

/// Runs vehicle_sil_launch with `args` in `dir` to its end. The time limit is only a last line
/// against a hang.
Captured launch(const Options& o, const std::vector<std::string>& args, const fs::path& dir)
{
    vehicle_sil::process::CaptureOptions options;
    options.timeout = 240s;
    options.working_directory = dir;
    return vehicle_sil::process::run_captured(o.launch, args, options);
}

void expect_code(const Captured& run, int code, const std::string& what)
{
    require(run.exit_code.has_value(), what + ": vehicle_sil_launch did not end");
    require(*run.exit_code == code, what + ": exit code " + std::to_string(*run.exit_code) + ", not " +
                                        std::to_string(code) + "\n--- stdout ---\n" + run.standard_output +
                                        "--- stderr ---\n" + run.standard_error);
}

/// How the launcher asks a participant to stop, as the result names it.
const std::string kStop =
#ifdef _WIN32
    "Ctrl+Break";
#else
    "SIGTERM";
#endif

/// A stop request can be sent: always on POSIX; on Windows only with a console that the
/// processes share.
bool can_request_stop()
{
#ifdef _WIN32
    return GetConsoleWindow() != nullptr || GetConsoleCP() != 0;
#else
    return true;
#endif
}

/// The part of the run's text from "run:" on.
std::string run_part(const std::string& text)
{
    const auto at = text.find("run: ");
    require(at != std::string::npos, "the output has no run: line\n" + text);
    return text.substr(at);
}

/// A scenario's directory with sim.yaml and the behaviour files of the test ECU.
fs::path scenario_dir(const Options& o, const std::string& config, const std::vector<std::pair<std::string, std::string>>& behaviours = {})
{
    const fs::path dir = o.work_dir / "run";
    fs::create_directories(dir);
    write_file(dir / "sim.yaml", config);
    for (const auto& [name, behaviour] : behaviours)
    {
        write_file(dir / (name + ".behaviour"), behaviour + "\n");
    }
    return dir;
}

std::string ecu(const Options& o) { return yaml_quote(shown_path(o.test_ecu)); }
std::string helper(const Options& o) { return yaml_quote(shown_path(o.helper)); }

/// Ends the processes whose IDs are in `pid_files` if they still run (after a failed check), and
/// says whether every one had ended within `timeout`.
bool all_gone(const fs::path& dir, const std::vector<std::string>& pid_files, std::chrono::milliseconds timeout)
{
    bool gone = true;
    for (const auto& file : pid_files)
    {
        const long pid = read_pid((dir / file).string(), 0ms);
        if (pid > 0 && !wait_gone(pid, timeout))
        {
            log("process " + std::to_string(pid) + " (" + file + ") is still running: ending it");
            kill_one(pid);
            gone = false;
        }
    }
    return gone;
}

// --- scenarios ---

void docs(const Options& o)
{
    const fs::path dir = o.example_dir;
    const Captured text = launch(o, {"run", "simulation.yaml"}, dir);
    expect_code(text, 0, "run simulation.yaml");
    expect_text(text.standard_output, block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml -->"),
                "run simulation.yaml");
    const std::string err = lf(text.standard_error);
    require(contains(err, "\n[Dashboard] t=0 ms engine speed 0 rpm (0 messages)\n"),
            "the participants' lines on stderr after [<name>]:\n" + err);
    require(err.rfind("[vehicle_sil_launch] registry at silkit://127.0.0.1:", 0) == 0,
            "the first line on stderr is where the registry listens:\n" + err);
    require(contains(err, "\n[vehicle_sil_launch] simulation running\n"), "the start on stderr:\n" + err);

    const Captured without = launch(o, {"run", "simulation.yaml", "--disable", "Dashboard"}, dir);
    expect_code(without, 0, "run simulation.yaml --disable Dashboard");
    expect_text(without.standard_output,
                block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --disable Dashboard -->"),
                "run simulation.yaml --disable Dashboard");

    const Captured json = launch(o, {"run", "simulation.yaml", "--json"}, dir);
    expect_code(json, 0, "run simulation.yaml --json");
    // Byte for byte: "\n" also on Windows.
    expect_text(without_versions(json.standard_output),
                without_versions(block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --json -->")),
                "run simulation.yaml --json");

    const fs::path logs = o.work_dir / "logs";
    const Captured logged = launch(o, {"run", "--json", "--log-dir", shown_path(logs), "simulation.yaml"}, dir);
    expect_code(logged, 0, "run --log-dir");
    // The JSON names each participant's file.
    require(contains(logged.standard_output, "      \"log\": \"" + shown_path(logs / "Dashboard.log") + "\"\n"),
            "the JSON names Dashboard's log:\n" + logged.standard_output);
    const std::string dashboard = lf(read_text(logs / "Dashboard.log"));
    require(dashboard.rfind("t=0 ms engine speed 0 rpm (0 messages)\n", 0) == 0,
            "Dashboard.log has the lines without [Dashboard]:\n" + dashboard);
    require(fs::exists(logs / "Engine.log"), "--log-dir makes a file for every participant (Engine.log)");
}

void check(const Options& o)
{
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 1\n"
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
                "here: name, command, step_ms, describe, required, enabled)\n"
                "note: sim.yaml:4: participant 'Logger': not checked (describe: false): vehicle_sil_launch cannot "
                "compare its name, step and networks with this file\n"
                "note: sim.yaml:5: participant 'Logger': the command has no {registry_uri}, so the executable is not told "
                "where the simulation's registry is: if it needs it, add its option for the registry URI with "
                "\"{registry_uri}\" as the value\n"
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
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
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
                "error: sim.yaml:5: participant 'Engine': it failed while the simulation was running, and the launcher "
                "aborted the simulation: its process exited with code 7; the lines [Engine] above show what it wrote\n"
                "\n"
                "failed: 1 error, 0 warnings, 0 notes\n",
                "run with a crashing ECU");
    require(all_gone(dir, {"Engine.pid", "Brake.pid"}, 10s), "every participant has ended");
}

void step(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
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
                "error: sim.yaml:5: participant 'Engine': it failed while the simulation was running, and the launcher "
                "aborted the simulation: its process exited with code 1; the lines [Engine] above show what it wrote\n"
                "\n"
                "failed: 1 error, 0 warnings, 0 notes\n",
                "run with an ECU whose step fails");
    require(contains(lf(run.standard_error), "[Engine] error: Engine: "), "the ECU's own message after [Engine]:\n" + run.standard_error);
}

void optional(const Options& o)
{
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 1\n"
                                         "stop_at_ms: 1000\n"
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
                                 "warning: sim.yaml:7: participant 'Logger': its process exited with code 5; the lines "
                                 "[Logger] above show what it wrote\n"
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
    // 8 s for Engine to join is generous; Ghost never joins.
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 1\n"
                                         "stop_at_ms: 1000\n"
                                         "start_timeout_ms: 8000\n"
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
                "error: sim.yaml:8: participant 'Ghost': it did not join the simulation within 8000 ms "
                "(start_timeout_ms), although its process was running: it must connect to the registry given with "
                "{registry_uri} as the participant 'Ghost' with a coordinated lifecycle (or set required: false); if it "
                "is only slow, raise start_timeout_ms\n"
                "\n"
                "failed: 1 error, 0 warnings, 1 note\n",
                "run with a participant that never joins");
    // The ECU waits longer than the launcher (start_timeout_ms + stop_timeout_ms + 10 s), so that the
    // launcher, not the ECU's own limit, ends the start and names who did not join.
    require(contains(lf(run.standard_error), " --start-timeout-ms 28000\n"), "the ECU's limit:\n" + run.standard_error);
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
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 1\n"
                                         "stop_at_ms: 1000\n"
                                         "start_timeout_ms: 600000\n"
                                         "networks: []\n"
                                         "participants:\n"
                                         "  - name: Engine\n"
                                         "    command: [" + ecu(o) + "]\n"
                                         "  - name: Quitter\n"
                                         "    command: [" + helper(o) + ", --err, \"no configuration\\n\", --exit, \"2\", --uri, \"{registry_uri}\"]\n"
                                         "    describe: false\n");
    const Captured run = launch(o, {"run", "sim.yaml"}, dir);
    expect_code(run, 1, "run with a participant that ends before the start");
    expect_text(run_part(run.standard_output),
                "run: the simulation did not start\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       stopped (" + kStop + "), exit code 130\n"
                "Quitter      yes       exit code 2\n"
                "\n"
                "error: sim.yaml:8: participant 'Quitter': it ended before the simulation started: its process exited "
                "with code 2, so the simulation could not start; the lines [Quitter] above show what it wrote\n"
                "\n"
                "failed: 1 error, 0 warnings, 1 note\n",
                "run with a participant that ends before the start");
}

void hang(const Options& o)
{
    if (!can_request_stop())
    {
        throw Skip("no console to send Ctrl+Break through (the participants are ended at once)");
    }
    // Both hang after the simulation for ever: 300 ms is not a guess about their speed.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "stop_timeout_ms: 300\n"
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
                "error: sim.yaml:6: participant 'Stuck': it was still running 300 ms (stop_timeout_ms) after the "
                "simulation ended: the launcher asked it to stop (" + kStop + "), and its process exited with code 9; a "
                "participant should end when the simulation ends (the lines [Stuck] above show what it wrote)\n"
                "error: sim.yaml:8: participant 'Deaf': it was still running 300 ms (stop_timeout_ms) after the "
                "simulation ended: the launcher asked it to stop (" + kStop + "), and as it did not end within 300 ms "
                "either, ended it with the processes it started; a participant should end when the simulation ends (the "
                "lines [Deaf] above show what it wrote)\n"
                "\n"
                "failed: 2 errors, 0 warnings, 0 notes\n",
                "run with ECUs that do not end");
    require(all_gone(dir, {"Stuck.pid", "Deaf.pid"}, 10s), "both have ended");
}

void grandchild(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
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
                "warning: sim.yaml:5: participant 'Engine': processes it started were still running after it had ended: "
                "the launcher ended them; end them in the participant before it exits\n"
                "\n"
                "passed: 0 errors, 1 warning, 0 notes\n",
                "run with an ECU that leaves a process");
    require(sleeper > 0, "the ECU started its process (Engine-grandchild.pid)");
    require(gone, "the process the ECU left has ended");
}

void path(const Options& o)
{
    // A bare name runs from PATH (here: only the helper's directory); the participants run in the
    // directory of the configuration file.
    const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 1\n"
                                         "stop_at_ms: 1000\n"
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
                              " (found on PATH)\n"),
            "the note names where it runs from:\n" + out);
    require(contains(out, "Where        no        exit code 0\n"), "it ran:\n" + out);
    const std::string err = lf(run.standard_error);
    // check runs --describe in the directory of the configuration file too: the helper writes a
    // description from a file there (a relative path).
    write_file(dir / "check.yaml", "vehicle_sil_simulation: 1\n"
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

/// Runs the launcher with `args` in `dir`, waits for "simulation running" on its stderr, then
/// calls `act` with it.
template <class Act>
std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::string> run_until_running(const Options& o, const fs::path& dir,
                                                                                         Act act)
{
    std::mutex mutex;
    std::condition_variable changed;
    std::string out;
    std::string err;
    bool running = false;
    vehicle_sil::process::ProcessOptions options;
    options.working_directory = dir;
    options.on_line = [&](vehicle_sil::process::Stream stream, std::string_view line) {
        const std::lock_guard lock(mutex);
        (stream == vehicle_sil::process::Stream::Output ? out : err) += std::string(line) + "\n";
        running = running || line == "[vehicle_sil_launch] simulation running";
        changed.notify_all();
    };
    vehicle_sil::process::Process launcher(o.launch, {"run", "sim.yaml"}, std::move(options));
    {
        std::unique_lock lock(mutex);
        // Only a last line against a hang: the start takes a second or two.
        require(changed.wait_for(lock, 120s, [&] { return running || launcher.poll().has_value(); }) && running,
                "the simulation did not start:\n" + err);
    }
    act(launcher);
    const auto status = launcher.wait_for(120s);
    launcher.finish_output(10s);
    const std::lock_guard lock(mutex);
    log("stderr of the launcher:\n" + err);
    return {status, out};
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
                                      "vehicle_sil_simulation: 1\n"
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
                "note: sim.yaml: interrupted (" + by + "): the launcher stopped the simulation and the participants\n"
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
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_timeout_ms: 600000\n"
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
    vehicle_sil::process::Process launcher(o.launch, {"run", "sim.yaml"}, std::move(options));
    const auto wait_for_line = [&](const std::string& line) {
        std::unique_lock lock(mutex);
        // Only a last line against a hang.
        require(changed.wait_for(lock, 120s, [&] { return contains(err, line); }), "no line " + line + " in:\n" + err);
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
    const auto status = launcher.wait_for(120s);
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
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000000000000\n"
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
                "error: sim.yaml:5: participant 'Engine': its process exited with code 130; the lines [Engine] above show "
                "what it wrote\n"
                "note: sim.yaml: the simulation ended before stop_at_ms (1000000000000 ms): a participant stopped it; if "
                "that is not expected, the lines above show which one\n"
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
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "stop_timeout_ms: 600000\n"
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
    vehicle_sil::process::Process launcher(o.launch, {"run", "sim.yaml"}, std::move(options));
    {
        std::unique_lock lock(mutex);
        // Only a last line against a hang.
        require(changed.wait_for(lock, 120s, [&] { return contains(err, "[vehicle_sil_launch] the simulation ended\n"); }),
                "the simulation did not end:\n" + err);
    }
#ifdef _WIN32
    require(launcher.request_stop(), "Ctrl+Break could not be sent");
#else
    ::kill(static_cast<pid_t>(launcher.id()), SIGINT);
#endif
    const auto status = launcher.wait_for(120s);
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
                "note: sim.yaml: interrupted (" + by + "): the launcher stopped the participants instead of waiting for "
                "them to end\n"
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
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "stop_timeout_ms: 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Hang\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Hang", "pid hang-at-ms 200"}});
    const Captured run = launch(o, {"run", "--timeout-ms", "5000", "sim.yaml"}, dir);
    expect_code(run, 1, "run with an ECU that hangs in a step");
    expect_text(run_part(run.standard_output),
                "run: the simulation was aborted: it did not end within --timeout-ms (5000 ms)\n"
                "\n"
                "participant  required  ended\n"
                "Hang         yes       killed\n"
                "\n"
                "error: sim.yaml: the simulation did not end within 5000 ms of wall-clock time (--timeout-ms), so the "
                "launcher aborted it: a participant may hang in a step (the lines above show what the participants "
                "wrote); if it is only slow, raise --timeout-ms\n"
                "error: sim.yaml:6: participant 'Hang': it was still running 300 ms (stop_timeout_ms) after the "
                "simulation ended: the launcher asked it to stop (" + kStop + "), and as it did not end within 300 ms "
                "either, ended it with the processes it started; a participant should end when the simulation ends (the "
                "lines [Hang] above show what it wrote)\n"
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
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "stop_timeout_ms: 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Deaf", "pid hang-after-end ignore-stop"}});
    vehicle_sil::process::CaptureOptions options;
    options.timeout = 240s;
    options.working_directory = dir;
    const Captured run = vehicle_sil::process::run_captured(o.helper, {"--run-detached", shown_path(o.launch), "run", "sim.yaml"}, options);
    expect_code(run, 1, "run without a console");
    expect_text(run_part(run.standard_output),
                "run: the simulation ran to stop_at_ms (1000 ms) and ended\n"
                "\n"
                "participant  required  ended\n"
                "Deaf         yes       killed\n"
                "\n"
                "error: sim.yaml:6: participant 'Deaf': it was still running 300 ms (stop_timeout_ms) after the "
                "simulation ended: the launcher could not ask it to stop (no console to send Ctrl+Break through) and "
                "ended it with the processes it started; a participant should end when the simulation ends (the lines "
                "[Deaf] above show what it wrote)\n"
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
                                      "vehicle_sil_simulation: 1\n"
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
    // Linux: what a participant started itself is not ended (docs/simulation-config.md says so).
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
    require(contains(json.standard_output, "\"vehicle_sil_run\": 1,\n") && contains(json.standard_output, "\"result\": \"usage\",\n") &&
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

void disabled(const Options& o)
{
    // Ghost's executable does not exist, and it is required: started or waited for, it would fail
    // the run.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 300\n"
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
                "started, and the other findings are about the simulation without it\n"
                "note: sim.yaml:10: participant 'Ghost': disabled (enabled: false, or --disable): it is neither checked nor "
                "started, and the other findings are about the simulation without it\n"
                "\n"
                "run: the simulation ran to stop_at_ms (300 ms) and ended\n"
                "\n"
                "participant  required  ended\n"
                "Engine       yes       exit code 0\n"
                "Brake        -         not started (disabled)\n"
                "Ghost        -         not started (disabled)\n"
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
    require(contains(json, "      \"name\": \"Engine\",\n      \"required\": true,\n      \"enabled\": false,\n"
                           "      \"ended\": \"not_started\",\n      \"exit_code\": null,\n      \"signal\": null,\n"
                           "      \"left_processes\": false,\n      \"log\": null\n") &&
                contains(json, "      \"name\": \"Brake\",\n      \"required\": true,\n      \"enabled\": true,\n"
                               "      \"ended\": \"exited\",\n      \"exit_code\": 0,\n") &&
                contains(json, "        \"enabled\": false,\n        \"step_ms\": null,\n        \"status\": \"disabled\",\n"),
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
    require(contains(typoJson.standard_output, "\"vehicle_sil_run\": 1,\n") &&
                contains(typoJson.standard_output, "\"result\": \"usage\",\n") &&
                contains(typoJson.standard_output, "\"message\": \"--disable and --enable both name Engine: give one of them\""),
            "the run's JSON for --disable and --enable of one name:\n" + typoJson.standard_output);
    require(!fs::exists(dir / "Engine.pid") && !fs::exists(dir / "Brake.pid"), "nothing was started after a usage error");

    // A run that fails with a disabled participant: it stays not started, with no finding.
    const fs::path failing = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "stop_at_ms: 1000\n"
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
                                                  "false,\n      \"ended\": \"not_started\",\n      \"exit_code\": null,\n"
                                                  "      \"signal\": null,\n      \"left_processes\": false,\n") &&
                !contains(crashed.standard_output.substr(0, crashed.standard_output.find("  \"check\": {")),
                          "\"participant\": \"Brake\""),
            "the run's JSON with a crashing ECU and a disabled one:\n" + crashed.standard_output);
    require(!fs::exists(failing / "Brake.pid"), "Brake was not started");

    // --strict: leaving Dashboard out of the example gives a warning (one participant on
    // J1939_CAN), so nothing is started.
    const Captured strict = launch(o, {"run", "--strict", "simulation.yaml", "--disable", "Dashboard"}, o.example_dir);
    expect_code(strict, 1, "run --strict simulation.yaml --disable Dashboard");
    require(contains(strict.standard_output, "\nnothing was started: the check found warnings, and --strict fails on them") &&
                !contains(strict.standard_error, "[vehicle_sil_launch] registry"),
            "run --strict simulation.yaml --disable Dashboard wrote:\n" + strict.standard_output);
}

} // namespace

// --- run --events and --stdin-commands ---

/// The lines of `text` (each ends with "\n").
std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t from = 0;
    for (auto end = text.find('\n'); end != std::string::npos; end = text.find('\n', from))
    {
        out.push_back(text.substr(from, end - from));
        from = end + 1;
    }
    require(from == text.size(), "the last line has no line end:\n" + text.substr(from));
    return out;
}

/// The string value of `key` in an event line (the first), or "" if it has none.
std::string value_of(const std::string& line, const std::string& key)
{
    const std::string head = "\"" + key + "\":";
    const auto at = line.find(head);
    if (at == std::string::npos)
    {
        return {};
    }
    const auto from = at + head.size();
    if (line.compare(from, 1, "\"") == 0)
    {
        return line.substr(from + 1, line.find('"', from + 1) - from - 1);
    }
    return line.substr(from, line.find_first_of(",}", from) - from);
}

/// The members of an event line after event, seq and t_ms ("" if it has none), without the braces.
std::string members_of(const std::string& line)
{
    const auto at = line.find(",\"t_ms\":");
    const auto from = at == std::string::npos ? at : line.find_first_of(",}", at + 8);
    return from == std::string::npos || line[from] == '}' ? std::string{} : line.substr(from + 1, line.size() - from - 2);
}

/// Checks the frame of the event line `i` (an object that starts with event, seq and t_ms; seq
/// 0, 1, 2, ...; hello first and only first).
void check_frame(const std::string& line, std::size_t i)
{
    const std::string event = value_of(line, "event");
    const std::string head = "{\"event\":\"" + event + "\",\"seq\":" + std::to_string(i) + ",\"t_ms\":";
    require(line.rfind(head, 0) == 0 && line.back() == '}', "event " + std::to_string(i) + " is not framed:\n" + line);
    require((event == "hello") == (i == 0), "hello is the first event and only it:\n" + line);
}

/// Checks every event line (check_frame(); result last and once; the phases never go back; a
/// participant's lines come after its "started"; every finding is in the result as it was
/// written, and none twice) and returns the lines.
std::vector<std::string> check_events(const std::vector<std::string>& lines)
{
    require(!lines.empty(), "no events");
    static const std::vector<std::string> kPhases{"checking", "starting", "waiting", "running", "stopping", "ended"};
    std::size_t phase = 0;
    std::vector<std::string> started;
    std::vector<std::string> findings;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string& line = lines[i];
        check_frame(line, i);
        const std::string event = value_of(line, "event");
        require((event == "result") == (i + 1 == lines.size()), "result is the last event and only it:\n" + line);
        if (event == "phase")
        {
            const auto at = std::find(kPhases.begin(), kPhases.end(), value_of(line, "phase"));
            require(at != kPhases.end() && static_cast<std::size_t>(at - kPhases.begin()) >= phase,
                    "the phases go forward: event " + std::to_string(i) + "\n" + line);
            phase = static_cast<std::size_t>(at - kPhases.begin());
        }
        else if (event == "participant" && value_of(line, "state") == "started")
        {
            started.push_back(value_of(line, "name"));
        }
        else if (event == "log" && value_of(line, "source") != "vehicle_sil_launch")
        {
            require(std::find(started.begin(), started.end(), value_of(line, "source")) != started.end(),
                    "a participant's line comes after its started event: event " + std::to_string(i) + "\n" + line);
        }
        else if (event == "finding")
        {
            require(std::find(findings.begin(), findings.end(), members_of(line)) == findings.end(),
                    "a finding is written once:\n" + line);
            findings.push_back(members_of(line));
        }
    }
    for (const auto& finding : findings)
    {
        require(contains(lines.back(), "{" + finding + "}"),
                "a finding that was written as an event is in the result as it was written:\n" + finding + "\n" + lines.back());
    }
    return lines;
}

std::vector<std::string> check_events(const std::string& text) { return check_events(lines_of(text)); }

/// The events of a stream that was cut (the launcher gave its reader up): framed up to the cut,
/// without a result; the last line may be a part of one.
void check_cut_events(const std::vector<std::string>& lines)
{
    for (std::size_t i = 0; i + 1 < lines.size(); ++i)
    {
        check_frame(lines[i], i);
        require(value_of(lines[i], "event") != "result", "no result in a stream that was cut:\n" + lines[i]);
    }
}

/// `text` (event lines) without the heartbeat events, "seq" counted without them.
std::string without_heartbeats(const std::string& text)
{
    std::string out;
    std::size_t seq = 0;
    for (const std::string& line : lines_of(text))
    {
        if (value_of(line, "event") == "heartbeat")
        {
            continue;
        }
        const auto at = line.find(",\"seq\":") + 7;
        out += line.substr(0, at) + std::to_string(seq++) + line.substr(line.find(',', at)) + "\n";
    }
    return out;
}

/// The lines that contain all of `parts`.
std::vector<std::string> events_with(const std::vector<std::string>& lines, const std::vector<std::string>& parts)
{
    std::vector<std::string> out;
    for (const auto& line : lines)
    {
        if (std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(line, p); }))
        {
            out.push_back(line);
        }
    }
    return out;
}

/// `pretty` (the JSON of --json) without the line ends and spaces outside its strings.
std::string compact(const std::string& pretty)
{
    std::string out;
    bool inString = false;
    for (std::size_t i = 0; i < pretty.size(); ++i)
    {
        const char c = pretty[i];
        if (inString && c == '\\')
        {
            out += c;
            out += pretty[++i];
            continue;
        }
        if (c == '"')
        {
            inString = !inString;
        }
        if (inString || (c != ' ' && c != '\n'))
        {
            out += c;
        }
    }
    return out;
}

/// `text` with what differs from run to run as the docs show it: <t_ms>, <pid>, <port>, <dir>
/// (the launcher's working directory `dir` in each form it may be written), <version>; and the
/// ".exe" of the example's executables.
std::string with_placeholders(std::string text, const fs::path& dir)
{
    const auto replace_all = [&text](const std::string& from, const std::string& to) {
        for (auto at = text.find(from); !from.empty() && at != std::string::npos; at = text.find(from, at + to.size()))
        {
            text.replace(at, from.size(), to);
        }
    };
    std::error_code ec;
    for (const fs::path& form : {fs::canonical(dir, ec), fs::weakly_canonical(fs::absolute(dir), ec),
                                 fs::absolute(dir).lexically_normal()})
    {
        std::string shown = shown_path(form);
        while (!shown.empty() && shown.back() == '/')
        {
            shown.pop_back();
        }
        replace_all(shown + "/", "<dir>/");
    }
    replace_all("vehicle_sil_engine_ecu.exe", "vehicle_sil_engine_ecu");
    for (const std::string key : {"\"t_ms\":", "\"pid\":", "silkit://127.0.0.1:"})
    {
        const std::string placeholder = key == "\"t_ms\":" ? "<t_ms>" : key == "\"pid\":" ? "<pid>" : "<port>";
        for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + 1))
        {
            const auto from = at + key.size();
            auto to = from;
            while (to < text.size() && text[to] >= '0' && text[to] <= '9')
            {
                ++to;
            }
            text.replace(from, to - from, placeholder);
        }
    }
    const std::string version = "\"vehicle_sil\":\"";
    for (auto at = text.find(version); at != std::string::npos; at = text.find(version, at + 1))
    {
        const auto from = at + version.size();
        text.replace(from, text.find('"', from) - from, "<version>");
    }
    return text;
}

void events(const Options& o)
{
    const fs::path dir = o.example_dir;
    const Captured alone = launch(o, {"run", "simulation.yaml", "--events", "--disable", "Dashboard"}, dir);
    expect_code(alone, 0, "run simulation.yaml --events --disable Dashboard");
    static_cast<void>(check_events(alone.standard_output));
    // Without the heartbeats: how many there are depends on how long the run takes.
    expect_text(with_placeholders(without_heartbeats(alone.standard_output), dir),
                block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --events --disable Dashboard -->"),
                "run simulation.yaml --events --disable Dashboard");
    expect_text(alone.standard_error, "", "run --events: stderr");

    // Both ECUs: their lines come as events of their own, in no fixed order with the rest.
    const Captured both = launch(o, {"run", "--events", "simulation.yaml"}, dir);
    expect_code(both, 0, "run --events simulation.yaml");
    const std::vector<std::string> lines = check_events(both.standard_output);
    expect_text(both.standard_error, "", "run --events: stderr");
    require(!events_with(lines, {"\"event\":\"log\"", "\"source\":\"Dashboard\"",
                                 "\"line\":\"t=0 ms engine speed 0 rpm (0 messages)\""})
                 .empty(),
            "Dashboard's lines are log events:\n" + both.standard_output);
    for (const std::string name : {"Engine", "Dashboard"})
    {
        const std::string who = "\"name\":\"" + name + "\"";
        for (const std::string state : {"started", "joined", "ended"})
        {
            require(events_with(lines, {"\"event\":\"participant\"", who, "\"state\":\"" + state + "\""}).size() == 1,
                    name + " is " + state + " once:\n" + both.standard_output);
        }
    }
    // The virtual time when it changes: 0 at the start, stop_at_ms at the end.
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(times.size() == 2 && contains(times[0], "\"sim_ms\":0}") && contains(times[1], "\"sim_ms\":1000}"),
            "the virtual time at 0 and at stop_at_ms:\n" + both.standard_output);
    // The result is the JSON of --json.
    const Captured json = launch(o, {"run", "--json", "simulation.yaml"}, dir);
    expect_code(json, 0, "run --json simulation.yaml");
    const std::string& last = lines.back();
    expect_text(last.substr(last.find(",\"result\":") + 10), compact(json.standard_output.substr(0, json.standard_output.size() - 1)) + "}",
                "the result event as run --json");
}

fs::path two_ecus(const Options& o);

void events_usage(const Options& o)
{
    const fs::path dir = o.work_dir;
    // hello comes first whatever is wrong with the command line, the result last.
    for (const auto& [args, message] :
         std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"run", "--events", "--bad", "x.yaml"}, "unknown option --bad"},
             {{"run", "--events", "--json", "x.yaml"}, "--json and --events cannot be used together"},
             {{"run", "--events", "--events-stall-ms", "999", "x.yaml"}, "--events-stall-ms must be from 1000 to 86400000"},
             {{"run", "--events", "--events-stall-ms", "soon", "x.yaml"}, "invalid value for --events-stall-ms: soon"},
             {{"run", "--events"}, "run needs the configuration file"}})
    {
        const Captured run = launch(o, args, dir);
        expect_code(run, 4, args.back());
        const std::vector<std::string> lines = check_events(run.standard_output);
        require(lines.size() == 2 && contains(lines[1], "\"result\":\"usage\"") && contains(lines[1], message),
                "hello and the result of a usage error:\n" + run.standard_output);
        require(contains(lf(run.standard_error), "error: " + message), "the error on stderr:\n" + run.standard_error);
    }
    // The limit is for the events: without them it is a mistake (and no event is written).
    const Captured alone = launch(o, {"run", "--events-stall-ms", "5000", "x.yaml"}, dir);
    expect_code(alone, 4, "run --events-stall-ms without --events");
    require(alone.standard_output.empty() && contains(lf(alone.standard_error), "error: --events-stall-ms is for --events"),
            "the error on stderr:\n" + alone.standard_error);
    const Captured missing = launch(o, {"run", "--events", "missing.yaml"}, dir);
    expect_code(missing, 3, "run --events missing.yaml");
    const std::vector<std::string> lines = check_events(missing.standard_output);
    require(lines.size() == 5 && value_of(lines[0], "file") == "missing.yaml" && contains(lines[2], "\"event\":\"check\"") &&
                contains(lines[4], "\"result\":\"unreadable\""),
            "hello, checking, check, ended and the result of a file that cannot be read:\n" + missing.standard_output);
    expect_text(missing.standard_error, "", "run --events missing.yaml: stderr");

    // A wrong command line that only the check finds (a name the file does not have): the check
    // is an event, then the result "usage".
    const fs::path sim = two_ecus(o);
    const Captured name = launch(o, {"run", "--events", "--disable", "Nobody", "sim.yaml"}, sim);
    expect_code(name, 4, "run --events --disable Nobody");
    const std::vector<std::string> found = check_events(name.standard_output);
    require(found.size() == 5 && contains(found[1], "\"phase\":\"checking\"") && contains(found[2], "\"event\":\"check\"") &&
                contains(found[2], "\"result\":\"usage\"") && contains(found[3], "\"phase\":\"ended\"") &&
                contains(found[4], "\"event\":\"result\"") && contains(found[4], "\"result\":\"usage\"") &&
                contains(found[4], "Nobody"),
            "hello, checking, check, ended and the result of a usage error the check finds:\n" + name.standard_output);
}

/// vehicle_sil_launch run with a pipe as its standard input and its events read as they come.
class Live
{
public:
    /// `detached` (Windows): without a console, through the helper's --run-detached.
    Live(const Options& o, const fs::path& dir, const std::vector<std::string>& args, bool detached = false)
    {
        vehicle_sil::process::ProcessOptions options;
        options.working_directory = dir;
        options.input = true;
        options.on_line = [this](vehicle_sil::process::Stream stream, std::string_view line) {
            std::unique_lock lock(mutex_);
            if (stream == vehicle_sil::process::Stream::Output)
            {
                out_.emplace_back(line);
            }
            else
            {
                err_ += std::string(line) + "\n";
            }
            changed_.notify_all();
            // hold(): the reader of the launcher's output stops here, and its pipes fill.
            changed_.wait(lock, [this] { return !held_; });
        };
        std::vector<std::string> all = args;
        if (detached)
        {
            all.insert(all.begin(), {"--run-detached", shown_path(o.launch)});
        }
        process_ = std::make_unique<vehicle_sil::process::Process>(detached ? o.helper : o.launch, all, std::move(options));
    }

    ~Live()
    {
        release();
        process_.reset(); // ends the launcher and its reader, which uses the members above
    }

    vehicle_sil::process::Process& process() { return *process_; }

    /// Stops reading the launcher's output (after the line that is being read) until release().
    void hold()
    {
        const std::lock_guard lock(mutex_);
        held_ = true;
    }

    void release()
    {
        const std::lock_guard lock(mutex_);
        held_ = false;
        changed_.notify_all();
    }

    /// Waits for an event line that contains every one of `parts`; returns it.
    std::string wait_for(const std::vector<std::string>& parts)
    {
        std::unique_lock lock(mutex_);
        std::string found;
        std::size_t next = 0;
        // Only a last line against a hang: the start takes a second or two.
        const bool seen = changed_.wait_for(lock, 120s, [&] {
            for (; next < out_.size() && found.empty(); ++next)
            {
                if (std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(out_[next], p); }))
                {
                    found = out_[next];
                }
            }
            return !found.empty() || process_->poll().has_value();
        });
        require(seen && !found.empty(), "no event with " + parts.front() + " in:\n" + text_locked() + "--- stderr ---\n" + err_);
        return found;
    }

    /// Waits until `n` event lines contain every one of `parts`.
    void wait_for_count(const std::vector<std::string>& parts, std::size_t n)
    {
        std::unique_lock lock(mutex_);
        std::size_t next = 0;
        std::size_t count = 0;
        const bool seen = changed_.wait_for(lock, 120s, [&] {
            for (; next < out_.size(); ++next)
            {
                count += std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(out_[next], p); }) ? 1 : 0;
            }
            return count >= n || process_->poll().has_value();
        });
        require(seen && count >= n, std::to_string(n) + " events with " + parts.front() + " did not come:\n" + text_locked());
    }

    void write(const std::string& text) { require(process_->write_input(text), "cannot write to the launcher"); }

    /// Waits for its end and reads the rest of its output; its status and its lines, unchecked.
    std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::vector<std::string>> finish_unchecked(
        std::chrono::milliseconds timeout = 120s)
    {
        release();
        const auto status = process_->wait_for(timeout);
        process_->finish_output(10s);
        const std::lock_guard lock(mutex_);
        log("stderr of the launcher:\n" + err_);
        return {status, out_};
    }

    /// Waits for its end; its status and its events (checked: framed, in order).
    std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::vector<std::string>> finish()
    {
        const auto [status, lines] = finish_unchecked();
        return {status, check_events(lines)};
    }

    std::string err()
    {
        const std::lock_guard lock(mutex_);
        return err_;
    }

    std::vector<std::string> lines()
    {
        const std::lock_guard lock(mutex_);
        return out_;
    }

private:
    /// The last event lines (the caller holds mutex_): enough to see where it stopped.
    std::string text_locked() const
    {
        std::string text;
        for (std::size_t i = out_.size() > 40 ? out_.size() - 40 : 0; i < out_.size(); ++i)
        {
            text += out_[i].substr(0, 400) + "\n";
        }
        return text;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> out_;
    std::string err_;
    bool held_ = false;
    std::unique_ptr<vehicle_sil::process::Process> process_;
};

/// Two ECUs that run until they are stopped (no stop_at_ms), with their process IDs in files.
fs::path two_ecus(const Options& o)
{
    return scenario_dir(o,
                        "vehicle_sil_simulation: 1\n"
                        "networks: []\n"
                        "participants:\n"
                        "  - name: Engine\n"
                        "    command: [" + ecu(o) + "]\n"
                        "  - name: Brake\n"
                        "    command: [" + ecu(o) + "]\n",
                        {{"Engine", "pid"}, {"Brake", "pid"}});
}

/// The run stopped in order: the result says `by`, both ECUs ended by themselves with 0.
void expect_stopped_in_order(const std::optional<vehicle_sil::process::ExitStatus>& status,
                             const std::vector<std::string>& lines, const std::string& by)
{
    std::string all;
    for (const auto& line : lines)
    {
        all += line + "\n";
    }
    require(status.has_value() && status->code == 130 && !status->signal,
            "the launcher ends with 130 when stopped, not " + (status ? std::to_string(status->code) : std::string("running")) +
                ":\n" + all);
    const std::string& result = lines.back();
    require(contains(result, "\"result\":\"interrupted\"") && contains(result, "\"simulation\":\"interrupted\"") &&
                contains(result, "\"interrupted_by\":\"" + by + "\""),
            "the result of a run stopped by " + by + ":\n" + result);
    for (const std::string name : {"Engine", "Brake"})
    {
        require(contains(result, "{\"name\":\"" + name + "\",\"required\":true,\"enabled\":true,\"ended\":\"exited\","
                                                          "\"exit_code\":0,"),
                name + " ended by itself with 0:\n" + result);
    }
    const auto note = events_with(lines, {"\"event\":\"finding\"", "\"code\":\"interrupted\"",
                                          "\"message\":\"interrupted (" + by + "): the launcher stopped the simulation and the "
                                                                               "participants\""});
    require(note.size() == 1, "the note interrupted as a finding:\n" + all);
    // Known when the simulation has stopped: written then, before the participants are ended.
    const auto stopping = events_with(lines, {"\"phase\":\"stopping\""});
    require(stopping.size() == 1 && std::stoll(value_of(note[0], "seq")) < std::stoll(value_of(stopping[0], "seq")),
            "the note comes before the stopping phase:\n" + all);
}

void stdin_stop(const Options& o)
{
    const fs::path dir = two_ecus(o);
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    // Nothing happens (without stop_at_ms the controller's time stays 0): a heartbeat every second.
    live.wait_for_count({"{\"event\":\"heartbeat\",\"seq\":"}, 2);
    live.write("not a command\n{\"command\":\"pause\"}\n");
    live.wait_for({"\"command\":\"pause\"", "\"accepted\":false"});
    live.write("{\"command\":\"stop\"}\n");
    live.wait_for({"\"phase\":\"stopping\""});
    // The end of input after a stop is no second stop (the ECUs still end by themselves).
    live.process().close_input();
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid", "Brake.pid"}, 30s);
    expect_stopped_in_order(status, lines, "stop command");
    const auto commands = events_with(lines, {"\"event\":\"command\""});
    require(commands.size() == 3, "three commands, no event for the end of input after the stop");
    require(contains(commands[0], "\"command\":null,\"source\":\"stdin\",\"accepted\":false,\"message\":\"the line is "
                                  "not a JSON object: "),
            "a line that is no JSON object:\n" + commands[0]);
    require(contains(commands[1], "\"message\":\"unknown command 'pause': the commands are stop\""),
            "an unknown command:\n" + commands[1]);
    require(contains(commands[2], "\"command\":\"stop\",\"source\":\"stdin\",\"accepted\":true,\"message\":null}"),
            "the stop:\n" + commands[2]);
    // The command comes before what it does.
    const auto at = [&lines](const std::string& part) {
        return std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return contains(l, part); }) - lines.begin();
    };
    require(at("\"accepted\":true") < at("\"phase\":\"stopping\""), "the stop command comes before the stopping phase");
    require(gone, "no participant is left");
    // The process IDs are the real ones; the time is written when it changes, so only once here.
    require(value_of(lines[0], "pid") == std::to_string(live.process().id()), "hello's pid is the launcher's:\n" + lines[0]);
    for (const std::string name : {"Engine", "Brake"})
    {
        const auto started = events_with(lines, {"\"name\":\"" + name + "\"", "\"state\":\"started\""});
        require(started.size() == 1 && value_of(started[0], "pid") == std::to_string(read_pid((dir / (name + ".pid")).string(), 0ms)),
                name + "'s started event has its process ID");
    }
    require(events_with(lines, {"\"event\":\"time\""}).size() == 1, "the time is written when it changes");
    const auto beats = events_with(lines, {"\"event\":\"heartbeat\""});
    require(!beats.empty() && members_of(beats[0]).empty(), "a heartbeat has no members:\n" + beats[0]);
}

void stdin_eof(const Options& o)
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n");
    // Without --stdin-commands, the input is not read: closed at once, it changes nothing.
    {
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.process().close_input();
        const auto [status, lines] = live.finish();
        require(status.has_value() && status->code == 0, "the run without --stdin-commands passes");
        require(contains(lines.back(), "\"simulation\":\"reached_stop_at\"") && events_with(lines, {"\"event\":\"command\""}).empty(),
                "it ran to stop_at_ms:\n" + lines.back());
    }
    const fs::path forever = two_ecus(o);
    Live live(o, forever, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    live.process().close_input();
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(forever, {"Engine.pid", "Brake.pid"}, 30s);
    expect_stopped_in_order(status, lines, "end of input");
    require(events_with(lines, {"\"event\":\"command\",", "\"command\":\"stop\",\"source\":\"eof\",\"accepted\":true"}).size() == 1,
            "the end of input is a stop command");
    require(gone, "no participant is left");
}

void stdin_twice(const Options& o)
{
    // Deaf does not end after the simulation and ignores the stop request; stop_timeout_ms would
    // wait 10 minutes twice. A second stop command ends the waiting.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_timeout_ms: 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Deaf", "pid hang-after-end ignore-stop"}});
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    live.write("{\"command\":\"stop\"}\n");
    live.wait_for({"\"phase\":\"stopping\""});
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Deaf.pid"}, 30s);
    require(status.has_value() && status->code == 130, "the launcher ends with 130 after the second stop");
    require(events_with(lines, {"\"name\":\"Deaf\"", "\"state\":\"killed\""}).size() == 1, "Deaf was killed");
    require(contains(lines.back(), "\"ended\":\"killed\""), "the result says so:\n" + lines.back());
    require(gone, "Deaf has ended");
}

void stdin_ending(const Options& o)
{
    // Stuck does not end after the simulation; the launcher would wait 10 minutes for it. A stop
    // command while it waits: it asks Stuck to stop at once.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "stop_timeout_ms: 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Stuck\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Stuck", "pid hang-after-end stop-exit 9"}});
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"stopping\""});
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Stuck.pid"}, 30s);
    require(status.has_value() && status->code == 130, "the launcher ends with 130 when stopped while it waits");
    require(contains(lines.back(), "\"result\":\"interrupted\"") && contains(lines.back(), "\"simulation\":\"reached_stop_at\""),
            "the result:\n" + lines.back());
    require(events_with(lines, {"\"name\":\"Stuck\"", "\"state\":\"stop_requested\""}).size() == 1 ||
                !can_request_stop(),
            "Stuck was asked to stop");
    // A finding known only after the participants have ended is an event too, before the result.
    const auto notes = events_with(lines, {"\"event\":\"finding\"", "\"code\":\"interrupted\""});
    require(notes.size() == 1 && contains(notes[0], "the launcher stopped the participants instead of waiting for them to end"),
            "the note as a finding");
    require(gone, "Stuck has ended");
}

void output_closed(const Options& o)
{
    {
        const fs::path dir = two_ecus(o);
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.wait_for({"\"phase\":\"running\""});
        // The reader goes away: the launcher's events have nowhere to go (its stderr closes too).
        live.process().finish_output(0ms);
        const auto status = live.process().wait_for(120s);
        const bool gone = all_gone(dir, {"Engine.pid", "Brake.pid"}, 30s);
        require(status.has_value() && status->code == 130 && !status->signal,
                "the launcher stops in order (130) when its output closes, not " +
                    (status ? std::to_string(status->code) + (status->signal ? " (signal)" : "") : std::string("running")));
        require(gone, "no participant is left");
    }
    // While it waits for a participant that never joins (10 minutes): nothing but the heartbeat
    // is written then, and the launcher notices all the same.
    {
        const fs::path dir = scenario_dir(o, "vehicle_sil_simulation: 1\n"
                                             "start_timeout_ms: 600000\n"
                                             "networks: []\n"
                                             "participants:\n"
                                             "  - name: Ghost\n"
                                             "    command: [" + helper(o) + ", --on-stop, \"9\", --pid-file, ghost.pid, --sleep-ms, \"600000\", --uri, \"{registry_uri}\"]\n"
                                             "    describe: false\n");
        fs::remove(dir / "ghost.pid");
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.wait_for({"\"phase\":\"waiting\""});
        const long ghost = read_pid((dir / "ghost.pid").string(), 60s);
        const auto closed = std::chrono::steady_clock::now();
        live.process().finish_output(0ms);
        const auto status = live.process().wait_for(60s);
        log("closed while waiting: the launcher ended after " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - closed).count()) + " ms");
        const bool gone = all_gone(dir, {"ghost.pid"}, 30s);
        require(ghost > 0 && status.has_value() && status->code == 130, "the launcher stops (130) when its output closes while it waits for the start");
        require(gone, "no participant is left");
    }
    // While it waits for a participant to end after the simulation (10 minutes).
    {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "stop_at_ms: 100\n"
                                          "stop_timeout_ms: 600000\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Stuck\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Stuck", "pid hang-after-end stop-exit 9"}});
        fs::remove(dir / "Stuck.pid");
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        live.wait_for({"\"phase\":\"stopping\""});
        live.process().finish_output(0ms);
        const auto status = live.process().wait_for(60s);
        const bool gone = all_gone(dir, {"Stuck.pid"}, 30s);
        require(status.has_value() && status->code == 130, "the launcher stops (130) when its output closes while it ends the participants");
        require(gone, "no participant is left");
    }
}

void stop_once(const Options& o)
{
    // Deaf does not end after the simulation and ignores the stop request: after a stop command the
    // launcher waits stop_timeout_ms (10 minutes) for it. The end of input and a closed output
    // after that stop are no second stop: it still waits.
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_timeout_ms: 600000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Deaf\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Deaf", "pid hang-after-end ignore-stop"}});
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    const long deaf = read_pid((dir / "Deaf.pid").string(), 60s);
    live.write("{\"command\":\"stop\"}\n");
    live.wait_for({"\"phase\":\"stopping\""});
    live.process().close_input();
    std::this_thread::sleep_for(1500ms);
    const bool waitsAfterEof = !live.process().poll().has_value() && process_alive(deaf) &&
                               events_with(live.lines(), {"\"state\":\"killed\""}).empty();
    // A closed output is seen within a second (the heartbeat).
    live.process().finish_output(0ms);
    std::this_thread::sleep_for(3000ms);
    const bool waitsAfterClose = !live.process().poll().has_value() && process_alive(deaf);
    // The launcher's death ends Deaf.
    kill_one(live.process().id());
    static_cast<void>(live.process().wait_for(30s));
    const bool gone = all_gone(dir, {"Deaf.pid"}, 30s);
    require(deaf > 0 && waitsAfterEof, "the end of input after a stop is no second stop: the launcher still waits for Deaf");
    require(waitsAfterClose, "a closed output after a stop is no second stop: the launcher still waits for Deaf");
    require(gone, "Deaf has ended");
}

/// Engine writes `lines` lines in every step, Brake nothing; they run until they are stopped
/// (or to `stop_at_ms`). Their process IDs are in files; nothing of an earlier run is left.
fs::path chatty(const Options& o, int lines, const std::string& more = "")
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n" + more +
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "pid chatter " + std::to_string(lines)}, {"Brake", "pid"}});
    std::error_code ec;
    fs::remove(dir / "Engine.pid", ec);
    fs::remove(dir / "Brake.pid", ec);
    fs::remove_all(dir / "logs", ec);
    return dir;
}

/// The size of `file` as a reader sees it now (0 if it cannot be opened): not the size in its
/// directory, which Windows brings up to date only when the writer closes the file.
std::uintmax_t size_now(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    const auto at = in ? static_cast<std::streamoff>(in.tellg()) : std::streamoff{0};
    return at > 0 ? static_cast<std::uintmax_t>(at) : 0;
}

/// Waits up to `timeout` until `file` has grown by `bytes`; how much it grew.
std::uintmax_t growth(const fs::path& file, std::uintmax_t bytes, std::chrono::milliseconds timeout)
{
    const std::uintmax_t from = size_now(file);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uintmax_t now = from;
    while (now < from + bytes && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(20ms);
        now = std::max(now, size_now(file));
    }
    return now - from;
}

/// What a scenario found, so that it can say everything that is wrong at its end (after it has
/// cleaned up).
struct Checks
{
    std::vector<std::string> failed;

    void expect(bool condition, const std::string& what)
    {
        log(std::string(condition ? "CHECK " : "FAILED ") + what);
        if (!condition)
        {
            failed.push_back(what);
        }
    }

    void done() const
    {
        std::string all;
        for (const auto& what : failed)
        {
            all += what + "\n";
        }
        require(failed.empty(), all);
    }
};

long long ms_since(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
}

/// A chatty run whose reader has stopped reading, once Engine has written 1 MiB more (the pipe
/// of the events is full by then, and what the launcher keeps in memory too).
struct Stalled
{
    fs::path dir;
    std::unique_ptr<Live> live;
    long engine = 0;
    long brake = 0;

    /// `grow`: false if the launcher may give the reader up before Engine has written that much.
    Stalled(const Options& o, const std::vector<std::string>& args, Checks& checks, bool grow = true) : dir(chatty(o, 20))
    {
        live = std::make_unique<Live>(o, dir, args);
        live->wait_for({"\"phase\":\"running\""});
        engine = read_pid((dir / "Engine.pid").string(), 60s);
        brake = read_pid((dir / "Brake.pid").string(), 60s);
        live->hold();
        if (!grow)
        {
            return;
        }
        const auto grown = growth(dir / "logs" / "Engine.log", 1 << 20, 30s);
        checks.expect(grown >= (1 << 20), "Engine goes on writing while the reader of the events does not read (its log grew by " +
                                              std::to_string(grown) + " bytes in at most 30 s)");
    }

    /// Both ECUs have ended within `timeout`.
    bool participants_gone(std::chrono::milliseconds timeout) const { return wait_gone(engine, timeout) && wait_gone(brake, timeout); }

    /// Ends what is left; the launcher's status after `timeout` and its lines.
    std::pair<std::optional<vehicle_sil::process::ExitStatus>, std::vector<std::string>> end(std::chrono::milliseconds timeout)
    {
        auto status = live->process().wait_for(timeout);
        if (!status)
        {
            kill_one(live->process().id());
        }
        auto [last, lines] = live->finish_unchecked(30s);
        for (const long pid : {engine, brake})
        {
            if (process_alive(pid))
            {
                kill_one(pid);
            }
        }
        return {status, lines};
    }
};

std::string code_text(const std::optional<vehicle_sil::process::ExitStatus>& status)
{
    return status ? "exit code " + std::to_string(status->code) : std::string("still running (killed by the test)");
}

void events_stalled(const Options& o)
{
    Checks checks;
    // The limit (--events-stall-ms) is far away: a stop command works while the reader does not
    // read, and a second one ends the launcher's wait for the reader (it cannot write its result).
    const std::vector<std::string> kRun{"run", "--events", "--events-stall-ms", "600000", "--stdin-commands", "--log-dir", "logs", "sim.yaml"};
    {
        Stalled run(o, kRun, checks);
        const auto asked = std::chrono::steady_clock::now();
        run.live->write("{\"command\":\"stop\"}\n");
        const bool gone = run.participants_gone(30s);
        checks.expect(gone, "a stop command stops the participants while the reader does not read (" + std::to_string(ms_since(asked)) + " ms)");
        checks.expect(!run.live->process().poll().has_value(), "the launcher waits for its reader to write the result");
        run.live->write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = run.end(30s);
        checks.expect(status.has_value() && status->code == 130,
                      "a second stop command ends the wait: 130, " + code_text(status) + " after " + std::to_string(ms_since(asked)) + " ms");
        check_cut_events(lines);
    }
    // --timeout-ms aborts the simulation while the reader does not read.
    {
        std::vector<std::string> args = kRun;
        args.insert(args.begin() + 1, {"--timeout-ms", "5000"});
        Stalled run(o, args, checks);
        checks.expect(run.participants_gone(60s), "--timeout-ms ends the run while the reader does not read");
        // Two: the launcher may still be ending the participants (their output), where the first
        // only ends that; a stop while it waits for the reader, or a second one, ends the wait.
        run.live->write("{\"command\":\"stop\"}\n");
        std::this_thread::sleep_for(200ms);
        static_cast<void>(run.live->process().write_input("{\"command\":\"stop\"}\n")); // it may have ended
        const auto [status, lines] = run.end(30s);
        // The run failed (the time-out) before the stop came; its result could not be written.
        checks.expect(status.has_value() && status->code == 3, "the result of the timed-out run cannot be written: 3, " + code_text(status));
        checks.expect(contains(run.live->err(), "error: cannot write the result"), "the launcher says so on stderr:\n" + run.live->err());
    }
    // A signal (Windows: Ctrl+Break, if there is a console to send it through).
    if (can_request_stop())
    {
        Stalled run(o, {"run", "--events", "--events-stall-ms", "600000", "--log-dir", "logs", "sim.yaml"}, checks);
        const auto interrupt = [&run] {
#ifdef _WIN32
            static_cast<void>(run.live->process().request_stop());
#else
            ::kill(static_cast<pid_t>(run.live->process().id()), SIGINT);
#endif
        };
        interrupt();
        checks.expect(run.participants_gone(30s), "an interrupt stops the participants while the reader does not read");
        interrupt();
        const auto [status, lines] = run.end(30s);
        checks.expect(status.has_value() && status->code == 130, "a second interrupt ends the wait: 130, " + code_text(status));
    }
    // The limit itself: the launcher gives the reader up, stops the run and says why.
    {
        Stalled run(o, {"run", "--events", "--events-stall-ms", "1500", "--log-dir", "logs", "sim.yaml"}, checks, false);
        const auto held = std::chrono::steady_clock::now();
        const bool ended = run.live->process().wait_for(30s).has_value();
        const long long after = ms_since(held);
        const bool gone = run.participants_gone(10s);
        const auto [status, lines] = run.end(0ms);
        checks.expect(ended && status.has_value() && status->code == 130,
                      "the launcher gives up a reader that does not read: 130, " + code_text(status) + " after " +
                          std::to_string(after) + " ms");
        checks.expect(gone, "it has ended the participants");
        const std::string err = run.live->err();
        checks.expect(contains(err, "vehicle_sil_launch: cannot write the events (their reader has not read for 1500 ms") &&
                          contains(err, "stopping the run"),
                      "it says why on stderr:\n" + err);
        check_cut_events(lines);
    }
    checks.done();
}

void events_slow(const Options& o)
{
    // The reader stops for a while and reads again: the participants' lines that did not fit are
    // dropped and counted; everything else is written, and the stream ends with the result.
    Checks checks;
    Stalled run(o, {"run", "--events", "--events-stall-ms", "600000", "--stdin-commands", "--log-dir", "logs", "sim.yaml"}, checks);
    run.live->release();
    run.live->wait_for({"\"event\":\"log_dropped\""});
    run.live->write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = run.live->finish();
    const bool gone = run.participants_gone(30s);
    checks.expect(status.has_value() && status->code == 130 && contains(lines.back(), "\"interrupted_by\":\"stop command\""),
                  "the run goes on and is stopped in order: " + code_text(status));
    // Every line of Engine is an event or is counted in a log_dropped event: as many as its log has.
    std::size_t events = 0;
    std::size_t dropped = 0;
    for (const auto& line : lines)
    {
        const std::string event = value_of(line, "event");
        events += event == "log" && value_of(line, "source") == "Engine" ? 1 : 0;
        if (event == "log_dropped")
        {
            const std::string engine = value_of(line.substr(line.find("\"sources\":")), "Engine");
            dropped += engine.empty() ? 0 : std::stoul(engine);
            checks.expect(std::stoul(value_of(line, "count")) >= (engine.empty() ? 0 : std::stoul(engine)) && members_of(line).rfind("\"count\":", 0) == 0,
                          "log_dropped has count and sources: " + line);
        }
    }
    std::size_t written = 0;
    {
        std::ifstream log(run.dir / "logs" / "Engine.log", std::ios::binary);
        for (std::string line; std::getline(log, line);)
        {
            ++written;
        }
    }
    checks.expect(dropped > 0, "lines were dropped while the reader did not read");
    checks.expect(events + dropped == written, "Engine wrote " + std::to_string(written) + " lines: " + std::to_string(events) +
                                                   " are events, " + std::to_string(dropped) + " are counted as dropped");
    checks.expect(gone, "no participant is left");
    checks.done();
}

void events_findings(const Options& o)
{
    // Engine's step fails at 200 ms; its process ends by itself 1.5 s after it has left the
    // simulation, while the launcher ends the participants. What the launcher knows about Engine
    // changes in between (first that it left, then how its process ended).
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 1000\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n"
                                      "  - name: Brake\n"
                                      "    command: [" + ecu(o) + "]\n",
                                      {{"Engine", "throw-at-ms 200 linger-ms 1500"}});
    const Captured run = launch(o, {"run", "--events", "sim.yaml"}, dir);
    expect_code(run, 1, "run --events with an ECU whose step fails");
    std::string findings;
    for (const auto& line : lines_of(run.standard_output))
    {
        findings += value_of(line, "event") == "finding" ? line + "\n" : std::string{};
    }
    log("the finding events:\n" + findings);
    // check_events(): every finding event is in the result as written, and none comes twice.
    const std::vector<std::string> lines = check_events(run.standard_output);
    const auto failed = events_with(lines, {"\"event\":\"finding\"", "\"code\":\"participant-failed\""});
    require(failed.size() == 1 && contains(failed[0], "its process exited with code 1"),
            "one participant-failed finding, with how the process ended:\n" + findings);
    // That Engine failed is an event at once (never dropped, unlike the launcher's line): before
    // the others are stopped, before Engine's process has ended, before the finding.
    const auto seq = [&lines](const std::vector<std::string>& parts) {
        const auto found = events_with(lines, parts);
        require(found.size() == 1, "one event with " + parts.back() + ", not " + std::to_string(found.size()));
        log(found[0].substr(0, 400));
        return std::stoll(value_of(found[0], "seq"));
    };
    const std::string engine = "\"name\":\"Engine\"";
    const auto failedAt = seq({"\"event\":\"participant\"", engine, "\"state\":\"failed\",\"reason\":\"left_simulation\"}"});
    const auto stoppingAt = seq({"\"phase\":\"stopping\""});
    const auto endedAt = seq({"\"event\":\"participant\"", engine, "\"state\":\"ended\""});
    const auto findingAt = seq({"\"event\":\"finding\"", "\"code\":\"participant-failed\""});
    require(failedAt < stoppingAt && stoppingAt < endedAt && endedAt < findingAt,
            "failed, then stopping, then Engine's ended, then the finding:\n" + run.standard_output);
    // A reader that does not know the state "failed" misses nothing else: the states it knows
    // come once each, as before.
    for (const std::string state : {"started", "joined", "ended"})
    {
        static_cast<void>(seq({"\"event\":\"participant\"", engine, "\"state\":\"" + state + "\""}));
    }
    // The same result as without --events.
    const Captured json = launch(o, {"run", "--json", "sim.yaml"}, dir);
    const std::string& last = lines.back();
    expect_text(last.substr(last.find(",\"result\":") + 10), compact(json.standard_output.substr(0, json.standard_output.size() - 1)) + "}",
                "the result event as run --json");

    // Engine's process ends in a step (exit code 7): the launcher learns of the failure by the end
    // of the process, so "ended" comes first; the finding is settled, and "failed" has its message.
    write_file(dir / "Engine.behaviour", "exit-at-ms 200 7\n");
    const Captured crash = launch(o, {"run", "--events", "sim.yaml"}, dir);
    expect_code(crash, 1, "run --events with an ECU whose process ends");
    const std::vector<std::string> crashed = check_events(crash.standard_output);
    const auto state = events_with(crashed, {"\"state\":\"failed\",\"reason\":\"process_ended\",\"message\":"});
    const auto ended = events_with(crashed, {"\"event\":\"participant\"", "\"name\":\"Engine\"", "\"state\":\"ended\""});
    const auto finding = events_with(crashed, {"\"event\":\"finding\"", "\"code\":\"participant-failed\""});
    require(state.size() == 1 && ended.size() == 1 && finding.size() == 1, "failed, ended and the finding once each:\n" + crash.standard_output);
    log(state[0]);
    require(std::stoll(value_of(ended[0], "seq")) < std::stoll(value_of(state[0], "seq")) &&
                std::stoll(value_of(state[0], "seq")) < std::stoll(value_of(finding[0], "seq")),
            "ended, then failed, then the finding");
    require(value_of(state[0], "message") == value_of(finding[0], "message") && contains(state[0], "its process exited with code 7"),
            "failed has the finding's message:\n" + state[0] + "\n" + finding[0]);
}

void events_stderr(const Options& o)
{
    // The launcher's standard output and standard error are one pipe (2>&1 of a shell), and
    // nothing reads it: the line that says why the events closed has nowhere to go either. The
    // launcher ends all the same.
    const fs::path dir = chatty(o, 20);
    std::mutex mutex;
    std::condition_variable changed;
    bool held = true;
    vehicle_sil::process::ProcessOptions options;
    options.working_directory = dir;
    options.on_line = [&](vehicle_sil::process::Stream, std::string_view) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&held] { return !held; });
    };
    const std::vector<std::string> run{"run", "--events", "--events-stall-ms", "1500", "sim.yaml"};
#ifdef _WIN32
    const char* shell = std::getenv("ComSpec");
    require(shell != nullptr, "ComSpec is not set");
    std::vector<std::string> args{"/c", shown_path(o.launch)};
    args.insert(args.end(), run.begin(), run.end());
    args.push_back("2>&1");
#else
    const char* shell = "/bin/sh";
    std::vector<std::string> args{"-c", "exec \"$0\" \"$@\" 2>&1", o.launch.string()};
    args.insert(args.end(), run.begin(), run.end());
#endif
    const auto from = std::chrono::steady_clock::now();
    vehicle_sil::process::Process wrapper(shell, args, std::move(options));
    const long engine = read_pid((dir / "Engine.pid").string(), 60s);
    const long brake = read_pid((dir / "Brake.pid").string(), 60s);
    const auto status = wrapper.wait_for(60s);
    const long long after = ms_since(from);
    const bool gone = wait_gone(engine, 10s) && wait_gone(brake, 10s);
    if (!status)
    {
        wrapper.kill_tree();
    }
    for (const long pid : {engine, brake})
    {
        if (process_alive(pid))
        {
            kill_one(pid);
        }
    }
    {
        const std::lock_guard lock(mutex);
        held = false;
        changed.notify_all();
    }
    wrapper.finish_output(10s);
    require(engine > 0 && status.has_value() && status->code == 130,
            "the launcher gives the reader up and ends (130) although its stderr is not read either: " + code_text(status) +
                " after " + std::to_string(after) + " ms");
    log("ended after " + std::to_string(after) + " ms");
    require(gone, "no participant is left");
}

void events_nonblocking(const Options& o)
{
#ifdef _WIN32
    static_cast<void>(o);
    throw Skip("a standard output with O_NONBLOCK is a POSIX case");
#else
    // The launcher's standard output is a pipe whose writes do not block (whoever started it set
    // O_NONBLOCK on it), and its reader reads late: a full pipe is no closed pipe.
    const fs::path dir = chatty(o, 20, "stop_at_ms: 3000\n");
    int fds[2];
    require(::pipe(fds) == 0 && ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL) | O_NONBLOCK) == 0, "a pipe");
    const std::string program = o.launch.string();
    const std::string in = dir.string();
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        if (::chdir(in.c_str()) != 0 || ::dup2(fds[1], STDOUT_FILENO) < 0)
        {
            ::_exit(126);
        }
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl(program.c_str(), program.c_str(), "run", "--events", "--log-dir", "logs", "sim.yaml", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    require(pid > 0, "fork");
    ::close(fds[1]);
    // The whole run writes about 6 MiB; the pipe holds 64 KiB.
    const auto grown = growth(dir / "logs" / "Engine.log", 512 * 1024, 60s);
    std::string out;
    char buffer[65536];
    for (ssize_t got = 0; (got = ::read(fds[0], buffer, sizeof(buffer))) != 0;)
    {
        require(got > 0 || errno == EINTR, "read");
        out.append(buffer, got > 0 ? static_cast<std::size_t>(got) : 0);
    }
    ::close(fds[0]);
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "waitpid");
    require(grown >= 512 * 1024, "Engine wrote before the events were read");
    const std::vector<std::string> all = lines_of(out);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "the run passes although its output was full for a while: status " + std::to_string(status) + ", " +
                std::to_string(all.size()) + " lines, the last:\n" + (all.empty() ? std::string{} : all.back().substr(0, 300)));
    const std::vector<std::string> lines = check_events(all);
    require(contains(lines.back(), "\"result\":\"passed\""), "the result is the last line:\n" + lines.back().substr(0, 300));
#endif
}

void events_output(const Options& o)
{
#ifdef _WIN32
    static_cast<void>(o);
    throw Skip("the shell redirections of this scenario are POSIX");
#else
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n"
                                      "stop_at_ms: 300\n"
                                      "networks: []\n"
                                      "participants:\n"
                                      "  - name: Engine\n"
                                      "    command: [" + ecu(o) + "]\n");
    vehicle_sil::process::CaptureOptions options;
    options.timeout = 120s;
    options.working_directory = dir;
    // A file: every event is in it.
    const Captured file = vehicle_sil::process::run_captured(
        "/bin/sh", {"-c", "exec \"$0\" run --events sim.yaml > events.ndjson", o.launch.string()}, options);
    expect_code(file, 0, "run --events > file");
    const std::vector<std::string> lines = check_events(read_text(dir / "events.ndjson"));
    require(contains(lines.back(), "\"result\":\"passed\""), "the result is in the file");
    // No standard output at all: a mistake of whoever started it, said at once; nothing starts.
    const Captured none = vehicle_sil::process::run_captured(
        "/bin/sh", {"-c", "exec \"$0\" run --events sim.yaml >&-", o.launch.string()}, options);
    expect_code(none, 4, "run --events without a standard output");
    require(contains(none.standard_error, "error: --events needs a standard output to write the events to"),
            "the error on stderr:\n" + none.standard_error);
#endif
}

void stdin_killed(const Options& o)
{
    // The last resort of a program that runs the launcher: its input closes, and at once it is
    // killed (not its process group or job). Its participants end all the same.
    const fs::path dir = two_ecus(o);
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    const std::vector<long> pids{read_pid((dir / "Engine.pid").string(), 60s), read_pid((dir / "Brake.pid").string(), 60s)};
    live.process().close_input();
    kill_one(live.process().id());
    require(live.process().wait_for(120s).has_value(), "the launcher has ended");
    const bool gone = wait_gone(pids[0], 60s) && wait_gone(pids[1], 60s);
    for (const long pid : pids)
    {
        if (process_alive(pid))
        {
            kill_one(pid);
        }
    }
    require(gone, "no participant is left after the launcher was killed");
}

void no_console_stdin(const Options& o)
{
#ifdef _WIN32
    // No console: neither Ctrl+C nor Ctrl+Break reaches the launcher. A stop command does, and
    // the ECUs end by themselves when the simulation stops.
    const fs::path dir = two_ecus(o);
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"}, true);
    live.wait_for({"\"phase\":\"running\""});
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid", "Brake.pid"}, 30s);
    expect_stopped_in_order(status, lines, "stop command");
    require(gone, "no participant is left");
#else
    static_cast<void>(o);
    throw Skip("a launcher without a console is a Windows case");
#endif
}

int main(int argc, char** argv)
{
#ifndef _WIN32
    // A write to the input of a launcher that has ended fails instead of ending this test.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    set_log_tag("launch-run");
    try
    {
        const Options o = parse(argc, argv);
        fs::remove_all(o.work_dir);
        fs::create_directories(o.work_dir);
        log("scenario " + o.scenario);
        const std::vector<std::pair<std::string, void (*)(const Options&)>> scenarios = {
            {"docs", docs},         {"check", check},     {"crash", crash},         {"step", step},
            {"optional", optional}, {"not_joined", not_joined}, {"quitter", quitter}, {"hang", hang},
            {"grandchild", grandchild}, {"path", path},   {"interrupt", interrupt}, {"interrupt_twice", interrupt_twice},
            {"killed", killed}, {"ended_early", ended_early}, {"interrupt_ending", interrupt_ending},
            {"timeout", timeout}, {"no_console", no_console},
            {"usage", usage}, {"disabled", disabled}, {"events", events}, {"events_usage", events_usage},
            {"stdin_stop", stdin_stop}, {"stdin_eof", stdin_eof}, {"stdin_twice", stdin_twice}, {"stdin_ending", stdin_ending},
            {"output_closed", output_closed}, {"stdin_killed", stdin_killed}, {"no_console_stdin", no_console_stdin},
            {"stop_once", stop_once}, {"events_stalled", events_stalled}, {"events_slow", events_slow},
            {"events_findings", events_findings}, {"events_nonblocking", events_nonblocking}, {"events_output", events_output},
            {"events_stderr", events_stderr}};
        const auto it = std::find_if(scenarios.begin(), scenarios.end(), [&](const auto& s) { return s.first == o.scenario; });
        if (it == scenarios.end())
        {
            throw std::invalid_argument("unknown scenario " + o.scenario);
        }
        it->second(o);
        log("PASS");
        return 0;
    }
    catch (const Skip& e)
    {
        log(std::string("SKIP: ") + e.what());
        return 77;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return 1;
    }
}
