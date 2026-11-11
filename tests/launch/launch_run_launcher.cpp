// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch run as a process, with the ECU executables of examples/ecu_main, an
// ecu_main executable that misbehaves as a file in its working directory says (run_test_ecu.cpp)
// and the helper of tests/process:
//
//   docs         the run of the example and its output in docs/simulation-config.md (text and
//                JSON, byte for byte but for the version), the participants' lines on stderr,
//                --log-dir
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
#include "launcher_support.hpp"
#include "spawn_sleeper.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
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
                "here: name, command, step_ms, describe, required)\n"
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

} // namespace

int main(int argc, char** argv)
{
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
            {"usage", usage}};
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
