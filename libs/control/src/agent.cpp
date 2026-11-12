// SPDX-License-Identifier: MIT
#include "vehicle_sil/control/agent.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::control {
namespace {

constexpr double kTwoPow63 = 9223372036854775808.0;

std::string describe(const Value& value)
{
    if (std::holds_alternative<bool>(value))
    {
        return std::get<bool>(value) ? "true" : "false";
    }
    if (std::holds_alternative<std::int64_t>(value))
    {
        return std::to_string(std::get<std::int64_t>(value));
    }
    return std::to_string(std::get<double>(value));
}

double as_double(const Value& value)
{
    return std::holds_alternative<std::int64_t>(value) ? static_cast<double>(std::get<std::int64_t>(value))
                                                       : std::get<double>(value);
}

/// The smallest integer >= `bound`, saturated to int64.
std::int64_t integer_at_least(double bound)
{
    const double c = std::ceil(bound);
    if (c <= -kTwoPow63)
    {
        return std::numeric_limits<std::int64_t>::lowest();
    }
    return c >= kTwoPow63 ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(c);
}

/// The largest integer <= `bound`, saturated to int64.
std::int64_t integer_at_most(double bound)
{
    const double f = std::floor(bound);
    if (f >= kTwoPow63)
    {
        return std::numeric_limits<std::int64_t>::max();
    }
    return f <= -kTwoPow63 ? std::numeric_limits<std::int64_t>::lowest() : static_cast<std::int64_t>(f);
}

} // namespace

Agent::Agent(AgentOptions options) :
  options_(options),
  commands_(options.max_pending == 0 ? 1 : options.max_pending),
  results_(options.max_pending == 0 ? 1 : options.max_pending),
  events_(options.max_events == 0 ? 1 : options.max_events)
{
    check_options(options_);
    waiting_.reserve(options_.max_pending);
    due_.reserve(options_.max_pending);
}

void Agent::check_options(const AgentOptions& options)
{
    if (options.max_pending == 0 || options.max_events == 0)
    {
        throw std::invalid_argument("vehicle_sil::control::AgentOptions: max_pending and max_events must be > 0 (got "
                                    + std::to_string(options.max_pending) + " and " + std::to_string(options.max_events) + ")");
    }
    if (options.sample_period < SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::control::AgentOptions: sample_period must be >= 0 (0: every step)");
    }
}

void Agent::require_registering(const std::string& what) const
{
    if (sealed_.load(std::memory_order_acquire) || completed_.load(std::memory_order_acquire))
    {
        throw std::logic_error("vehicle_sil::control::Agent: " + what
                               + " after the schema was complete (by the start of the run, the first submit() or a "
                                 "transport such as AgentClient): declare every variable before that");
    }
}

void Agent::complete_schema()
{
    if (completed_.load(std::memory_order_acquire))
    {
        return;
    }
    const std::lock_guard lock(schema_mutex_);
    if (completed_.load(std::memory_order_relaxed))
    {
        return;
    }
    if (complete_)
    {
        complete_(); // may throw: the schema stays open
    }
    completed_.store(true, std::memory_order_release);
}

void Agent::add(VariableInfo info, Variable variable)
{
    require_registering("variable '" + info.name + "' declared");
    const auto bad = [&](const std::string& why) {
        return std::invalid_argument("vehicle_sil::control::Agent: variable '" + info.name + "' " + why);
    };
    if (info.name.empty())
    {
        throw std::invalid_argument("vehicle_sil::control::Agent: a variable needs a name (an empty name was given)");
    }
    if (index_.count(info.name) != 0)
    {
        throw bad("is declared twice: give each variable of the agent its own name");
    }
    if ((info.min && std::isnan(*info.min)) || (info.max && std::isnan(*info.max)))
    {
        throw bad("has a NaN limit");
    }
    if (info.min && info.max && *info.min > *info.max)
    {
        throw bad("has min > max");
    }
    if ((info.type == ValueType::Integer || info.type == ValueType::Enum)
        && ((info.min && *info.min > static_cast<double>(variable.int_max))
            || (info.max && *info.max < static_cast<double>(variable.int_min))))
    {
        throw bad("has limits outside its type");
    }
    if (info.type == ValueType::Enum)
    {
        if (info.items.empty())
        {
            throw bad("needs items");
        }
        std::set<std::string> names;
        std::set<std::int64_t> values;
        for (const auto& item : info.items)
        {
            if (item.value < variable.int_min || item.value > variable.int_max)
            {
                throw bad("has an item that does not fit its type: " + item.name);
            }
            if (!names.insert(item.name).second || !values.insert(item.value).second)
            {
                throw bad("has a duplicate item: " + item.name);
            }
        }
    }
    index_.emplace(info.name, infos_.size());
    infos_.push_back(std::move(info));
    variables_.push_back(std::move(variable));
}

void Agent::bind_driver(std::function<void()> wakeup, std::function<void()> complete)
{
    require_registering("bind_driver()");
    if (std::exchange(bound_, true))
    {
        throw std::logic_error("vehicle_sil::control::Agent: already bound to a driver of its steps (one driver per agent)");
    }
    complete_ = std::move(complete);
    const std::lock_guard lock(wakeup_mutex_);
    wakeup_ = std::move(wakeup);
}

void Agent::clear_wakeup()
{
    const std::lock_guard lock(wakeup_mutex_);
    wakeup_ = nullptr;
}

std::optional<std::string> Agent::check(const Write& write, std::pair<std::size_t, Value>& out, bool& parameter) const
{
    const auto it = index_.find(write.name);
    if (it == index_.end())
    {
        return "unknown variable '" + write.name + "'";
    }
    const auto& info = infos_[it->second];
    const auto& variable = variables_[it->second];
    const auto fail = [&](const std::string& why) { return std::optional<std::string>(write.name + ": " + why); };
    if (info.causality == Causality::Output)
    {
        return fail("an output is not writable");
    }
    parameter = parameter || info.causality == Causality::Parameter;

    Value value = write.value;
    switch (info.type)
    {
    case ValueType::Bool:
        if (!std::holds_alternative<bool>(value))
        {
            return fail("needs a bool");
        }
        break;
    case ValueType::Integer:
    case ValueType::Enum:
    {
        if (std::holds_alternative<bool>(value))
        {
            return fail("needs an integer");
        }
        if (std::holds_alternative<double>(value))
        {
            const double d = std::get<double>(value);
            // Exactly representable integers only (2^63 itself is out of range).
            if (!std::isfinite(d) || d != std::trunc(d) || d < -kTwoPow63 || d >= kTwoPow63)
            {
                return fail("needs an integer, got " + describe(value));
            }
            value = static_cast<std::int64_t>(d);
        }
        const auto v = std::get<std::int64_t>(value);
        if (v < variable.int_min || v > variable.int_max)
        {
            return fail(describe(value) + " does not fit its type");
        }
        // Compared as integers: a double comparison loses precision above 2^53.
        if ((info.min && v < integer_at_least(*info.min)) || (info.max && v > integer_at_most(*info.max)))
        {
            return fail(describe(value) + " is out of range");
        }
        if (info.type == ValueType::Enum
            && std::none_of(info.items.begin(), info.items.end(), [v](const EnumItem& item) { return item.value == v; }))
        {
            return fail(describe(value) + " is not one of its values");
        }
        break;
    }
    case ValueType::Real:
    {
        if (std::holds_alternative<bool>(value))
        {
            return fail("needs a number");
        }
        const double d = as_double(value);
        if (!std::isfinite(d) || std::fabs(d) > variable.real_max)
        {
            return fail(describe(value) + " is not a finite number of its type");
        }
        if ((info.min && d < *info.min) || (info.max && d > *info.max))
        {
            return fail(describe(value) + " is out of range");
        }
        value = d;
        break;
    }
    }
    out = {it->second, std::move(value)};
    return std::nullopt;
}

SubmitResult Agent::submit(Command command)
{
    const std::lock_guard transport(transport_mutex_);
    complete_schema(); // the schema the command is checked against is final from here
    sealed_.store(true, std::memory_order_release);
    stats_.submitted.fetch_add(1, std::memory_order_relaxed);
    const auto invalid = [&](std::string reason) {
        stats_.invalid.fetch_add(1, std::memory_order_relaxed);
        return SubmitResult{SubmitStatus::Invalid, std::move(reason)};
    };
    if (command.writes.empty())
    {
        return invalid("no writes");
    }
    Pending pending;
    pending.seq = command.seq;
    pending.at = command.at;
    pending.late = command.late;
    pending.writes.resize(command.writes.size());
    for (std::size_t i = 0; i < command.writes.size(); ++i)
    {
        if (auto error = check(command.writes[i], pending.writes[i], pending.writes_parameter))
        {
            return invalid(std::move(*error));
        }
    }
    if (pending.writes_parameter && stepped_.load(std::memory_order_acquire))
    {
        return invalid("a parameter is writable only before the first step");
    }
    {
        // finish() drains the queue under the same lock: a command is either queued before it
        // (and cancelled by it) or refused after it.
        const std::lock_guard lock(finish_mutex_);
        if (finished_)
        {
            return invalid("the simulation has ended");
        }
        if (in_flight_.fetch_add(1, std::memory_order_acq_rel) >= options_.max_pending || !commands_.try_push(pending))
        {
            in_flight_.fetch_sub(1, std::memory_order_acq_rel);
            stats_.busy.fetch_add(1, std::memory_order_relaxed);
            return SubmitResult{SubmitStatus::Busy, "too many commands in flight"};
        }
    }
    const std::lock_guard lock(wakeup_mutex_);
    if (wakeup_)
    {
        wakeup_();
    }
    return SubmitResult{};
}

std::size_t Agent::drain_results(std::vector<CommandResult>& out)
{
    const std::lock_guard transport(transport_mutex_);
    std::size_t n = 0;
    while (auto result = results_.try_pop())
    {
        out.push_back(std::move(*result));
        ++n;
    }
    in_flight_.fetch_sub(n, std::memory_order_acq_rel);
    return n;
}

std::size_t Agent::drain_events(std::vector<Event>& out)
{
    const std::lock_guard transport(transport_mutex_);
    std::size_t n = 0;
    while (auto event = events_.try_pop())
    {
        out.push_back(std::move(*event));
        ++n;
    }
    return n;
}

Snapshot Agent::latest() const
{
    const std::lock_guard transport(transport_mutex_);
    if (middle_.load(std::memory_order_acquire) & kFresh)
    {
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & ~kFresh;
    }
    return buffers_[front_];
}

AgentStats Agent::stats() const
{
    AgentStats s;
    s.submitted = stats_.submitted.load(std::memory_order_relaxed);
    s.busy = stats_.busy.load(std::memory_order_relaxed);
    s.invalid = stats_.invalid.load(std::memory_order_relaxed);
    s.applied = stats_.applied.load(std::memory_order_relaxed);
    s.late = stats_.late.load(std::memory_order_relaxed);
    s.rejected = stats_.rejected.load(std::memory_order_relaxed);
    s.cancelled = stats_.cancelled.load(std::memory_order_relaxed);
    s.events_dropped = stats_.events_dropped.load(std::memory_order_relaxed);
    return s;
}

void Agent::push_result(CommandResult result)
{
    // Cannot fail: every result belongs to one of at most max_pending commands in flight, and the
    // ring holds max_pending.
    static_cast<void>(results_.try_push(result));
}

void Agent::on_step_begin(SimTime now)
{
    if (!completed_.load(std::memory_order_acquire))
    {
        // A driver with a completion function completes the schema before its first step;
        // without one, the first step closes the registration.
        if (complete_)
        {
            throw std::logic_error("vehicle_sil::control::Agent::on_step_begin(): the schema is not complete; the driver "
                                   "that gave bind_driver() a completion function must call complete_schema() before "
                                   "its first step");
        }
        completed_.store(true, std::memory_order_release);
    }
    sealed_.store(true, std::memory_order_release);
    if (last_step_ && now <= *last_step_)
    {
        throw std::logic_error("Agent::on_step_begin(): time must increase");
    }
    due_.clear();
    while (auto popped = commands_.try_pop())
    {
        auto& command = *popped;
        if (!command.at || *command.at <= now)
        {
            // Late: a step at or after its time ran before the command arrived.
            const bool late = command.at && last_step_ && *last_step_ >= *command.at;
            if (late && command.late == LatePolicy::Reject)
            {
                stats_.late.fetch_add(1, std::memory_order_relaxed);
                push_result({command.seq, CommandStatus::Late, std::nullopt, true, "its time had passed"});
                continue;
            }
            const auto key = command.at.value_or(now); // a command without a time sorts as the current step
            due_.push_back({key, late, std::move(command)});
        }
        else
        {
            waiting_.push_back(std::move(command));
        }
    }
    // Waiting commands were not due at any earlier step, so they are never late.
    for (auto it = waiting_.begin(); it != waiting_.end();)
    {
        if (*it->at <= now)
        {
            const auto at = *it->at;
            due_.push_back({at, false, std::move(*it)});
            it = waiting_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Insertion sort: stable, allocates nothing, and the lists are short.
    for (std::size_t i = 1; i < due_.size(); ++i)
    {
        for (std::size_t j = i; j > 0; --j)
        {
            auto& a = due_[j - 1];
            auto& b = due_[j];
            const bool before = b.at != a.at ? b.at < a.at : b.command.seq < a.command.seq;
            if (!before)
            {
                break;
            }
            std::swap(a, b);
        }
    }
    for (auto& due : due_)
    {
        apply(due.command, now, due.late);
    }
    due_.clear();
    last_step_ = now;
    stepped_.store(true, std::memory_order_release);
}

void Agent::apply(Pending& command, SimTime now, bool late)
{
    if (command.writes_parameter && last_step_)
    {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        push_result({command.seq, CommandStatus::Rejected, std::nullopt, late, "a parameter is writable only before the first step"});
        return;
    }
    try
    {
        for (auto& [index, value] : command.writes)
        {
            variables_[index].set(value);
        }
    }
    catch (const std::exception& e)
    {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        push_result({command.seq, CommandStatus::Rejected, std::nullopt, late, std::string("a setter threw: ") + e.what()});
        return;
    }
    catch (...)
    {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        push_result({command.seq, CommandStatus::Rejected, std::nullopt, late, "a setter threw"});
        return;
    }
    stats_.applied.fetch_add(1, std::memory_order_relaxed);
    push_result({command.seq, CommandStatus::Applied, now, late, {}});
}

void Agent::on_step_end(SimTime now)
{
    const Snapshot* sampled = nullptr;
    if (!next_sample_ || now >= *next_sample_)
    {
        sampled = &sample(now);
        if (options_.sample_period > SimDuration::zero())
        {
            // The next multiple of the period after `now`, as Peer::every() counts.
            const auto periods = now.time_since_epoch() / options_.sample_period + 1;
            next_sample_ = SimTime{} + options_.sample_period * periods;
        }
    }
    if (step_sink_)
    {
        step_sink_(now, sampled);
    }
}

const Snapshot& Agent::sample(SimTime now)
{
    auto& buffer = buffers_[back_];
    buffer.values.resize(variables_.size()); // allocates once per buffer, in its first sample
    for (std::size_t i = 0; i < variables_.size(); ++i)
    {
        buffer.values[i] = variables_[i].get();
    }
    buffer.time = now;
    buffer.sequence = ++sample_sequence_;
    back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & ~kFresh;
    // Handed over, and from now on only read (by latest() too) until this thread gets it back as
    // its back buffer, which is at its next sample at the earliest.
    return buffer;
}

void Agent::set_step_sink(StepSink sink)
{
    if (!sink)
    {
        throw std::invalid_argument("vehicle_sil::control::Agent::set_step_sink(): the function is empty");
    }
    if (step_sink_)
    {
        throw std::logic_error("vehicle_sil::control::Agent::set_step_sink(): the agent has a step sink already (one "
                               "transport sends from its steps); attach one such transport per agent");
    }
    const std::lock_guard lock(finish_mutex_);
    if (stepped_.load(std::memory_order_acquire) || finished_)
    {
        throw std::logic_error("vehicle_sil::control::Agent::set_step_sink(): the steps have begun; set the step sink "
                               "(attach the transport) before the first step");
    }
    step_sink_ = std::move(sink);
}

void Agent::log(EventLevel level, std::string text)
{
    Event event{last_step_.value_or(SimTime{}), level, std::move(text)};
    if (!events_.try_push(event))
    {
        stats_.events_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void Agent::finish()
{
    const auto cancel = [&](Pending& command) {
        stats_.cancelled.fetch_add(1, std::memory_order_relaxed);
        push_result({command.seq, CommandStatus::Cancelled, std::nullopt, false, "the simulation ended"});
    };
    const std::lock_guard lock(finish_mutex_);
    finished_ = true;
    while (auto popped = commands_.try_pop())
    {
        cancel(*popped);
    }
    for (auto& command : waiting_)
    {
        cancel(command);
    }
    waiting_.clear();
}

} // namespace vehicle_sil::control
