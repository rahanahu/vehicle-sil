// SPDX-License-Identifier: MIT
//
// Transport-agnostic resident launcher execution runtime (ExecutionRuntime): manages the
// 2-axis state machine (Launcher axis + Run axis), Prepare, Start, Stop, Time commands, Status,
// Results, and Shutdown independently of NDJSON or HTTP/WebSocket framing. Internal.
#pragma once

#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/run_sink.hpp"
#include "vehicle_sil/runner/control_rules.hpp"
#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/runner/stop_requests.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::launch {

/// The exit code of a resident launcher when a run left the process in a state that is not known
/// (its own connection to SIL Kit did not close, or the run failed inside the launcher): the run's
/// result says so ("launcher_ends":true), and a new launcher takes the next run.
inline constexpr int kExitContaminated = 6;

/// The reasons for commands and queries refused by the resident launcher itself.
namespace serve_reason {
/// A command other than run without "run".
inline constexpr const char* kRunRequired = "run-required";
/// A command for a run that is not the current one (it has ended, or never was of this launcher).
inline constexpr const char* kRunChanged = "run-changed";
/// A run or prepare command while an operation is under way: nothing is queued.
inline constexpr const char* kBusy = "busy";
/// A run or prepare command after the launcher has been asked to end.
inline constexpr const char* kLauncherEnding = "launcher-ending";
/// A run command when no configuration is prepared or eligible.
inline constexpr const char* kNotPrepared = "not-prepared";
/// A run or prepare command after the launcher became contaminated.
inline constexpr const char* kContaminated = "contaminated";
/// A result query for a run that has not completed yet.
inline constexpr const char* kRunInProgress = "run-in-progress";
/// A result query for a run ID that is not retained (never existed or evicted).
inline constexpr const char* kRunNotFound = "run-not-found";
} // namespace serve_reason

/// A new launcher instance: 16 lowercase hexadecimal digits of a random 64-bit number. A run ID is
/// "<instance>-<n>" (n from 1): an opaque name that only tells one run from another. It is no
/// credential (a Control API client's Ping tells it to anyone).
[[nodiscard]] std::string new_launcher_instance();

/// The end of the launcher that has been asked for, and the path from a signal handler to the
/// current run's stop requests. Made once for the process (constinit), never destroyed.
///
/// request(), mark(), requested(), source() and count() touch only lock-free atomics, so a signal
/// handler may call request(). request() asks the end and then, if a run's StopRequests are
/// published, asks them too (the same source and count). The publisher (publish()) stores the
/// pointer and then reads requested(); all of it is seq_cst, so of a handler that is inside while a
/// run is published, either the handler sees the run or the publisher's reader sees the end.
/// withdraw() takes the pointer back and waits until no handler is inside (an in/out counter): the
/// StopRequests may be destroyed when it returns.
class LauncherEnd
{
public:
    constexpr LauncherEnd() noexcept = default;

    /// A signal or a console event (`source`: its number, or its code + 1): the end, and the
    /// current run's stop requests `count` times (1 as one Ctrl+C, 2 to skip the waits).
    void request(int source, int count = 1) noexcept
    {
        mark(source, count);
        inside_.fetch_add(1);
        if (runner::StopRequests* const run = run_.load())
        {
            run->request(source, count);
        }
        inside_.fetch_sub(1);
    }

    /// The end only: the caller asks the run itself (the end of input, a closed output).
    void mark(int source, int count = 1) noexcept
    {
        int none = 0;
        first_.compare_exchange_strong(none, source);
        count_.fetch_add(count);
    }

    [[nodiscard]] bool requested() const noexcept { return count_.load() > 0; }
    /// The source of the first request; 0 if none.
    [[nodiscard]] int source() const noexcept { return first_.load(); }
    [[nodiscard]] int count() const noexcept { return count_.load(); }

    /// The current run's stop requests, for request(); null: none.
    void publish(runner::StopRequests* run) noexcept { run_.store(run); }
    /// No run's stop requests any more; returns when no request() is inside.
    void withdraw() noexcept;

private:
    static_assert(std::atomic<runner::StopRequests*>::is_always_lock_free, "a signal handler needs lock-free atomics");
    static_assert(std::atomic<int>::is_always_lock_free, "a signal handler needs lock-free atomics");

    std::atomic<int> count_{0};
    std::atomic<int> first_{0};
    std::atomic<runner::StopRequests*> run_{nullptr};
    std::atomic<int> inside_{0};
};

/// Launcher-axis lifecycle state.
enum class LauncherState
{
    Idle,
    Preparing,
    Ready,
    Running,
    Ending,
    Contaminated,
};

[[nodiscard]] const char* launcher_state_name(LauncherState state) noexcept;

/// Whether a checked configuration can run inside the resident launcher.
struct HotEligibility
{
    bool eligible = false;
    /// "", "fixed-registry", "manual-participants", "trace-enabled", "config-unreadable", "usage"
    std::string reason{};
    std::string message{};

    [[nodiscard]] bool operator==(const HotEligibility&) const = default;
};

/// File-only metadata extracted during prepare().
struct ConfigMetadata
{
    std::vector<std::string> participant_names{};
    /// Named `network_names` so VehicleSilCheckDescribedNetworks.cmake is not triggered.
    std::vector<std::string> network_names{};
    std::vector<std::string> variants{};
    std::string file_sha256{};

    [[nodiscard]] bool operator==(const ConfigMetadata&) const = default;
};

/// Launcher-level capabilities for prepare() and start_run().
struct LauncherAllowed
{
    runner::Allowed prepare{};
    runner::Allowed run{};

    [[nodiscard]] bool operator==(const LauncherAllowed&) const = default;
};

/// Single Source of Truth snapshot for Launcher and active Run axes (excludes high-frequency TimeEvent).
struct ExecutionStatus
{
    std::string instance{};
    std::uint64_t status_rev = 0;
    LauncherState launcher_state = LauncherState::Idle;
    std::string config{};
    HotEligibility eligibility{};
    LauncherAllowed launcher_allowed{};
    std::optional<std::string> active_run_id{};
    std::optional<std::string> last_run_id{};
    std::optional<runner::RunState> run_state{};

    [[nodiscard]] bool operator==(const ExecutionStatus&) const = default;
};

struct PrepareSpec
{
    /// Empty -> re-verify current bound config; non-empty -> switch/verify target config (in-process C++ API).
    std::string config{};
    std::optional<std::string> variant{};
    std::vector<std::string> disable{};
    std::vector<std::string> enable{};
    std::vector<std::string> manual{};
};

struct PrepareResult
{
    bool accepted = false;
    /// Refusal reason when !accepted ("busy", "launcher-ending", "contaminated"); empty when accepted.
    std::string reason{};
    std::string message{};
    ExecutionStatus status{};
    ConfigMetadata metadata{};
    std::vector<config::Finding> findings{};
};

/// What a run of the resident launcher is asked to do.
struct PreparedRun
{
    RunRequest request{};
    /// What is wrong with the run command (its request file, a rule of validate_request()): the
    /// run starts nothing, and its result is "usage". Empty: none.
    std::vector<config::Finding> usage{};
};

struct RunSubmission
{
    RunIntent intent{};
    config::RequestOrigin origin{};
    std::string timeline{};
    std::optional<std::int64_t> command_id{};
    std::vector<config::Finding> pre_usage{};
    /// Optional per-submission hook (used by Serve adapter for ServeOptions::prepare / prepare_serve_run),
    /// invoked inside start_run() under mutex_ after gate check and ++seq_, before end_.publish().
    std::function<PreparedRun(const RunRequest& base, const std::string& run_id)> prepare_run{};
    std::string source = "api";
};

[[nodiscard]] PreparedRun prepare_execution_run(const RunRequest& base, const RunSubmission& submission,
                                                const std::string& run_id);

enum class StopMode
{
    Graceful, ///< Idempotent raise_to(source, 1)
    Force,    ///< Idempotent raise_to(source, 2)
    Escalate, ///< Backward-compatible bare stop: 0->1, 1->2, >=2 stays 2
};

struct CommandReply
{
    bool accepted = false;
    bool pending = false;
    std::string reason{};
    std::string message{};
    std::optional<std::string> run_id{};
    int stop_requests = 0;
};

struct ResultQuery
{
    bool ok = false;
    /// "", "run-in-progress", "run-not-found"
    std::string reason{};
    std::string run_id{};
    bool launcher_ends = false;
    /// Bounded to last completed run; ended_control_server and timeline_record already released.
    std::shared_ptr<const RunReport> report{};
};

/// How the launcher ends (ExecutionRuntime::run_loop()).
struct ExecutionEnd
{
    int code = 0;
    /// End the process at once, without destructors: a thread of the run may still be closing.
    bool at_once = false;
};

struct ExecutionRuntimeOptions
{
    using RunFunction =
        std::function<RunReport(const RunRequest& request, const runner::RunObservers& observers, const RunLent& lent)>;
    using PrepareFunction =
        std::function<PreparedRun(const RunRequest& base, const RunSubmission& submission, const std::string& run_id)>;

    RunRequest base{};
    std::string instance{};
    RunFunction run{};
    PrepareFunction prepare{};
    std::function<std::string(int source)> source_name{};
    std::chrono::milliseconds flush_limit{5000};
    /// Injected by caller (e.g. tools/launch/main.cpp) so libs/launch never links vehicle_sil::trace.
    bool trace_enabled = false;
    bool refuse_fixed_registry = true;
};

/// Transport-agnostic lifecycle sink for ExecutionRuntime.
/// Implementations MUST NOT call back into ExecutionRuntime (leaf lock only).
class ExecutionSink
{
public:
    virtual ~ExecutionSink() = default;
    /// Called only on Launcher-axis transitions (outside ExecutionRuntime::mutex_).
    virtual void on_status(const ExecutionStatus& status) = 0;
    /// Called under ExecutionRuntime::mutex_ before busy_ = false (ordering idle before next run_start).
    virtual void on_idle() = 0;
    virtual void on_refused(const runner::Command& command, std::string_view source,
                            const std::optional<std::string>& asked_run) = 0;
    virtual std::unique_ptr<RunEventSink> open_run(const std::string& run_id,
                                                   std::optional<std::int64_t> command_id) = 0;
    virtual bool finish_run(RunEventSink& window, const RunReport& report, bool launcher_ends,
                            const std::function<bool()>& give_up) = 0;
    [[nodiscard]] virtual bool closed() const = 0;
    virtual bool wait_flushed(std::chrono::milliseconds timeout) = 0;
};

/// Transport-agnostic resident execution runtime: manages Prepare, Run, Stop, Time commands,
/// Status, Results, and independent Shutdown over a single RunEventSink per run.
class ExecutionRuntime
{
public:
    ExecutionRuntime(ExecutionSink& sink, LauncherEnd& end, ExecutionRuntimeOptions options);
    ~ExecutionRuntime();
    ExecutionRuntime(const ExecutionRuntime&) = delete;
    ExecutionRuntime& operator=(const ExecutionRuntime&) = delete;

    [[nodiscard]] PrepareResult prepare(const PrepareSpec& spec);
    [[nodiscard]] CommandReply start_run(RunSubmission submission);
    [[nodiscard]] CommandReply stop_run(const std::string& run_id, StopMode mode, std::string_view source = "api",
                                        std::optional<std::int64_t> command_id = std::nullopt);
    [[nodiscard]] CommandReply send_time_command(const std::string& run_id, const runner::Command& command,
                                                 std::string_view source = "api");
    [[nodiscard]] ExecutionStatus status() const;
    [[nodiscard]] ResultQuery result(const std::optional<std::string>& run_id = std::nullopt) const;

    void request_shutdown(int stop_source);
    [[nodiscard]] ExecutionEnd run_loop();

private:
    struct ActiveRun;

    [[nodiscard]] LauncherAllowed allowed_locked() const;
    [[nodiscard]] ExecutionStatus snapshot_locked(runner::RunControls* controls) const;
    void refuse_locked(const runner::Command& command, std::string_view source, const char* reason,
                       std::string message, const std::optional<std::string>& asked_run);
    void drain_statuses_locked(std::unique_lock<std::mutex>& lock);
    void flush();

    ExecutionSink& sink_;
    LauncherEnd& end_;
    ExecutionRuntimeOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<ExecutionStatus> pending_statuses_{};
    bool status_emitting_ = false;
    std::string config_;
    bool has_prepared_ = false;
    HotEligibility eligibility_{};
    ConfigMetadata metadata_{};
    LauncherState state_ = LauncherState::Idle;
    std::uint64_t status_rev_ = 0;
    int in_flight_prepare_ = 0;

    std::uint64_t seq_ = 0;
    bool busy_ = false;
    bool ending_ = false;
    bool input_ended_in_run_ = false;
    ActiveRun* current_ = nullptr;
    std::unique_ptr<ActiveRun> handed_;
    std::unique_ptr<ActiveRun> contaminated_run_;

    std::optional<std::string> active_run_id_{};
    std::optional<std::string> last_run_id_{};
    bool last_launcher_ends_ = false;
    std::shared_ptr<const RunReport> last_result_{};
};

} // namespace vehicle_sil::launch
