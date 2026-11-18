// SPDX-License-Identifier: MIT
//
// vehicle_sil_socketcan_bridge: forwards classic CAN frames in both directions between a SIL Kit
// CAN network and a SocketCAN interface of this computer (a CAN adapter such as can0, or a virtual
// one such as vcan0). Linux only.
//
// Exit codes: 0 stopped normally (Ctrl+C / SIGTERM) or --list-channels/--describe/--help done,
//             1 failed while running: the SIL Kit participant stopped or ended on its own (e.g.
//               connection lost) or the interface went away (e.g. the adapter was unplugged),
//             3 setup failed (the interface is missing or down, --bitrate differs, SIL Kit),
//             4 bad command line.
#include "vehicle_sil/bridge_runtime/describe.hpp"
#include "vehicle_sil/bridge_runtime/device_mark.hpp"
#include "vehicle_sil/bridge_runtime/link_line.hpp"
#include "vehicle_sil/bridge_runtime/nothing_came_hint.hpp"
#include "vehicle_sil/bridge_runtime/registry_failure.hpp"
#include "vehicle_sil/bridge_runtime/run_bridge.hpp"
#include "vehicle_sil/can_bridge/can_bridge.hpp"
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/cli/utf8.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/socketcan/bridge_options.hpp"
#include "vehicle_sil/socketcan/can_device.hpp"
#include "vehicle_sil/socketcan/can_interfaces.hpp"
#include "vehicle_sil/socketcan/socketcan_port.hpp"

#include <silkit/services/can/ICanController.hpp>

#include <algorithm>
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
using namespace vehicle_sil::socketcan;
using namespace vehicle_sil::can_bridge;
using cli::log;

using cli::kExitOk;
using cli::kExitSetupFailed;
using cli::kExitUsage;

constexpr const char* kTag = "bridge";

/// Upper bound for the SIL Kit lifecycle to shut down when the bridge stops.
constexpr auto kSilKitShutdownTimeout = std::chrono::milliseconds(2000);

std::string_view link_value(LinkState state) noexcept
{
    switch (state)
    {
    case LinkState::Up: return "up";
    case LinkState::Down: return "down";
    case LinkState::Unknown: break;
    }
    return "unknown";
}

/// What run_bridge() asks of the CAN bridge.
class SocketCanHooks final : public bridge_runtime::BridgeHooks
{
public:
    /// `interface`: how the texts name it ("CAN interface 'can0'"). `bus`: "CAN bus 'can0'".
    SocketCanHooks(CanBridge& bridge, const SocketCanPort& port, const std::string& interface, std::string bus,
                   bridge_runtime::DeviceShareWatch& share) :
      bridge_(bridge),
      port_(port),
      bus_(std::move(bus)),
      share_(share),
      reported_(bridge.health()),
      nothing_came_([interface] { return nothing_came_warning(interface, bridge_runtime::kNothingCameAfter); })
    {
    }

    /// "link bus=<bus state> link=<up|down>" and the keys of the mark.
    [[nodiscard]] std::string line() const
    {
        return bridge_runtime::format_link_line("bus", link_line_value(bridge_.health().bus_state)) +
               " link=" + std::string(link_value(port_.link_state())) + share_.link_keys();
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
        for (auto& warning : share_.take_warnings())
        {
            warnings.push_back(std::move(warning));
        }
        return warnings;
    }
    std::optional<std::string> link_line() override
    {
        // Once a second: a bridge that started later on the same interface is found here.
        share_.count();
        return line();
    }
    std::string statistics_line() override { return format_statistics(bridge_.statistics()); }
    std::optional<std::string> health_line() override
    {
        const CanBridgeHealth now = bridge_.health();
        auto line = format_health_change(reported_, now, bus_, problems_before_);
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
    const SocketCanPort& port_;
    std::string bus_;
    bridge_runtime::DeviceShareWatch& share_;
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

int list_channels(const BridgeOptions& options)
{
    try
    {
        const std::vector<CanInterfaceInfo> interfaces = read_can_interfaces();
        // The marks of the bridges that use an interface, looked at without placing one.
        const InterfaceInUse inUse = [](const CanInterfaceInfo& info) {
            return bridge_runtime::look_for_marks(bridge_runtime::kCanDevice, info.name);
        };
        cli::write_text(stdout, options.json ? interfaces_json(interfaces, inUse) : format_interface_list(interfaces, inUse));
        return kExitOk;
    }
    catch (const std::exception& e)
    {
        if (!options.json)
        {
            throw; // main() says FAIL
        }
        // stdout holds the JSON alone, for programs; the FAIL line goes to stderr, for people.
        cli::write_text(stdout, interfaces_error_json("failed", e.what()));
        cli::print(stderr, "[%s] FAIL setup: %s\n", kTag, e.what());
        return kExitSetupFailed;
    }
}

int run(const BridgeOptions& options)
{
    if (options.describe)
    {
        // Without the interface: vehicle_sil_launch check asks this on computers where the bridge
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
    if (options.list_channels)
    {
        return list_channels(options);
    }

    // Errors from here on are setup failures that main() reports.
    const std::vector<CanInterfaceInfo> interfaces = read_can_interfaces();
    const auto found = std::find_if(interfaces.begin(), interfaces.end(),
                                    [&](const CanInterfaceInfo& info) { return info.name == options.interface_name; });
    if (found == interfaces.end())
    {
        throw std::runtime_error(missing_interface_message(options.interface_name, interfaces));
    }
    const InterfaceCheck check = check_interface(*found, options.bitrate);
    if (check.failure)
    {
        throw std::runtime_error(*check.failure);
    }
    log(kTag, "CAN interface: %s (%s, %s)", found->name.c_str(), found->is_virtual() ? "virtual" : "CAN adapter",
        found->bitrate ? (std::to_string(*found->bitrate) + " bit/s").c_str() : "no bit rate");
    for (const auto& warning : check.warnings)
    {
        log(kTag, "warning: %s", warning.c_str());
    }
    const std::string interfaceText = "CAN interface '" + found->name + "'";
    // The mark on the interface, before the port opens it; the first count is in the first link line.
    bridge_runtime::DeviceShareWatch share(bridge_runtime::DeviceMark::place(bridge_runtime::kCanDevice, found->name),
                                           interfaceText);

    // Declaration order is destruction order in reverse: the bridge goes first, then the port, the
    // SIL Kit participant (which owns the CAN controller) and the device.
    std::unique_ptr<CanDevice> device = make_linux_can_device(found->name);
    std::unique_ptr<ParticipantRuntime> runtime;
    std::unique_ptr<SocketCanPort> port;
    std::unique_ptr<CanBridge> bridge;
    std::string firstLinkLine;
    try
    {
        ParticipantRuntimeOptions runtimeOptions{options.participant, options.registry_uri};
        // Bounded so that stopping fits in the time a closed console grants (run_bridge.hpp).
        runtimeOptions.shutdown_timeout = kSilKitShutdownTimeout;
        runtime = connect_to_registry(runtimeOptions);
        auto& controller = *runtime->participant().CreateCanController("SocketCanBridge", options.network);

        SocketCanPortOptions portOptions;
        portOptions.transmit_timeout = options.tx_timeout;
        port = std::make_unique<SocketCanPort>(*device, portOptions);

        // Start forwarding before the lifecycle reaches Running: peers may start sending as soon as
        // they see this participant Running, and the SIL Kit handler must be ready by then. The
        // controller is started where SIL Kit wants it, before Running.
        bridge = std::make_unique<CanBridge>(controller, *port);
        runtime->add_communication_ready_handler([b = bridge.get()] { b->start_controller(); });
        bridge->start();
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
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
    SocketCanHooks hooks(*bridge, *port, interfaceText, "CAN bus '" + found->name + "'", share);
    try
    {
        // The first link line, before the lifecycle starts (a program that waits for Running has read it),
        // with a count as of now: connecting to the registry may have taken a while since the mark.
        share.count();
        firstLinkLine = hooks.line();
        log(kTag, "%s", firstLinkLine.c_str());
        runtime->start(options.start_timeout);
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
        // A start that timed out leaves a lifecycle that can still get to CommunicationReady. Once
        // the runtime's stop() has returned, the handler above is not called any more, so the
        // bridge it points at can go (in whatever order the three are destroyed).
        bridge->stop();
        runtime->stop("the setup failed");
        return kExitSetupFailed;
    }

    bridge_runtime::RunBridgeOptions runOptions;
    runOptions.tag = kTag;
    runOptions.ready_line = "bridging SIL Kit network '" + options.network + "' (participant '" + options.participant +
                            "', " + options.registry_uri + ") <-> " + interfaceText + "; Ctrl+C to stop";
    runOptions.statistics_interval = options.statistics_interval;
    runOptions.link_line_printed = firstLinkLine;
    return bridge_runtime::run_bridge(*runtime, hooks, runOptions);
}

} // namespace

int main(int argc, char** argv)
{
    BridgeOptions options;
    try
    {
        const std::vector<std::string> arguments = cli::utf8_arguments(argc, argv);
        options = parse_bridge_options(std::vector<std::string_view>(arguments.begin(), arguments.end()));
    }
    catch (const BridgeUsageError& e)
    {
        // Two lines: the whole usage text would push the error out of sight.
        cli::print(stderr, "error: %s\nRun 'vehicle_sil_socketcan_bridge --help' for the options.\n", e.what());
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
