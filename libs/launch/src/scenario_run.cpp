// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/scenario_run.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::launch {

namespace {

constexpr std::int64_t kNanosPerMilli = 1'000'000;

/// A virtual time in milliseconds, in the shortest form ("300", "2.5").
std::string ms_text(SimTime t)
{
    const std::int64_t ns = to_ns(t);
    if (ns % kNanosPerMilli == 0)
    {
        return std::to_string(ns / kNanosPerMilli);
    }
    std::array<char, 32> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<double>(ns) / static_cast<double>(kNanosPerMilli));
    return std::string(buffer.data(), result.ptr);
}

SimTime at_ms(std::int64_t ms)
{
    return SimTime{} + std::chrono::milliseconds(ms);
}

const char* target_status_text(control::TargetStatus status)
{
    switch (status)
    {
    case control::TargetStatus::Sent: return "it was not queued";
    case control::TargetStatus::Queued: return "it was queued, but no result came";
    case control::TargetStatus::Disconnected: return "the ECU's agent disconnected before a result came";
    case control::TargetStatus::Applied: return "applied";
    case control::TargetStatus::Late: return "it came too late for its step";
    case control::TargetStatus::Rejected: return "the ECU rejected it";
    case control::TargetStatus::Cancelled: return "the ECU ended before its time";
    case control::TargetStatus::Invalid: return "the ECU's agent refused it";
    case control::TargetStatus::Busy: return "the ECU's agent had too many commands waiting";
    case control::TargetStatus::Unknown: return "no result will come";
    }
    return "unknown";
}

double number_of(const control::Value& value)
{
    if (const auto* b = std::get_if<bool>(&value))
    {
        return *b ? 1.0 : 0.0;
    }
    if (const auto* i = std::get_if<std::int64_t>(&value))
    {
        return static_cast<double>(*i);
    }
    return std::get<double>(value);
}

/// A signal's value as a value of the variable's type: an integer is rounded to the nearest
/// (halves away from zero), a bool is true from 0.5.
///
/// Before an integer is rounded, the value is put on a grid of 2^-30: a value that is a half but
/// for the last bits of the arithmetic (a sine at 30 degrees is 0.49999999999999994 with one C
/// library and 0.5 with another) then rounds the same way everywhere. What can still differ by
/// one between platforms is a value that close to the middle between two points of the grid,
/// which a scenario does not meet by writing round numbers.
control::Value value_of_type(double value, control::ValueType type)
{
    switch (type)
    {
    case control::ValueType::Integer:
    case control::ValueType::Enum:
        if (std::fabs(value) < 0x1p22) // (times 2^30 it still has bits below one)
        {
            value = std::round(value * 0x1p30) * 0x1p-30;
        }
        return control::Value{static_cast<std::int64_t>(std::llround(value))};
    case control::ValueType::Bool: return control::Value{value >= 0.5};
    case control::ValueType::Real: break;
    }
    return control::Value{value};
}

} // namespace

const char* step_status_name(StepStatus status)
{
    switch (status)
    {
    case StepStatus::NotRun: return "not_run";
    case StepStatus::Running: return "running";
    case StepStatus::Passed: return "passed";
    case StepStatus::Failed: return "failed";
    case StepStatus::Unfinished: return "unfinished";
    case StepStatus::Skipped: return "skipped";
    }
    return "not_run";
}

std::size_t ScenarioRun::skipped() const
{
    return static_cast<std::size_t>(std::count_if(steps.begin(), steps.end(), [](const ScenarioStepRun& step) {
        return step.status == StepStatus::Skipped;
    }));
}

const char* scenario_status_name(ScenarioStatus status)
{
    switch (status)
    {
    case ScenarioStatus::NotRun: return "not_run";
    case ScenarioStatus::Passed: return "passed";
    case ScenarioStatus::Failed: return "failed";
    case ScenarioStatus::Incomplete: return "incomplete";
    }
    return "not_run";
}

ScenarioRun scenario_not_run(const Scenario& scenario)
{
    ScenarioRun run;
    run.name = scenario.name;
    for (std::size_t i = 0; i < scenario.steps.size(); ++i)
    {
        ScenarioStepRun step;
        step.index = i + 1;
        step.line = scenario.steps[i].line;
        step.kind = scenario.steps[i].kind;
        step.text = scenario.steps[i].text;
        if (!scenario.steps[i].skipped.empty())
        {
            step.status = StepStatus::Skipped;
            step.code = "scenario-skipped";
            step.message = scenario.steps[i].skipped;
        }
        run.steps.push_back(std::move(step));
    }
    return run;
}

ScenarioEngine::ScenarioEngine(const Scenario& scenario, SimDuration tick, ScenarioPort& port, OnStep on_step) :
  scenario_(scenario), tick_(tick), port_(port), on_step_(std::move(on_step))
{
    if (tick_ <= SimDuration::zero())
    {
        throw std::invalid_argument("ScenarioEngine: the tick must be > 0");
    }
    run_ = scenario_not_run(scenario);
    variables_.resize(scenario.variables.size());
    samples_.resize(scenario.participants.size());
    open_writes_.resize(scenario.steps.size(), 0);
}

SimTime ScenarioEngine::next_tick_after(SimTime t) const
{
    return SimTime{} + (to_ns(t) / tick_.count() + 1) * tick_;
}

SimTime ScenarioEngine::judge_time(const std::vector<ScenarioWrite>& writes, SimTime at) const
{
    SimTime latest = next_tick_after(at);
    for (const ScenarioWrite& w : writes)
    {
        const SimDuration step = scenario_.participants[scenario_.variables[w.variable].participant].step;
        if (step > SimDuration::zero())
        {
            // The ECU's first step at or after `at` applies it; its result is there at the
            // controller's first step after that one.
            const std::int64_t applied = (to_ns(at) + step.count() - 1) / step.count() * step.count();
            latest = std::max(latest, next_tick_after(SimTime{} + SimDuration(applied)));
        }
    }
    return latest;
}

void ScenarioEngine::begin(const std::vector<EnginePeer>& peers)
{
    if (begun_)
    {
        throw std::logic_error("ScenarioEngine::begin() must be called once");
    }
    begun_ = true;
    for (std::size_t i = 0; i < scenario_.variables.size(); ++i)
    {
        const ScenarioVariable& v = scenario_.variables[i];
        const std::string& participant = scenario_.participants[v.participant].name;
        if (v.participant >= peers.size() || !peers[v.participant].connected)
        {
            variables_[i].problem = "participant '" + participant +
                                    "' has no control link in this run: its agent did not connect before the simulation "
                                    "started (the run's control-link findings and its lines say why)";
            continue;
        }
        const auto& schema = peers[v.participant].schema;
        const auto it = std::find_if(schema.begin(), schema.end(), [&v](const control::VariableInfo& info) { return info.name == v.name; });
        if (it == schema.end() || it->type != v.type)
        {
            variables_[i].problem = "participant '" + participant + "' does not offer the variable '" + v.name +
                                    "' as --describe said it would (is the executable the one that was checked?)";
            continue;
        }
        variables_[i].index = static_cast<std::size_t>(it - schema.begin());
    }
    run_.status = ScenarioStatus::Incomplete;
    if (scenario_.steps.empty())
    {
        finish(ScenarioStatus::Passed);
        return;
    }
    issue();
    if (!over_)
    {
        submit(SimTime{} - SimDuration(1), SimTime{}, true);
    }
}

void ScenarioEngine::sample(std::size_t participant, std::shared_ptr<const control::Snapshot> sample)
{
    if (participant < samples_.size() && sample && !over_)
    {
        samples_[participant].push_back(std::move(sample));
    }
}

bool ScenarioEngine::tick(SimTime p)
{
    if (!begun_)
    {
        return false;
    }
    now_ = p;
    if (!over_)
    {
        judge(p);
    }
    if (!over_)
    {
        observe(p);
    }
    if (!over_)
    {
        submit(p, p + tick_, false);
    }
    if (over_ && !run_.over_at)
    {
        run_.over_at = p;
    }
    return over_;
}

void ScenarioEngine::stopped_at(SimTime p)
{
    if (over_ || !begun_)
    {
        return;
    }
    now_ = p;
    judge(p);
    if (!over_)
    {
        observe(p);
    }
    if (over_)
    {
        run_.over_at = p;
        return;
    }
    for (std::size_t i = 0; i < run_.steps.size(); ++i)
    {
        if (run_.steps[i].status != StepStatus::Passed && run_.steps[i].status != StepStatus::Skipped)
        {
            const ScenarioStepRun& step = run_.steps[i];
            fail(i, p, "scenario-out-of-time",
                 "the simulation stopped at " + ms_text(p) + " ms (stop_at_ms) " +
                     (step.status == StepStatus::Running
                          ? "before this step was decided (it began at " + ms_text(*step.start) + " ms)"
                          : step.start ? "before this step could be decided (its time is " + ms_text(*step.start) + " ms)"
                                       : std::string("before this step began")) +
                     ": there was not enough time for the scenario; raise stop_at_ms, or leave it out (the run then ends "
                     "with the scenario)");
            run_.over_at = p;
            return;
        }
    }
}

void ScenarioEngine::cut_short()
{
    if (!over_ && begun_)
    {
        finish(ScenarioStatus::Incomplete);
    }
}

void ScenarioEngine::superseded()
{
    if (!begun_ || run_.status == ScenarioStatus::Passed)
    {
        return;
    }
    for (ScenarioStepRun& step : run_.steps)
    {
        if (step.status == StepStatus::Failed)
        {
            step.status = StepStatus::Running;
            step.at.reset();
            step.code.clear();
            step.message.clear();
        }
    }
    run_.over_at.reset();
    finish(ScenarioStatus::Incomplete);
}

void ScenarioEngine::issue()
{
    while (!over_ && base_ && next_ < scenario_.steps.size())
    {
        const std::size_t index = next_++;
        const ScenarioStep& step = scenario_.steps[index];
        const SimTime time = step.at_ms ? at_ms(*step.at_ms) : *base_ + std::chrono::milliseconds(step.after_ms);
        issue_step(index, time);
    }
}

bool ScenarioEngine::unusable(std::size_t index, SimTime time)
{
    const ScenarioStep& step = scenario_.steps[index];
    std::vector<std::size_t> used;
    for (const auto& w : step.writes)
    {
        used.push_back(w.variable);
    }
    for (const auto& r : step.signals)
    {
        used.push_back(r.variable);
    }
    for (const auto& c : step.conditions)
    {
        used.push_back(c.variable);
    }
    for (const std::size_t v : used)
    {
        if (!variables_[v].problem.empty())
        {
            const bool writes = step.kind == StepKind::Set || step.kind == StepKind::Ramp || step.kind == StepKind::Wave;
            fail(index, time, writes ? "scenario-write" : "scenario-failed",
                 scenario_.variables[v].text + " cannot be " + (writes ? "written" : "read") + ": " + variables_[v].problem);
            return true;
        }
    }
    return false;
}

void ScenarioEngine::issue_step(std::size_t index, SimTime time)
{
    const ScenarioStep& step = scenario_.steps[index];
    run_.steps[index].start = time;
    if (!step.skipped.empty())
    {
        // Nothing to do. It ends after the time its text says (0 for a set or a wait), and the
        // next step counts from there, as in a configuration where the step runs. No `at`: it
        // was decided before the run, not at a time of the simulation.
        run_.steps[index].end = time + std::chrono::milliseconds(step.skipped_ms);
        base_ = run_.steps[index].end;
        ++done_;
        if (on_step_)
        {
            on_step_(run_.steps[index]);
        }
        if (done_ == run_.steps.size())
        {
            finish(ScenarioStatus::Passed);
        }
        return;
    }
    if (unusable(index, time))
    {
        return;
    }
    switch (step.kind)
    {
    case StepKind::Set:
    {
        Write write;
        write.step = index;
        write.at = time;
        write.writes = step.writes;
        to_send_.push_back(std::move(write));
        open_writes_[index] = 1;
        run_.steps[index].end = time;
        base_ = time;
        break;
    }
    case StepKind::Ramp:
    case StepKind::Wave:
    {
        std::int64_t longest = 0;
        for (const ScenarioSignal& r : step.signals)
        {
            Signal signal;
            signal.step = index;
            signal.signal = r;
            signal.from_time = time;
            signal.base = r.base;
            signal.period = scenario_.participants[scenario_.variables[r.variable].participant].step;
            if (signal.period <= SimDuration::zero())
            {
                signal.period = tick_;
            }
            // One write per period before the end, and the last one at the end.
            const std::int64_t over = r.length_ms * kNanosPerMilli;
            open_writes_[index] += static_cast<std::size_t>((over + signal.period.count() - 1) / signal.period.count()) + 1;
            signals_.push_back(std::move(signal));
            longest = std::max(longest, r.length_ms);
        }
        run_.steps[index].end = time + std::chrono::milliseconds(longest);
        base_ = run_.steps[index].end;
        break;
    }
    case StepKind::Expect:
    case StepKind::WaitUntil:
    {
        Watch watch;
        watch.step = index;
        watch.from = time;
        if (step.within_ms)
        {
            watch.until = time + std::chrono::milliseconds(*step.within_ms);
            base_.reset(); // known when it becomes true
        }
        else if (step.hold_for_ms)
        {
            watch.until = time + std::chrono::milliseconds(*step.hold_for_ms);
            watch.hold = true;
            run_.steps[index].end = *watch.until;
            base_ = *watch.until;
        }
        else
        {
            run_.steps[index].end = time;
            base_ = time;
        }
        watches_.push_back(watch);
        break;
    }
    }
}

void ScenarioEngine::started(std::size_t index)
{
    ScenarioStepRun& step = run_.steps[index];
    if (step.status == StepStatus::NotRun)
    {
        step.status = StepStatus::Running;
        if (on_step_)
        {
            on_step_(step);
        }
    }
}

void ScenarioEngine::pass(std::size_t index, SimTime at, SimTime end)
{
    started(index);
    ScenarioStepRun& step = run_.steps[index];
    step.status = StepStatus::Passed;
    step.at = at;
    step.end = end;
    ++done_;
    if (on_step_)
    {
        on_step_(step);
    }
    if (done_ == run_.steps.size())
    {
        finish(ScenarioStatus::Passed);
    }
}

void ScenarioEngine::fail(std::size_t index, SimTime at, std::string code, std::string message)
{
    started(index);
    ScenarioStepRun& step = run_.steps[index];
    step.status = StepStatus::Failed;
    step.at = at;
    if (step.kind == StepKind::WaitUntil || scenario_.steps[index].within_ms)
    {
        step.end.reset();
    }
    step.code = std::move(code);
    step.message = std::move(message);
    if (on_step_)
    {
        on_step_(step);
    }
    finish(ScenarioStatus::Failed);
}

void ScenarioEngine::finish(ScenarioStatus status)
{
    over_ = true;
    run_.status = status;
    for (ScenarioStepRun& step : run_.steps)
    {
        if (step.status == StepStatus::Running)
        {
            step.status = StepStatus::Unfinished;
            step.end.reset();
        }
        else if (step.status == StepStatus::NotRun)
        {
            step.start.reset();
            step.end.reset();
        }
    }
    samples_.clear();
    samples_.resize(scenario_.participants.size());
}

// --- writes ---

bool ScenarioEngine::submit_one(Write write)
{
    started(write.step);
    if (write.signal)
    {
        // The signal's value at this time, now that the writes before it have gone out.
        Signal& signal = signals_[*write.signal];
        const ScenarioVariable& v = scenario_.variables[signal.signal.variable];
        const Waveform& shape = *signal.signal.shape;
        if (!signal.base && shape.uses_base())
        {
            // Where the scenario left the variable: what it wrote last, whatever the samples
            // since say (another writer does not move the start). A sample only if the scenario
            // has not written the variable yet.
            const Variable& known = variables_[signal.signal.variable];
            if (known.written)
            {
                signal.base = number_of(*known.written);
            }
            else if (known.value)
            {
                signal.base = number_of(*known.value);
            }
            else
            {
                const std::string kind = step_kind_name(scenario_.steps[write.step].kind);
                fail(write.step, write.at, "scenario-write",
                     "the " + kind + " of " + v.text + " has no value to start from: no sample of its ECU has arrived before " +
                         ms_text(write.at) + " ms and the scenario has not written it; " + signal.signal.hint);
                return false;
            }
        }
        const double base = signal.base.value_or(0.0);
        const double value = write.signal_last ? shape.end(base) : shape.value(write.at - signal.from_time, base);
        write.writes.push_back(ScenarioWrite{signal.signal.variable, value_of_type(value, v.type)});
    }
    std::vector<control::PeerWrite> out;
    for (const ScenarioWrite& w : write.writes)
    {
        const ScenarioVariable& v = scenario_.variables[w.variable];
        out.push_back(control::PeerWrite{scenario_.participants[v.participant].name, v.name, w.value});
        variables_[w.variable].written = w.value;
    }
    write.ticket = port_.write(std::move(out), write.at);
    write.judge = judge_time(write.writes, write.at);
    // Refused at once (the agent's check, no room, no link): no need to wait for the step.
    if (const auto result = port_.result(write.ticket))
    {
        for (const control::TargetResult& target : result->targets)
        {
            if (control::is_final(target.status) && target.status != control::TargetStatus::Applied)
            {
                fail(write.step, write.at, "scenario-write",
                     "the write for " + ms_text(write.at) + " ms was not taken by participant '" + target.peer + "': " +
                         target_status_text(target.status) + (target.reason.empty() ? "" : " (" + target.reason + ")"));
                return false;
            }
        }
    }
    sent_.push_back(std::move(write));
    return true;
}

void ScenarioEngine::submit(SimTime from_exclusive, SimTime to_inclusive, bool at_start)
{
    const auto in_range = [&](SimTime t) { return t > from_exclusive && t <= to_inclusive; };
    // Cannot happen: a write's time is always after the step of the controller that issues it,
    // and tick() is called at every step.
    const auto missed = [&](std::size_t step, SimTime at) {
        fail(step, at, "scenario-write",
             "the write for " + ms_text(at) + " ms was not sent in time (the launcher stood at " + ms_text(now_) +
                 " ms already): this is an error of vehicle_sil_launch");
    };
    std::vector<Write> due;
    for (auto it = to_send_.begin(); it != to_send_.end();)
    {
        if (it->at <= from_exclusive && !at_start)
        {
            missed(it->step, it->at);
            return;
        }
        if (in_range(it->at))
        {
            due.push_back(std::move(*it));
            it = to_send_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (std::size_t i = 0; i < signals_.size(); ++i)
    {
        Signal& signal = signals_[i];
        const SimTime end = signal.from_time + std::chrono::milliseconds(signal.signal.length_ms);
        while (!signal.done)
        {
            const SimTime regular = signal.from_time + signal.next * signal.period;
            const bool last = regular >= end;
            const SimTime at = last ? end : regular;
            if (at <= from_exclusive && !at_start)
            {
                missed(signal.step, at);
                return;
            }
            if (!in_range(at))
            {
                break;
            }
            Write write;
            write.step = signal.step;
            write.at = at;
            write.signal = i;
            write.signal_last = last;
            due.push_back(std::move(write));
            ++signal.next;
            signal.done = last;
        }
    }
    // Writes for one time take effect in the order of the steps.
    std::stable_sort(due.begin(), due.end(), [](const Write& a, const Write& b) {
        return a.at != b.at ? a.at < b.at : a.step < b.step;
    });
    for (Write& write : due)
    {
        if (!submit_one(std::move(write)))
        {
            return;
        }
    }
}

void ScenarioEngine::judge(SimTime p)
{
    while (!over_ && !sent_.empty())
    {
        // In the order sent; the judge times of one participant's writes follow that order.
        const auto it = std::find_if(sent_.begin(), sent_.end(), [p](const Write& w) { return w.judge <= p; });
        if (it == sent_.end())
        {
            return;
        }
        const Write write = std::move(*it);
        sent_.erase(it);
        const auto result = port_.result(write.ticket);
        if (!result)
        {
            fail(write.step, write.at, "scenario-write",
                 "the result of the write for " + ms_text(write.at) + " ms is not known any more: this is an error of "
                                                                      "vehicle_sil_launch");
            return;
        }
        SimTime applied = write.at;
        for (const control::TargetResult& target : result->targets)
        {
            if (target.status != control::TargetStatus::Applied)
            {
                fail(write.step, write.at, "scenario-write",
                     "the write for " + ms_text(write.at) + " ms was not applied by participant '" + target.peer + "': " +
                         target_status_text(target.status) + (target.reason.empty() ? "" : " (" + target.reason + ")") +
                         (control::is_final(target.status)
                              ? ""
                              : "; at " + ms_text(p) + " ms the result of the ECU's step at or after " + ms_text(write.at) +
                                    " ms should be there (does the ECU step as --describe says?)"));
                return;
            }
            if (target.applied_at)
            {
                applied = std::max(applied, *target.applied_at);
            }
        }
        if (--open_writes_[write.step] == 0)
        {
            const ScenarioStepRun& step = run_.steps[write.step];
            pass(write.step, step.kind == StepKind::Ramp || step.kind == StepKind::Wave ? *step.end : applied, *step.end);
        }
    }
}

// --- samples ---

std::string ScenarioEngine::unmet(const ScenarioStep& step) const
{
    std::string out;
    for (const ScenarioCondition& condition : step.conditions)
    {
        const Variable& known = variables_[condition.variable];
        const ScenarioVariable& v = scenario_.variables[condition.variable];
        const bool met = known.value && std::all_of(condition.all.begin(), condition.all.end(), [&](const ScenarioComparison& c) {
            return holds(c, *known.value);
        });
        if (met)
        {
            continue;
        }
        out += (out.empty() ? "" : "; ") + condition.text + ", but " +
               (known.value ? "it was " + value_text(*known.value, &v) + " (its sample of " + ms_text(*known.value_time) + " ms)"
                            : std::string("it had no value yet (no sample of its ECU had arrived)"));
    }
    return out;
}

void ScenarioEngine::evaluate(Watch& watch, SimTime t)
{
    const ScenarioStep& step = scenario_.steps[watch.step];
    const std::string unmetNow = unmet(step);
    const bool ok = unmetNow.empty();
    if (!watch.until)
    {
        if (ok)
        {
            pass(watch.step, t, t);
        }
        else
        {
            fail(watch.step, t, "scenario-failed", "not true at " + ms_text(t) + " ms: " + unmetNow);
        }
        watch.step = run_.steps.size(); // decided
    }
    else if (watch.hold)
    {
        if (!ok)
        {
            fail(watch.step, t, "scenario-failed",
                 "not true for " + std::to_string(*step.hold_for_ms) + " ms (from " + ms_text(watch.from) + " ms): at " +
                     ms_text(t) + " ms " + unmetNow);
            watch.step = run_.steps.size();
        }
    }
    else if (ok)
    {
        // Known now, at the controller's step now_: the next step's writes can take effect from
        // the step after it.
        const std::size_t index = watch.step;
        watch.step = run_.steps.size();
        base_ = now_ + tick_;
        pass(index, t, *base_);
    }
}

void ScenarioEngine::close(Watch& watch, SimTime t)
{
    const ScenarioStep& step = scenario_.steps[watch.step];
    const std::size_t index = watch.step;
    watch.step = run_.steps.size();
    if (watch.hold)
    {
        pass(index, t, t);
    }
    else if (step.kind == StepKind::WaitUntil)
    {
        fail(index, t, "scenario-timeout",
             "timed out at " + ms_text(t) + " ms (" + std::to_string(*step.within_ms) + " ms after " + ms_text(watch.from) +
                 " ms): " + unmet(step));
    }
    else
    {
        fail(index, t, "scenario-failed",
             "not true at any time within " + std::to_string(*step.within_ms) + " ms (from " + ms_text(watch.from) + " ms to " +
                 ms_text(t) + " ms): at the end " + unmet(step));
    }
}

void ScenarioEngine::observe(SimTime p)
{
    // The samples of steps before p, by time; within one time in the order of the participants.
    struct Taken
    {
        SimTime time;
        std::size_t participant;
        std::shared_ptr<const control::Snapshot> sample;
    };
    std::vector<Taken> batch;
    for (std::size_t i = 0; i < samples_.size(); ++i)
    {
        while (!samples_[i].empty() && samples_[i].front()->time < p)
        {
            batch.push_back(Taken{samples_[i].front()->time, i, std::move(samples_[i].front())});
            samples_[i].pop_front();
        }
    }
    std::stable_sort(batch.begin(), batch.end(), [](const Taken& a, const Taken& b) {
        return a.time != b.time ? a.time < b.time : a.participant < b.participant;
    });
    const auto decided = [this](const Watch& w) { return w.step >= run_.steps.size(); };
    std::size_t next = 0;
    while (!over_)
    {
        watches_.erase(std::remove_if(watches_.begin(), watches_.end(), decided), watches_.end());
        // The next moment a watch begins or ends, if the samples up to it are all there.
        std::optional<SimTime> boundary;
        for (const Watch& w : watches_)
        {
            const std::optional<SimTime> at = !w.opened ? std::optional<SimTime>(w.from) : w.until;
            if (at && *at < p && (!boundary || *at < *boundary))
            {
                boundary = at;
            }
        }
        const bool haveSample = next < batch.size();
        if (!haveSample && !boundary)
        {
            break;
        }
        if (haveSample && (!boundary || batch[next].time <= *boundary))
        {
            const SimTime t = batch[next].time;
            for (; next < batch.size() && batch[next].time == t; ++next)
            {
                const Taken& taken = batch[next];
                for (std::size_t v = 0; v < variables_.size(); ++v)
                {
                    Variable& variable = variables_[v];
                    if (scenario_.variables[v].participant == taken.participant && variable.index &&
                        *variable.index < taken.sample->values.size())
                    {
                        variable.value = taken.sample->values[*variable.index];
                        variable.value_time = t;
                    }
                }
            }
            // Index loop: a watch that becomes true issues the steps after it (new watches).
            for (std::size_t i = 0; i < watches_.size() && !over_; ++i)
            {
                if (decided(watches_[i]))
                {
                    continue;
                }
                if (!watches_[i].opened && watches_[i].from <= t)
                {
                    watches_[i].opened = true;
                    started(watches_[i].step);
                }
                if (watches_[i].opened)
                {
                    evaluate(watches_[i], t);
                    // Its last moment: decided here, so that the steps are decided in their order.
                    if (!over_ && !decided(watches_[i]) && watches_[i].until == t)
                    {
                        close(watches_[i], t);
                    }
                    issue();
                }
            }
        }
        else
        {
            const SimTime t = *boundary;
            for (std::size_t i = 0; i < watches_.size() && !over_; ++i)
            {
                if (decided(watches_[i]))
                {
                    continue;
                }
                if (!watches_[i].opened && watches_[i].from == t)
                {
                    watches_[i].opened = true;
                    started(watches_[i].step);
                    evaluate(watches_[i], t);
                    issue();
                }
                else if (watches_[i].opened && watches_[i].until == t)
                {
                    close(watches_[i], t);
                    issue();
                }
            }
        }
    }
}

} // namespace vehicle_sil::launch
