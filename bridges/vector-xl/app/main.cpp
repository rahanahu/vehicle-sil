// SPDX-License-Identifier: MIT
//
// vehicle_sil_vector_xl_bridge: forwards classic CAN frames in both directions between a SIL Kit
// CAN network and a Vector XL CAN channel (Vector Virtual CAN or CAN hardware such as CANcase).
//
// Exit codes: 0 stopped normally (Ctrl+C / Ctrl+Break / SIGTERM / console closed) or
//               --list-channels/--help done,
//             1 failed while running: the SIL Kit participant left Running (e.g. connection
//               lost) or the XL channel stopped working (e.g. device removed),
//             3 setup failed (XL driver, channel selection, SIL Kit), 4 bad command line.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/vector_xl/bridge_options.hpp"
#include "vehicle_sil/vector_xl/can_bridge.hpp"
#include "vehicle_sil/vector_xl/channel_selection.hpp"
#include "vehicle_sil/vector_xl/xl_driver.hpp"

#include <silkit/services/can/ICanController.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using namespace vehicle_sil;
using namespace vehicle_sil::vector_xl;
using cli::log;

using cli::kExitOk;
using cli::kExitSetupFailed;
using cli::kExitUsage;

constexpr const char* kTag = "bridge";

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void on_stop_signal(int)
{
    g_stop_requested = 1;
}

/// How often the main loop reports a change of the external bus (format_health_change()).
constexpr auto kHealthInterval = std::chrono::seconds(1);
/// Upper bound for the SIL Kit lifecycle to shut down when the bridge stops.
constexpr auto kSilKitShutdownTimeout = std::chrono::milliseconds(2000);

#ifdef _WIN32
// Closing the console window (and logoff, shutdown) ends the process as soon as the console
// control handler returns, and the C runtime's handler for SIGBREAK returns at once. This
// handler asks the main loop to stop, waits for run() to finish (statistics printed) within
// the about 5 s Windows grants, and then ends the process itself with run()'s exit code
// (returning from the handler would end it with STATUS_CONTROL_C_EXIT instead).
HANDLE g_stopped_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
std::atomic<int> g_exit_code{kExitOk};

BOOL WINAPI on_console_close(DWORD event)
{
    if (event != CTRL_CLOSE_EVENT && event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT)
    {
        return FALSE; // Ctrl+C and Ctrl+Break: the C runtime's signal handlers
    }
    g_stop_requested = 1;
    if (g_stopped_event != nullptr && WaitForSingleObject(g_stopped_event, 4000) == WAIT_OBJECT_0)
    {
        ExitProcess(static_cast<UINT>(g_exit_code.load()));
    }
    return TRUE;
}

/// Called with run()'s exit code: lets a waiting on_console_close() end the process with it.
void finish_console_close(int code)
{
    g_exit_code = code;
    if (g_stopped_event != nullptr)
    {
        (void)SetEvent(g_stopped_event);
    }
}
#else
void finish_console_close(int /*code*/) {}
#endif

int run(const BridgeOptions& options)
{
    // Declaration order is destruction order in reverse: the bridge goes first, then the XL
    // port, the SIL Kit participant (which owns the CAN controller) and the XL driver.
    std::unique_ptr<XlDriver> driver;
    try
    {
        driver = std::make_unique<XlDriver>();
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: cannot open the Vector XL driver: %s", e.what());
        return kExitSetupFailed;
    }

    // Errors from here on are setup failures that main() reports.
    const std::vector<XlChannelInfo> channels = driver->channels();

    if (options.list_channels)
    {
        std::fputs(format_channel_list(channels).c_str(), stdout);
        return kExitOk;
    }

    const XlChannelInfo channel = select_channel(channels, options.target, options.channel);
    log(kTag, "XL channel: %s %u = \"%s\" (hwType=%u, serial=%u)", std::string(to_string(options.target)).c_str(),
        options.channel, channel.name.c_str(), channel.hw_type, channel.serial_number);

    std::unique_ptr<ParticipantRuntime> runtime;
    std::unique_ptr<XlCanPort> port;
    std::unique_ptr<CanBridge> bridge;
    try
    {
        ParticipantRuntimeOptions runtimeOptions{options.participant, options.registry_uri};
        // Bounded so that stopping fits in the time Windows grants a closed console (on_console_close).
        runtimeOptions.shutdown_timeout = kSilKitShutdownTimeout;
        runtime = std::make_unique<ParticipantRuntime>(runtimeOptions);
        auto& controller = *runtime->participant().CreateCanController("VectorXlBridge", options.network);

        XlCanPortOptions portOptions;
        portOptions.bitrate = options.bitrate;
        portOptions.rx_queue_size = options.rx_queue_size;
        portOptions.transmit_timeout = options.tx_timeout;
        if (!options.bus_monitor)
        {
            portOptions.bus_state_interval = std::chrono::milliseconds{0};
        }
        port = std::make_unique<XlCanPort>(*driver, channel, portOptions);
        log(kTag, "XL port open, init access: %s%s", port->has_init_access() ? "yes" : "no",
            options.bitrate ? (", bit rate " + std::to_string(*options.bitrate)).c_str() : ", bit rate unchanged");

        // Start forwarding before the lifecycle reaches Running: peers may start sending as soon as
        // they see this participant Running, and the SIL Kit handler and controller must be ready
        // by then (frames sent to a controller that is not started are lost).
        bridge = std::make_unique<CanBridge>(controller, *port);
        bridge->start();
        runtime->start(std::chrono::seconds(15));
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
        return kExitSetupFailed;
    }

    if (options.tx_timeout && !port->has_init_access())
    {
        log(kTag, "warning: --tx-timeout-ms is not used: it needs init access (--bitrate) to switch the "
                  "transmit receipts on");
    }
    log(kTag, "bridging SIL Kit network '%s' (participant '%s', %s) <-> XL '%s'; Ctrl+C to stop", options.network.c_str(),
        options.participant.c_str(), options.registry_uri.c_str(), channel.name.c_str());

    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_stop_signal);
#endif
#ifdef _WIN32
    // Registered after the signal handlers so that Windows asks this handler first.
    (void)SetConsoleCtrlHandler(on_console_close, TRUE);
#endif
    int result = kExitOk;
    CanBridgeHealth reported = bridge->health();
    bool problemsBefore = false;
    auto nextReport = std::chrono::steady_clock::now() + kHealthInterval;
    while (g_stop_requested == 0)
    {
        // The SIL Kit side can fail while running (registry gone, connection lost).
        if (const auto state = runtime->state(); state != SilKit::Services::Orchestration::ParticipantState::Running)
        {
            log(kTag, "FAIL: SIL Kit participant left Running (state %s)",
                SilKit::Services::Orchestration::to_string(state).c_str());
            result = cli::kExitFailed;
            break;
        }
        // The XL side can fail while running (device removed): nothing would be forwarded any more.
        if (const auto fault = bridge->fault())
        {
            log(kTag, "FAIL: %s", fault->c_str());
            result = cli::kExitFailed;
            break;
        }
        for (const auto& warning : bridge->take_warnings())
        {
            log(kTag, "warning: %s", warning.c_str());
        }
        if (std::chrono::steady_clock::now() >= nextReport)
        {
            nextReport += kHealthInterval;
            const CanBridgeHealth now = bridge->health();
            if (const auto line = format_health_change(reported, now, problemsBefore))
            {
                log(kTag, "%s", line->c_str());
            }
            problemsBefore = problems_grew(reported.statistics, now.statistics);
            reported = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    log(kTag, "stopping");
    bridge->stop();
    // The statistics are final once the bridge stopped: print them before the SIL Kit shutdown,
    // which may take its time (and a closed console may not grant it).
    log(kTag, "%s", format_statistics(bridge->statistics()).c_str());
    if (const auto error = bridge->last_error(); !error.empty())
    {
        log(kTag, "last error: %s", error.c_str());
    }
    runtime->stop("bridge stopped");
    log(kTag, "stopped");
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    BridgeOptions options;
    try
    {
        options = parse_bridge_options(std::vector<std::string_view>(argv + 1, argv + argc));
    }
    catch (const BridgeUsageError& e)
    {
        std::fprintf(stderr, "error: %s\n\n%s", e.what(), bridge_usage().c_str());
        return kExitUsage;
    }
    if (options.help)
    {
        std::fputs(bridge_usage().c_str(), stdout);
        return kExitOk;
    }
    int code = kExitSetupFailed;
    try
    {
        code = run(options);
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
    }
    finish_console_close(code);
    return code;
}
