// SPDX-License-Identifier: MIT
//
// check_connections(): the --connect requests against the configuration and the participants'
// descriptions, the bridge tools found and their --describe (check.hpp). The rules that need the
// whole simulation (the bridge as a user of its network, loops with a role: bridge participant)
// are in check_participants() (check_rules.cpp).
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/config/check.hpp"


#include "vehicle_sil/config/detail/check_words.hpp"
#include "vehicle_sil/config/detail/seen_ecu.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::config {

namespace {

using detail::and_list;
using detail::join;

/// `target` for a message: control characters, and any byte above 0x7F if it is not UTF-8, as
/// \xNN; cut after 80 bytes.
std::string shown_target(std::string_view target)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    const bool utf8 = vehicle_sil::detail::is_utf8(target);
    std::string out;
    for (std::size_t i = 0; i < target.size(); ++i)
    {
        if (i == 80)
        {
            return out + "...";
        }
        const auto byte = static_cast<unsigned char>(target[i]);
        if (byte < 0x20 || byte == 0x7F || (!utf8 && byte >= 0x80))
        {
            out += std::string("\\x") + kHex[byte >> 4] + kHex[byte & 0xF];
        }
        else
        {
            out += target[i];
        }
    }
    return out;
}

/// The line of a tool's standard error that says why (detail::LastLine), or "".
std::string last_line(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    detail::LastLine last;
    while (std::getline(in, line))
    {
        last.add(line);
    }
    return last.text();
}

/// Where to see the targets of a network of `kind`.
std::string target_hint(NetworkKind kind, Os os)
{
    if (kind == NetworkKind::Can && os == Os::Linux)
    {
        return "give a SocketCAN interface (can0), or a Vector XL channel as xl:virtual:<n> or xl:physical:<n> "
               "(vehicle_sil_launch connections <file> lists those of this computer), or none for SIL Kit only";
    }
    return kind == NetworkKind::Can
               ? "give a channel as virtual:<n> or physical:<n> (vehicle_sil_launch connections <file> lists those of "
                 "this computer), or none for SIL Kit only"
               : "give an adapter that vehicle_sil_launch connections <file> lists, or none for SIL Kit only";
}

/// What the participants on the Ethernet network `network` say about their MAC addresses.
struct MacsOnNetwork
{
    std::vector<eth::MacAddress> macs;  ///< in the file's order
    std::vector<std::string> without;   ///< ECUs described on the network without a MAC address
    std::vector<std::string> unknown;   ///< ECUs with describe: false (their networks are not known)
    bool tests = false;                 ///< a participant with role: test reports a MAC address on the network
    bool incomplete = false;            ///< an enabled participant has no description (its error says why)
};

MacsOnNetwork macs_on(const SimulationConfig& config, const std::vector<ParticipantResult>& results,
                      const std::string& network)
{
    MacsOnNetwork out;
    for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
    {
        const ParticipantConfig& p = config.participants[i];
        const ParticipantResult& result = results[i];
        // Only the frames of the ECUs go out by default: not those of a participant for testing,
        // nor of a bridge, which carries the frames of others (connect-loop is about it).
        if (!p.enabled || p.role == ParticipantRole::Bridge)
        {
            continue;
        }
        const bool test = p.role == ParticipantRole::Test;
        if (result.status == ParticipantStatus::NotChecked)
        {
            if (!test)
            {
                out.unknown.push_back(p.name);
            }
            continue;
        }
        if (result.status != ParticipantStatus::Described || !result.ecu)
        {
            out.incomplete = true;
            continue;
        }
        if (test)
        {
            for (const SeenNetwork& seen : SeenEcu(p, *result.ecu).seen_networks())
            {
                out.tests = out.tests || (seen.network.name == network &&
                                          seen.network.kind() == NetworkKind::Ethernet && seen.network.mac);
            }
            continue;
        }
        bool on = false;
        bool reported = false;
        for (const SeenNetwork& seen : SeenEcu(p, *result.ecu).seen_networks())
        {
            if (seen.network.name == network && seen.network.kind() == NetworkKind::Ethernet)
            {
                on = true;
                if (seen.network.mac)
                {
                    reported = true;
                    out.macs.push_back(*seen.network.mac);
                }
            }
        }
        if (on && !reported)
        {
            out.without.push_back(p.name);
        }
    }
    return out;
}

/// The error connect-filter for `network`, or empty: the default filter (--src-mac with the ECUs'
/// MAC addresses) lets the frames of every ECU on it pass.
std::optional<std::string> filter_problem(const NetworkConfig& network, const MacsOnNetwork& macs)
{
    std::vector<std::string> why;
    if (!macs.without.empty())
    {
        why.push_back(and_list(macs.without) + (macs.without.size() == 1 ? " reports" : " report") +
                      " no MAC address on it");
    }
    if (!macs.unknown.empty())
    {
        why.push_back(and_list(macs.unknown) + (macs.unknown.size() == 1 ? " has" : " have") +
                      " describe: false, so the check cannot see whether " + (macs.unknown.size() == 1 ? "it is" : "they are") +
                      " on it and with which address");
    }
    if (why.empty() && macs.macs.empty())
    {
        why.emplace_back(macs.tests ? "no ECU on it reports a MAC address (only participants with role: test do)"
                                    : "no ECU on it reports a MAC address");
    }
    if (why.empty())
    {
        return std::nullopt;
    }
    return "network '" + network.name +
           "': without external.ethertypes or external.all_frames, the bridge of --connect passes only the frames of "
           "the MAC addresses that the ECUs on the network report (not those of participants with role: test), and " +
           and_list(why) +
           ": write external: { ethertypes: [0x88b5] } (the EtherTypes to pass, also for participants with role: test) "
           "or external: { all_frames: true } for the network" +
           (macs.without.empty() ? std::string{} : ", or give each ECU on it a MAC address (eth::ChannelOptions::mac)");
}

/// The error connect-tool-failed for one run of the bridge's --describe, or empty if the bridge
/// described itself as `participant` on `network` of its kind.
std::optional<std::string> describe_problem(const NetworkConfig& network, const std::string& participant,
                                            const DescribeRun& run)
{
    const std::string prefix = "network '" + network.name + "': ";
    const std::string fix = "use the bridge built with the vehicle-sil of vehicle_sil_launch";
    if (run.start_error)
    {
        return prefix + run.command_text + " could not be started (" + *run.start_error +
               "): check that the bridge runs on this computer, or " + fix;
    }
    if (!run.exit_code)
    {
        return prefix + run.command_text + " did not end within " + std::to_string(run.timeout.count()) +
               " ms: the bridge answers --describe at once; " + fix;
    }
    if (const std::string dll = run.signal ? std::string{} : detail::dll_start_failure(*run.exit_code); !dll.empty())
    {
        // Windows did not start it; its standard error is empty.
        return prefix + run.command_text + " " + dll + ": " + detail::bridge_dll_fix(network.kind) +
               "; to check without the real bus on this computer, leave --connect " + network.name + " out";
    }
    if (*run.exit_code != 0 || run.signal)
    {
        const std::string line = last_line(run.standard_error);
        return prefix + run.command_text +
               (run.signal ? " was ended by signal " + std::to_string(*run.signal) : " ended with exit code " + std::to_string(*run.exit_code)) +
               (line.empty() ? std::string(" and wrote nothing on stderr") : ": " + line) +
               ": the bridge does not take what --connect gives it; correct the target or external of the network as the "
               "line says, or " + fix;
    }
    std::string described;
    try
    {
        const ReportedEcu ecu = read_description(run.standard_output);
        ParticipantConfig entry;
        entry.name = participant;
        entry.role = ParticipantRole::Bridge;
        entry.role_written = true;
        std::vector<std::string> on;
        bool found = false;
        for (const SeenNetwork& seen : SeenEcu(entry, ecu).seen_networks())
        {
            on.push_back(seen.network.name + " (" + to_string(seen.network.kind()) + ")");
            found = found || (seen.network.name == network.name && seen.network.kind() == network.kind);
        }
        const std::string& name = SeenEcu(entry, ecu).name();
        if (name == participant && found)
        {
            // The bridge of --connect is started from the launcher's own folder: a bridge of another
            // release may count or name things differently, so it is refused (a bridge of
            // role: bridge in the YAML is the author's own and is not compared).
            if (ecu.vehicle_sil_version == vehicle_sil::version())
            {
                return std::nullopt;
            }
            return prefix + run.command_text + " was built from vehicle-sil " +
                   (ecu.vehicle_sil_version.empty() ? std::string("unknown") : ecu.vehicle_sil_version) +
                   "; this launcher is " + vehicle_sil::version() + ": use the bridge of the same release";
        }
        described = "described itself as '" + name + "' on " + (on.empty() ? std::string("no network") : join(on)) +
                    ", not as '" + participant + "' on " + network.name + " (" + to_string(network.kind) + ")";
    }
    catch (const DescriptionError& e)
    {
        described = "wrote no description that vehicle_sil_launch reads (" + std::string(e.what()) + ")";
    }
    return prefix + run.command_text + " " + described + ": " + fix;
}

/// The networks of each enabled ECU (not role: test or role: bridge, which carry no frames from one
/// network to another) that was described. `unknown`: ECUs with describe: false.
std::vector<std::vector<std::string>> ecu_networks(const SimulationConfig& config,
                                                   const std::vector<ParticipantResult>& results,
                                                   std::vector<std::string>& unknown)
{
    std::vector<std::vector<std::string>> out;
    for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
    {
        const ParticipantConfig& p = config.participants[i];
        if (!p.enabled || p.role != ParticipantRole::Ecu)
        {
            continue;
        }
        if (results[i].status == ParticipantStatus::NotChecked)
        {
            unknown.push_back(p.name);
        }
        else if (results[i].status == ParticipantStatus::Described && results[i].ecu)
        {
            std::vector<std::string> networks;
            for (const SeenNetwork& seen : SeenEcu(p, *results[i].ecu).seen_networks())
            {
                networks.push_back(seen.network.name);
            }
            out.push_back(std::move(networks));
        }
    }
    return out;
}

/// connect-target-shared and connect-shared-loop: two networks of `requests` (valid, connected)
/// whose targets are the same device. Where the two networks are joined through participants, only
/// the loop is reported.
void check_shared_targets(const SimulationConfig& config, const std::vector<ParticipantResult>& results,
                          const std::vector<std::pair<const NetworkConfig*, const ConnectRequest*>>& known,
                          Os os, ConnectCheck& out)
{
    const bool described = results.size() == config.participants.size();
    std::vector<std::string> unknown;
    const std::vector<std::vector<std::string>> joins =
        described ? ecu_networks(config, results, unknown) : std::vector<std::vector<std::string>>{};
    for (std::size_t i = 0; i < known.size(); ++i)
    {
        const NetworkConfig& first = *known[i].first;
        const std::string key = known[i].first->valid ? connect_device_key(first.kind, known[i].second->target, os) : "";
        for (std::size_t j = i + 1; !key.empty() && j < known.size(); ++j)
        {
            const NetworkConfig& second = *known[j].first;
            if (!second.valid || second.kind != first.kind ||
                connect_device_key(second.kind, known[j].second->target, os) != key)
            {
                continue;
            }
            const std::string same = key == "virtual" ? std::string("Vector Virtual CAN channels (all of them are one bus)")
                                                      : "'" + shown_target(known[i].second->target) + "'";
            const std::string both = "networks '" + first.name + "' and '" + second.name + "': --connect gives both " +
                                     (key == "virtual" ? "" : "the same target ") + same;
            const bool can = first.kind == NetworkKind::Can;
            if (described && networks_are_joined(joins, first.name, second.name))
            {
                const std::string maybe =
                    unknown.empty() ? std::string{}
                                    : "; " + and_list(unknown) + (unknown.size() == 1 ? " has" : " have") +
                                          " describe: false, so the check cannot see what " +
                                          (unknown.size() == 1 ? "it joins" : "they join") + " (more may be joined)";
                const std::string join_text = "an ECU is on both networks (or a chain of ECUs joins them)";
                const std::string message =
                    both + ", and " + join_text +
                    ": when such an ECU forwards a frame from one to the other, the frame goes round between the " +
                    (can ? "real bus" : "adapter") +
                    " and the simulation, and the bridge's guard against that cuts the external device off from one of the "
                    "networks: connect them to different " + (can ? "targets" : "adapters") +
                    ", or make them one network in the file" + maybe;
                out.findings.push_back(Finding{Severity::Error, "connect-shared-loop", std::move(message), first.line, "",
                                               first.name});
                for (NetworkOutside& o : out.outside)
                {
                    if (o.network == first.name || o.network == second.name)
                    {
                        o.arguments.reset();
                    }
                }
                continue;
            }
            out.findings.push_back(Finding{Severity::Warning, "connect-target-shared",
                                           both + ": the frames of both networks are mixed there: connect them to different "
                                                  "channels (adapters) if they must stay apart, or make them one network in "
                                                  "the file if they are one bus",
                                           first.line, "", first.name});
        }
    }
}

} // namespace

ConnectCheck check_connections(const SimulationConfig& config, const std::vector<ParticipantResult>& results,
                               const std::vector<ConnectRequest>& requests, const ConnectTools* tools)
{
    ConnectCheck out;
    const Os os = tools != nullptr ? tools->os : kThisOs;
    const auto add = [&out](Severity severity, const char* code, std::size_t line, std::string message,
                            const std::string& participant, const std::string& network) {
        out.findings.push_back(Finding{severity, code, std::move(message), line, participant, network});
    };
    std::vector<std::string> names;
    for (const NetworkConfig& network : config.networks)
    {
        names.push_back(network.name);
    }
    // The requests for networks of the file, in the file's order (the others first: no line).
    std::vector<std::pair<const NetworkConfig*, const ConnectRequest*>> known;
    for (const ConnectRequest& request : requests)
    {
        const auto it = std::find_if(config.networks.begin(), config.networks.end(),
                                     [&](const NetworkConfig& n) { return n.name == request.network; });
        if (it != config.networks.end())
        {
            known.emplace_back(&*it, &request);
            continue;
        }
        // A name with '=' cannot be given: the request is split at the first '='.
        const std::string given = request.network + "=" + request.target;
        const auto split = std::find_if(config.networks.begin(), config.networks.end(), [&](const NetworkConfig& n) {
            return n.name.find('=') != std::string::npos && given.rfind(n.name + "=", 0) == 0;
        });
        std::string message = "--connect names the network '" + request.network + "', which the file does not have";
        if (split != config.networks.end())
        {
            message += ": the network '" + split->name +
                       "' has '=' in its name, and --connect takes the network's name up to the first '=': rename the "
                       "network (a name without '=')";
        }
        else if (const std::string close = detail::closest(request.network, names); !close.empty())
        {
            message += ": did you mean '" + close + "'? (the networks: " + join(names) + ")";
        }
        else
        {
            message += names.empty() ? std::string(": the file has no networks") : ": the networks are " + join(names);
        }
        add(Severity::Error, "connect-network", 0, std::move(message), "", "");
    }
    std::stable_sort(known.begin(), known.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [network, request] : known)
    {
        const std::string prefix = "network '" + network->name + "': ";
        // Its kind could not be read: its own error says so, and there is no bridge to choose.
        if (!network->valid || request->target == kNoTarget)
        {
            continue;
        }
        if (network->kind == NetworkKind::Lin)
        {
            add(Severity::Error, "connect-kind", network->line,
                prefix + "it is lin: there is no bridge for LIN, so it runs on SIL Kit only: leave --connect " +
                    network->name + " out (or give " + network->name + "=none)",
                "", network->name);
            continue;
        }
        const std::size_t before = out.findings.size();
        NetworkOutside outside;
        outside.network = network->name;
        const ConnectTarget read = read_connect_target(network->kind, request->target, os);
        outside.tool = std::string(bridge_tool(network->kind, read.backend));
        outside.target = request->target;
        outside.participant = bridge_participant(network->name);
        if (auto problem = backend_problem(network->kind, request->target, os))
        {
            add(Severity::Error, "connect-backend", network->line,
                prefix + "--connect " + network->name + "=" + shown_target(request->target) + ": " + *problem, "",
                network->name);
            out.outside.push_back(std::move(outside));
            continue;
        }
        if (auto problem = target_problem(network->kind, request->target, os))
        {
            add(Severity::Error, "connect-target", network->line,
                prefix + "--connect " + network->name + "=" + shown_target(request->target) + ": " + *problem + ": " +
                    target_hint(network->kind, os),
                "", network->name);
        }
        for (const ParticipantConfig& p : config.participants)
        {
            if (p.name == outside.participant)
            {
                add(Severity::Error, "connect-name-taken", p.line,
                    "participant '" + p.name + "' has the name that --connect " + network->name +
                        " gives the bridge of the network: rename the participant",
                    p.name, network->name);
            }
        }
        std::vector<eth::MacAddress> macs;
        // false: what passes is not known (a participant without a description), so there is no
        // command to give the bridge, and its --describe is not run.
        bool passKnown = true;
        const bool described = tools != nullptr && results.size() == config.participants.size();
        if (described && network->kind == NetworkKind::Ethernet &&
            !(network->external && (network->external->all_frames || !network->external->ethertypes.empty())))
        {
            MacsOnNetwork on = macs_on(config, results, network->name);
            // A participant without a description: its error comes first.
            passKnown = !on.incomplete;
            if (passKnown)
            {
                if (auto problem = filter_problem(*network, on))
                {
                    add(Severity::Error, "connect-filter", network->line, std::move(*problem), "", network->name);
                }
            }
            macs = std::move(on.macs);
        }
        const std::vector<std::string> bridgeArguments = bridge_arguments(*network, request->target, macs, os);
        if (tools != nullptr && tools->find)
        {
            if (auto found = tools->find(outside.tool))
            {
                outside.tool_path = std::move(*found);
            }
            else
            {
                const std::optional<std::string> none = no_bridge_here(network->kind, read.backend, os);
                add(Severity::Error, "connect-tool-missing", network->line,
                    none ? prefix + "--connect needs " + outside.tool + ", and " + *none +
                               ": on this computer the network runs on SIL Kit only: leave --connect " + network->name +
                               " out (or give " + network->name + "=none)"
                         : prefix + "--connect needs " + outside.tool + ", which is " + tool_not_found(tools->launcher_dir) +
                               ": " + tool_missing_fix(outside.tool),
                    "", network->name);
            }
        }
        if (out.findings.size() == before && passKnown && !outside.tool_path.empty() && tools->describe)
        {
            std::vector<std::string> arguments = bridgeArguments;
            arguments.insert(arguments.end(), {"--describe", "--name", outside.participant});
            if (auto problem = describe_problem(*network, outside.participant, tools->describe(outside.tool_path, arguments)))
            {
                add(Severity::Error, "connect-tool-failed", network->line, std::move(*problem), "", network->name);
            }
        }
        if (out.findings.size() == before && passKnown)
        {
            outside.arguments = bridgeArguments;
        }
        out.outside.push_back(std::move(outside));
    }
    check_shared_targets(config, results, known, os, out);

    return out;
}

} // namespace vehicle_sil::config
