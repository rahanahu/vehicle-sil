// SPDX-License-Identifier: MIT
//
// vehicle_sil_ethernet_bridge: forwards Ethernet frames in both directions between a SIL Kit
// Ethernet network and an Ethernet interface outside SIL Kit (a TAP device, a network interface).
//
// Exit codes: 0 stopped normally (Ctrl+C / Ctrl+Break / SIGTERM / console closed) or
//               --list-interfaces/--describe/--help done,
//             1 failed while running: the SIL Kit participant stopped or ended on its own (e.g.
//               connection lost) or the interface stopped working (e.g. removed),
//             3 setup failed (the backend cannot be used here, the interface, SIL Kit),
//             4 bad command line.
#include "link_factory.hpp"
#include "pcap_library.hpp"

#include "vehicle_sil/bridge_runtime/describe.hpp"
#include "vehicle_sil/bridge_runtime/device_mark.hpp"
#include "vehicle_sil/bridge_runtime/link_line.hpp"
#include "vehicle_sil/bridge_runtime/nothing_came_hint.hpp"
#include "vehicle_sil/bridge_runtime/registry_failure.hpp"
#include "vehicle_sil/bridge_runtime/run_bridge.hpp"
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/cli/utf8.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"
#include "vehicle_sil/ethernet_bridge/ethernet_bridge.hpp"
#include "vehicle_sil/ethernet_bridge/interface_list.hpp"

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
    /// `hint`: null when the link has no promiscuous mode (tap). `nothing_came`: null when the
    /// link stays in this computer (tap). `share`: the mark on the interface.
    EthernetBridgeHooks(EthernetBridge& bridge, PromiscuousHint* hint, std::unique_ptr<bridge_runtime::NothingCameHint> nothing_came,
                        std::string interface_name, bridge_runtime::DeviceShareWatch& share) :
      bridge_(bridge),
      hint_(hint),
      nothing_came_(std::move(nothing_came)),
      interface_name_(std::move(interface_name)),
      share_(share),
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
        if (nothing_came_)
        {
            const auto counts = bridge_.statistics();
            if (auto hint = nothing_came_->look(counts.silkit_to_ext, counts.ext_from_devices, std::chrono::steady_clock::now()))
            {
                warnings.push_back(std::move(*hint));
            }
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
        return bridge_runtime::format_link_line("link", link_line_value(bridge_.health().link_state)) + share_.link_keys();
    }
    std::string statistics_line() override { return format_statistics(bridge_.statistics()); }
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
        std::vector<std::string> lines{statistics_line()};
        if (auto error = bridge_.last_error(); !error.empty())
        {
            lines.push_back("last error: " + error);
        }
        return lines;
    }

private:
    EthernetBridge& bridge_;
    PromiscuousHint* hint_;
    const std::unique_ptr<bridge_runtime::NothingCameHint> nothing_came_;
    const std::string interface_name_;
    bridge_runtime::DeviceShareWatch& share_;
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
        std::string list;
        try
        {
            list = interface_list(options);
        }
        catch (const std::exception& e)
        {
            if (!options.json)
            {
                throw; // main() says FAIL
            }
            // stdout holds the JSON alone, for programs; the FAIL line goes to stderr, for people.
            cli::write_text(stdout, interface_list_error_json(options.backend, e));
            cli::print(stderr, "[%s] FAIL setup: %s\n", kTag, e.what());
            return kExitSetupFailed;
        }
        if (!cli::write_text(stdout, list))
        {
            throw std::runtime_error("could not write the list to stdout (is it closed?)");
        }
        return kExitOk;
    }
    if (options.describe)
    {
        // Before anything is opened or loaded (the pcap library too): vehicle_sil_launch check asks
        // this on computers where the bridge will not run.
        const bridge_runtime::BridgeDescription description{options.participant, options.network,
                                                            bridge_runtime::BridgedNetworkKind::Ethernet};
        if (!cli::write_text(stdout, bridge_runtime::describe_json(description)))
        {
            throw std::runtime_error("could not write the description to stdout (is it closed?)");
        }
        return kExitOk;
    }

    // Declaration order is destruction order in reverse: the bridge goes first, then the SIL Kit
    // participant (which owns the Ethernet controller), then the link (which closes the interface).
    // Nothing is opened here: a backend that cannot be used on this computer fails before SIL Kit
    // is touched.
    const std::unique_ptr<eth_link::ExternalLink> link = make_external_link(options);
    std::unique_ptr<PromiscuousHint> hint;
    if (reaches_real_adapters(*options.backend))
    {
        // It stays quiet while the link is in promiscuous mode (known once it has started).
        hint = std::make_unique<PromiscuousHint>(*link, options.promiscuous);
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
    // The mark on the interface, once it is open: only then does the link know the device's own
    // name (Npcap's pcap name for a connection name). The first count is in the first link line.
    bridge_runtime::DeviceShareWatch share(
        bridge_runtime::DeviceMark::place(bridge_runtime::kEthernetDevice, device_mark_identity(external.device_identity())),
        "interface '" + options.interface_name + "'");
    // The first link line, before the lifecycle starts (a program that waits for Running has read it).
    const std::string firstLinkLine =
        bridge_runtime::format_link_line("link", link_line_value(bridge.health().link_state)) + share.link_keys();
    log(kTag, "%s", firstLinkLine.c_str());
    try
    {
        runtime.start(options.start_timeout);
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
    runOptions.ready_line = format_start_line(options, external.describe(), external.promiscuous());
    runOptions.statistics_interval = options.statistics_interval;
    runOptions.link_line_printed = firstLinkLine;
    std::unique_ptr<bridge_runtime::NothingCameHint> nothingCame;
    if (reaches_real_adapters(*options.backend))
    {
        nothingCame = std::make_unique<bridge_runtime::NothingCameHint>(
            [&bridge, &external] {
                return nothing_came_warning(external.describe(), bridge.this_computer_addresses().has_value(),
                                            bridge_runtime::kNothingCameAfter);
            });
    }
    EthernetBridgeHooks hooks(bridge, hint.get(), std::move(nothingCame), options.interface_name, share);
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
        options = parse_bridge_options(std::vector<std::string_view>(arguments.begin(), arguments.end()), default_backend());
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
