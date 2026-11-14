// SPDX-License-Identifier: MIT
#include "vehicle_sil/bridge_runtime/run_bridge.hpp"

#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <atomic>
#include <csignal>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vehicle_sil::bridge_runtime {
namespace {

namespace orch = SilKit::Services::Orchestration;

// Set by the stop handlers (signal handlers, the Windows console handler's thread) and
// request_stop(); lock-free, so that a signal handler may set it.
std::atomic<int> g_stop_requested{0};
static_assert(std::atomic<int>::is_always_lock_free);

extern "C" void on_stop_signal(int)
{
    g_stop_requested.store(1);
}

#ifdef _WIN32
// Closing the console window (and logoff, shutdown) ends the process as soon as the console
// control handler returns, and the C runtime's handler for SIGBREAK returns at once. This
// handler asks the loop to stop, waits for main() to finish (statistics printed) within the
// about 5 s Windows grants, and then ends the process itself with main()'s exit code
// (returning from the handler would end it with STATUS_CONTROL_C_EXIT instead).
HANDLE g_stopped_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
std::atomic<int> g_exit_code{cli::kExitOk};

BOOL WINAPI on_console_close(DWORD event)
{
    if (event != CTRL_CLOSE_EVENT && event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT)
    {
        return FALSE; // Ctrl+C and Ctrl+Break: the C runtime's signal handlers
    }
    g_stop_requested.store(1);
    if (g_stopped_event != nullptr && WaitForSingleObject(g_stopped_event, 4000) == WAIT_OBJECT_0)
    {
        ExitProcess(static_cast<UINT>(g_exit_code.load()));
    }
    return TRUE;
}
#endif

void install_stop_handlers()
{
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_stop_signal);
#endif
#ifdef _WIN32
    // Registered after the signal handlers so that Windows asks this handler first.
    (void)SetConsoleCtrlHandler(on_console_close, TRUE);
#endif
}

class RuntimeParticipant final : public BridgeParticipant
{
public:
    explicit RuntimeParticipant(ParticipantRuntime& runtime) : runtime_(runtime) {}
    [[nodiscard]] orch::ParticipantState state() const override { return runtime_.state(); }
    void stop(std::string_view reason) noexcept override { runtime_.stop(reason); }

private:
    ParticipantRuntime& runtime_;
};

} // namespace

ParticipantVerdict judge_participant_state(orch::ParticipantState state) noexcept
{
    switch (state)
    {
    case orch::ParticipantState::Running:
    case orch::ParticipantState::Paused: // only the participant itself could pause it
        return ParticipantVerdict::Running;
    case orch::ParticipantState::Stopping:
    case orch::ParticipantState::Stopped:
    case orch::ParticipantState::Error:
    case orch::ParticipantState::ShuttingDown:
    case orch::ParticipantState::Shutdown:
    case orch::ParticipantState::Aborting:
        return ParticipantVerdict::Ended;
    case orch::ParticipantState::Invalid:
    case orch::ParticipantState::ServicesCreated:
    case orch::ParticipantState::CommunicationInitializing:
    case orch::ParticipantState::CommunicationInitialized:
    case orch::ParticipantState::ReadyToRun:
        // The start is still on its way (a participant other than a ParticipantRuntime after
        // its start() can be seen here).
        return ParticipantVerdict::Starting;
    }
    // No `default:` above, so that a state that SIL Kit adds fails the build (-Werror=switch)
    // until it is judged there. A value that is no state at all: nothing to run a bridge on.
    return ParticipantVerdict::Ended;
}

int run_bridge(BridgeParticipant& participant, BridgeHooks& hooks, const RunBridgeOptions& options)
{
    const auto print = [&options](const std::string& line) {
        if (options.print)
        {
            options.print(line);
        }
        else
        {
            cli::log(options.tag, "%s", line.c_str());
        }
    };

    g_stop_requested.store(0);
    // Before the line that says "Ctrl+C to stop": from then on, a stop request stops the bridge
    // in order (statistics, exit code 0).
    if (options.handle_stop_signals)
    {
        install_stop_handlers();
    }
    print(options.ready_line);

    int result = cli::kExitOk;
    const auto began = std::chrono::steady_clock::now();
    auto nextReport = began + options.health_interval;
    bool reachedRunning = false;
    bool noticedStarting = false;
    while (g_stop_requested.load() == 0)
    {
        // The SIL Kit side can fail while running (registry gone, connection lost).
        const auto state = participant.state();
        const auto verdict = judge_participant_state(state);
        if (verdict == ParticipantVerdict::Ended)
        {
            print(std::string(reachedRunning ? "FAIL: SIL Kit participant left Running (state "
                                             : "FAIL: SIL Kit participant stopped before it reached Running (state ")
                  + orch::to_string(state) + ")");
            result = cli::kExitFailed;
            break;
        }
        if (verdict == ParticipantVerdict::Running)
        {
            reachedRunning = true;
        }
        else if (!reachedRunning)
        {
            const auto waited = std::chrono::steady_clock::now() - began;
            if (waited >= options.starting_limit)
            {
                print("FAIL: SIL Kit participant did not reach Running within "
                      + std::to_string(options.starting_limit.count()) + " ms (state "
                      + orch::to_string(state) + "); its SIL Kit log may say why");
                result = cli::kExitFailed;
                break;
            }
            if (!noticedStarting && waited >= options.starting_notice_after)
            {
                noticedStarting = true;
                print("waiting for the SIL Kit participant to reach Running (state " + orch::to_string(state)
                      + "; at most " + std::to_string(options.starting_limit.count()) + " ms)");
            }
        }
        // The external side can fail while running (device removed): nothing would be forwarded any more.
        if (const auto fault = hooks.fault())
        {
            print("FAIL: " + *fault);
            result = cli::kExitFailed;
            break;
        }
        for (const auto& warning : hooks.take_warnings())
        {
            print("warning: " + warning);
        }
        if (std::chrono::steady_clock::now() >= nextReport)
        {
            nextReport += options.health_interval;
            if (const auto line = hooks.health_line())
            {
                print(*line);
            }
        }
        std::this_thread::sleep_for(options.poll_interval);
    }

    print("stopping");
    hooks.stop();
    // What the bridge had to say since the loop's last look (a warning raised just before the
    // stop, or by the stop itself) is not lost with it.
    for (const auto& warning : hooks.take_warnings())
    {
        print("warning: " + warning);
    }
    // The statistics are final once the bridge stopped: print them before the SIL Kit shutdown,
    // which may take its time (and a closed console may not grant it).
    for (const auto& line : hooks.final_statistics())
    {
        print(line);
    }
    participant.stop(options.stop_reason);
    print("stopped");
    return result;
}

int run_bridge(ParticipantRuntime& runtime, BridgeHooks& hooks, const RunBridgeOptions& options)
{
    RuntimeParticipant participant(runtime);
    return run_bridge(participant, hooks, options);
}

void request_stop() noexcept
{
    g_stop_requested.store(1);
}

void finish_console_close([[maybe_unused]] int exit_code) noexcept
{
#ifdef _WIN32
    g_exit_code = exit_code;
    if (g_stopped_event != nullptr)
    {
        (void)SetEvent(g_stopped_event);
    }
#endif
}

} // namespace vehicle_sil::bridge_runtime
