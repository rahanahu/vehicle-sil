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
//                event is in the result, and none comes twice; the state "failed" comes at once; a
//                process that ends in a step: one "failed" (by the end of the process or because it
//                left the simulation), one "ended", one finding with the exit code
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
//   realtime     speed 1 (the file), --speed 2, max and 0.5: the wall-clock time it takes at least, the
//                timing in the result and the line of the time in the text
//   late         an ECU whose steps take longer than real time: the late steps are counted, a warning
//                (not about a participant), --strict fails, the time events count them
//   pause_step   --start-paused and the time commands: step (rounded up to the controller's steps),
//                resume, a refused step, speed, pause, stop; it stands still where it paused
//   paused_failures while it stands still: a process that ends fails the run, --timeout-ms aborts it,
//                start_timeout_ms is not counted
//   time_usage   --start-paused without --stdin-commands, a wrong --speed, the options for check, and
//                wrong time keys in the file
//   scenario     run --scenario with the example of examples/ecu_main and its output in
//                docs/scenario.md (text, byte for byte); the scenario in the JSON and in the events;
//                the examples of scenario.md with mistakes (check) and that fails (text, the
//                scenario of the JSON)
//   scenario_same  one scenario, run as fast as possible, at a speed, with a slow ECU and with a
//                pause in between: the same result each time; every write takes effect in the step
//                of its time (the ECU says in which step it saw each value)
//   scenario_disconnect  the example scenario that takes the engine off the bus (ecu.connected):
//                the dashboard sees the speed stop and come back, at the same times at any speed
//   scenario_failures  a wait that times out, a simulation that stops before the scenario is over, an
//                ECU that fails in the middle, a scenario with errors (nothing is started), the
//                command line
//   manual       participants started by hand (--manual, manual: true), started by this test from
//                the command the launcher writes, as a person would copy it: the example and its
//                lines in docs/simulation-config.md. The other parts, each a scenario of its own
//                so that they run side by side: manual_late (started long after start_timeout_ms
//                and standing still in a step, as in a debugger), manual_fails (it fails while the
//                simulation runs: the events), manual_stop (a stop before the start ends one that
//                has joined), manual_frozen and manual_thawed (frozen as a debugger would), manual_again
//                (ended before the start), manual_timeout (--timeout-ms while the launcher waits)
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
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <tlhelp32.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
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

/// `text` with the values that depend on how fast the machine is as the docs show them: every
/// "wall_ms" and "actual_speed" (in the JSON and in the events) as <wall_ms> and <actual_speed>.
std::string without_timing(std::string text)
{
    for (const std::string key : {"wall_ms", "actual_speed"})
    {
        const std::string head = "\"" + key + "\":";
        for (auto at = text.find(head); at != std::string::npos; at = text.find(head, at + 1))
        {
            auto from = at + head.size();
            if (from < text.size() && text[from] == ' ')
            {
                ++from;
            }
            const auto to = text.find_first_of(",}\n", from);
            text.replace(from, to - from, "<" + key + ">");
        }
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
        fs::remove(dir / (name + ".hanging"));
    }
    return dir;
}

/// Waits until the test ECU `name` (hang-after-end) hangs after the run: from then on its own
/// handler answers a stop request. Before, ecu_main may still handle it, and the request is spent
/// on a run that has already stopped.
void wait_hanging(const fs::path& dir, const std::string& name)
{
    require(pid_written((dir / (name + ".hanging")).string(), 60s),
            name + " did not reach its end after the run within 60 s (no " + name + ".hanging)");
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

/// The copies in docs/howto/*.md: the block after each line "<!-- vehicle_sil_launch {command} ... -->"
/// there is the block after the same line in `docs` or in scenario.md (which this test compares with
/// the output).
void howto_copies_agree(const fs::path& docs, const std::string& command)
{
    const std::string head = "<!-- vehicle_sil_launch " + command + " ";
    const fs::path howto = docs.parent_path() / "howto";
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
                // The original is in `docs`, or, for a run of a scenario, in scenario.md next to it.
                const fs::path scenario = docs.parent_path() / "scenario.md";
                const bool in_docs = lf(read_text(docs)).find(line + "\n") != std::string::npos;
                const fs::path& original = in_docs ? docs : scenario;
                expect_text(block_after(entry.path(), line), block_after(original, line),
                            "docs/howto/" + entry.path().filename().string() + ": the copy after " + line +
                                " (copy the block after the same line in " + original.filename().string() + ")");
                ++copies;
            }
        }
    }
    require(copies > 0, "no copy of a vehicle_sil_launch " + command + " example in " + howto.string());
}

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
    expect_text(without_timing(without_versions(json.standard_output)),
                without_versions(block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --json -->")),
                "run simulation.yaml --json");
    // As fast as possible: the time as it went, never late, never paused.
    require(contains(json.standard_output, "  \"timing\": {\n    \"speed\": \"max\",\n    \"time_step_ms\": 10,\n") &&
                contains(json.standard_output, "    \"sim_ms\": 1000,\n") &&
                contains(json.standard_output, "    \"paced_steps\": 0,\n    \"late_steps\": 0,\n    \"max_late_ms\": 0,\n"
                                               "    \"slipped_ms\": 0,\n    \"late_ms\": 0,\n    \"paused_ms\": 0\n  },\n"),
            "the timing of a run as fast as possible:\n" + json.standard_output);
    // With a speed: the line of the time, as the manual shows it (the numbers that depend on the
    // machine as <...>). The controller steps every 5 ms then, as Dashboard does: 200 steps.
    const Captured paced = launch(o, {"run", "simulation.yaml", "--speed", "4"}, dir);
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
    const Captured logged = launch(o, {"run", "--json", "--log-dir", shown_path(logs), "simulation.yaml"}, dir);
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
                "here: name, command, step_ms, describe, required, enabled, manual)\n"
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
    const std::string shown = "\n--- output ---\n" + run.standard_output + "--- errors ---\n" + run.standard_error;
    require(sleeper > 0, "the ECU started its process (Engine-grandchild.pid)" + shown);
    require(gone, "the process the ECU left has ended (" + std::to_string(sleeper) + ")" + shown);
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
    wait_hanging(dir, "Stuck");
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

/// The ECUs' control agents: who is connected when the simulation starts, in the events, the
/// result and the findings.
void control(const Options& o)
{
    {
        // Engine and Brake offer variables; Tool declares none (it has ecu.connected); Quiet has some and does not offer them
        // (--no-control, which its --describe says too).
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "stop_at_ms: 200\n"
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
        const Captured run = launch(o, {"run", "sim.yaml", "--events"}, dir);
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
                                  "\"late_messages\":0}}],\"findings\":[]"),
                "the result has each participant's control link:\n" + out);
        // The check notes the variables that are not offered, and says what each executable offers.
        require(contains(out, "\"code\":\"control-link-not-offered\"") &&
                    contains(out, "participant 'Quiet': it describes variables, but does not offer them to the simulation") &&
                    contains(out, "\"control\":{\"transport\":\"silkit\",\"wire_version\":1}") && contains(out, "\"control\":null") &&
                    !contains(out, "control-link-no-hello") && !contains(out, "control-link-version"),
                "the check notes what is not offered, and nothing else is found:\n" + out);
        log("CHECK control agents in the events and the result -> ok");
    }
    {
        // An ECU whose --describe offers its variables and whose run does not (it runs with
        // --no-control behind the launcher's back): the start waits for its hello in vain, and
        // says so.
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "stop_at_ms: 200\n"
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
    require(contains(json, "      \"name\": \"Engine\",\n      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n"
                           "      \"ended\": \"not_started\",\n      \"exit_code\": null,\n      \"signal\": null,\n"
                           "      \"left_processes\": false,\n      \"log\": null,\n      \"control_link\": {\n"
                           "        \"state\": \"none\",\n") &&
                contains(json, "      \"name\": \"Brake\",\n      \"required\": true,\n      \"enabled\": true,\n      \"manual\": false,\n"
                               "      \"ended\": \"exited\",\n      \"exit_code\": 0,\n") &&
                contains(json, "        \"enabled\": false,\n        \"manual\": false,\n        \"step_ms\": null,\n        \"status\": \"disabled\",\n"),
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
                                                  "false,\n      \"manual\": false,\n      \"ended\": \"not_started\",\n      \"exit_code\": null,\n"
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

/// `text` (event lines) without the heartbeat events and the time events between the first and
/// the last (how many there are depends on how long the run takes), "seq" counted without them.
std::string without_heartbeats(const std::string& text)
{
    std::string out;
    std::size_t seq = 0;
    const std::vector<std::string> all = lines_of(text);
    std::vector<std::size_t> times;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (value_of(all[i], "event") == "time")
        {
            times.push_back(i);
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const std::string& line = all[i];
        if (value_of(line, "event") == "heartbeat" ||
            (value_of(line, "event") == "time" && i != times.front() && i != times.back()))
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
    expect_text(without_timing(with_placeholders(without_heartbeats(alone.standard_output), dir)),
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
    // The virtual time when it changes: 0 at the start, stop_at_ms at the end, at most every 100 ms
    // in between (more on a slow machine), always further on.
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(times.size() >= 2 && contains(times.front(), "\"sim_ms\":0,") && contains(times.back(), "\"sim_ms\":1000,"),
            "the virtual time at 0 and at stop_at_ms:\n" + both.standard_output);
    for (std::size_t i = 1; i < times.size(); ++i)
    {
        require(std::stoll(value_of(times[i], "sim_ms")) > std::stoll(value_of(times[i - 1], "sim_ms")) &&
                    (i + 1 == times.size() ||
                     // (t_ms is stamped when the event is written, a moment after the look at
                     // the clock that spaced them: room for that.)
                     std::stoll(value_of(times[i], "t_ms")) - std::stoll(value_of(times[i - 1], "t_ms")) >= 90),
                "the time moves on, at most every 100 ms:\n" + times[i - 1] + "\n" + times[i]);
        require(contains(times[i], "\"speed\":\"max\",\"paused\":false,") && contains(times[i], "\"late_steps\":0,\"max_late_ms\":0,\"slipped_ms\":0,\"late_ms\":0}"),
                "as fast as possible, never late:\n" + times[i]);
    }
    // The result is the JSON of --json.
    const Captured json = launch(o, {"run", "--json", "simulation.yaml"}, dir);
    expect_code(json, 0, "run --json simulation.yaml");
    const std::string& last = lines.back();
    expect_text(without_timing(last.substr(last.find(",\"result\":") + 10)),
                without_timing(compact(json.standard_output.substr(0, json.standard_output.size() - 1)) + "}"),
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
        // An event is one line, however long (the result of a run with many variables or steps).
        options.max_line = std::size_t{1} << 20;
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

    /// Waits until its standard error makes `done` true, or it has ended; returns the standard error.
    template <class Done>
    std::string wait_for_error(Done done)
    {
        std::unique_lock lock(mutex_);
        // Only a last line against a hang.
        static_cast<void>(changed_.wait_for(lock, 120s, [&] { return done(err_) || process_->poll().has_value(); }));
        return err_;
    }

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
        require(contains(result, "{\"name\":\"" + name + "\",\"required\":true,\"enabled\":true,\"manual\":false,\"ended\":\"exited\","
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
    Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
    live.wait_for({"\"phase\":\"running\""});
    // Nothing happens (it stands still at 0): a heartbeat every second.
    live.wait_for_count({"{\"event\":\"heartbeat\",\"seq\":"}, 2);
    live.write("not a command\n{\"command\":\"jump\"}\n");
    live.wait_for({"\"command\":\"jump\"", "\"accepted\":false"});
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
    require(contains(commands[1], "\"message\":\"unknown command 'jump': the commands are stop, pause, resume, step and "
                                  "speed\""),
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
    // Standing still at 0 from the start; the end (it stands still nowhere then).
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(times.size() == 2 && contains(times[0], "\"sim_ms\":0,\"speed\":\"max\",\"paused\":true,") &&
                contains(times[1], "\"sim_ms\":0,\"speed\":\"max\",\"paused\":false,"),
            "the time is written when it changes:\n" + (times.empty() ? std::string{} : times[0]));
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
    wait_hanging(dir, "Stuck");
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
        wait_hanging(dir, "Stuck");
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
        // Gone here includes a process that is ending but that the launcher cannot reap yet (its
        // first thread has ended, others have not: under a sanitizer that takes a while). A stop
        // then comes while the launcher waits for the participants to end, which is an interrupted
        // run (130), rightly. So wait until the launcher has reaped them: their process IDs are
        // free then. Windows shows no such thing from outside: a second is room there.
#ifdef _WIN32
        std::this_thread::sleep_for(1000ms);
#else
        for (const long pid : {run.engine, run.brake})
        {
            const auto limit = std::chrono::steady_clock::now() + 30s;
            while (!(::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) && std::chrono::steady_clock::now() < limit)
            {
                std::this_thread::sleep_for(5ms);
            }
        }
#endif
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
    // (But for how long the two runs took, and for the controller's last step: the launcher
    // notices the failed participant either before or after the controller begins its step at
    // the time of the failure, so a run that a failure ends reaches that step or the one before.)
    const auto comparable = [](std::string text) {
        const std::string head = "\"sim_ms\":";
        const auto at = text.find(head);
        require(at != std::string::npos, "the result has sim_ms:\n" + text);
        const auto to = text.find_first_of(",}", at);
        const std::string value = text.substr(at + head.size(), to - at - head.size());
        require(value == "190" || value == "200", "the simulation got to the step of the failure (200 ms) or the one before, not to " + value);
        text.replace(at + head.size(), value.size(), "<sim_ms>");
        return without_timing(text);
    };
    expect_text(comparable(last.substr(last.find(",\"result\":") + 10)),
                comparable(compact(json.standard_output.substr(0, json.standard_output.size() - 1)) + "}"),
                "the result event as run --json");

    // Engine's process ends in a step (exit code 7). The launcher learns of the failure either by
    // the end of the process ("process_ended": "ended" comes first, and "failed" has the finding's
    // message) or, when its connection is gone first, because it left the simulation
    // ("left_simulation": "failed" comes before "ended", without a message). Either way: one
    // "failed", one "ended", one finding, which says how the process ended.
    write_file(dir / "Engine.behaviour", "exit-at-ms 200 7\n");
    const Captured crash = launch(o, {"run", "--events", "sim.yaml"}, dir);
    expect_code(crash, 1, "run --events with an ECU whose process ends");
    const std::vector<std::string> crashed = check_events(crash.standard_output);
    const auto state = events_with(crashed, {"\"event\":\"participant\"", "\"name\":\"Engine\"", "\"state\":\"failed\""});
    const auto ended = events_with(crashed, {"\"event\":\"participant\"", "\"name\":\"Engine\"", "\"state\":\"ended\""});
    const auto finding = events_with(crashed, {"\"event\":\"finding\"", "\"code\":\"participant-failed\""});
    require(state.size() == 1 && ended.size() == 1 && finding.size() == 1, "failed, ended and the finding once each:\n" + crash.standard_output);
    log(state[0]);
    require(contains(finding[0], "its process exited with code 7"), "the finding says how the process ended:\n" + finding[0]);
    const auto seqOf = [](const std::string& line) { return std::stoll(value_of(line, "seq")); };
    require(seqOf(state[0]) < seqOf(finding[0]), "failed before the finding:\n" + crash.standard_output);
    const std::string reason = value_of(state[0], "reason");
    if (reason == "process_ended")
    {
        require(seqOf(ended[0]) < seqOf(state[0]), "process_ended: ended, then failed:\n" + crash.standard_output);
        require(value_of(state[0], "message") == value_of(finding[0], "message"),
                "failed has the finding's message:\n" + state[0] + "\n" + finding[0]);
    }
    else
    {
        require(reason == "left_simulation", "failed with the reason process_ended or left_simulation, not " + reason + ":\n" + state[0]);
        require(seqOf(state[0]) < seqOf(ended[0]), "left_simulation: failed, then ended:\n" + crash.standard_output);
        require(!contains(state[0], "\"message\":"), "left_simulation: failed has no message:\n" + state[0]);
    }
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

// --- time: speed, pause, step, lateness ---

/// One test ECU, Engine (with `behaviour`), and the keys `keys` before networks.
fs::path one_ecu(const Options& o, const std::string& keys, const std::string& behaviour = "")
{
    return scenario_dir(o,
                        "vehicle_sil_simulation: 1\n" + keys +
                            "networks: []\n"
                            "participants:\n"
                            "  - name: Engine\n"
                            "    command: [" + ecu(o) + "]\n",
                        {{"Engine", behaviour}});
}

/// The value of `key` in the pretty JSON of --json (the first), as text.
std::string json_value(const std::string& json, const std::string& key)
{
    const std::string head = "\"" + key + "\": ";
    const auto at = json.find(head);
    require(at != std::string::npos, "no " + key + " in:\n" + json);
    const auto from = at + head.size();
    return json.substr(from, json.find_first_of(",\n", from) - from);
}

double json_number(const std::string& json, const std::string& key) { return std::stod(json_value(json, key)); }

void realtime(const Options& o)
{
    // Speed 1 (from the file): 1000 ms of virtual time take at least 1 s of wall-clock time. A
    // tolerance of 1 s: nothing that runs is that late (the test ECU's step takes no time).
    const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\nlate_tolerance_ms: 1000\n");
    Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "run at speed 1");
    const std::string& json = run.standard_output;
    require(json_value(json, "speed") == "1" && json_value(json, "sim_ms") == "1000" &&
                json_value(json, "time_step_ms") == "10" && json_value(json, "ecu_step_ms") == "10" &&
                json_value(json, "step_delay_ms") == "0" && json_value(json, "late_tolerance_ms") == "1000",
            "the timing of speed 1:\n" + json);
    // From the step at 0 to the step at 1000 ms: never shorter than 1000 ms (only a lower bound).
    require(json_number(json, "wall_ms") >= 999 && json_number(json, "actual_speed") <= 1.002,
            "at speed 1, 1000 ms of virtual time take at least 1000 ms:\n" + json);
    // The steps at 10 .. 1000 ms are paced (the one at 0 is the base), the one at stop_at_ms too.
    require(json_value(json, "paced_steps") == "100" && json_value(json, "late_steps") == "0" &&
                json_value(json, "slipped_ms") == "0" && json_value(json, "late_ms") == "0" &&
                json_value(json, "paused_ms") == "0" && json_number(json, "max_late_ms") <= 1000,
            "nothing late, nothing paused:\n" + json);
    require(contains(json, "  \"findings\": [],\n"), "no finding:\n" + json);

    // --speed replaces the file's speed: twice as fast takes at least half as long.
    run = launch(o, {"run", "--json", "--speed", "2", "sim.yaml"}, dir);
    expect_code(run, 0, "run --speed 2");
    require(json_value(run.standard_output, "speed") == "2" && json_number(run.standard_output, "wall_ms") >= 499,
            "--speed 2:\n" + run.standard_output);
    // As fast as possible: nothing is paced.
    run = launch(o, {"run", "--json", "--speed", "max", "sim.yaml"}, dir);
    expect_code(run, 0, "run --speed max");
    require(json_value(run.standard_output, "speed") == "\"max\"" && json_value(run.standard_output, "paced_steps") == "0",
            "--speed max:\n" + run.standard_output);
    // The text says how the time went, after the run's line.
    run = launch(o, {"run", "--speed", "0.5", "sim.yaml"}, dir);
    expect_code(run, 0, "run --speed 0.5");
    require(contains(run.standard_output, "run: the simulation ran to stop_at_ms (1000 ms) and ended\ntime: speed 0.5, ran at "),
            "the line of the time:\n" + run.standard_output);
    require(contains(run.standard_output, " times real time; 0 of 100 paced steps more than 1000 ms late (the latest by "),
            "the line of the time:\n" + run.standard_output);
    require(contains(run.standard_output, " ms), 0 ms behind in all\n"), "the line of the time:\n" + run.standard_output);
}

void on_time(const Options& o)
{
    // What a user does first: run --speed 1, nothing else. An ECU that does nothing must not be
    // told as late with the default tolerance (10 ms): the launcher's own timer has to be that
    // good on every system. This is a bound on real time, so a machine that stands still for 10 ms
    // fails a run: three runs, and one without a late step is enough (a timer that is too coarse
    // is late in every run).
    const fs::path dir = one_ecu(o, "stop_at_ms: 1000\n");
    std::string seen;
    bool onTime = false;
    for (int i = 0; i < 3 && !onTime; ++i)
    {
        const Captured run = launch(o, {"run", "--json", "--speed", "1", "sim.yaml"}, dir);
        expect_code(run, 0, "run --speed 1");
        const std::string& json = run.standard_output;
        require(json_value(json, "late_tolerance_ms") == "10" && json_value(json, "paced_steps") == "100" &&
                    json_number(json, "wall_ms") >= 999,
                "speed 1 with the default tolerance:\n" + json);
        const std::string numbers = "late_steps " + json_value(json, "late_steps") + ", max_late_ms " + json_value(json, "max_late_ms") +
                                    ", slipped_ms " + json_value(json, "slipped_ms") + ", wall_ms " + json_value(json, "wall_ms");
        log("run --speed 1 with the default tolerance: " + numbers);
        seen += numbers + "\n";
        onTime = json_value(json, "late_steps") == "0" && contains(json, "  \"findings\": [],\n");
    }
    require(onTime, "an ECU that does nothing is late at speed 1 with the default tolerance, three times:\n" + seen);
}

void late(const Options& o)
{
    // Each 10 ms step of Engine takes 30 ms of wall-clock time: at speed 1 the simulation runs at
    // a third of the speed or less, its steps begin 20 ms or more after their real time, and it
    // does not catch up. Only lower bounds: a slower machine is later. (Not every step is late: an
    // ECU that goes from one step straight into the next tells the others only after it, so the
    // controller sees every other step one step late, and the one after it on time.)
    const fs::path dir = one_ecu(o, "stop_at_ms: 300\nspeed: 1\n", "slow-ms 30");
    const Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "a run that falls behind real time (a warning)");
    const std::string& json = run.standard_output;
    require(json_value(json, "paced_steps") == "30" && json_number(json, "late_steps") >= 10 &&
                json_number(json, "max_late_ms") >= 19 && json_number(json, "slipped_ms") >= 550 &&
                json_number(json, "actual_speed") <= 0.35,
            "the steps that came late:\n" + json);
    // Nothing is caught up: the time it took is the time at the speed and what it slipped (the
    // lateness within the tolerance at the end is not in the slip).
    require(json_number(json, "wall_ms") >= 300 + json_number(json, "slipped_ms") - 2, "wall_ms is 300 ms and the slip:\n" + json);
    require(contains(json, "      \"severity\": \"warning\",\n      \"code\": \"behind-real-time\",\n      \"file\": \"sim.yaml\",\n"
                           "      \"line\": null,\n      \"participant\": null,\n"),
            "a warning about no participant (the launcher cannot tell which one is slow):\n" + json);
    const auto messageAt = json.find("\"message\": \"the simulation fell behind");
    require(messageAt != std::string::npos, "the warning's message:\n" + json);
    const std::string message = json.substr(messageAt + 11, json.find('\n', messageAt) - messageAt - 11);
    const std::string expected = "\"the simulation fell behind real time: " + json_value(json, "late_steps") +
                                 " of 30 paced steps of the launcher's controller (10 ms each) began more than 10 ms "
                                 "(late_tolerance_ms) after the real time they stand for, the latest by " +
                                 json_value(json, "max_late_ms") +
                                 " ms. The launcher does not catch up after a late step (the ECUs' periodic messages would "
                                 "leave in a burst): it goes on at the speed from there, so the simulation came " +
                                 json_value(json, "slipped_ms") + " ms behind in all and ran at " + json_value(json, "actual_speed") +
                                 " times real time (pauses not counted); ";
    require(message.rfind(expected, 0) == 0, "the warning:\n" + message + "\nexpected to start with\n" + expected);
    // --strict fails on it; the events have it before the participants are ended, and the time
    // events count the late steps as they come.
    const Captured strict = launch(o, {"run", "--strict", "--events", "sim.yaml"}, dir);
    expect_code(strict, 1, "--strict fails a run that fell behind");
    const auto lines = check_events(strict.standard_output);
    const auto at = [&lines](const std::string& part) {
        return std::find_if(lines.begin(), lines.end(), [&](const std::string& l) { return contains(l, part); }) - lines.begin();
    };
    require(at("\"code\":\"behind-real-time\"") < at("\"phase\":\"stopping\""), "the warning comes before stopping");
    const auto times = events_with(lines, {"\"event\":\"time\""});
    require(times.size() >= 3 && std::stoll(value_of(times.back(), "late_steps")) >= 10 &&
                std::stoll(value_of(times.back(), "max_late_ms")) >= 19 && std::stoll(value_of(times.back(), "slipped_ms")) >= 550 &&
                contains(times.back(), "\"speed\":1,"),
            "the time events count the late steps:\n" + strict.standard_output);
    // Last (it writes sim.yaml anew): the tolerance is the file's. As late, but within 100 s: no step is
    // late, nothing slips (the lateness adds up instead), nothing is found.
    const fs::path tolerant = one_ecu(o, "stop_at_ms: 300\nspeed: 1\nlate_tolerance_ms: 100000\n", "slow-ms 30");
    const Captured within = launch(o, {"run", "--json", "sim.yaml"}, tolerant);
    expect_code(within, 0, "a run within late_tolerance_ms");
    require(json_value(within.standard_output, "late_steps") == "0" && json_number(within.standard_output, "max_late_ms") >= 200 &&
                json_value(within.standard_output, "slipped_ms") == "0" && contains(within.standard_output, "  \"findings\": [],\n"),
            "late, but within the tolerance:\n" + within.standard_output);
}

void stall(const Options& o)
{
    // One slow interval: Engine's step at 300 ms takes 500 ms. One late step (more on a machine
    // that is slow elsewhere too), and the simulation goes on from there: it does not hurry
    // through the 490 ms it lost, so the run takes them longer.
    {
        const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\n", "slow-at-ms 300 500");
        const Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "one slow interval (a warning)");
        const std::string& json = run.standard_output;
        require(json_value(json, "paced_steps") == "100" && json_number(json, "late_steps") >= 1 &&
                    json_number(json, "max_late_ms") >= 480 && json_number(json, "slipped_ms") >= 480 &&
                    json_number(json, "wall_ms") >= 1000 + json_number(json, "slipped_ms") - 2 && json_value(json, "late_ms") == "0",
                "one slow interval:\n" + json);
        require(contains(json, "\"code\": \"behind-real-time\""), "the warning:\n" + json);
        log("one slow interval of 500 ms: late_steps " + json_value(json, "late_steps") + ", max_late_ms " +
            json_value(json, "max_late_ms") + ", slipped_ms " + json_value(json, "slipped_ms") + ", wall_ms " +
            json_value(json, "wall_ms"));
    }
    // Only the last interval is slow (the step at 990 ms): the controller's step at stop_at_ms,
    // which nothing holds back, is the late one.
    {
        const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\n", "slow-at-ms 990 500");
        const Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "a slow last interval (a warning)");
        const std::string& json = run.standard_output;
        require(json_value(json, "paced_steps") == "100" && json_number(json, "late_steps") >= 1 &&
                    json_number(json, "max_late_ms") >= 480 && json_number(json, "wall_ms") >= 1480 &&
                    contains(json, "\"code\": \"behind-real-time\""),
                "a slow last interval:\n" + json);
    }
    // stop_at_ms at the controller's first or second step after 0: the steps there are, are
    // measured. (A slow step at 0 would not count: the first step is the base, and the ECU may
    // run its own step at 0 before the controller's.)
    {
        fs::path dir = one_ecu(o, "stop_at_ms: 10\nspeed: 1\n");
        Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "a run of one step");
        require(json_value(run.standard_output, "paced_steps") == "1" && json_number(run.standard_output, "wall_ms") >= 9,
                "a run of one step:\n" + run.standard_output);
        dir = one_ecu(o, "stop_at_ms: 20\nspeed: 1\n", "slow-at-ms 10 300");
        run = launch(o, {"run", "--json", "sim.yaml"}, dir);
        expect_code(run, 0, "a run of two steps (a warning)");
        const std::string& json = run.standard_output;
        require(json_value(json, "paced_steps") == "2" && json_number(json, "late_steps") >= 1 &&
                    json_number(json, "max_late_ms") >= 280 && json_number(json, "wall_ms") >= 300,
                "a run of two steps:\n" + json);
    }
    if (!can_request_stop())
    {
        log("no console to send Ctrl+Break through: not the run that ends in a stall");
        return;
    }
    // Engine never returns from its step at 500 ms, and --timeout-ms ends the run 3 s after the
    // start: the step at 510 ms never begins. While it lasts, the time events say how late it is
    // by now; the result has the stall as a late step and in the time the run took.
    {
        const fs::path dir = one_ecu(o, "stop_timeout_ms: 300\nspeed: 1\n", "hang-at-ms 500");
        const Captured events = launch(o, {"run", "--events", "--timeout-ms", "3000", "sim.yaml"}, dir);
        expect_code(events, 1, "a run that ends in a stall");
        const auto lines = check_events(events.standard_output);
        const auto stalled = events_with(lines, {"\"event\":\"time\"", "\"sim_ms\":500,"});
        std::int64_t last = 0;
        std::size_t growing = 0;
        for (const auto& line : stalled)
        {
            const std::int64_t late = std::stoll(value_of(line, "late_ms"));
            require(late >= last, "the lateness of a stall only grows:\n" + events.standard_output);
            growing += late > last ? 1 : 0;
            if (late > 10)
            {
                require(std::stoll(value_of(line, "late_steps")) >= 1 && std::stoll(value_of(line, "max_late_ms")) >= late &&
                            std::stoll(value_of(line, "slipped_ms")) >= late,
                        "a step that is overdue is counted while it is:\n" + line);
            }
            last = late;
        }
        require(growing >= 3 && last >= 1500, "the time events show the stall while it lasts:\n" + events.standard_output);
        const std::string& result = lines.back();
        require(value_of(result, "sim_ms") == "500" && std::stoll(value_of(result, "late_ms")) >= 1500 &&
                    std::stoll(value_of(result, "late_steps")) >= 1 && std::stoll(value_of(result, "max_late_ms")) >= 1500 &&
                    std::stoll(value_of(result, "slipped_ms")) >= 1500 && std::stoll(value_of(result, "wall_ms")) >= 2000 &&
                    std::stod(value_of(result, "actual_speed")) <= 0.25,
                "the result of a run that ended in a stall:\n" + result);
        require(!events_with(lines, {"\"code\":\"behind-real-time\"", "when the run ended, the next step was " +
                                                                         value_of(result, "late_ms") + " ms overdue"})
                     .empty(),
                "the warning says that it ended in a stall:\n" + events.standard_output);
        // The time of the result is the gate's: from the first step to the abort. The launcher's
        // own end (the abort going round, the participants' end: 300 ms and more for the one that
        // hangs) is not in it.
        const std::int64_t ended = std::stoll(value_of(lines.back(), "t_ms"));
        require(std::stoll(value_of(result, "wall_ms")) <= ended - 300,
                "wall_ms ends at the abort, not when the participants have ended:\n" + result);
    }
}

void speed_change(const Options& o)
{
    // Speed 1 up to 200 ms or a little further, then speed 4: the 800 ms (or a little less) that
    // are left take a quarter of their time, counted from where the change came. Lower bounds:
    // 1000 ms of virtual time never take less than 200 + 800 / 4 ms.
    const fs::path dir = one_ecu(o, "stop_at_ms: 1000\nspeed: 1\nlate_tolerance_ms: 100000\n");
    Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
    // The third time event after the one at 0: 200 ms or more have passed (they are 100 ms apart).
    live.wait_for_count({"\"event\":\"time\"", "\"speed\":1,"}, 4);
    const std::int64_t reached = std::stoll(value_of(events_with(live.lines(), {"\"event\":\"time\""})[3], "sim_ms"));
    live.write("{\"command\":\"speed\",\"value\":4}\n");
    const auto [status, lines] = live.finish();
    require(status && status->code == 0, "a run whose speed changes on the way: 0");
    require(reached >= 200 && reached < 1000, "200 ms or more at speed 1: " + std::to_string(reached));
    // The change came at `reached` or later: the rest takes a quarter of its time at least.
    const std::int64_t least = reached + (1000 - reached) / 4;
    const std::string& result = lines.back();
    require(value_of(result, "sim_ms") == "1000" && contains(result, "\"speed\":4,") &&
                std::stoll(value_of(result, "wall_ms")) >= least - 1 &&
                std::stod(value_of(result, "actual_speed")) <= 1000.0 / static_cast<double>(least - 1) + 0.001 &&
                value_of(result, "paced_steps") == "100" && value_of(result, "slipped_ms") == "0",
            "the time of a run that went on four times as fast from " + std::to_string(reached) + " ms or later:\n" + result);
}

void step_width(const Options& o)
{
    const auto step_file = [](const fs::path& dir, const std::string& ms) { write_file(dir / "test-ecu-step-ms", ms + "\n"); };
    // An ECU that steps every millisecond. As fast as possible, the controller steps every 10 ms
    // as ever; with a speed, as finely as the ECU, so that every ECU step runs at its real time.
    fs::path dir = one_ecu(o, "stop_at_ms: 200\n");
    step_file(dir, "1");
    Captured run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "1 ms steps as fast as possible");
    require(json_value(run.standard_output, "time_step_ms") == "10" && json_value(run.standard_output, "ecu_step_ms") == "1" &&
                json_value(run.standard_output, "step_delay_ms") == "9" && contains(run.standard_output, "  \"findings\": [],\n"),
            "as fast as possible, the controller's step is 10 ms and nothing is found:\n" + run.standard_output);
    run = launch(o, {"run", "--json", "--speed", "2", "sim.yaml"}, dir);
    expect_code(run, 0, "1 ms steps at speed 2");
    require(json_value(run.standard_output, "time_step_ms") == "1" && json_value(run.standard_output, "ecu_step_ms") == "1" &&
                json_value(run.standard_output, "step_delay_ms") == "0" && json_value(run.standard_output, "paced_steps") == "200" &&
                json_number(run.standard_output, "wall_ms") >= 99 && !contains(run.standard_output, "coarse-time-step"),
            "with a speed, the controller steps as finely as the ECU:\n" + run.standard_output);
    // time_step_ms as written stays: 10 ms over 1 ms steps at a speed is a warning that says by
    // how much the ECU's steps run late, and what to write.
    dir = one_ecu(o, "stop_at_ms: 200\ntime_step_ms: 10\n");
    run = launch(o, {"run", "--json", "--speed", "2", "sim.yaml"}, dir);
    expect_code(run, 0, "time_step_ms coarser than the ECU's step (a warning)");
    require(json_value(run.standard_output, "time_step_ms") == "10" && json_value(run.standard_output, "step_delay_ms") == "9" &&
                json_value(run.standard_output, "paced_steps") == "20",
            "time_step_ms as written:\n" + run.standard_output);
    require(contains(run.standard_output,
                     "\"code\": \"coarse-time-step\",\n      \"file\": \"sim.yaml\",\n      \"line\": null,\n"
                     "      \"participant\": null,\n      \"network\": null,\n      \"message\": \"the launcher's controller steps "
                     "every 10 ms "
                     "(time_step_ms), but the ECUs' steps are as small as 1 ms: with a speed, the launcher keeps real time "
                     "only at the controller's steps. An ECU step that starts between two of them runs at the later one, up "
                     "to 9 ms after the real time it stands for (never before it), together with the other steps in between; "
                     "a device attached in real time gets the messages of those steps up to 9 ms late and in a burst. Write "
                     "time_step_ms: 1 (or leave time_step_ms out: with a speed, the default is that)\""),
            "the warning:\n" + run.standard_output);
    expect_code(launch(o, {"run", "--strict", "--speed", "2", "sim.yaml"}, dir), 1, "--strict fails on coarse-time-step");
    expect_code(launch(o, {"run", "--strict", "sim.yaml"}, dir), 0, "as fast as possible, a coarse step is nothing to warn of");
    // With the speed in the file too, check knows it before the run, and run reports it once (as
    // the check's); run --speed max has no speed to warn of.
    dir = one_ecu(o, "stop_at_ms: 200\ntime_step_ms: 10\nspeed: 2\n");
    step_file(dir, "1");
    const auto warnings = [](const std::string& text) {
        std::size_t n = 0;
        for (auto at = text.find("\"code\": \"coarse-time-step\""); at != std::string::npos;
             at = text.find("\"code\": \"coarse-time-step\"", at + 1))
        {
            ++n;
        }
        return n;
    };
    Captured checked = launch(o, {"check", "--json", "sim.yaml"}, dir);
    expect_code(checked, 0, "check of a file with speed and a coarse time_step_ms");
    require(warnings(checked.standard_output) == 1 &&
                contains(checked.standard_output, "but the ECUs' steps are as small as 1 ms: with a speed, the launcher keeps "
                                                  "real time only at the controller's steps."),
            "check warns of the coarse time_step_ms:\n" + checked.standard_output);
    expect_code(launch(o, {"check", "--strict", "sim.yaml"}, dir), 1, "check --strict fails on coarse-time-step");
    run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    expect_code(run, 0, "a run at the file's speed with a coarse time_step_ms");
    require(warnings(run.standard_output) == 1, "run reports coarse-time-step once:\n" + run.standard_output);
    run = launch(o, {"run", "--json", "--speed", "max", "sim.yaml"}, dir);
    expect_code(run, 0, "the same file as fast as possible");
    require(warnings(run.standard_output) == 0, "as fast as possible, nothing to warn of:\n" + run.standard_output);
    expect_code(launch(o, {"run", "--strict", "--speed", "max", "sim.yaml"}, dir), 0,
                "run --strict --speed max of a file with a speed and a coarse time_step_ms");
    // A participant that does not describe itself: with a speed, 1 ms to be safe.
    dir = scenario_dir(o,
                       "vehicle_sil_simulation: 1\nstop_at_ms: 200\nspeed: 4\nnetworks: []\nparticipants:\n"
                       "  - name: Engine\n    command: [" + ecu(o) + "]\n"
                       "  - name: Logger\n    command: [" + helper(o) + ", --pid-file, logger.pid]\n"
                       "    describe: false\n    required: false\n",
                       {{"Engine", ""}});
    step_file(dir, "10");
    run = launch(o, {"run", "--json", "sim.yaml"}, dir);
    const bool gone = all_gone(dir, {"logger.pid"}, 30s);
    expect_code(run, 0, "a participant that does not describe itself, at a speed");
    require(gone, "Logger has ended");
    require(json_value(run.standard_output, "time_step_ms") == "1" && json_value(run.standard_output, "ecu_step_ms") == "null" &&
                json_value(run.standard_output, "step_delay_ms") == "null" && !contains(run.standard_output, "coarse-time-step"),
            "an unknown step: 1 ms\n" + run.standard_output);
}

void pause_step(const Options& o)
{
    // No stop_at_ms: it runs until it is stopped. Started paused, stepped, resumed, paced, paused.
    const fs::path dir = one_ecu(o, "", "pid");
    Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
    live.wait_for({"\"event\":\"time\"", "\"sim_ms\":0,", "\"paused\":true,"});
    // A step goes the time given, in whole steps of the controller (10 ms), and stands still.
    live.write("{\"command\":\"step\",\"ms\":50}\n");
    live.wait_for({"\"command\":\"step\"", "\"accepted\":true,\"message\":null,\"ms\":50}"});
    live.wait_for({"\"event\":\"time\"", "\"sim_ms\":50,", "\"paused\":true,"});
    std::this_thread::sleep_for(300ms);
    require(events_with(live.lines(), {"\"sim_ms\":60,"}).empty(), "it stands still at 50 ms");
    live.write("{\"command\":\"step\",\"ms\":15}\n");
    live.wait_for({"\"command\":\"step\"", "\"ms\":20}"});
    live.wait_for({"\"event\":\"time\"", "\"sim_ms\":70,", "\"paused\":true,"});
    // Running: a step is refused; the speed changes.
    live.write("{\"command\":\"resume\"}\n");
    live.wait_for({"\"event\":\"time\"", "\"paused\":false,"});
    live.write("{\"command\":\"step\",\"ms\":10}\n{\"command\":\"speed\",\"value\":1}\n");
    live.wait_for({"\"command\":\"step\"", "\"accepted\":false"});
    live.wait_for({"\"event\":\"time\"", "\"speed\":1,", "\"paused\":false,"});
    live.write("{\"command\":\"pause\"}\n");
    const std::string paused = live.wait_for({"\"event\":\"time\"", "\"speed\":1,", "\"paused\":true,"});
    std::this_thread::sleep_for(300ms);
    live.write("{\"command\":\"stop\"}\n");
    const auto [status, lines] = live.finish();
    const bool gone = all_gone(dir, {"Engine.pid"}, 30s);
    require(status && status->code == 130, "stopped while paused: 130");
    require(gone, "no participant is left");
    std::string all;
    for (const auto& line : lines)
    {
        all += line + "\n";
    }
    // It stood still where it paused.
    const std::int64_t where = std::stoll(value_of(paused, "sim_ms"));
    for (const auto& time : events_with(lines, {"\"event\":\"time\""}))
    {
        require(std::stoll(value_of(time, "sim_ms")) <= where, "nothing moved after the pause:\n" + all);
    }
    // Each command comes before what it does; the refused step says why.
    const auto at = [&lines](const std::vector<std::string>& parts) {
        return std::find_if(lines.begin(), lines.end(),
                            [&](const std::string& l) { return std::all_of(parts.begin(), parts.end(), [&](const std::string& p) { return contains(l, p); }); }) -
               lines.begin();
    };
    require(at({"\"ms\":50}"}) < at({"\"sim_ms\":50,"}), "the step comes before the time it reaches:\n" + all);
    require(!events_with(lines, {"\"command\":\"step\"", "\"accepted\":false,\"message\":\"step works while the simulation is "
                                                         "paused: send {\\\"command\\\":\\\"pause\\\"} first (or run with "
                                                         "--start-paused)\"}"})
                 .empty(),
            "a step while it runs is refused:\n" + all);
    // The result: stood still for at least the waits above.
    const std::string& result = lines.back();
    require(std::stoll(value_of(result, "paused_ms")) >= 600 && contains(result, "\"speed\":1,"),
            "the result counts the time it stood still:\n" + result);
}

void paused_failures(const Options& o)
{
    // While it stands still, a participant whose process ends fails the run (as while it runs).
    {
        const fs::path dir = scenario_dir(o,
                                          "vehicle_sil_simulation: 1\n"
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Engine\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Brake\n"
                                          "    command: [" + ecu(o) + "]\n",
                                          {{"Engine", "exit-after-wall-ms 1500 3"}, {"Brake", "pid"}});
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "--timeout-ms", "60000", "sim.yaml"});
        const auto [status, lines] = live.finish();
        const bool gone = all_gone(dir, {"Brake.pid"}, 30s);
        require(status && status->code == 1, "a participant that fails while it stands still fails the run (1)");
        require(contains(lines.back(), "participant 'Engine': it failed while the simulation was running, and the launcher "
                                       "aborted the simulation: its process exited with code 3"),
                "the failure, not the timeout:\n" + lines.back());
        // The launcher learns of it in two ways, and either may be first: its look at the process
        // (process_ended), or the controller, which sees the connection go as the process ends, a
        // moment before the process can be waited for (left_simulation). The result names the
        // exit code in both (checked above).
        const auto failed = events_with(lines, {"\"name\":\"Engine\"", "\"state\":\"failed\""});
        require(failed.size() == 1 && (contains(failed[0], "\"reason\":\"process_ended\"") ||
                                       contains(failed[0], "\"reason\":\"left_simulation\"")),
                "Engine is failed at once, with one event" + (failed.empty() ? std::string() : ":\n" + failed[0]));
        require(gone, "no participant is left");
    }
    // --timeout-ms counts the time it stands still (wall-clock time).
    {
        const fs::path dir = one_ecu(o, "", "pid");
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "--timeout-ms", "1500", "sim.yaml"});
        const auto [status, lines] = live.finish();
        const bool gone = all_gone(dir, {"Engine.pid"}, 30s);
        require(status && status->code == 1 && contains(lines.back(), "\"code\":\"timeout\""),
                "a run that stands still past --timeout-ms is aborted:\n" + lines.back());
        require(gone, "no participant is left");
    }
    // start_timeout_ms is for the start: standing still at 0 longer than it is fine.
    {
        const fs::path dir = one_ecu(o, "stop_at_ms: 100\nstart_timeout_ms: 1000\n");
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--start-paused", "sim.yaml"});
        live.wait_for({"\"event\":\"time\"", "\"paused\":true,"});
        std::this_thread::sleep_for(2000ms);
        live.write("{\"command\":\"resume\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 0 && contains(lines.back(), "\"simulation\":\"reached_stop_at\""),
                "a start paused for longer than start_timeout_ms runs to its end:\n" + lines.back());
        require(std::stoll(value_of(lines.back(), "paused_ms")) >= 2000, "the time it stood still:\n" + lines.back());
    }
}

void time_usage(const Options& o)
{
    const fs::path dir = one_ecu(o, "stop_at_ms: 100\n");
    Captured run = launch(o, {"run", "--start-paused", "sim.yaml"}, dir);
    expect_code(run, 4, "--start-paused without --stdin-commands");
    require(contains(run.standard_error, "error: --start-paused needs --stdin-commands or --control: without commands nothing "
                                         "could make the simulation go on (resume, step)\n"),
            run.standard_error);
    // The options of the control server: without --control, and --control in a build without the
    // server (with it: tests/control_server/test_launcher.py).
    run = launch(o, {"run", "--control-credentials", "c.txt", "sim.yaml"}, dir);
    expect_code(run, 4, "--control-credentials without --control");
    require(contains(run.standard_error, "error: --control-credentials is for --control: without it there is no control "
                                         "server\n"),
            run.standard_error);
    run = launch(o, {"check", "--control", "0", "sim.yaml"}, dir);
    expect_code(run, 4, "check --control");
    require(contains(run.standard_error, "error: --control is for run: check starts no participant\n"), run.standard_error);
    const Captured version = launch(o, {"version", "--json"}, dir);
    if (!contains(version.standard_output, "\"control-server\""))
    {
        run = launch(o, {"run", "--control", "0", "sim.yaml"}, dir);
        expect_code(run, 4, "--control without the control server");
        require(contains(run.standard_error, "error: --control needs a vehicle_sil_launch built with the control server: "
                                             "configure with -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON (it needs gRPC) and build "
                                             "again"),
                run.standard_error);
    }
    run = launch(o, {"run", "--speed", "fast", "sim.yaml"}, dir);
    expect_code(run, 4, "--speed fast");
    require(contains(run.standard_error, "error: --speed must be a number of simulation seconds per wall-clock second from "
                                         "0.000001 to 1000000 (1: real time, 2: twice as fast) or max (as fast as possible), "
                                         "not 'fast'\n"),
            run.standard_error);
    for (const std::string option : {"--speed", "--start-paused"})
    {
        std::vector<std::string> args{"check", option};
        if (option == "--speed")
        {
            args.push_back("1");
        }
        args.push_back("sim.yaml");
        run = launch(o, args, dir);
        expect_code(run, 4, "check " + option);
        require(contains(run.standard_error, "error: " + option + " is for run: check starts no participant\n"), run.standard_error);
    }
    // In the file: an error of the check, nothing is started.
    const fs::path bad = one_ecu(o, "stop_at_ms: 105\ntime_step_ms: 10\nspeed: 0\n");
    run = launch(o, {"run", "sim.yaml"}, bad);
    expect_code(run, 1, "a speed of 0 and a step that does not divide stop_at_ms");
    require(contains(run.standard_output, "error: sim.yaml:3: stop_at_ms (105) is not a multiple of time_step_ms (10)") &&
                contains(run.standard_output, "error: sim.yaml:4: speed must be ") &&
                contains(run.standard_output, "nothing was started"),
            run.standard_output);
}

// --- run --scenario ---

/// The object "scenario" of the run's pretty JSON (the last one at the top level), as text.
std::string scenario_json(const std::string& json)
{
    const auto at = json.rfind("\n  \"scenario\": {");
    require(at != std::string::npos, "the result has no scenario:\n" + json);
    const auto end = json.find("\n  }", at + 1);
    require(end != std::string::npos, "the scenario object does not end:\n" + json);
    return json.substr(at + 1, end + 4 - at - 1) + "\n";
}

/// Two test ECUs with the variables for scenarios, and `keys` at the top of the configuration.
fs::path scenario_ecus(const Options& o, const std::string& keys, const std::string& scenario, const std::string& body = "")
{
    const fs::path dir = scenario_dir(o,
                                      "vehicle_sil_simulation: 1\n" + keys +
                                          "networks: []\n"
                                          "participants:\n"
                                          "  - name: Body\n"
                                          "    command: [" + ecu(o) + "]\n"
                                          "  - name: Door\n"
                                          "    command: [" + ecu(o) + "]\n",
                                      {{"Body", "scenario-variables " + body}, {"Door", "scenario-variables"}});
    write_file(dir / "s.yaml", scenario);
    return dir;
}

void scenario(const Options& o)
{
    const fs::path docs = o.docs.parent_path() / "scenario.md";
    const fs::path dir = o.example_dir;
    const std::vector<std::string> args{"run", "simulation.yaml", "--scenario", "scenarios/throttle_step.yaml"};
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
    write_file(dir / "throttle_mistakes.yaml", block_after(docs, "<!-- throttle_mistakes.yaml -->"));
    const Captured mistakes = launch(o, {"check", "simulation.yaml", "--scenario", "throttle_mistakes.yaml"}, dir);
    expect_code(mistakes, 1, "check --scenario throttle_mistakes.yaml");
    expect_text(mistakes.standard_output,
                block_after(docs, "<!-- vehicle_sil_launch check simulation.yaml --scenario throttle_mistakes.yaml -->"),
                "check simulation.yaml --scenario throttle_mistakes.yaml");
    write_file(dir / "throttle_response.yaml", block_after(docs, "<!-- throttle_response.yaml -->"));
    const Captured failed = launch(o, {"run", "simulation.yaml", "--scenario", "throttle_response.yaml"}, dir);
    expect_code(failed, 1, "run --scenario throttle_response.yaml");
    expect_text(failed.standard_output,
                block_after(docs, "<!-- vehicle_sil_launch run simulation.yaml --scenario throttle_response.yaml -->"),
                "run simulation.yaml --scenario throttle_response.yaml");
    const Captured failed_json = launch(o, {"run", "simulation.yaml", "--scenario", "throttle_response.yaml", "--json"}, dir);
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
    const fs::path slow = scenario_ecus(o, "stop_at_ms: 2000\n", file, "slow-ms 3");
    expect_text(run(slow, {}, "with a slow ECU"), fastest, "with a slow ECU");
    expect_text(run(slow, {"--speed", "8"}, "with a slow ECU at a speed"), fastest, "with a slow ECU at a speed");

    // With the time commands: paused in the middle and stepped on, the result is the same. (Slow
    // enough that the pause comes long before the end: 260 ms take 2.6 s.)
    {
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--scenario", "s.yaml", "--speed", "0.1", "sim.yaml"});
        live.wait_for({"\"event\":\"scenario_step\"", "\"line\":6,", "\"status\":\"passed\""});
        live.write("{\"command\":\"pause\"}\n");
        live.wait_for({"\"event\":\"time\"", "\"paused\":true,"});
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

void scenario_disconnect(const Options& o)
{
    // Engine off its bus at 300 ms: its last speed is the one of 200 ms, so the dashboard says it is
    // lost after 500 ms (the sample of 510 ms). Back at 520 ms: the speed of 600 ms arrives.
    const auto run = [&](std::vector<std::string> more, const std::string& what) {
        std::vector<std::string> args{"run", "simulation.yaml", "--scenario", "scenarios/engine_disconnect.yaml", "--json"};
        args.insert(args.end(), more.begin(), more.end());
        const Captured r = launch(o, args, o.example_dir);
        expect_code(r, 0, what);
        return scenario_json(lf(r.standard_output));
    };
    const std::string fastest = run({}, "as fast as possible");
    require(contains(fastest, "\"status\": \"passed\"") && contains(fastest, "\"over_at_ms\": 615,"),
            "the scenario passes, and is over at 615 ms:\n" + fastest);
    for (const auto& [line, at] : std::vector<std::pair<int, int>>{{9, 300}, {11, 510}, {13, 520}, {14, 610}})
    {
        const std::string expected = "\"at_ms\": " + std::to_string(at) + ",";
        const auto step = fastest.find("\"line\": " + std::to_string(line) + ",");
        const auto decided = step == std::string::npos ? step : fastest.find("\"at_ms\": ", step);
        require(decided != std::string::npos && fastest.compare(decided, expected.size(), expected) == 0,
                "line " + std::to_string(line) + " is decided at " + std::to_string(at) + " ms:\n" + fastest);
    }
    expect_text(run({"--speed", "4"}, "at four times real time"), fastest, "at a speed");
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
                                  "(100 ms after 0 ms): Body.level eq 6, but it was 5 (its sample of 100 ms)\n") &&
                    contains(out, "\nfailed: 1 error, 0 warnings, 0 notes\n"),
                "the failure in the text:\n" + out);
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
        // stop_at_ms comes first: the step that could not be decided fails, and says what to do.
        const fs::path dir = scenario_ecus(o, "stop_at_ms: 100\n",
                                           head + "  - at_ms: 50\n    set: { Body.level: 5 }\n"
                                                  "  - at_ms: 90\n    expect: { Body.level: 5 }\n"
                                                  "  - wait_until: { Body.level: 6 }\n    timeout_ms: 1000\n");
        const Captured run = launch(o, {"run", "sim.yaml", "--scenario", "s.yaml"}, dir);
        expect_code(run, 1, "stop_at_ms before the end of the scenario");
        const std::string out = lf(run.standard_output);
        require(contains(out, "run: the simulation ran to stop_at_ms (100 ms) and ended\n") &&
                    contains(out, "scenario s.yaml: FAILED, 2 of 3 steps passed\n") &&
                    contains(out, "error: s.yaml:7: wait_until Body.level eq 6 (timeout 1000 ms): the simulation stopped at 100 ms "
                                  "(stop_at_ms) before this step was decided (it began at 90 ms): there was not enough time for the "
                                  "scenario; raise stop_at_ms, or leave it out (the run then ends with the scenario)\n"),
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
                    contains(out, "scenario s.yaml: incomplete, 1 of 2 steps passed\n") && contains(out, "participant-failed") == false &&
                    contains(out, "participant 'Body': it failed while the simulation was running") &&
                    contains(out, "error: s.yaml:4: the scenario did not get to its end: the simulation ended at ") &&
                    contains(out, " when 1 of 2 steps had passed and the step at line 4 (wait_until Door.level eq 6 (timeout 5000 "
                                  "ms)) was not decided yet; the other findings and the lines above say why the simulation ended\n"),
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
        // A stop command in the middle: stopped in order (130), the scenario is incomplete.
        const fs::path dir = scenario_ecus(o, "",
                                           head + "  - set: { Body.level: 5 }\n"
                                                  "  - wait_until: { Door.level: 6 }\n    timeout_ms: 600000\n");
        Live live(o, dir, {"run", "--events", "--stdin-commands", "--scenario", "s.yaml", "--speed", "1", "sim.yaml"});
        live.wait_for({"\"event\":\"scenario_step\"", "\"line\":4,", "\"status\":\"running\""});
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 130, "a stop command during a scenario: 130");
        const std::string& result = lines.back();
        require(contains(result, "\"result\":\"interrupted\"") && contains(result, "\"simulation\":\"interrupted\"") &&
                    contains(result, "\"code\":\"scenario-incomplete\"") &&
                    contains(result, "\"name\":null,\"status\":\"incomplete\",\"over_at_ms\":null,") &&
                    contains(result, "\"status\":\"unfinished\""),
                "the stopped scenario:\n" + result);
        log("CHECK a stop command during a scenario -> ok");
    }
    {
        // An ECU hangs in a step while the scenario writes to it: --timeout-ms ends the run, and
        // the scenario's thread with it.
        const fs::path dir = scenario_ecus(o, "stop_timeout_ms: 500\n",
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
                                   "(vehicle_sil_launch check --json lists the variables)\n"
                                   "error: s.yaml:5: Body.gain is a parameter: it can be written only before the first step, that is "
                                   "with set in a step at 0 (at_ms: 0, or the first steps without a time), not at 10 ms\n"
                                   "error: s.yaml:5: Door.ticks is an output of its ECU: it can be read (expect, wait_until), not "
                                   "written; write one of the ECU's inputs instead\n";
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
                         "take it in (--enable Door), or run a scenario that does not use it\n"),
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
                                             "--stdin-commands if needed\n"),
                run.standard_error);
        run = launch(o, {"run", "sim.yaml", "--scenario"}, dir);
        expect_code(run, 4, "--scenario without a file");
        const Captured version = launch(o, {"version", "--json"}, dir);
        require(contains(version.standard_output, "\"scenario\""), "version --json lists the feature scenario");
        // With --control: a build with the control server serves it during the scenario (its
        // clients only read; tests/control_server/test_launcher.py), and says so in its features.
        const bool server = contains(version.standard_output, "\"control-server\"");
        require(server == contains(version.standard_output, "\"scenario-control\""),
                "version --json lists scenario-control exactly with control-server: " + version.standard_output);
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

// --- participants started by hand ---

/// Where and how the launcher's lines on `err` say to start `name` by hand: (the directory, the
/// command line, the two lines); empty if they are not there yet.
struct HandCommand
{
    std::string directory;
    std::string line;
    std::string text;
};

HandCommand hand_command(const std::string& err, const std::string& name)
{
    const auto at = err.find("[vehicle_sil_launch] not starting " + name + ": ");
    const auto end = at == std::string::npos ? at : err.find('\n', at);
    const auto next = end == std::string::npos ? end : err.find('\n', end + 1);
    if (next == std::string::npos)
    {
        return {};
    }
    const std::string first = err.substr(at, end - at);
    const std::string second = err.substr(end + 1, next - end - 1);
    const std::string in = ". In the directory ";
    const std::string tail = ", run:";
    const std::string tag = "[vehicle_sil_launch]   ";
    const auto from = first.rfind(in);
    require(from != std::string::npos && first.size() > tail.size() &&
                first.compare(first.size() - tail.size(), tail.size(), tail) == 0 && second.rfind(tag, 0) == 0,
            "the lines about " + name + " say where and how to start it:\n" + first + "\n" + second);
    return {first.substr(from + in.size(), first.size() - tail.size() - from - in.size()), second.substr(tag.size()),
            first + "\n" + second + "\n"};
}

/// A participant started from the line the launcher wrote, as a person who copies it would: in
/// its directory; on POSIX through sh, on Windows split as the C runtimes split a command line.
class ByHand
{
public:
    ByHand(const std::string& line, const std::string& directory)
    {
        vehicle_sil::process::ProcessOptions options;
        options.working_directory = vehicle_sil::process::path_from_utf8(directory);
        options.on_line = [this](vehicle_sil::process::Stream, std::string_view text) {
            const std::lock_guard lock(mutex_);
            output_ += std::string(text) + "\n";
        };
        log("starting by hand in " + directory + ": " + line);
#ifdef _WIN32
        const int size = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), size);
        int count = 0;
        wchar_t** words = CommandLineToArgvW(wide.c_str(), &count);
        require(words != nullptr && count > 0, "the command line cannot be split: " + line);
        std::vector<std::string> args;
        for (int i = 0; i < count; ++i)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, words[i], -1, nullptr, 0, nullptr, nullptr);
            std::string word(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, words[i], -1, word.data(), n, nullptr, nullptr);
            args.push_back(std::move(word));
        }
        LocalFree(words);
        const fs::path executable = vehicle_sil::process::path_from_utf8(args.front());
        args.erase(args.begin());
        process_ = std::make_unique<vehicle_sil::process::Process>(executable, args, std::move(options));
#else
        process_ = std::make_unique<vehicle_sil::process::Process>("/bin/sh", std::vector<std::string>{"-c", line},
                                                                   std::move(options));
#endif
    }
    ByHand(const ByHand&) = delete;
    ByHand& operator=(const ByHand&) = delete;
    ~ByHand() { process_.reset(); } // ends it if it still runs

    /// Stops every thread of it where it is, as a debugger's pause does (POSIX SIGSTOP to its process
    /// group, Windows each of its threads suspended); thaw() lets it go on.
    void freeze() { suspend(true); }
    void thaw() { suspend(false); }

    /// Waits for its end (it ends by itself): its status.
    std::optional<vehicle_sil::process::ExitStatus> wait(std::chrono::milliseconds timeout)
    {
        const auto status = process_->wait_for(timeout);
        process_->finish_output(10s);
        const std::lock_guard lock(mutex_);
        log("output of the participant started by hand:\n" + output_);
        return status;
    }

private:
    void suspend(bool stop)
    {
        const long pid = process_->id();
#ifdef _WIN32
        // Started without a shell: the process is the ECU.
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        require(snapshot != INVALID_HANDLE_VALUE, "cannot list the threads");
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != static_cast<DWORD>(pid))
            {
                continue;
            }
            if (const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID); thread != nullptr)
            {
                static_cast<void>(stop ? SuspendThread(thread) : ResumeThread(thread));
                CloseHandle(thread);
            }
        }
        CloseHandle(snapshot);
#else
        // sh and the ECU it started: the process group the process library made.
        require(::kill(-static_cast<pid_t>(pid), stop ? SIGSTOP : SIGCONT) == 0, "cannot stop or continue the process");
#endif
        log(std::string(stop ? "froze" : "thawed") + " the participant started by hand");
    }

    std::mutex mutex_;
    std::string output_;
    std::unique_ptr<vehicle_sil::process::Process> process_;
};

/// A TCP port on 127.0.0.1 that is free now (for a registry_uri of a test).
int free_port()
{
#ifdef _WIN32
    WSADATA data;
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(s != INVALID_SOCKET, "cannot make a socket");
#else
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    require(s >= 0, "cannot make a socket");
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    const bool bound = ::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                       ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0;
#ifdef _WIN32
    closesocket(s);
    WSACleanup();
#else
    ::close(s);
#endif
    require(bound, "cannot find a free port");
    return ntohs(address.sin_port);
}

/// A string value of an event line as JSON writes it (the escapes of a path undone).
std::string json_text(const std::string& line, const std::string& key)
{
    const std::string raw = value_of(line, key);
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        out += raw[i] == '\\' && i + 1 < raw.size() ? raw[++i] : raw[i];
    }
    return out;
}

std::string joined_lines(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& line : lines)
    {
        out += line + "\n";
    }
    return out;
}

/// A directory of its own for a part of the scenario manual, with sim.yaml and behaviour files.
fs::path hand_dir(const Options& o, const std::string& part, const std::string& config,
                  const std::vector<std::pair<std::string, std::string>>& behaviours)
{
    const fs::path dir = o.work_dir / part;
    fs::create_directories(dir);
    write_file(dir / "sim.yaml", config);
    for (const auto& [name, behaviour] : behaviours)
    {
        write_file(dir / (name + ".behaviour"), behaviour + "\n");
    }
    return dir;
}

/// Each line of the run's standard output, for a message.
std::string out_of(const std::vector<std::string>& lines) { return joined_lines(lines); }

/// The configuration of a part of the manual scenarios: Auto, started by the launcher, Hand,
/// started by hand, and `more`, at a free port of its own (not 8500, the port a run with a
/// participant started by hand takes by default, which may be taken here).
std::string hand_config(const Options& o, const std::string& keys, const std::string& more)
{
    return "vehicle_sil_simulation: 1\n" + keys + "registry_uri: silkit://127.0.0.1:" + std::to_string(free_port()) +
           "\n"
           "networks: []\n"
           "participants:\n"
           "  - name: Auto\n"
           "    command: [" + ecu(o) + "]\n"
           "  - name: Hand\n"
           "    command: [" + ecu(o) + "]\n"
           "    manual: true\n" + more;
}

/// A third participant started by hand, Late, for hand_config().
std::string late_participant(const Options& o) { return "  - name: Late\n    command: [" + ecu(o) + "]\n    manual: true\n"; }

void manual_late(const Options& o)
{
    {
        // Started long after start_timeout_ms (8 s for Auto to join is generous), and standing
        // still in its step at 100 ms for 1.5 s, as in a debugger: the run passes.
        const fs::path dir = hand_dir(o, "late", hand_config(o, "stop_at_ms: 300\nstart_timeout_ms: 8000\n", ""),
                                      {{"Auto", "pid"}, {"Hand", "slow-at-ms 100 1500"}});
        Live live(o, dir, {"run", "sim.yaml"});
        const std::string err = live.wait_for_error(
            [](const std::string& text) { return contains(text, "[vehicle_sil_launch] waiting for Hand to join (manual): "); });
        const HandCommand hand = hand_command(err, "Hand");
        require(!hand.line.empty() && contains(err, "waiting for Hand to join (manual): start it with the command above"),
                "the launcher waits for Hand:\n" + err);
        // The ECUs it starts wait for the start as long as it takes.
        require(contains(err, "--name Auto --registry-uri silkit://127.0.0.1:") &&
                    contains(err, " --start-timeout-ms 1000000000000\n") &&
                    contains(hand.line, " --start-timeout-ms 1000000000000"),
                "every ECU gets --start-timeout-ms 1000000000000:\n" + err);
        std::this_thread::sleep_for(8500ms);
        ByHand byHand(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "the run with Hand started late ends with 0:\n" + out_of(out));
        expect_text(run_part(out_of(out)),
                    "run: the simulation ran to stop_at_ms (300 ms) and ended\n"
                    "\n"
                    "participant  required  ended\n"
                    "Auto         yes       exit code 0\n"
                    "Hand         yes       started by you (manual)\n"
                    "\n"
                    "passed: 0 errors, 0 warnings, 1 note\n",
                    "a participant started by hand after start_timeout_ms");
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 0, "Hand ends by itself with 0");
        require(all_gone(dir, {"Auto.pid"}, 30s), "Auto has ended");
    }
}

void manual_fails(const Options& o)
{
    {
        // It fails while the simulation runs: the launcher sees it leave the simulation.
        const fs::path dir = hand_dir(o, "fails", hand_config(o, "stop_at_ms: 1000\n", ""), {{"Auto", ""}, {"Hand", "exit-at-ms 100 3"}});
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        const std::string event = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        require(contains(event, "\"command\":[\"") && contains(event, "\"--name\",\"Hand\",\"--registry-uri\","),
                "the event manual has the command:\n" + event);
        ByHand byHand(json_text(event, "command_line"), json_text(event, "working_directory"));
        const auto [status, lines] = live.finish();
        require(status && status->code == 1, "the run ends with 1 when Hand fails");
        require(events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"joined\""}).size() == 1, "Hand joined (the event)");
        require(events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"failed\"", "\"reason\":\"left_simulation\""}).size() == 1,
                "Hand failed: it left the simulation (the event)");
        require(events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"started\""}).empty() &&
                    events_with(lines, {"\"name\":\"Hand\"", "\"state\":\"ended\""}).empty(),
                "no process of Hand in the events");
        const std::string& result = lines.back();
        require(contains(result, "{\"name\":\"Hand\",\"required\":true,\"enabled\":true,\"manual\":true,\"ended\":\"not_started\","
                                 "\"exit_code\":null,"),
                "the result says that Hand was started by hand:\n" + result);
        require(contains(result, "\"code\":\"participant-failed\"") &&
                    contains(result, "participant 'Hand': it failed while the simulation was running") &&
                    contains(result, "you started it (manual), so what it wrote is where you started it"),
                "the finding about Hand:\n" + result);
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 3, "Hand ended with 3, as its behaviour says");
    }
}

void manual_stop(const Options& o)
{
    {
        // A stop before the start (Late never comes): Hand, which has joined, is told to end.
        const fs::path dir = hand_dir(o, "stop", hand_config(o, "", late_participant(o)), {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
        const std::string event = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        ByHand byHand(json_text(event, "command_line"), json_text(event, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish();
        require(status && status->code == 130, "the run stopped before the start ends with 130");
        const auto ended = byHand.wait(30s);
        require(ended && ended->code == 3, "Hand, which waited for the start, ends as a start that failed (3)");
        require(contains(lines.back(), "{\"name\":\"Late\",\"required\":true,\"enabled\":true,\"manual\":true,\"ended\":\"not_started\","),
                "Late in the result:\n" + lines.back());
    }
}

void manual_frozen(const Options& o)
{
    {
        // Frozen (a debugger's pause) as soon as it has joined: the start has no time limit, and
        // a stop command ends the run all the same. Late is the last to join, so the start goes on
        // at once; whether it is frozen before its first step or the simulation runs without it
        // stepping depends on how far it got (logged), and both must end on the stop.
        // No stop_at_ms: if Late got to its first steps before it was frozen, the simulation must not
        // end before the stop command.
        const fs::path dir = hand_dir(o, "frozen", hand_config(o, "stop_timeout_ms: 3000\n", late_participant(o)),
                                      {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"run", "--events", "--stdin-commands", "sim.yaml"});
        const std::string hand = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        const std::string lateEvent = live.wait_for({"\"name\":\"Late\"", "\"state\":\"manual\""});
        ByHand byHand(json_text(hand, "command_line"), json_text(hand, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        ByHand lateByHand(json_text(lateEvent, "command_line"), json_text(lateEvent, "working_directory"));
        live.wait_for({"\"name\":\"Late\"", "\"state\":\"joined\""});
        lateByHand.freeze();
        std::this_thread::sleep_for(2000ms);
        const bool running = !events_with(live.lines(), {"\"phase\":\"running\""}).empty();
        log(std::string("RESULT Late frozen as it joined: ") +
            (running ? "the simulation runs (Late got to its first step)" : "the start waits for Late"));
        // On Windows, with a participant frozen while the simulation runs, the launcher sometimes
        // does not end after the stop: destroying its controller's SIL Kit participant does not
        // return (a known limit of the product; docs/simulation-config.md says to let the
        // participant go on first). There, in that branch only, Late goes on before the stop. A
        // stop with Late still frozen is checked on Windows before the start, on POSIX in both.
#ifdef _WIN32
        const bool thawFirst = running;
#else
        const bool thawFirst = false;
#endif
        if (thawFirst)
        {
            lateByHand.thaw();
        }
        live.write("{\"command\":\"stop\"}\n");
        const auto [status, lines] = live.finish_unchecked(60s);
        if (!thawFirst)
        {
            lateByHand.thaw();
        }
        require(status && status->code == 130,
                "a stop ends a run that waits for a frozen participant (130), not " +
                    (status ? std::to_string(status->code) : std::string("still running")) + ":\n" + out_of(lines));
        check_events(lines);
        require(contains(lines.back(), "\"result\":\"interrupted\"") && contains(lines.back(), "\"interrupted_by\":\"stop command\""),
                "the run was stopped by the command:\n" + lines.back());
        // Hand ends when it was told to (the start was aborted); if the simulation ran, it may wait
        // in its step for Late's for ever (the launcher has gone): it is ended with the test.
        if (!running)
        {
            require(byHand.wait(30s).has_value(), "Hand, which waited for the start, has ended:\n" + out_of(lines));
        }
    }
}

void manual_thawed(const Options& o)
{
    {
        // Frozen for 3 s after it joined, before the first step (Late cannot join while it is), then
        // going on: the run passes. Whether its variables can be used is logged (its control link
        // at the start).
        const fs::path dir = hand_dir(o, "thawed", hand_config(o, "stop_at_ms: 200\n", late_participant(o)), {{"Auto", ""}, {"Hand", "variables"}, {"Late", ""}});
        Live live(o, dir, {"run", "--events", "sim.yaml"});
        const std::string hand = live.wait_for({"\"name\":\"Hand\"", "\"state\":\"manual\""});
        const std::string lateEvent = live.wait_for({"\"name\":\"Late\"", "\"state\":\"manual\""});
        ByHand byHand(json_text(hand, "command_line"), json_text(hand, "working_directory"));
        live.wait_for({"\"name\":\"Hand\"", "\"state\":\"joined\""});
        byHand.freeze();
        ByHand lateByHand(json_text(lateEvent, "command_line"), json_text(lateEvent, "working_directory"));
        std::this_thread::sleep_for(3000ms);
        byHand.thaw();
        const auto [status, lines] = live.finish();
        require(status && status->code == 0, "a participant frozen before the first step and thawed: the run passes:\n" +
                                                 lines.back());
        const auto links = events_with(lines, {"\"event\":\"control_link\"", "\"name\":\"Hand\""});
        require(links.size() == 1, "one control_link of Hand");
        log("RESULT the control link of Hand, frozen for 3 s before the start: " + links.front());
        require(byHand.wait(30s).has_value() && lateByHand.wait(30s).has_value(), "Hand and Late have ended");
    }
}

void manual_again(const Options& o)
{
    {
        // Ended after it joined, before the start (to start it again): the start fails, and the
        // finding says to start the launcher again as well.
        const fs::path dir = hand_dir(o, "again", hand_config(o, "", late_participant(o)), {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"run", "sim.yaml"});
        const std::string err = live.wait_for_error([](const std::string& text) {
            return !hand_command(text, "Hand").line.empty() && !hand_command(text, "Late").line.empty();
        });
        auto byHand = std::make_unique<ByHand>(hand_command(err, "Hand").line, hand_command(err, "Hand").directory);
        static_cast<void>(live.wait_for_error([](const std::string& text) {
            return contains(text, "[vehicle_sil_launch] waiting for Late to join (manual)");
        }));
        byHand.reset(); // ends it
        const HandCommand lateCommand = hand_command(err, "Late");
        ByHand lateByHand(lateCommand.line, lateCommand.directory);
        const auto [status, out] = live.finish_unchecked(60s);
        require(status && status->code == 1 &&
                    contains(out_of(out), "error: sim.yaml: the simulation did not start: ") &&
                    contains(out_of(out), "participant 'Hand' left the simulation before it started") &&
                    contains(out_of(out), "you started it yourself (manual), and it cannot join this run again: to start "
                                          "it again, start vehicle_sil_launch again as well"),
                "a participant started by hand that ended before the start:\n" + out_of(out));
        require(lateByHand.wait(30s).has_value(), "Late, which waited for the start, has ended");
    }
}

void manual_timeout(const Options& o)
{
    {
        // --timeout-ms counts from when the launcher began to wait: Late never comes.
        const fs::path dir = hand_dir(o, "timeout", hand_config(o, "", late_participant(o)), {{"Auto", ""}, {"Hand", ""}, {"Late", ""}});
        Live live(o, dir, {"run", "--timeout-ms", "3000", "sim.yaml"});
        const std::string err = live.wait_for_error([](const std::string& text) { return !hand_command(text, "Hand").line.empty(); });
        const HandCommand hand = hand_command(err, "Hand");
        ByHand byHand(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked(60s);
        require(status && status->code == 1, "--timeout-ms ends a run that waits for a participant started by hand:\n" +
                                                 out_of(out) + "--- stderr ---\n" + live.err());
        require(contains(out_of(out), "error: sim.yaml: the simulation did not start within 3000 ms of wall-clock time "
                                      "(--timeout-ms"),
                "the timeout finding:\n" + out_of(out));
        require(contains(out_of(out), "Late (manual) has not joined"), "the finding names Late:\n" + out_of(out));
        require(byHand.wait(30s).has_value(), "Hand, which waited for the start, has ended");
    }
}

void manual(const Options& o)
{
    {
        // The example of the docs: Engine by hand, Dashboard by the launcher.
        Live live(o, o.example_dir, {"run", "simulation.yaml", "--manual", "Engine"});
        HandCommand hand;
        const std::string err = live.wait_for_error([&](const std::string& text) {
            hand = hand_command(text, "Engine");
            return !hand.line.empty();
        });
        if (contains(err, "the registry cannot listen at silkit://127.0.0.1:8500"))
        {
            throw Skip("another program listens at 127.0.0.1:8500: the example of the docs was not run (the other "
                       "manual_* scenarios run at ports of their own)");
        }
        require(!hand.line.empty(), "no lines about Engine:\n" + err);
        require(fs::equivalent(vehicle_sil::process::path_from_utf8(hand.directory), o.example_dir),
                "the directory is the example's: " + hand.directory);
        // As the docs show them: the directory as <dir>, paths with '/' and without ".exe".
        std::string text = hand.text;
        for (auto at = text.find(hand.directory); at != std::string::npos; at = text.find(hand.directory, at))
        {
            text.replace(at, hand.directory.size(), "<dir>");
        }
#ifdef _WIN32
        std::replace(text.begin(), text.end(), '\\', '/');
        for (auto at = text.find(".exe"); at != std::string::npos; at = text.find(".exe", at))
        {
            text.erase(at, 4);
        }
#endif
        expect_text(text, block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --manual Engine: the lines about Engine -->"),
                    "the lines about Engine started by hand");
        ByHand engine(hand.line, hand.directory);
        const auto [status, out] = live.finish_unchecked();
        require(status && status->code == 0, "run simulation.yaml --manual Engine ends with 0:\n" + out_of(out));
        expect_text(out_of(out), block_after(o.docs, "<!-- vehicle_sil_launch run simulation.yaml --manual Engine -->"),
                    "run simulation.yaml --manual Engine");
        const auto ended = engine.wait(30s);
        require(ended && ended->code == 0 && !ended->signal, "Engine, started by hand, ends by itself with 0");
    }
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
            {"events_stderr", events_stderr}, {"realtime", realtime}, {"on_time", on_time}, {"late", late}, {"stall", stall},
            {"speed_change", speed_change}, {"step_width", step_width}, {"pause_step", pause_step},
            {"paused_failures", paused_failures}, {"time_usage", time_usage}, {"control", control},
            {"scenario", scenario}, {"scenario_same", scenario_same}, {"scenario_disconnect", scenario_disconnect},
            {"scenario_failures", scenario_failures}, {"manual", manual},
            {"manual_late", manual_late}, {"manual_fails", manual_fails}, {"manual_stop", manual_stop},
            {"manual_frozen", manual_frozen}, {"manual_thawed", manual_thawed}, {"manual_again", manual_again},
            {"manual_timeout", manual_timeout}};
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
