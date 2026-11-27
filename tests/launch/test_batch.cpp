// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch test (launch/test.hpp) without a process: the list of the scenario files,
// what ends a test before its first run, the runs that start nothing, and the report for people
// and as JSON. The runs themselves: launch_run.test_command (launch_run_launcher.cpp).

#include "launch_test_util.hpp"

#include "vehicle_sil/launch/test.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;

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

/// test_file() of the file `name` in `dir`, from that directory: the outputs name the file and the
/// scenarios as a user who stands there does. (The scenarios are read by the names they are shown
/// with.)
TestReport test_in(const std::filesystem::path& dir, const std::string& name, TestOptions options)
{
    const vehicle_sil::config::test::InDirectory there(dir);
    options.request.host.config = name;
    return test_file(options);
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
const char* const kConfig = "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
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
    // Each is read by the name it is shown with: the name is that of the file.
    for (const TestScenario& scenario : list_scenarios({d, d + "/"}).scenarios)
    {
        CHECK(std::filesystem::is_regular_file(vehicle_sil::process::path_from_utf8(scenario.shown)));
    }
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
    options.scenarios = {{"plain.yaml"}, {"other.yaml"}};

    // A file that is not there.
    const TestReport missing = test_in(dir, "none.yaml", options);
    CHECK(missing.result() == TestResult::Unreadable && missing.exit_code() == 3 && missing.runs.empty());
    check_text(format_test_text(missing), "error: none.yaml: the file does not exist: check the path [file]\n\n"
                                        "cannot read none.yaml: nothing was run\n");

    // A variant the file does not have: the check's words.
    options.variants = {"medium", "smal"};
    const TestReport typo = test_in(dir, "sim.yaml", options);
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
    options.request.intent.params = {ParamValue{"fill", "3", false, ParamSource::CommandLine}, ParamValue{"dept", "2", false, ParamSource::CommandLine}};
    const TestReport unused = test_in(dir, "sim.yaml", options);
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
    options.request.intent.params = {ParamValue{"fill", "3", false, ParamSource::CommandLine}};
    CHECK(test_in(dir, "sim.yaml", options).result() == TestResult::Failed);
    options.request.intent.params.clear();

    // --param passed to a scenario whose template call fails does not raise a false usage error (exit 4);
    // the test runs and reports the template failure (exit 1).
    std::filesystem::create_directories(dir / "templates");
    write(dir / "missing_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/missing.yaml\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "templates" / "cyc_a.yaml",
          "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer }\nsteps:\n  - template: cyc_b.yaml\n");
    write(dir / "templates" / "cyc_b.yaml",
          "vehicle_sil_scenario_template: 1\nsteps:\n  - template: cyc_a.yaml\n    params: { throttle: 10 }\n");
    write(dir / "cyclic_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/cyc_a.yaml\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "templates" / "empty_steps.yaml",
          "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer }\nsteps: []\n");
    write(dir / "empty_steps_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/empty_steps.yaml\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "templates" / "bad_desc.yaml",
          "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer, description: 123 }\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "bad_desc_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/bad_desc.yaml\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "templates" / "missing_type.yaml",
          "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { default: 1 }\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "missing_type_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/missing_type.yaml\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "templates" / "unknown_key.yaml",
          "vehicle_sil_scenario_template: 1\nonly: [small]\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "unknown_key_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/unknown_key.yaml\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "templates" / "dup_key.yaml",
          "vehicle_sil_scenario_template: 1\nname: a\nname: b\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "dup_key_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/dup_key.yaml\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "templates" / "healthy.yaml",
          "vehicle_sil_scenario_template: 1\nparameters:\n  throttle: { type: integer }\nsteps:\n  - set: { Engine.level: \"${throttle}\" }\n");
    write(dir / "mixed_action_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/healthy.yaml\n    set: { Engine.level: 1 }\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "null_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: null\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "params_on_set.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - set: { Engine.level: 1 }\n    params: { throttle: \"${cli_param}\" }\n");
    write(dir / "syntax_err.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - set: [1\n");
    write(dir / "healthy_tpl.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/healthy.yaml\n    params: { throttle: 10 }\n");
    write(dir / "bad_time.yaml",
          "vehicle_sil_scenario: 1\nsteps:\n  - at_ms: \"bad\"\n    set: { Engine.level: 1 }\n");
    for (const char* brokenScenario : {"missing_tpl.yaml", "cyclic_tpl.yaml", "empty_steps_tpl.yaml", "bad_desc_tpl.yaml",
                                       "missing_type_tpl.yaml", "unknown_key_tpl.yaml", "dup_key_tpl.yaml",
                                       "mixed_action_tpl.yaml", "null_tpl.yaml", "params_on_set.yaml", "syntax_err.yaml"})
    {
        TestOptions tplOpts;
        tplOpts.scenarios = {{brokenScenario}};
        tplOpts.request.intent.params = {ParamValue{"cli_param", "50", false, ParamSource::CommandLine, 0, true}};
        const TestReport tplReport = test_in(dir, "sim.yaml", tplOpts);
        CHECK(tplReport.result() == TestResult::Failed);
        CHECK(tplReport.exit_code() == 1);
        CHECK(!tplReport.usage);
        CHECK(tplReport.runs.size() == 2);
    }
    // Genuinely unused --param on a healthy template or a non-template bad_time scenario still returns Usage (exit 4).
    for (const auto& [sc, unusedName] : std::vector<std::pair<std::string, std::string>>{{"healthy_tpl.yaml", "genuinely_unused"},
                                                                                          {"bad_time.yaml", "unused"}})
    {
        TestOptions usageOpts;
        usageOpts.scenarios = {{sc}};
        usageOpts.request.intent.params = {ParamValue{unusedName, "1", false, ParamSource::CommandLine, 0, true}};
        const TestReport usageReport = test_in(dir, "sim.yaml", usageOpts);
        CHECK(usageReport.result() == TestResult::Usage);
        CHECK(usageReport.exit_code() == 4);
        CHECK(usageReport.usage);
        CHECK(usageReport.runs.empty());
    }

    // A participant that someone starts by hand: every run would wait for a person. Refused
    // before the first run, with what to do; left out with --disable, the test runs.
    write(dir / "manual.yaml", "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                               "  - name: Engine\n    command: [missing/engine]\n"
                               "  - name: Hil\n    command: [missing/hil]\n    manual: true\n"
                               "  - name: Off\n    command: [missing/off]\n    manual: true\n    enabled: false\n");
    std::vector<std::string> lines;
    options.observers.write_line = [&lines](std::string_view line) { lines.emplace_back(line); };
    const TestReport manual = test_in(dir, "manual.yaml", options);
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
    options.request.intent.disable = {"Hil"};
    const TestReport without = test_in(dir, "manual.yaml", options);
    CHECK(without.result() == TestResult::Failed && without.runs.size() == 2 && lines.size() == 4);
    // Taken in with --enable: refused as one that the file enables. With variants: which ones.
    options.request.intent.disable = {"Hil"};
    options.request.intent.enable = {"Off"};
    const TestReport enabled = test_in(dir, "manual.yaml", options);
    CHECK(enabled.result() == TestResult::Usage && enabled.findings.size() == 1 &&
          enabled.findings[0].message.rfind("participant 'Off' is manual (manual: true in the file): ", 0) == 0);
    options.request.intent.disable.clear();
    options.request.intent.enable.clear();
    write(dir / "manual-variants.yaml", "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                                        "  - name: Hil\n    command: [missing/hil]\n    manual: true\n"
                                        "variants:\n  small: {}\n  medium: { participants: { Hil: { enabled: false } } }\n"
                                        "  large: {}\n");
    const TestReport some = test_in(dir, "manual-variants.yaml", options);
    CHECK(some.result() == TestResult::Usage && some.findings.size() == 1 &&
          some.findings[0].message.rfind("participant 'Hil' is manual (manual: true in the file, in the variants small and large): ", 0) == 0);
    // Only the variants that run count.
    options.variants = {"medium"};
    CHECK(test_in(dir, "manual-variants.yaml", options).result() == TestResult::Failed);
}

TEST_CASE(a_test_runs_every_variant_with_every_scenario_in_order)
{
    const auto dir = fresh_dir("batch-order");
    write(dir / "sim.yaml", kConfig);
    write(dir / "plain.yaml", kPlain);
    write(dir / "only.yaml", "vehicle_sil_scenario: 1\nonly: [medium]\nsteps:\n  - set: { Engine.level: 1 }\n");
    TestOptions options;
    options.scenarios = {{"plain.yaml"}, {"only.yaml"}};
    std::vector<std::string> lines;
    options.observers.write_line = [&lines](std::string_view line) { lines.emplace_back(line); };

    // Every check fails (the executable is missing): the runs are all there, failed, in the
    // file's order of the variants. One that is not for the variant fails too while the check has
    // another error: it is skipped only if that is all that is wrong.
    const TestReport all = test_in(dir, "sim.yaml", options);
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
    check_text(results(test_in(dir, "sim.yaml", options)), "medium plain.yaml 1\nmedium only.yaml 1\n");
    options.variants = {"medium", "small"};
    options.fail_fast = true;
    const TestReport fast = test_in(dir, "sim.yaml", options);
    check_text(results(fast), "small plain.yaml 1\nsmall only.yaml 4\nmedium plain.yaml 4\nmedium only.yaml 4\n");
    check_text(fast.runs[3].reason, "a run before it failed (--fail-fast)");
    CHECK(!fast.runs[3].run.has_value() && fast.exit_code() == 1);

    // An interrupt that is already there: the first run is the last.
    StopRequests stops;
    stops.request(1); // the source is not read here (no interrupt_name)
    options.fail_fast = false;
    options.lent.stops = &stops;
    const TestReport stopped = test_in(dir, "sim.yaml", options);
    CHECK(stopped.result() == TestResult::Interrupted && stopped.exit_code() == kExitInterrupted);
    check_text(stopped.runs[1].reason, "the test was interrupted before it");
    CHECK(stopped.count(TestRunResult::NotRun) == 3);
}

/// A participant with role: bridge (describe: false; its executable is missing, so nothing starts).
const char* const kBridged = "vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                             "  - name: Gateway\n    command: [missing/gateway]\n    role: bridge\n    describe: false\n";

std::string codes_of(const CheckReport& check, std::string_view prefix)
{
    std::string out;
    for (const Finding& f : check.findings)
    {
        if (f.code.rfind(prefix, 0) == 0)
        {
            out += f.code + (f.severity == Severity::Error ? " E\n" : " N\n");
        }
    }
    return out;
}

TEST_CASE(a_run_gives_its_check_the_speed_and_start_paused_of_its_request)
{
    const auto dir = fresh_dir("batch-time-lock");
    write(dir / "sim.yaml", kBridged);
    const auto checked = [&dir](RunRequest request, const CheckOptions& options = {}) {
        request.host.config = vehicle_sil::process::utf8_from_path(dir / "sim.yaml");
        const RunReport report = run_file(request, {}, {}, {}, options);
        CHECK(!report.run.has_value());
        return report.check;
    };
    // The request alone: the note, and the errors for what the run asks for.
    RunRequest request;
    CHECK(checked(request).time_lock.locked);
    check_text(codes_of(checked(request), "outside-"), "outside-speed-set N\n");
    request.intent.speed = 2.0;
    const Checked fast = checked(request);
    check_text(codes_of(fast, "outside-"), "outside-real-time E\n");
    CHECK(format_text(fast).find("--speed 2 is refused") != std::string::npos);
    request.intent.speed.reset();
    request.intent.start_paused = true;
    const Checked paused = checked(request);
    check_text(codes_of(paused, "outside-"), "outside-real-time E\noutside-speed-set N\n"); // errors first
    request.intent.speed = 1.0;
    check_text(codes_of(checked(request), "outside-"), "outside-real-time E\n");
    // A run reads the whole, whatever the check's options say: the participants are looked for.
    CheckOptions only;
    only.file_only = true;
    CHECK(checked(RunRequest{}, only).participants.size() == checked(RunRequest{}).participants.size());
    CHECK(!checked(RunRequest{}, only).participants.empty());
    // A test's speed (as fast as possible) is its default: a note, not a refused --speed max.
    request = RunRequest{};
    request.intent.speed.emplace(std::optional<double>{});
    check_text(codes_of(checked(request), "outside-"), "outside-real-time E\n");
    CheckOptions test_options;
    test_options.fastest_by_default = true;
    const Checked test = checked(request, test_options);
    check_text(codes_of(test, "outside-"), "outside-speed-set N\n");
    request.intent.speed = 0.5;
    const Checked locked = checked(request, test_options);
    check_text(codes_of(locked, "outside-"), "outside-speed-set N\n");
    CHECK(locked.run_timing && locked.run_timing->speed == 1.0);
    CHECK(format_text(locked).find("not at the default speed 0.5") != std::string::npos);

    request.intent.time_policy = TimePolicy::Free;
    const Checked unlocked = checked(request, test_options);
    CHECK(unlocked.run_timing && unlocked.run_timing->speed == 0.5);
    CHECK(unlocked.checked_speed == request.intent.speed);
    check_text(codes_of(unlocked, "outside-"), "outside-not-real-time N\n");
    CHECK(format_text(unlocked).find("the simulation runs at the default speed 0.5") != std::string::npos);

    request.intent.speed = 1.0;
    const Checked real_time = checked(request, test_options);
    CHECK(real_time.run_timing && real_time.run_timing->speed == 1.0);
    check_text(codes_of(real_time, "outside-"), "");

    // --time-policy free: the notes for the same requests.
    request = RunRequest{};
    request.intent.time_policy = TimePolicy::Free;
    request.intent.speed = 2.0;
    request.intent.start_paused = true;
    check_text(codes_of(checked(request), "outside-"), "outside-not-real-time N\noutside-not-real-time N\n");
}

TEST_CASE(a_test_says_once_that_the_time_lock_set_its_runs_to_speed_1)
{
    const std::string why = "participant 'Gateway' is a bridge (role: bridge) to something outside the simulation, which does not "
                            "wait for the virtual time: the run goes at speed 1 (real time), not as fast as possible. To go at "
                            "another speed, run with --disable Gateway, or pass --time-policy free if what is connected there "
                            "can take messages that do not come in real time (the bridge still carries frames both ways)";
    RunReport locked;
    locked.check.time_lock = TimeLock{true, {}, {"Gateway"}};
    locked.check.findings = time_lock_findings(locked.check.time_lock, {}, std::nullopt, false, RequestOrigin{});
    CHECK(locked.check.findings.size() == 1 && locked.check.findings.front().message == why);
    TestReport report;
    report.file = "sim.yaml";
    report.variants = {"small", "medium"};
    report.scenarios = {"a.yaml", "b.yaml"};
    report.runs = {{"small", "a.yaml", TestRunResult::Passed, "", RunReport{}},
                   {"small", "b.yaml", TestRunResult::Skipped, "the scenario is only for medium (its only)", {}},
                   {"medium", "a.yaml", TestRunResult::Passed, "", locked},
                   {"medium", "b.yaml", TestRunResult::Passed, "", locked}};
    const std::string note = "2 of 4 runs went at speed 1 (real time), not as fast as possible: each takes as long as its scenario "
                             "does in real time (mind --timeout-ms and the time limit of what runs the test). The first of them "
                             "(scenario a.yaml, variant medium) says why: " + why;
    CHECK(report.remarks().size() == 1);
    CHECK(report.result() == TestResult::Passed);
    check_text(format_test_text(report),
             "variant  scenario  result\n"
             "small    a.yaml    passed\n"
             "small    b.yaml    skipped  the scenario is only for medium (its only)\n"
             "medium   a.yaml    passed\n"
             "medium   b.yaml    passed\n"
             "\n"
             "note: sim.yaml: " + note + " [test-speed-set]\n"
             "\n"
             "passed: 4 runs, 3 passed, 0 failed, 1 skipped\n");
    CHECK(format_test_json(report).find("  \"findings\": [\n    {\n      \"severity\": \"note\",\n      \"code\": \"test-speed-set\",\n") !=
          std::string::npos);
    // --strict does not fail on a note; without variants the note names the scenario only.
    report.strict = true;
    CHECK(report.result() == TestResult::Passed);
    report.variants.clear();
    report.runs = {{"", "a.yaml", TestRunResult::Failed, "an error", locked}};
    CHECK(report.remarks().size() == 1 &&
          report.remarks().front().message.rfind("1 of 1 runs went at speed 1 (real time), not as fast as possible: ", 0) == 0 &&
          report.remarks().front().message.find("The first of them (scenario a.yaml) says why: ") != std::string::npos);
    // No run was set to speed 1 (--time-policy free: another note): nothing to say.
    RunReport freed;
    freed.check.time_lock = TimeLock{false, {}, {"Gateway"}};
    freed.check.findings = time_lock_findings(freed.check.time_lock, {}, std::nullopt, false, RequestOrigin{});
    report.runs = {{"", "a.yaml", TestRunResult::Passed, "", freed}};
    CHECK(report.remarks().empty());
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
                    "      \"result\": \"passed\",\n      \"reason\": null,\n      \"run\": {\n        \"vehicle_sil_run\": 2,\n") !=
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
    report.given.participant_start_timeout_ms = 300;
    report.given.participant_stop_timeout_ms = 400;
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
             "--participant-start-timeout-ms 300 --participant-stop-timeout-ms 400 --timeout-ms 60000 --describe-timeout-ms 500");

    report.runs[0].run->check.run_timing.emplace().speed = 0.5;
    CHECK(format_test_text(report).find("vehicle_sil_launch run 'my sims/sim.yaml' --speed 0.5 --variant") != std::string::npos);

    // A scenario that the check did not read: no --param (the run says what is wrong with it).
    report.runs[0].run = RunReport{};
    report.strict = false;
    report.given.timeout.reset();
    report.given.describe_timeout.reset();
    report.given.participant_start_timeout_ms.reset();
    report.given.participant_stop_timeout_ms.reset();
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
             "error: sim.yaml: " + leftMessage + " [test-runs-left]\n"
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
             "checked nothing: it passes (--strict would fail on that) [test-nothing-run]\n"
             "\n"
             "passed: 2 runs, 0 passed, 0 failed, 2 skipped\n");
    none.strict = true;
    CHECK(none.result() == TestResult::Failed && none.exit_code() == 1);
    const std::string strictText = format_test_text(none);
    CHECK(strictText.find("The test checked nothing, and --strict fails on that [test-nothing-run]\n\nFAILED: 2 runs, 0 passed, 0 failed, 2 skipped (--strict)\n") !=
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

TEST_CASE(batch_named_cases_expansion_filtering_rerun_and_formatting)
{
    const auto dir = fresh_dir("batch-cases");
    std::filesystem::create_directories(dir / "templates");
    write(dir / "sim.yaml", kConfig);
    write(dir / "templates" / "inner.yaml",
          "vehicle_sil_scenario_template: 1\nparameters:\n  x: { type: integer }\nsteps:\n  - set: { Engine.level: \"${x}\" }\n");
    write(dir / "a_cases.yaml",
          "vehicle_sil_scenario: 1\nparams: { x: 1 }\n"
          "cases:\n  無負荷: { params: { x: 0 } }\n  定格: { only: [medium], params: { x: 50 } }\n  \"-10C\": { params: { x: 20 } }\n"
          "steps:\n  - template: templates/inner.yaml\n    params: { x: \"${x}\" }\n");
    write(dir / "b_caseless.yaml", kPlain);
    write(dir / "c_only_b.yaml",
          "vehicle_sil_scenario: 1\nparams: { z: 1 }\n"
          "cases:\n  extra_only: { params: { z: 2 } }\n"
          "steps:\n  - set: { Engine.level: \"${z}\" }\n");

    // 1. Omitting --case expands (variant, scenario, case) in file order.
    TestOptions allOpts;
    allOpts.scenarios = {{"a_cases.yaml"}, {"b_caseless.yaml"}};
    const TestReport allReport = test_in(dir, "sim.yaml", allOpts);
    // 2 variants * (3 cases + 1 caseless) = 8 runs.
    CHECK_EQ(allReport.runs.size(), std::size_t{8});
    CHECK(allReport.runs[0].variant == "small" && allReport.runs[0].scenario == "a_cases.yaml" && allReport.runs[0].case_name == "無負荷");
    CHECK(allReport.runs[1].variant == "small" && allReport.runs[1].scenario == "a_cases.yaml" && allReport.runs[1].case_name == "定格");
    CHECK(allReport.runs[1].run && allReport.runs[1].run->check.scenario && allReport.runs[1].run->check.scenario->not_for_variant);
    CHECK(allReport.runs[2].variant == "small" && allReport.runs[2].scenario == "a_cases.yaml" && allReport.runs[2].case_name == "-10C");
    CHECK(allReport.runs[3].variant == "small" && allReport.runs[3].scenario == "b_caseless.yaml" && allReport.runs[3].case_name.empty());

    // Text table includes case column with "-" for caseless rows, and run_alone appends --case.
    const std::string text = format_test_text(allReport);
    CHECK(text.find("variant  scenario         case") != std::string::npos);
    CHECK(text.find("b_caseless.yaml  -") != std::string::npos);
    CHECK(text.find("--scenario a_cases.yaml --case '無負荷'") != std::string::npos);

    // JSON emits "case" after "run" only when non-empty.
    const std::string json = format_test_json(allReport);
    CHECK(json.find("\"case\": \"定格\"") != std::string::npos);
    CHECK(json.find("\"scenario\": \"b_caseless.yaml\",\n      \"result\": \"failed\"") != std::string::npos);

    // 2. Passing --case filters in file order and skips caseless scenarios.
    TestOptions filterOpts;
    filterOpts.scenarios = {{"a_cases.yaml"}, {"b_caseless.yaml"}};
    filterOpts.cases = {"-10C", "無負荷"};
    const TestReport filtered = test_in(dir, "sim.yaml", filterOpts);
    CHECK_EQ(filtered.runs.size(), std::size_t{4});
    CHECK(filtered.runs[0].case_name == "無負荷");
    CHECK(filtered.runs[1].case_name == "-10C");
    CHECK(filtered.runs[2].case_name == "無負荷");
    CHECK(filtered.runs[3].case_name == "-10C");

    // 3. Unknown --case returns Usage (exit 4) with singular/plural grammar + "did you mean", and --case on all-caseless returns Usage (exit 4).
    TestOptions unknownCaseOpts;
    unknownCaseOpts.scenarios = {{"a_cases.yaml"}};
    unknownCaseOpts.cases = {"定格X"};
    const TestReport unknownCase = test_in(dir, "sim.yaml", unknownCaseOpts);
    CHECK(unknownCase.result() == TestResult::Usage && unknownCase.exit_code() == 4);
    CHECK(unknownCase.findings.front().message.find("the scenario has no case with this name") != std::string::npos);
    CHECK(unknownCase.findings.front().message.find("did you mean '定格'?") != std::string::npos);

    TestOptions unknownPluralOpts;
    unknownPluralOpts.scenarios = {{"a_cases.yaml"}, {"c_only_b.yaml"}};
    unknownPluralOpts.cases = {"nonexistent"};
    const TestReport unknownPlural = test_in(dir, "sim.yaml", unknownPluralOpts);
    CHECK(unknownPlural.result() == TestResult::Usage && unknownPlural.exit_code() == 4);
    CHECK(unknownPlural.findings.front().message.find("none of the 2 scenarios has a case with this name") != std::string::npos);

    TestOptions caselessOnlyOpts;
    caselessOnlyOpts.scenarios = {{"b_caseless.yaml"}};
    caselessOnlyOpts.cases = {"無負荷"};
    const TestReport caselessOnly = test_in(dir, "sim.yaml", caselessOnlyOpts);
    CHECK(caselessOnly.result() == TestResult::Usage && caselessOnly.exit_code() == 4);

    // 4. unused_params() respects --case filtering (rejects --param used only by skipped scenario/case).
    TestOptions unusedOpts;
    unusedOpts.scenarios = {{"a_cases.yaml"}, {"c_only_b.yaml"}};
    unusedOpts.cases = {"無負荷"};
    unusedOpts.request.intent.params = {ParamValue{"z", "2", false, ParamSource::CommandLine, 0, true}};
    const TestReport unusedReport = test_in(dir, "sim.yaml", unusedOpts);
    CHECK(unusedReport.result() == TestResult::Usage && unusedReport.exit_code() == 4);

    // 4b. Broken cases: ({}, 123, "--bad") never silently pass (exit 0); they always fail (exit 1).
    write(dir / "empty_cases.yaml", "vehicle_sil_scenario: 1\ncases: {}\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "int_cases.yaml", "vehicle_sil_scenario: 1\ncases: 123\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "dashdash_cases.yaml", "vehicle_sil_scenario: 1\ncases: { \"--bad\": {} }\nsteps:\n  - set: { Engine.level: 1 }\n");
    for (const auto& [badFile, expectMsg] : std::vector<std::pair<std::string, std::string>>{
             {"empty_cases.yaml", "cases must be a non-empty mapping"},
             {"int_cases.yaml", "cases must be a non-empty mapping"},
             {"dashdash_cases.yaml", "does not start with '--'"}})
    {
        for (const std::vector<TestScenario>& scList :
             std::vector<std::vector<TestScenario>>{{{badFile}}, {{"a_cases.yaml"}, {badFile}}})
        {
            for (const std::vector<std::string>& caseFilter :
                 std::vector<std::vector<std::string>>{{}, {"無負荷"}, {"nonexistent"}})
            {
                TestOptions opts;
                opts.scenarios = scList;
                opts.cases = caseFilter;
                if (!caseFilter.empty() && caseFilter.front() == "無負荷")
                {
                    opts.request.intent.params = {ParamValue{"bogus", "1", false, ParamSource::CommandLine, 0, true}};
                }
                const TestReport rep = test_in(dir, "sim.yaml", opts);
                CHECK(rep.result() == TestResult::Failed);
                CHECK_EQ(rep.exit_code(), 1);
                CHECK(!rep.usage);
                CHECK(std::any_of(rep.runs.begin(), rep.runs.end(), [&](const TestRun& r) {
                    return r.scenario == badFile && r.result == TestRunResult::Failed && r.run &&
                           std::any_of(r.run->check.findings.begin(), r.run->check.findings.end(), [&](const Finding& f) {
                               return f.severity == Severity::Error && f.message.find(expectMsg) != std::string::npos;
                           });
                }));
            }
        }
    }

    // 4c. Non-allowlisted broken scenarios (unknown-key, scenario-variant, value, duplicate-key) are surfaced, not skipped under --case + --param.
    write(dir / "typo_cases.yaml",
          "vehicle_sil_scenario: 1\ncase: { a: { params: { bogus: 1 } } }\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "bad_only.yaml",
          "vehicle_sil_scenario: 1\nonly: [nonexistent]\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "bad_name.yaml",
          "vehicle_sil_scenario: 1\nname: [1, 2]\nsteps:\n  - set: { Engine.level: 1 }\n");
    write(dir / "dup_top.yaml",
          "vehicle_sil_scenario: 1\nname: a\nname: b\nsteps:\n  - set: { Engine.level: 1 }\n");
    for (const auto& [badFile, expectMsg] : std::vector<std::pair<std::string, std::string>>{
             {"typo_cases.yaml", "unknown key 'case'"},
             {"bad_only.yaml", "does not define"},
             {"bad_name.yaml", "name must be a text"},
             {"dup_top.yaml", "'name' appears twice"}})
    {
        TestOptions opts;
        opts.scenarios = {{"a_cases.yaml"}, {badFile}};
        opts.cases = {"無負荷"};
        opts.request.intent.params = {ParamValue{"bogus", "1", false, ParamSource::CommandLine, 0, true}};
        const TestReport rep = test_in(dir, "sim.yaml", opts);
        CHECK(rep.result() == TestResult::Failed);
        CHECK_EQ(rep.exit_code(), 1);
        CHECK(!rep.usage);
        CHECK(std::any_of(rep.runs.begin(), rep.runs.end(), [&](const TestRun& r) {
            return r.scenario == badFile && r.result == TestRunResult::Failed && r.run &&
                   std::any_of(r.run->check.findings.begin(), r.run->check.findings.end(), [&](const Finding& f) {
                       return f.severity == Severity::Error && f.message.find(expectMsg) != std::string::npos;
                   }) &&
                   std::none_of(r.run->check.findings.begin(), r.run->check.findings.end(), [&](const Finding& f) {
                       return f.message.find("has no cases") != std::string::npos;
                   });
        }));
    }

    // 4d. Healthy caseless scenarios with only: or declared params are skipped cleanly under --case and never trigger false skips.
    write(dir / "only_medium.yaml", "vehicle_sil_scenario: 1\nonly: [medium]\nsteps:\n  - set: { Engine.level: 1 }\n");
    {
        TestOptions opts;
        opts.scenarios = {{"a_cases.yaml"}, {"only_medium.yaml"}};
        opts.cases = {"無負荷"};
        opts.request.intent.params = {ParamValue{"bogus", "1", false, ParamSource::CommandLine, 0, true}};
        const TestReport rep = test_in(dir, "sim.yaml", opts);
        CHECK(rep.result() == TestResult::Usage);
        CHECK_EQ(rep.exit_code(), 4);
    }
    write(dir / "caseless_param.yaml",
          "vehicle_sil_scenario: 1\nparams: { fill: 1 }\nsteps:\n  - set: { Engine.level: \"${fill}\" }\n");
    {
        TestOptions opts;
        opts.scenarios = {{"a_cases.yaml"}, {"caseless_param.yaml"}};
        opts.cases = {"無負荷"};
        opts.request.intent.params = {ParamValue{"fill", "2", false, ParamSource::CommandLine, 0, true}};
        const TestReport rep = test_in(dir, "sim.yaml", opts);
        CHECK(rep.result() == TestResult::Usage);
        CHECK_EQ(rep.exit_code(), 4);
    }

    // 5. failure_reason() prefixes template file:line when first->file != run.scenario, including post-pass fail_grid.
    RunReport tplFail;
    tplFail.check.scenario.emplace();
    tplFail.check.scenario->file = "a_cases.yaml";
    tplFail.findings.push_back(Finding{Severity::Error, "scenario-failed", "check failed", 24, "", "", "",
                                       "templates/inner.yaml"});
    TestReport reasonReport;
    reasonReport.file = "sim.yaml";
    reasonReport.scenarios = {"a_cases.yaml"};
    TestRun tr;
    tr.variant = "small";
    tr.scenario = "a_cases.yaml";
    tr.case_name = "-10C";
    tr.result = TestRunResult::Failed;
    tr.reason = "templates/inner.yaml:24: check failed";
    tr.run = tplFail;
    reasonReport.runs.push_back(tr);
    const std::string reasonText = format_test_text(reasonReport);
    CHECK(reasonText.find("templates/inner.yaml:24: check failed") != std::string::npos);
    CHECK(reasonText.find("--variant small --scenario a_cases.yaml --case -10C") != std::string::npos);

    RunReport gridRep;
    gridRep.check.scenario.emplace();
    gridRep.check.scenario->file = "a_cases.yaml";
    gridRep.findings.push_back(Finding{Severity::Error, "scenario-write",
                                       "'inject dither': the dither of Engine.level (templates/inner.yaml:5) wrote 102 at 40 ms",
                                       5, "", "", "called from: a_cases.yaml:10 (step 'run inner')\ncase: 無負荷",
                                       "templates/inner.yaml"});
    TestReport gridReport;
    gridReport.file = "sim.yaml";
    gridReport.scenarios = {"a_cases.yaml"};
    TestRun gridTr;
    gridTr.variant = "small";
    gridTr.scenario = "a_cases.yaml";
    gridTr.case_name = "無負荷";
    gridTr.result = TestRunResult::Failed;
    gridTr.reason = "templates/inner.yaml:5: 'inject dither': the dither of Engine.level (templates/inner.yaml:5) wrote 102 at 40 ms";
    gridTr.run = gridRep;
    gridReport.runs.push_back(gridTr);
    const std::string gridText = format_test_text(gridReport);
    CHECK(gridText.find("templates/inner.yaml:5: 'inject dither': the dither of Engine.level") != std::string::npos);
    CHECK(gridText.find("--variant small --scenario a_cases.yaml --case '無負荷'") != std::string::npos);

    // 6. Bug A-1: failure_reason() includes template file even when first->line == 0 (empty template).
    write(dir / "sim_ok_exec.yaml",
          std::string("vehicle_sil_simulation: 2\nnetworks: []\nparticipants:\n"
                      "  - name: Engine\n    command: [\"") +
              VEHICLE_SIL_PROCESS_HELPER +
              "\"]\n    describe: false\n"
              "variants:\n  small: {}\n  medium: {}\n");
    write(dir / "templates" / "empty_tpl.yaml", "");
    write(dir / "calls_empty_tpl.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - template: templates/empty_tpl.yaml\n");
    {
        TestOptions opts;
        opts.variants = {"small"};
        opts.scenarios = {{"calls_empty_tpl.yaml"}};
        const TestReport emptyRep = test_in(dir, "sim_ok_exec.yaml", opts);
        CHECK_EQ(emptyRep.exit_code(), 1);
        CHECK_EQ(emptyRep.runs.size(), std::size_t{1});
        CHECK(emptyRep.runs[0].result == TestRunResult::Failed);
        CHECK(emptyRep.runs[0].reason.rfind("templates/empty_tpl.yaml: the template file is empty", 0) == 0);
    }

    // 7. Bug A-2 & Gap 6: singular unused_params() wording with --case + --param, caseless no-params singular wording,
    // broken unselected case under --case + --param, empty only: intersection (not_for_variant == false -> fail),
    // and run_alone() shell metacharacters (', ", $, \).
    {
        TestOptions opts;
        opts.variants = {"small"};
        opts.scenarios = {{"a_cases.yaml"}};
        opts.cases = {"無負荷"};
        opts.request.intent.params = {ParamValue{"bogus", "1", false, ParamSource::CommandLine, 0, true}};
        const TestReport singleUnused = test_in(dir, "sim.yaml", opts);
        CHECK_EQ(singleUnused.exit_code(), 4);
        CHECK(singleUnused.findings.size() == 1);
        CHECK(singleUnused.findings[0].message.find("--param bogus=1: the scenario has no parameter bogus: its parameters are x") !=
              std::string::npos);
    }
    write(dir / "caseless_noparams.yaml", "vehicle_sil_scenario: 1\nsteps:\n  - set: { Engine.level: 1 }\n");
    {
        TestOptions opts;
        opts.variants = {"small"};
        opts.scenarios = {{"caseless_noparams.yaml"}};
        opts.request.intent.params = {ParamValue{"bogus", "1", false, ParamSource::CommandLine, 0, true}};
        const TestReport noneUnused = test_in(dir, "sim.yaml", opts);
        CHECK_EQ(noneUnused.exit_code(), 4);
        CHECK(noneUnused.findings.size() == 1);
        CHECK(noneUnused.findings[0].message.find("--param bogus=1: the scenario has no parameter bogus: it uses none") !=
              std::string::npos);
    }
    write(dir / "broken_unselected_case.yaml",
          "vehicle_sil_scenario: 1\nparams: { x: 1 }\n"
          "cases:\n  good: { params: { x: 5 } }\n  bad: { params: { typo: 99 } }\n"
          "steps:\n  - template: templates/inner.yaml\n    params: { x: \"${x}\" }\n");
    {
        TestOptions opts;
        opts.variants = {"small"};
        opts.scenarios = {{"broken_unselected_case.yaml"}};
        opts.cases = {"good"};
        opts.request.intent.params = {ParamValue{"bogus", "1", false, ParamSource::CommandLine, 0, true}};
        const TestReport brokenOther = test_in(dir, "sim.yaml", opts);
        CHECK(brokenOther.result() == TestResult::Failed);
        CHECK_EQ(brokenOther.exit_code(), 1);
        CHECK(!brokenOther.usage);
    }
    write(dir / "empty_only_inter.yaml",
          "vehicle_sil_scenario: 1\nonly: [small]\n"
          "cases:\n  c_empty: { only: [medium] }\n"
          "steps:\n  - set: { Engine.level: 1 }\n");
    {
        TestOptions opts;
        opts.variants = {"small"};
        opts.scenarios = {{"empty_only_inter.yaml"}};
        const TestReport emptyInter = test_in(dir, "sim_ok_exec.yaml", opts);
        CHECK(emptyInter.result() == TestResult::Failed);
        CHECK_EQ(emptyInter.exit_code(), 1);
        CHECK_EQ(emptyInter.runs.size(), std::size_t{1});
        CHECK(emptyInter.runs[0].result == TestRunResult::Failed);
        CHECK(emptyInter.runs[0].reason.find("have no variant in common") != std::string::npos);
    }
    {
        TestReport metaReport;
        metaReport.file = "sim.yaml";
        metaReport.scenarios = {"a_cases.yaml"};
        TestRun metaTr;
        metaTr.variant = "small";
        metaTr.scenario = "a_cases.yaml";
        metaTr.case_name = "case 'q' \"dq\" $var \\bs";
        metaTr.result = TestRunResult::Failed;
        metaTr.reason = "failed";
        metaReport.runs.push_back(metaTr);
        const std::string metaText = format_test_text(metaReport);
        CHECK(metaText.find("--case 'case '\\''q'\\'' \"dq\" $var \\bs'") != std::string::npos);
    }
}
