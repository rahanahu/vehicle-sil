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

#include "vehicle_sil/bridge_runtime/registry_failure.hpp"
#include "vehicle_sil/bridge_runtime/run_bridge.hpp"
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/cli/utf8.hpp"
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
#include <stdexcept>
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

/// The participant, connected to the registry. A registry that does not answer is the usual first
/// failure: say where the bridge looked and what to do, not only SIL Kit's text.
ParticipantRuntime connect_to_registry(const ParticipantRuntimeOptions& options)
{
    try
    {
        return ParticipantRuntime(options);
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
    if (options.list_interfaces)
    {
        cli::write_text(stdout, interface_list(options));
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
    ParticipantRuntime runtime = connect_to_registry(runtimeOptions);
    auto& controller = *runtime.participant().CreateEthernetController("EthernetBridge", options.network);
    EthernetBridge bridge(controller, external, options.bridge);

    // Start forwarding before the lifecycle reaches Running: peers may start sending as soon as
    // they see this participant Running, and the link and the SIL Kit handlers must be ready by
    // then. start() opens the interface (it throws eth_link::LinkError with the way to fix it).
    // The controller is activated where SIL Kit wants it, in the CommunicationReady handler.
    runtime.add_communication_ready_handler([&bridge] { bridge.start_controller(); });
    bridge.start();
    try
    {
        runtime.start(std::chrono::seconds(15));
    }
    catch (...)
    {
        // `bridge` is destroyed before `runtime` (declared after it), and a participant whose
        // start timed out can still reach CommunicationReady. After stop() the handler is not
        // called, so it never meets a bridge that is gone.
        bridge.stop();
        runtime.stop("the setup failed");
        throw;
    }

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
        // UTF-8 on Windows too: SIL Kit takes the names and the address as UTF-8, and the links
        // compare the interface's name as UTF-8.
        const std::vector<std::string> arguments = cli::utf8_arguments(argc, argv);
        options = parse_bridge_options(std::vector<std::string_view>(arguments.begin(), arguments.end()));
    }
    catch (const BridgeUsageError& e)
    {
        // Two lines: the whole usage text would push the error out of sight.
        cli::print(stderr, "error: %s\nRun 'vehicle_sil_ethernet_bridge --help' for the options.\n", e.what());
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
