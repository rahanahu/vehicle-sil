// SPDX-License-Identifier: MIT
//
// Deterministic unit tests for ExecutionRuntime (execution_runtime.hpp): 2-axis state machine,
// Prepare/Eligibility/Metadata, Prepare+Shutdown barrier, RunSubmission, StopMode
// (Graceful/Force/Escalate) + close_cut, send_time_command via Entry::Api, bounded result()
// retention, and contamination handling. Uses injectable RunFunction + mutex/CV barriers
// (no SIL Kit, no subprocesses, no fixed sleeps).
#include "test_harness.hpp"

#include "vehicle_sil/launch/execution_runtime.hpp"
#include "vehicle_sil/launch/run_sink.hpp"
#include "vehicle_sil/scenario/timeline.hpp"
#include "usecases/sha256.hpp"

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

class MockExecutionSink final : public ExecutionSink
{
public:
    std::function<void(const ExecutionStatus&)> status_hook{};
    std::function<void()> finish_hook{};

    void on_status(const ExecutionStatus& status) override
    {
        if (status_hook)
        {
            status_hook(status);
        }
        const std::lock_guard lock(mu_);
        statuses_.push_back(status);
        cv_.notify_all();
    }

    void on_idle() override
    {
        const std::lock_guard lock(mu_);
        ++idle_count_;
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

    bool finish_run(RunEventSink& window, const RunReport&, bool launcher_ends, const std::function<bool()>&) override
    {
        const auto& w = static_cast<const MockRunWindow&>(window);
        {
            const std::lock_guard lock(mu_);
            finished_runs_.push_back({w.run_id(), launcher_ends});
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

    int idle_count() const
    {
        const std::lock_guard lock(mu_);
        return idle_count_;
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

    std::vector<std::pair<std::string, bool>> finished_runs() const
    {
        const std::lock_guard lock(mu_);
        return finished_runs_;
    }

private:
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::vector<ExecutionStatus> statuses_;
    int idle_count_ = 0;
    std::vector<RecordedRefusal> refusals_;
    std::vector<std::string> opened_runs_;
    std::vector<std::pair<std::string, bool>> finished_runs_;
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
    CHECK_EQ(sink.idle_count(), 1);
    const ExecutionStatus init = rt.status();
    CHECK(init.launcher_state == LauncherState::Idle);
    CHECK(std::string(launcher_state_name(init.launcher_state)) == "idle");
    CHECK_EQ(init.status_rev, std::uint64_t{0});
    CHECK(init.instance == "0011223344556677");
    CHECK(!init.active_run_id.has_value());
    CHECK(!init.last_run_id.has_value());
    CHECK(!init.run_state.has_value());
    CHECK(init.launcher_allowed.prepare.ok);
    CHECK(!init.launcher_allowed.run.ok);
    CHECK(init.launcher_allowed.run.reason == serve_reason::kNotPrepared);

    // 1. Valid config -> Idle -> Preparing -> Ready with ConfigMetadata.
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

    // 2. Fixed registry -> resets Ready back to Idle with fixed-registry reason.
    const PrepareResult p_fixed = rt.prepare(PrepareSpec{fixed_cfg.string()});
    CHECK(p_fixed.accepted);
    CHECK(!p_fixed.status.eligibility.eligible);
    CHECK(p_fixed.status.eligibility.reason == "fixed-registry");
    CHECK(p_fixed.status.launcher_state == LauncherState::Idle);
    CHECK(!p_fixed.status.launcher_allowed.run.ok);
    CHECK(p_fixed.status.launcher_allowed.run.reason == "fixed-registry");

    // 3. Manual participants -> manual-participants, Idle.
    const PrepareResult p_manual = rt.prepare(PrepareSpec{manual_cfg.string()});
    CHECK(p_manual.accepted);
    CHECK(!p_manual.status.eligibility.eligible);
    CHECK(p_manual.status.eligibility.reason == "manual-participants");
    CHECK(p_manual.status.launcher_state == LauncherState::Idle);

    // 4. Unreadable config -> config-unreadable, Idle.
    const PrepareResult p_missing = rt.prepare(PrepareSpec{(dir / "does_not_exist.yaml").string()});
    CHECK(p_missing.accepted);
    CHECK(!p_missing.status.eligibility.eligible);
    CHECK(p_missing.status.eligibility.reason == "config-unreadable");
    CHECK(p_missing.status.launcher_state == LauncherState::Idle);

    // 5. Usage error (nonexistent variant) -> usage, Idle, file_sha256 still computed from readable file.
    const PrepareResult p_usage = rt.prepare(PrepareSpec{valid_cfg.string(), std::string("missing_variant")});
    CHECK(p_usage.accepted);
    CHECK(!p_usage.status.eligibility.eligible);
    CHECK(p_usage.status.eligibility.reason == "usage");
    CHECK(p_usage.status.launcher_state == LauncherState::Idle);
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

    // 8. trace_enabled = true -> trace-enabled, Idle.
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
    CHECK(p_trace.status.launcher_state == LauncherState::Idle);
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

    std::mutex mu;
    std::condition_variable cv;
    bool entered_preparing = false;
    bool release_prepare = false;

    MockExecutionSink sink;
    sink.status_hook = [&](const ExecutionStatus& s) {
        if (s.launcher_state == LauncherState::Preparing)
        {
            {
                const std::lock_guard lock(mu);
                entered_preparing = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_prepare; });
        }
    };

    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "1122334455667788";
    ExecutionRuntime rt(sink, end, std::move(opts));

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    auto prep_future = std::async(std::launch::async, [&] { return rt.prepare(PrepareSpec{valid_cfg.string()}); });

    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered_preparing; });
    }
    rt.request_shutdown(stop_source::kEndOfInput);
    {
        const std::lock_guard lock(mu);
        release_prepare = true;
    }
    cv.notify_all();

    const PrepareResult prep = prep_future.get();
    const ExecutionEnd ended = loop_future.get();
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

    // 1. Compatible Idle start_run (without prepare()) and pre_usage finding -> Usage report, seq 1.
    RunSubmission bad_sub;
    bad_sub.pre_usage.push_back(Finding{Severity::Error, "usage", "bad inline intent", 0, "", ""});
    const CommandReply r1 = rt.start_run(std::move(bad_sub));
    CHECK(r1.accepted);
    CHECK(r1.run_id == "abcdef0123456789-1");

    // Wait for Run 1's post-run Idle status delivery (happens after busy_ clears) so it cannot
    // arrive after status_before_run2 below.
    sink.wait_status_count(2);
    PrepareResult prep;
    for (;;)
    {
        prep = rt.prepare(PrepareSpec{});
        if (prep.accepted)
        {
            break;
        }
        CHECK(prep.reason == serve_reason::kBusy);
        std::this_thread::yield();
    }
    CHECK(prep.status.launcher_state == LauncherState::Ready);
    const ResultQuery q1 = rt.result(*r1.run_id);
    CHECK(q1.ok);
    CHECK(q1.report && q1.report->result() == RunResult::Usage);

    sink.wait_status_count(4);
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

    // Wait for Run 2 to return to Ready via idle_count barrier? Check result via loop / start Run 3.
    // Start Run 3 once Run 2 finishes to verify seq_ == 3 (not 4) and scenario+timeline forwarding.
    CommandReply r3;
    for (;;)
    {
        RunSubmission sub3;
        sub3.intent.scenario = "drive.scenario.yaml";
        sub3.timeline = "out/timeline.json";
        r3 = rt.start_run(std::move(sub3));
        if (r3.accepted)
        {
            break;
        }
        CHECK(r3.reason == serve_reason::kBusy);
        std::this_thread::yield();
    }
    CHECK(r3.run_id == "abcdef0123456789-3");
    CHECK(seen_req2.intent.start_paused);
    CHECK(seen_req2.host.control.has_value());
    CHECK(seen_req2.host.control->run_id == *r2.run_id);
    CHECK(seen_req2.host.control->run_id_required);

    // Wait for Run 3 to complete before reading seen_req3 and shutting down.
    for (;;)
    {
        if (rt.result(*r3.run_id).ok)
        {
            break;
        }
        std::this_thread::yield();
    }
    CHECK(seen_req3.intent.scenario == std::optional<std::string>("drive.scenario.yaml"));
    CHECK(seen_req3.host.timeline == "out/timeline.json");

    rt.request_shutdown(stop_source::kEndOfInput);
    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, 0);
    CHECK(!rt.start_run(RunSubmission{}).accepted);
}

TEST_CASE(execution_runtime_stop_modes_graceful_force_escalate_and_close_cut)
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
        }
        else if (num == 3)
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return phase_step >= 4; });
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

    // Run 2: Escalate (0->1->2->2).
    CommandReply s2;
    for (;;)
    {
        s2 = rt.start_run(RunSubmission{});
        if (s2.accepted)
        {
            break;
        }
        std::this_thread::yield();
    }
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return active_num == 2; });
    }
    CHECK_EQ(rt.stop_run(*s2.run_id, StopMode::Escalate).stop_requests, 1);
    CHECK_EQ(rt.stop_run(*s2.run_id, StopMode::Escalate).stop_requests, 2);
    CHECK_EQ(rt.stop_run(*s2.run_id, StopMode::Escalate).stop_requests, 2);
    {
        const std::lock_guard lock(mu);
        phase_step = 3;
    }
    cv.notify_all();

    // Run 3: Graceful (1) before close, then Force (2) during close -> close_cut(1) == true.
    CommandReply s3;
    for (;;)
    {
        s3 = rt.start_run(RunSubmission{});
        if (s3.accepted)
        {
            break;
        }
        std::this_thread::yield();
    }
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return active_num == 3; });
    }
    CHECK_EQ(rt.stop_run(*s3.run_id, StopMode::Graceful).stop_requests, 1);
    CHECK_EQ(rt.stop_run(*s3.run_id, StopMode::Force).stop_requests, 2);
    {
        const std::lock_guard lock(mu);
        phase_step = 4;
    }
    cv.notify_all();

    // Wait for Run 3 to finish before shutting down cleanly.
    for (;;)
    {
        if (rt.result(*s3.run_id).ok)
        {
            break;
        }
        std::this_thread::yield();
    }
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

    for (;;)
    {
        if (rt.result(run_id).ok)
        {
            break;
        }
        std::this_thread::yield();
    }
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

TEST_CASE(execution_runtime_results_bounded_memory_and_contamination)
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
            lent.controls->set_phase(Phase::Ended);
            return passed_report();
        }
        if (n == 3)
        {
            RunReport r = passed_report();
            r.run->close.not_closed_ms = 5000;
            lent.controls->set_phase(Phase::Ended);
            return r;
        }
        throw std::runtime_error("unexpected run");
    };

    ExecutionRuntime rt(sink, end, std::move(opts));
    CHECK(rt.result().reason == serve_reason::kRunNotFound);

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    const CommandReply s1 = rt.start_run(RunSubmission{});
    CHECK(s1.accepted);
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return in_run1; });
    }
    CHECK(!rt.result().ok);
    CHECK(rt.result().reason == serve_reason::kRunInProgress);
    CHECK(rt.result(*s1.run_id).reason == serve_reason::kRunInProgress);

    {
        const std::lock_guard lock(mu);
        release_run1 = true;
    }
    cv.notify_all();

    // Wait for Run 1 result.
    ResultQuery q1;
    for (;;)
    {
        q1 = rt.result(*s1.run_id);
        if (q1.ok)
        {
            break;
        }
        std::this_thread::yield();
    }
    CHECK(!q1.launcher_ends);
    CHECK(q1.report != nullptr);
    CHECK(q1.report->ended_control_server == nullptr);
    CHECK(q1.report->run->scenario.timeline_record == nullptr);
    CHECK(weak_srv.expired());
    CHECK(weak_tl.expired());

    // Run 2 replaces Run 1 in bounded last_result_.
    const CommandReply s2 = rt.start_run(RunSubmission{});
    CHECK(s2.accepted);
    ResultQuery q2;
    for (;;)
    {
        q2 = rt.result(*s2.run_id);
        if (q2.ok)
        {
            break;
        }
        std::this_thread::yield();
    }
    CHECK(rt.result(*s1.run_id).reason == serve_reason::kRunNotFound);
    CHECK(rt.result().run_id == *s2.run_id);

    // Run 3 contaminates (not_closed_ms).
    const int idles_before_r3 = sink.idle_count();
    const CommandReply s3 = rt.start_run(RunSubmission{});
    CHECK(s3.accepted);
    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, kExitContaminated);
    CHECK(ended.at_once);
    CHECK(rt.status().launcher_state == LauncherState::Contaminated);
    CHECK_EQ(sink.idle_count(), idles_before_r3); // on_idle NOT called after contamination

    const ResultQuery q3 = rt.result(*s3.run_id);
    CHECK(q3.ok);
    CHECK(q3.launcher_ends);
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

TEST_CASE(execution_runtime_prepare_done_status_and_shutdown_stay_monotonic)
{
    const std::filesystem::path dir = scratch_dir("prepare_done_shutdown_monotonic");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    write_text(valid_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    std::mutex mu;
    std::condition_variable cv;
    bool entered_ready = false;
    bool release_ready = false;

    MockExecutionSink sink;
    sink.status_hook = [&](const ExecutionStatus& s) {
        if (s.launcher_state == LauncherState::Ready)
        {
            {
                const std::lock_guard lock(mu);
                entered_ready = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_ready; });
        }
    };

    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "66778899aabbccdd";
    ExecutionRuntime rt(sink, end, std::move(opts));

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    auto prep_future = std::async(std::launch::async, [&] { return rt.prepare(PrepareSpec{valid_cfg.string()}); });

    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered_ready; });
    }
    rt.request_shutdown(stop_source::kEndOfInput);
    {
        const std::lock_guard lock(mu);
        release_ready = true;
    }
    cv.notify_all();

    const PrepareResult prep = prep_future.get();
    const ExecutionEnd ended = loop_future.get();
    CHECK(prep.accepted);
    CHECK(prep.status.launcher_state == LauncherState::Ending);
    CHECK_EQ(prep.status.status_rev, std::uint64_t{3});
    CHECK_EQ(ended.code, 0);
    CHECK(!ended.at_once);

    const auto statuses = sink.statuses();
    CHECK_EQ(statuses.size(), std::size_t{3});
    CHECK(statuses[0].launcher_state == LauncherState::Preparing);
    CHECK_EQ(statuses[0].status_rev, std::uint64_t{1});
    CHECK(statuses[1].launcher_state == LauncherState::Ready);
    CHECK_EQ(statuses[1].status_rev, std::uint64_t{2});
    CHECK(statuses[2].launcher_state == LauncherState::Ending);
    CHECK_EQ(statuses[2].status_rev, std::uint64_t{3});
}

TEST_CASE(execution_runtime_run_loop_drains_in_flight_status_before_exit_and_destruction)
{
    const std::filesystem::path dir = scratch_dir("run_loop_drains_destruction");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    write_text(valid_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    std::mutex mu;
    std::condition_variable cv;
    bool entered_ready = false;
    bool entered_finish = false;
    bool release_ready = false;
    std::atomic<bool> loop_exited{false};

    MockExecutionSink sink;
    sink.status_hook = [&](const ExecutionStatus& s) {
        if (s.launcher_state == LauncherState::Ready)
        {
            {
                const std::lock_guard lock(mu);
                entered_ready = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_ready; });
        }
    };
    sink.finish_hook = [&] {
        {
            const std::lock_guard lock(mu);
            entered_finish = true;
        }
        cv.notify_all();
    };

    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "778899aabbccdde0";
    opts.run = [](const RunRequest&, const RunObservers&, const RunLent&) -> RunReport {
        throw std::runtime_error("contaminate");
    };
    auto rt = std::make_unique<ExecutionRuntime>(sink, end, std::move(opts));

    auto loop_future = std::async(std::launch::async, [&] {
        const ExecutionEnd ended = rt->run_loop();
        loop_exited.store(true);
        return ended;
    });
    auto prep_future = std::async(std::launch::async, [&] { return rt->prepare(PrepareSpec{valid_cfg.string()}); });

    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered_ready; });
    }
    const CommandReply started = rt->start_run(RunSubmission{});
    CHECK(started.accepted);

    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered_finish; });
    }
    while (rt->status().launcher_state != LauncherState::Contaminated)
    {
        std::this_thread::yield();
    }
    CHECK(!loop_exited.load());
    {
        const std::lock_guard lock(mu);
        release_ready = true;
    }
    cv.notify_all();

    const ExecutionEnd ended = loop_future.get();
    CHECK(loop_exited.load());
    CHECK_EQ(ended.code, kExitContaminated);
    CHECK(ended.at_once);

    rt.reset();
    const PrepareResult prep = prep_future.get();
    CHECK(prep.accepted);
    CHECK(prep.status.launcher_state == LauncherState::Contaminated);

    const auto statuses = sink.statuses();
    CHECK_EQ(statuses.size(), std::size_t{4});
    CHECK(statuses[0].launcher_state == LauncherState::Preparing);
    CHECK_EQ(statuses[0].status_rev, std::uint64_t{1});
    CHECK(statuses[1].launcher_state == LauncherState::Ready);
    CHECK_EQ(statuses[1].status_rev, std::uint64_t{2});
    CHECK(statuses[2].launcher_state == LauncherState::Running);
    CHECK_EQ(statuses[2].status_rev, std::uint64_t{3});
    CHECK(statuses[3].launcher_state == LauncherState::Contaminated);
    CHECK_EQ(statuses[3].status_rev, std::uint64_t{4});
}

TEST_CASE(execution_runtime_start_run_and_run_loop_serialize_on_status)
{
    std::mutex mu;
    std::condition_variable cv;
    bool entered_running = false;
    bool entered_finish = false;
    bool release_running = false;

    MockExecutionSink sink;
    sink.status_hook = [&](const ExecutionStatus& s) {
        if (s.launcher_state == LauncherState::Running)
        {
            {
                const std::lock_guard lock(mu);
                entered_running = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release_running; });
        }
    };
    sink.finish_hook = [&] {
        {
            const std::lock_guard lock(mu);
            entered_finish = true;
        }
        cv.notify_all();
    };

    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "8899aabbccddeeff";
    opts.base.host.config = "sim.yaml";
    opts.run = [](const RunRequest&, const RunObservers&, const RunLent& lent) {
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };
    ExecutionRuntime rt(sink, end, std::move(opts));

    auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
    auto start_future = std::async(std::launch::async, [&] { return rt.start_run(RunSubmission{}); });

    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered_running; });
    }
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return entered_finish; });
    }
    {
        const std::lock_guard lock(mu);
        release_running = true;
    }
    cv.notify_all();

    const CommandReply started = start_future.get();
    CHECK(started.accepted);
    sink.wait_status_count(2);
    CHECK(rt.result(*started.run_id).ok);

    rt.request_shutdown(stop_source::kEndOfInput);
    const ExecutionEnd ended = loop_future.get();
    CHECK_EQ(ended.code, 0);

    const auto statuses = sink.statuses();
    CHECK_EQ(statuses.size(), std::size_t{3});
    CHECK(statuses[0].launcher_state == LauncherState::Running);
    CHECK_EQ(statuses[0].status_rev, std::uint64_t{1});
    CHECK(statuses[1].launcher_state == LauncherState::Idle);
    CHECK_EQ(statuses[1].status_rev, std::uint64_t{2});
    CHECK(statuses[2].launcher_state == LauncherState::Ending);
    CHECK_EQ(statuses[2].status_rev, std::uint64_t{3});
}

TEST_CASE(execution_runtime_prepare_exception_after_ready_clears_eligibility)
{
    const std::filesystem::path dir = scratch_dir("prepare_guard_exception");
    const std::filesystem::path valid_cfg = dir / "valid.yaml";
    write_text(valid_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    MockExecutionSink sink;
    LauncherEnd end;
    ExecutionRuntimeOptions opts;
    opts.instance = "99aabbccddeeff00";
    ExecutionRuntime rt(sink, end, std::move(opts));

    const PrepareResult ready = rt.prepare(PrepareSpec{valid_cfg.string()});
    CHECK(ready.accepted);
    CHECK(ready.status.launcher_state == LauncherState::Ready);
    CHECK(ready.status.launcher_allowed.run.ok);

    sink.status_hook = [](const ExecutionStatus& s) {
        if (s.launcher_state == LauncherState::Preparing)
        {
            throw std::runtime_error("sink failure");
        }
    };

    bool threw = false;
    try
    {
        static_cast<void>(rt.prepare(PrepareSpec{valid_cfg.string()}));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    sink.status_hook = nullptr;

    const ExecutionStatus after = rt.status();
    CHECK(after.launcher_state == LauncherState::Idle);
    CHECK(!after.eligibility.eligible);
    CHECK(after.eligibility.reason == "config-unreadable");
    CHECK(!after.launcher_allowed.run.ok);
    CHECK(after.launcher_allowed.prepare.ok);
    CHECK(!rt.start_run(RunSubmission{}).accepted);

    const PrepareResult recovered = rt.prepare(PrepareSpec{});
    CHECK(recovered.accepted);
    CHECK(recovered.status.launcher_state == LauncherState::Ready);
    CHECK(recovered.status.launcher_allowed.run.ok);
}

TEST_CASE(execution_runtime_throwing_on_status_drains_exit_barriers_without_hanging)
{
    {
        std::mutex mu;
        std::condition_variable cv;
        bool in_on_status = false;
        bool in_run = false;
        bool release_run = false;
        bool entered_finish = false;
        bool release_throw = false;
        std::atomic<bool> loop_exited{false};

        MockExecutionSink sink;
        sink.status_hook = [&](const ExecutionStatus& s) {
            if (s.launcher_state == LauncherState::Running)
            {
                {
                    const std::lock_guard lock(mu);
                    in_on_status = true;
                }
                cv.notify_all();
                std::unique_lock lock(mu);
                cv.wait(lock, [&] { return release_throw; });
                throw std::runtime_error("on_status boom");
            }
        };
        sink.finish_hook = [&] {
            {
                const std::lock_guard lock(mu);
                entered_finish = true;
            }
            cv.notify_all();
        };

        LauncherEnd end;
        ExecutionRuntimeOptions opts;
        opts.instance = "aabbccddeeff0011";
        opts.base.host.config = "sim.yaml";
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
        auto loop_future = std::async(std::launch::async, [&] {
            const ExecutionEnd ended = rt.run_loop();
            loop_exited.store(true);
            return ended;
        });
        auto start_future = std::async(std::launch::async, [&] { return rt.start_run(RunSubmission{}); });

        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return in_on_status && in_run; });
        }
        // Hold the active run open across request_shutdown() so input_ended_in_run_ is guaranteed,
        // then release the run to finish_run() so run_loop() parks at its post-run exit barrier
        // while status_emitting_ == true. Releasing on_status to throw must wake run_loop() and
        // drain pending_statuses_ cleanly without hanging.
        rt.request_shutdown(stop_source::kEndOfInput);
        {
            const std::lock_guard lock(mu);
            release_run = true;
        }
        cv.notify_all();
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return entered_finish; });
        }
        while (rt.status().launcher_state != LauncherState::Ending)
        {
            std::this_thread::yield();
        }
        CHECK(!loop_exited.load());
        {
            const std::lock_guard lock(mu);
            release_throw = true;
        }
        cv.notify_all();

        bool start_threw = false;
        try
        {
            static_cast<void>(start_future.get());
        }
        catch (const std::runtime_error&)
        {
            start_threw = true;
        }
        CHECK(start_threw);
        const ExecutionEnd ended = loop_future.get();
        CHECK(loop_exited.load());
        CHECK_EQ(ended.code, kExitInterrupted);
    }

    // Throwing directly inside run_loop()'s idle exit barrier (Ending) must be caught cleanly.
    {
        MockExecutionSink sink;
        sink.status_hook = [](const ExecutionStatus& s) {
            if (s.launcher_state == LauncherState::Ending)
            {
                throw std::runtime_error("ending status boom");
            }
        };
        LauncherEnd end;
        ExecutionRuntimeOptions opts;
        opts.instance = "bbccddeeff001122";
        ExecutionRuntime rt(sink, end, std::move(opts));
        auto loop_future = std::async(std::launch::async, [&] { return rt.run_loop(); });
        sink.set_closed(true);
        const ExecutionEnd ended = loop_future.get();
        CHECK_EQ(ended.code, kExitInterrupted);
        CHECK(rt.status().launcher_state == LauncherState::Ending);
    }

    // Throwing directly inside run_loop()'s post-run exit barrier (Ending) must also be caught cleanly.
    {
        std::mutex mu;
        std::condition_variable cv;
        bool in_run = false;
        bool release_run = false;

        MockExecutionSink sink;
        sink.status_hook = [](const ExecutionStatus& s) {
            if (s.launcher_state == LauncherState::Ending)
            {
                throw std::runtime_error("post-run ending status boom");
            }
        };
        LauncherEnd end;
        ExecutionRuntimeOptions opts;
        opts.instance = "ccddeeff00112233";
        opts.base.host.config = "sim.yaml";
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
        CHECK(rt.status().launcher_state == LauncherState::Ending);
    }
}
