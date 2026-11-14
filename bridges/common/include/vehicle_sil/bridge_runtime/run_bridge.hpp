// SPDX-License-Identifier: MIT
//
// The part of a bridge executable that runs after setup: stop handling, the monitor loop, the
// order of stopping, and the exit code. One place for every bridge executable.
#pragma once

#include <silkit/services/orchestration/OrchestrationDatatypes.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil {
class ParticipantRuntime;
}

namespace vehicle_sil::bridge_runtime {

/// What run_bridge() needs of a bridge. All calls come from the thread that runs run_bridge().
class BridgeHooks
{
public:
    virtual ~BridgeHooks() = default;

    /// Why the external side stopped working (device removed, interface gone), or nullopt. A
    /// fault ends the run: "FAIL: <fault>", exit code 1.
    [[nodiscard]] virtual std::optional<std::string> fault() = 0;
    /// The warnings since the last call, each printed as "warning: <text>". Asked while the bridge
    /// runs and once more after stop(), before the statistics.
    [[nodiscard]] virtual std::vector<std::string> take_warnings() = 0;
    /// Called once per health interval (RunBridgeOptions::health_interval): the monitor line to
    /// print, or nullopt when there is nothing new to say.
    [[nodiscard]] virtual std::optional<std::string> health_line() = 0;
    /// Stops forwarding in both directions. Once it returns, final_statistics() is final.
    virtual void stop() noexcept = 0;
    /// The lines printed after stop() and before the SIL Kit participant stops: the statistics
    /// line, then e.g. "last error: ...".
    [[nodiscard]] virtual std::vector<std::string> final_statistics() = 0;
};

/// The SIL Kit participant as run_bridge() sees it. run_bridge(ParticipantRuntime&, ...) adapts a
/// ParticipantRuntime; tests can give a participant whose states they script.
class BridgeParticipant
{
public:
    virtual ~BridgeParticipant() = default;
    [[nodiscard]] virtual SilKit::Services::Orchestration::ParticipantState state() const = 0;
    /// Stops the lifecycle and waits for it within the participant's own limit.
    virtual void stop(std::string_view reason) noexcept = 0;
};

/// How the monitor loop judges one state of the bridge's participant (judge_participant_state()).
enum class ParticipantVerdict
{
    Starting, ///< still on its way to Running: not a failure (up to RunBridgeOptions::starting_limit)
    Running,  ///< Running (or Paused): the bridge works
    Ended,    ///< stopping or ended (Stopping, Stopped, Error, ShuttingDown, Shutdown, Aborting): failure
};

/// The one rule for what a state of the participant means to a bridge that runs in real time,
/// outside time synchronisation: only stopping and ended states are failures.
[[nodiscard]] ParticipantVerdict judge_participant_state(SilKit::Services::Orchestration::ParticipantState state) noexcept;

struct RunBridgeOptions
{
    /// The prefix of every line: "[<tag>] ...".
    std::string tag = "bridge";
    /// Printed once the stop handlers are in place; it should end with "Ctrl+C to stop".
    std::string ready_line;
    /// How often hooks.health_line() is asked.
    std::chrono::milliseconds health_interval{1000};
    /// How often the loop looks at the participant, the fault and the warnings.
    std::chrono::milliseconds poll_interval{50};
    /// While the participant is still starting this long, the loop says once what it waits for.
    std::chrono::milliseconds starting_notice_after{1000};
    /// A participant still starting this long after run_bridge() began fails the run (exit code 1).
    std::chrono::milliseconds starting_limit{15000};
    /// The reason given to the participant's stop().
    std::string stop_reason = "bridge stopped";
    /// Install the stop handlers: SIGINT, SIGTERM, SIGBREAK where it exists, and on Windows the
    /// console's close, logoff and shutdown events. Without them, only request_stop() stops
    /// the loop (tests).
    bool handle_stop_signals = true;
    /// Where the lines go; by default to stdout as "[<tag>] <line>", flushed.
    std::function<void(const std::string& line)> print;
};

/// Runs a bridge whose setup is done (forwarding started, participant started) until a stop is
/// requested or something fails, then stops it in this order:
///   "stopping", hooks.stop(), hooks.final_statistics(), participant.stop(), "stopped".
/// The statistics come before the SIL Kit shutdown, which may take its time (and a closed
/// console may not grant it).
/// @returns 0 (kExitOk) after a stop request, 1 (kExitFailed) if the participant stopped or ended
/// on its own, did not reach Running within the starting limit, or hooks.fault() reported one.
/// Call it once at a time in a process; stop requests before it began are forgotten.
[[nodiscard]] int run_bridge(BridgeParticipant& participant, BridgeHooks& hooks, const RunBridgeOptions& options);

/// run_bridge() with a ParticipantRuntime (started) as the participant.
[[nodiscard]] int run_bridge(ParticipantRuntime& runtime, BridgeHooks& hooks, const RunBridgeOptions& options);

/// Asks a running run_bridge() to stop, as a stop signal does. Async-signal-safe; any thread.
void request_stop() noexcept;

/// Call at the end of main() with the process's exit code. On Windows, a console close that is
/// waiting for the bridge to stop (at most 4 s) then ends the process with this code; returning
/// from the console handler would end it with STATUS_CONTROL_C_EXIT instead. Elsewhere it does
/// nothing.
void finish_console_close(int exit_code) noexcept;

} // namespace vehicle_sil::bridge_runtime
