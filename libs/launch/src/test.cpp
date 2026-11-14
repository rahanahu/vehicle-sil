// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch test: the list of the scenario files, the runs one after the other
// (test_file()) and the report for people and as JSON.

#include "vehicle_sil/launch/test.hpp"

#include "check_format.hpp"
#include "check_words.hpp"
#include "run_words.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace vehicle_sil::launch {

namespace {

/// The largest scenario file read for its parameters (as the check's limit).
constexpr std::uintmax_t kMaxScenarioBytes = std::uintmax_t{16} << 20;

Finding usage(std::string message) { return Finding{Severity::Error, "usage", std::move(message), 0, "", ""}; }

/// true if `name` ends in .yaml, .yml or .json (ASCII letters of either case).
bool is_scenario_name(std::string_view name)
{
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
    {
        return false;
    }
    std::string extension(name.substr(dot));
    for (char& c : extension)
    {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return extension == ".yaml" || extension == ".yml" || extension == ".json";
}

const char* result_name(TestRunResult result)
{
    switch (result)
    {
    case TestRunResult::Passed: return "passed";
    case TestRunResult::Failed: return "failed";
    case TestRunResult::Skipped: return "skipped";
    case TestRunResult::Interrupted: return "interrupted";
    case TestRunResult::NotRun: return "not_run";
    }
    return "failed";
}

/// As the table and the lines show it: a failure in capitals, like the run's result.
const char* result_text(TestRunResult result)
{
    switch (result)
    {
    case TestRunResult::Failed: return "FAILED";
    case TestRunResult::NotRun: return "not run";
    default: return result_name(result);
    }
}

const char* result_name(TestResult result)
{
    switch (result)
    {
    case TestResult::Passed: return "passed";
    case TestResult::Failed: return "failed";
    case TestResult::Interrupted: return "interrupted";
    case TestResult::Unreadable: return "unreadable";
    case TestResult::Usage: return "usage";
    }
    return "failed";
}

/// Why a run failed, in one line: its first error (the check's before the run's; with --strict
/// and no error, its first warning), with the line of the scenario if it has one, and how many
/// more there are.
std::string failure_reason(const RunReport& report)
{
    const Severity severity = report.errors() > 0 ? Severity::Error : Severity::Warning;
    const Finding* first = nullptr;
    std::size_t count = 0;
    for (const std::vector<Finding>* findings : {&report.check.findings, &report.findings})
    {
        for (const Finding& finding : *findings)
        {
            if (finding.severity == severity)
            {
                first = first == nullptr ? &finding : first;
                ++count;
            }
        }
    }
    if (first == nullptr)
    {
        return report.run && report.run->simulation != SimulationEnd::Ended && report.run->simulation != SimulationEnd::ReachedStopAt
                   ? "the simulation did not run to its end"
                   : "";
    }
    const char* kind = severity == Severity::Error ? "error" : "warning";
    return (!first->file.empty() && first->line > 0 ? "line " + std::to_string(first->line) + ": " : std::string{}) + first->message +
           (severity == Severity::Warning ? " (a warning, and --strict fails on it)" : "") +
           (count > 1 ? " (and " + std::to_string(count - 1) + " more " + kind + (count == 2 ? "" : "s") + ")" : std::string{});
}

/// What became of one run.
void judge(TestRun& run, RunReport report)
{
    const std::optional<ScenarioCheck>& scenario = report.check.scenario;
    // Not for this variant, and nothing else is wrong: skipped. (The error that says so is one
    // finding; any other error is of the file or of the scenario, and fails the run.)
    if (scenario && scenario->not_for_variant && report.check.errors() == 1)
    {
        run.result = TestRunResult::Skipped;
        run.reason = "the scenario is only for " + detail::and_list(scenario->only.value_or(std::vector<std::string>{})) + " (its only)";
        return;
    }
    switch (report.result())
    {
    case RunResult::Passed:
        run.result = TestRunResult::Passed;
        if (report.run && report.run->scenario && report.run->scenario->skipped() > 0)
        {
            const std::size_t skipped = report.run->scenario->skipped();
            run.reason = std::to_string(skipped) + " of " + std::to_string(report.run->scenario->steps.size()) +
                         (skipped == 1 ? " steps was skipped" : " steps were skipped");
        }
        // It passed with warnings (no --strict): how many, so that the table does not hide them.
        if (const std::size_t warnings = report.warnings(); warnings > 0)
        {
            run.reason += (run.reason.empty() ? "" : ", ") + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
        }
        break;
    case RunResult::Interrupted:
        run.result = TestRunResult::Interrupted;
        run.reason = report.run && !report.run->interrupted_by.empty() ? "by " + report.run->interrupted_by : "";
        break;
    default:
        run.result = TestRunResult::Failed;
        run.reason = failure_reason(report);
        break;
    }
    run.run = std::move(report);
}

/// The parameters of the command line that no scenario declares or uses: a usage error each (a
/// typing error would otherwise change nothing, silently). Nothing if a scenario cannot be read
/// that far: its run says what is wrong with it, and the parameter may be in it. `configured`:
/// the parameters the configuration gives a value (in any of the variants that run), so that a
/// scenario's parameter without a default of its own is found too.
std::vector<Finding> unused_params(const std::vector<TestScenario>& scenarios, const std::vector<ParamValue>& configured,
                                   const std::vector<ParamValue>& given)
{
    if (given.empty())
    {
        return {};
    }
    std::vector<std::string> names;
    for (const TestScenario& scenario : scenarios)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(scenario.path, ec) || std::filesystem::file_size(scenario.path, ec) > kMaxScenarioBytes)
        {
            return {};
        }
        std::ifstream in(scenario.path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        ScenarioContext context;
        context.params = configured;
        context.params.insert(context.params.end(), given.begin(), given.end());
        context.params_for_many = true;
        // Without the participants: the errors about the variables are not looked at.
        const ScenarioRead read = read_scenario(text, {}, context);
        for (const Finding& finding : read.findings)
        {
            if (finding.code == "syntax" || finding.code == "not-a-scenario" || finding.code == "unsupported-yaml")
            {
                return {};
            }
        }
        for (const ParamValue& param : read.params)
        {
            if (std::find(names.begin(), names.end(), param.name) == names.end())
            {
                names.push_back(param.name);
            }
        }
    }
    std::vector<Finding> errors;
    for (const ParamValue& param : given)
    {
        if (std::find(names.begin(), names.end(), param.name) != names.end())
        {
            continue;
        }
        const std::string guess = detail::closest(param.name, names);
        const std::string which = scenarios.size() == 1 ? "the scenario has" : "none of the " + std::to_string(scenarios.size()) + " scenarios has";
        errors.push_back(usage("--param " + param.name + "=" + param.text + ": " + which + " a parameter " + param.name +
                               (guess.empty() ? "" : " (did you mean " + guess + "?)") + ": " +
                               (names.empty() ? std::string("they use none (a step uses one as ${name})")
                                              : "their parameters are " + detail::join(names)) +
                               "; remove the option or correct the name"));
    }
    return errors;
}

/// The participants that someone starts by hand (enabled and manual) in a configuration a test
/// would run: a usage error each. test starts every run by itself, and each run would wait for a
/// person (without --timeout-ms: for ever). `configs`: the variant ("" without variants) and its
/// configuration, after --disable and --enable.
std::vector<Finding> manual_participants(const std::vector<std::pair<std::string, const SimulationConfig*>>& configs)
{
    std::vector<std::pair<std::string, std::vector<std::string>>> found; // the participant, the variants it is manual in
    for (const auto& [variant, config] : configs)
    {
        for (const ParticipantConfig& p : config->participants)
        {
            if (!p.enabled || !p.manual)
            {
                continue;
            }
            auto it = std::find_if(found.begin(), found.end(), [&p](const auto& one) { return one.first == p.name; });
            it = it == found.end() ? found.insert(found.end(), {p.name, {}}) : it;
            if (!variant.empty())
            {
                it->second.push_back(variant);
            }
        }
    }
    std::vector<Finding> errors;
    for (const auto& [name, variants] : found)
    {
        errors.push_back(usage("participant '" + name + "' is manual (manual: true in the file" +
                               (variants.empty() ? std::string{}
                                                 : std::string(variants.size() == 1 ? ", in the variant " : ", in the variants ") +
                                                       detail::and_list(variants)) +
                               "): someone starts it by hand, and test starts every run by itself, so each run would wait "
                               "for it: leave it out with --disable " + name +
                               " (a scenario with a step that uses its variables then fails its check), or run one scenario "
                               "with it yourself: vehicle_sil_launch run <file> --scenario <file>"));
    }
    return errors;
}

/// `text` as one word of a POSIX shell: as it is if it has only letters, digits and _@%+=:,./-
/// (ASCII), in single quotes otherwise.
std::string shell_word(std::string_view text)
{
    const auto plain = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("_@%+=:,./-").find(c) != std::string_view::npos;
    };
    if (!text.empty() && std::all_of(text.begin(), text.end(), plain))
    {
        return std::string(text);
    }
    std::string out = "'";
    for (const char c : text)
    {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

/// The command that runs `run` alone as the test ran it: the options of the test that a run
/// takes. A --param only if the run's scenario has a parameter of that name (run refuses one it
/// does not have); none if the scenario was not read that far.
std::string run_alone(const TestReport& report, const TestRun& run)
{
    std::string out = "vehicle_sil_launch run " + shell_word(report.file) + " --speed max" +
                      (run.variant.empty() ? std::string{} : " --variant " + shell_word(run.variant)) + " --scenario " +
                      shell_word(run.scenario);
    if (run.run && run.run->check.scenario)
    {
        const std::vector<ParamValue>& has = run.run->check.scenario->params;
        for (const ParamValue& param : report.given.params)
        {
            if (std::any_of(has.begin(), has.end(), [&param](const ParamValue& one) { return one.name == param.name; }))
            {
                out += " --param " + shell_word(param.name + "=" + param.text);
            }
        }
    }
    for (const std::string& name : report.given.disable)
    {
        out += " --disable " + shell_word(name);
    }
    for (const std::string& name : report.given.enable)
    {
        out += " --enable " + shell_word(name);
    }
    out += report.strict ? " --strict" : "";
    if (report.given.timeout)
    {
        out += " --timeout-ms " + std::to_string(report.given.timeout->count());
    }
    if (report.given.describe_timeout)
    {
        out += " --describe-timeout-ms " + std::to_string(report.given.describe_timeout->count());
    }
    return out;
}

/// "variant small" / "variants small and medium".
std::string variants_text(const std::vector<std::string>& variants)
{
    return (variants.size() == 1 ? "variant " : "variants ") + detail::and_list(variants);
}

} // namespace

ScenarioList list_scenarios(const std::vector<std::string>& given)
{
    ScenarioList out;
    for (const std::string& one : given)
    {
        const std::filesystem::path path = process::path_from_utf8(one);
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec))
        {
            if (!std::filesystem::exists(path, ec))
            {
                out.errors.push_back(usage("--scenarios " + one +
                                           ": there is no such file or directory: check the path (it is relative to the "
                                           "current directory, as the configuration file is)"));
                continue;
            }
            out.scenarios.push_back(TestScenario{path, one});
            continue;
        }
        std::vector<std::pair<std::string, std::filesystem::path>> found; // the name as UTF-8, the file
        std::filesystem::directory_iterator it(path, ec);
        for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            std::error_code ignored;
            std::string name = process::utf8_from_path(it->path().filename());
            if (it->is_regular_file(ignored) && is_scenario_name(name))
            {
                found.emplace_back(std::move(name), it->path());
            }
        }
        if (ec)
        {
            out.errors.push_back(usage("--scenarios " + one + ": cannot list the directory (" + ec.message() +
                                       "): check its permissions, or name the scenario files themselves"));
            continue;
        }
        if (found.empty())
        {
            out.errors.push_back(usage("--scenarios " + one +
                                       ": the directory has no scenario file (a file directly in it whose name ends in "
                                       ".yaml, .yml or .json; subdirectories are not searched): name the directory that "
                                       "holds the scenarios, or the files themselves"));
            continue;
        }
        // The order of the names as bytes: the same on every system, whatever order it lists them in.
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const bool separated = !one.empty() && (one.back() == '/' || one.back() == '\\');
        for (auto& [name, file] : found)
        {
            out.scenarios.push_back(TestScenario{std::move(file), one + (separated ? "" : "/") + name});
        }
    }
    return out;
}

std::size_t TestReport::count(TestRunResult result) const
{
    return static_cast<std::size_t>(std::count_if(runs.begin(), runs.end(), [result](const TestRun& run) { return run.result == result; }));
}

std::size_t TestReport::ran() const
{
    return count(TestRunResult::Passed) + count(TestRunResult::Failed) + count(TestRunResult::Interrupted);
}

std::vector<Finding> TestReport::remarks() const
{
    std::vector<Finding> out;
    if (usage || unreadable || interrupted)
    {
        return out;
    }
    if (const std::size_t left = count(TestRunResult::NotRun); left > 0 && connection_not_closed)
    {
        // The run that left the connection open: the one before the first that was not run.
        const auto first = std::find_if(runs.begin(), runs.end(), [](const TestRun& run) { return run.result == TestRunResult::NotRun; });
        const std::string after = first == runs.begin() ? std::string{}
                                                        : " after the run of scenario " + std::prev(first)->scenario +
                                                              (std::prev(first)->variant.empty()
                                                                   ? std::string{}
                                                                   : " (variant " + std::prev(first)->variant + ")");
        out.push_back(Finding{Severity::Error, "test-runs-left",
                              std::to_string(left) + " of " + std::to_string(runs.size()) + (left == 1 ? " runs was" : " runs were") +
                                  " not run: the launcher's connection to the simulation did not close" + after +
                                  ", and the launcher ends its process rather than start another simulation in it. Run the "
                                  "test again for the runs that are 'not run' (--variant and --scenarios choose them), and "
                                  "see that run's warning connection-not-closed",
                              0, "", ""});
    }
    if (ran() == 0 && count(TestRunResult::NotRun) == 0)
    {
        std::string why = "there is no scenario to run";
        if (!runs.empty())
        {
            why = (runs.size() == 1 ? std::string("the 1 run was skipped") : "all " + std::to_string(runs.size()) + " runs were skipped") +
                  ", because no scenario is for " + (variants.empty() ? std::string("this configuration") : "the " + variants_text(variants)) +
                  " (the only of each scenario" + (given.variants.empty() ? "" : "; --variant chose what runs") +
                  "). Run a variant the scenarios are for, or correct their only";
        }
        out.push_back(Finding{Severity::Warning, "test-nothing-run",
                              "nothing was run: " + why + ". The test checked nothing" +
                                  (strict ? ", and --strict fails on that" : ": it passes (--strict would fail on that)"),
                              0, "", ""});
    }
    return out;
}

TestResult TestReport::result() const
{
    if (usage || unreadable || interrupted)
    {
        return usage ? TestResult::Usage : unreadable ? TestResult::Unreadable : TestResult::Interrupted;
    }
    // Not interrupted, and yet runs are left: the test did not do what it was asked to, whatever
    // the runs before did. With --strict: a test that ran nothing (a warning) fails too.
    const bool failed = count(TestRunResult::Failed) > 0 || count(TestRunResult::NotRun) > 0 || (strict && ran() == 0);
    return failed ? TestResult::Failed : TestResult::Passed;
}

int TestReport::exit_code() const
{
    switch (result())
    {
    case TestResult::Passed: return 0;
    case TestResult::Failed: return 1;
    case TestResult::Unreadable: return 3;
    case TestResult::Usage: return 4;
    case TestResult::Interrupted: return kExitInterrupted;
    }
    return 1;
}

TestReport test_file(const std::filesystem::path& path, const std::string& shown_as, const TestOptions& options)
{
    TestReport report;
    report.file = shown_as;
    report.strict = options.run.check.strict;
    report.given.variants = options.variants;
    report.given.params = options.run.check.params;
    report.given.disable = options.run.check.overrides.disable;
    report.given.enable = options.run.check.overrides.enable;
    report.given.timeout = options.run.timeout;
    if (options.run.check.describe_timeout != CheckOptions{}.describe_timeout)
    {
        report.given.describe_timeout = options.run.check.describe_timeout;
    }
    for (const TestScenario& scenario : options.scenarios)
    {
        report.scenarios.push_back(scenario.shown);
    }
    const auto refuse = [&report](std::vector<Finding> errors) {
        report.usage = true;
        report.findings = std::move(errors);
        return report;
    };

    // The file alone, without a variant: what it defines. For a file with variants this is the
    // usage error of a check without --variant, which lists them.
    CheckOptions read = options.run.check;
    read.file_only = true;
    read.overrides.variant.reset();
    const CheckReport file = check_file(path, shown_as, read);
    const bool hasVariants = !file.variants.empty();
    if (file.usage && !hasVariants)
    {
        return refuse(file.findings);
    }
    if (!file.usage && !file.config)
    {
        report.unreadable = true;
        report.findings = file.findings;
        return report;
    }
    // Each --variant as a check takes it: the same errors, in the same words.
    for (const std::string& variant : options.variants)
    {
        read.overrides.variant = variant;
        if (CheckReport chosen = check_file(path, shown_as, read); chosen.usage)
        {
            return refuse(std::move(chosen.findings));
        }
    }
    if (hasVariants)
    {
        for (const std::string& variant : file.variants)
        {
            if (options.variants.empty() || std::find(options.variants.begin(), options.variants.end(), variant) != options.variants.end())
            {
                report.variants.push_back(variant);
            }
        }
    }
    // The configuration of each variant that runs (the file's own if it defines none), after
    // --disable and --enable. One that cannot be read is left out here: its runs say why.
    std::vector<CheckReport> chosen;
    for (const std::string& variant : report.variants)
    {
        read.overrides.variant = variant;
        chosen.push_back(check_file(path, shown_as, read));
        if (chosen.back().usage)
        {
            // Of the command line (--disable <no such name>), so of every run.
            return refuse(std::move(chosen.back().findings));
        }
    }
    std::vector<std::pair<std::string, const SimulationConfig*>> configs;
    if (!hasVariants && file.config)
    {
        configs.emplace_back(std::string{}, &*file.config);
    }
    for (std::size_t i = 0; i < chosen.size(); ++i)
    {
        if (chosen[i].config)
        {
            configs.emplace_back(report.variants[i], &*chosen[i].config);
        }
    }
    if (auto manual = manual_participants(configs); !manual.empty())
    {
        return refuse(std::move(manual));
    }
    if (!options.run.check.params.empty())
    {
        std::vector<ParamValue> configured;
        for (const auto& [variant, config] : configs)
        {
            configured.insert(configured.end(), config->params.begin(), config->params.end());
        }
        if (auto unused = unused_params(options.scenarios, configured, options.run.check.params); !unused.empty())
        {
            return refuse(std::move(unused));
        }
    }

    for (const std::string& variant : hasVariants ? report.variants : std::vector<std::string>{std::string{}})
    {
        for (const TestScenario& scenario : options.scenarios)
        {
            report.runs.push_back(TestRun{variant, scenario.shown, TestRunResult::NotRun, "", {}});
        }
    }
    const auto say = [&options](const std::string& text) {
        if (options.run.write_line)
        {
            options.run.write_line(std::string(detail::kLauncherTag) + text);
        }
    };
    const auto interrupted = [&options] { return options.run.interrupts != nullptr && options.run.interrupts->load() > 0; };
    std::string left; // why the runs from here on are not run
    for (std::size_t i = 0; i < report.runs.size(); ++i)
    {
        TestRun& run = report.runs[i];
        if (!left.empty())
        {
            run.reason = left;
            continue;
        }
        const std::string which = "test " + std::to_string(i + 1) + " of " + std::to_string(report.runs.size()) + ": " +
                                  (run.variant.empty() ? "" : "variant " + run.variant + ", ") + "scenario " + run.scenario;
        say(which);
        RunOptions one = options.run;
        one.check.overrides.variant = run.variant.empty() ? std::optional<std::string>{} : run.variant;
        one.check.scenario = options.scenarios[i % options.scenarios.size()].path;
        one.check.scenario_shown = run.scenario;
        one.check.params_for_many = true;
        RunReport result = run_file(path, shown_as, one);
        if (result.check.usage)
        {
            // Of the command line, so of every run: nothing more is run.
            return refuse(std::move(result.check.findings));
        }
        const bool notClosed = result.run && result.run->connection_not_closed_ms;
        judge(run, std::move(result));
        say(which + ": " + result_text(run.result) + (run.reason.empty() ? "" : " (" + run.reason + ")"));
        if (interrupted())
        {
            report.interrupted = true;
            left = "the test was interrupted before it";
        }
        else if (notClosed)
        {
            report.connection_not_closed = true;
            left = "the launcher's connection to the simulation did not close in the run before it";
        }
        else if (options.fail_fast && run.result == TestRunResult::Failed)
        {
            left = "a run before it failed (--fail-fast)";
        }
    }
    return report;
}

std::string format_test_text(const TestReport& report)
{
    const TestResult result = report.result();
    if (result == TestResult::Usage || result == TestResult::Unreadable)
    {
        return detail::findings_text(report.file, report.findings) +
               (result == TestResult::Usage ? "usage" : "cannot read " + report.file) + ": nothing was run\n";
    }
    const bool variants = !report.variants.empty();
    std::vector<std::vector<std::string>> rows;
    const auto row = [&](const std::string& variant, const std::string& scenario, const std::string& what, const std::string& why) {
        std::vector<std::string> cells;
        if (variants)
        {
            cells.push_back(variant);
        }
        cells.push_back(scenario);
        cells.push_back(what);
        if (!why.empty())
        {
            cells.push_back(why); // no cell (and no spaces at the end of the line) without a reason
        }
        rows.push_back(std::move(cells));
    };
    row("variant", "scenario", "result", "");
    const TestRun* failed = nullptr;
    for (const TestRun& run : report.runs)
    {
        row(run.variant, run.scenario, result_text(run.result), run.reason);
        failed = failed == nullptr && run.result == TestRunResult::Failed ? &run : failed;
    }
    std::string out = detail::table_text(rows) + "\n";
    if (failed != nullptr)
    {
        out += "to see a failed run in full, run it alone: " + run_alone(report, *failed) + "\n\n";
    }
    out += detail::findings_text(report.file, report.remarks());
    const auto count = [&report](TestRunResult which, const char* word, bool always) {
        const std::size_t n = report.count(which);
        return n > 0 || always ? ", " + std::to_string(n) + " " + word : std::string{};
    };
    out += std::string(result == TestResult::Failed ? "FAILED" : result_name(result)) + ": " + std::to_string(report.runs.size()) +
           (report.runs.size() == 1 ? " run" : " runs") + count(TestRunResult::Passed, "passed", true) +
           count(TestRunResult::Failed, "failed", true) + count(TestRunResult::Skipped, "skipped", true) +
           count(TestRunResult::Interrupted, "interrupted", false) + count(TestRunResult::NotRun, "not run", false) +
           (result == TestResult::Failed && report.connection_not_closed && report.count(TestRunResult::NotRun) > 0
                ? " (the launcher's connection to the simulation did not close)"
                : "") +
           (report.strict ? " (--strict)" : "") + "\n";
    return out;
}

std::string format_test_json(const TestReport& report)
{
    vehicle_sil::detail::JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_test", kTestFormat);
    w.string("vehicle_sil", version());
    if (report.file.empty())
    {
        w.null("file");
    }
    else
    {
        w.string("file", report.file);
    }
    w.string("result", result_name(report.result()));
    w.boolean("strict", report.strict);
    const auto names = [&w](std::string_view key, const std::vector<std::string>& items) {
        w.begin_array(key);
        for (const std::string& item : items)
        {
            w.string({}, item);
        }
        w.end_array();
    };
    names("variants", report.variants);
    names("scenarios", report.scenarios);
    for (const TestRunResult which : {TestRunResult::Passed, TestRunResult::Failed, TestRunResult::Skipped, TestRunResult::Interrupted,
                                      TestRunResult::NotRun})
    {
        w.integer(result_name(which), static_cast<std::int64_t>(report.count(which)));
    }
    w.begin_array("runs");
    for (const TestRun& run : report.runs)
    {
        w.begin_object();
        if (run.variant.empty())
        {
            w.null("variant");
        }
        else
        {
            w.string("variant", run.variant);
        }
        w.string("scenario", run.scenario);
        w.string("result", result_name(run.result));
        if (run.reason.empty())
        {
            w.null("reason");
        }
        else
        {
            w.string("reason", run.reason);
        }
        if (run.run)
        {
            detail::write_run_json(w, "run", *run.run);
        }
        else
        {
            w.null("run");
        }
        w.end_object();
    }
    w.end_array();
    std::vector<Finding> findings = report.findings;
    for (Finding& remark : report.remarks())
    {
        findings.push_back(std::move(remark));
    }
    detail::write_findings_json(w, report.file, findings);
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil::launch
