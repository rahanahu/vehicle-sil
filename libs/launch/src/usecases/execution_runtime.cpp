// SPDX-License-Identifier: MIT
//
// ExecutionRuntime (execution_runtime.hpp): 2-axis launcher/run state machine, Prepare, Run, Stop,
// Time commands, Status, Results, and Shutdown.
#include "vehicle_sil/launch/execution_runtime.hpp"

#include "vehicle_sil/launch/error_result.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <random>
#include <thread>
#include <utility>

namespace vehicle_sil::launch {

namespace {

std::string default_source_name(int source)
{
    std::string name = runner::stop_source_name(source);
    return name.empty() ? std::string("interrupt") : name;
}

runner::Command refused_command(const runner::Command& command, const char* reason, std::string message)
{
    runner::Command out = command;
    out.accepted = false;
    out.pending = false;
    out.reason = reason;
    out.message = std::move(message);
    return out;
}

std::string first_error_message(const std::vector<config::Finding>& findings, std::string_view fallback)
{
    for (const config::Finding& f : findings)
    {
        if (f.severity == config::Severity::Error && !f.message.empty())
        {
            return f.message;
        }
    }
    return std::string(fallback);
}

HotEligibility evaluate_eligibility(const Checked& checked, const ExecutionRuntimeOptions& options)
{
    HotEligibility out;
    if (checked.usage)
    {
        out.reason = "usage";
        out.message = first_error_message(checked.findings, "invalid prepare arguments");
        return out;
    }
    if (!checked.config)
    {
        out.reason = "config-unreadable";
        out.message = first_error_message(checked.findings, "configuration could not be read");
        return out;
    }
    if (options.refuse_fixed_registry && checked.config->registry_uri.has_value())
    {
        out.reason = "fixed-registry";
        out.message = "configuration uses a fixed registry_uri (" + *checked.config->registry_uri +
                      "): use run instead of serve";
        return out;
    }
    if (options.refuse_fixed_registry && config::waits_for_manual(*checked.config))
    {
        out.reason = "manual-participants";
        out.message = "configuration waits for manual participants: use run instead of serve";
        return out;
    }
    if (options.trace_enabled)
    {
        out.reason = "trace-enabled";
        out.message = "serve does not run with VEHICLE_SIL_TRACE set";
        return out;
    }
    out.eligible = true;
    return out;
}

ConfigMetadata extract_metadata(const Checked& checked)
{
    ConfigMetadata metadata;
    metadata.variants = checked.variants;
    if (checked.config)
    {
        const config::SimulationConfig& config = *checked.config;
        metadata.participant_names.reserve(config.participants.size());
        for (const config::ParticipantConfig& p : config.participants)
        {
            metadata.participant_names.push_back(p.name);
        }
        metadata.network_names.reserve(config.networks.size());
        for (const config::NetworkConfig& n : config.networks)
        {
            metadata.network_names.push_back(n.name);
        }
    }
    return metadata;
}

runner::Allowed blocked_allowed(const char* reason, std::string message)
{
    runner::Allowed out;
    out.ok = false;
    out.reason = reason;
    out.message = std::move(message);
    return out;
}

} // namespace

std::string new_launcher_instance()
{
    std::random_device device;
    std::uint64_t value = (std::uint64_t{device()} << 32) ^ std::uint64_t{device()};
    value ^= static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

void LauncherEnd::withdraw() noexcept
{
    run_.store(nullptr);
    while (inside_.load() != 0)
    {
        std::this_thread::yield();
    }
}

const char* launcher_state_name(LauncherState state) noexcept
{
    switch (state)
    {
    case LauncherState::Idle: return "idle";
    case LauncherState::Preparing: return "preparing";
    case LauncherState::Ready: return "ready";
    case LauncherState::Running: return "running";
    case LauncherState::Ending: return "ending";
    case LauncherState::Contaminated: return "contaminated";
    }
    return "idle";
}

PreparedRun prepare_execution_run(const RunRequest& base, const RunSubmission& submission, const std::string& run_id)
{
    PreparedRun out;
    if (!submission.pre_usage.empty())
    {
        out.request = base;
        out.usage = submission.pre_usage;
        return out;
    }
    out.request = base;
    out.request.intent = submission.intent;
    out.request.origin = submission.origin;
    if (out.request.intent.connect.empty())
    {
        out.request.host.tool_directory.clear();
    }
    out.request.host.timeline = submission.timeline;
    if (!base.host.log_dir.empty())
    {
        out.request.host.log_dir = base.host.log_dir / process::path_from_utf8(run_id);
    }
    if (out.request.host.control)
    {
        out.request.host.control->run_id = run_id;
        out.request.host.control->run_id_required = true;
    }
    out.usage = validate_request(out.request, RequestCommand::Run);
    return out;
}

struct ExecutionRuntime::ActiveRun
{
    std::string id;
    PreparedRun prepared;
    std::unique_ptr<RunEventSink> window;
    runner::StopRequests stops;
    std::unique_ptr<runner::RunControls> controls;
};

ExecutionRuntime::ExecutionRuntime(ExecutionSink& sink, LauncherEnd& end, ExecutionRuntimeOptions options) :
  sink_(sink), end_(end), options_(std::move(options)), config_(options_.base.host.config)
{
    if (options_.instance.empty())
    {
        options_.instance = new_launcher_instance();
    }
    if (!options_.run)
    {
        options_.run = [](const RunRequest& request, const runner::RunObservers& observers, const RunLent& lent) {
            return run_file(request, observers, lent);
        };
    }
    if (!options_.prepare)
    {
        options_.prepare = prepare_execution_run;
    }
    if (!options_.source_name)
    {
        options_.source_name = default_source_name;
    }
    if (!end_.requested() && !sink_.closed())
    {
        sink_.on_idle();
    }
}

ExecutionRuntime::~ExecutionRuntime()
{
    end_.withdraw();
    std::unique_lock lock(mutex_);
    ending_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] {
        return state_ != LauncherState::Preparing && in_flight_prepare_ == 0 && !status_emitting_ &&
               pending_statuses_.empty();
    });
}

LauncherAllowed ExecutionRuntime::allowed_locked() const
{
    LauncherAllowed out;
    if (state_ == LauncherState::Contaminated)
    {
        const std::string msg =
            "the launcher is contaminated after an earlier run and cannot continue: start a new launcher";
        out.prepare = blocked_allowed(serve_reason::kContaminated, msg);
        out.run = blocked_allowed(serve_reason::kContaminated, msg);
        return out;
    }
    if (ending_ || end_.requested() || sink_.closed())
    {
        const std::string msg =
            "the launcher has been asked to end and takes no run any more: start a new launcher for the next run";
        out.prepare = blocked_allowed(serve_reason::kLauncherEnding, msg);
        out.run = blocked_allowed(serve_reason::kLauncherEnding, msg);
        return out;
    }
    if (busy_ || state_ == LauncherState::Preparing || state_ == LauncherState::Running || in_flight_prepare_ > 0)
    {
        const std::string msg =
            "a run or prepare is under way" +
            (current_ != nullptr ? " (" + current_->id + ")" : std::string{}) +
            ": wait for it to finish (nothing is queued)";
        out.prepare = blocked_allowed(serve_reason::kBusy, msg);
        out.run = blocked_allowed(serve_reason::kBusy, msg);
        return out;
    }
    if (config_.empty())
    {
        out.run = blocked_allowed(serve_reason::kNotPrepared, "no configuration has been prepared for this launcher");
    }
    else if (options_.trace_enabled)
    {
        out.run = blocked_allowed("trace-enabled", "serve does not run with VEHICLE_SIL_TRACE set");
    }
    else if (has_prepared_ && !eligibility_.eligible)
    {
        const char* const reason =
            !eligibility_.reason.empty() ? eligibility_.reason.c_str() : serve_reason::kNotPrepared;
        const std::string msg =
            !eligibility_.message.empty() ? eligibility_.message : "configuration is not eligible for serve";
        out.run = blocked_allowed(reason, msg);
    }
    return out;
}

ExecutionStatus ExecutionRuntime::snapshot_locked(runner::RunControls* controls) const
{
    ExecutionStatus out;
    out.instance = options_.instance;
    out.status_rev = status_rev_;
    out.launcher_state = state_;
    out.config = config_;
    out.eligibility = eligibility_;
    out.launcher_allowed = allowed_locked();
    out.active_run_id = active_run_id_;
    out.last_run_id = last_run_id_;
    if (controls != nullptr)
    {
        out.run_state = controls->state();
    }
    return out;
}

void ExecutionRuntime::refuse_locked(const runner::Command& command, std::string_view source, const char* reason,
                                     std::string message, const std::optional<std::string>& asked_run)
{
    sink_.on_refused(refused_command(command, reason, std::move(message)), source, asked_run);
}

void ExecutionRuntime::drain_statuses_locked(std::unique_lock<std::mutex>& lock)
{
    if (status_emitting_)
    {
        return;
    }
    status_emitting_ = true;
    struct EmitGuard
    {
        ExecutionRuntime& self;
        std::unique_lock<std::mutex>& lock;

        ~EmitGuard()
        {
            if (!lock.owns_lock())
            {
                lock.lock();
            }
            self.status_emitting_ = false;
            self.changed_.notify_all();
        }
    } guard{*this, lock};

    while (!pending_statuses_.empty())
    {
        ExecutionStatus next = std::move(pending_statuses_.front());
        pending_statuses_.pop_front();
        lock.unlock();
        sink_.on_status(next);
        lock.lock();
    }
}

PrepareResult ExecutionRuntime::prepare(const PrepareSpec& spec)
{
    std::unique_lock lock(mutex_);
    if (state_ == LauncherState::Contaminated)
    {
        return PrepareResult{false,
                             serve_reason::kContaminated,
                             "the launcher is contaminated after an earlier run and cannot prepare",
                             snapshot_locked(nullptr),
                             {},
                             {}};
    }
    if (ending_ || end_.requested() || sink_.closed())
    {
        return PrepareResult{false,
                             serve_reason::kLauncherEnding,
                             "the launcher has been asked to end and takes no prepare any more",
                             snapshot_locked(current_ != nullptr ? current_->controls.get() : nullptr),
                             {},
                             {}};
    }
    if (busy_ || state_ == LauncherState::Preparing || state_ == LauncherState::Running || in_flight_prepare_ > 0)
    {
        return PrepareResult{false,
                             serve_reason::kBusy,
                             "a run or prepare is already under way",
                             snapshot_locked(current_ != nullptr ? current_->controls.get() : nullptr),
                             {},
                             {}};
    }

    const std::string target_config = spec.config.empty() ? config_ : spec.config;
    RunRequest req = options_.base;
    req.host.config = target_config;
    req.host.config_from_stdin = false;
    req.host.descriptions.clear();
    req.host.write_descriptions.clear();
    req.host.compare_commands.clear();
    req.host.timeline.clear();
    req.intent = RunIntent{};
    req.intent.variant = spec.variant;
    req.intent.disable = spec.disable;
    req.intent.enable = spec.enable;
    req.intent.manual = spec.manual;
    req.origin = config::RequestOrigin{};

    state_ = LauncherState::Preparing;
    ++status_rev_;
    ++in_flight_prepare_;

    struct PrepareGuard
    {
        ExecutionRuntime& self;
        std::unique_lock<std::mutex>& lock;
        bool active = true;

        ~PrepareGuard()
        {
            if (!active)
            {
                return;
            }
            if (!lock.owns_lock())
            {
                lock.lock();
            }
            --self.in_flight_prepare_;
            if (self.state_ == LauncherState::Preparing)
            {
                if (self.ending_ || self.end_.requested() || self.sink_.closed())
                {
                    self.state_ = LauncherState::Ending;
                }
                else
                {
                    self.state_ = LauncherState::Idle;
                    self.has_prepared_ = true;
                    self.eligibility_ = HotEligibility{false, "config-unreadable", "prepare failed"};
                    self.metadata_ = ConfigMetadata{};
                }
                ++self.status_rev_;
                self.pending_statuses_.push_back(self.snapshot_locked(nullptr));
                self.changed_.notify_all();
                try
                {
                    self.drain_statuses_locked(lock);
                }
                catch (...)
                {
                }
                return;
            }
            self.changed_.notify_all();
        }
    } guard{*this, lock};

    pending_statuses_.push_back(snapshot_locked(nullptr));
    drain_statuses_locked(lock);
    lock.unlock();

    CheckOptions check_opts;
    check_opts.file_only = true;
    check_opts.refuse_fixed_registry = false;
    Checked checked = check_file(req, check_opts);
    ConfigMetadata metadata = extract_metadata(checked);
    HotEligibility eligibility = evaluate_eligibility(checked, options_);

    lock.lock();
    if (ending_ || end_.requested() || sink_.closed())
    {
        state_ = LauncherState::Ending;
        ++status_rev_;
        --in_flight_prepare_;
        guard.active = false;
        changed_.notify_all();
        ExecutionStatus ending_status = snapshot_locked(nullptr);
        pending_statuses_.push_back(ending_status);
        drain_statuses_locked(lock);
        if (state_ == LauncherState::Contaminated)
        {
            ending_status = snapshot_locked(nullptr);
        }
        return PrepareResult{false,
                             serve_reason::kLauncherEnding,
                             "the launcher has been asked to end while preparing",
                             std::move(ending_status),
                             {},
                             {}};
    }

    config_ = target_config;
    options_.base.host.config = target_config;
    has_prepared_ = true;
    eligibility_ = std::move(eligibility);
    metadata_ = metadata;
    state_ = eligibility_.eligible ? LauncherState::Ready : LauncherState::Idle;
    ++status_rev_;
    --in_flight_prepare_;
    guard.active = false;
    changed_.notify_all();
    ExecutionStatus done_status = snapshot_locked(nullptr);
    pending_statuses_.push_back(done_status);
    drain_statuses_locked(lock);
    if (ending_ || state_ == LauncherState::Ending || state_ == LauncherState::Contaminated)
    {
        done_status = snapshot_locked(nullptr);
    }
    return PrepareResult{
        true, "", "", std::move(done_status), std::move(metadata), std::move(checked.findings)};
}

CommandReply ExecutionRuntime::start_run(RunSubmission submission)
{
    runner::Command run_cmd;
    run_cmd.name = "run";
    run_cmd.id = submission.command_id;

    std::unique_lock lock(mutex_);
    if (state_ == LauncherState::Contaminated)
    {
        const std::string msg =
            "the launcher is contaminated after an earlier run and takes no run any more: start a new launcher";
        refuse_locked(run_cmd, submission.source, serve_reason::kContaminated, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kContaminated, msg, std::nullopt, 0};
    }
    if (ending_ || end_.requested() || sink_.closed())
    {
        const std::string msg =
            "the launcher has been asked to end and takes no run any more: start a new launcher for the next run";
        refuse_locked(run_cmd, submission.source, serve_reason::kLauncherEnding, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kLauncherEnding, msg, std::nullopt, 0};
    }
    if (busy_ || state_ == LauncherState::Preparing || state_ == LauncherState::Running || in_flight_prepare_ > 0)
    {
        const std::string msg =
            "a run is under way" +
            (current_ != nullptr ? " (" + current_->id + ")" : std::string{}) +
            ": wait for its result and the event idle, then send run again (nothing is queued)";
        refuse_locked(run_cmd, submission.source, serve_reason::kBusy, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kBusy, msg, std::nullopt, 0};
    }
    if (config_.empty())
    {
        const std::string msg = "no configuration has been prepared for this launcher";
        refuse_locked(run_cmd, submission.source, serve_reason::kNotPrepared, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kNotPrepared, msg, std::nullopt, 0};
    }
    if (options_.trace_enabled)
    {
        const std::string msg = "serve does not run with VEHICLE_SIL_TRACE set";
        refuse_locked(run_cmd, submission.source, "trace-enabled", msg, std::nullopt);
        return CommandReply{false, false, "trace-enabled", msg, std::nullopt, 0};
    }
    if (has_prepared_ && !eligibility_.eligible)
    {
        const char* const reason =
            !eligibility_.reason.empty() ? eligibility_.reason.c_str() : serve_reason::kNotPrepared;
        const std::string msg =
            !eligibility_.message.empty() ? eligibility_.message : "configuration is not eligible for serve";
        refuse_locked(run_cmd, submission.source, reason, msg, std::nullopt);
        return CommandReply{false, false, reason, msg, std::nullopt, 0};
    }

    auto run = std::make_unique<ActiveRun>();
    run->id = options_.instance + "-" + std::to_string(++seq_);
    try
    {
        run->prepared = submission.prepare_run ? submission.prepare_run(options_.base, run->id)
                                               : options_.prepare(options_.base, submission, run->id);
    }
    catch (const std::exception& e)
    {
        run->prepared.request = options_.base;
        run->prepared.usage = {
            config::Finding{config::Severity::Error, "usage", std::string("run: ") + e.what(), 0, "", ""}};
    }
    const RunRequest& request = run->prepared.request;
    run->window = sink_.open_run(run->id, submission.command_id);
    RunEventSink* const window = run->window.get();
    runner::RunControls::Observers observers;
    observers.on_phase = [window](runner::Phase phase) { window->phase(phase); };
    observers.on_command = [window](const runner::Command& c, std::string_view source) {
        window->command(c, source);
    };
    observers.on_state = [window](const runner::RunState& state) { window->state(state); };
    run->controls = std::make_unique<runner::RunControls>(run->stops, request.intent.start_paused,
                                                          std::move(observers), request.intent.scenario.has_value());
    runner::StopRequests* const stops = &run->stops;
    run->controls->set_stop_name([this, stops] { return options_.source_name(stops->first()); });
    run->controls->set_control_server(request.host.control.has_value());
    current_ = run.get();
    busy_ = true;
    state_ = LauncherState::Running;
    active_run_id_ = run->id;
    ++status_rev_;
    end_.publish(stops);
    handed_ = std::move(run);
    std::string started_id = *active_run_id_;
    pending_statuses_.push_back(snapshot_locked(current_->controls.get()));
    changed_.notify_all();
    drain_statuses_locked(lock);
    return CommandReply{true, false, "", "", std::move(started_id), 0};
}

CommandReply ExecutionRuntime::stop_run(const std::string& run_id, StopMode mode, std::string_view source,
                                        std::optional<std::int64_t> command_id)
{
    runner::Command stop_cmd;
    stop_cmd.name = "stop";
    stop_cmd.id = command_id;
    stop_cmd.accepted = true;

    const std::lock_guard lock(mutex_);
    if (run_id.empty())
    {
        const std::string msg =
            "stop needs \"run\", the ID of the run it is for, as the event run_start gave it (serve runs one "
            "run after the other, and a command must not reach the next one)";
        refuse_locked(stop_cmd, source, serve_reason::kRunRequired, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kRunRequired, msg, std::nullopt, 0};
    }
    if (current_ == nullptr || current_->id != run_id)
    {
        const std::string msg =
            "the run " + run_id + " is not the current run" +
            (current_ == nullptr ? std::string(" (no run is under way)") : " (that is " + current_->id + ")") +
            ": it has ended, or is not of this launcher; the command is for no run";
        refuse_locked(stop_cmd, source, serve_reason::kRunChanged, msg, run_id);
        return CommandReply{false, false, serve_reason::kRunChanged, msg, run_id, 0};
    }
    ActiveRun& run = *current_;
    const runner::Command applied = run.controls->time().apply(stop_cmd, source, [&run, mode] {
        constexpr int stop_src = runner::stop_source::kStopCommand;
        switch (mode)
        {
        case StopMode::Graceful: static_cast<void>(run.stops.raise_to(stop_src, 1)); break;
        case StopMode::Force: static_cast<void>(run.stops.raise_to(stop_src, 2)); break;
        case StopMode::Escalate:
        {
            const int cur = run.stops.count();
            static_cast<void>(run.stops.raise_to(stop_src, cur < 1 ? 1 : 2));
            break;
        }
        }
    });
    return CommandReply{applied.accepted, applied.pending, applied.reason, applied.message, run.id, run.stops.count()};
}

CommandReply ExecutionRuntime::send_time_command(const std::string& run_id, const runner::Command& command,
                                                 std::string_view source)
{
    if (!command.accepted && (!command.reason.empty() || !command.message.empty()))
    {
        const std::optional<std::string> asked =
            run_id.empty() ? std::nullopt : std::optional<std::string>(run_id);
        const std::lock_guard lock(mutex_);
        sink_.on_refused(command, source, asked);
        return CommandReply{false, false, command.reason, command.message, asked, 0};
    }
    runner::Command prepared = command;
    prepared.accepted = true;
    if (prepared.is_stop())
    {
        return stop_run(run_id, StopMode::Graceful, source, prepared.id);
    }
    const std::lock_guard lock(mutex_);
    if (run_id.empty())
    {
        const std::string msg =
            prepared.name.value_or("command") +
            " needs \"run\", the ID of the run it is for, as the event run_start gave it (serve runs one "
            "run after the other, and a command must not reach the next one)";
        refuse_locked(prepared, source, serve_reason::kRunRequired, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kRunRequired, msg, std::nullopt, 0};
    }
    if (current_ == nullptr || current_->id != run_id)
    {
        const std::string msg =
            "the run " + run_id + " is not the current run" +
            (current_ == nullptr ? std::string(" (no run is under way)") : " (that is " + current_->id + ")") +
            ": it has ended, or is not of this launcher; the command is for no run";
        refuse_locked(prepared, source, serve_reason::kRunChanged, msg, run_id);
        return CommandReply{false, false, serve_reason::kRunChanged, msg, run_id, 0};
    }
    const runner::Command applied = current_->controls->time().apply(prepared, source);
    return CommandReply{
        applied.accepted, applied.pending, applied.reason, applied.message, current_->id, current_->stops.count()};
}

ExecutionStatus ExecutionRuntime::status() const
{
    const std::lock_guard lock(mutex_);
    return snapshot_locked(current_ != nullptr ? current_->controls.get() : nullptr);
}

ResultQuery ExecutionRuntime::result(const std::optional<std::string>& run_id) const
{
    const std::lock_guard lock(mutex_);
    if (active_run_id_.has_value() && (!run_id.has_value() || *run_id == *active_run_id_))
    {
        return ResultQuery{false, serve_reason::kRunInProgress, *active_run_id_, false, nullptr};
    }
    if (!last_result_)
    {
        return ResultQuery{false, serve_reason::kRunNotFound, run_id.value_or(""), false, nullptr};
    }
    if (run_id.has_value() && last_run_id_ != *run_id)
    {
        return ResultQuery{false, serve_reason::kRunNotFound, *run_id, false, nullptr};
    }
    return ResultQuery{true, "", last_run_id_.value_or(""), last_launcher_ends_, last_result_};
}

void ExecutionRuntime::request_shutdown(int stop_source)
{
    std::unique_lock lock(mutex_);
    end_.mark(stop_source);
    ending_ = true;
    if (current_ != nullptr)
    {
        input_ended_in_run_ = input_ended_in_run_ || stop_source == runner::stop_source::kEndOfInput;
        RunEventSink* const window = current_->window.get();
        static_cast<void>(current_->stops.request_first(stop_source, [window, stop_source] {
            if (stop_source == runner::stop_source::kEndOfInput && window != nullptr)
            {
                runner::Command stop;
                stop.name = "stop";
                stop.accepted = true;
                window->command(stop, "eof");
            }
        }));
        current_->controls->publish();
    }
    if (state_ == LauncherState::Idle || state_ == LauncherState::Ready)
    {
        state_ = LauncherState::Ending;
        ++status_rev_;
        pending_statuses_.push_back(snapshot_locked(nullptr));
    }
    changed_.notify_all();
    drain_statuses_locked(lock);
}

void ExecutionRuntime::flush()
{
    const auto deadline = std::chrono::steady_clock::now() + options_.flush_limit;
    const int requests = end_.count();
    while (!sink_.wait_flushed(std::chrono::milliseconds(50)) && !sink_.closed() &&
           std::chrono::steady_clock::now() < deadline && end_.count() <= requests)
    {
    }
}

ExecutionEnd ExecutionRuntime::run_loop()
{
    while (true)
    {
        std::unique_ptr<ActiveRun> run;
        std::string run_config;
        {
            std::unique_lock lock(mutex_);
            // A signal does not wake this: the end is looked at every few milliseconds.
            while (!handed_ && !end_.requested() && !sink_.closed())
            {
                changed_.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (!handed_)
            {
                ending_ = true;
                if (sink_.closed() && !end_.requested())
                {
                    end_.mark(runner::stop_source::kOutputClosed);
                }
                while (state_ == LauncherState::Preparing || in_flight_prepare_ > 0)
                {
                    changed_.wait(lock);
                }
                if (state_ != LauncherState::Ending && state_ != LauncherState::Contaminated)
                {
                    state_ = LauncherState::Ending;
                    ++status_rev_;
                    pending_statuses_.push_back(snapshot_locked(nullptr));
                }
                changed_.notify_all();
                drain_statuses_locked(lock);
                while (state_ == LauncherState::Preparing || in_flight_prepare_ > 0 || status_emitting_ ||
                       !pending_statuses_.empty())
                {
                    changed_.wait(lock);
                }
                const bool quiet = end_.source() == runner::stop_source::kEndOfInput && !input_ended_in_run_;
                lock.unlock();
                flush();
                return {quiet ? 0 : kExitInterrupted, false};
            }
            run = std::move(handed_);
            run_config = config_;
        }
        // Read again after the run was published: a request that came meanwhile stops the run
        // before it starts (once: a handler that saw the run has asked it already).
        if (end_.requested())
        {
            static_cast<void>(run->stops.request_first(end_.source(), [] {}));
        }
        runner::RunObservers observers;
        runner::StopRequests* const stops = &run->stops;
        observers.interrupt_name = [this, stops] { return options_.source_name(stops->first()); };
        // A late single stop while the run closes, or a Force already issued before closing began,
        // does not leave its connection open: only a new stop request while closing that brings
        // the run's count to 2+ (Force) or a launcher-end request cuts close_within.
        observers.close_cut = [this, stops](int stops_before_close) {
            const int now = stops->count();
            return now > stops_before_close && (now >= 2 || end_.requested());
        };
        RunReport report;
        bool contaminated = false;
        if (!run->prepared.usage.empty())
        {
            const std::string cfg =
                run->prepared.request.host.config.empty() ? run_config : run->prepared.request.host.config;
            report = usage_run_report(cfg, run->prepared.usage);
            run->controls->set_phase(runner::Phase::Ended);
        }
        else
        {
            RunLent lent;
            lent.controls = run->controls.get();
            lent.events = run->window.get();
            lent.refuse_fixed_registry = options_.refuse_fixed_registry;
            try
            {
                report = options_.run(run->prepared.request, observers, lent);
            }
            catch (const std::exception& e)
            {
                report = internal_error_run_report(e.what());
                contaminated = true;
                if (run->controls->phase() != runner::Phase::Ended)
                {
                    try
                    {
                        run->controls->set_phase(runner::Phase::Ended);
                    }
                    catch (...)
                    {
                        // The launcher ends: what is reported is the exception above.
                    }
                }
            }
        }
        contaminated = contaminated || (report.run && report.run->close.not_closed_ms);
        {
            const std::lock_guard lock(mutex_);
            current_ = nullptr; // a command for it is refused from now on
        }
        end_.withdraw(); // no signal reaches its stop requests any more
        const bool end_before_result = end_.requested() || sink_.closed();
        const bool ends = contaminated || end_before_result;
        const int requests = end_.count();
        const bool wrote = sink_.finish_run(*run->window, report, ends,
                                            [this, requests] { return end_.count() > std::min(requests, 1); });
        const bool output_failed = !wrote || sink_.closed();
        if (output_failed && !end_.requested())
        {
            end_.mark(runner::stop_source::kOutputClosed);
        }

        // Release heavy/process-bound handles before storing bounded last_result_.
        report.ended_control_server.reset();
        if (report.run)
        {
            report.run->scenario.timeline_record.reset();
        }
        auto stored_report = std::make_shared<const RunReport>(std::move(report));
        std::string finished_id = run->id;

        if (ends || output_failed)
        {
            if (!contaminated)
            {
                run.reset();
            }
            std::unique_lock lock(mutex_);
            ending_ = true;
            const bool quiet =
                end_before_result && end_.source() == runner::stop_source::kEndOfInput && !input_ended_in_run_;
            const int code = end_before_result
                                 ? (quiet ? (contaminated ? kExitContaminated : 0) : kExitInterrupted)
                                 : (contaminated ? kExitContaminated : kExitInterrupted);
            state_ = contaminated ? LauncherState::Contaminated : LauncherState::Ending;
            ++status_rev_;
            last_result_ = std::move(stored_report);
            last_run_id_ = std::move(finished_id);
            last_launcher_ends_ = ends;
            active_run_id_.reset();
            if (contaminated)
            {
                contaminated_run_ = std::move(run);
            }
            pending_statuses_.push_back(snapshot_locked(nullptr));
            changed_.notify_all();
            drain_statuses_locked(lock);
            while (state_ == LauncherState::Preparing || in_flight_prepare_ > 0 || status_emitting_ ||
                   !pending_statuses_.empty())
            {
                changed_.wait(lock);
            }
            return {code, contaminated};
        }

        run.reset();
        {
            std::unique_lock lock(mutex_);
            last_result_ = std::move(stored_report);
            last_run_id_ = std::move(finished_id);
            last_launcher_ends_ = false;
            active_run_id_.reset();
            state_ = (has_prepared_ && eligibility_.eligible) ? LauncherState::Ready : LauncherState::Idle;
            ++status_rev_;
            sink_.on_idle();
            busy_ = false;
            pending_statuses_.push_back(snapshot_locked(nullptr));
            changed_.notify_all();
            drain_statuses_locked(lock);
        }
    }
}

} // namespace vehicle_sil::launch
