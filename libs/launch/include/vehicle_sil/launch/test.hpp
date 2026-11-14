// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch test: runs every scenario of a list against a configuration, once for each
// of its variants, one run after the other (run_file() each time: its own registry, controller
// and participants' processes, nothing kept between two runs), and reports all of them as one
// table for people and as JSON. Internal; not part of the library API. docs/simulation-run.md
// (the section on vehicle_sil_launch test).
#pragma once

#include "vehicle_sil/launch/run.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

/// The version of the JSON of `vehicle_sil_launch test --json` (the key "vehicle_sil_test").
inline constexpr int kTestFormat = 1;

/// One scenario file of a test.
struct TestScenario
{
    std::filesystem::path path;
    std::string shown; ///< as the outputs name it (UTF-8)
};

/// What list_scenarios() found.
struct ScenarioList
{
    std::vector<TestScenario> scenarios{};
    /// What is wrong with the command line (code "usage"): a path that does not exist, a directory
    /// without scenario files or one that cannot be listed.
    std::vector<Finding> errors{};
};

/// The scenario files of `--scenarios` (each of `given` as written, UTF-8), in the order given. A
/// directory stands for the regular files directly in it whose names end in .yaml, .yml or .json
/// (ASCII letters of either case), in the order of their names as UTF-8 bytes (the same on every
/// system; subdirectories are not entered), each shown as "<given>/<name>". Anything else that
/// exists is taken as a scenario file. Nothing is removed if a file is named twice.
[[nodiscard]] ScenarioList list_scenarios(const std::vector<std::string>& given);

enum class TestRunResult
{
    Passed,      ///< the run passed (RunResult::Passed)
    Failed,      ///< the check or the run failed, or nothing could be set up
    Skipped,     ///< the scenario is not for the variant (its "only"): nothing was started
    Interrupted, ///< the launcher was interrupted during this run
    /// The test ended before it: an interrupt, --fail-fast, or a run that left the launcher's
    /// SIL Kit connection open (TestReport::connection_not_closed).
    NotRun,
};

/// One variant with one scenario.
struct TestRun
{
    std::string variant; ///< "" if the configuration defines no variants
    std::string scenario; ///< TestScenario::shown
    TestRunResult result = TestRunResult::NotRun;
    /// Why, in one line: the first error of a failed run (and how many more), what the scenario
    /// is only for, why it was not run; how many steps a run that passed skipped and how many
    /// warnings it has; or "".
    std::string reason{};
    /// The run's report (what `run --json` writes for it); empty for Skipped and NotRun.
    std::optional<RunReport> run{};
};

enum class TestResult
{
    Passed, ///< no run failed (skipped ones do not count), and none was left out
    /// At least one run failed; or runs were left out though nobody interrupted the test
    /// (TestReport::connection_not_closed); or, with --strict, no run was run at all.
    Failed,
    Interrupted, ///< the launcher was interrupted: the runs after that were not run
    Unreadable,  ///< the configuration cannot be read: nothing was run
    Usage,       ///< the command line was wrong: nothing was run
};

/// The options of the command line that every run of a test takes: what `vehicle_sil_launch run`
/// needs to be one of them again (format_test_text()), and what chose the variants.
struct TestArguments
{
    std::vector<std::string> variants{}; ///< --variant, as given; empty: every variant
    std::vector<ParamValue> params{};    ///< --param
    std::vector<std::string> disable{};  ///< --disable
    std::vector<std::string> enable{};   ///< --enable
    std::optional<std::chrono::milliseconds> timeout{};          ///< --timeout-ms
    std::optional<std::chrono::milliseconds> describe_timeout{}; ///< --describe-timeout-ms, if not the default
};

/// The result of `vehicle_sil_launch test`.
struct TestReport
{
    std::string file; ///< the configuration as the user named it
    bool strict = false;
    TestArguments given{};
    /// The variants that were to run, in the file's order; empty if the file defines none.
    std::vector<std::string> variants{};
    /// The scenarios, in the order of the command line (TestScenario::shown).
    std::vector<std::string> scenarios{};
    /// Every variant with every scenario, in the order they run: variant by variant.
    std::vector<TestRun> runs{};
    /// Why nothing was run (`usage`, `unreadable`); empty otherwise.
    std::vector<Finding> findings{};
    bool usage = false;
    bool unreadable = false;
    bool interrupted = false;
    /// A run left the launcher's SIL Kit connection open (RunOutcome::connection_not_closed_ms):
    /// the test stopped there (the rest is NotRun, and the test failed), and the caller ends the
    /// process as it does for such a run.
    bool connection_not_closed = false;

    [[nodiscard]] std::size_t count(TestRunResult result) const;
    /// The runs that started or were checked: passed, failed or interrupted.
    [[nodiscard]] std::size_t ran() const;
    /// What the test as a whole has to say when it ran to its end (result() Passed or Failed);
    /// nothing otherwise. An error "test-runs-left": runs were left out without an interrupt.
    /// A warning "test-nothing-run": no run was run (every one was skipped), so nothing was
    /// checked; --strict fails on it.
    [[nodiscard]] std::vector<Finding> remarks() const;
    [[nodiscard]] TestResult result() const;
    /// 0 passed; 1 failed; 3 unreadable; 4 usage; kExitInterrupted.
    [[nodiscard]] int exit_code() const;
};

struct TestOptions
{
    /// For every run: the check's options (strict, --disable and --enable, the parameters, the
    /// --describe limit), the speed, the time limit of one run, where the lines go, the
    /// interrupts. Its variant and scenario are set for each run.
    RunOptions run{};
    /// --variant: only these variants (each must be one of the file's); empty: all of them.
    std::vector<std::string> variants{};
    std::vector<TestScenario> scenarios{};
    /// Stop after the first run that fails; the rest is TestRunResult::NotRun.
    bool fail_fast = false;
};

/// Runs every scenario for every variant of the file `path` (shown as `shown_as`), one after the
/// other, in the order of TestReport::runs. Before the first run: the file is read (an unreadable
/// one ends the test), the variants are looked up, and these are usage errors: a parameter of the
/// command line that no scenario declares or uses, and an enabled participant that someone starts
/// by hand (manual: every run would wait for a person). A failed run does not end the test (unless
/// `fail_fast`); an interrupt ends the current run in order and the test with it. Writes a line
/// before and after each run (RunOptions::write_line). As run_file(): from the main thread, and
/// never throws for a problem of a file, an executable or a participant.
[[nodiscard]] TestReport test_file(const std::filesystem::path& path, const std::string& shown_as, const TestOptions& options);

/// The report for people: a table with one row for each run (the variant if the file has
/// variants, the scenario, the result, why), how to see one failed run in full (the command of
/// that run alone, with the options of the test that it takes, each word quoted as a POSIX shell
/// needs it), the remarks(), then one line with the result and the counts. LF line ends; no time,
/// duration or process ID in it.
[[nodiscard]] std::string format_test_text(const TestReport& report);

/// The report as JSON (format kTestFormat): the counts, then "runs", each with the object of
/// format_run_json() as "run" (null if nothing was run for it); "findings" has the remarks().
[[nodiscard]] std::string format_test_json(const TestReport& report);

} // namespace vehicle_sil::launch
