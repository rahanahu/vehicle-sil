// SPDX-License-Identifier: MIT
#include "vehicle_sil/scenario/scenario_run.hpp"

#include "scenario_failure.hpp"
#include "vehicle_sil/scenario/detail/scenario_line.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::scenario {

namespace {

constexpr std::int64_t kNanosPerMilli = 1'000'000;

} // namespace

namespace detail {

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

} // namespace detail

using detail::ms_text;

namespace {

SimTime at_ms(std::int64_t ms)
{
    return SimTime{} + std::chrono::milliseconds(ms);
}

bool applies_before_stop(SimTime at, SimDuration step, const std::optional<SimTime>& stop)
{
    if (!stop) return true;
    const auto remainder = to_ns(at) % step.count();
    return at + SimDuration(remainder == 0 ? 0 : step.count() - remainder) < *stop;
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

/// "0 to 100", "100 at most", "0 at least".
std::string range_text(const ScenarioVariable& v)
{
    const auto text = [&v](double end) { return value_text(v.type == control::ValueType::Real ? control::Value{end} : control::Value{static_cast<std::int64_t>(end)}); };
    return v.min && v.max ? text(*v.min) + " to " + text(*v.max) : v.max ? "(" + text(*v.max) + " at most)" : v.min ? "(" + text(*v.min) + " at least)" : std::string{};
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

const char* step_result_text(StepStatus status)
{
    switch (status)
    {
    case StepStatus::NotRun: return "not run";
    case StepStatus::Running: return "running";
    case StepStatus::Passed: return "ok";
    case StepStatus::Failed: return "FAILED";
    case StepStatus::Unfinished: return "unfinished";
    case StepStatus::Skipped: return "skipped";
    }
    return "not run";
}

std::size_t ScenarioRun::skipped() const
{
    return static_cast<std::size_t>(std::count_if(steps.begin(), steps.end(), [](const ScenarioStepRun& step) {
        return step.status == StepStatus::Skipped;
    }));
}

std::size_t ScenarioRun::unchecked_expects() const
{
    return static_cast<std::size_t>(std::count_if(steps.begin(), steps.end(), [](const ScenarioStepRun& step) {
        return step.status != StepStatus::Skipped && !step.expect_skipped.empty();
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
    run.case_name = scenario.case_name;
    run.params = scenario.params;
    run.end_ms = scenario.end_ms;
    run.has_dither = scenario.has_dither;
    if (run.has_dither)
    {
        run.inputs = scenario_inputs_text(scenario);
        run.lines = scenario.lines;
    }
    for (std::size_t i = 0; i < scenario.steps.size(); ++i)
    {
        ScenarioStepRun step;
        step.index = i + 1;
        step.file = scenario.steps[i].file;
        step.line = scenario.steps[i].line;
        step.kind = scenario.steps[i].kind;
        step.text = scenario.steps[i].text;
        step.expect_skipped = scenario.steps[i].expect_skipped;
        step.name = scenario.steps[i].name;
        step.call_stack = scenario.steps[i].call_stack;
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

struct ScenarioEngine::Lines
{
    std::vector<detail::Line> lines;                       ///< per variable of the scenario
    std::vector<std::optional<detail::LineMotion>> placed; ///< per motion of motions_; empty until its time
    /// Per variable: the dither that is on its line (the line's overlay is its shape), or null.
    std::vector<const ScenarioOverlay*> dither;
    /// The variables that the scenario ever puts a dither on, in the order of Scenario::variables.
    std::vector<std::size_t> dithered;
};

ScenarioEngine::~ScenarioEngine() = default;

ScenarioEngine::ScenarioEngine(const Scenario& scenario, SimDuration tick, ScenarioPort& port, OnStep on_step,
                               TimelineRecord* record, std::optional<SimTime> stop_at) :
  scenario_(scenario), tick_(tick), stop_at_(stop_at),
  end_ms_(scenario.end_ms ? std::optional<SimTime>(at_ms(*scenario.end_ms)) : std::nullopt), port_(port),
  on_step_(std::move(on_step)), lines_(std::make_unique<Lines>()), record_(record)
{
    if (tick_ <= SimDuration::zero())
    {
        throw std::invalid_argument("ScenarioEngine: the tick must be > 0");
    }
    run_ = scenario_not_run(scenario);
    variables_.resize(scenario.variables.size());
    samples_.resize(scenario.participants.size());
    open_writes_.resize(scenario.steps.size(), 0);
    for (const ScenarioVariable& v : scenario.variables)
    {
        lines_->lines.emplace_back(v.type);
    }
    // What "inputs" declares is on the lines from the start.
    lines_->dither.resize(scenario.variables.size(), nullptr);
    std::vector<bool> dithered(scenario.variables.size(), false);
    for (const ScenarioOverlay& input : scenario.inputs)
    {
        lines_->lines[input.variable].overlay(input.shape);
        lines_->dither[input.variable] = &input;
        dithered[input.variable] = true;
        if (record_ != nullptr)
        {
            TimelineRecord::Line line;
            line.step = TimelineRecord::kNoStep;
            line.part = static_cast<std::size_t>(&input - scenario.inputs.data());
            line.variable = input.variable;
            record_->dither(std::move(line));
        }
    }
    for (const ScenarioStep& step : scenario.steps)
    {
        for (const ScenarioOverlay& overlay : step.overlays)
        {
            dithered[overlay.variable] = dithered[overlay.variable] || overlay.shape != nullptr;
        }
    }
    for (std::size_t v = 0; v < dithered.size(); ++v)
    {
        if (dithered[v])
        {
            lines_->dithered.push_back(v);
        }
    }
}

SimDuration ScenarioEngine::step_of(std::size_t variable) const
{
    const SimDuration step = scenario_.participants[scenario_.variables[variable].participant].step;
    return step > SimDuration::zero() ? step : tick_;
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
        if (it == schema.end() || it->type != v.type ||
            (v.type == control::ValueType::Enum && !enum_items_match(it->items, v.items)))
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
        if (!end_ms_ || *end_ms_ <= SimTime{})
        {
            finish(ScenarioStatus::Passed);
            close_record();
        }
        return;
    }
    issue();
    if (!over_)
    {
        submit(SimTime{} - SimDuration(1), SimTime{}, true);
    }
    close_record();
}

void ScenarioEngine::sample(std::size_t participant, std::shared_ptr<const control::Snapshot> sample)
{
    if (participant < samples_.size() && sample && !over_ && (!stop_at_ || sample->time < *stop_at_))
    {
        samples_[participant].push_back(std::move(sample));
    }
}

bool ScenarioEngine::tick(SimTime p)
{
    if (!begun_ || (stop_at_ && p >= *stop_at_))
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
    if (!over_ && can_settle_at(p))
    {
        settle_at(p);
    }
    if (!over_ && end_ms_ && p >= *end_ms_ && done_ == run_.steps.size() && sent_.empty() && to_send_.empty() &&
        watches_.empty() && base_.has_value() && next_ == scenario_.steps.size() &&
        std::all_of(motions_.begin(), motions_.end(), [](const Motion& m) { return m.done; }))
    {
        finish(ScenarioStatus::Passed);
    }
    if (!over_)
    {
        submit(p, p + tick_, false);
    }
    if (over_ && !run_.over_at)
    {
        run_.over_at = p;
    }
    close_record();
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
        observe(p, true);
    }
    if (!over_)
    {
        if (done_ == run_.steps.size() && (!end_ms_ || p >= *end_ms_) && sent_.empty() && to_send_.empty() &&
            watches_.empty() && base_.has_value() && next_ == scenario_.steps.size() &&
            std::all_of(motions_.begin(), motions_.end(), [](const Motion& m) { return m.done; }))
        {
            finish(ScenarioStatus::Passed);
        }
        else
        {
            finish(ScenarioStatus::Incomplete);
        }
    }
    run_.over_at = p;
    close_record();
}

void ScenarioEngine::cut_short()
{
    if (!over_ && begun_)
    {
        finish(ScenarioStatus::Incomplete);
        close_record();
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
    run_.code.clear();
    run_.message.clear();
    run_.failed_at.reset();
    run_.failed_file.clear();
    run_.failed_line = 0;
    run_.failed_name.clear();
    run_.failed_call_stack.clear();
    finish(ScenarioStatus::Incomplete);
    close_record();
}

void ScenarioEngine::issue()
{
    while (!over_ && base_ && next_ < scenario_.steps.size())
    {
        const std::size_t index = next_;
        const ScenarioStep& step = scenario_.steps[index];
        const SimTime time = step.at_ms ? at_ms(*step.at_ms) : *base_ + std::chrono::milliseconds(step.after_ms);
        if (stop_at_ && time >= *stop_at_)
        {
            // Later relative steps depend on this step; they must not use the previous base.
            // A suffix of steps already known to be skipped has no work and does not extend a
            // scenario past its last actual step, even if its nominal time is beyond the stop.
            const auto first = scenario_.steps.begin() + static_cast<std::ptrdiff_t>(next_);
            if (std::all_of(first, scenario_.steps.end(), [](const ScenarioStep& s) { return !s.skipped.empty(); }))
            {
                // Use the normal skipped path: it records nominal start/end and notifies the
                // observer, without waiting for these steps to occur in the simulation.
                while (!over_ && next_ < scenario_.steps.size())
                {
                    const std::size_t skipped = next_++;
                    const ScenarioStep& item = scenario_.steps[skipped];
                    const SimTime nominal = item.at_ms ? at_ms(*item.at_ms) : *base_ + std::chrono::milliseconds(item.after_ms);
                    issue_step(skipped, nominal);
                }
            }
            break;
        }
        ++next_;
        issue_step(index, time);
    }
}

bool ScenarioEngine::unusable(std::size_t index, SimTime time)
{
    const ScenarioStep& step = scenario_.steps[index];
    std::vector<std::pair<std::size_t, bool>> used; // the variable, and whether the step writes it
    for (const auto& w : step.writes)
    {
        if (applies_before_stop(time, step_of(w.variable), stop_at_)) used.emplace_back(w.variable, true);
    }
    for (const auto& r : step.motions)
    {
        if (applies_before_stop(time, step_of(r.variable), stop_at_)) used.emplace_back(r.variable, true);
    }
    for (const auto& o : step.overlays)
    {
        if (applies_before_stop(time, step_of(o.variable), stop_at_)) used.emplace_back(o.variable, true);
    }
    for (const auto& c : step.conditions)
    {
        used.emplace_back(c.variable, false);
        for (const auto& comp : c.all)
        {
            if (comp.rhs_variable)
            {
                used.emplace_back(*comp.rhs_variable, false);
            }
        }
    }
    for (const auto& [v, writes] : used)
    {
        if (!variables_[v].problem.empty())
        {
            TimelineFailure failure;
            if (writes)
            {
                const auto set = std::find_if(step.writes.begin(), step.writes.end(), [variable = v](const ScenarioWrite& w) { return w.variable == variable; });
                // Nothing was sent: the one write the step could not make.
                failure.writes.push_back(TimelineFailedWrite{
                    v, time, set == step.writes.end() ? std::nullopt : std::optional<control::Value>(set->value)});
                failure.write = 0;
            }
            else
            {
                for (std::size_t c = 0; c < step.conditions.size(); ++c)
                {
                    const ScenarioCondition& cond = step.conditions[c];
                    const bool usesVar =
                        cond.variable == v ||
                        std::any_of(cond.all.begin(), cond.all.end(),
                                    [variable = v](const ScenarioComparison& comp) { return comp.rhs_variable == variable; });
                    if (usesVar)
                    {
                        TimelineFailedCondition fc{c, std::nullopt, std::nullopt};
                        for (const ScenarioComparison& comp : cond.all)
                        {
                            if (comp.rhs_variable &&
                                std::none_of(fc.compared.begin(), fc.compared.end(),
                                             [rhs = *comp.rhs_variable](const TimelineComparedValue& cv) {
                                                 return cv.variable == rhs;
                                             }))
                            {
                                fc.compared.push_back(TimelineComparedValue{*comp.rhs_variable, std::nullopt, std::nullopt});
                            }
                        }
                        failure.conditions.push_back(std::move(fc));
                    }
                }
            }
            fail(index, time, writes ? "scenario-write" : "scenario-failed",
                 scenario_.variables[v].text + " cannot be " + (writes ? "written" : "read") + ": " + variables_[v].problem,
                 std::move(failure));
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
        if (done_ == run_.steps.size() && (!end_ms_ || (now_ >= *end_ms_ && sent_.empty())))
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
    case StepKind::Dither:
    {
        // One write at its time, whose values are worked out when it is sent (the lines then).
        Write write;
        write.step = index;
        write.at = time;
        write.dither = true;
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
        SimTime lastApplied = time; // the latest step of an ECU that applies an end value
        for (const ScenarioMotion& r : step.motions)
        {
            Motion motion;
            motion.step = index;
            motion.motion = r;
            motion.part = static_cast<std::size_t>(&r - step.motions.data());
            motion.from_time = time;
            motion.period = scenario_.participants[scenario_.variables[r.variable].participant].step;
            if (motion.period <= SimDuration::zero())
            {
                motion.period = tick_;
            }
            // One write per period before the end, and the last one at the end.
            const std::int64_t over = r.length_ms * kNanosPerMilli;
            open_writes_[index] += static_cast<std::size_t>((over + motion.period.count() - 1) / motion.period.count()) + 1;
            // The ECU's first step at or after the end applies the end value.
            const std::int64_t end = to_ns(time) + over;
            lastApplied = std::max(lastApplied, SimTime{} + SimDuration((end + motion.period.count() - 1) / motion.period.count() *
                                                                        motion.period.count()));
            motions_.push_back(std::move(motion));
            lines_->placed.emplace_back();
            longest = std::max(longest, r.length_ms);
        }
        run_.steps[index].end = time + std::chrono::milliseconds(longest);
        base_ = run_.steps[index].end;
        if (!step.conditions.empty())
        {
            // The expect beside it: true at every sample from T up to the step that applies the
            // end value (E, or the ECU's next step if E is between two), and one more thing that
            // decides the step.
            Watch watch;
            watch.step = index;
            watch.from = time;
            watch.until = lastApplied;
            watch.hold = true;
            watch.beside = true;
            watches_.push_back(watch);
            if (record_ != nullptr)
            {
                record_->watch(index, TimelineRecord::Watch{WatchMode::During, watch.from, watch.until});
            }
            ++open_writes_[index];
        }
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
        if (record_ != nullptr)
        {
            record_->watch(index, TimelineRecord::Watch{!watch.until ? WatchMode::At : watch.hold ? WatchMode::Always : WatchMode::Once,
                                                        watch.from, watch.until});
        }
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
    if (done_ == run_.steps.size() && (!end_ms_ || (now_ >= *end_ms_ && sent_.empty())))
    {
        finish(ScenarioStatus::Passed);
    }
}

void ScenarioEngine::fail(std::size_t index, SimTime at, std::string code, std::string message, TimelineFailure failure)
{
    if (record_ != nullptr)
    {
        record_->failed(index, std::move(failure));
    }
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
    if (record_ != nullptr && !record_closed_)
    {
        // Not looked at, and nothing is decided by them any more: for the record (close_record()).
        for (std::size_t i = 0; i < samples_.size(); ++i)
        {
            for (auto& sample : samples_[i])
            {
                left_.emplace_back(i, std::move(sample));
            }
        }
    }
    samples_.clear();
    samples_.resize(scenario_.participants.size());
}

void ScenarioEngine::close_record()
{
    if (record_ == nullptr || record_closed_ || !over_)
    {
        return;
    }
    record_closed_ = true;
    // Recorded to: the time that decided the scenario.
    SimTime to = last_seen_.value_or(SimTime{});
    if (run_.status == ScenarioStatus::Incomplete)
    {
        // A write can be observed by an ECU even when no condition watches any samples. Keep
        // writes reached by the controller, but not future writes submitted ahead of its step.
        for (const auto& series : record_->series())
        {
            for (const auto& point : series.written)
            {
                if (point.at <= now_) to = std::max(to, point.at);
            }
        }
    }
    if (run_.status != ScenarioStatus::Incomplete)
    {
        to = SimTime{};
        bool has_step_failure = false;
        for (const ScenarioStepRun& step : run_.steps)
        {
            if (step.at && step.status == StepStatus::Failed)
            {
                to = *step.at;
                has_step_failure = true;
                break;
            }
            if (step.at && step.status == StepStatus::Passed)
            {
                to = std::max(to, *step.at);
            }
        }
        if (!has_step_failure && run_.status == ScenarioStatus::Failed && run_.failed_at)
        {
            to = std::max(to, *run_.failed_at);
        }
        if (run_.status == ScenarioStatus::Passed && end_ms_)
        {
            to = std::max(to, std::min(*end_ms_, now_));
        }
        // The samples up to it that were not looked at. Those of steps before the controller's
        // step being handled have all arrived; a later one is there in one run and not in another.
        std::stable_sort(left_.begin(), left_.end(), [](const auto& a, const auto& b) {
            return a.second->time != b.second->time ? a.second->time < b.second->time : a.first < b.first;
        });
        for (const auto& [participant, sample] : left_)
        {
            if (sample->time > to || sample->time >= now_)
            {
                continue;
            }
            for (std::size_t v = 0; v < variables_.size(); ++v)
            {
                if (scenario_.variables[v].participant == participant && variables_[v].index &&
                    *variables_[v].index < sample->values.size())
                {
                    record_->sampled(v, sample->time, sample->values[*variables_[v].index]);
                }
            }
        }
    }
    left_.clear();
    record_->close(to);
}

// --- writes ---

bool ScenarioEngine::submit_one(Write write, std::size_t unchecked)
{
    const auto at_or_after_stop = [&](std::size_t variable) {
        return !applies_before_stop(write.at, step_of(variable), stop_at_);
    };
    if (write.motion && at_or_after_stop(motions_[*write.motion].motion.variable))
    {
        return true;
    }
    // Its step begins when the engine looks at what became of it (taken_at_once()), in the order
    // sent: a write after one that was refused at once leaves its step NotRun.
    bool omitted_overlay = false;
    bool usable_overlay = false;
    if (write.grid)
    {
        // Its values were worked out when it was made (submit()).
    }
    else if (write.dither)
    {
        // The step's inputs that have something to swing around: the line as it stands now, with
        // the dither the step put (none: the line's value alone). An input the scenario has put
        // nothing on yet is not written: its dither waits for the first set, ramp or wave.
        for (const ScenarioOverlay& overlay : scenario_.steps[write.step].overlays)
        {
            if (at_or_after_stop(overlay.variable))
            {
                omitted_overlay = true;
                continue;
            }
            usable_overlay = true;
            if (const auto value = lines_->lines[overlay.variable].written_value(write.at))
            {
                write.writes.push_back(ScenarioWrite{overlay.variable, *value});
                write.bases.push_back(*lines_->lines[overlay.variable].number(write.at));
            }
        }
        if (write.writes.empty())
        {
            if (!usable_overlay) return true;
            if (omitted_overlay) ++open_writes_[write.step];
            // Nothing to send. Decided like a write all the same, at the controller's first step
            // after its time (judge()): not now, when its time may still be ahead.
            write.judge = next_tick_after(write.at);
            sent_.push_back(std::move(write));
            return true;
        }
    }
    else if (write.motion)
    {
        // The motion's value at this time. It was put on its line before the writes of its
        // first time went out (place()); without a base it fails here, at its first write.
        const Motion& motion = motions_[*write.motion];
        const ScenarioVariable& v = scenario_.variables[motion.motion.variable];
        const std::optional<detail::LineMotion>& placed = lines_->placed[*write.motion];
        if (!placed || !placed->has_value())
        {
            // A write that went out before this one and was refused at once comes first.
            if (!taken_at_once(unchecked))
            {
                return false;
            }
            const std::string kind = step_kind_name(scenario_.steps[write.step].kind);
            TimelineFailure failure;
            // Nothing was sent: the one write the step could not make.
            failure.writes.push_back(TimelineFailedWrite{motion.motion.variable, write.at, std::nullopt});
            failure.write = 0;
            fail(write.step, write.at, "scenario-write",
                 "the " + kind + " of " + v.text + " has no value to start from: no sample of its ECU has arrived before " +
                     ms_text(write.at) + " ms and the scenario has not written it; " + motion.motion.hint,
                 std::move(failure));
            return false;
        }
        const double number = detail::motion_number(*placed, write.at);
        write.writes.push_back(ScenarioWrite{motion.motion.variable, lines_->lines[motion.motion.variable].written(number, write.at)});
        write.bases.push_back(number);
    }
    else
    {
        // A set: its value, with what is on top of the line at this time (written()).
        for (ScenarioWrite& w : write.writes)
        {
            write.bases.push_back(detail::number_of(w.value));
            w.value = lines_->lines[w.variable].written(w.value, write.at);
        }
    }
    // A command can address ECUs with different periods. Keep the targets that can apply it
    // before the stop, preserving their order and the positions of writes within each ECU.
    if (stop_at_)
    {
        std::vector<ScenarioWrite> before_stop;
        std::vector<double> before_stop_bases;
        for (std::size_t i = 0; i < write.writes.size(); ++i)
        {
            if (!at_or_after_stop(write.writes[i].variable))
            {
                before_stop.push_back(std::move(write.writes[i]));
                before_stop_bases.push_back(write.bases[i]);
            }
        }
        if (before_stop.empty())
        {
            return true;
        }
        if ((before_stop.size() != write.writes.size() || omitted_overlay) && !write.grid)
        {
            // The submitted targets still need their results checked, but their success cannot
            // complete a step whose other targets will not run before the boundary.
            ++open_writes_[write.step];
        }
        write.writes = std::move(before_stop);
        write.bases = std::move(before_stop_bases);
    }
    std::vector<control::PeerWrite> out;
    for (std::size_t i = 0; i < write.writes.size(); ++i)
    {
        const ScenarioWrite& w = write.writes[i];
        const ScenarioVariable& v = scenario_.variables[w.variable];
        out.push_back(control::PeerWrite{scenario_.participants[v.participant].name, v.name, w.value});
        write.sent.push_back(record_ != nullptr
                                 ? record_->written(write.grid ? TimelineRecord::kNoStep : write.step, w.variable, write.at, w.value,
                                                    write.motion.has_value() || write.grid, write.bases[i])
                                 : std::nullopt);
    }
    write.ticket = port_.write(std::move(out), write.at);
    write.judge = judge_time(write.writes, write.at);
    // Refused by the hub (no link, an unknown variable, no room): known now, without waiting.
    // Nothing more goes out; the writes before it come first (taken_at_once()).
    const auto result = port_.result(write.ticket);
    const bool refused = result && std::any_of(result->targets.begin(), result->targets.end(), [](const control::TargetResult& t) {
                             return control::is_final(t.status) && t.status != control::TargetStatus::Applied;
                         });
    sent_.push_back(std::move(write));
    return !refused || taken_at_once(unchecked);
}

bool ScenarioEngine::taken_at_once(std::size_t from)
{
    if (from >= sent_.size())
    {
        return true;
    }
    port_.settle();
    for (std::size_t i = from; i < sent_.size(); ++i)
    {
        const Write& write = sent_[i];
        if (!write.grid)
        {
            started(write.step);
        }
        if (write.writes.empty())
        {
            continue; // a dither step with nothing to write: nothing went out
        }
        // Refused at once (the agent's check, no room, no link): no need to wait for the step.
        const auto result = port_.result(write.ticket);
        if (!result)
        {
            // (Cannot happen: as judge().)
            if (write.grid)
            {
                control::TargetResult lost;
                lost.peer = scenario_.participants[scenario_.variables[write.writes.front().variable].participant].name;
                lost.status = control::TargetStatus::Unknown;
                lost.reason = "the result is not known any more: this is an error of vehicle_sil_launch";
                fail_grid(write, nullptr, lost, "did not take");
            }
            else
            {
                fail(write.step, write.at, "scenario-write",
                     "the result of the write for " + ms_text(write.at) + " ms is not known any more: this is an error of "
                                                                          "vehicle_sil_launch",
                     failed_write(write, nullptr, {}));
            }
            sent_.erase(sent_.begin() + static_cast<std::ptrdiff_t>(i), sent_.end());
            return false;
        }
        for (const control::TargetResult& target : result->targets)
        {
            if (control::is_final(target.status) && target.status != control::TargetStatus::Applied)
            {
                if (write.grid)
                {
                    fail_grid(write, &*result, target, "did not take");
                }
                else
                {
                    TimelineFailure failure = failed_write(write, &*result, target);
                    const std::string which = applied_text(failure, target.peer); // before `failure` is moved from
                    fail(write.step, write.at, "scenario-write",
                         "the write for " + ms_text(write.at) + " ms was not taken by participant '" + target.peer +
                             "': " + target_status_text(target.status) +
                             (target.reason.empty() ? "" : " (" + target.reason + ")") + which,
                         std::move(failure));
                }
                // This write and the ones after it are not judged: as if they had not been made.
                sent_.erase(sent_.begin() + static_cast<std::ptrdiff_t>(i), sent_.end());
                return false;
            }
        }
    }
    return true;
}

void ScenarioEngine::submit(SimTime from_exclusive, SimTime to_inclusive, bool at_start)
{
    const auto in_range = [&](SimTime t) { return t > from_exclusive && t <= to_inclusive && (!stop_at_ || t < *stop_at_); };
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
    for (std::size_t i = 0; i < motions_.size(); ++i)
    {
        Motion& motion = motions_[i];
        const SimTime end = motion.from_time + std::chrono::milliseconds(motion.motion.length_ms);
        while (!motion.done)
        {
            const SimTime regular = motion.from_time + motion.next * motion.period;
            const bool last = regular >= end;
            const SimTime at = last ? end : regular;
            if (at <= from_exclusive && !at_start)
            {
                missed(motion.step, at);
                return;
            }
            if (!in_range(at))
            {
                break;
            }
            Write write;
            write.step = motion.step;
            write.at = at;
            write.motion = i;
            write.motion_last = last;
            due.push_back(std::move(write));
            ++motion.next;
            motion.done = last;
        }
    }
    // Writes for one time take effect in the order of the steps.
    std::stable_sort(due.begin(), due.end(), [](const Write& a, const Write& b) {
        return a.at != b.at ? a.at < b.at : a.step < b.step;
    });
    // The times at which a dither alone may write: the steps of the ECUs of the inputs that ever
    // have one, k x step. (None without a dither in the scenario: the loop below is then the
    // writes of the steps alone.)
    std::vector<SimTime> grid;
    for (const std::size_t v : lines_->dithered)
    {
        const std::int64_t step = step_of(v).count();
        const std::int64_t from = to_ns(from_exclusive);
        // The first multiple of the step after `from` (which is -1 at the start: 0).
        for (std::int64_t t = from < 0 ? 0 : (from / step + 1) * step; t <= to_ns(to_inclusive); t += step)
        {
            const SimTime at = SimTime{} + SimDuration(t);
            if (!in_range(at) || (end_ms_ && at >= *end_ms_)) break;
            grid.push_back(at);
        }
    }
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());

    std::vector<bool> stepWrites(scenario_.variables.size(), false); // per variable: a step writes it at the time at hand
    // The writes go out one after the other without waiting; the engine waits once, after the
    // last, and then looks at what the agents said (taken_at_once()).
    const std::size_t unchecked = sent_.size();
    for (std::size_t first = 0, next = 0; first < due.size() || next < grid.size();)
    {
        const SimTime t = first < due.size() && (next == grid.size() || due[first].at <= grid[next]) ? due[first].at : grid[next];
        std::size_t end = first;
        while (end < due.size() && due[end].at == t)
        {
            ++end;
        }
        // Time by time. Before any write of a time goes out, the steps that begin at it put what
        // they do on the lines, in the order of the steps: a motion's base is where the line
        // stands then, whether or not the write of a step before it is taken; and every write of
        // the time carries the dither that was put last at it.
        for (std::size_t i = first; i < end; ++i)
        {
            place(due[i]);
        }
        const bool onGrid = next < grid.size() && grid[next] == t;
        if (onGrid)
        {
            ++next;
            std::fill(stepWrites.begin(), stepWrites.end(), false);
            for (std::size_t i = first; i < end; ++i)
            {
                for (const ScenarioWrite& w : due[i].writes)
                {
                    stepWrites[w.variable] = true;
                }
                if (due[i].motion)
                {
                    stepWrites[motions_[*due[i].motion].motion.variable] = true;
                }
                if (due[i].dither)
                {
                    for (const ScenarioOverlay& overlay : scenario_.steps[due[i].step].overlays)
                    {
                        stepWrites[overlay.variable] = true;
                    }
                }
            }
        }
        for (; first < end; ++first)
        {
            if (!submit_one(std::move(due[first]), unchecked))
            {
                return;
            }
        }
        if (onGrid)
        {
            // After the steps' writes of this time: the inputs whose dither is all that moves
            // them now, in the order of the variables, as one command per participant.
            Write write;
            write.at = t;
            write.grid = true;
            for (const std::size_t v : lines_->dithered)
            {
                const detail::Line& line = lines_->lines[v];
                if (!line.overlay() || stepWrites[v] || to_ns(t) % step_of(v).count() != 0)
                {
                    continue;
                }
                // While a motion runs (from its time to its end, both included) its own writes
                // carry the dither.
                if (line.motion() && t <= line.motion()->from + line.motion()->length)
                {
                    continue;
                }
                if (const auto value = line.written_value(t))
                {
                    write.writes.push_back(ScenarioWrite{v, *value});
                    write.bases.push_back(*line.number(t));
                }
            }
            if (!write.writes.empty() && !submit_one(std::move(write), unchecked))
            {
                return;
            }
        }
    }
    taken_at_once(unchecked);
}

void ScenarioEngine::fail_grid(const Write& write, const control::OperationResult* result, const control::TargetResult& target,
                               const std::string& did_not, const std::string& hint)
{
    // What became of every write of the request, and which of them is the reason, if that can
    // be told (failed_write(): the only one of the participant's command, or the one the result
    // points at). The ECU has applied the writes of its command before the one it rejected.
    TimelineFailure failure = failed_write(write, result, target);
    // The writes of the command that the participant refused.
    std::vector<const ScenarioWrite*> command;
    for (const ScenarioWrite& w : write.writes)
    {
        const ScenarioVariable& v = scenario_.variables[w.variable];
        if (scenario_.participants[v.participant].name == target.peer)
        {
            command.push_back(&w);
        }
    }
    const ScenarioWrite* named = failure.write ? &write.writes[*failure.write] : nullptr;
    const std::string why = std::string(target_status_text(target.status)) + (target.reason.empty() ? "" : " (" + target.reason + ")") + hint;
    std::string message;
    if (named != nullptr)
    {
        const ScenarioVariable& v = scenario_.variables[named->variable];
        const ScenarioOverlay* dither = lines_->dither[named->variable];
        message = "the dither of " + v.text +
                  (dither != nullptr ? " (" +
                                           (dither->file.empty() ? "line " + std::to_string(dither->line)
                                                                 : dither->file + ":" + std::to_string(dither->line)) +
                                           ")"
                                     : "") +
                  " wrote " + value_text(named->value, &v) + " at " + ms_text(write.at) + " ms, ";
        // Beyond its range, if that is what the agent says (not for a value that happens to be
        // outside while the write failed of something else: no link, no room, a setter).
        // This find("out of range") is the one place left that reads a reason's text to learn
        // what kind of refusal it is: a result has no field for the kind yet (a later revision
        // of the control messages can add one; then compare that field here).
        const double number = detail::number_of(named->value);
        const bool above = v.max && number > *v.max;
        const bool below = v.min && number < *v.min;
        if (target.status == control::TargetStatus::Invalid && target.reason.find("out of range") != std::string::npos && (above || below))
        {
            message += std::string(above ? "above" : "below") + " its range " + range_text(v) + " (" + target.reason +
                       "): lower its amplitude, or keep the value further from the end of the range";
        }
        else
        {
            message += "and participant '" + target.peer + "' " + did_not + " the write: " + why;
        }
        if (command.size() > 1)
        {
            message += "; the dither" + std::string(command.size() > 2 ? "s" : "") + " of " + std::to_string(command.size() - 1) +
                       " other input" + (command.size() > 2 ? "s" : "") + " of that participant " +
                       (command.size() > 2 ? "were" : "was") + " in the same command";
        }
    }
    else
    {
        // Not known which: the participant's command as a whole, with everything in it.
        std::string all;
        for (std::size_t i = 0; i < command.size(); ++i)
        {
            const ScenarioVariable& v = scenario_.variables[command[i]->variable];
            all += (i == 0 ? "" : i + 1 == command.size() ? " and " : ", ") + v.text + " " + value_text(command[i]->value, &v);
        }
        message = "the dithers of participant '" + target.peer + "' wrote " + all + " at " + ms_text(write.at) +
                  " ms as one command, and the participant " + did_not + " it: " + why +
                  " (which of the inputs it was about is not known; the reason may say)";
    }
    message += applied_text(failure, target.peer);
    // It is no step's write: the first step that is not decided yet fails (as when the
    // simulation stops by itself, stopped_at()).
    const auto undecided = std::find_if(run_.steps.begin(), run_.steps.end(), [](const ScenarioStepRun& step) {
        return step.status != StepStatus::Passed && step.status != StepStatus::Skipped;
    });
    if (undecided != run_.steps.end())
    {
        fail(static_cast<std::size_t>(undecided - run_.steps.begin()), write.at, "scenario-write",
             std::move(message), std::move(failure));
        return;
    }
    // All steps are already Passed or Skipped (e.g. a dither grid write failed in (last_step_at, end_ms]):
    // fail at the scenario level without overwriting a decided step or emitting a contradictory step event.
    if (record_ != nullptr)
    {
        record_->failed(TimelineRecord::kNoStep, std::move(failure));
    }
    run_.code = "scenario-write";
    run_.message = std::move(message);
    run_.failed_at = write.at;
    if (named != nullptr)
    {
        if (const ScenarioOverlay* dither = lines_->dither[named->variable]; dither != nullptr)
        {
            run_.failed_file = dither->file;
            run_.failed_line = dither->line;
            run_.failed_name = dither->name;
            run_.failed_call_stack = dither->call_stack;
        }
    }
    else if (!command.empty())
    {
        const ScenarioOverlay* firstDither = lines_->dither[command.front()->variable];
        if (firstDither != nullptr &&
            std::all_of(command.begin() + 1, command.end(), [&](const ScenarioWrite* w) {
                const ScenarioOverlay* other = lines_->dither[w->variable];
                return other != nullptr && other->file == firstDither->file && other->line == firstDither->line &&
                       other->name == firstDither->name && other->call_stack == firstDither->call_stack;
            }))
        {
            run_.failed_file = firstDither->file;
            run_.failed_line = firstDither->line;
            run_.failed_name = firstDither->name;
            run_.failed_call_stack = firstDither->call_stack;
        }
    }
    finish(ScenarioStatus::Failed);
}

void ScenarioEngine::place(const Write& write)
{
    if (write.dither)
    {
        const std::vector<ScenarioOverlay>& overlays = scenario_.steps[write.step].overlays;
        for (std::size_t i = 0; i < overlays.size(); ++i)
        {
            const ScenarioOverlay& overlay = overlays[i];
            if (!applies_before_stop(write.at, step_of(overlay.variable), stop_at_)) continue;
            lines_->lines[overlay.variable].overlay(overlay.shape);
            lines_->dither[overlay.variable] = overlay.shape ? &overlay : nullptr;
            if (record_ != nullptr)
            {
                TimelineRecord::Line line;
                line.step = write.step;
                line.part = i;
                line.variable = overlay.variable;
                record_->dither(std::move(line));
            }
        }
        return;
    }
    if (!write.motion)
    {
        for (std::size_t i = 0; i < write.writes.size(); ++i)
        {
            const ScenarioWrite& w = write.writes[i];
            if (!applies_before_stop(write.at, step_of(w.variable), stop_at_)) continue;
            lines_->lines[w.variable].set(write.at, w.value);
            if (record_ != nullptr)
            {
                record_->line(TimelineRecord::Line{write.step, i, w.variable, write.at, w.value,
                                                   detail::base_source_name(detail::BaseSource::Written)});
            }
        }
        return;
    }
    const Motion& motion = motions_[*write.motion];
    if (write.at != motion.from_time)
    {
        return; // not its first write
    }
    const std::size_t v = motion.motion.variable;
    if (!applies_before_stop(write.at, step_of(v), stop_at_)) return;
    // A sample only if the scenario has put nothing on the line yet: the latest looked at.
    const detail::LineMotion& placed = lines_->lines[v].move(write.at, motion.motion.shape, std::chrono::milliseconds(motion.motion.length_ms),
                                                             motion.motion.base, variables_[v].value);
    lines_->placed[*write.motion] = placed;
    if (record_ != nullptr)
    {
        // The base as the file shows it: the number the step names as it is (a bool as a bool),
        // what the line or the sample had as the variable's type has it.
        const control::ValueType type = scenario_.variables[v].type;
        std::optional<control::Value> base;
        if (placed.base)
        {
            base = placed.source == detail::BaseSource::Written && type != control::ValueType::Bool
                       ? control::Value{*placed.base}
                       : detail::value_of_type(*placed.base, type);
        }
        record_->line(TimelineRecord::Line{write.step, motion.part, v, write.at, std::move(base), detail::base_source_name(placed.source)});
    }
}

SimTime ScenarioEngine::applied_step(const Write& write) const
{
    SimTime latest = write.at;
    for (const ScenarioWrite& w : write.writes)
    {
        const SimDuration step = step_of(w.variable);
        const std::int64_t applied = (to_ns(write.at) + step.count() - 1) / step.count() * step.count();
        latest = std::max(latest, SimTime{} + SimDuration(applied));
    }
    return latest;
}

bool ScenarioEngine::can_settle_at(SimTime p) const
{
    if (!end_ms_ || p < *end_ms_ || over_)
    {
        return false;
    }
    if (!to_send_.empty() || !watches_.empty() || !base_.has_value() || next_ != scenario_.steps.size() || sent_.empty())
    {
        return false;
    }
    if (std::any_of(motions_.begin(), motions_.end(), [](const Motion& m) { return !m.done; }))
    {
        return false;
    }
    return std::all_of(sent_.begin(), sent_.end(), [this, p](const Write& w) { return applied_step(w) <= p; });
}

void ScenarioEngine::settle_at(SimTime p)
{
    while (!over_ && !sent_.empty())
    {
        Write write = std::move(sent_.front());
        sent_.pop_front();
        if (write.writes.empty())
        {
            judge_write(write, std::nullopt, p);
            continue;
        }
        const auto result = port_.wait_done(write.ticket);
        if (result && std::any_of(result->targets.begin(), result->targets.end(),
                                  [](const control::TargetResult& t) { return !control::is_final(t.status); }))
        {
            sent_.push_front(std::move(write));
            return;
        }
        judge_write(write, result, p);
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
        judge_write(write, write.writes.empty() ? std::nullopt : port_.result(write.ticket), p);
    }
}

void ScenarioEngine::judge_write(const Write& write, const std::optional<control::OperationResult>& result, SimTime p)
{
    if (write.writes.empty())
    {
        // A dither step that had nothing to write (no input of it had a value yet).
        if (--open_writes_[write.step] == 0)
        {
            pass(write.step, write.at, *run_.steps[write.step].end);
        }
        return;
    }
    if (!result)
    {
        if (write.grid)
        {
            // (Cannot happen. Said of the participant of its first write.)
            control::TargetResult lost;
            lost.peer = scenario_.participants[scenario_.variables[write.writes.front().variable].participant].name;
            lost.status = control::TargetStatus::Unknown;
            lost.reason = "the result is not known any more: this is an error of vehicle_sil_launch";
            fail_grid(write, nullptr, lost, "did not apply");
            return;
        }
        fail(write.step, write.at, "scenario-write",
             "the result of the write for " + ms_text(write.at) + " ms is not known any more: this is an error of "
                                                                  "vehicle_sil_launch",
             failed_write(write, nullptr, {}));
        return;
    }
    SimTime applied = write.at;
    for (const control::TargetResult& target : result->targets)
    {
        if (target.status != control::TargetStatus::Applied && write.grid)
        {
            fail_grid(write, &*result, target, "did not apply",
                      control::is_final(target.status)
                          ? std::string{}
                          : "; at " + ms_text(p) + " ms the result of the ECU's step at " + ms_text(write.at) + " ms should be there");
            return;
        }
        if (target.status != control::TargetStatus::Applied)
        {
            TimelineFailure failure = failed_write(write, &*result, target);
            const std::string which = applied_text(failure, target.peer); // before `failure` is moved from
            fail(write.step, write.at, "scenario-write",
                 "the write for " + ms_text(write.at) + " ms was not applied by participant '" + target.peer + "': " +
                     target_status_text(target.status) + (target.reason.empty() ? "" : " (" + target.reason + ")") +
                     (control::is_final(target.status)
                          ? ""
                          : "; at " + ms_text(p) + " ms the result of the ECU's step at or after " + ms_text(write.at) +
                                " ms should be there (does the ECU step as --describe says?)") +
                     which,
                 std::move(failure));
            return;
        }
        if (target.applied_at)
        {
            applied = std::max(applied, *target.applied_at);
        }
    }
    // (A dither's write of no step decides nothing: it is no step's.)
    if (!write.grid && --open_writes_[write.step] == 0)
    {
        const ScenarioStepRun& step = run_.steps[write.step];
        pass(write.step, step.kind == StepKind::Ramp || step.kind == StepKind::Wave ? *step.end : applied, *step.end);
    }
}

// --- samples ---

void ScenarioEngine::evaluate(Watch& watch, SimTime t)
{
    const ScenarioStep& step = scenario_.steps[watch.step];
    const std::string unmetNow = unmet(step, watch.beside ? std::optional<SimTime>(watch.from) : std::nullopt);
    const bool ok = unmetNow.empty();
    if (!watch.until)
    {
        if (ok)
        {
            pass(watch.step, t, t);
        }
        else
        {
            fail(watch.step, t, "scenario-failed", "not true at " + ms_text(t) + " ms: " + unmetNow, unmet_values(step));
        }
        watch.step = run_.steps.size(); // decided
    }
    else if (watch.hold)
    {
        if (!ok)
        {
            fail(watch.step, t, "scenario-failed",
                 (watch.beside ? "not true during the " + std::string(step_kind_name(step.kind)) + " (" + during(watch.step, watch) + ")"
                               : "not true for " + std::to_string(*step.hold_for_ms) + " ms (from " + ms_text(watch.from) + " ms)") +
                     ": at " + ms_text(t) + " ms " + unmetNow,
                 unmet_values(step, watch.beside ? std::optional<SimTime>(watch.from) : std::nullopt));
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
    if (watch.beside)
    {
        // A variable whose ECU sent no sample in the time was not looked at: that is no pass.
        const auto seen_in_window = [&](std::size_t v) {
            return variables_[v].value.has_value() && variables_[v].value_time && *variables_[v].value_time >= watch.from;
        };
        std::vector<std::size_t> unseenVars;
        const auto note_unseen = [&](std::size_t v) {
            if (std::find(unseenVars.begin(), unseenVars.end(), v) == unseenVars.end())
            {
                unseenVars.push_back(v);
            }
        };
        TimelineFailure notSeen;
        for (std::size_t c = 0; c < step.conditions.size(); ++c)
        {
            const ScenarioCondition& condition = step.conditions[c];
            const bool lhsSeen = seen_in_window(condition.variable);
            if (!lhsSeen)
            {
                note_unseen(condition.variable);
            }
            std::vector<std::size_t> rhsVars;
            bool anyRhsUnseen = false;
            for (const ScenarioComparison& comp : condition.all)
            {
                if (comp.rhs_variable && std::find(rhsVars.begin(), rhsVars.end(), *comp.rhs_variable) == rhsVars.end())
                {
                    rhsVars.push_back(*comp.rhs_variable);
                    if (!seen_in_window(*comp.rhs_variable))
                    {
                        note_unseen(*comp.rhs_variable);
                        anyRhsUnseen = true;
                    }
                }
            }
            if (!lhsSeen || anyRhsUnseen)
            {
                const auto finite_val = [](const std::optional<control::Value>& val) -> std::optional<control::Value> {
                    if (!val)
                    {
                        return std::nullopt;
                    }
                    if (const auto* d = std::get_if<double>(&*val); d != nullptr && !std::isfinite(*d))
                    {
                        return std::nullopt;
                    }
                    return val;
                };
                const Variable& lhs = variables_[condition.variable];
                const std::optional<control::Value> lhsVal = lhsSeen ? finite_val(lhs.value) : std::nullopt;
                TimelineFailedCondition failed{
                    c,
                    lhsVal,
                    lhsVal ? lhs.value_time : std::nullopt,
                };
                for (const std::size_t rhsIdx : rhsVars)
                {
                    const bool rhsSeen = seen_in_window(rhsIdx);
                    if (lhsSeen && rhsSeen)
                    {
                        continue;
                    }
                    const Variable& rhs = variables_[rhsIdx];
                    const std::optional<control::Value> rhsVal = rhsSeen ? finite_val(rhs.value) : std::nullopt;
                    failed.compared.push_back(TimelineComparedValue{
                        rhsIdx,
                        rhsVal,
                        rhsVal ? rhs.value_time : std::nullopt,
                    });
                }
                notSeen.conditions.push_back(std::move(failed));
            }
        }
        if (!unseenVars.empty())
        {
            std::string unseen;
            for (const std::size_t v : unseenVars)
            {
                unseen += (unseen.empty() ? "" : ", ") + scenario_.variables[v].text;
            }
            fail(index, t, "scenario-failed",
                 "not checked during the " + std::string(step_kind_name(step.kind)) + " (" + during(index, watch) + "): no sample of " +
                     unseen + " in that time (its ECU sends one after each of its steps; with samples: as_sent, every sample_period); make the " + step_kind_name(step.kind) +
                     " longer, or check in an expect step with within_ms after it",
                 std::move(notSeen));
            return;
        }
        // The expect beside it held to the end: the step passes once its writes have, too.
        if (--open_writes_[index] == 0)
        {
            const SimTime end = *run_.steps[index].end;
            pass(index, end, end);
        }
    }
    else if (watch.hold)
    {
        pass(index, t, t);
    }
    else if (step.kind == StepKind::WaitUntil)
    {
        fail(index, t, "scenario-timeout",
             "timed out at " + ms_text(t) + " ms (" + std::to_string(*step.within_ms) + " ms after " + ms_text(watch.from) +
                 " ms): " + unmet(step),
             unmet_values(step));
    }
    else
    {
        fail(index, t, "scenario-failed",
             "not true at any time within " + std::to_string(*step.within_ms) + " ms (from " + ms_text(watch.from) + " ms to " +
                 ms_text(t) + " ms): at the end " + unmet(step),
             unmet_values(step));
    }
}

void ScenarioEngine::observe(SimTime p, bool final_boundary)
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
            if (at && (*at < p || (final_boundary && w.opened && *at == p)) && (!boundary || *at < *boundary))
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
                last_seen_ = t;
                for (std::size_t v = 0; v < variables_.size(); ++v)
                {
                    Variable& variable = variables_[v];
                    if (scenario_.variables[v].participant == taken.participant && variable.index &&
                        *variable.index < taken.sample->values.size())
                    {
                        variable.value = taken.sample->values[*variable.index];
                        variable.value_time = t;
                        if (record_ != nullptr)
                        {
                            record_->sampled(v, t, *variable.value);
                        }
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
                if (!watches_[i].opened && watches_[i].from == t && (!final_boundary || t < p))
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
    if (over_ && record_ != nullptr && !record_closed_)
    {
        // Taken out of samples_ above and not looked at: for the record, as finish() does.
        for (; next < batch.size(); ++next)
        {
            left_.emplace_back(batch[next].participant, std::move(batch[next].sample));
        }
    }
}

} // namespace vehicle_sil::scenario
