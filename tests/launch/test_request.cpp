// SPDX-License-Identifier: MIT
//
// The request of a run (launch::RunRequest): the rules of validate_request() on fields that do not
// go together, and what the check and the plan of a run read of a request, field by field. No
// process.

#include "launch_test_util.hpp"

#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/run_plan.hpp"
#include "vehicle_sil/launch/test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::launch;
using namespace std::chrono_literals;

constexpr const char* kParamNeedsScenario = "--param sets a parameter of a scenario: give --scenario <file> too, or leave "
                                            "--param out";
constexpr const char* kTimelineNeedsScenario = "--timeline writes what a scenario did over the time: give --scenario <file> "
                                               "too, or leave --timeline out";
constexpr const char* kScenarioNotPaused = "--scenario cannot be used with --start-paused: the scenario writes its values for "
                                           "0 before the first step and runs by itself; pause and step it with "
                                           "--stdin-commands or from a Control Server client (--control) if needed";
constexpr const char* kPausedNeedsCommands = "--start-paused needs --stdin-commands or --control: without commands nothing "
                                             "could make the simulation go on (resume, step)";

/// The strings as a vector (a braced list cannot go into CHECK(): its commas split the macro's arguments).
template <typename... Strings>
std::vector<std::string> list(const Strings&... items)
{
    return {std::string(items)...};
}

ParamValue param(std::string name, std::string text)
{
    ParamValue p;
    p.name = std::move(name);
    p.text = std::move(text);
    p.source = ParamSource::CommandLine;
    return p;
}

RunRequest with_params()
{
    RunRequest r;
    r.intent.params.push_back(param("idle_rpm", "650"));
    return r;
}

std::vector<std::string> messages(const RunRequest& r, RequestCommand command = RequestCommand::Run)
{
    std::vector<std::string> out;
    for (const Finding& f : validate_request(r, command))
    {
        CHECK(f.code == "usage" && f.severity == Severity::Error && f.line == 0);
        out.push_back(f.message);
    }
    return out;
}

} // namespace

TEST_CASE(an_empty_request_breaks_no_rule)
{
    for (const RequestCommand command : {RequestCommand::Check, RequestCommand::Run, RequestCommand::Test})
    {
        CHECK(messages(RunRequest{}, command).empty());
    }
}

TEST_CASE(a_parameter_needs_a_scenario_except_in_a_test)
{
    RunRequest r = with_params();
    CHECK(messages(r) == list(kParamNeedsScenario));
    CHECK(messages(r, RequestCommand::Check) == list(kParamNeedsScenario));
    CHECK(messages(r, RequestCommand::Test).empty()); // the scenarios of a test come from its own list
    r.intent.scenario = "drive.yaml";
    CHECK(messages(r).empty());
    r.intent.scenario = ""; // given: its value is for the reader to refuse
    CHECK(messages(r).empty());
}

TEST_CASE(a_timeline_needs_a_scenario)
{
    RunRequest r;
    r.host.timeline = "out.json";
    CHECK(messages(r) == list(kTimelineNeedsScenario));
    r.intent.scenario = "drive.yaml";
    CHECK(messages(r).empty());
}

TEST_CASE(a_scenario_and_a_start_paused_are_refused_with_or_without_commands)
{
    RunRequest r;
    r.intent.scenario = "drive.yaml";
    r.intent.start_paused = true;
    CHECK(messages(r) == list(kScenarioNotPaused, kPausedNeedsCommands));
    r.host.stdin_commands = true;
    CHECK(messages(r) == list(kScenarioNotPaused));
    r.host.stdin_commands = false;
    r.host.control.emplace(); // its clients could not end the hold either
    CHECK(messages(r) == list(kScenarioNotPaused));
}

TEST_CASE(a_start_paused_needs_a_way_to_go_on)
{
    RunRequest r;
    r.intent.start_paused = true;
    CHECK(messages(r) == list(kPausedNeedsCommands));
    r.host.stdin_commands = true;
    CHECK(messages(r).empty());
    r.host.stdin_commands = false;
    r.host.control.emplace();
    CHECK(messages(r).empty());
    r.intent.start_paused = false;
    r.host.control.reset();
    CHECK(messages(r).empty());
}

TEST_CASE(the_rules_are_reported_in_the_order_of_the_checks)
{
    RunRequest r = with_params();
    r.host.timeline = "out.json";
    r.intent.start_paused = true;
    CHECK(messages(r) == list(kParamNeedsScenario, kTimelineNeedsScenario, kPausedNeedsCommands));
}

namespace {

/// A file with two participants whose executables are missing (nothing is run) and a bridge, which
/// sets the time lock; B is switched off.
const char* const kSim = "vehicle_sil_simulation: 1\nnetworks:\n  - name: ETH1\n    kind: ethernet\nparticipants:\n"
                         "  - name: A\n    command: [missing/a]\n"
                         "  - name: B\n    command: [missing/b]\n    enabled: false\n"
                         "  - name: Gateway\n    command: [missing/gateway]\n    role: bridge\n    describe: false\n";
const char* const kVariants = "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                              "  - name: A\n    command: [missing/a]\nvariants:\n  winter: {}\n  summer: {}\n";
const char* const kDrive = "vehicle_sil_scenario: 1\nparams: { x: 0 }\nsteps:\n  - set: { A.level: \"${x}\" }\n";

/// The files above in a directory of their own: one for each test, since CTest may run two tests at
/// once and each starts by removing what was there.
std::filesystem::path request_files(const char* name)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "request" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "sim.yaml", std::ios::binary) << kSim;
    std::ofstream(dir / "variants.yaml", std::ios::binary) << kVariants;
    std::ofstream(dir / "drive.yaml", std::ios::binary) << kDrive;
    // A runnable file with the name of a bridge; nothing runs it.
    std::filesystem::create_directories(dir / "tools");
#ifdef _WIN32
    const std::filesystem::path tool = dir / "tools" / "vehicle_sil_ethernet_bridge.exe";
#else
    const std::filesystem::path tool = dir / "tools" / "vehicle_sil_ethernet_bridge";
#endif
    std::ofstream(tool, std::ios::binary) << "#!/bin/sh\n";
    std::filesystem::permissions(tool, std::filesystem::perms::owner_all);
    return dir;
}

RunRequest of_file(const std::string& name)
{
    RunRequest r;
    r.host.config = name;
    return r;
}

/// The codes of the findings that begin with `prefix`, each with a space after it.
std::string codes(const CheckReport& report, std::string_view prefix)
{
    std::string out;
    for (const Finding& f : report.findings)
    {
        if (f.code.rfind(prefix, 0) == 0)
        {
            out += f.code + " ";
        }
    }
    return out;
}

bool says(const CheckReport& report, std::string_view text)
{
    for (const Finding& f : report.findings)
    {
        if (f.message.find(text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE(a_check_reads_every_field_of_the_request_that_is_the_checks)
{
    // One field changes at a time from the request of the file alone: the check differs in what
    // that field is for. (Its executables are missing: nothing is run.)
    const std::filesystem::path dir = request_files("check-fields");
    const vehicle_sil::launch::test::InDirectory there(dir);
    const CheckOptions file_only{.params_for_many = false, .file_only = true, .fastest_by_default = false};
    const Checked none = check_file(of_file("sim.yaml"));
    CHECK(none.file == "sim.yaml" && none.config.has_value());
    if (!none.config)
    {
        return;
    }
    CHECK(none.config->variant.empty() && none.config->participants.at(0).enabled && !none.config->participants.at(1).enabled &&
          !none.config->participants.at(0).manual && !none.strict && !none.scenario && none.outside.empty() &&
          none.time_lock.locked && codes(none, "outside-real-time").empty() && codes(none, "connect-").empty());

    RunRequest r = of_file("variants.yaml");
    CHECK(check_file(r).usage); // the file has variants: one must be named
    r.intent.variant = "winter";
    CHECK(check_file(r).config.has_value() && check_file(r).config->variant == "winter");
    r = of_file("sim.yaml");
    r.intent.disable = {"A"};
    CHECK(!check_file(r).config->participants.at(0).enabled);
    r = of_file("sim.yaml");
    r.intent.enable = {"B"};
    CHECK(check_file(r).config->participants.at(1).enabled);
    r = of_file("sim.yaml");
    r.intent.manual = {"A"};
    CHECK(check_file(r).config->participants.at(0).manual);
    r = of_file("sim.yaml");
    r.intent.strict = true;
    CHECK(check_file(r).strict);
    r = of_file("sim.yaml");
    r.intent.scenario = "drive.yaml";
    const Checked driven = check_file(r);
    CHECK(driven.scenario.has_value() && driven.scenario->file == "drive.yaml" && codes(driven, "file").empty());
    CHECK(driven.scenario && driven.scenario->params.size() == 1 && driven.scenario->params[0].text == "0");
    // A parameter is read with a scenario only (validate_request() asks for both).
    r.intent.params.push_back(param("x", "1"));
    const Checked set = check_file(r);
    CHECK(set.scenario && set.scenario->params.size() == 1 && set.scenario->params[0].text == "1" &&
          set.scenario->params[0].source == ParamSource::CommandLine);
    r = of_file("sim.yaml");
    r.intent.connect.push_back({"ETH_EXT", "none"});
    CHECK(says(check_file(r, file_only), "ETH_EXT") && !codes(check_file(r, file_only), "connect-").empty());
    r = of_file("sim.yaml");
    r.intent.time_policy = TimePolicy::Free;
    CHECK(!check_file(r).time_lock.locked);
    // Where the tools are: the first place a bridge is looked for, before PATH. A runnable file of
    // the bridge's name there is found, whatever is on PATH. (It is not asked: the participants of
    // the file could not be described.)
    r = of_file("sim.yaml");
    r.intent.connect.push_back({"ETH1", "eth-none0"});
    r.host.tool_directory = dir / "tools";
    const Checked asked = check_file(r);
    CHECK(codes(asked, "connect-tool-missing").empty());
    CHECK(asked.outside.size() == 1 && asked.outside.at(0).tool_path.parent_path() == dir / "tools");
    // The speed and --start-paused of the run that the request asks for, for the time lock; a
    // test's speed is its default, which the check does not judge.
    r = of_file("sim.yaml");
    r.intent.speed = std::optional<double>(2.0);
    CHECK(codes(check_file(r), "outside-real-time") == "outside-real-time ");
    CHECK(codes(check_file(r, CheckOptions{.params_for_many = false, .file_only = false, .fastest_by_default = true}),
                "outside-real-time")
            .empty());
    r = of_file("sim.yaml");
    r.intent.start_paused = true;
    CHECK(codes(check_file(r), "outside-real-time") == "outside-real-time ");
    // What only a run has is not the check's: the same findings with it.
    r = of_file("sim.yaml");
    r.host.log_dir = "logs";
    r.host.timeline = "out/t.json";
    r.host.timeout = 900ms;
    r.host.control.emplace();
    r.host.stdin_commands = true;
    CHECK(codes(check_file(r), "") == codes(none, ""));
    CHECK(!std::filesystem::exists(dir / "logs") && !std::filesystem::exists(dir / "out"));
}

TEST_CASE(the_describe_limit_of_the_request_is_what_a_test_reports_and_defaults_to_the_checks)
{
    // The limit of one --describe needs a process that is slow to answer: the launcher's own
    // tests hold that the check keeps to it (launch_cli.launcher). Here: the default, and that a
    // test reads the request's (TestReport::given, before it reads the file).
    CHECK(RunHost{}.describe_timeout == kDescribeTimeout && kDescribeTimeout == 10000ms);
    TestOptions options;
    options.request.host.config = "no-such-directory/sim.yaml";
    CHECK(!test_file(options).given.describe_timeout);
    options.request.host.describe_timeout = 5ms;
    CHECK(test_file(options).given.describe_timeout == std::optional(std::chrono::milliseconds(5)));
}

TEST_CASE(a_plan_takes_every_field_of_the_request_that_the_run_reads)
{
    Checked check;
    check.config.emplace();
    const RunPlan none = plan_run(check, "dir", RunRequest{});
    CHECK(!none.timing.speed && !none.holds_before_start && none.log_dir.empty() && none.timeline.empty() &&
          none.timeline_shown.empty() && !none.timeout && !none.strict);

    RunRequest r;
    r.intent.speed = std::optional<double>(2.0);
    CHECK(plan_run(check, "dir", r).timing.speed == std::optional<double>(2.0));
    r = {};
    check.config->speed = 3.0;
    CHECK(plan_run(check, "dir", r).timing.speed == std::optional<double>(3.0)); // the file's
    r.intent.speed = std::optional<double>{}; // max: replaces the file's speed
    CHECK(!plan_run(check, "dir", r).timing.speed);
    check.config->speed.reset();
    r = {};
    r.intent.start_paused = true;
    CHECK(plan_run(check, "dir", r, {}, true).holds_before_start && !plan_run(check, "dir", r).holds_before_start);
    r = {};
    r.host.log_dir = "logs";
    CHECK(plan_run(check, "dir", r).log_dir == "logs");
    r = {};
    r.host.timeline = "out/t.json";
    CHECK(plan_run(check, "dir", r).timeline == "out/t.json" && plan_run(check, "dir", r).timeline_shown == "out/t.json");
    r = {};
    r.host.timeout = 900ms;
    CHECK(plan_run(check, "dir", r).timeout == std::optional(900ms));
    r = {};
    r.intent.strict = true;
    CHECK(plan_run(check, "dir", r).strict);
}

TEST_CASE(the_check_of_a_run_is_the_check_of_its_request)
{
    const std::filesystem::path dir = request_files("check-of-run");
    const vehicle_sil::launch::test::InDirectory there(dir);
    RunRequest r = of_file("variants.yaml");
    r.intent.strict = true;
    r.intent.variant = "summer";
    const RunReport report = run_file(r);
    CHECK(!report.run.has_value()); // the executable is missing: nothing starts
    CHECK(report.strict && report.check.strict && report.check.file == "variants.yaml" && report.check.config.has_value() &&
          report.check.config->variant == "summer");
}
