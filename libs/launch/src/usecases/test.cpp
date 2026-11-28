// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch test: the list of the scenario files, the runs one after the other
// (test_file()) and the report for people and as JSON.

#include "vehicle_sil/launch/test.hpp"

#include "format/check_format.hpp"
#include "vehicle_sil/config/detail/check_words.hpp"
#include "format/exit_codes.hpp"
#include "format/run_format.hpp"
#include "vehicle_sil/runner/detail/run_words.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace vehicle_sil::launch {

namespace {

/// The largest scenario file read for its parameters (as the check's limit).
constexpr std::uintmax_t kMaxScenarioBytes = std::uintmax_t{16} << 20;

config::Finding usage(std::string message) { return config::Finding{config::Severity::Error, "usage", std::move(message), 0, "", ""}; }

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
    const config::Severity severity = report.errors() > 0 ? config::Severity::Error : config::Severity::Warning;
    const config::Finding* first = nullptr;
    std::size_t count = 0;
    for (const std::vector<config::Finding>* findings : {&report.check.findings, &report.findings})
    {
        for (const config::Finding& finding : *findings)
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
        return report.run && report.run->end.simulation != runner::SimulationEnd::Ended && report.run->end.simulation != runner::SimulationEnd::ReachedStopAt
                   ? "the simulation did not run to its end"
                   : "";
    }
    const char* kind = severity == config::Severity::Error ? "error" : "warning";
    const std::string scenarioFile = report.check.scenario ? report.check.scenario->file : std::string{};
    std::string loc;
    if (!first->file.empty() && !scenarioFile.empty() && first->file != scenarioFile)
    {
        loc = first->file + (first->line > 0 ? ":" + std::to_string(first->line) : "") + ": ";
    }
    else if (!first->file.empty() && first->line > 0)
    {
        loc = "line " + std::to_string(first->line) + ": ";
    }
    const std::string caller = detail::called_from_inline(first->detail);
    return loc + first->message +
           (caller.empty() ? std::string{} : " (" + caller + ")") +
           (severity == config::Severity::Warning ? " (a warning, and --strict fails on it)" : "") +
           (count > 1 ? " (and " + std::to_string(count - 1) + " more " + kind + (count == 2 ? "" : "s") + ")" : std::string{});
}

/// What became of one run.
void judge(TestRun& run, RunReport report)
{
    const std::optional<scenario::ScenarioCheck>& scenario = report.check.scenario;
    // Not for this variant, and nothing else is wrong: skipped. (The error that says so is one
    // finding; any other error is of the file or of the scenario, and fails the run.)
    if (scenario && scenario->not_for_variant && report.check.errors() == 1)
    {
        run.result = TestRunResult::Skipped;
        run.reason = (scenario->case_name.empty() ? "the scenario is only for " : "the case is only for ") +
                     config::detail::and_list(scenario->only.value_or(std::vector<std::string>{})) + " (its only)";
        return;
    }
    switch (report.result())
    {
    case RunResult::Passed:
        run.result = TestRunResult::Passed;
        if (report.run && report.run->scenario.result && report.run->scenario.result->skipped() > 0)
        {
            const std::size_t skipped = report.run->scenario.result->skipped();
            run.reason = std::to_string(skipped) + " of " + std::to_string(report.run->scenario.result->steps.size()) +
                         (skipped == 1 ? " steps was skipped" : " steps were skipped");
        }
        // Expects beside a ramp or a wave that were not checked: the steps ran, but did not look.
        if (report.run && report.run->scenario.result && report.run->scenario.result->unchecked_expects() > 0)
        {
            const std::size_t unchecked = report.run->scenario.result->unchecked_expects();
            run.reason += (run.reason.empty() ? "" : ", ") + std::to_string(unchecked) +
                          (unchecked == 1 ? " expect was not checked" : " expects were not checked");
        }
        // It passed with warnings (no --strict): how many, so that the table does not hide them.
        if (const std::size_t warnings = report.warnings(); warnings > 0)
        {
            run.reason += (run.reason.empty() ? "" : ", ") + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
        }
        break;
    case RunResult::Interrupted:
        run.result = TestRunResult::Interrupted;
        run.reason = report.run && !report.run->end.interrupted_by.empty() ? "by " + report.run->end.interrupted_by : "";
        break;
    case RunResult::Incomplete:
        run.result = TestRunResult::Failed;
        run.reason = "the scenario did not finish before the requested stop time";
        break;
    default:
        run.result = TestRunResult::Failed;
        run.reason = failure_reason(report);
        break;
    }
    run.run = std::move(report);
}

struct InspectedScenario
{
    bool read_ok = false;
    bool broken = false;
    std::string text{};
    scenario::ScenarioInspection inspection{};
};

bool has_inspection_error(const InspectedScenario& item)
{
    return !item.read_ok || item.inspection.params_incomplete ||
           std::any_of(item.inspection.findings.begin(), item.inspection.findings.end(),
                       [](const config::Finding& f) { return f.severity == config::Severity::Error; });
}

std::vector<InspectedScenario> inspect_scenarios(const std::vector<TestScenario>& scenarios,
                                                 const std::vector<std::string>& variants,
                                                 const std::vector<config::ParamValue>& configured,
                                                 const std::vector<config::ParamValue>& given)
{
    std::vector<InspectedScenario> out;
    out.reserve(scenarios.size());
    for (const TestScenario& scenario : scenarios)
    {
        InspectedScenario item;
        const std::filesystem::path path = process::path_from_utf8(scenario.shown);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) <= kMaxScenarioBytes)
        {
            std::ifstream in(path, std::ios::binary);
            if (in)
            {
                item.read_ok = true;
                item.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                scenario::ScenarioContext context;
                context.variants = variants;
                context.params = configured;
                context.params.insert(context.params.end(), given.begin(), given.end());
                context.params_for_many = true;
                context.scenario_file = scenario.shown;
                context.read_file = files_beside(path);
                context.read_template = files_beside(path, "the template file");
                item.inspection = scenario::inspect_scenario(item.text, context);
            }
        }
        if (!item.read_ok)
        {
            item.inspection.params_incomplete = true;
        }
        item.broken = has_inspection_error(item);
        out.push_back(std::move(item));
    }
    return out;
}

/// The parameters of the command line that no scenario declares or uses: a usage error each (a
/// typing error would otherwise change nothing, silently). Nothing if a scenario cannot be read
/// that far: its run says what is wrong with it, and the parameter may be in it.
std::vector<config::Finding> unused_params(const std::vector<TestScenario>& scenarios,
                                           const std::vector<InspectedScenario>& inspected,
                                           const std::vector<std::string>& filterCases,
                                           const std::vector<config::ParamValue>& given)
{
    if (given.empty())
    {
        return {};
    }
    std::vector<std::string> names;
    for (std::size_t i = 0; i < scenarios.size(); ++i)
    {
        const InspectedScenario& item = inspected[i];
        if (!item.read_ok)
        {
            return {};
        }
        if (!item.inspection.has_cases)
        {
            if (!filterCases.empty())
            {
                if (!item.broken)
                {
                    continue; // healthy caseless scenario is skipped when --case filters
                }
                return {};
            }
            if (item.inspection.params_incomplete)
            {
                return {};
            }
            for (const std::string& p : item.inspection.param_names)
            {
                if (std::find(names.begin(), names.end(), p) == names.end())
                {
                    names.push_back(p);
                }
            }
            continue;
        }
        if (item.inspection.cases.empty())
        {
            return {};
        }
        if (filterCases.empty())
        {
            if (item.inspection.params_incomplete)
            {
                return {};
            }
            for (const std::string& p : item.inspection.param_names)
            {
                if (std::find(names.begin(), names.end(), p) == names.end())
                {
                    names.push_back(p);
                }
            }
            continue;
        }
        for (const std::string& caseName : item.inspection.cases)
        {
            if (std::find(filterCases.begin(), filterCases.end(), caseName) == filterCases.end())
            {
                continue;
            }
            const auto ci = std::find_if(item.inspection.case_inspections.begin(), item.inspection.case_inspections.end(),
                                         [&](const scenario::CaseInspection& c) { return c.name == caseName; });
            if (ci == item.inspection.case_inspections.end() || ci->params_incomplete || ci->has_error)
            {
                return {};
            }
            for (const std::string& p : ci->param_names)
            {
                if (std::find(names.begin(), names.end(), p) == names.end())
                {
                    names.push_back(p);
                }
            }
        }
    }
    std::vector<config::Finding> errors;
    for (const config::ParamValue& param : given)
    {
        if (std::find(names.begin(), names.end(), param.name) != names.end())
        {
            continue;
        }
        const std::string guess = config::detail::closest(param.name, names);
        const bool single = scenarios.size() == 1;
        const std::string which =
            single ? "the scenario has no parameter "
                   : "none of the " + std::to_string(scenarios.size()) + " scenarios has a parameter ";
        errors.push_back(usage("--param " + param.name + "=" + param.text + ": " + which + param.name +
                               (guess.empty() ? "" : " (did you mean " + guess + "?)") + ": " +
                               (names.empty() ? std::string(single ? "it uses none (a step uses one as ${name})"
                                                                   : "they use none (a step uses one as ${name})")
                                              : std::string(single ? "its parameters are " : "their parameters are ") +
                                                    config::detail::join(names)) +
                               "; remove the option or correct the name"));
    }
    return errors;
}

/// The participants that someone starts by hand (enabled and manual) in a configuration a test
/// would run: a usage error each. test starts every run by itself, and each run would wait for a
/// person (without --timeout-ms: for ever). `configs`: the variant ("" without variants) and its
/// configuration, after --disable and --enable.
std::vector<config::Finding> manual_participants(const std::vector<std::pair<std::string, const config::SimulationConfig*>>& configs)
{
    std::vector<std::pair<std::string, std::vector<std::string>>> found; // the participant, the variants it is manual in
    for (const auto& [variant, config] : configs)
    {
        for (const config::ParticipantConfig& p : config->participants)
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
    std::vector<config::Finding> errors;
    for (const auto& [name, variants] : found)
    {
        errors.push_back(usage("participant '" + name + "' is manual (manual: true in the file" +
                               (variants.empty() ? std::string{}
                                                 : std::string(variants.size() == 1 ? ", in the variant " : ", in the variants ") +
                                                       config::detail::and_list(variants)) +
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
    // The time lock already selects speed 1. Otherwise reproduce the checked starting speed.
    const bool locked = run.run && run.run->check.time_lock.locked;
    const std::optional<double> speed = run.run && run.run->check.run_timing ? run.run->check.run_timing->speed : std::nullopt;
    std::string out = "vehicle_sil_launch run " + shell_word(report.file) + (locked ? "" : " --speed " + config::speed_text(speed)) +
                      (report.given.time_policy == config::TimePolicy::Free ? " --time-policy free" : "") +
                      (run.variant.empty() ? std::string{} : " --variant " + shell_word(run.variant)) + " --scenario " +
                      shell_word(run.scenario) +
                      (run.case_name.empty() ? std::string{} : " --case " + shell_word(run.case_name));
    if (run.run && run.run->check.scenario)
    {
        const std::vector<config::ParamValue>& has = run.run->check.scenario->params;
        for (const config::ParamValue& param : report.given.params)
        {
            if (std::any_of(has.begin(), has.end(), [&param](const config::ParamValue& one) { return one.name == param.name; }))
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
    if (report.given.stop_at_ms)
    {
        out += " --stop-at-ms " + std::to_string(*report.given.stop_at_ms);
    }
    if (report.given.late_tolerance_ms)
    {
        out += " --late-tolerance-ms " + std::to_string(*report.given.late_tolerance_ms);
    }
    if (report.given.participant_start_timeout_ms)
    {
        out += " --participant-start-timeout-ms " + std::to_string(*report.given.participant_start_timeout_ms);
    }
    if (report.given.participant_stop_timeout_ms)
    {
        out += " --participant-stop-timeout-ms " + std::to_string(*report.given.participant_stop_timeout_ms);
    }
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
    return (variants.size() == 1 ? "variant " : "variants ") + config::detail::and_list(variants);
}

} // namespace

namespace {

/// What goes between the directory `one` (`path`: the same as a path) and the name of a file in
/// it, so that the result names that file (as `path / name` does) and reads as the user wrote the
/// directory: "/" wherever that is so. Nothing after a separator, and nothing after a drive alone
/// (Windows "C:": the files of the drive's current directory are "C:<name>"; "C:/<name>" is in its
/// root). A path that Windows takes as written ("\\?\...") has no "/": there it is "\".
std::string separator_after(const std::string& one, const std::filesystem::path& path)
{
    if (!one.empty() && (one.back() == '/' || one.back() == '\\'))
    {
        return "";
    }
    if (path.has_root_name() && !path.has_root_directory() && path.relative_path().empty())
    {
        return "";
    }
    return one.rfind("\\\\?\\", 0) == 0 ? "\\" : "/";
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
            out.scenarios.push_back(TestScenario{one});
            continue;
        }
        std::vector<std::string> found; // the names as UTF-8
        std::filesystem::directory_iterator it(path, ec);
        for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            std::error_code ignored;
            std::string name = process::utf8_from_path(it->path().filename());
            if (it->is_regular_file(ignored) && is_scenario_name(name))
            {
                found.push_back(std::move(name));
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
        std::sort(found.begin(), found.end());
        const std::string between = separator_after(one, path);
        for (const std::string& name : found)
        {
            out.scenarios.push_back(TestScenario{one + between + name});
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

std::vector<config::Finding> TestReport::remarks() const
{
    std::vector<config::Finding> out;
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
                                                              (std::prev(first)->case_name.empty()
                                                                   ? std::string{}
                                                                   : " (case " + std::prev(first)->case_name + ")") +
                                                              (std::prev(first)->variant.empty()
                                                                   ? std::string{}
                                                                   : " (variant " + std::prev(first)->variant + ")");
        out.push_back(config::Finding{config::Severity::Error, "test-runs-left",
                              std::to_string(left) + " of " + std::to_string(runs.size()) + (left == 1 ? " runs was" : " runs were") +
                                  " not run: the launcher's connection to the simulation did not close" + after +
                                  ", and the launcher ends its process rather than start another simulation in it. Run the "
                                  "test again for the runs that are 'not run' (--variant and --scenarios choose them), and "
                                  "see that run's warning connection-not-closed",
                              0, "", ""});
    }
    // The runs that the time lock set to speed 1: the table has their results only, and a test is
    // as fast as possible otherwise. Said once, with the note of the first of them.
    const auto speed_set = [](const TestRun& run) -> const config::Finding* {
        if (!run.run)
        {
            return nullptr;
        }
        const std::vector<config::Finding>& found = run.run->check.findings;
        const auto note = std::find_if(found.begin(), found.end(), [](const config::Finding& f) { return f.code == "outside-speed-set"; });
        return note == found.end() ? nullptr : &*note;
    };
    if (const auto first = std::find_if(runs.begin(), runs.end(), [&](const TestRun& run) { return speed_set(run) != nullptr; });
        first != runs.end())
    {
        const auto slowed = static_cast<std::size_t>(std::count_if(first, runs.end(), [&](const TestRun& run) { return speed_set(run) != nullptr; }));
        out.push_back(config::Finding{config::Severity::Note, "test-speed-set",
                              std::to_string(slowed) + " of " + std::to_string(runs.size()) + " runs" +
                                  " went at speed 1 (real time), not as fast as possible: each takes as long as its scenario "
                                  "does in real time (mind --timeout-ms and the time limit of what runs the test). The first "
                                  "of them (scenario " + first->scenario +
                                  (first->case_name.empty() ? "" : ", case " + first->case_name) +
                                  (first->variant.empty() ? "" : ", variant " + first->variant) +
                                  ") says why: " + speed_set(*first)->message,
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
        out.push_back(config::Finding{config::Severity::Warning, "test-nothing-run",
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

int TestReport::exit_code() const { return exit_code_of(result()); }

TestReport test_file(const TestOptions& options)
{
    if (options.lent.controls != nullptr)
    {
        throw std::invalid_argument(
          "TestOptions::lent.controls must be null: every run of a test needs RunControls of its own, so test_file() "
          "makes them for each run on `lent.stops`.");
    }
    const RunIntent& intent = options.request.intent;
    const RunHost& host = options.request.host;
    TestReport report;
    report.file = host.config;
    report.strict = intent.strict;
    report.given.variants = options.variants;
    report.given.cases = options.cases;
    report.given.params = intent.params;
    report.given.disable = intent.disable;
    report.given.enable = intent.enable;
    report.given.timeout = host.timeout;
    report.given.stop_at_ms = intent.stop_at_ms;
    report.given.late_tolerance_ms = intent.late_tolerance_ms;
    if (host.start_timeout_ms != RunHost{}.start_timeout_ms)
    {
        report.given.participant_start_timeout_ms = host.start_timeout_ms;
    }
    if (host.stop_timeout_ms != RunHost{}.stop_timeout_ms)
    {
        report.given.participant_stop_timeout_ms = host.stop_timeout_ms;
    }
    report.given.time_policy = intent.time_policy;
    if (host.describe_timeout != kDescribeTimeout)
    {
        report.given.describe_timeout = host.describe_timeout;
    }
    for (const TestScenario& scenario : options.scenarios)
    {
        report.scenarios.push_back(scenario.shown);
    }
    const auto refuse = [&report](std::vector<config::Finding> errors) {
        report.usage = true;
        report.findings = std::move(errors);
        return report;
    };
    if (auto errors = validate_request(options.request, RequestCommand::Test); !errors.empty())
    {
        return refuse(std::move(errors));
    }

    // The file alone, without a variant: what it defines. For a file with variants this is the
    // usage error of a check without --variant, which lists them.
    RunRequest read = options.request;
    read.intent.variant.reset();
    CheckOptions only;
    only.file_only = true;
    only.fastest_by_default = options.fastest_by_default;
    const Checked file = check_file(read, only);
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
        read.intent.variant = variant;
        if (Checked chosen = check_file(read, only); chosen.usage)
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
    std::vector<Checked> chosen;
    for (const std::string& variant : report.variants)
    {
        read.intent.variant = variant;
        chosen.push_back(check_file(read, only));
        if (chosen.back().usage)
        {
            // Of the command line (--disable <no such name>), so of every run.
            return refuse(std::move(chosen.back().findings));
        }
    }
    std::vector<std::pair<std::string, const config::SimulationConfig*>> configs;
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
    std::vector<config::ParamValue> configured;
    for (const auto& [variant, config] : configs)
    {
        configured.insert(configured.end(), config->params.begin(), config->params.end());
    }
    const std::vector<InspectedScenario> inspected =
        inspect_scenarios(options.scenarios, file.variants, configured, intent.params);
    if (!options.cases.empty())
    {
        std::vector<std::string> allCases;
        bool anyCasesUnreadable = false;
        for (const InspectedScenario& item : inspected)
        {
            if (item.broken && item.inspection.cases.empty())
            {
                anyCasesUnreadable = true;
            }
            for (const std::string& c : item.inspection.cases)
            {
                if (std::find(allCases.begin(), allCases.end(), c) == allCases.end())
                {
                    allCases.push_back(c);
                }
            }
        }
        std::vector<config::Finding> caseErrors;
        for (const std::string& c : options.cases)
        {
            if (std::find(allCases.begin(), allCases.end(), c) == allCases.end() && !anyCasesUnreadable)
            {
                if (allCases.empty())
                {
                    caseErrors.push_back(usage(
                        "--case " + c + ": " +
                        (options.scenarios.size() == 1 ? "the scenario has no cases"
                                                       : "none of the " + std::to_string(options.scenarios.size()) + " scenarios has cases") +
                        ": leave --case out, or add cases to a scenario"));
                }
                else
                {
                    const std::string guess = config::detail::closest(c, allCases);
                    caseErrors.push_back(usage(
                        "--case " + c + ": " +
                        (options.scenarios.size() == 1 ? "the scenario has no case with this name"
                                                       : "none of the " + std::to_string(options.scenarios.size()) +
                                                             " scenarios has a case with this name") +
                        (guess.empty() ? "" : " (did you mean '" + guess + "'?)") + "; " +
                        (options.scenarios.size() == 1 ? "its" : "their") + " cases are " + config::detail::join(allCases)));
                }
            }
        }
        if (!caseErrors.empty())
        {
            return refuse(std::move(caseErrors));
        }
    }
    if (!intent.params.empty())
    {
        if (auto unused = unused_params(options.scenarios, inspected, options.cases, intent.params); !unused.empty())
        {
            return refuse(std::move(unused));
        }
    }

    for (const std::string& variant : hasVariants ? report.variants : std::vector<std::string>{std::string{}})
    {
        for (std::size_t si = 0; si < options.scenarios.size(); ++si)
        {
            const TestScenario& scenario = options.scenarios[si];
            const InspectedScenario& item = inspected[si];
            if (!item.inspection.cases.empty())
            {
                for (const std::string& caseName : item.inspection.cases)
                {
                    if (options.cases.empty() ||
                        std::find(options.cases.begin(), options.cases.end(), caseName) != options.cases.end())
                    {
                        TestRun entry{variant, scenario.shown, TestRunResult::NotRun, "", {}};
                        entry.case_name = caseName;
                        report.runs.push_back(std::move(entry));
                    }
                }
            }
            else if (options.cases.empty() || item.inspection.has_cases || item.broken)
            {
                report.runs.push_back(TestRun{variant, scenario.shown, TestRunResult::NotRun, "", {}});
            }
        }
    }
    const auto say = [&options](const std::string& text) {
        if (options.observers.write_line)
        {
            options.observers.write_line(std::string(runner::detail::kLauncherTag) + text);
        }
    };
    const auto interrupted = [&options] { return options.lent.stops != nullptr && options.lent.stops->count() > 0; };
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
                                  (run.variant.empty() ? "" : "variant " + run.variant + ", ") + "scenario " + run.scenario +
                                  (run.case_name.empty() ? "" : ", case " + run.case_name);
        say(which);
        RunRequest one = options.request;
        one.intent.variant = run.variant.empty() ? std::optional<std::string>{} : run.variant;
        one.intent.scenario = run.scenario;
        one.intent.scenario_case = run.case_name.empty() ? std::optional<std::string>{} : run.case_name;
        CheckOptions check;
        check.params_for_many = true;
        check.fastest_by_default = options.fastest_by_default;
        RunReport result = run_file(one, options.observers, options.lent, options.limits, check);
        if (result.check.usage)
        {
            // Of the command line, so of every run: nothing more is run.
            return refuse(std::move(result.check.findings));
        }
        const bool notClosed = result.run && result.run->close.not_closed_ms;
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
    const bool cases = std::any_of(report.runs.begin(), report.runs.end(),
                                   [](const TestRun& r) { return !r.case_name.empty(); });
    std::vector<std::vector<std::string>> rows;
    const auto row = [&](const std::string& variant, const std::string& scenario, const std::string& caseName,
                         const std::string& what, const std::string& why) {
        std::vector<std::string> cells;
        if (variants)
        {
            cells.push_back(variant);
        }
        cells.push_back(scenario);
        if (cases)
        {
            cells.push_back(caseName);
        }
        cells.push_back(what);
        if (!why.empty())
        {
            cells.push_back(why); // no cell (and no spaces at the end of the line) without a reason
        }
        rows.push_back(std::move(cells));
    };
    row("variant", "scenario", "case", "result", "");
    const TestRun* failed = nullptr;
    for (const TestRun& run : report.runs)
    {
        row(run.variant, run.scenario, run.case_name.empty() ? "-" : run.case_name, result_text(run.result), run.reason);
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
    vehicle_sil::json::JsonWriter w;
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
    if (report.given.stop_at_ms)
    {
        w.integer("stop_at_ms", *report.given.stop_at_ms);
    }
    if (report.given.late_tolerance_ms)
    {
        w.integer("late_tolerance_ms", *report.given.late_tolerance_ms);
    }
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
        if (!run.case_name.empty())
        {
            w.string("case", run.case_name);
        }
        w.end_object();
    }
    w.end_array();
    std::vector<config::Finding> findings = report.findings;
    for (config::Finding& remark : report.remarks())
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
