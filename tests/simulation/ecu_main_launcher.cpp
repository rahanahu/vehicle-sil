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
#include "launcher_support.hpp"

#include "vehicle_sil/cli/args.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

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
        else
        {
            throw std::invalid_argument("unknown scenario " + o.scenario);
        }
        log("PASS");
        return 0;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return 1;
    }
}
