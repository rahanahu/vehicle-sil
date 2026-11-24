// SPDX-License-Identifier: MIT
//
// Deterministic unit tests for ExecutionRuntime (execution_runtime.hpp): 2-axis state machine,
// Prepare/Eligibility/Metadata, Prepare+Shutdown barrier, RunSubmission, StopMode
// (Graceful/Force) + close_cut, send_time_command via Entry::Api, synchronous on_status
// emission, and contamination handling. Uses injectable RunFunction + mutex/CV barriers
// (no SIL Kit, no subprocesses, no fixed sleeps).
#include "test_harness.hpp"

#include "vehicle_sil/launch/execution_runtime.hpp"
#include "vehicle_sil/launch/run_sink.hpp"
#include "vehicle_sil/scenario/timeline.hpp"
#include "usecases/sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::config;

namespace {

struct RecordedRefusal
{
    std::string command;
    std::string reason;
    std::string source;
    std::optional<std::string> asked_run;
};

struct RecordedCommandEvent
{
    std::string run_id;
    std::string command;
    std::string source;
    bool accepted = false;
    std::string reason;
};

class MockRunWindow final : public RunEventSink
{
public:
    MockRunWindow(std::string run_id, std::mutex& mu, std::vector<RecordedCommandEvent>& commands) :
      run_id_(std::move(run_id)), mu_(mu), commands_(commands)
    {
    }

    const std::string& run_id() const { return run_id_; }

    void phase(Phase) override {}
    void check(const Checked&) override {}

    void command(const Command& cmd, std::string_view source) override
    {
        const std::lock_guard lock(mu_);
        commands_.push_back(RecordedCommandEvent{
            run_id_, cmd.name.value_or(""), std::string(source), cmd.accepted, cmd.reason});
    }

    void state(const RunState&) override {}
    void registry(const std::string&) override {}
    void control_server(const std::string&, bool, bool) override {}
    void participant(const std::string&, ParticipantState) override {}
    void participant_not_started(const std::string&, const std::string&) override {}
    void participant_failed(const std::string&, std::string_view, const std::string&) override {}
    void participant_started(const std::string&, long, const std::vector<std::string>&, const std::string&) override {}
    void participant_ended(const std::string&, const vehicle_sil::process::ExitStatus&) override {}
    void participant_manual(const std::string&, const std::vector<std::string>&, const std::string&, const std::string&,
                            const std::vector<std::pair<std::string, std::string>>&) override
    {
    }
    void control_link(const std::string&, std::string_view, std::optional<std::size_t>, std::optional<int>) override {}
    void scenario_step(const vehicle_sil::scenario::ScenarioStepRun&) override {}
    void log(const std::string&, const std::string&) override {}
    void outside(const std::string&, std::uint64_t, std::uint64_t) override {}
    void finding(const std::string&, const Finding&) override {}
    void time(const TimeEvent&) override {}
    [[nodiscard]] std::size_t waiting_events() const override { return 0; }

private:
    std::string run_id_;
    std::mutex& mu_;
    std::vector<RecordedCommandEvent>& commands_;
};

struct FinishedRun
{
    std::string run_id;
    bool launcher_ends = false;
    RunReport report{};
    ExecutionStatus status{};
};

class MockExecutionSink final : public ExecutionSink
{
public:
    std::function<void(const ExecutionStatus&)> status_hook{};
    std::function<void()> finish_hook{};

    void on_status(const ExecutionStatus& status, std::optional<std::int64_t>) noexcept override
    {
        if (status_hook)
        {
            status_hook(status);
        }
        const std::lock_guard lock(mu_);
        statuses_.push_back(status);
        cv_.notify_all();
    }

    void on_prepare(const PrepareResult& result, std::optional<std::int64_t>) noexcept override
    {
        if (status_hook)
        {
            status_hook(result.status);
        }
        const std::lock_guard lock(mu_);
        if (statuses_.empty() || statuses_.back().status_rev != result.status.status_rev)
        {
            statuses_.push_back(result.status);
        }
        cv_.notify_all();
    }

    void on_refused(const Command& command, std::string_view source,
                    const std::optional<std::string>& asked_run) override
    {
        const std::lock_guard lock(mu_);
        refusals_.push_back(
            RecordedRefusal{command.name.value_or(""), command.reason, std::string(source), asked_run});
        cv_.notify_all();
    }

    std::unique_ptr<RunEventSink> open_run(const std::string& run_id, std::optional<std::int64_t>) override
    {
        const std::lock_guard lock(mu_);
        opened_runs_.push_back(run_id);
        return std::make_unique<MockRunWindow>(run_id, mu_, command_events_);
    }

    bool finish_run(RunEventSink& window, const RunReport& report, bool launcher_ends,
                    const ExecutionStatus& status, const std::function<bool()>&) override
    {
        const auto& w = static_cast<const MockRunWindow&>(window);
        if (status_hook)
        {
            status_hook(status);
        }
        {
            const std::lock_guard lock(mu_);
            if (statuses_.empty() || statuses_.back().status_rev != status.status_rev)
            {
                statuses_.push_back(status);
            }
            finished_runs_.push_back(FinishedRun{w.run_id(), launcher_ends, report, status});
        }
        cv_.notify_all();
        if (finish_hook)
        {
            finish_hook();
        }
        return !fail_finish_;
    }

    [[nodiscard]] bool closed() const override { return closed_.load(); }

    bool wait_flushed(std::chrono::milliseconds) override { return true; }

    void set_closed(bool v)
    {
        closed_.store(v);
        cv_.notify_all();
    }

    std::vector<ExecutionStatus> statuses() const
    {
        const std::lock_guard lock(mu_);
        return statuses_;
    }

    void wait_status_count(std::size_t min_count) const
    {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return statuses_.size() >= min_count; });
    }

    /// Waits until the status after the end of `run_id` has been delivered: the runtime takes that
    /// snapshot under its lock after it left Running (busy cleared), so start_run() and prepare()
    /// see the run as over once this returns true.
    [[nodiscard]] bool wait_last_run(const std::string& run_id) const
    {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, vehicle_sil::unit::scaled_timeout(std::chrono::seconds(10)), [&] {
            return std::any_of(statuses_.begin(), statuses_.end(),
                               [&](const ExecutionStatus& s) { return s.last_run_id == run_id; });
        });
    }

    std::optional<FinishedRun> finished_run(const std::string& run_id) const
    {
        const std::lock_guard lock(mu_);
        for (const FinishedRun& f : finished_runs_)
        {
            if (f.run_id == run_id)
            {
                return f;
            }
        }
        return std::nullopt;
    }

    std::vector<RecordedRefusal> refusals() const
    {
        const std::lock_guard lock(mu_);
        return refusals_;
    }

    std::vector<RecordedCommandEvent> command_events() const
    {
        const std::lock_guard lock(mu_);
        return command_events_;
    }

    std::vector<FinishedRun> finished_runs() const
    {
        const std::lock_guard lock(mu_);
        return finished_runs_;
    }

private:
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::vector<ExecutionStatus> statuses_;
    std::vector<RecordedRefusal> refusals_;
    std::vector<std::string> opened_runs_;
    std::vector<FinishedRun> finished_runs_;
    std::vector<RecordedCommandEvent> command_events_;
    std::atomic<bool> closed_{false};
    bool fail_finish_ = false;
};

RunReport passed_report(const std::string& file = "sim.yaml")
{
    RunReport report;
    report.check.file = file;
    report.check.config.emplace();
    report.run.emplace();
    report.run->end.simulation = SimulationEnd::Ended;
    return report;
}

std::filesystem::path scratch_dir(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "execution_runtime" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_text(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary);
    CHECK(out.good());
    out << text;
}

} // namespace

TEST_CASE(execution_runtime_prepare_eligibility_metadata_and_status_transitions)
{
    const std::filesystem::path dir = scratch_dir("prepare_meta");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    const std::string valid_text =
        "vehicle_sil_simulation: 2\n"
        "networks:\n"
        "  - name: CAN1\n    kind: can\n"
        "  - name: ETH0\n    kind: ethernet\n"
        "participants:\n"
        "  - name: Engine\n    command: [\"./engine\"]\n"
        "  - name: Dash\n    command: [\"./dash\"]\n"
        "variants:\n"
        "  fast: {}\n"
        "  slow: {}\n";
    write_text(valid_cfg, valid_text);
    const std::filesystem::path fixed_cfg = dir / "fixed.yaml";
    write_text(fixed_cfg,
               "vehicle_sil_simulation: 2\n"
               "registry_uri: silkit://127.0.0.1:8500\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");
    const std::filesystem::path manual_cfg = dir / "manual.yaml";
    write_text(manual_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n    manual: true\n");

    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "0011223344556677";

    std::mutex run_mu;
    std::condition_variable run_cv;
    bool in_run = false;
    bool allow_finish = false;
    opts.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        {
            const std::lock_guard lock(run_mu);
            in_run = true;
        }
        run_cv.notify_all();
        std::unique_lock lock(run_mu);
        run_cv.wait(lock, [&] { return allow_finish; });
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    ExecutionRuntime rt(sink, end, std::move(opts));
    CHECK_EQ(sink.statuses().size(), std::size_t{1});
    const ExecutionStatus init = rt.status();
    CHECK(init.launcher_state == LauncherState::Unprepared);
    CHECK(std::string(launcher_state_name(init.launcher_state)) == "unprepared");
    CHECK_EQ(init.status_rev, std::uint64_t{0});
    CHECK(init.instance == "0011223344556677");
    CHECK(!init.active_run_id.has_value());
    CHECK(!init.last_run_id.has_value());
    CHECK(!init.run_state.has_value());
    CHECK(init.launcher_allowed.prepare.ok);
    CHECK(!init.launcher_allowed.run.ok);
    CHECK(init.launcher_allowed.run.reason == serve_reason::kNotPrepared);

    // 1. Valid config -> Unprepared -> Preparing -> Ready with ConfigMetadata.
    const PrepareResult p_valid = rt.prepare(PrepareSpec{valid_cfg.string(), std::string("fast")});
    CHECK(p_valid.accepted);
    CHECK(p_valid.reason.empty());
    CHECK(p_valid.status.launcher_state == LauncherState::Ready);
    CHECK_EQ(p_valid.status.status_rev, std::uint64_t{2});
    CHECK(p_valid.status.eligibility.eligible);
    CHECK(p_valid.status.eligibility.reason.empty());
    CHECK(p_valid.status.launcher_allowed.prepare.ok);
    CHECK(p_valid.status.launcher_allowed.run.ok);
    CHECK((p_valid.metadata.participant_names == std::vector<std::string>{"Engine", "Dash"}));
    CHECK((p_valid.metadata.network_names == std::vector<std::string>{"CAN1", "ETH0"}));
    CHECK((p_valid.metadata.variants == std::vector<std::string>{"fast", "slow"}));
    CHECK(p_valid.metadata.file_sha256 == vehicle_sil::launch::detail::sha256_hex(valid_text));
    CHECK_EQ(p_valid.metadata.file_sha256.size(), std::size_t{64});

    // 2. Fixed registry -> resets Ready to ColdOnly with fixed-registry reason.
    const PrepareResult p_fixed = rt.prepare(PrepareSpec{fixed_cfg.string()});
    CHECK(p_fixed.accepted);
    CHECK(!p_fixed.status.eligibility.eligible);
    CHECK(p_fixed.status.eligibility.reason == "fixed-registry");
    CHECK(p_fixed.status.launcher_state == LauncherState::ColdOnly);
    CHECK(std::string(launcher_state_name(p_fixed.status.launcher_state)) == "cold_only");
    CHECK(!p_fixed.status.launcher_allowed.run.ok);
    CHECK(p_fixed.status.launcher_allowed.run.reason == "fixed-registry");

    // 3. Manual participants -> manual-participants, ColdOnly.
    const PrepareResult p_manual = rt.prepare(PrepareSpec{manual_cfg.string()});
    CHECK(p_manual.accepted);
    CHECK(!p_manual.status.eligibility.eligible);
    CHECK(p_manual.status.eligibility.reason == "manual-participants");
    CHECK(p_manual.status.launcher_state == LauncherState::ColdOnly);

    // 4. Unreadable config -> config-unreadable, ColdOnly.
    const PrepareResult p_missing = rt.prepare(PrepareSpec{(dir / "does_not_exist.yaml").string()});
    CHECK(p_missing.accepted);
    CHECK(!p_missing.status.eligibility.eligible);
    CHECK(p_missing.status.eligibility.reason == "config-unreadable");
    CHECK(p_missing.status.launcher_state == LauncherState::ColdOnly);

    // 5. Usage error (nonexistent variant) -> usage, ColdOnly, file_sha256 still computed from readable file.
    const PrepareResult p_usage = rt.prepare(PrepareSpec{valid_cfg.string(), std::string("missing_variant")});
    CHECK(p_usage.accepted);
    CHECK(!p_usage.status.eligibility.eligible);
    CHECK(p_usage.status.eligibility.reason == "usage");
    CHECK(p_usage.status.launcher_state == LauncherState::ColdOnly);
    CHECK(p_usage.metadata.file_sha256 == vehicle_sil::launch::detail::sha256_hex(valid_text));

    // 6. Re-prepare valid config -> Ready, then refuse prepare with busy during Running.
    const PrepareResult p_ready = rt.prepare(PrepareSpec{valid_cfg.string(), std::string("slow")});
    CHECK(p_ready.accepted);
    CHECK(p_ready.status.launcher_state == LauncherState::Ready);

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    const CommandReply started = rt.start_run(RunSubmission{});
    CHECK(started.accepted);
    {
        std::unique_lock lock(run_mu);
        run_cv.wait(lock, [&] { return in_run; });
    }

    const PrepareResult p_busy = rt.prepare(PrepareSpec{valid_cfg.string()});
    CHECK(!p_busy.accepted);
    CHECK(p_busy.reason == serve_reason::kBusy);
    CHECK(p_busy.status.launcher_state == LauncherState::Running);
    CHECK(p_busy.status.launcher_allowed.prepare.reason == serve_reason::kBusy);
    CHECK(p_busy.status.launcher_allowed.run.reason == serve_reason::kBusy);

    rt.request_shutdown(stop_source::kEndOfInput);
    {
        const std::lock_guard lock(run_mu);
        allow_finish = true;
    }
    run_cv.notify_all();
    const ExecutionEnd ended = loop_future.get();
    CHECK(ended.code == kExitInterrupted);

    // 7. Prepare during Ending is refused with kLauncherEnding.
    const PrepareResult p_ending = rt.prepare(PrepareSpec{valid_cfg.string()});
    CHECK(!p_ending.accepted);
    CHECK(p_ending.reason == serve_reason::kLauncherEnding);

    // 8. trace_enabled = true -> trace-enabled, ColdOnly.
    MockExecutionSink trace_sink;
    LauncherEnd trace_end;
    ExecutionRuntimeOptions trace_opts;
    trace_opts.instance = "8899aabbccddeeff";
    trace_opts.trace_enabled = true;
    ExecutionRuntime trace_rt(trace_sink, trace_end, std::move(trace_opts));
    const PrepareResult p_trace = trace_rt.prepare(PrepareSpec{valid_cfg.string(), std::string("fast")});
    CHECK(p_trace.accepted);
    CHECK(!p_trace.status.eligibility.eligible);
    CHECK(p_trace.status.eligibility.reason == "trace-enabled");
    CHECK(p_trace.status.launcher_state == LauncherState::ColdOnly);
    CHECK(trace_rt.status().launcher_allowed.run.reason == "trace-enabled");
}

TEST_CASE(execution_runtime_prepare_shutdown_race_stays_ending)
{
    const std::filesystem::path dir = scratch_dir("prepare_shutdown_race");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    write_text(valid_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    LauncherEnd end;
    MockExecutionSink sink;
    // Mark LauncherEnd during the Preparing status callback (under mutex_, lock-free atomic):
    // once prepare() finishes check_file() and re-acquires mutex_, it must see end.requested()
    // and transition directly to Ending.
    sink.status_hook = [&](const ExecutionStatus& s) {
        if (s.launcher_state == LauncherState::Preparing)
        {
            end.mark(stop_source::kEndOfInput);
        }
    };

    ExecutionRuntimeOptions opts;
    opts.instance = "1122334455667788";
    ExecutionRuntime rt(sink, end, std::move(opts));

    const PrepareResult prep = rt.prepare(PrepareSpec{valid_cfg.string()});
    const ExecutionEnd ended = rt.run_loop();
    CHECK(!prep.accepted);
    CHECK(prep.reason == serve_reason::kLauncherEnding);
    CHECK(prep.status.launcher_state == LauncherState::Ending);
    CHECK_EQ(ended.code, 0);
    CHECK(!ended.at_once);

    const ExecutionStatus final_status = rt.status();
    CHECK(final_status.launcher_state == LauncherState::Ending);
    const auto statuses = sink.statuses();
    CHECK(!statuses.empty());
    CHECK(statuses.back().launcher_state == LauncherState::Ending);
    for (std::size_t i = 0; i < statuses.size(); ++i)
    {
        if (statuses[i].launcher_state == LauncherState::Ending)
        {
            for (std::size_t j = i; j < statuses.size(); ++j)
            {
                CHECK(statuses[j].launcher_state == LauncherState::Ending);
            }
            break;
        }
    }
}

TEST_CASE(execution_runtime_start_run_contracts_and_in_memory_submission)
{
    const std::filesystem::path dir = scratch_dir("start_run_contracts");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    write_text(valid_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "abcdef0123456789";
    opts.base.host.config = valid_cfg.string();
    opts.base.host.stdin_commands = true;
    opts.base.host.control = ControlHost{};
    opts.base.host.control->listen = "127.0.0.1:0";

    std::mutex mu;
    std::condition_variable cv;
    int run_calls = 0;
    bool in_run2 = false;
    bool release_run2 = false;
    RunRequest seen_req2;
    RunRequest seen_req3;
    std::optional<ExecutionStatus> mid_status;

    opts.run = [&](const RunRequest& req, const RunObservers&, const RunLent& lent) {
        int call = 0;
        {
            const std::lock_guard lock(mu);
            call = ++run_calls;
        }
        if (call == 1)
        {
            lent.controls->set_phase(Phase::Running);
            {
                const std::lock_guard lock(mu);
                seen_req2 = req;
                in_run2 = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_run2; });
        }
        else if (call == 2)
        {
            const std::lock_guard lock(mu);
            seen_req3 = req;
        }
        lent.controls->set_phase(Phase::Ended);
        return passed_report(req.host.config);
    };

    ExecutionRuntime rt(sink, end, std::move(opts));
    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });

    // 1. Compatible Unprepared start_run (without prepare()) and pre_usage finding -> Usage report, seq 1.
    RunSubmission bad_sub;
    bad_sub.pre_usage.push_back(Finding{Severity::Error, "usage", "bad inline intent", 0, "", ""});
    const CommandReply r1 = rt.start_run(std::move(bad_sub));
    CHECK(r1.accepted);
    CHECK(r1.run_id == "abcdef0123456789-1");

    // Wait for Run 1's post-run status delivery (happens after busy_ clears) so it cannot
    // arrive after status_before_run2 below.
    CHECK(sink.wait_last_run(*r1.run_id));
    const auto f1 = sink.finished_run(*r1.run_id);
    CHECK(f1.has_value());
    CHECK(f1->report.result() == RunResult::Usage);
    CHECK(f1->status.launcher_state == LauncherState::Unprepared);

    const PrepareResult prep = rt.prepare(PrepareSpec{});
    CHECK(prep.accepted);
    CHECK(prep.status.launcher_state == LauncherState::Ready);

    const std::size_t status_before_run2 = sink.statuses().size();

    // 2. Ready start_run with in-memory RunSubmission (RunIntent start_paused).
    RunSubmission sub2;
    sub2.intent.start_paused = true;
    const CommandReply r2 = rt.start_run(std::move(sub2));
    CHECK(r2.accepted);
    CHECK(r2.run_id == "abcdef0123456789-2");

    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return in_run2; });
    }
    mid_status = rt.status();
    CHECK(mid_status->launcher_state == LauncherState::Running);
    CHECK(mid_status->active_run_id == r2.run_id);
    CHECK(mid_status->run_state.has_value());
    CHECK(mid_status->run_state->phase == Phase::Running);
    CHECK(mid_status->run_state->paused);
    // Only Launcher-axis transition (Ready -> Running) fired on_status; internal RunState updates did not.
    CHECK_EQ(sink.statuses().size(), status_before_run2 + 1);
    CHECK(sink.statuses().back().launcher_state == LauncherState::Running);

    // 3. Refused start_run while busy does NOT advance seq_.
    const CommandReply busy_reply = rt.start_run(RunSubmission{});
    CHECK(!busy_reply.accepted);
    CHECK(busy_reply.reason == serve_reason::kBusy);

    {
        const std::lock_guard lock(mu);
        release_run2 = true;
    }
    cv.notify_all();

    // Start Run 3 once Run 2 has finished to verify seq_ == 3 (not 4) and scenario+timeline forwarding.
    CHECK(sink.wait_last_run(*r2.run_id));
    RunSubmission sub3;
    sub3.intent.scenario = "drive.scenario.yaml";
    sub3.timeline = "out/timeline.json";
    const CommandReply r3 = rt.start_run(std::move(sub3));
    CHECK(r3.accepted);
    CHECK(r3.run_id == "abcdef0123456789-3");
    CHECK(seen_req2.intent.start_paused);
    CHECK(seen_req2.host.control.has_value());
    CHECK(seen_req2.host.control->run_id == *r2.run_id);
    CHECK(seen_req2.host.control->run_id_required);

    // Wait for Run 3 to complete before reading seen_req3 and shutting down.
    CHECK(sink.wait_last_run(*r3.run_id));
    CHECK(sink.finished_run(*r3.run_id).has_value());
    CHECK(seen_req3.intent.scenario == std::optional<std::string>("drive.scenario.yaml"));
    CHECK(seen_req3.host.timeline == "out/timeline.json");

    rt.request_shutdown(stop_source::kEndOfInput);
    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, 0);
    CHECK(!rt.start_run(RunSubmission{}).accepted);
}

TEST_CASE(execution_runtime_stop_modes_graceful_force_and_close_cut)
{
    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "2233445566778899";
    opts.base.host.config = "sim.yaml";
    opts.base.host.stdin_commands = true;

    std::mutex mu;
    std::condition_variable cv;
    int active_num = 0;
    int phase_step = 0;
    bool checked_step1 = false;
    bool cut_after_graceful_dup = true;
    bool cut_force_from_zero = false;
    bool cut_force_dup = true;
    bool cut_graceful_then_force = false;

    opts.run = [&](const RunRequest&, const RunObservers& obs, const RunLent& lent) {
        int num = 0;
        {
            const std::lock_guard lock(mu);
            num = ++active_num;
        }
        cv.notify_all();
        if (num == 1)
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return phase_step >= 1; });
            cut_after_graceful_dup = obs.close_cut(1);
            checked_step1 = true;
            lock.unlock();
            cv.notify_all();
            lock.lock();
            cv.wait(lock, [&] { return phase_step >= 2; });
            cut_force_from_zero = obs.close_cut(0);
            cut_force_dup = obs.close_cut(2);
        }
        else if (num == 2)
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return phase_step >= 3; });
            cut_graceful_then_force = obs.close_cut(1);
        }
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    ExecutionRuntime rt(sink, end, std::move(opts));
    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });

    // Validation while idle.
    const CommandReply missing_id = rt.stop_run("", StopMode::Graceful);
    CHECK(!missing_id.accepted);
    CHECK(missing_id.reason == serve_reason::kRunRequired);

    const CommandReply idle_stop = rt.stop_run("2233445566778899-1", StopMode::Graceful);
    CHECK(!idle_stop.accepted);
    CHECK(idle_stop.reason == serve_reason::kRunChanged);

    // Run 1: Graceful (idempotent 1->1), then Force (idempotent 2->2).
    const CommandReply s1 = rt.start_run(RunSubmission{});
    CHECK(s1.accepted);
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return active_num == 1; });
    }
    CHECK_EQ(rt.stop_run(*s1.run_id, StopMode::Graceful).stop_requests, 1);
    CHECK_EQ(rt.stop_run(*s1.run_id, StopMode::Graceful).stop_requests, 1);
    {
        const std::lock_guard lock(mu);
        phase_step = 1;
    }
    cv.notify_all();
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return checked_step1; });
    }
    CHECK_EQ(rt.stop_run(*s1.run_id, StopMode::Force).stop_requests, 2);
    CHECK_EQ(rt.stop_run(*s1.run_id, StopMode::Force).stop_requests, 2);
    {
        const std::lock_guard lock(mu);
        phase_step = 2;
    }
    cv.notify_all();

    // Run 2: Graceful (1) before close, then Force (2) during close -> close_cut(1) == true.
    CHECK(sink.wait_last_run(*s1.run_id));
    const CommandReply s2 = rt.start_run(RunSubmission{});
    CHECK(s2.accepted);
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return active_num == 2; });
    }
    CHECK_EQ(rt.stop_run(*s2.run_id, StopMode::Graceful).stop_requests, 1);
    CHECK_EQ(rt.stop_run(*s2.run_id, StopMode::Force).stop_requests, 2);
    {
        const std::lock_guard lock(mu);
        phase_step = 3;
    }
    cv.notify_all();

    // Wait for Run 2 to finish before shutting down cleanly.
    CHECK(sink.wait_last_run(*s2.run_id));
    CHECK(sink.finished_run(*s2.run_id).has_value());
    rt.request_shutdown(stop_source::kEndOfInput);
    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, 0);
    CHECK(!cut_after_graceful_dup);
    CHECK(cut_force_from_zero);
    CHECK(!cut_force_dup);
    CHECK(cut_graceful_then_force);
}

TEST_CASE(execution_runtime_send_time_command_via_entry_api)
{
    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "33445566778899aa";
    opts.base.host.config = "sim.yaml";
    opts.base.host.stdin_commands = true;

    std::mutex mu;
    std::condition_variable cv;
    bool in_run = false;
    bool detached = false;
    bool release_run = false;
    int stops_after_detach = 0;

    opts.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        lent.controls->set_time_lock({});
        {
            const std::lock_guard lock(mu);
            in_run = true;
        }
        cv.notify_all();
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_run; });
        }
        lent.controls->set_phase(Phase::Ended);
        {
            const std::lock_guard lock(mu);
            detached = true;
        }
        cv.notify_all();
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return stops_after_detach > 0; });
        return passed_report();
    };

    ExecutionRuntime rt(sink, end, std::move(opts));
    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });

    const CommandReply started = rt.start_run(RunSubmission{});
    CHECK(started.accepted);
    const std::string run_id = *started.run_id;
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return in_run; });
    }

    // Missing and mismatched run IDs.
    Command pause_cmd;
    pause_cmd.name = "pause";
    CHECK(rt.send_time_command("", pause_cmd, "api").reason == serve_reason::kRunRequired);
    CHECK(rt.send_time_command("33445566778899aa-99", pause_cmd, "api").reason == serve_reason::kRunChanged);

    // Programmatic pause without setting accepted=true is normalized and accepted via Entry::Api.
    const CommandReply paused = rt.send_time_command(run_id, pause_cmd, "api");
    CHECK(paused.accepted);

    // Pre-refused command preserves refusal without calling TimeCommands.
    Command pre_refused;
    pre_refused.name = "pause";
    pre_refused.accepted = false;
    pre_refused.reason = reason::kInvalidCommand;
    pre_refused.message = "bad payload";
    const CommandReply pre_reply = rt.send_time_command(run_id, pre_refused, "api");
    CHECK(!pre_reply.accepted);
    CHECK(pre_reply.reason == reason::kInvalidCommand);

    // "write" via Entry::Api -> not-a-time-command.
    Command write_cmd;
    write_cmd.name = "write";
    const CommandReply write_reply = rt.send_time_command(run_id, write_cmd, "api");
    CHECK(!write_reply.accepted);
    CHECK(write_reply.reason == "not-a-time-command");

    // Resume and speed via "api".
    Command speed_cmd;
    speed_cmd.name = "speed";
    speed_cmd.speed = 2.0;
    CHECK(rt.send_time_command(run_id, speed_cmd, "api").accepted);

    Command resume_cmd;
    resume_cmd.name = "resume";
    CHECK(rt.send_time_command(run_id, resume_cmd, "api").accepted);

    {
        const std::lock_guard lock(mu);
        release_run = true;
    }
    cv.notify_all();
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return detached; });
    }

    // After detach: pause is refused ("not-running"), while stop via "api" is still accepted.
    CHECK(rt.send_time_command(run_id, pause_cmd, "api").reason == "not-running");
    Command stop_cmd;
    stop_cmd.name = "stop";
    const CommandReply stop_reply = rt.send_time_command(run_id, stop_cmd, "api");
    CHECK(stop_reply.accepted);
    CHECK_EQ(stop_reply.stop_requests, 1);
    {
        const std::lock_guard lock(mu);
        stops_after_detach = stop_reply.stop_requests;
    }
    cv.notify_all();

    CHECK(sink.wait_last_run(run_id));
    CHECK(sink.finished_run(run_id).has_value());
    rt.request_shutdown(stop_source::kEndOfInput);
    CHECK_EQ(loop_future.get().code, 0);

    bool saw_api_pause = false;
    bool saw_api_stop = false;
    for (const RecordedCommandEvent& ev : sink.command_events())
    {
        if (ev.source == "api" && ev.command == "pause" && ev.accepted)
        {
            saw_api_pause = true;
        }
        if (ev.source == "api" && ev.command == "stop" && ev.accepted)
        {
            saw_api_stop = true;
        }
    }
    CHECK(saw_api_pause);
    CHECK(saw_api_stop);
}

TEST_CASE(execution_runtime_handle_release_and_contamination)
{
    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "445566778899aabb";
    opts.base.host.config = "sim.yaml";
    opts.base.host.stdin_commands = true;

    std::mutex mu;
    std::condition_variable cv;
    int run_idx = 0;
    bool in_run1 = false;
    bool release_run1 = false;
    std::weak_ptr<void> weak_srv;
    std::weak_ptr<const scenario::TimelineRecord> weak_tl;

    opts.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) -> RunReport {
        const int n = ++run_idx;
        if (n == 1)
        {
            {
                const std::lock_guard lock(mu);
                in_run1 = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_run1; });
            RunReport r = passed_report();
            auto srv = std::make_shared<int>(42);
            scenario::Scenario empty_scenario{};
            auto tl = std::make_shared<scenario::TimelineRecord>(empty_scenario);
            weak_srv = srv;
            weak_tl = tl;
            r.ended_control_server = std::move(srv);
            r.run->scenario.timeline_record = std::move(tl);
            lent.controls->set_phase(Phase::Ended);
            return r;
        }
        if (n == 2)
        {
            RunReport r = passed_report();
            r.run->close.not_closed_ms = 5000;
            lent.controls->set_phase(Phase::Ended);
            return r;
        }
        throw std::runtime_error("unexpected run");
    };

    ExecutionRuntime rt(sink, end, std::move(opts));

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    const CommandReply s1 = rt.start_run(RunSubmission{});
    CHECK(s1.accepted);
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return in_run1; });
    }
    {
        const std::lock_guard lock(mu);
        release_run1 = true;
    }
    cv.notify_all();

    // Wait for Run 1 result: ended_control_server and timeline_record are released before finish_run.
    CHECK(sink.wait_last_run(*s1.run_id));
    const auto f1 = sink.finished_run(*s1.run_id);
    CHECK(f1.has_value());
    CHECK(!f1->launcher_ends);
    CHECK(f1->report.ended_control_server == nullptr);
    CHECK(f1->report.run->scenario.timeline_record == nullptr);
    CHECK(weak_srv.expired());
    CHECK(weak_tl.expired());

    // Run 2 contaminates (not_closed_ms).
    const CommandReply s2 = rt.start_run(RunSubmission{});
    CHECK(s2.accepted);
    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, kExitContaminated);
    CHECK(ended.at_once);
    CHECK(rt.status().launcher_state == LauncherState::Contaminated);

    const auto f2 = sink.finished_run(*s2.run_id);
    CHECK(f2.has_value());
    CHECK(f2->launcher_ends);
    CHECK(f2->status.launcher_state == LauncherState::Contaminated);
    CHECK(rt.prepare(PrepareSpec{}).reason == serve_reason::kContaminated);
    CHECK(rt.start_run(RunSubmission{}).reason == serve_reason::kContaminated);

    // Exception contamination in a fresh runtime.
    MockExecutionSink ex_sink;
    LauncherEnd ex_end;
    ExecutionRuntimeOptions ex_opts;
    ex_opts.instance = "5566778899aabbcc";
    ex_opts.base.host.config = "sim.yaml";
    ex_opts.run = [](const RunRequest&, const RunObservers&, const RunLent&) -> RunReport {
        throw std::runtime_error("injected failure");
    };
    ExecutionRuntime ex_rt(ex_sink, ex_end, std::move(ex_opts));
    auto ex_loop = std::async(std::launch::async, [&] { return ex_rt.run_loop(); });
    const CommandReply ex_s1 = ex_rt.start_run(RunSubmission{});
    CHECK(ex_s1.accepted);
    const ExecutionEnd ex_ended = ex_loop.get();
    CHECK_EQ(ex_ended.code, kExitContaminated);
    CHECK(ex_ended.at_once);
    CHECK(ex_rt.status().launcher_state == LauncherState::Contaminated);
    CHECK(ex_rt.start_run(RunSubmission{}).reason == serve_reason::kContaminated);
}

TEST_CASE(execution_runtime_pipe_wakeup_order_allows_immediate_next_run_inside_finish_run)
{
    const std::filesystem::path dir = scratch_dir("pipe_wakeup_order");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    write_text(valid_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "66778899aabbccdd";
    std::atomic<int> runs{0};
    opts.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        ++runs;
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };
    ExecutionRuntime rt(sink, end, std::move(opts));
    const PrepareResult prep = rt.prepare(PrepareSpec{valid_cfg.string()});
    CHECK(prep.accepted);
    CHECK(prep.status.launcher_state == LauncherState::Ready);

    CommandReply wake_reply;
    // Simulate a reader waking up the instant finish_run writes the result to the OS pipe,
    // before finish_run returns to run_loop(): busy_ must already be false and state_ Ready.
    sink.finish_hook = [&] {
        if (runs.load() == 1)
        {
            wake_reply = rt.start_run(RunSubmission{});
        }
    };

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    const CommandReply s1 = rt.start_run(RunSubmission{});
    CHECK(s1.accepted);
    CHECK(s1.run_id == "66778899aabbccdd-1");

    CHECK(sink.wait_last_run("66778899aabbccdd-2"));
    CHECK(wake_reply.accepted);
    CHECK(wake_reply.run_id == "66778899aabbccdd-2");

    rt.request_shutdown(stop_source::kEndOfInput);
    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, 0);

    const auto statuses = sink.statuses();
    for (std::size_t i = 0; i < statuses.size(); ++i)
    {
        CHECK_EQ(statuses[i].status_rev, i);
    }
}

TEST_CASE(execution_runtime_shutdown_during_run_transitions_directly_to_ending_in_result)
{
    const std::filesystem::path dir = scratch_dir("shutdown_during_run_no_transient_ready");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    write_text(valid_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    std::mutex mu;
    std::condition_variable cv;
    bool in_run = false;
    bool release_run = false;

    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "778899aabbccdde0";
    opts.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        {
            const std::lock_guard lock(mu);
            in_run = true;
        }
        cv.notify_all();
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_run; });
        }
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };
    ExecutionRuntime rt(sink, end, std::move(opts));
    const PrepareResult prep = rt.prepare(PrepareSpec{valid_cfg.string()});
    CHECK(prep.accepted);
    CHECK(prep.status.launcher_state == LauncherState::Ready);

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    const CommandReply started = rt.start_run(RunSubmission{});
    CHECK(started.accepted);
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return in_run; });
    }
    rt.request_shutdown(stop_source::kEndOfInput);
    {
        const std::lock_guard lock(mu);
        release_run = true;
    }
    cv.notify_all();

    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, kExitInterrupted);
    CHECK(!ended.at_once);

    const auto f1 = sink.finished_run(*started.run_id);
    CHECK(f1.has_value());
    CHECK(f1->launcher_ends);
    CHECK(f1->status.launcher_state == LauncherState::Ending);

    // Status sequence: 0 Unprepared -> 1 Preparing -> 2 Ready -> 3 Running -> 4 Ending (no transient Ready after Running).
    const auto statuses = sink.statuses();
    CHECK_EQ(statuses.size(), std::size_t{5});
    CHECK(statuses[0].launcher_state == LauncherState::Unprepared);
    CHECK(statuses[1].launcher_state == LauncherState::Preparing);
    CHECK(statuses[2].launcher_state == LauncherState::Ready);
    CHECK(statuses[3].launcher_state == LauncherState::Running);
    CHECK(statuses[4].launcher_state == LauncherState::Ending);
    for (std::size_t i = 0; i < statuses.size(); ++i)
    {
        CHECK_EQ(statuses[i].status_rev, i);
    }
}

TEST_CASE(execution_runtime_single_run_allows_trace_and_fixed_registry_and_exits_after_one_run)
{
    const std::filesystem::path dir = scratch_dir("single_run_mode");
    const std::filesystem::path fixed_manual_cfg = dir / "fixed_manual.yaml";
    write_text(fixed_manual_cfg,
               "vehicle_sil_simulation: 2\n"
               "registry_uri: silkit://127.0.0.1:8500\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n    manual: true\n");

    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "99aabbccddeeff00";
    opts.base.host.config = fixed_manual_cfg.string();
    opts.single_run = true;
    opts.trace_enabled = true;
    opts.refuse_fixed_registry = false;
    bool seen_refuse_fixed = true;
    opts.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        seen_refuse_fixed = lent.refuse_fixed_registry;
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    ExecutionRuntime rt(sink, end, std::move(opts));
    const PrepareResult prep = rt.prepare(PrepareSpec{});
    CHECK(prep.accepted);
    CHECK(prep.status.eligibility.eligible);
    CHECK(prep.status.launcher_state == LauncherState::Ready);
    CHECK(prep.status.launcher_allowed.run.ok);

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    const CommandReply started = rt.start_run(RunSubmission{});
    CHECK(started.accepted);

    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, 0);
    CHECK(!ended.at_once);
    CHECK(!seen_refuse_fixed);

    const auto f1 = sink.finished_run(*started.run_id);
    CHECK(f1.has_value());
    CHECK(f1->launcher_ends);
    CHECK(f1->status.launcher_state == LauncherState::Ending);
    CHECK(!rt.start_run(RunSubmission{}).accepted);
}

TEST_CASE(execution_runtime_single_run_preserves_operator_stop_exit_codes_for_control_stop)
{
    for (const bool with_incomplete_scenario : {false, true})
    {
        MockExecutionSink sink;
        LauncherEnd end;
        ExecutionRuntimeOptions opts;
        opts.instance = with_incomplete_scenario ? "aabbccddeeff0012" : "aabbccddeeff0011";
        opts.base.host.config = "sim.yaml";
        opts.single_run = true;
        opts.run = [with_incomplete_scenario](const RunRequest&, const RunObservers&, const RunLent& lent) {
            lent.controls->set_phase(Phase::Running);
            lent.controls->stops().request(stop_source::kControlStop);
            lent.controls->set_phase(Phase::Ended);
            RunReport report = passed_report();
            report.run->end.simulation = SimulationEnd::Interrupted;
            report.run->end.interrupted_by = std::string(stop_source_name(stop_source::kControlStop));
            report.run->end.interrupted_kind = StopKind::Operator;
            if (with_incomplete_scenario)
            {
                report.run->scenario.result.emplace();
                report.run->scenario.result->status = scenario::ScenarioStatus::Incomplete;
            }
            return report;
        };

        ExecutionRuntime rt(sink, end, std::move(opts));
        auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
        const CommandReply started = rt.start_run(RunSubmission{});
        CHECK(started.accepted);

        const ExecutionEnd ended = loop_future.get();
        CHECK_EQ(ended.code, with_incomplete_scenario ? kExitIncomplete : 0);
        CHECK(!ended.at_once);
    }
}
