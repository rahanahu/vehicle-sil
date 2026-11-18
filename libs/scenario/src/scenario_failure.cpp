// SPDX-License-Identifier: MIT
//
// Why a scenario's step failed: the members of ScenarioEngine (scenario_run.hpp) that say it, in
// the step's message (the conditions that were not met, with the values seen; which writes of a
// refused command the ECU applied all the same) and in the record (TimelineFailure: the failed
// conditions and writes of the timeline). The engine that runs the scenario and decides its
// steps is in scenario_run.cpp.
#include "vehicle_sil/scenario/scenario_run.hpp"

#include "scenario_failure.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

using detail::ms_text;

// --- the conditions that were not met ---

bool ScenarioEngine::is_unmet(const ScenarioCondition& condition, std::optional<SimTime> since, bool& judged) const
{
    const Variable& known = variables_[condition.variable];
    judged = !(since && (!known.value_time || *known.value_time < *since));
    if (!judged)
    {
        return false; // no sample of its ECU from `since` yet: not judged
    }
    return !(known.value && std::all_of(condition.all.begin(), condition.all.end(),
                                        [&](const ScenarioComparison& c) { return holds(c, *known.value); }));
}

TimelineFailure ScenarioEngine::unmet_values(const ScenarioStep& step, std::optional<SimTime> since) const
{
    TimelineFailure out;
    for (std::size_t i = 0; i < step.conditions.size(); ++i)
    {
        bool judged = false;
        if (is_unmet(step.conditions[i], since, judged))
        {
            const Variable& known = variables_[step.conditions[i].variable];
            out.conditions.push_back(TimelineFailedCondition{i, known.value, known.value ? known.value_time : std::nullopt});
        }
    }
    return out;
}

std::string ScenarioEngine::unmet(const ScenarioStep& step, std::optional<SimTime> since) const
{
    std::string out;
    for (const ScenarioCondition& condition : step.conditions)
    {
        const Variable& known = variables_[condition.variable];
        const ScenarioVariable& v = scenario_.variables[condition.variable];
        bool judged = false;
        if (!is_unmet(condition, since, judged))
        {
            continue;
        }
        out += (out.empty() ? "" : "; ") + condition.text + ", but " +
               (known.value ? "it was " + value_text(*known.value, &v) + " (its sample of " + ms_text(*known.value_time) + " ms)"
                            : std::string("it had no value yet (no sample of its ECU had arrived)"));
    }
    return out;
}

std::string ScenarioEngine::during(std::size_t step, const Watch& watch) const
{
    const SimTime end = *run_.steps[step].end;
    return "from " + ms_text(watch.from) + " ms to " + ms_text(*watch.until) + " ms" +
           (*watch.until == end ? std::string{}
                                : ", where its end (" + ms_text(end) + " ms) takes effect");
}

// --- the writes that were not applied ---

TimelineFailure ScenarioEngine::failed_write(const Write& write, const control::OperationResult* result,
                                             const control::TargetResult& failed) const
{
    TimelineFailure out;
    for (std::size_t i = 0; i < write.writes.size(); ++i)
    {
        // Not known, unless the result of its participant says (below).
        out.writes.push_back(TimelineFailedWrite{write.writes[i].variable, write.at, write.writes[i].value,
                                                 i < write.sent.size() ? write.sent[i] : std::nullopt, std::nullopt});
    }
    if (result == nullptr)
    {
        return out;
    }
    for (const control::TargetResult& target : result->targets)
    {
        // The participant's command: its writes of the request, in the order sent.
        std::vector<std::size_t> command;
        for (std::size_t i = 0; i < write.writes.size(); ++i)
        {
            if (scenario_.participants[scenario_.variables[write.writes[i].variable].participant].name == target.peer)
            {
                command.push_back(i);
            }
        }
        // Which of them took effect and which one the reason is about come as positions in that
        // command (control::TargetResult): the reason's text is not read for them.
        const auto all = [&](std::optional<bool> applied) {
            for (const std::size_t i : command)
            {
                out.writes[i].applied = applied;
            }
        };
        switch (target.status)
        {
        case control::TargetStatus::Applied:
            all(true);
            break;
        case control::TargetStatus::Late:
        case control::TargetStatus::Cancelled:
        case control::TargetStatus::Invalid:
        case control::TargetStatus::Busy:
            // The command as a whole was refused, or never came to its step.
            all(false);
            break;
        case control::TargetStatus::Rejected:
            // The ECU applies the writes of a command one after the other and stops at the one
            // whose setter throws: the ones before it are applied. Not known when the result
            // does not say how many (a transport of its own that leaves the number out, for a
            // command of several writes).
            if (target.applied_writes && *target.applied_writes <= command.size())
            {
                for (std::size_t k = 0; k < command.size(); ++k)
                {
                    out.writes[command[k]].applied = k < *target.applied_writes;
                }
            }
            break;
        case control::TargetStatus::Sent:
        case control::TargetStatus::Queued:
        case control::TargetStatus::Disconnected:
        case control::TargetStatus::Unknown:
            break; // no result: it may or may not have taken effect
        }
        if (target.peer == failed.peer)
        {
            // The reason: the write the result points at, or the only one there is.
            out.write = target.failed_write && *target.failed_write < command.size()
                            ? std::optional<std::size_t>(command[*target.failed_write])
                            : command.size() == 1 ? std::optional<std::size_t>(command.front()) : std::nullopt;
        }
    }
    return out;
}

std::string ScenarioEngine::applied_text(const TimelineFailure& failure, const std::string& peer) const
{
    // Of a command of several writes for that participant: which of them took effect.
    std::vector<const TimelineFailedWrite*> command;
    for (const TimelineFailedWrite& w : failure.writes)
    {
        if (scenario_.participants[scenario_.variables[w.variable].participant].name == peer)
        {
            command.push_back(&w);
        }
    }
    if (command.size() < 2)
    {
        return {};
    }
    std::string applied;
    std::size_t count = 0;
    bool unknown = false;
    for (const TimelineFailedWrite* w : command)
    {
        unknown = unknown || !w->applied;
        if (w->applied == std::optional<bool>(true))
        {
            const ScenarioVariable& v = scenario_.variables[w->variable];
            applied += (count++ == 0 ? "" : ", ") + v.text + " = " + (w->value ? value_text(*w->value, &v) : "?");
        }
    }
    if (unknown)
    {
        return "; which of the " + std::to_string(command.size()) + " writes of that command the ECU applied is not known";
    }
    if (count == 0)
    {
        return "; none of the " + std::to_string(command.size()) + " writes of that command was applied";
    }
    return "; the ECU had already applied the write" + std::string(count > 1 ? "s" : "") + " before it in that command (" + applied +
           "), and " + (count > 1 ? "they stay" : "it stays") + " applied";
}

} // namespace vehicle_sil::launch
