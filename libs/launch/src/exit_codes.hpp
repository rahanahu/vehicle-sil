// SPDX-License-Identifier: MIT
//
// What each kind of result means, in one place: the process exit code of `run` (RunResult) with
// whether the result is a success, and the exit code of `test` (TestResult). Pure functions.
#pragma once

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

[[nodiscard]] constexpr int exit_code_of(RunResult result) { return meaning_of(result).exit_code; }

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
