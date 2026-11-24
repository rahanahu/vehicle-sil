// SPDX-License-Identifier: MIT
//
// ExecutionRuntime (execution_runtime.hpp): 2-axis launcher/run state machine, Prepare, Run, Stop,
// Time commands, Status, Results, and Shutdown.
#include "vehicle_sil/launch/execution_runtime.hpp"

#include "vehicle_sil/launch/error_result.hpp"
#include "vehicle_sil/launch/probe.hpp"
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

ConfigMetadata extract_metadata(const Checked& checked)
{
    ConfigMetadata metadata;
    metadata.variants = checked.variants;
    metadata.file_sha256 = checked.file_sha256;
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
    case LauncherState::Unprepared: return "unprepared";
    case LauncherState::ColdOnly: return "cold_only";
    case LauncherState::Preparing: return "preparing";
    case LauncherState::Ready: return "ready";
    case LauncherState::Running: return "running";
    case LauncherState::Ending: return "ending";
    case LauncherState::Contaminated: return "contaminated";
    }
    return "unprepared";
}

PreparedRun prepare_execution_run(const RunRequest& base, const RunSubmission& submission, const std::string& run_id,
                                  bool single_run)
{
    PreparedRun out;
    out.request = base;
    if (!submission.pre_usage.empty())
    {
        out.usage = submission.pre_usage;
        return out;
    }
    if (!submission.request_file.empty() && submission.intent_json.has_value())
    {
        out.usage.push_back(config::Finding{
            config::Severity::Error, "usage", "run: cannot give both \"request\" and \"intent\"; choose one", 0, "", ""});
        return out;
    }
    if (!submission.request_file.empty())
    {
        std::string text;
        if (const std::string problem = read_request_file_text(process::path_from_utf8(submission.request_file), text);
            !problem.empty())
        {
            config::Finding finding{
                config::Severity::Error, "usage", "run: request " + submission.request_file + ": " + problem, 0, "", ""};
            finding.file = submission.request_file;
            out.usage.push_back(std::move(finding));
            return out;
        }
        RequestFileRead read = read_request_file(text, submission.request_file);
        if (!read.file)
        {
            out.usage = std::move(read.findings);
            return out;
        }
        out.request = with_request_file(base, *read.file, false);
    }
    else if (submission.intent_json.has_value())
    {
        RequestFileRead read = read_request_intent(*submission.intent_json, "intent");
        if (!read.file)
        {
            out.usage = std::move(read.findings);
            return out;
        }
        out.request = with_request_file(base, *read.file, false);
    }
    else
    {
        out.request.intent = submission.intent;
        out.request.origin = submission.origin;
    }
    if (out.request.intent.connect.empty())
    {
        out.request.host.tool_directory.clear();
    }
    out.request.host.timeline = submission.timeline;
    if (!base.host.log_dir.empty())
    {
        out.request.host.log_dir = single_run ? base.host.log_dir : (base.host.log_dir / process::path_from_utf8(run_id));
    }
    if (out.request.host.control)
    {
        out.request.host.control->run_id = run_id;
        out.request.host.control->run_id_required = !single_run;
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
    if (!options_.source_name)
    {
        options_.source_name = default_source_name;
    }
    if (!end_.requested() && !sink_.closed())
    {
        const std::lock_guard lock(mutex_);
        sink_.on_status(snapshot_locked(nullptr));
    }
}

ExecutionRuntime::~ExecutionRuntime()
{
    end_.withdraw();
    std::unique_lock lock(mutex_);
    ending_ = true;
    changed_.notify_all();
    while (state_ == LauncherState::Preparing || in_flight_prepare_ > 0)
    {
        changed_.wait(lock);
    }
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
    else if (options_.trace_enabled && !options_.single_run)
    {
        out.run = blocked_allowed("trace-enabled", "serve does not run with VEHICLE_SIL_TRACE set");
    }
    else if (state_ == LauncherState::ColdOnly)
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

PrepareResult ExecutionRuntime::prepare(const PrepareSpec& spec)
{
    std::unique_lock lock(mutex_);
    if (state_ == LauncherState::Contaminated)
    {
        PrepareResult res{false,
                          serve_reason::kContaminated,
                          "the launcher is contaminated after an earlier run and cannot prepare",
                          snapshot_locked(nullptr),
                          {},
                          {}};
        sink_.on_prepare(res, spec.command_id);
        return res;
    }
    if (ending_ || end_.requested() || sink_.closed())
    {
        PrepareResult res{false,
                          serve_reason::kLauncherEnding,
                          "the launcher has been asked to end and takes no prepare any more",
                          snapshot_locked(current_ != nullptr ? current_->controls.get() : nullptr),
                          {},
                          {}};
        sink_.on_prepare(res, spec.command_id);
        return res;
    }
    if (busy_ || state_ == LauncherState::Preparing || state_ == LauncherState::Running || in_flight_prepare_ > 0)
    {
        PrepareResult res{false,
                          serve_reason::kBusy,
                          "a run or prepare is already under way",
                          snapshot_locked(current_ != nullptr ? current_->controls.get() : nullptr),
                          {},
                          {}};
        sink_.on_prepare(res, spec.command_id);
        return res;
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
                    self.state_ = LauncherState::ColdOnly;
                    self.eligibility_ = HotEligibility{false, "config-unreadable", "prepare failed"};
                    self.metadata_ = ConfigMetadata{};
                }
                ++self.status_rev_;
                self.sink_.on_status(self.snapshot_locked(nullptr));
                self.changed_.notify_all();
                return;
            }
            self.changed_.notify_all();
        }
    } guard{*this, lock};

    sink_.on_status(snapshot_locked(nullptr));
    lock.unlock();

    const bool effective_trace = options_.trace_enabled && !options_.single_run;
    const bool effective_refuse_fixed = options_.refuse_fixed_registry && !options_.single_run;
    CheckOptions check_opts;
    check_opts.file_only = true;
    check_opts.refuse_fixed_registry = false;
    check_opts.trace_enabled = effective_trace;
    Checked checked = check_file(req, check_opts);
    ConfigMetadata metadata = extract_metadata(checked);
    HotEligibility eligibility =
        effective_refuse_fixed
            ? checked.hot_eligibility
            : evaluate_hot_eligibility(checked, effective_trace, effective_refuse_fixed);

    lock.lock();
    if (ending_ || end_.requested() || sink_.closed())
    {
        state_ = LauncherState::Ending;
        ++status_rev_;
        --in_flight_prepare_;
        guard.active = false;
        changed_.notify_all();
        ExecutionStatus ending_status = snapshot_locked(nullptr);
        PrepareResult res{false,
                          serve_reason::kLauncherEnding,
                          "the launcher has been asked to end while preparing",
                          std::move(ending_status),
                          {},
                          {}};
        sink_.on_prepare(res, spec.command_id);
        return res;
    }

    config_ = target_config;
    options_.base.host.config = target_config;
    eligibility_ = std::move(eligibility);
    metadata_ = metadata;
    state_ = eligibility_.eligible ? LauncherState::Ready : LauncherState::ColdOnly;
    ++status_rev_;
    --in_flight_prepare_;
    guard.active = false;
    changed_.notify_all();
    ExecutionStatus done_status = snapshot_locked(nullptr);
    PrepareResult res{
        true, "", "", std::move(done_status), std::move(metadata), std::move(checked.findings)};
    sink_.on_prepare(res, spec.command_id);
    return res;
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
            ": wait for its result, then send run again (nothing is queued)";
        refuse_locked(run_cmd, submission.source, serve_reason::kBusy, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kBusy, msg, std::nullopt, 0};
    }
    if (config_.empty())
    {
        const std::string msg = "no configuration has been prepared for this launcher";
        refuse_locked(run_cmd, submission.source, serve_reason::kNotPrepared, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kNotPrepared, msg, std::nullopt, 0};
    }
    if (options_.trace_enabled && !options_.single_run)
    {
        const std::string msg = "serve does not run with VEHICLE_SIL_TRACE set";
        refuse_locked(run_cmd, submission.source, "trace-enabled", msg, std::nullopt);
        return CommandReply{false, false, "trace-enabled", msg, std::nullopt, 0};
    }
    if (state_ == LauncherState::ColdOnly)
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
        run->prepared = prepare_execution_run(options_.base, submission, run->id, options_.single_run);
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
    sink_.on_status(snapshot_locked(current_->controls.get()));
    changed_.notify_all();
    return CommandReply{true, false, "", "", std::move(started_id), 0};
}

CommandReply ExecutionRuntime::stop_run(const std::string& run_id, StopMode mode, std::string_view source,
                                        std::optional<std::int64_t> command_id)
{
    const runner::Command stop_cmd = runner::Command::make("stop", command_id);

    const std::lock_guard lock(mutex_);
    if (run_id.empty() && !options_.single_run)
    {
        const std::string msg =
            "stop needs \"run\", the ID of the run it is for, as the event run_start gave it (serve runs one "
            "run after the other, and a command must not reach the next one)";
        refuse_locked(stop_cmd, source, serve_reason::kRunRequired, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kRunRequired, msg, std::nullopt, 0};
    }
    if (current_ == nullptr || (!run_id.empty() && current_->id != run_id))
    {
        if (options_.single_run && current_ == nullptr)
        {
            end_.mark(runner::stop_source::kStopCommand);
            if (run_id.empty() && last_run_id_.has_value())
            {
                const std::string msg = "the simulation is not running";
                refuse_locked(stop_cmd, source, "not-running", msg, std::nullopt);
                return CommandReply{false, false, "not-running", msg, std::nullopt, 0};
            }
        }
        const std::string asked = run_id.empty() ? (last_run_id_.value_or("1")) : run_id;
        const std::string msg =
            "the run " + asked + " is not the current run" +
            (current_ == nullptr ? std::string(" (no run is under way)") : " (that is " + current_->id + ")") +
            ": it has ended, or is not of this launcher; the command is for no run";
        const std::optional<std::string> asked_opt =
            run_id.empty() ? std::nullopt : std::optional<std::string>(run_id);
        refuse_locked(stop_cmd, source, serve_reason::kRunChanged, msg, asked_opt);
        return CommandReply{false, false, serve_reason::kRunChanged, msg, asked_opt, 0};
    }
    ActiveRun& run = *current_;
    const runner::Command applied = run.controls->time().apply(stop_cmd, source, [&run, mode] {
        constexpr int stop_src = runner::stop_source::kStopCommand;
        switch (mode)
        {
        case StopMode::Graceful: static_cast<void>(run.stops.raise_to(stop_src, runner::stop_level::kGraceful)); break;
        case StopMode::Force: static_cast<void>(run.stops.raise_to(stop_src, runner::stop_level::kForce)); break;
        }
    });
    return CommandReply{applied.accepted, applied.pending, applied.reason, applied.message, run.id, run.stops.count()};
}

CommandReply ExecutionRuntime::send_time_command(const std::string& run_id, const runner::Command& command,
                                                 std::string_view source)
{
    if (command.is_refused())
    {
        const std::optional<std::string> asked =
            run_id.empty() ? std::nullopt : std::optional<std::string>(run_id);
        const std::lock_guard lock(mutex_);
        sink_.on_refused(command, source, asked);
        return CommandReply{false, false, command.reason, command.message, asked, 0};
    }
    if (command.name == "stop")
    {
        return stop_run(run_id, StopMode::Graceful, source, command.id);
    }
    const std::lock_guard lock(mutex_);
    if (run_id.empty() && !options_.single_run)
    {
        const std::string msg =
            command.name.value_or("command") +
            " needs \"run\", the ID of the run it is for, as the event run_start gave it (serve runs one "
            "run after the other, and a command must not reach the next one)";
        refuse_locked(command, source, serve_reason::kRunRequired, msg, std::nullopt);
        return CommandReply{false, false, serve_reason::kRunRequired, msg, std::nullopt, 0};
    }
    if (current_ == nullptr || (!run_id.empty() && current_->id != run_id))
    {
        if (options_.single_run && run_id.empty() && current_ == nullptr && last_run_id_.has_value())
        {
            const std::string msg = "the simulation is not running";
            refuse_locked(command, source, "not-running", msg, std::nullopt);
            return CommandReply{false, false, "not-running", msg, std::nullopt, 0};
        }
        const std::string asked = run_id.empty() ? (last_run_id_.value_or("1")) : run_id;
        const std::string msg =
            "the run " + asked + " is not the current run" +
            (current_ == nullptr ? std::string(" (no run is under way)") : " (that is " + current_->id + ")") +
            ": it has ended, or is not of this launcher; the command is for no run";
        const std::optional<std::string> asked_opt =
            run_id.empty() ? std::nullopt : std::optional<std::string>(run_id);
        refuse_locked(command, source, serve_reason::kRunChanged, msg, asked_opt);
        return CommandReply{false, false, serve_reason::kRunChanged, msg, asked_opt, 0};
    }
    const runner::Command applied = current_->controls->time().apply(command, source);
    return CommandReply{
        applied.accepted, applied.pending, applied.reason, applied.message, current_->id, current_->stops.count()};
}

ExecutionStatus ExecutionRuntime::status() const
{
    const std::lock_guard lock(mutex_);
    return snapshot_locked(current_ != nullptr ? current_->controls.get() : nullptr);
}

ExecutionStatus ExecutionRuntime::report_status(std::optional<std::int64_t> command_id) const
{
    const std::lock_guard lock(mutex_);
    ExecutionStatus snap = snapshot_locked(current_ != nullptr ? current_->controls.get() : nullptr);
    sink_.on_status(snap, command_id);
    return snap;
}

void ExecutionRuntime::request_shutdown(int stop_source)
{
    const std::lock_guard lock(mutex_);
    end_.mark(stop_source);
    ending_ = true;
    if (current_ != nullptr)
    {
        input_ended_in_run_ = input_ended_in_run_ || stop_source == runner::stop_source::kEndOfInput;
        RunEventSink* const window = current_->window.get();
        static_cast<void>(current_->stops.request_first(stop_source, [window, stop_source] {
            if (stop_source == runner::stop_source::kEndOfInput && window != nullptr)
            {
                window->command(runner::Command::make("stop"), "eof");
            }
        }));
        current_->controls->publish();
    }
    if (state_ == LauncherState::Unprepared || state_ == LauncherState::ColdOnly || state_ == LauncherState::Ready)
    {
        state_ = LauncherState::Ending;
        ++status_rev_;
        sink_.on_status(snapshot_locked(nullptr));
    }
    changed_.notify_all();
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
                    sink_.on_status(snapshot_locked(nullptr));
                }
                changed_.notify_all();
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
            return now > stops_before_close && (now >= runner::stop_level::kForce || end_.requested());
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
            lent.refuse_fixed_registry = options_.refuse_fixed_registry && !options_.single_run;
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

        // Step 1: release per-Run handles and ActiveRun before transitioning state and clearing busy_.
        const int run_stops = stops->count();
        const int first_stop = stops->first();
        report.ended_control_server.reset();
        if (report.run)
        {
            report.run->scenario.timeline_record.reset();
        }
        std::unique_ptr<RunEventSink> window = std::move(run->window);
        std::string finished_id = std::move(run->id);
        if (!contaminated)
        {
            run.reset();
        }

        // Step 2: under mutex_, re-check ending_, finalize state_ and busy_ = false, and take settled status.
        bool end_before_result = false;
        bool ends = false;
        ExecutionStatus settled_status;
        {
            const std::lock_guard lock(mutex_);
            end_before_result = ending_ || end_.requested() || sink_.closed();
            ends = contaminated || options_.single_run || end_before_result;
            last_run_id_ = std::move(finished_id);
            active_run_id_.reset();
            if (ends)
            {
                ending_ = true;
                state_ = contaminated ? LauncherState::Contaminated : LauncherState::Ending;
                if (contaminated)
                {
                    contaminated_run_ = std::move(run);
                }
            }
            else
            {
                state_ = eligibility_.eligible ? LauncherState::Ready : LauncherState::Unprepared;
                busy_ = false;
            }
            ++status_rev_;
            settled_status = snapshot_locked(nullptr);
            sink_.on_run_result(*window, report, ends, settled_status);
            changed_.notify_all();
        }

        // Step 3: enqueue and flush result embedding the settled ExecutionStatus.
        const int requests = end_.count() + (options_.single_run ? run_stops : 0);
        const bool wrote = sink_.finish_run(
            *window, report, ends, settled_status,
            [this, requests, run_stops] {
                const int now = end_.count() + (options_.single_run ? run_stops : 0);
                return now > std::min(requests, runner::stop_level::kGraceful);
            });
        const bool output_failed = !wrote || sink_.closed();
        if (output_failed && !end_.requested())
        {
            end_.mark(runner::stop_source::kOutputClosed);
        }
        if (!contaminated)
        {
            window.reset();
        }
        if (ends || output_failed)
        {
            {
                const std::lock_guard lock(mutex_);
                ending_ = true;
                if (state_ != LauncherState::Ending && state_ != LauncherState::Contaminated)
                {
                    state_ = LauncherState::Ending;
                    ++status_rev_;
                    sink_.on_status(snapshot_locked(nullptr));
                }
                changed_.notify_all();
            }
            if (options_.single_run)
            {
                const bool interrupted =
                    report.result() == RunResult::Interrupted ||
                    (first_stop != 0 && runner::stop_kind(first_stop) != runner::StopKind::Operator);
                const int code = contaminated ? kExitContaminated
                               : output_failed ? (interrupted ? kExitInterrupted : ecu_executable::kExitSetupFailed)
                               : interrupted   ? kExitInterrupted
                                               : report.exit_code();
                return {code, contaminated};
            }
            const bool quiet =
                end_before_result && end_.source() == runner::stop_source::kEndOfInput && !input_ended_in_run_;
            const int code = end_before_result
                                 ? (quiet ? (contaminated ? kExitContaminated : 0) : kExitInterrupted)
                                 : (contaminated ? kExitContaminated : (output_failed ? kExitInterrupted : 0));
            return {code, contaminated};
        }
    }
}

} // namespace vehicle_sil::launch
