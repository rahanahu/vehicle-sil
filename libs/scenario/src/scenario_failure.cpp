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
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::scenario {

using detail::ms_text;

// --- the conditions that were not met ---

namespace {

template <class Var>
bool has_sample_since(const Var& var, const std::optional<SimTime>& since) noexcept
{
    return var.value.has_value() && (!since || (var.value_time && *var.value_time >= *since));
}

bool is_non_finite(const control::Value& val) noexcept
{
    const auto* d = std::get_if<double>(&val);
    return d != nullptr && !std::isfinite(*d);
}

std::optional<control::Value> finite_or_empty(const std::optional<control::Value>& val) noexcept
{
    if (!val || is_non_finite(*val))
    {
        return std::nullopt;
    }
    return val;
}

template <class Vars>
std::vector<std::size_t> failed_rhs_variables(const ScenarioCondition& condition, const Vars& variables,
                                              const std::optional<SimTime>& since)
{
    const auto& lhs = variables[condition.variable];
    const bool hasLhs = has_sample_since(lhs, since);
    const bool lhsNonFinite = hasLhs && is_non_finite(*lhs.value);
    std::vector<std::size_t> out;
    for (const ScenarioComparison& c : condition.all)
    {
        if (!c.rhs_variable)
        {
            continue;
        }
        const auto& rhs = variables[*c.rhs_variable];
        const bool hasRhs = has_sample_since(rhs, since);
        bool failed = false;
        if (!hasLhs)
        {
            failed = !since || (hasRhs && is_non_finite(*rhs.value));
        }
        else if (lhsNonFinite)
        {
            failed = true;
        }
        else if (!since || hasRhs)
        {
            failed = !rhs.value || !holds(c.op, *lhs.value, *rhs.value, c.tolerance);
        }
        if (failed && std::find(out.begin(), out.end(), *c.rhs_variable) == out.end())
        {
            out.push_back(*c.rhs_variable);
        }
    }
    return out;
}

template <class Item>
bool has_distinct_lines(const std::vector<Item>& items) noexcept
{
    if (items.size() < 2)
    {
        return false;
    }
    std::size_t first = 0;
    for (const auto& item : items)
    {
        if (item.line == 0)
        {
            continue;
        }
        if (first == 0)
        {
            first = item.line;
        }
        else if (item.line != first)
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool ScenarioEngine::is_unmet(const ScenarioCondition& condition, std::optional<SimTime> since, bool& judged) const
{
    const Variable& lhs = variables_[condition.variable];
    const bool hasLhs = has_sample_since(lhs, since);
    if (!hasLhs)
    {
        if (!since)
        {
            judged = true;
            return true;
        }
        for (const ScenarioComparison& c : condition.all)
        {
            if (c.rhs_variable)
            {
                const Variable& rhs = variables_[*c.rhs_variable];
                if (has_sample_since(rhs, since) && is_non_finite(*rhs.value))
                {
                    judged = true;
                    return true;
                }
            }
        }
        judged = false;
        return false; // no sample of its ECU from `since` yet (and no non-finite RHS in window): not judged
    }
    if (!condition.all.empty() && is_non_finite(*lhs.value))
    {
        judged = true;
        return true;
    }
    bool any_judged = false;
    for (const ScenarioComparison& c : condition.all)
    {
        if (c.rhs_variable)
        {
            const Variable& rhs = variables_[*c.rhs_variable];
            if (since && (!rhs.value_time || *rhs.value_time < *since))
            {
                continue; // no sample of the right-hand ECU from `since` yet: checked at later samples / close()
            }
            any_judged = true;
            if (!rhs.value || !holds(c.op, *lhs.value, *rhs.value, c.tolerance))
            {
                judged = true;
                return true;
            }
        }
        else
        {
            any_judged = true;
            if (!holds(c, *lhs.value))
            {
                judged = true;
                return true;
            }
        }
    }
    judged = any_judged;
    return false;
}

TimelineFailure ScenarioEngine::unmet_values(const ScenarioStep& step, std::optional<SimTime> since) const
{
    TimelineFailure out;
    for (std::size_t i = 0; i < step.conditions.size(); ++i)
    {
        bool judged = false;
        const ScenarioCondition& condition = step.conditions[i];
        if (is_unmet(condition, since, judged))
        {
            const Variable& lhs = variables_[condition.variable];
            const bool hasLhs = has_sample_since(lhs, since);
            const std::optional<control::Value> lhsVal = hasLhs ? finite_or_empty(lhs.value) : std::nullopt;
            TimelineFailedCondition failed{
                i,
                lhsVal,
                lhsVal ? lhs.value_time : std::nullopt,
            };
            for (const std::size_t rhsIdx : failed_rhs_variables(condition, variables_, since))
            {
                const Variable& rhs = variables_[rhsIdx];
                const bool hasRhs = has_sample_since(rhs, since);
                const std::optional<control::Value> rhsVal = hasRhs ? finite_or_empty(rhs.value) : std::nullopt;
                failed.compared.push_back(TimelineComparedValue{
                    rhsIdx,
                    rhsVal,
                    rhsVal ? rhs.value_time : std::nullopt,
                });
            }
            out.conditions.push_back(std::move(failed));
        }
    }
    return out;
}

std::string ScenarioEngine::unmet(const ScenarioStep& step, std::optional<SimTime> since) const
{
    std::string out;
    const bool distinctClauseLines = has_distinct_lines(step.conditions);
    for (const ScenarioCondition& condition : step.conditions)
    {
        const Variable& lhs = variables_[condition.variable];
        const ScenarioVariable& v = scenario_.variables[condition.variable];
        bool judged = false;
        if (!is_unmet(condition, since, judged))
        {
            continue;
        }
        const bool hasLhs = has_sample_since(lhs, since);
        const std::string clauseLoc =
            distinctClauseLines && condition.line > 0 ? " (line " + std::to_string(condition.line) + ")" : std::string{};
        std::string entry =
            condition.text + clauseLoc + ", but " +
            (hasLhs ? "it was " + value_text(*lhs.value, &v) + " (its sample of " + ms_text(*lhs.value_time) + " ms)"
                    : since ? "it had no sample from " + ms_text(*since) + " ms yet"
                            : std::string("it had no value yet (no sample of its ECU had arrived)"));
        const std::vector<std::size_t> rhsVars = failed_rhs_variables(condition, variables_, since);
        if (!rhsVars.empty())
        {
            entry += " while ";
            for (std::size_t k = 0; k < rhsVars.size(); ++k)
            {
                const Variable& rhs = variables_[rhsVars[k]];
                const ScenarioVariable& rv = scenario_.variables[rhsVars[k]];
                const bool hasRhs = has_sample_since(rhs, since);
                if (k > 0)
                {
                    entry += " and ";
                }
                entry += rv.text + " " +
                         (hasRhs ? "was " + value_text(*rhs.value, &rv) + " (its sample of " + ms_text(*rhs.value_time) + " ms)"
                                 : since ? "had no sample from " + ms_text(*since) + " ms yet"
                                         : std::string("had no value yet (no sample of its ECU had arrived)"));
            }
        }
        out += (out.empty() ? "" : "; ") + entry;
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

template <class Item>
bool has_distinct_lines(const std::vector<Item>& items) noexcept
{
    if (items.size() < 2)
    {
        return false;
    }
    std::size_t first = 0;
    for (const auto& item : items)
    {
        if (item.line == 0)
        {
            continue;
        }
        if (first == 0)
        {
            first = item.line;
        }
        else if (item.line != first)
        {
            return true;
        }
    }
    return false;
}

} // namespace vehicle_sil::scenario
