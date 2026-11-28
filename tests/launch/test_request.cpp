// SPDX-License-Identifier: MIT
//
// The request of a run (launch::RunRequest): the rules of validate_request() on fields that do not
// go together, and what the check and the plan of a run read of a request, field by field. No
// process.

#include "launch_test_util.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/runner/run_plan.hpp"
#include "vehicle_sil/launch/test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::launch;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::config;
using namespace std::chrono_literals;

constexpr const char* kParamNeedsScenario = "--param sets a parameter of a scenario: give --scenario <file> too, or leave "
                                            "--param out";
constexpr const char* kTimelineNeedsScenarioOrRecord =
    "--timeline needs --scenario, --record, --record-events or --record-can: give --scenario <file>, at least one "
    "--record <participant>.<variable>, --record-events or --record-can, or leave --timeline out";
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

TEST_CASE(direct_check_and_run_refuse_invalid_timing_before_reading_a_file)
{
    RunRequest r;
    r.host.config = "not-a-file.yaml";
    for (const auto value : {std::int64_t{0}, std::numeric_limits<std::int64_t>::min(), kMaxMillis + 1})
    {
        r.intent.stop_at_ms = value;
        const Checked checked = check_file(r);
        CHECK(checked.usage && !checked.config && !checked.findings.empty());
        const RunReport run = run_file(r);
        CHECK(run.check.usage && !run.run);
    }
    r.intent.stop_at_ms.reset();
    r.intent.late_tolerance_ms = -1;
    CHECK(check_file(r).usage);
    r.intent.late_tolerance_ms.reset();
    r.intent.speed = std::numeric_limits<double>::infinity();
    CHECK(check_file(r).usage);
    r.intent.speed.reset();
    r.host.stop_timeout_ms = 0;
    CHECK(check_file(r).usage);
    r.host.stop_timeout_ms = 10000;
    for (const auto value : {std::chrono::milliseconds{0}, std::chrono::milliseconds{-1},
                             std::chrono::milliseconds{kMaxMillis + 1}, std::chrono::milliseconds::max()})
    {
        r.host.describe_timeout = kDescribeTimeout;
        r.host.timeout = value;
        CHECK(check_file(r).usage);
        CHECK(run_file(r).check.usage);
        r.host.timeout.reset();
        r.host.describe_timeout = value;
        CHECK(check_file(r).usage);
    }
    for (const auto value : {std::chrono::milliseconds{1}, std::chrono::milliseconds{kMaxMillis}})
    {
        r.host.timeout = value;
        r.host.describe_timeout = value;
        CHECK(!check_file(r).usage);
    }
    r.host.timeout.reset();
    CHECK(!check_file(r).usage);
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

TEST_CASE(a_timeline_needs_a_scenario_or_record)
{
    RunRequest r;
    r.host.timeline = "out.json";
    CHECK(messages(r) == list(kTimelineNeedsScenarioOrRecord));
    r.intent.scenario = "drive.yaml";
    CHECK(messages(r).empty());
    r.intent.scenario.reset();
    r.intent.record = {"A.level"};
    CHECK(messages(r).empty());
    r.intent.record.clear();
    r.intent.record_events = true;
    CHECK(messages(r).empty());
    r.intent.record_events = false;
    r.intent.record_can = true;
    CHECK(messages(r).empty());
}

TEST_CASE(control_metrics_is_for_run_only)
{
    RunRequest r;
    r.host.control_metrics = "metrics.json";
    CHECK(messages(r, RequestCommand::Run).empty());
    CHECK(messages(r, RequestCommand::Check) == list("--control-metrics is for run: check starts no participant"));
    CHECK(messages(r, RequestCommand::Test) ==
          list("--control-metrics is for run, not for test (which runs multiple simulations into one summary): run one "
               "simulation with vehicle_sil_launch run --control-metrics <file>"));
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

TEST_CASE(the_text_from_stdin_and_the_saved_descriptions_are_for_check_only)
{
    RunRequest r;
    r.host.config_from_stdin = true;
    r.host.descriptions = "saved.json";
    r.host.write_descriptions = "saved.json";
    CHECK(messages(r, RequestCommand::Check).empty());
    const auto refused = list("--text-from-stdin is for check: run and test read the configuration file itself, so leave it out",
                              "--descriptions is for check: run and test run --describe of each participant again before "
                              "they start it, so leave it out",
                              "--write-descriptions is for check: leave it out, and write the descriptions with "
                              "vehicle_sil_launch check --write-descriptions <file>");
    CHECK(messages(r, RequestCommand::Run) == refused);
    CHECK(messages(r, RequestCommand::Test) == refused);
}

TEST_CASE(the_file_to_compare_the_commands_with_is_for_check_only)
{
    RunRequest r;
    r.host.compare_commands = "saved.yaml";
    CHECK(messages(r, RequestCommand::Check).empty());
    const auto refused = list("--compare-commands is for check: leave it out, and compare the commands with "
                              "vehicle_sil_launch check --compare-commands <file>");
    CHECK(messages(r, RequestCommand::Run) == refused);
    CHECK(messages(r, RequestCommand::Test) == refused);
}

TEST_CASE(the_rules_are_reported_in_the_order_of_the_checks)
{
    RunRequest r = with_params();
    r.host.timeline = "out.json";
    r.intent.start_paused = true;
    CHECK(messages(r) == list(kParamNeedsScenario, kTimelineNeedsScenarioOrRecord, kPausedNeedsCommands));
}

namespace {

/// A file with two participants whose executables are missing (nothing is run) and a bridge, which
/// sets the time lock; B is switched off.
const char* const kSim = "vehicle_sil_simulation: 2\nnetworks:\n  - name: ETH1\n    kind: ethernet\nparticipants:\n"
                         "  - name: A\n    command: [missing/a]\n"
                         "  - name: B\n    command: [missing/b]\n    enabled: false\n"
                         "  - name: Gateway\n    command: [missing/gateway]\n    role: bridge\n    describe: false\n";
const char* const kVariants = "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
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
    const vehicle_sil::config::test::InDirectory there(dir);
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
    // The saved descriptions: the file to write them to (none here: no executable described
    // itself), and the file to take them from, with which no bridge is looked for either.
    r = of_file("sim.yaml");
    r.host.write_descriptions = "saved.json";
    CHECK(codes(check_file(r), "") == codes(none, ""));
    CHECK(std::filesystem::exists(dir / "saved.json") && read_saved_descriptions(dir / "saved.json").problem.empty() &&
          read_saved_descriptions(dir / "saved.json").entries.empty());
    r = of_file("sim.yaml");
    r.host.descriptions = "sim.yaml"; // not a file of saved descriptions
    CHECK(codes(check_file(r), "descriptions-") == "descriptions-not-used ");
    r = of_file("sim.yaml");
    r.intent.connect.push_back({"ETH1", "eth-none0"});
    r.host.tool_directory = dir / "tools";
    r.host.descriptions = "saved.json";
    const Checked saved = check_file(r);
    CHECK(codes(saved, "connect-tool-missing").empty() && codes(saved, "descriptions-").empty());
    CHECK(saved.outside.size() == 1 && saved.outside.at(0).tool_path.empty());
    // The file to compare the commands with: the same, another, one that is missing; also for a
    // check that only reads the file, and with the variant of the request (every variant counts).
    CHECK(none.commands == CommandsCompared::NotAsked && codes(none, "commands-").empty());
    r = of_file("sim.yaml");
    r.host.compare_commands = "sim.yaml";
    const Checked same = check_file(r);
    CHECK(same.commands == CommandsCompared::Same && codes(same, "commands-").empty());
    CHECK(codes(same, "") == codes(none, ""));
    CHECK(check_file(r, file_only).commands == CommandsCompared::Same);
    r.host.compare_commands = "variants.yaml";
    CHECK(check_file(r).commands == CommandsCompared::Changed && codes(check_file(r), "commands-") == "commands-changed ");
    r.host.compare_commands = "missing.yaml";
    const Checked missing = check_file(r);
    CHECK(missing.commands == CommandsCompared::NotCompared && codes(missing, "commands-") == "commands-not-compared ");
    CHECK(says(missing, "the commands that run are not compared with missing.yaml: the file does not exist: check the path"));
    r = of_file("variants.yaml");
    r.intent.variant = "winter";
    r.host.compare_commands = "variants.yaml";
    CHECK(check_file(r).commands == CommandsCompared::Same);
    // A check that stops at a usage error compares nothing, and says only the usage error.
    r.intent.variant.reset();
    const Checked usage = check_file(r);
    CHECK(usage.usage && usage.commands == CommandsCompared::NotAsked && codes(usage, "commands-").empty());
    // A configuration file that cannot be read.
    r = of_file("missing.yaml");
    r.host.compare_commands = "sim.yaml";
    const Checked unreadable = check_file(r);
    CHECK(!unreadable.config && unreadable.commands == CommandsCompared::NotCompared &&
          codes(unreadable, "commands-") == "commands-not-compared " &&
          says(unreadable, "the commands that run are not compared with sim.yaml: the configuration being checked cannot be "
                           "read: see the errors"));
}

namespace {

std::filesystem::path scratch(const char* name)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "descriptions" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

ParticipantConfig participant(const std::string& name, std::vector<std::string> command)
{
    ParticipantConfig p;
    p.name = name;
    p.command = std::move(command);
    return p;
}

} // namespace

TEST_CASE(saved_descriptions_come_back_as_they_were_written)
{
    const std::filesystem::path dir = scratch("round-trip");
    std::vector<SavedDescription> entries(2);
    entries[0].name = "Engine";
    entries[0].command = {"engine/engine_ecu", "--mode", "a \"b\""};
    entries[0].executable = ExecutableStamp{"/x/engine/engine_ecu", 12345, -6789012345678};
    entries[0].output = "{\"name\": \"Engine \xC3\xA9\"}\n\ttab \\ back\x01\r\n";
    entries[1].name = "Dashboard";
    entries[1].command = {"dashboard"};
    entries[1].executable = ExecutableStamp{"/x/dashboard", 0, 1};
    CHECK(write_saved_descriptions(dir / "saved.json", entries).empty());
    const SavedDescriptions read = read_saved_descriptions(dir / "saved.json");
    CHECK(read.problem.empty() && read.entries.size() == 2);
    for (std::size_t i = 0; i < read.entries.size() && i < entries.size(); ++i)
    {
        CHECK(read.entries[i].name == entries[i].name && read.entries[i].command == entries[i].command &&
              read.entries[i].executable == entries[i].executable && read.entries[i].output == entries[i].output);
    }
    // Only the file is left in the directory: the one written beside it was renamed.
    CHECK(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 1);
    // An output that is not UTF-8 would not come back as it was: it is not saved.
    entries[1].output = "\xFF";
    CHECK(write_saved_descriptions(dir / "saved.json", entries).empty());
    CHECK(read_saved_descriptions(dir / "saved.json").entries.size() == 1);
}

TEST_CASE(saved_descriptions_that_cannot_be_used_say_why)
{
    const std::filesystem::path dir = scratch("unusable");
    // None yet: nothing to use, and nothing wrong.
    SavedDescriptions none = read_saved_descriptions(dir / "none.json");
    CHECK(none.entries.empty() && none.problem.empty());
    CHECK(read_saved_descriptions(dir).problem == "the file is not a regular file");
    std::ofstream(dir / "garbage.json", std::ios::binary) << "{\"vehicle_sil_descriptions\": ";
    CHECK(read_saved_descriptions(dir / "garbage.json").problem.rfind("the file is not one that check --write-descriptions wrote (", 0) == 0);
    std::ofstream(dir / "config.json", std::ios::binary) << "{\"vehicle_sil_simulation\": 2}";
    CHECK(read_saved_descriptions(dir / "config.json").problem ==
          "the file is not one that check --write-descriptions wrote (it has no vehicle_sil_descriptions)");
    std::ofstream(dir / "other.json", std::ios::binary)
        << "{\"vehicle_sil_descriptions\": 1, \"vehicle_sil\": \"0.0.1-other\", \"participants\": "
           "[{\"name\": \"Engine\"}]}";
    const SavedDescriptions other = read_saved_descriptions(dir / "other.json");
    CHECK(other.entries.empty() && other.problem == "vehicle-sil 0.0.1-other wrote the file, and this vehicle_sil_launch is " +
                                                        std::string(vehicle_sil::version()) +
                                                        " (only the same version reads it)");
    // Another version may write another form: the version is what is said.
    std::ofstream(dir / "later.json", std::ios::binary)
        << "{\"vehicle_sil_descriptions\": 2, \"vehicle_sil\": \"0.0.2-other\", \"entries\": {}}";
    CHECK(read_saved_descriptions(dir / "later.json").problem.rfind("vehicle-sil 0.0.2-other wrote the file", 0) == 0);
    std::ofstream(dir / "form.json", std::ios::binary)
        << "{\"vehicle_sil_descriptions\": 2, \"vehicle_sil\": \"" << vehicle_sil::version() << "\", \"participants\": []}";
    CHECK(read_saved_descriptions(dir / "form.json").problem ==
          "the file is not one that check --write-descriptions wrote (it is not of the form 1)");
    // A file that cannot be written: what is wrong, and nothing left behind.
    CHECK(!write_saved_descriptions(dir / "missing" / "saved.json", {}).empty());
    CHECK(!std::filesystem::exists(dir / "missing"));
}

TEST_CASE(a_saved_description_is_taken_for_the_same_name_command_and_executable_only)
{
    const std::filesystem::path dir = scratch("source");
    const std::filesystem::path exe = dir / "engine_ecu";
    std::ofstream(exe, std::ios::binary) << "built";
    const std::optional<ExecutableStamp> stamp = executable_stamp(exe);
    CHECK(stamp.has_value());
    if (!stamp)
    {
        return;
    }
    // Another spelling of the same file has the same stamp.
    CHECK(executable_stamp(dir / "." / "engine_ecu") == stamp);
    CHECK(!executable_stamp(dir / "missing"));
    std::vector<SavedDescription> saved(1);
    saved[0].name = "Engine";
    saved[0].command = {"engine_ecu", "--mode", "x"};
    saved[0].executable = *stamp;
    saved[0].output = "{}";
    const DescribeSource source = describe_from_saved(saved);
    const std::vector<std::string> arguments{"--mode", "x", "--describe", "--name", "Engine"};
    const std::optional<DescribeRun> run = source(participant("Engine", {"engine_ecu", "--mode", "x"}), exe, arguments);
    CHECK(run.has_value() && run->exit_code == 0 && run->standard_output == "{}" && !run->start_error &&
          run->command_text == "engine_ecu --mode x --describe --name Engine");
    CHECK(!source(participant("Engine2", {"engine_ecu", "--mode", "x"}), exe, arguments));
    CHECK(!source(participant("Engine", {"engine_ecu", "--mode", "y"}), exe, arguments));
    CHECK(!source(participant("Engine", {"engine_ecu", "--mode", "x"}), dir / "missing", arguments));
    // Built again: another size.
    std::ofstream(exe, std::ios::binary) << "built again";
    CHECK(!source(participant("Engine", {"engine_ecu", "--mode", "x"}), exe, arguments));
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
    // The request reaches the plan through run_choices(), as in run_file().
    const auto plan = [&check](const RunRequest& r, bool control_server = false) {
        return plan_run(check, nullptr, "dir", run_choices(r), {}, control_server);
    };
    const RunPlan none = plan(RunRequest{});
    CHECK(!none.timing.speed && !none.holds_before_start && none.log_dir.empty() && none.timeline.empty() &&
          none.timeline_shown.empty() && !none.timeout && !none.strict);

    RunRequest r;
    r.intent.speed = std::optional<double>(2.0);
    CHECK(plan(r).timing.speed == std::optional<double>(2.0));
    r.intent.speed = std::optional<double>{};
    CHECK(!plan(r).timing.speed);
    r.intent.stop_at_ms = 1003;
    r.intent.late_tolerance_ms = 0;
    r.host.start_timeout_ms = 17;
    r.host.stop_timeout_ms = 19;
    const RunPlan limited = plan(r);
    CHECK(limited.stop_at_ms == 1003 && limited.timing.time_step_ms == 1);
    CHECK(limited.timing.late_tolerance_ms == 0 && limited.start_timeout_ms == 17 && limited.stop_timeout_ms == 19);
    r = {};
    r.intent.start_paused = true;
    CHECK(plan(r, true).holds_before_start && !plan(r).holds_before_start);
    r = {};
    r.host.log_dir = "logs";
    CHECK(plan(r).log_dir == "logs");
    r = {};
    r.host.timeline = "out/t.json";
    CHECK(plan(r).timeline == "out/t.json" && plan(r).timeline_shown == "out/t.json");
    r = {};
    r.host.timeout = 900ms;
    CHECK(plan(r).timeout == std::optional(900ms));
    r = {};
    r.intent.strict = true;
    CHECK(plan(r).strict);
}

TEST_CASE(the_check_of_a_run_is_the_check_of_its_request)
{
    const std::filesystem::path dir = request_files("check-of-run");
    const vehicle_sil::config::test::InDirectory there(dir);
    RunRequest r = of_file("variants.yaml");
    r.intent.strict = true;
    r.intent.variant = "summer";
    const RunReport report = run_file(r);
    CHECK(!report.run.has_value()); // the executable is missing: nothing starts
    CHECK(report.strict && report.check.strict && report.check.file == "variants.yaml" && report.check.config.has_value() &&
          report.check.config->variant == "summer");
}

TEST_CASE(validate_request_enforces_after_scenario_pause_rules)
{
    RunRequest r;
    r.intent.after_scenario = AfterScenario::Pause;
    // Missing scenario and missing stdin/control.
    CHECK(!validate_request(r, RequestCommand::Run).empty());

    r.intent.scenario = "drive.yaml";
    // Still missing stdin/control for Run.
    CHECK(!validate_request(r, RequestCommand::Run).empty());
    // Accepted for Check (no control needed during check).
    CHECK(validate_request(r, RequestCommand::Check).empty());
    // Refused for Test even with scenario.
    CHECK(!validate_request(r, RequestCommand::Test).empty());

    r.host.stdin_commands = true;
    CHECK(validate_request(r, RequestCommand::Run).empty());

    // Refused if stop_at_ms is set.
    r.intent.stop_at_ms = 500;
    CHECK(!validate_request(r, RequestCommand::Run).empty());
}

TEST_CASE(validate_request_requires_scenario_when_case_is_given)
{
    RunRequest r;
    r.intent.scenario_case = "nominal";
    CHECK(!validate_request(r, RequestCommand::Check).empty());
    CHECK(!validate_request(r, RequestCommand::Run).empty());
    CHECK(validate_request(r, RequestCommand::Check).front().message.find("--case chooses a case of a scenario") !=
          std::string::npos);

    r.intent.scenario = "drive.yaml";
    CHECK(validate_request(r, RequestCommand::Check).empty());
    CHECK(validate_request(r, RequestCommand::Run).empty());
}

TEST_CASE(external_registry_validation_and_config_conflict)
{
    RunRequest r;
    r.host.external_registry_uri = "silkit://127.0.0.1:18500";
    CHECK(messages(r, RequestCommand::Check) == list("--external-registry is for run, serve and test: check starts no participant"));
    CHECK(messages(r, RequestCommand::Run).empty());
    CHECK(messages(r, RequestCommand::Test).empty());
    CHECK(run_choices(r).external_registry_uri == "silkit://127.0.0.1:18500");

    for (const char* bad : {"silkit://127.0.0.1:0", "silkit://0.0.0.0:18500", "silkit://127.0.0.1:18500/sim"})
    {
        r.host.external_registry_uri = bad;
        CHECK(!messages(r, RequestCommand::Run).empty());
        CHECK(!messages(r, RequestCommand::Test).empty());
        r.host.config = "not-a-file.yaml";
        CHECK(check_file(r).usage);
        CHECK(run_file(r).check.usage);
    }

    const std::filesystem::path dir = scratch("ext-reg-conflict");
    {
        std::ofstream out(dir / "fixed.yaml", std::ios::binary);
        out << "vehicle_sil_simulation: 2\n"
               "registry_uri: silkit://127.0.0.1:8500\n"
               "networks: []\n"
               "participants:\n"
               "  - name: Engine\n"
               "    command: [./missing]\n";
    }
    {
        std::ofstream out(dir / "plain.yaml", std::ios::binary);
        out << "vehicle_sil_simulation: 2\n"
               "networks: []\n"
               "participants:\n"
               "  - name: Engine\n"
               "    command: [./missing]\n";
    }
    RunRequest conflict;
    conflict.host.config = (dir / "fixed.yaml").string();
    conflict.host.external_registry_uri = "silkit://127.0.0.1:18500";
    CheckOptions opts;
    opts.file_only = true;
    const Checked bad_check = check_file(conflict, opts);
    CHECK(bad_check.usage);
    CHECK(!bad_check.hot_eligibility.eligible);
    CHECK(bad_check.findings.front().message.find("cannot be used when the configuration file sets registry_uri") !=
          std::string::npos);

    RunRequest eligible;
    eligible.host.config = (dir / "plain.yaml").string();
    eligible.host.external_registry_uri = "silkit://127.0.0.1:18500";
    const Checked ok_check = check_file(eligible, opts);
    CHECK(!ok_check.usage);
    CHECK(ok_check.hot_eligibility.eligible);
}

TEST_CASE(record_intent_is_allowed_and_validated_pre_run_even_without_scenario)
{
    RunRequest r;
    r.intent.record = {"A.level"};
    CHECK(messages(r, RequestCommand::Check).empty());
    CHECK(messages(r, RequestCommand::Run).empty());
    // --timeline without --scenario is allowed when --record is non-empty, and refused when empty.
    r.host.timeline = "out.json";
    CHECK(messages(r, RequestCommand::Run).empty());
    r.intent.record.clear();
    CHECK(messages(r, RequestCommand::Run) == list(kTimelineNeedsScenarioOrRecord));
    r.intent.scenario = "drive.yaml";
    CHECK(messages(r, RequestCommand::Run).empty());

    // Pre-run validation runs in check_file() without --scenario and reports invalid entries.
    const std::filesystem::path dir = request_files("record-check");
    const vehicle_sil::config::test::InDirectory there(dir);
    RunRequest checkReq = of_file("sim.yaml");
    checkReq.intent.record = {"badformat", "NoSuch.x", "B.level", "A.level"};
    const Checked rep = check_file(checkReq);
    CHECK(rep.recorded_variables.empty());
    CHECK(codes(rep, "record-") == "record-variable-format record-unknown-participant record-participant-unusable record-participant-unusable ");
}

TEST_CASE(record_after_fail_ms_validation_and_plan_propagation)
{
    // Out-of-range values are rejected by validate_timing(), validate_request(), check_file(), and run_file().
    for (const std::int64_t bad : {std::int64_t{0}, std::int64_t{-1}, kMaxRecordAfterFailMs + 1})
    {
        RunRequest r;
        r.host.config = "not-a-file.yaml";
        r.intent.scenario = "drive.yaml";
        r.intent.record_after_fail_ms = bad;
        CHECK(!validate_timing(r).empty());
        CHECK(!messages(r, RequestCommand::Check).empty());
        CHECK(!messages(r, RequestCommand::Run).empty());
        CHECK(check_file(r).usage);
        CHECK(run_file(r).check.usage);
    }

    // Setting record_after_fail_ms without scenario is a usage error.
    RunRequest r;
    r.intent.record_after_fail_ms = 300;
    CHECK(messages(r, RequestCommand::Check) ==
          list("--record-after-fail-ms needs a scenario: give --scenario <file> too, or leave --record-after-fail-ms out"));
    CHECK(messages(r, RequestCommand::Run) ==
          list("--record-after-fail-ms needs a scenario: give --scenario <file> too, or leave --record-after-fail-ms out"));
    CHECK(!messages(r, RequestCommand::Test).empty());

    // Allowed with scenario even without --timeline (both check and run), and refused on test.
    r.intent.scenario = "drive.yaml";
    CHECK(messages(r, RequestCommand::Check).empty());
    CHECK(messages(r, RequestCommand::Run).empty());
    CHECK(!messages(r, RequestCommand::Test).empty());

    // Boundary values 1 and kMaxRecordAfterFailMs are accepted.
    r.intent.record_after_fail_ms = 1;
    CHECK(messages(r, RequestCommand::Run).empty());
    r.intent.record_after_fail_ms = kMaxRecordAfterFailMs;
    CHECK(messages(r, RequestCommand::Run).empty());

    // run_choices() and plan_run() propagate record_after_fail into RunPlan.
    r.intent.record_after_fail_ms = 300;
    const RunChoices choices = run_choices(r);
    CHECK(choices.record_after_fail == std::optional<std::chrono::milliseconds>(300ms));

    Checked check;
    check.config.emplace();
    vehicle_sil::scenario::ScenarioCheck sc;
    sc.scenario.emplace();
    const RunPlan plan = plan_run(check, &sc, "dir", choices);
    CHECK(plan.record_after_fail == std::optional<std::chrono::milliseconds>(300ms));
}

TEST_CASE(record_events_validation_and_plan_propagation)
{
    RunRequest r;
    r.intent.record_events = true;
    // Allowed on check and run (with or without scenario / --timeline), refused on test.
    CHECK(messages(r, RequestCommand::Check).empty());
    CHECK(messages(r, RequestCommand::Run).empty());
    CHECK(messages(r, RequestCommand::Test) ==
          list("test does not accept --record-events: run one simulation with vehicle_sil_launch run --record-events"));
    r.host.timeline = "out.json";
    CHECK(messages(r, RequestCommand::Run).empty());

    const RunChoices choices = run_choices(r);
    CHECK(choices.record_events);

    Checked check;
    check.config.emplace();
    const RunPlan plan = plan_run(check, nullptr, "dir", choices);
    CHECK(plan.record_events);
}

TEST_CASE(record_can_validation_and_plan_can_networks_propagation)
{
    RunRequest r;
    r.intent.record_can = true;
    // Allowed on check and run (with or without scenario / --timeline), refused on test.
    CHECK(messages(r, RequestCommand::Check).empty());
    CHECK(messages(r, RequestCommand::Run).empty());
    CHECK(messages(r, RequestCommand::Test) ==
          list("test does not accept --record-can: run one simulation with vehicle_sil_launch run --record-can"));
    r.host.timeline = "out.json";
    CHECK(messages(r, RequestCommand::Run).empty());

    const RunChoices choices = run_choices(r);
    CHECK(choices.record_can);

    Checked check;
    check.config.emplace();
    check.config->networks = {
        NetworkConfig{"CAN_A", vehicle_sil::NetworkKind::Can, 1, true, std::nullopt},
        NetworkConfig{"ETH_1", vehicle_sil::NetworkKind::Ethernet, 2, true, std::nullopt},
        NetworkConfig{"CAN_INVALID", vehicle_sil::NetworkKind::Can, 3, false, std::nullopt},
        NetworkConfig{"LIN_1", vehicle_sil::NetworkKind::Lin, 4, true, std::nullopt},
        NetworkConfig{"CAN_B", vehicle_sil::NetworkKind::Can, 5, true, std::nullopt},
    };
    const RunPlan plan = plan_run(check, nullptr, "dir", choices);
    CHECK(plan.record_can);
    CHECK(plan.can_networks == list("CAN_A", "CAN_B"));
}
