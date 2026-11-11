// SPDX-License-Identifier: MIT
//
// ECU executables made with ecu_main() (examples/ecu_main: Engine and Dashboard), run as
// processes:
//
//   describe  --describe writes the JSON that docs/ecu-executable.md shows (the block after the
//             line "<!-- vehicle_sil_engine_ecu --describe -->"), byte for byte (also on Windows:
//             no "\r") but for the version in "vehicle_sil"; --name renames
//   local     --local --run-for-ms runs one ECU alone (the dashboard receives nothing)
//   silkit    a sil-kit-registry, vehicle_sil_sim_controller and both ECUs with --registry-uri:
//             the dashboard shows the engine's speed; every process exits 0
//   usage     without arguments: the usage and exit code 4
//   signal    a stop request (POSIX SIGTERM, Windows Ctrl+Break) to an ECU with --registry-uri:
//             before the start it ends at once; while the simulation runs it stops the simulation
//             in order (the controller of this test sees it shut down) and ends; exit code 130
#include "launcher_support.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <filesystem>
#include <stdexcept>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;

namespace {

struct Options
{
    fs::path registry;
    fs::path controller;
    fs::path engine;
    fs::path dashboard;
    fs::path docs;
    fs::path work_dir;
    std::string scenario;
    std::chrono::milliseconds timeout{120000};
};

Options parse(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options o;
    o.registry = args.required("registry");
    o.controller = args.required("controller");
    o.engine = args.required("engine");
    o.dashboard = args.required("dashboard");
    o.docs = args.required("docs");
    o.work_dir = args.required("work-dir");
    o.scenario = args.required("scenario");
    o.timeout = args.millis("timeout-ms", o.timeout);
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

/// `text` without '\r': what the ECUs print in text mode has "\r\n" on Windows, and so may a
/// checked-out Markdown file.
std::string lf(std::string text)
{
    std::erase(text, '\r');
    return text;
}

/// Runs `exe args` to its end; returns its exit code and what it wrote (stdout and stderr).
std::pair<int, std::string> run(const Options& o, const fs::path& exe, const std::vector<std::string>& args,
                                const std::string& log_name)
{
    const fs::path log_file = o.work_dir / (log_name + ".log");
    ChildProcess process(exe, args, log_file);
    const auto code = wait_or_kill(process, std::chrono::steady_clock::now() + o.timeout);
    require(code.has_value(), log_name + " did not exit in time");
    return {*code, read_file(log_file)};
}

/// The fenced block after the line `marker` in the Markdown file `path`, without the fences.
std::string block_after(const fs::path& path, const std::string& marker)
{
    const std::string text = lf(read_file(path));
    const auto at = text.find(marker + "\n");
    require(at != std::string::npos, path.string() + " has no line " + marker);
    const auto open = text.find('\n', text.find("```", at)) + 1;
    const auto close = text.find("\n```", open);
    require(close != std::string::npos, path.string() + ": the block after " + marker + " does not end");
    return text.substr(open, close - open + 1);
}

/// `json` with the value of its "vehicle_sil" key (the project's version) replaced by
/// "<version>": the documented example does not change with every version.
std::string without_version(const std::string& json)
{
    const std::string key = "\n  \"vehicle_sil\": \"";
    const auto at = json.find(key);
    require(at != std::string::npos, "no \"vehicle_sil\" key in:\n" + json);
    const auto from = at + key.size();
    const auto to = json.find("\",\n", from);
    require(to != std::string::npos, "the \"vehicle_sil\" line does not end in:\n" + json);
    return json.substr(0, from) + "<version>" + json.substr(to);
}

void describe(const Options& o)
{
    const auto [code, written] = run(o, o.engine, {"--describe"}, "engine-describe");
    require(code == 0, "engine --describe exited " + std::to_string(code) + ":\n" + written);
    // Byte for byte, except the version (also no "\r" on Windows).
    const std::string out = without_version(written);
    const std::string documented = without_version(block_after(o.docs, "<!-- vehicle_sil_engine_ecu --describe -->"));
    if (out != documented)
    {
        throw std::runtime_error("engine --describe differs from " + o.docs.string() + "\n--- written ---\n" + out +
                                 "--- documented ---\n" + documented);
    }
    log("CHECK engine --describe is the documented JSON -> ok");

    const auto [renamedCode, renamed] = run(o, o.dashboard, {"--describe", "--name", "Dashboard2"}, "dashboard-describe");
    require(renamedCode == 0, "dashboard --describe --name exited " + std::to_string(renamedCode) + ":\n" + renamed);
    require(renamed.rfind("{\n  \"vehicle_sil_ecu\": 1,\n", 0) == 0 &&
                contains(renamed, "\n  \"name\": \"Dashboard2\",\n  \"step_ns\": 5000000,\n"),
            "dashboard --describe --name Dashboard2 wrote:\n" + renamed);
    require(contains(renamed, "\"name\": \"dashboard.engine_speed\""), "the dashboard's variable is missing:\n" + renamed);
    log("CHECK dashboard --describe --name Dashboard2 -> ok");
}

void local(const Options& o)
{
    const auto [code, printed] = run(o, o.dashboard, {"--local", "--run-for-ms", "1000"}, "dashboard-local");
    const std::string out = lf(printed);
    require(code == 0, "dashboard --local exited " + std::to_string(code) + ":\n" + out);
    require(out == "t=0 ms engine speed 0 rpm (0 messages)\nt=500 ms engine speed 0 rpm (0 messages)\n",
            "dashboard --local wrote:\n" + out);
    const auto [engineCode, engineOut] = run(o, o.engine, {"--local", "--run-for-ms", "1000"}, "engine-local");
    require(engineCode == 0 && engineOut.empty(), "engine --local exited " + std::to_string(engineCode) + ":\n" + engineOut);
    log("CHECK both ECUs run alone with --local -> ok");
}

void silkit(const Options& o)
{
    RegistryProcess registry(o.registry, o.work_dir);
    const std::string& uri = registry.uri();
    const auto deadline = std::chrono::steady_clock::now() + o.timeout;
    ChildProcess controller(o.controller,
                            {"--registry-uri", uri, "--required", "Engine,Dashboard", "--stop-at-ms", "1000", "--timeout-ms",
                             std::to_string(o.timeout.count())},
                            o.work_dir / "controller.log");
    ChildProcess engine(o.engine, {"--registry-uri", uri}, o.work_dir / "engine.log");
    ChildProcess dashboard(o.dashboard, {"--registry-uri", uri}, o.work_dir / "dashboard.log");
    const auto controllerCode = wait_or_kill(controller, deadline);
    const auto engineCode = wait_or_kill(engine, deadline);
    const auto dashboardCode = wait_or_kill(dashboard, deadline);
    if (controllerCode != 0 || engineCode != 0 || dashboardCode != 0)
    {
        dump_log(o.work_dir / "controller.log");
        dump_log(o.work_dir / "engine.log");
        dump_log(o.work_dir / "dashboard.log");
        throw std::runtime_error("exit codes: controller " + exit_code_text(controllerCode) + ", engine " +
                                 exit_code_text(engineCode) + ", dashboard " + exit_code_text(dashboardCode));
    }
    // SIL Kit may log to the console; the engine itself writes nothing, and no error line.
    const std::string engineOut = read_file(o.work_dir / "engine.log");
    require(!contains(engineOut, "error"), "the engine wrote:\n" + engineOut);
    // Every 100 ms from 0 the engine sends its speed, which reaches 1200 rpm at 20 % throttle; the
    // dashboard prints at 0 and 500 ms what it received in the steps before. Its step at 1000 ms
    // races with the controller's stop at 1000 ms, so that line may be missing.
    const std::string shown = lf(read_file(o.work_dir / "dashboard.log"));
    const std::string first = "t=0 ms engine speed 0 rpm (0 messages)\n"
                              "t=500 ms engine speed 1200 rpm (5 messages)\n";
    require(shown == first || shown == first + "t=1000 ms engine speed 1200 rpm (10 messages)\n",
            "the dashboard showed:\n" + shown);
    log("CHECK the dashboard shows the engine's speed over SIL Kit -> ok");
}

void usage(const Options& o)
{
    const auto [code, written] = run(o, o.engine, {}, "engine-usage");
    const std::string out = lf(written);
    require(code == 4, "engine without arguments exited " + std::to_string(code) + ":\n" + out);
    require(out.rfind("error: Engine: no arguments: give --describe, --registry-uri <uri> or --local --run-for-ms <ms>\n"
                      "usage: vehicle_sil_engine_ecu",
                      0) == 0,
            "engine without arguments wrote:\n" + out);
    log("CHECK engine without arguments: usage and exit code 4 -> ok");
}

/// The engine with --registry-uri `uri` in a process group of its own, its lines collected.
struct Engine
{
    std::mutex mutex;
    std::string lines;
    std::unique_ptr<vehicle_sil::process::Process> process;

    Engine(const Options& o, const std::string& uri)
    {
        vehicle_sil::process::ProcessOptions options;
        options.on_line = [this](vehicle_sil::process::Stream, std::string_view line) {
            const std::lock_guard lock(mutex);
            lines += std::string(line) + "\n";
        };
        process = std::make_unique<vehicle_sil::process::Process>(o.engine, std::vector<std::string>{"--registry-uri", uri},
                                                                   std::move(options));
    }
    std::string text()
    {
        process->finish_output(10s);
        const std::lock_guard lock(mutex);
        return lines;
    }
};

void signal(const Options& o)
{
#ifdef _WIN32
    if (GetConsoleWindow() == nullptr && GetConsoleCP() == 0)
    {
        throw Skip("no console to send Ctrl+Break through");
    }
    const std::string by = "Ctrl+Break";
#else
    const std::string by = "SIGTERM";
#endif
    RegistryProcess registry(o.registry, o.work_dir);
    const auto deadline = std::chrono::steady_clock::now() + o.timeout;
    {
        // Before the start: the controller waits for Ghost too, which never comes. Once the
        // engine has connected, it has its handler (it installs it before it connects).
        vehicle_sil::SimulationControllerOptions c;
        c.registry_uri = registry.uri();
        c.required_participants = {"Engine", "Ghost"};
        vehicle_sil::SimulationController controller(c);
        Engine engine(o, registry.uri());
        const auto engine_missing = [&controller] {
            const std::vector<std::string> missing = controller.missing_participants();
            return std::find(missing.begin(), missing.end(), "Engine") != missing.end();
        };
        while (engine_missing())
        {
            require(std::chrono::steady_clock::now() < deadline && !engine.process->poll(), "the engine did not connect");
            std::this_thread::sleep_for(10ms);
        }
        require(engine.process->request_stop(), "the stop request could not be sent");
        const auto status = engine.process->wait_for(o.timeout);
        const std::string out = lf(engine.text());
        require(status && status->code == 130 && !status->signal,
                "before the start: exit code 130, not " + (status ? std::to_string(status->code) : std::string("running")) +
                    ":\n" + out);
        require(out == "error: Engine: interrupted (" + by + ") before the simulation started: it ends at once\n",
                "before the start the engine wrote:\n" + out);
        log("CHECK a stop request before the start ends the ECU at once -> ok");
    }
    {
        vehicle_sil::SimulationControllerOptions c;
        c.participant_name = "SignalController";
        c.registry_uri = registry.uri();
        c.required_participants = {"Engine"};
        vehicle_sil::SimulationController controller(c);
        Engine engine(o, registry.uri());
        controller.start(o.timeout);
        // The controller runs before the engine may: wait until the engine itself runs (its
        // published state), so that the request finds it running.
        while (controller.runtime().peer_status("Engine").state != SilKit::Services::Orchestration::ParticipantState::Running)
        {
            require(std::chrono::steady_clock::now() < deadline && !engine.process->poll(), "the engine did not run");
            std::this_thread::sleep_for(10ms);
        }
        require(engine.process->request_stop(), "the stop request could not be sent");
        // The engine stops the simulation in order: the controller shuts down.
        const auto end = controller.wait_until_finished(o.timeout);
        require(end == SilKit::Services::Orchestration::ParticipantState::Shutdown,
                "the simulation did not shut down in order after the stop request");
        const auto status = engine.process->wait_for(o.timeout);
        const std::string out = lf(engine.text());
        require(status && status->code == 130 && !status->signal,
                "while running: exit code 130, not " + (status ? std::to_string(status->code) : std::string("running")) +
                    ":\n" + out);
        require(out == "error: Engine: interrupted (" + by + "): it stopped the simulation and left it\n",
                "while running the engine wrote:\n" + out);
        log("CHECK a stop request while the simulation runs stops it in order -> ok");
    }
}

} // namespace

int main(int argc, char** argv)
{
    set_log_tag("ecu-main-launcher");
    try
    {
        const Options o = parse(argc, argv);
        fs::remove_all(o.work_dir);
        fs::create_directories(o.work_dir);
        log("scenario " + o.scenario);
        if (o.scenario == "describe")
        {
            describe(o);
        }
        else if (o.scenario == "local")
        {
            local(o);
        }
        else if (o.scenario == "silkit")
        {
            silkit(o);
        }
        else if (o.scenario == "usage")
        {
            usage(o);
        }
        else if (o.scenario == "signal")
        {
            signal(o);
        }
        else
        {
            throw std::invalid_argument("unknown scenario " + o.scenario);
        }
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
