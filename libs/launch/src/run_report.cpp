// SPDX-License-Identifier: MIT
//
// What a run's report adds up to: the counts of errors, warnings and notes, the result and the exit
// code. The findings themselves come from assess_run() (run_assess.cpp).
#include "exit_codes.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <algorithm>

namespace vehicle_sil::launch {

namespace {

std::size_t count(const std::vector<Finding>& findings, Severity severity)
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [severity](const Finding& f) { return f.severity == severity; }));
}

} // namespace

std::size_t RunReport::errors() const { return check.errors() + count(findings, Severity::Error); }
std::size_t RunReport::warnings() const { return check.warnings() + count(findings, Severity::Warning); }
std::size_t RunReport::notes() const { return check.notes() + count(findings, Severity::Note); }

RunResult RunReport::result() const
{
    if (usage || check.usage)
    {
        return RunResult::Usage;
    }
    if (!check.config)
    {
        return RunResult::Unreadable;
    }
    if (!run)
    {
        return RunResult::CheckFailed;
    }
    if (!run->interrupted_by.empty())
    {
        return RunResult::Interrupted;
    }
    if (!run->setup_error.empty())
    {
        return RunResult::SetupFailed;
    }
    const bool ended = run->simulation == SimulationEnd::ReachedStopAt || run->simulation == SimulationEnd::Ended;
    if (!ended || errors() > 0 || (strict && warnings() > 0))
    {
        return RunResult::Failed;
    }
    return RunResult::Passed;
}

int RunReport::exit_code() const { return exit_code_of(result()); }

} // namespace vehicle_sil::launch
