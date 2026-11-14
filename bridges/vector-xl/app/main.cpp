// SPDX-License-Identifier: MIT
//
// vehicle_sil_vector_xl_bridge: forwards classic CAN frames in both directions between a SIL Kit
// CAN network and a Vector XL CAN channel (Vector Virtual CAN or CAN hardware such as CANcase).
//
// Exit codes: 0 stopped normally (Ctrl+C / Ctrl+Break / SIGTERM / console closed) or
//               --list-channels/--help done,
//             1 failed while running: the SIL Kit participant stopped or ended on its own (e.g.
//               connection lost) or the XL channel stopped working (e.g. device removed),
//             3 setup failed (XL driver, channel selection, SIL Kit), 4 bad command line.
#include "vehicle_sil/bridge_runtime/run_bridge.hpp"
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/vector_xl/bridge_options.hpp"
#include "vehicle_sil/vector_xl/can_bridge.hpp"
#include "vehicle_sil/vector_xl/channel_selection.hpp"
#include "vehicle_sil/vector_xl/xl_driver.hpp"

#include <silkit/services/can/ICanController.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace vehicle_sil;
using namespace vehicle_sil::vector_xl;
using cli::log;

using cli::kExitOk;
using cli::kExitSetupFailed;
using cli::kExitUsage;

constexpr const char* kTag = "bridge";

/// Upper bound for the SIL Kit lifecycle to shut down when the bridge stops.
constexpr auto kSilKitShutdownTimeout = std::chrono::milliseconds(2000);

/// What run_bridge() asks of the CAN bridge.
class XlBridgeHooks final : public bridge_runtime::BridgeHooks
{
public:
    explicit XlBridgeHooks(CanBridge& bridge) : bridge_(bridge), reported_(bridge.health()) {}

    std::optional<std::string> fault() override { return bridge_.fault(); }
    std::vector<std::string> take_warnings() override { return bridge_.take_warnings(); }
    std::optional<std::string> health_line() override
    {
        const CanBridgeHealth now = bridge_.health();
        auto line = format_health_change(reported_, now, problems_before_);
        problems_before_ = problems_grew(reported_.statistics, now.statistics);
        reported_ = now;
        return line;
    }
    void stop() noexcept override { bridge_.stop(); }
    std::vector<std::string> final_statistics() override
    {
        std::vector<std::string> lines{format_statistics(bridge_.statistics())};
        if (auto error = bridge_.last_error(); !error.empty())
        {
            lines.push_back("last error: " + error);
        }
        return lines;
    }

private:
    CanBridge& bridge_;
    CanBridgeHealth reported_;
    bool problems_before_ = false;
};

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
        // Bounded so that stopping fits in the time Windows grants a closed console (run_bridge.hpp).
        runtimeOptions.shutdown_timeout = kSilKitShutdownTimeout;
        runtime = std::make_unique<ParticipantRuntime>(runtimeOptions);
        auto& controller = *runtime->participant().CreateCanController("VectorXlBridge", options.network);

        XlCanPortOptions portOptions;
        portOptions.bitrate = options.bitrate;
        portOptions.rx_queue_size = options.rx_queue_size;
        portOptions.transmit_timeout = options.tx_timeout;
        if (options.bus_off_restart)
        {
            portOptions.bus_off_restart = *options.bus_off_restart;
        }
        if (!options.bus_monitor)
        {
            portOptions.bus_state_interval = std::chrono::milliseconds{0};
        }
        port = std::make_unique<XlCanPort>(*driver, channel, portOptions);
        log(kTag, "XL port open, init access: %s%s", port->has_init_access() ? "yes" : "no",
            options.bitrate ? (", bit rate " + std::to_string(*options.bitrate)).c_str() : ", bit rate unchanged");

        // Start forwarding before the lifecycle reaches Running: peers may start sending as soon as
        // they see this participant Running, and the SIL Kit handler must be ready by then. The
        // controller is started where SIL Kit wants it, before Running.
        bridge = std::make_unique<CanBridge>(controller, *port);
        runtime->add_communication_ready_handler([b = bridge.get()] { b->start_controller(); });
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
    if (options.bus_off_restart && options.bus_off_restart->count() > 0 && !port->has_init_access())
    {
        log(kTag, "warning: --bus-off-restart-ms is not used: it needs init access (--bitrate)");
    }
    bridge_runtime::RunBridgeOptions runOptions;
    runOptions.tag = kTag;
    runOptions.ready_line = "bridging SIL Kit network '" + options.network + "' (participant '" + options.participant
                            + "', " + options.registry_uri + ") <-> XL '" + channel.name + "'; Ctrl+C to stop";
    XlBridgeHooks hooks(*bridge);
    return bridge_runtime::run_bridge(*runtime, hooks, runOptions);
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
    bridge_runtime::finish_console_close(code);
    return code;
}
