// SPDX-License-Identifier: MIT
//
// The exit code of each kind of result, in one place: the process exit code of `run` (RunResult)
// and of `test` (TestResult). Pure functions.
#pragma once

#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/test.hpp"

namespace vehicle_sil::launch {

[[nodiscard]] constexpr int exit_code_of(RunResult result)
{
    switch (result)
    {
    case RunResult::Passed: return 0;
    case RunResult::Failed:
    case RunResult::CheckFailed: return 1;
    case RunResult::Unreadable:
    case RunResult::SetupFailed: return 3;
    case RunResult::Usage: return 4;
    case RunResult::Interrupted: return kExitInterrupted;
    }
    return 1;
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
