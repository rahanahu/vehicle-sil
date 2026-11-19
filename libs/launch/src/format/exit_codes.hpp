// SPDX-License-Identifier: MIT
//
// What each kind of result means, in one place: the process exit code of `run` (RunResult) with
// whether the result is a success, and the exit codes of `test` (TestResult), `check` (CheckReport)
// and `connections` (ConnectionsReport). Pure functions.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/launch/connections.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/test.hpp"

namespace vehicle_sil::launch {

/// What a result means to whoever started the run: the process exit code, and whether the result
/// counts as a success (the result's "ok").
struct ResultMeaning
{
    int exit_code;
    bool ok;
};

/// The one table of `run`: each result, its exit code and whether it is a success. A stop by an
/// operator is a success unless it cut a scenario short (Incomplete: the scenario has no verdict).
[[nodiscard]] constexpr ResultMeaning meaning_of(RunResult result)
{
    switch (result)
    {
    case RunResult::Passed: return {0, true};
    case RunResult::Stopped: return {0, true};
    case RunResult::Incomplete: return {kExitIncomplete, false};
    case RunResult::Failed:
    case RunResult::CheckFailed: return {1, false};
    case RunResult::Unreadable:
    case RunResult::SetupFailed: return {3, false};
    case RunResult::Usage: return {4, false};
    case RunResult::Interrupted: return {kExitInterrupted, false};
    }
    return {1, false};
}

/// The one table of `check`: 4 the command line was wrong, 3 the file could not be read, 1 the
/// check did not pass (an error; with --strict also a warning), 0 it passed.
[[nodiscard]] inline int exit_code_of(const config::CheckReport& report)
{
    if (report.usage)
    {
        return 4;
    }
    if (!report.config)
    {
        return 3;
    }
    return report.passed() ? 0 : 1;
}

/// The one table of `connections`: 4 the command line was wrong, 3 the file could not be read, 1
/// the list is incomplete (or a target of a request cannot be used), 0 the list is complete.
[[nodiscard]] inline int exit_code_of(const ConnectionsReport& report)
{
    if (report.usage)
    {
        return 4;
    }
    if (!report.readable)
    {
        return 3;
    }
    return report.incomplete() || report.request_unusable() ? 1 : 0;
}

[[nodiscard]] constexpr int exit_code_of(TestResult result)
{
    switch (result)
    {
    case TestResult::Passed: return 0;
    case TestResult::Failed: return 1;
    case TestResult::Unreadable: return 3;
    case TestResult::Usage: return 4;
    case TestResult::Interrupted: return kExitInterrupted;
    }
    return 1;
}

} // namespace vehicle_sil::launch
