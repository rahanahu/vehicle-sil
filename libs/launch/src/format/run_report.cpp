// SPDX-License-Identifier: MIT
//
// What a run's report adds up to: the counts of errors, warnings and notes, why the run ended, the
// result, the exit code and the sentence for people. The findings themselves come from assess_run()
// (run_assess.cpp).
#include "vehicle_sil/config/detail/check_words.hpp"
#include "exit_codes.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

namespace {

std::size_t count(const std::vector<config::Finding>& findings, config::Severity severity)
{
    return static_cast<std::size_t>(
        std::count_if(findings.begin(), findings.end(), [severity](const config::Finding& f) { return f.severity == severity; }));
}

} // namespace

std::size_t RunReport::errors() const { return check.errors() + count(findings, config::Severity::Error); }
std::size_t RunReport::warnings() const { return check.warnings() + count(findings, config::Severity::Warning); }
std::size_t RunReport::notes() const { return check.notes() + count(findings, config::Severity::Note); }

// The order of the results, strongest first: usage, what cannot be read or set up, an interrupt,
// a failure, a scenario that a stop cut short, a stop, a pass. The exit codes are in exit_codes.hpp.
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
    const runner::EndReason end = runner::end_reason(*run);
    if (end == runner::EndReason::SetupFailed)
    {
        return RunResult::SetupFailed;
    }
    if (end == runner::EndReason::Interrupted)
    {
        return RunResult::Interrupted;
    }
    if (end == runner::EndReason::Failed || errors() > 0 || (strict && warnings() > 0))
    {
        return RunResult::Failed;
    }
    if (end == runner::EndReason::Stopped)
    {
        // A scenario that did not pass and did not fail has no verdict: it did not begin, or the
        // stop came before its end.
        const bool cutShort = run->scenario.result && run->scenario.result->status != scenario::ScenarioStatus::Passed;
        return cutShort ? RunResult::Incomplete : RunResult::Stopped;
    }
    return RunResult::Passed;
}

int RunReport::exit_code() const { return meaning_of(result()).exit_code; }

bool RunReport::ok() const { return meaning_of(result()).ok; }

namespace {

/// The message of the first finding of `severity`, the check's before the run's; "" if none.
std::string first_message(const RunReport& report, config::Severity severity)
{
    for (const std::vector<config::Finding>* findings : {&report.check.findings, &report.findings})
    {
        const auto found = std::find_if(findings->begin(), findings->end(), [severity](const config::Finding& f) { return f.severity == severity; });
        if (found != findings->end())
        {
            return found->message;
        }
    }
    return {};
}

/// `lead`, then the first error (or, for --strict, the first warning) if there is one.
std::string with_first_problem(const RunReport& report, std::string lead)
{
    std::string problem = first_message(report, config::Severity::Error);
    if (problem.empty() && report.strict)
    {
        problem = first_message(report, config::Severity::Warning);
        if (!problem.empty())
        {
            problem = "a warning counts as an error with --strict: " + problem;
        }
    }
    return problem.empty() ? lead : lead + ": " + problem;
}

} // namespace

// Each sentence says what happened, as a fact: a screen reads it out right after the result,
// also after a stop that the person asked for.
std::string RunReport::message() const
{
    const scenario::ScenarioRun* scenario = run && run->scenario.result ? &*run->scenario.result : nullptr;
    const std::string by = run ? run->end.stop_phrase() : std::string{};
    // Whoever asked to stop did not wait for them: no error, but said.
    const std::vector<std::string> forced = run ? runner::ended_by_force(*run) : std::vector<std::string>{};
    const std::string force =
        forced.empty() ? std::string{}
                       : "; " + config::detail::and_list(forced) + (forced.size() == 1 ? " was" : " were") + " ended by force, without waiting";
    switch (result())
    {
    case RunResult::Passed:
        // A stop that came only while the participants were ending may have cut their end short.
        return "The run reached its end" + std::string(forced.empty() ? " without errors" : "") +
               (scenario != nullptr ? ", and the scenario passed" : "") + force;
    case RunResult::Stopped:
        return "The run was stopped " + by + (forced.empty() ? " without errors" : "") +
               (scenario != nullptr ? ", after the scenario had passed" : "") + force;
    case RunResult::Incomplete:
        return "The run was stopped " + by + " before the scenario reached its end, so the scenario has no verdict" + force;
    case RunResult::Interrupted: return "The run was interrupted " + by + force;
    case RunResult::Failed:
        // The failure first: the stop is not its cause (the errors may be from before it, or from
        // the participants' end).
        return with_first_problem(*this, runner::end_reason(*run) == runner::EndReason::Stopped ? "The run failed (it was stopped " + by + ")"
                                                                                  : "The run failed");
    case RunResult::SetupFailed: return with_first_problem(*this, "The run failed before anything was started");
    case RunResult::CheckFailed: return with_first_problem(*this, "Nothing was started, because the check failed");
    case RunResult::Unreadable: return with_first_problem(*this, "Nothing was started, because the configuration cannot be read");
    case RunResult::Usage: return with_first_problem(*this, "Nothing was started, because the command line is wrong");
    }
    return "The run failed";
}

} // namespace vehicle_sil::launch
