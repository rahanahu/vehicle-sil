// SPDX-License-Identifier: MIT
//
// vehicle_sil_ethernet_bridge: forwards Ethernet frames in both directions between a SIL Kit
// Ethernet network and an Ethernet interface outside SIL Kit (a TAP device, a network interface).
//
// Exit codes: 0 stopped normally (Ctrl+C / Ctrl+Break / SIGTERM / console closed) or
//               --list-interfaces/--help done,
//             1 failed while running: the SIL Kit participant stopped or ended on its own (e.g.
//               connection lost) or the interface stopped working (e.g. removed),
//             3 setup failed (the backend cannot be used here, the interface, SIL Kit),
//             4 bad command line.
#include "link_factory.hpp"

#include "vehicle_sil/bridge_runtime/run_bridge.hpp"
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"
#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"

#include <silkit/services/ethernet/IEthernetController.hpp>

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
using namespace vehicle_sil::ethernet_bridge;
using cli::log;

using cli::kExitOk;
using cli::kExitSetupFailed;
using cli::kExitUsage;

constexpr const char* kTag = "eth-bridge";

/// Upper bound for the SIL Kit lifecycle to shut down when the bridge stops.
constexpr auto kSilKitShutdownTimeout = std::chrono::milliseconds(2000);

/// What run_bridge() asks of the Ethernet bridge.
class EthernetBridgeHooks final : public bridge_runtime::BridgeHooks
{
public:
    /// `hint`: null when the link is in promiscuous mode or has no such mode.
    EthernetBridgeHooks(EthernetBridge& bridge, PromiscuousHint* hint, std::string interface_name) :
      bridge_(bridge),
      hint_(hint),
      interface_name_(std::move(interface_name)),
      reported_(bridge.health())
    {
    }

    std::optional<std::string> fault() override { return bridge_.fault(); }
    std::vector<std::string> take_warnings() override
    {
        auto warnings = bridge_.take_warnings();
        if (hint_ != nullptr)
        {
            if (auto hint = hint_->take_warning())
            {
                warnings.push_back(std::move(*hint));
            }
        }
        return warnings;
    }
    std::optional<std::string> health_line() override
    {
        const EthernetBridgeHealth now = bridge_.health();
        auto line = format_health_change(reported_, now, interface_name_, problems_before_);
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
    EthernetBridge& bridge_;
    PromiscuousHint* hint_;
    const std::string interface_name_;
    EthernetBridgeHealth reported_;
    bool problems_before_ = false;
};

int run(const BridgeOptions& options)
{
    if (options.list_interfaces)
    {
        std::fputs(interface_list(options).c_str(), stdout);
        return kExitOk;
    }

    // Declaration order is destruction order in reverse: the bridge goes first, then the SIL Kit
    // participant (which owns the Ethernet controller), then the link (which closes the interface).
    // Nothing is opened here: a backend that cannot be used on this computer fails before SIL Kit
    // is touched.
    const std::unique_ptr<eth_link::ExternalLink> link = make_external_link(options);
    std::unique_ptr<PromiscuousHint> hint;
    if (reaches_real_adapters(*options.backend) && !options.promiscuous)
    {
        hint = std::make_unique<PromiscuousHint>(*link);
    }
    eth_link::ExternalLink& external = hint ? static_cast<eth_link::ExternalLink&>(*hint) : *link;

    ParticipantRuntimeOptions runtimeOptions{options.participant, options.registry_uri};
    // Bounded so that stopping fits in the time Windows grants a closed console (run_bridge.hpp).
    runtimeOptions.shutdown_timeout = kSilKitShutdownTimeout;
    ParticipantRuntime runtime(runtimeOptions);
    auto& controller = *runtime.participant().CreateEthernetController("EthernetBridge", options.network);
    EthernetBridge bridge(controller, external, options.bridge);

    // Start forwarding before the lifecycle reaches Running: peers may start sending as soon as
    // they see this participant Running, and the link, the SIL Kit handlers and the controller
    // must be ready by then. start() opens the interface (it throws eth_link::LinkError with the
    // way to fix it) and activates the controller.
    //
    // If activating the controller moves out of EthernetBridge::start() into the lifecycle's
    // communication-ready handler, register that handler here, before runtime.start().
    bridge.start();
    runtime.start(std::chrono::seconds(15));

    bridge_runtime::RunBridgeOptions runOptions;
    runOptions.tag = kTag;
    runOptions.ready_line = format_start_line(options, external.describe());
    EthernetBridgeHooks hooks(bridge, hint.get(), options.interface_name);
    return bridge_runtime::run_bridge(runtime, hooks, runOptions);
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
