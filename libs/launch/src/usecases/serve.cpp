// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch serve as a use case: the frame of the current run, the installing and the ending
// of a run, and the loop of the main thread. What each thread may do: the comment at the top of
// serve.hpp.
#include "vehicle_sil/launch/serve.hpp"

#include "vehicle_sil/launch/error_result.hpp"
#include "vehicle_sil/launch/probe.hpp"

#include "vehicle_sil/process/paths.hpp"
#include "vehicle_sil/runner/run_controls.hpp"

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

/// A refusal of `command` with `reason`.
runner::Command refused(const runner::Command& command, const char* reason, std::string message)
{
    runner::Command out = command;
    out.accepted = false;
    out.pending = false;
    out.reason = reason;
    out.message = std::move(message);
    return out;
}

} // namespace

std::string new_launcher_instance()
{
    std::random_device device;
    std::uint64_t value = (std::uint64_t{device()} << 32) ^ std::uint64_t{device()};
    // A random_device that is not random (some old runtimes) still differs from launch to launch.
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

PreparedRun prepare_serve_run(const RunRequest& base, const ServeCommand& command, const std::string& run_id)
{
    PreparedRun out;
    out.request = base;
    if (!command.request.empty())
    {
        std::string text;
        if (const std::string problem = read_request_file_text(process::path_from_utf8(command.request), text); !problem.empty())
        {
            config::Finding finding{config::Severity::Error, "usage", "run: request " + command.request + ": " + problem, 0, "", ""};
            finding.file = command.request;
            out.usage.push_back(std::move(finding));
            return out;
        }
        RequestFileRead read = read_request_file(text, command.request);
        if (!read.file)
        {
            out.usage = std::move(read.findings);
            return out;
        }
        out.request = with_request_file(base, *read.file, false);
    }
    if (out.request.intent.connect.empty())
    {
        out.request.host.tool_directory.clear(); // as run: the bridges are looked for only for --connect
    }
    out.request.host.timeline = command.timeline;
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

/// What a run of serve owns, from its install to its idle. The members go in the reverse order:
/// the controls (which use the stop requests and write to the window) first, the window last.
struct Serve::Run
{
    std::string id;
    PreparedRun prepared;
    std::unique_ptr<EventStream> window;
    runner::StopRequests stops;
    std::unique_ptr<runner::RunControls> controls;
};

Serve::Serve(EventStream& events, LauncherEnd& end, ServeOptions options) :
  events_(events), end_(end), options_(std::move(options))
{
    if (!options_.run)
    {
        options_.run = [](const RunRequest& request, const runner::RunObservers& observers, const RunLent& lent) {
            return run_file(request, observers, lent);
        };
    }
    if (!options_.prepare)
    {
        options_.prepare = prepare_serve_run;
    }
    if (!options_.source_name)
    {
        options_.source_name = default_source_name;
    }
    // Queue the initial "idle" before any command reader can install the first run (H1).
    if (!end_.requested() && !events_.closed())
    {
        events_.idle();
    }
}

Serve::~Serve()
{
    // A run that was installed and never taken (a test that ends early): not reachable by a signal
    // any more before it goes.
    end_.withdraw();
}

void Serve::refuse(const runner::Command& command, const char* reason, std::string message,
                   const std::optional<std::string>& asked_run)
{
    events_.refused(refused(command, reason, std::move(message)), "stdin", asked_run);
}

void Serve::command(const ServeCommand& line)
{
    const runner::Command& command = line.command;
    if (!command.accepted)
    {
        events_.refused(command, "stdin", line.run);
        return;
    }
    const std::lock_guard lock(mutex_);
    if (*command.name == "run")
    {
        install(line);
        return;
    }
    if (!line.run)
    {
        refuse(command, serve_reason::kRunRequired,
               *command.name + " needs \"run\", the ID of the run it is for, as the event run_start gave it (serve runs "
                               "one run after the other, and a command must not reach the next one)",
               std::nullopt);
        return;
    }
    if (current_ == nullptr || current_->id != *line.run)
    {
        refuse(command, serve_reason::kRunChanged,
               "the run " + *line.run + " is not the current run" +
                 (current_ == nullptr ? std::string(" (no run is under way)") : " (that is " + current_->id + ")") +
                 ": it has ended, or is not of this launcher; the command is for no run",
               line.run);
        return;
    }
    // Matched and applied under the frame's mutex: the run cannot end in between.
    Run& run = *current_;
    if (command.is_stop())
    {
        static_cast<void>(run.controls->time().apply(command, "stdin", [&run] { run.stops.request(runner::stop_source::kStopCommand); }));
    }
    else
    {
        static_cast<void>(run.controls->time().apply(command, "stdin"));
    }
}

void Serve::install(const ServeCommand& line)
{
    const runner::Command& command = line.command;
    if (ending_ || end_.requested() || events_.closed())
    {
        refuse(command, serve_reason::kLauncherEnding,
               "the launcher has been asked to end and takes no run any more: start a new launcher for the next run",
               std::nullopt);
        return;
    }
    if (busy_)
    {
        refuse(command, serve_reason::kBusy,
               "a run is under way" + (current_ != nullptr ? " (" + current_->id + ")" : std::string{}) +
                 ": wait for its result and the event idle, then send run again (nothing is queued)",
               std::nullopt);
        return;
    }
    auto run = std::make_unique<Run>();
    run->id = options_.instance + "-" + std::to_string(++seq_);
    try
    {
        run->prepared = options_.prepare(options_.base, line, run->id);
    }
    catch (const std::exception& e)
    {
        // A path that cannot be a file name, say: the run's usage error, as a request file's.
        run->prepared.request = options_.base;
        run->prepared.usage = {config::Finding{config::Severity::Error, "usage", std::string("run: ") + e.what(), 0, "", ""}};
    }
    const RunRequest& request = run->prepared.request;
    run->window = events_.open_run(run->id, command.id);
    EventStream* const window = run->window.get();
    runner::RunControls::Observers observers;
    observers.on_phase = [window](runner::Phase phase) { window->phase(phase); };
    observers.on_command = [window](const runner::Command& c, std::string_view source) { window->command(c, source); };
    observers.on_state = [window](const runner::RunState& state) { window->state(state); };
    run->controls = std::make_unique<runner::RunControls>(run->stops, request.intent.start_paused, std::move(observers),
                                                          request.intent.scenario.has_value());
    runner::StopRequests* const stops = &run->stops;
    run->controls->set_stop_name([this, stops] { return options_.source_name(stops->first()); });
    run->controls->set_control_server(request.host.control.has_value());
    current_ = run.get();
    busy_ = true;
    end_.publish(stops);
    handed_ = std::move(run);
    changed_.notify_all();
}

void Serve::supervisor_end(int source)
{
    const std::lock_guard lock(mutex_);
    end_.mark(source);
    if (current_ != nullptr)
    {
        input_ended_in_run_ = input_ended_in_run_ || source == runner::stop_source::kEndOfInput;
        EventStream* const window = current_->window.get();
        static_cast<void>(current_->stops.request_first(source, [window, source] {
            if (source == runner::stop_source::kEndOfInput)
            {
                runner::Command stop;
                stop.name = "stop";
                stop.accepted = true;
                window->command(stop, "eof");
            }
        }));
        current_->controls->publish();
    }
    changed_.notify_all();
}

void Serve::end_of_input() { supervisor_end(runner::stop_source::kEndOfInput); }

void Serve::output_closed() { supervisor_end(runner::stop_source::kOutputClosed); }

void Serve::flush()
{
    const auto deadline = std::chrono::steady_clock::now() + options_.flush_limit;
    const int requests = end_.count();
    while (!events_.wait_written(std::chrono::milliseconds(50)) && !events_.closed() &&
           std::chrono::steady_clock::now() < deadline && end_.count() <= requests)
    {
    }
}

ServeEnd Serve::run()
{
    while (true)
    {
        std::unique_ptr<Run> run;
        {
            std::unique_lock lock(mutex_);
            // A signal does not wake this: the end is looked at every few milliseconds.
            while (!handed_ && !end_.requested() && !events_.closed())
            {
                changed_.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (!handed_)
            {
                ending_ = true;
                if (events_.closed() && !end_.requested())
                {
                    end_.mark(runner::stop_source::kOutputClosed);
                }
                const bool quiet = end_.source() == runner::stop_source::kEndOfInput && !input_ended_in_run_;
                lock.unlock();
                flush();
                return {quiet ? 0 : kExitInterrupted, false};
            }
            run = std::move(handed_);
        }
        // Read again after the run was published (serve.hpp): a request that came meanwhile stops
        // the run before it starts (once: a handler that saw the run has asked it already).
        if (end_.requested())
        {
            static_cast<void>(run->stops.request_first(end_.source(), [] {}));
        }
        runner::RunObservers observers;
        runner::StopRequests* const stops = &run->stops;
        observers.interrupt_name = [this, stops] { return options_.source_name(stops->first()); };
        // A late single stop while the run closes, or a Force already issued before closing began,
        // does not leave its connection open: only a new stop request while closing that brings
        // the run's count to 2+ (Force) or a launcher-end request cuts close_within (serve.hpp).
        observers.close_cut = [this, stops](int stops_before_close) {
            const int now = stops->count();
            return now > stops_before_close && (now >= 2 || end_.requested());
        };
        RunReport report;
        bool contaminated = false;
        if (!run->prepared.usage.empty())
        {
            report = usage_run_report(options_.base.host.config, run->prepared.usage);
            run->controls->set_phase(runner::Phase::Ended); // every command that waits gets its answer
        }
        else
        {
            RunLent lent;
            lent.controls = run->controls.get();
            lent.events = run->window.get();
            lent.refuse_fixed_registry = true;
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
        const bool end_before_result = end_.requested() || events_.closed();
        const bool ends = contaminated || end_before_result;
        const int requests = end_.count();
        const bool wrote = run->window->result(format_result_event(report, ends),
                                               [this, requests] { return end_.count() > std::min(requests, 1); }, ends);
        const bool output_failed = !wrote || events_.closed();
        if (output_failed && !end_.requested())
        {
            end_.mark(runner::stop_source::kOutputClosed);
        }
        if (ends || output_failed)
        {
            const std::lock_guard lock(mutex_);
            ending_ = true;
            const bool quiet = end_before_result && end_.source() == runner::stop_source::kEndOfInput && !input_ended_in_run_;
            const int code = end_before_result
                                 ? (quiet ? (contaminated ? kExitContaminated : 0) : kExitInterrupted)
                                 : (contaminated ? kExitContaminated : kExitInterrupted);
            if (contaminated)
            {
                // Kept on `this` (main.cpp keeps Serve alive and exits at once via
                // end_process_at_once(); unit tests destroy Serve cleanly for sanitizers).
                report.ended_control_server.reset(); // the hub stays with the thread that closes
                contaminated_run_ = std::move(run);
            }
            return {code, contaminated};
        }
        report = RunReport{}; // what is left of its control server goes here
        run.reset();
        const std::lock_guard lock(mutex_);
        events_.idle();
        busy_ = false;
    }
}

} // namespace vehicle_sil::launch
