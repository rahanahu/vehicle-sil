// SPDX-License-Identifier: MIT
//
// vehicle_sil_vector_xl_bridge: forwards classic CAN frames in both directions between a SIL Kit
// CAN network and a Vector XL CAN channel (Vector Virtual CAN or CAN hardware such as CANcase).
//
// Exit codes: 0 stopped normally (Ctrl+C / SIGTERM) or --list-channels/--help done,
//             1 the SIL Kit participant failed while running (e.g. connection lost),
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
        runtime = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{options.participant, options.registry_uri});
        auto& controller = *runtime->participant().CreateCanController("VectorXlBridge", options.network);

        XlCanPortOptions portOptions;
        portOptions.bitrate = options.bitrate;
        portOptions.rx_queue_size = options.rx_queue_size;
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

    log(kTag, "bridging SIL Kit network '%s' (participant '%s', %s) <-> XL '%s'; Ctrl+C to stop", options.network.c_str(),
        options.participant.c_str(), options.registry_uri.c_str(), channel.name.c_str());

    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_stop_signal);
#endif
    int result = kExitOk;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    log(kTag, "stopping");
    bridge->stop();
    runtime->stop("bridge stopped");
    log(kTag, "%s", format_statistics(bridge->statistics()).c_str());
    if (const auto error = bridge->last_error(); !error.empty())
    {
        log(kTag, "last error: %s", error.c_str());
    }
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
    try
    {
        return run(options);
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
        return kExitSetupFailed;
    }
}
