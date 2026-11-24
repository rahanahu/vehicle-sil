// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch serve as a thin NDJSON adapter over ExecutionRuntime (execution_runtime.hpp).
#include "vehicle_sil/launch/serve.hpp"

#include <utility>

namespace vehicle_sil::launch {

class Serve::Sink final : public ExecutionSink
{
public:
    explicit Sink(EventStream& events) : events_(events) {}

    void on_status(const ExecutionStatus& status,
                   std::optional<std::int64_t> command_id) noexcept override
    {
        events_.status(status, command_id);
    }

    void on_prepare(const PrepareResult& result,
                    std::optional<std::int64_t> command_id) noexcept override
    {
        events_.prepare(result, command_id);
    }

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

    void on_run_result(RunEventSink& window, const RunReport& report, bool launcher_ends,
                       const ExecutionStatus& status) noexcept override
    {
        window.enqueue_result(format_result_event(report, launcher_ends, &status), launcher_ends);
    }

    bool finish_run(RunEventSink& window, const RunReport& /*report*/, bool launcher_ends,
                    const ExecutionStatus& /*status*/, const std::function<bool()>& give_up) override
    {
        return window.wait_result(give_up, launcher_ends);
    }

    [[nodiscard]] bool closed() const override { return events_.closed(); }

    bool wait_flushed(std::chrono::milliseconds timeout) override { return events_.wait_written(timeout); }

private:
    EventStream& events_;
};

Serve::Serve(EventStream& events, LauncherEnd& end, ServeOptions options) :
  sink_(std::make_unique<Sink>(events))
{
    ExecutionRuntimeOptions rt_opts;
    rt_opts.base = std::move(options.base);
    rt_opts.instance = std::move(options.instance);
    rt_opts.run = std::move(options.run);
    rt_opts.source_name = std::move(options.source_name);
    rt_opts.flush_limit = options.flush_limit;
    rt_opts.trace_enabled = options.trace_enabled;
    rt_opts.single_run = options.single_run;
    rt_opts.refuse_fixed_registry = !options.single_run;
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
        submission.request_file = line.request;
        submission.intent_json = line.intent_json;
        submission.timeline = line.timeline;
        submission.command_id = command.id;
        submission.source = "stdin";
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
        spec.command_id = command.id;
        static_cast<void>(runtime_->prepare(spec));
        return;
    }
    if (*command.name == "status")
    {
        static_cast<void>(runtime_->report_status(command.id));
        return;
    }
    if (command.is_stop())
    {
        const StopMode mode = line.force.value_or(false) ? StopMode::Force : StopMode::Graceful;
        static_cast<void>(runtime_->stop_run(line.run.value_or(""), mode, "stdin", command.id));
        return;
    }
    static_cast<void>(runtime_->send_time_command(line.run.value_or(""), command, "stdin"));
}

void Serve::end_of_input() { runtime_->request_shutdown(runner::stop_source::kEndOfInput); }

void Serve::output_closed() { runtime_->request_shutdown(runner::stop_source::kOutputClosed); }

ServeEnd Serve::run() { return runtime_->run_loop(); }

} // namespace vehicle_sil::launch
