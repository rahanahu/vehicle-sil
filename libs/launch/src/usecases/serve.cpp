// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch serve as a thin NDJSON adapter over ExecutionRuntime (execution_runtime.hpp).
#include "vehicle_sil/launch/serve.hpp"

#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/process/paths.hpp"

#include <utility>

namespace vehicle_sil::launch {

PreparedRun prepare_serve_run(const RunRequest& base, const ServeCommand& command, const std::string& run_id)
{
    PreparedRun out;
    out.request = base;
    if (!command.request.empty() && command.intent_json.has_value())
    {
        out.usage.push_back(config::Finding{
            config::Severity::Error, "usage", "run: cannot give both \"request\" and \"intent\"; choose one", 0, "", ""});
        return out;
    }
    if (!command.request.empty())
    {
        std::string text;
        if (const std::string problem = read_request_file_text(process::path_from_utf8(command.request), text);
            !problem.empty())
        {
            config::Finding finding{
                config::Severity::Error, "usage", "run: request " + command.request + ": " + problem, 0, "", ""};
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
    else if (command.intent_json.has_value())
    {
        RequestFileRead read = read_request_intent(*command.intent_json, "intent");
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

class Serve::Sink final : public ExecutionSink
{
public:
    explicit Sink(EventStream& events) : events_(events) {}

    void on_status(const ExecutionStatus&) override {}

    void on_idle() override { events_.idle(); }

    void on_refused(const runner::Command& command, std::string_view source,
                    const std::optional<std::string>& asked_run) override
    {
        events_.refused(command, source, asked_run);
    }

    std::unique_ptr<RunEventSink> open_run(const std::string& run_id,
                                           std::optional<std::int64_t> command_id) override
    {
        return events_.open_run(run_id, command_id);
    }

    bool finish_run(RunEventSink& window, const RunReport& report, bool launcher_ends,
                    const std::function<bool()>& give_up) override
    {
        auto& stream = static_cast<EventStream&>(window);
        return stream.result(format_result_event(report, launcher_ends), give_up, launcher_ends);
    }

    [[nodiscard]] bool closed() const override { return events_.closed(); }

    bool wait_flushed(std::chrono::milliseconds timeout) override { return events_.wait_written(timeout); }

    void emit_prepare(const PrepareResult& result, std::optional<std::int64_t> id) { events_.prepare(result, id); }
    void emit_status(const ExecutionStatus& status, std::optional<std::int64_t> id) { events_.status(status, id); }

private:
    EventStream& events_;
};

Serve::Serve(EventStream& events, LauncherEnd& end, ServeOptions options) :
  prepare_(options.prepare ? std::move(options.prepare) : prepare_serve_run), sink_(std::make_unique<Sink>(events))
{
    ExecutionRuntimeOptions rt_opts;
    rt_opts.base = std::move(options.base);
    rt_opts.instance = std::move(options.instance);
    rt_opts.run = std::move(options.run);
    rt_opts.source_name = std::move(options.source_name);
    rt_opts.flush_limit = options.flush_limit;
    rt_opts.trace_enabled = options.trace_enabled;
    rt_opts.refuse_fixed_registry = true;
    runtime_ = std::make_unique<ExecutionRuntime>(*sink_, end, std::move(rt_opts));
}

Serve::~Serve() = default;

void Serve::command(const ServeCommand& line)
{
    const runner::Command& command = line.command;
    if (!command.accepted)
    {
        static_cast<void>(runtime_->send_time_command(line.run.value_or(""), command, "stdin"));
        return;
    }
    if (*command.name == "run")
    {
        RunSubmission submission;
        submission.timeline = line.timeline;
        submission.command_id = command.id;
        submission.source = "stdin";
        submission.prepare_run = [this, line](const RunRequest& base, const std::string& run_id) {
            return prepare_(base, line, run_id);
        };
        static_cast<void>(runtime_->start_run(std::move(submission)));
        return;
    }
    if (*command.name == "prepare")
    {
        PrepareSpec spec;
        spec.variant = line.variant;
        spec.disable = line.disable;
        spec.enable = line.enable;
        spec.manual = line.manual;
        sink_->emit_prepare(runtime_->prepare(spec), command.id);
        return;
    }
    if (*command.name == "status")
    {
        sink_->emit_status(runtime_->status(), command.id);
        return;
    }
    if (command.is_stop())
    {
        const StopMode mode = !line.force.has_value() ? StopMode::Escalate
                              : (*line.force ? StopMode::Force : StopMode::Graceful);
        static_cast<void>(runtime_->stop_run(line.run.value_or(""), mode, "stdin", command.id));
        return;
    }
    static_cast<void>(runtime_->send_time_command(line.run.value_or(""), command, "stdin"));
}

void Serve::end_of_input() { runtime_->request_shutdown(runner::stop_source::kEndOfInput); }

void Serve::output_closed() { runtime_->request_shutdown(runner::stop_source::kOutputClosed); }

ServeEnd Serve::run() { return runtime_->run_loop(); }

} // namespace vehicle_sil::launch
