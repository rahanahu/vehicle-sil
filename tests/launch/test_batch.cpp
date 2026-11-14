// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch test (launch/test.hpp) without a process: the list of the scenario files,
// what ends a test before its first run, the runs that start nothing, and the report for people
// and as JSON. The runs themselves: launch_run.test_command (launch_run_launcher.cpp).

#include "launch_test_util.hpp"

#include "vehicle_sil/launch/test.hpp"

#include <atomic>
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
                   {"medium", "scn/idle.yaml", TestRunResult::Failed, "line 4: expect Engine.rpm ge 650: it was 640", RunReport{}},
                   {"medium", "scn/turbo.yaml", TestRunResult::Passed, "1 of 3 steps was skipped", RunReport{}}};
    CHECK(report.result() == TestResult::Failed);
    check_text(format_test_text(report),
             "variant  scenario        result\n"
             "small    scn/idle.yaml   passed\n"
             "small    scn/turbo.yaml  skipped  the scenario is only for medium (its only)\n"
             "medium   scn/idle.yaml   FAILED   line 4: expect Engine.rpm ge 650: it was 640\n"
             "medium   scn/turbo.yaml  passed   1 of 3 steps was skipped\n"
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
}
