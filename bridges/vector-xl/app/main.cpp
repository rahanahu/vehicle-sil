// SPDX-License-Identifier: MIT
//
// vehicle_sil_vector_xl_bridge: forwards classic CAN frames in both directions between a SIL Kit
// CAN network and a Vector XL CAN channel (Vector Virtual CAN or CAN hardware such as CANcase).
//
// Exit codes: 0 stopped normally (Ctrl+C / Ctrl+Break / SIGTERM / console closed) or
//               --list-channels/--describe/--help done,
//             1 failed while running: the SIL Kit participant stopped or ended on its own (e.g.
//               connection lost) or the XL channel stopped working (e.g. device removed),
//             3 setup failed (XL driver, channel selection, SIL Kit), 4 bad command line.
#include "vehicle_sil/bridge_runtime/describe.hpp"
#include "vehicle_sil/bridge_runtime/link_line.hpp"
#include "vehicle_sil/bridge_runtime/nothing_came_hint.hpp"
#include "vehicle_sil/bridge_runtime/registry_failure.hpp"
#include "vehicle_sil/bridge_runtime/run_bridge.hpp"
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/cli/utf8.hpp"
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
#include <stdexcept>
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
    /// `channel`: what the start line names ("XL 'Virtual Channel 1'").
    XlBridgeHooks(CanBridge& bridge, const std::string& channel) :
      bridge_(bridge),
      reported_(bridge.health()),
      nothing_came_([channel] { return nothing_came_warning(channel, bridge_runtime::kNothingCameAfter); })
    {
    }

    std::optional<std::string> fault() override { return bridge_.fault(); }
    std::vector<std::string> take_warnings() override
    {
        auto warnings = bridge_.take_warnings();
        const auto counts = bridge_.statistics();
        // The receipts of the bridge's own frames are not in external_to_silkit (own_tx_dropped).
        if (auto hint = nothing_came_.look(counts.silkit_to_external, counts.external_to_silkit, std::chrono::steady_clock::now()))
        {
            warnings.push_back(std::move(*hint));
        }
        return warnings;
    }
    std::optional<std::string> link_line() override { return bridge_runtime::format_link_line("bus", link_line_value(bridge_.health().bus_state)); }
    std::string statistics_line() override { return format_statistics(bridge_.statistics()); }
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
        std::vector<std::string> lines{statistics_line()};
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
    bridge_runtime::NothingCameHint nothing_came_;
};

/// The participant, connected to the registry. A registry that does not answer is the usual first
/// failure: say where the bridge looked and what to do, not only SIL Kit's text.
std::unique_ptr<ParticipantRuntime> connect_to_registry(const ParticipantRuntimeOptions& options)
{
    try
    {
        return std::make_unique<ParticipantRuntime>(options);
    }
    catch (const std::exception& e)
    {
        if (auto message = bridge_runtime::registry_failure_message(e.what(), options.registry_uri))
        {
            throw std::runtime_error(*message);
        }
        throw;
    }
}

int run(const BridgeOptions& options)
{
    if (options.describe)
    {
        // Without the XL driver: vehicle_sil_launch check asks this on computers where the bridge
        // will not run.
        const bridge_runtime::BridgeDescription description{options.participant, options.network,
                                                            bridge_runtime::BridgedNetworkKind::Can};
        if (!cli::write_text(stdout, bridge_runtime::describe_json(description)))
        {
            log(kTag, "FAIL setup: could not write the description to stdout (is it closed?)");
            return kExitSetupFailed;
        }
        return kExitOk;
    }

    // Declaration order is destruction order in reverse: the bridge goes first, then the XL
    // port, the SIL Kit participant (which owns the CAN controller) and the XL driver.
    std::unique_ptr<XlDriver> driver;
    try
    {
        driver = std::make_unique<XlDriver>();
    }
    catch (const std::exception& e)
    {
        const std::string message = std::string("cannot open the Vector XL driver: ") + e.what();
        if (options.json)
        {
            // stdout holds the JSON alone, for programs; the FAIL line goes to stderr, for people.
            cli::write_text(stdout, channels_error_json("driver-missing", message));
            cli::print(stderr, "[%s] FAIL setup: %s\n", kTag, message.c_str());
        }
        else
        {
            log(kTag, "FAIL setup: %s", message.c_str());
        }
        return kExitSetupFailed;
    }

    // Errors from here on are setup failures that main() reports.
    if (options.list_channels)
    {
        std::string list;
        try
        {
            const std::vector<XlChannelInfo> channels = driver->channels();
            list = options.json ? channels_json(channels) : format_channel_list(channels);
        }
        catch (const std::exception& e)
        {
            if (!options.json)
            {
                throw; // main() says FAIL
            }
            cli::write_text(stdout, channels_error_json("failed", e.what()));
            cli::print(stderr, "[%s] FAIL setup: %s\n", kTag, e.what());
            return kExitSetupFailed;
        }
        cli::write_text(stdout, list);
        return kExitOk;
    }
    const std::vector<XlChannelInfo> channels = driver->channels();

    const XlChannelInfo channel = select_channel(channels, options.target, options.channel);
    log(kTag, "XL channel: %s %u = \"%s\" (hwType=%u, serial=%u)", std::string(to_string(options.target)).c_str(),
        options.channel, channel.name.c_str(), channel.hw_type, channel.serial_number);

    std::unique_ptr<ParticipantRuntime> runtime;
    std::unique_ptr<XlCanPort> port;
    std::unique_ptr<CanBridge> bridge;
    std::string firstLinkLine;
    try
    {
        ParticipantRuntimeOptions runtimeOptions{options.participant, options.registry_uri};
        // Bounded so that stopping fits in the time Windows grants a closed console (run_bridge.hpp).
        runtimeOptions.shutdown_timeout = kSilKitShutdownTimeout;
        runtime = connect_to_registry(runtimeOptions);
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
        // The first link line, before the lifecycle starts (a program that waits for Running has read it).
        firstLinkLine = bridge_runtime::format_link_line("bus", link_line_value(bridge->health().bus_state));
        log(kTag, "%s", firstLinkLine.c_str());
        runtime->start(options.start_timeout);
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
        // A start that timed out leaves a lifecycle that can still get to CommunicationReady. Once
        // the runtime's stop() has returned, the handler above is not called any more, so the
        // bridge it points at can go (in whatever order the three are destroyed).
        if (bridge)
        {
            bridge->stop();
        }
        if (runtime)
        {
            runtime->stop("the setup failed");
        }
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
    const std::string xlChannel = "XL '" + channel.name + "'";
    runOptions.ready_line = "bridging SIL Kit network '" + options.network + "' (participant '" + options.participant
                            + "', " + options.registry_uri + ") <-> " + xlChannel + "; Ctrl+C to stop";
    runOptions.statistics_interval = options.statistics_interval;
    runOptions.link_line_printed = firstLinkLine;
    XlBridgeHooks hooks(*bridge, xlChannel);
    return bridge_runtime::run_bridge(*runtime, hooks, runOptions);
}

} // namespace

int main(int argc, char** argv)
{
    BridgeOptions options;
    try
    {
        // UTF-8 on Windows too: SIL Kit takes the names and the address as UTF-8.
        const std::vector<std::string> arguments = cli::utf8_arguments(argc, argv);
        options = parse_bridge_options(std::vector<std::string_view>(arguments.begin(), arguments.end()));
    }
    catch (const BridgeUsageError& e)
    {
        // Two lines: the whole usage text would push the error out of sight.
        cli::print(stderr, "error: %s\nRun 'vehicle_sil_vector_xl_bridge --help' for the options.\n", e.what());
        return kExitUsage;
    }
    if (options.help)
    {
        cli::write_text(stdout, bridge_usage());
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
