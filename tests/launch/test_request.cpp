// SPDX-License-Identifier: MIT
//
// The request of a run (launch::RunRequest): the rules of validate_request() on fields that do not
// go together, and the options that a request makes (check_options_of(), run_options_of()). No
// process, no file.

#include "test_harness.hpp"

#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/request.hpp"

#include <chrono>
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
                                           "--stdin-commands if needed";
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
    r.host.control = true; // its clients could not end the hold either
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
    r.host.control = true;
    CHECK(messages(r).empty());
    r.intent.start_paused = false;
    r.host.control = false;
    CHECK(messages(r).empty());
}

TEST_CASE(the_rules_are_reported_in_the_order_of_the_checks)
{
    RunRequest r = with_params();
    r.host.timeline = "out.json";
    r.intent.start_paused = true;
    CHECK(messages(r) == list(kParamNeedsScenario, kTimelineNeedsScenario, kPausedNeedsCommands));
}

TEST_CASE(check_options_take_every_field_of_the_request_that_the_check_reads)
{
    // One field changes at a time from the empty request: the options differ in that field only.
    const CheckOptions none = check_options_of(RunRequest{});
    CHECK(none.describe_timeout == 10000ms && !none.strict && none.params.empty() && none.scenario.empty() &&
          none.scenario_shown.empty() && none.connect.empty() && none.tool_directory.empty() &&
          none.time_policy == TimePolicy::Realtime && !none.overrides.variant && none.overrides.disable.empty() &&
          none.overrides.enable.empty() && none.overrides.manual.empty() && !none.run_speed && !none.start_paused);

    RunRequest r;
    r.intent.variant = "winter";
    CHECK(check_options_of(r).overrides.variant == std::optional<std::string>("winter"));
    r = {};
    r.intent.disable = {"A"};
    CHECK(check_options_of(r).overrides.disable == list("A"));
    r = {};
    r.intent.enable = {"B"};
    CHECK(check_options_of(r).overrides.enable == list("B"));
    r = {};
    r.intent.manual = {"C"};
    CHECK(check_options_of(r).overrides.manual == list("C"));
    r = {};
    r.intent.strict = true;
    CHECK(check_options_of(r).strict);
    r = {};
    r.intent.params.push_back(param("x", "1"));
    CHECK(check_options_of(r).params.size() == 1 && check_options_of(r).params[0].name == "x");
    r = {};
    r.intent.scenario = "drive.yaml";
    CHECK(check_options_of(r).scenario_shown == "drive.yaml" && check_options_of(r).scenario == "drive.yaml");
    r = {};
    r.intent.connect.push_back({"ETH_EXT", "none"});
    CHECK(check_options_of(r).connect.size() == 1 && check_options_of(r).connect[0].network == "ETH_EXT");
    r = {};
    r.intent.time_policy = TimePolicy::Free;
    CHECK(check_options_of(r).time_policy == TimePolicy::Free);
    r = {};
    r.host.tool_directory = "/opt/tools";
    CHECK(check_options_of(r).tool_directory == "/opt/tools");
    r = {};
    r.host.describe_timeout = 5ms;
    CHECK(check_options_of(r).describe_timeout == 5ms);
    // What the run gives its check itself stays unset.
    r = {};
    r.intent.speed = std::optional<double>(2.0);
    r.intent.start_paused = true;
    CHECK(!check_options_of(r).run_speed && !check_options_of(r).start_paused);
}

TEST_CASE(run_options_take_every_field_of_the_request_that_the_run_reads)
{
    const RunOptions none = run_options_of(RunRequest{});
    CHECK(!none.speed && !none.start_paused && none.log_dir.empty() && none.timeline.empty() && none.timeline_shown.empty() &&
          !none.timeout);

    RunRequest r;
    r.intent.speed = std::optional<double>(2.0);
    CHECK(run_options_of(r).speed == std::optional<std::optional<double>>(2.0));
    r = {};
    r.intent.speed = std::optional<double>{}; // max: replaces the file's speed
    CHECK(run_options_of(r).speed.has_value() && !run_options_of(r).speed->has_value());
    r = {};
    r.intent.start_paused = true;
    CHECK(run_options_of(r).start_paused);
    r = {};
    r.host.log_dir = "logs";
    CHECK(run_options_of(r).log_dir == "logs");
    r = {};
    r.host.timeline = "out/t.json";
    CHECK(run_options_of(r).timeline == "out/t.json" && run_options_of(r).timeline_shown == "out/t.json");
    r = {};
    r.host.timeout = 900ms;
    CHECK(run_options_of(r).timeout == std::optional(900ms));
    // The check inside is the request's check.
    r = {};
    r.intent.strict = true;
    r.intent.variant = "v";
    CHECK(run_options_of(r).check.strict && run_options_of(r).check.overrides.variant == std::optional<std::string>("v"));
}
