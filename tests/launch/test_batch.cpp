// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch test (launch/test.hpp) without a process: the list of the scenario files,
// what ends a test before its first run, the runs that start nothing, and the report for people
// and as JSON. The runs themselves: launch_run.test_command (launch_run_launcher.cpp).

#include "launch_test_util.hpp"

#include "vehicle_sil/launch/test.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;

namespace {

std::filesystem::path fresh_dir(const char* name)
{
    const auto dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write(const std::filesystem::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; }

std::string shown(const ScenarioList& list)
{
    std::string out;
    for (const TestScenario& scenario : list.scenarios)
    {
        out += scenario.shown + "\n";
    }
    for (const Finding& error : list.errors)
    {
        out += "E " + error.code + ": " + error.message + "\n";
    }
    return out;
}

std::string results(const TestReport& report)
{
    std::string out;
    for (const TestRun& run : report.runs)
    {
        out += run.variant + " " + run.scenario + " " + std::to_string(static_cast<int>(run.result)) + "\n";
    }
    return out;
}

/// Two variants of a participant whose executable is missing: every check fails, nothing starts.
const char* const kConfig = "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                            "  - name: Engine\n    command: [missing/engine]\n"
                            "variants:\n  small: {}\n  medium: { params: { depth: 4 } }\n";
const char* const kPlain = "vehicle_sil_scenario: 1\nparams: { fill: 1 }\nsteps:\n  - set: { Engine.level: \"${fill}\" }\n";

} // namespace

TEST_CASE(the_scenarios_of_a_directory_come_in_the_order_of_their_names)
{
    const auto dir = fresh_dir("batch-list");
    // Written in another order than they are listed in; 'C' sorts before 'a' (bytes).
    for (const char* name : {"b.yaml", "notes.txt", "a.yml", "C.JSON", "yaml", ".yaml", "z.yaml.bak"})
    {
        write(dir / name, "x");
    }
    std::filesystem::create_directories(dir / "sub.yaml");
    write(dir / "sub.yaml" / "inner.yaml", "x");
    std::filesystem::create_directories(dir / "empty");
    const std::string d = dir.generic_string();
    check_text(shown(list_scenarios({d})), d + "/C.JSON\n" + d + "/a.yml\n" + d + "/b.yaml\n");
    // Files as given, in the order given, also one that is no .yaml and one named twice.
    check_text(shown(list_scenarios({d + "/notes.txt", d + "/", d + "/b.yaml"})),
             d + "/notes.txt\n" + d + "/C.JSON\n" + d + "/a.yml\n" + d + "/b.yaml\n" + d + "/b.yaml\n");
    check_text(shown(list_scenarios({d + "/none.yaml", d + "/empty", d + "/b.yaml"})),
             d + "/b.yaml\n"
             "E usage: --scenarios " + d + "/none.yaml: there is no such file or directory: check the path (it is relative to "
             "the current directory, as the configuration file is)\n"
             "E usage: --scenarios " + d + "/empty: the directory has no scenario file (a file directly in it whose name ends "
             "in .yaml, .yml or .json; subdirectories are not searched): name the directory that holds the scenarios, or the "
             "files themselves\n");
}

TEST_CASE(a_test_that_cannot_begin_runs_nothing)
{
    const auto dir = fresh_dir("batch-begin");
    write(dir / "sim.yaml", kConfig);
    write(dir / "plain.yaml", kPlain);
    write(dir / "other.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - set: { Engine.level: \"${depth}\" }\n");
    TestOptions options;
    options.scenarios = {{dir / "plain.yaml", "plain.yaml"}, {dir / "other.yaml", "other.yaml"}};

    // A file that is not there.
    const TestReport missing = test_file(dir / "none.yaml", "none.yaml", options);
    CHECK(missing.result() == TestResult::Unreadable && missing.exit_code() == 3 && missing.runs.empty());
    check_text(format_test_text(missing), "error: none.yaml: the file does not exist: check the path\n\n"
                                        "cannot read none.yaml: nothing was run\n");

    // A variant the file does not have: the check's words.
    options.variants = {"medium", "smal"};
    const TestReport typo = test_file(dir / "sim.yaml", "sim.yaml", options);
    CHECK(typo.result() == TestResult::Usage && typo.exit_code() == 4 && typo.runs.empty());
    CHECK(typo.findings.size() == 1);
    if (!(typo.findings.size() == 1))
    {
        return;
    }
    check_text(typo.findings[0].message,
             "--variant smal: the file has no variant with this name (did you mean 'small'?); its variants are small, medium");
    options.variants.clear();

    // A parameter no scenario has (the names: also one that only a variant gives a value); one
    // that only one of them has is fine.
    options.run.check.params = {ParamValue{"fill", "3", false, ParamSource::CommandLine}, ParamValue{"dept", "2", false, ParamSource::CommandLine}};
    const TestReport unused = test_file(dir / "sim.yaml", "sim.yaml", options);
    CHECK(unused.result() == TestResult::Usage && unused.runs.empty());
    CHECK(unused.findings.size() == 1);
    if (!(unused.findings.size() == 1))
    {
        return;
    }
    check_text(unused.findings[0].message,
             "--param dept=2: none of the 2 scenarios has a parameter dept (did you mean depth?): their parameters are fill, "
             "depth; remove the option or correct the name");
    check_text(format_test_json(unused).substr(0, 24), "{\n  \"vehicle_sil_test\": ");
    CHECK(format_test_json(unused).find("\"result\": \"usage\",") != std::string::npos);
    options.run.check.params = {ParamValue{"fill", "3", false, ParamSource::CommandLine}};
    CHECK(test_file(dir / "sim.yaml", "sim.yaml", options).result() == TestResult::Failed);
    options.run.check.params.clear();

    // A participant that someone starts by hand: every run would wait for a person. Refused
    // before the first run, with what to do; left out with --disable, the test runs.
    write(dir / "manual.yaml", "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                               "  - name: Engine\n    command: [missing/engine]\n"
                               "  - name: Hil\n    command: [missing/hil]\n    manual: true\n"
                               "  - name: Off\n    command: [missing/off]\n    manual: true\n    enabled: false\n");
    std::vector<std::string> lines;
    options.run.write_line = [&lines](std::string_view line) { lines.emplace_back(line); };
    const TestReport manual = test_file(dir / "manual.yaml", "manual.yaml", options);
    CHECK(manual.result() == TestResult::Usage && manual.exit_code() == 4 && manual.runs.empty() && lines.empty());
    CHECK(manual.findings.size() == 1);
    if (!(manual.findings.size() == 1))
    {
        return;
    }
    check_text(manual.findings[0].message,
             "participant 'Hil' is manual (manual: true in the file): someone starts it by hand, and test starts every run by "
             "itself, so each run would wait for it: leave it out with --disable Hil (a scenario with a step that uses its "
             "variables then fails its check), or run one scenario with it yourself: vehicle_sil_launch run <file> --scenario "
             "<file>");
    options.run.check.overrides.disable = {"Hil"};
    const TestReport without = test_file(dir / "manual.yaml", "manual.yaml", options);
    CHECK(without.result() == TestResult::Failed && without.runs.size() == 2 && lines.size() == 4);
    // Taken in with --enable: refused as one that the file enables. With variants: which ones.
    options.run.check.overrides.disable = {"Hil"};
    options.run.check.overrides.enable = {"Off"};
    const TestReport enabled = test_file(dir / "manual.yaml", "manual.yaml", options);
    CHECK(enabled.result() == TestResult::Usage && enabled.findings.size() == 1 &&
          enabled.findings[0].message.rfind("participant 'Off' is manual (manual: true in the file): ", 0) == 0);
    options.run.check.overrides = {};
    write(dir / "manual-variants.yaml", "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                                        "  - name: Hil\n    command: [missing/hil]\n    manual: true\n"
                                        "variants:\n  small: {}\n  medium: { participants: { Hil: { enabled: false } } }\n"
                                        "  large: {}\n");
    const TestReport some = test_file(dir / "manual-variants.yaml", "manual-variants.yaml", options);
    CHECK(some.result() == TestResult::Usage && some.findings.size() == 1 &&
          some.findings[0].message.rfind("participant 'Hil' is manual (manual: true in the file, in the variants small and large): ", 0) == 0);
    // Only the variants that run count.
    options.variants = {"medium"};
    CHECK(test_file(dir / "manual-variants.yaml", "manual-variants.yaml", options).result() == TestResult::Failed);
}

TEST_CASE(a_test_runs_every_variant_with_every_scenario_in_order)
{
    const auto dir = fresh_dir("batch-order");
    write(dir / "sim.yaml", kConfig);
    write(dir / "plain.yaml", kPlain);
    write(dir / "only.yaml", "vehicle_sil_scenario: 1\nonly: [medium]\nsteps:\n  - set: { Engine.level: 1 }\n");
    TestOptions options;
    options.scenarios = {{dir / "plain.yaml", "plain.yaml"}, {dir / "only.yaml", "only.yaml"}};
    std::vector<std::string> lines;
    options.run.write_line = [&lines](std::string_view line) { lines.emplace_back(line); };

    // Every check fails (the executable is missing): the runs are all there, failed, in the
    // file's order of the variants. One that is not for the variant fails too while the check has
    // another error: it is skipped only if that is all that is wrong.
    const TestReport all = test_file(dir / "sim.yaml", "sim.yaml", options);
    check_text(results(all), "small plain.yaml 1\nsmall only.yaml 1\nmedium plain.yaml 1\nmedium only.yaml 1\n");
    CHECK(all.result() == TestResult::Failed && all.exit_code() == 1);
    CHECK(all.variants == std::vector<std::string>({"small", "medium"}));
    CHECK(lines.size() == 8);
    if (!(lines.size() == 8))
    {
        return;
    }
    check_text(lines[0], "[vehicle_sil_launch] test 1 of 4: variant small, scenario plain.yaml");
    CHECK(lines[1].rfind("[vehicle_sil_launch] test 1 of 4: variant small, scenario plain.yaml: FAILED (participant 'Engine': ", 0) == 0);
    CHECK(all.runs[1].reason.find(" (and 1 more error)") != std::string::npos);
    CHECK(all.runs[0].run.has_value() && !all.runs[0].run->run.has_value());

    // --variant chooses, in the file's order whatever the order given; --fail-fast leaves the rest.
    options.variants = {"medium"};
    check_text(results(test_file(dir / "sim.yaml", "sim.yaml", options)), "medium plain.yaml 1\nmedium only.yaml 1\n");
    options.variants = {"medium", "small"};
    options.fail_fast = true;
    const TestReport fast = test_file(dir / "sim.yaml", "sim.yaml", options);
    check_text(results(fast), "small plain.yaml 1\nsmall only.yaml 4\nmedium plain.yaml 4\nmedium only.yaml 4\n");
    check_text(fast.runs[3].reason, "a run before it failed (--fail-fast)");
    CHECK(!fast.runs[3].run.has_value() && fast.exit_code() == 1);

    // An interrupt that is already there: the first run is the last.
    std::atomic<int> interrupts{1};
    options.fail_fast = false;
    options.run.interrupts = &interrupts;
    const TestReport stopped = test_file(dir / "sim.yaml", "sim.yaml", options);
    CHECK(stopped.result() == TestResult::Interrupted && stopped.exit_code() == kExitInterrupted);
    check_text(stopped.runs[1].reason, "the test was interrupted before it");
    CHECK(stopped.count(TestRunResult::NotRun) == 3);
}

TEST_CASE(the_report_of_a_test_for_people_and_as_json)
{
    TestReport report;
    report.file = "sim.yaml";
    report.variants = {"small", "medium"};
    report.scenarios = {"scn/idle.yaml", "scn/turbo.yaml"};
    report.runs = {{"small", "scn/idle.yaml", TestRunResult::Passed, "", RunReport{}},
                   {"small", "scn/turbo.yaml", TestRunResult::Skipped, "the scenario is only for medium (its only)", {}},
                   {"medium", "scn/idle.yaml", TestRunResult::Failed,
                    "line 4: expect Engine.rpm ge 650: not true at 100 ms: Engine.rpm ge 650, but it was 640 (its sample of 100 ms)",
                    RunReport{}},
                   {"medium", "scn/turbo.yaml", TestRunResult::Passed, "1 of 3 steps was skipped, 1 warning", RunReport{}}};
    CHECK(report.result() == TestResult::Failed);
    check_text(format_test_text(report),
             "variant  scenario        result\n"
             "small    scn/idle.yaml   passed\n"
             "small    scn/turbo.yaml  skipped  the scenario is only for medium (its only)\n"
             "medium   scn/idle.yaml   FAILED   line 4: expect Engine.rpm ge 650: not true at 100 ms: Engine.rpm ge 650, but it was "
             "640 (its sample of 100 ms)\n"
             "medium   scn/turbo.yaml  passed   1 of 3 steps was skipped, 1 warning\n"
             "\n"
             "to see a failed run in full, run it alone: vehicle_sil_launch run sim.yaml --speed max --variant medium --scenario "
             "scn/idle.yaml\n"
             "\n"
             "FAILED: 4 runs, 2 passed, 1 failed, 1 skipped\n");
    const std::string json = format_test_json(report);
    CHECK(json.find("  \"result\": \"failed\",\n  \"strict\": false,\n  \"variants\": [\n    \"small\",\n    \"medium\"\n  ],\n"
                    "  \"scenarios\": [\n    \"scn/idle.yaml\",\n    \"scn/turbo.yaml\"\n  ],\n"
                    "  \"passed\": 2,\n  \"failed\": 1,\n  \"skipped\": 1,\n  \"interrupted\": 0,\n  \"not_run\": 0,\n"
                    "  \"runs\": [\n    {\n      \"variant\": \"small\",\n      \"scenario\": \"scn/idle.yaml\",\n"
                    "      \"result\": \"passed\",\n      \"reason\": null,\n      \"run\": {\n        \"vehicle_sil_run\": 1,\n") !=
          std::string::npos);
    CHECK(json.find("      \"result\": \"skipped\",\n      \"reason\": \"the scenario is only for medium (its only)\",\n"
                    "      \"run\": null\n    },\n") != std::string::npos);
    CHECK(json.find("  ],\n  \"findings\": []\n}\n") != std::string::npos);

    // Without variants: no column for them; an interrupted test says what was left.
    TestReport plain;
    plain.file = "sim.yaml";
    plain.strict = true;
    plain.interrupted = true;
    plain.scenarios = {"a.yaml", "b.yaml"};
    plain.runs = {{"", "a.yaml", TestRunResult::Interrupted, "by SIGINT", RunReport{}},
                  {"", "b.yaml", TestRunResult::NotRun, "the test was interrupted before it", {}}};
    check_text(format_test_text(plain), "scenario  result\n"
                                      "a.yaml    interrupted  by SIGINT\n"
                                      "b.yaml    not run      the test was interrupted before it\n"
                                      "\n"
                                      "interrupted: 2 runs, 0 passed, 0 failed, 0 skipped, 1 interrupted, 1 not run (--strict)\n");
    CHECK(format_test_json(plain).find("\"variants\": [],") != std::string::npos);
    CHECK(format_test_json(plain).find("      \"variant\": null,\n") != std::string::npos);
    CHECK(plain.remarks().empty());
}

TEST_CASE(the_command_of_one_run_alone_has_the_options_of_the_test)
{
    // The scenario of the failed run has the parameters fill and depth; the test was also given
    // one for another scenario (rpm), which run would refuse for this one.
    RunReport failed;
    failed.check.scenario.emplace();
    failed.check.scenario->params = {ParamValue{"fill", "1", false, ParamSource::Scenario},
                                     ParamValue{"depth", "x", false, ParamSource::CommandLine}};
    TestReport report;
    report.file = "my sims/sim.yaml";
    report.strict = true;
    report.variants = {"it's"};
    report.scenarios = {"scn/a b.yaml"};
    report.given.params = {ParamValue{"rpm", "650", false, ParamSource::CommandLine},
                           ParamValue{"depth", "a b", false, ParamSource::CommandLine},
                           ParamValue{"fill", "", false, ParamSource::CommandLine}};
    report.given.disable = {"Dashboard", "Rear Axle"};
    report.given.enable = {"Hil"};
    report.given.timeout = std::chrono::milliseconds(60000);
    report.given.describe_timeout = std::chrono::milliseconds(500);
    report.runs = {{"it's", "scn/a b.yaml", TestRunResult::Failed, "why", failed}};
    const std::string text = format_test_text(report);
    const std::string begin = "to see a failed run in full, run it alone: ";
    const auto from = text.find(begin);
    CHECK(from != std::string::npos);
    if (from == std::string::npos)
    {
        return;
    }
    check_text(text.substr(from + begin.size(), text.find('\n', from) - from - begin.size()),
             "vehicle_sil_launch run 'my sims/sim.yaml' --speed max --variant 'it'\\''s' --scenario 'scn/a b.yaml' "
             "--param 'depth=a b' --param fill= --disable Dashboard --disable 'Rear Axle' --enable Hil --strict "
             "--timeout-ms 60000 --describe-timeout-ms 500");

    // A scenario that the check did not read: no --param (the run says what is wrong with it).
    report.runs[0].run = RunReport{};
    report.strict = false;
    report.given.timeout.reset();
    report.given.describe_timeout.reset();
    CHECK(format_test_text(report).find("--scenario 'scn/a b.yaml' --disable Dashboard --disable 'Rear Axle' --enable Hil\n") !=
          std::string::npos);
}

TEST_CASE(a_test_that_left_runs_out_or_ran_nothing_does_not_just_pass)
{
    // A run left the launcher's connection open: the rest was not run. No run failed, and yet
    // the test did: it did not run what it was asked to.
    TestReport left;
    left.file = "sim.yaml";
    left.variants = {"small", "medium"};
    left.scenarios = {"a.yaml", "b.yaml"};
    left.connection_not_closed = true;
    left.runs = {{"small", "a.yaml", TestRunResult::Passed, "", RunReport{}},
                 {"small", "b.yaml", TestRunResult::Passed, "1 warning", RunReport{}},
                 {"medium", "a.yaml", TestRunResult::NotRun, "the launcher's connection to the simulation did not close in the run before it", {}},
                 {"medium", "b.yaml", TestRunResult::NotRun, "the launcher's connection to the simulation did not close in the run before it", {}}};
    CHECK(left.result() == TestResult::Failed && left.exit_code() == 1);
    const std::string leftMessage =
        "2 of 4 runs were not run: the launcher's connection to the simulation did not close after the run of scenario b.yaml "
        "(variant small), and the launcher ends its process rather than start another simulation in it. Run the test again for "
        "the runs that are 'not run' (--variant and --scenarios choose them), and see that run's warning "
        "connection-not-closed";
    check_text(format_test_text(left),
             "variant  scenario  result\n"
             "small    a.yaml    passed\n"
             "small    b.yaml    passed   1 warning\n"
             "medium   a.yaml    not run  the launcher's connection to the simulation did not close in the run before it\n"
             "medium   b.yaml    not run  the launcher's connection to the simulation did not close in the run before it\n"
             "\n"
             "error: sim.yaml: " + leftMessage + "\n"
             "\n"
             "FAILED: 4 runs, 2 passed, 0 failed, 0 skipped, 2 not run (the launcher's connection to the simulation did not close)\n");
    const std::string leftJson = format_test_json(left);
    CHECK(leftJson.find("  \"result\": \"failed\",\n") != std::string::npos);
    CHECK(leftJson.find("  \"findings\": [\n    {\n      \"severity\": \"error\",\n      \"code\": \"test-runs-left\",\n") !=
          std::string::npos);
    CHECK(leftJson.find(leftMessage) != std::string::npos);
    // Left out by an interrupt: interrupted, as before; by --fail-fast: failed by the run that failed, no remark.
    left.connection_not_closed = false;
    left.interrupted = true;
    CHECK(left.result() == TestResult::Interrupted && left.exit_code() == kExitInterrupted && left.remarks().empty());
    left.interrupted = false;
    left.runs[1].result = TestRunResult::Failed;
    CHECK(left.result() == TestResult::Failed && left.remarks().empty());

    // Every run skipped: nothing was checked. A warning, and a failure with --strict.
    TestReport none;
    none.file = "sim.yaml";
    none.variants = {"small"};
    none.given.variants = {"small"};
    none.scenarios = {"a.yaml", "b.yaml"};
    none.runs = {{"small", "a.yaml", TestRunResult::Skipped, "the scenario is only for medium (its only)", {}},
                 {"small", "b.yaml", TestRunResult::Skipped, "the scenario is only for medium (its only)", {}}};
    CHECK(none.result() == TestResult::Passed && none.exit_code() == 0);
    check_text(format_test_text(none),
             "variant  scenario  result\n"
             "small    a.yaml    skipped  the scenario is only for medium (its only)\n"
             "small    b.yaml    skipped  the scenario is only for medium (its only)\n"
             "\n"
             "warning: sim.yaml: nothing was run: all 2 runs were skipped, because no scenario is for the variant small (the only "
             "of each scenario; --variant chose what runs). Run a variant the scenarios are for, or correct their only. The test "
             "checked nothing: it passes (--strict would fail on that)\n"
             "\n"
             "passed: 2 runs, 0 passed, 0 failed, 2 skipped\n");
    none.strict = true;
    CHECK(none.result() == TestResult::Failed && none.exit_code() == 1);
    const std::string strictText = format_test_text(none);
    CHECK(strictText.find("The test checked nothing, and --strict fails on that\n\nFAILED: 2 runs, 0 passed, 0 failed, 2 skipped (--strict)\n") !=
          std::string::npos);
    CHECK(format_test_json(none).find("      \"severity\": \"warning\",\n      \"code\": \"test-nothing-run\",\n") != std::string::npos);
    // One run that ran is enough, also with --strict; and without --variant the remark does not name it.
    none.runs[1] = {"small", "b.yaml", TestRunResult::Passed, "", RunReport{}};
    CHECK(none.result() == TestResult::Passed && none.remarks().empty());
    none.runs.pop_back();
    none.given.variants.clear();
    none.variants = {"small", "large"};
    CHECK(none.remarks().size() == 1 &&
          none.remarks()[0].message.find("nothing was run: the 1 run was skipped, because no scenario is for the variants small and "
                                         "large (the only of each scenario). Run a variant") == 0);
}
