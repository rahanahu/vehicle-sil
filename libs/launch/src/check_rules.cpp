// SPDX-License-Identifier: MIT
//
// The rules of the check: what a run of --describe means (interpret_describe()) and the comparison
// of the descriptions with the configuration (check_participants()): each participant against
// its entry, then each network by its kind. Pure functions.
#include "vehicle_sil/launch/check.hpp"

#include "check_words.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/ecu/detail/json_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using detail::and_list;
using detail::hex2;
using detail::step_text;

std::string participant_prefix(const std::string& name) { return "participant '" + name + "': "; }

/// "0x80 (128)".
std::string address_text(j1939::Address address) { return hex2(address) + " (" + std::to_string(address) + ")"; }

/// `line` cut to at most 300 bytes, not inside a UTF-8 sequence, with "..." when cut.
std::string shortened(std::string line)
{
    if (line.size() <= 300)
    {
        return line;
    }
    std::size_t cut = 300;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    return line.substr(0, cut) + "...";
}

/// The last `lines` lines of `text` (without "\r", each at most 300 bytes).
std::string last_lines(const std::string& text, std::size_t lines)
{
    std::vector<std::string> all;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        std::erase(line, '\r');
        all.push_back(shortened(std::move(line)));
    }
    while (!all.empty() && all.back().empty())
    {
        all.pop_back();
    }
    const std::size_t from = all.size() > lines ? all.size() - lines : 0;
    std::string out;
    for (std::size_t i = from; i < all.size(); ++i)
    {
        out += (out.empty() ? "" : "\n") + all[i];
    }
    return out;
}

/// The first `lines` lines of `text` (without "\r", each at most 300 bytes).
std::string first_lines(const std::string& text, std::size_t lines)
{
    std::string out;
    std::istringstream in(text.substr(0, 4096));
    std::string line;
    for (std::size_t n = 0; n < lines && std::getline(in, line); ++n)
    {
        std::erase(line, '\r');
        out += (n == 0 ? "" : "\n") + shortened(std::move(line));
    }
    return out;
}

std::string stderr_detail(const std::string& standard_error, bool truncated)
{
    const std::string tail = last_lines(standard_error, 10);
    return tail.empty()  ? "its standard error is empty"
           : truncated ? "the end of its standard error (longer than 1 MiB; only its end was kept):\n" + tail
                       : "the end of its standard error:\n" + tail;
}

/// true if the participant is enabled and described itself (its networks count).
bool is_on_any(const ParticipantConfig& participant, const ParticipantResult& result)
{
    return participant.enabled && result.status == ParticipantStatus::Described && result.ecu;
}

/// One J1939 node on a network: who has it, for messages.
struct NodeOnNetwork
{
    std::string label; ///< "Engine", or "Engine (node 2)" if Engine has more than one there
    J1939NodeDescription node;
};

} // namespace

DescribeOutcome interpret_describe(const ParticipantConfig& participant, const DescribeRun& run)
{
    const std::string prefix = participant_prefix(participant.name);
    const auto finding = [&](std::string code, std::string message, std::string detail) {
        return DescribeOutcome{std::nullopt, Finding{Severity::Error, std::move(code), prefix + std::move(message),
                                                     participant.command_line, participant.name, "", std::move(detail)}};
    };
    if (run.start_error)
    {
        return finding("describe-start",
                       *run.start_error + (run.start_refused_by_system
                                               ? ": the system did not let vehicle_sil_launch start a process (see the "
                                                 "reason); this is not about the executable"
                                               : ": check that the file is an executable for this computer"),
                       "");
    }
    if (!run.exit_code)
    {
        return finding("describe-timeout",
                       run.command_text + " did not end within " + std::to_string(run.timeout.count()) +
                           " ms and was stopped. --describe only builds the ECU, so something in its set-up waits "
                           "(a connection, input): fix that, or give vehicle_sil_launch check a longer "
                           "--describe-timeout-ms",
                       stderr_detail(run.standard_error, run.error_truncated));
    }
    if (*run.exit_code != 0)
    {
        std::string ended;
        std::string hint;
        if (run.signal)
        {
            ended = "was ended by the signal " + std::to_string(*run.signal);
            hint = "it crashed or was killed while it built the ECU: fix the ECU (its standard error may say where)";
        }
#ifdef _WIN32
        else if (const auto code = static_cast<std::uint32_t>(*run.exit_code); code >= 0xC0000000u)
        {
            // A Windows exception code, such as 0xC0000005.
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(code));
            ended = std::string("ended with the exception code ") + hex;
            hint = "it crashed while it built the ECU: fix the ECU (its standard error may say where)";
        }
#endif
        else
        {
            ended = "ended with exit code " + std::to_string(*run.exit_code);
            hint = *run.exit_code == ecu_executable::kExitUsage
                       ? "it did not take --describe or --name: if it is not made with ecu_main, set describe: false"
                       : "its set-up failed (see its standard error): fix the ECU, or set describe: false if it is not "
                         "made with ecu_main";
        }
        return finding("describe-failed", run.command_text + " " + ended + "; " + hint,
                       stderr_detail(run.standard_error, run.error_truncated));
    }
    try
    {
        return DescribeOutcome{read_description(run.standard_output), std::nullopt};
    }
    catch (const DescriptionError& e)
    {
        const std::string what = e.what();
        if (what.rfind("format ", 0) == 0)
        {
            return finding("describe-version",
                           run.command_text + " wrote a description in " + what + ", but this vehicle_sil_launch reads format " +
                               std::to_string(kEcuDescriptionFormat) +
                               ": build the executable and vehicle_sil_launch from the same vehicle-sil",
                           "");
        }
        const std::string head = first_lines(run.standard_output, 5);
        std::string detail;
        const auto add_detail = [&detail](const std::string& text) {
            if (!text.empty())
            {
                detail += (detail.empty() ? "" : "\n") + text;
            }
        };
        add_detail(run.output_truncated ? "its standard output was longer than 1 MiB and was cut there" : "");
        add_detail(e.context());
        add_detail(head.empty() ? stderr_detail(run.standard_error, run.error_truncated)
                                : "the start of its standard output:\n" + head);
        return finding("describe-output",
                       run.command_text + " ended with exit code 0, but its output is not a description: " + what +
                           ". Is it made with ecu_main, and does nothing else write to its standard output? If it is "
                           "not made with ecu_main, set describe: false",
                       detail);
    }
}

namespace {

/// What the rules of check_participants() share.
struct Rules
{
    const SimulationConfig& config;
    const std::vector<ParticipantResult>& results;
    std::vector<Finding>& out;
    /// The rules that need every participant's description: not said while a description is
    /// missing (the error of that participant says why), only noted while a participant has
    /// describe: false (it may be the one). Only the enabled participants count: a disabled one is
    /// not in the simulation, whatever its status.
    bool incomplete = false;
    bool unchecked = false;
    std::string not_counted{};

    void add(Severity severity, const char* code, std::size_t line, std::string message, const std::string& participant,
             const std::string& network) const
    {
        out.push_back(Finding{severity, code, std::move(message), line, participant, network});
    }
};

/// The participant `p` (entry `i`) against its entry: name, step, networks, variables, version.
void check_participant(const Rules& rules, std::size_t i, const ParticipantConfig& p, const ParticipantResult& result,
                       const std::map<std::string, const NetworkConfig*>& networks,
                       const std::vector<std::string>& networkNames, std::string_view launcher_version)
{
    const std::string prefix = participant_prefix(p.name);
    if (!p.enabled)
    {
        rules.add(Severity::Note, "participant-disabled", p.line,
            (p.name.empty() ? "participants[" + std::to_string(i) + "]: " : prefix) +
                "disabled (enabled: false, or --disable): it is neither checked nor started, and the other "
                "findings are about the simulation without it",
            p.name, "");
        return;
    }
    if (p.manual && !p.name.empty())
    {
        rules.add(Severity::Note, "participant-manual", p.line,
            prefix + "you start it yourself (manual: true, or --manual): run does not start it, but prints the command "
                     "to start it with" +
                (p.required ? std::string(", and waits for it to join without a time limit (with --timeout-ms, at most that long)")
                            : std::string(", and does not wait for it (required: false)")) +
                (registry_on_network(rules.config)
                     ? "; start it with " + std::string(kListenOnNetworkVariable) +
                           "=1 in its environment, so that the other computers reach it (registry_uri is not a "
                           "loopback address)"
                     : std::string{}),
            p.name, "");
    }
    if (result.status == ParticipantStatus::NotChecked)
    {
        rules.add(Severity::Note, "not-checked", p.line,
            prefix + "not checked (describe: false): vehicle_sil_launch cannot compare its name, step and networks "
                     "with this file",
            p.name, "");
        return;
    }
    if (result.status != ParticipantStatus::Described || !result.ecu)
    {
        return;
    }
    const EcuDescription& ecu = result.ecu->description;
    if (ecu.name != p.name)
    {
        rules.add(Severity::Error, "name-mismatch", p.line,
            prefix + "the executable reports the name '" + ecu.name + "' although it was given --name " + p.name +
                ": its main() must pass all of its arguments to ecu_main",
            p.name, "");
    }
    if (p.step_ms)
    {
        const SimDuration wanted = std::chrono::milliseconds(*p.step_ms);
        if (!ecu.step)
        {
            rules.add(Severity::Error, "step-mismatch", p.step_line,
                prefix + "step_ms is " + std::to_string(*p.step_ms) +
                    ", but the executable reports no step (it runs in real time): remove step_ms",
                p.name, "");
        }
        else if (*ecu.step != wanted)
        {
            const auto ns = ecu.step->count();
            rules.add(Severity::Error, "step-mismatch", p.step_line,
                prefix + "step_ms is " + std::to_string(*p.step_ms) + ", but the executable's step is " +
                    step_text(*ecu.step) +
                    (ns % 1'000'000 == 0 ? ": write step_ms: " + std::to_string(ns / 1'000'000) + ", or remove step_ms"
                                         : ": remove step_ms (it takes whole milliseconds)"),
                p.name, "");
        }
    }
    std::set<std::string> reported;
    for (const auto& network : ecu.networks)
    {
        if (!reported.insert(network.name + "\n" + to_string(network.kind())).second)
        {
            continue;
        }
        const auto it = networks.find(network.name);
        if (it == networks.end())
        {
            const std::string guess = detail::closest(network.name, networkNames);
            rules.add(Severity::Error, "unknown-network", p.line,
                prefix + "the executable uses the network '" + network.name + "' (" + to_string(network.use) +
                    "), which is not in networks: add it to networks with kind: " + to_string(network.kind()) +
                    ", or correct the network's name in the ECU" +
                    (guess.empty() ? "" : " (did you mean '" + guess + "'?)"),
                p.name, network.name);
        }
        else if (it->second->valid && it->second->kind != network.kind())
        {
            rules.add(Severity::Error, "kind-mismatch", p.line,
                prefix + "the executable uses the network '" + network.name + "' as " + to_string(network.kind()) +
                    " (" + to_string(network.use) + "), but networks says kind: " + to_string(it->second->kind) +
                    " (line " + std::to_string(it->second->line) + "): correct the kind, or the network in the ECU",
                p.name, network.name);
        }
    }
    // The variables: whether vehicle_sil_launch run can read and write them (run.hpp, ControlLink).
    if (!result.ecu->description.variables.empty())
    {
        const auto& offer = result.ecu->description.control;
        if (!offer)
        {
            rules.add(Severity::Note, "control-link-not-offered", p.line,
                prefix + "it describes variables, but does not offer them to the simulation (--no-control, or an "
                         "executable of a vehicle-sil that does not yet): vehicle_sil_launch run cannot read or write "
                         "them",
                p.name, "");
        }
        else if (offer->transport != "silkit" || offer->wire_version != static_cast<int>(control::wire::kVersion))
        {
            rules.add(Severity::Warning, "control-link-version", p.line,
                prefix + "it offers its variables as " + offer->transport + " messages of version " +
                    std::to_string(offer->wire_version) + ", and this vehicle_sil_launch speaks silkit messages of version " +
                    std::to_string(control::wire::kVersion) +
                    ", so vehicle_sil_launch run cannot read or write them: build the ECU and vehicle_sil_launch from the "
                    "same version of vehicle-sil",
                p.name, "");
        }
    }
    if (!result.ecu->vehicle_sil_version.empty() && result.ecu->vehicle_sil_version != launcher_version)
    {
        rules.add(Severity::Note, "version-mismatch", p.line,
            prefix + "the executable was built with vehicle-sil " + result.ecu->vehicle_sil_version +
                ", vehicle_sil_launch with " + std::string(launcher_version) +
                ": if they do not work together, build them from the same version",
            p.name, "");
    }
}

/// The rules of a CAN network: the J1939 addresses and NAMEs of its nodes.
void check_can_network(const Rules& rules, const NetworkConfig& network, const std::string& prefix,
                       const std::vector<NodeOnNetwork>& nodes)
{
    // J1939: the same address asked for twice (254: any free address), the same NAME twice.
    // What happens is what vehicle_sil::j1939 does: the lowest NAME wins the address claim; a
    // loser whose NAME is arbitrary address capable claims a free address, any other loser
    // has none (Cannot Claim); a node without a NAME does not claim and keeps sending from its
    // address.
    for (unsigned address = 0; address < j1939::kNullAddress; ++address)
    {
        std::vector<const NodeOnNetwork*> group;
        for (const auto& n : nodes)
        {
            if (n.node.address == address)
            {
                group.push_back(&n);
            }
        }
        if (group.size() < 2)
        {
            continue;
        }
        std::vector<std::string> labels;
        std::vector<std::string> nameless;
        const NodeOnNetwork* winner = nullptr;
        for (const NodeOnNetwork* n : group)
        {
            labels.push_back(n->label);
            if (!n->node.name)
            {
                nameless.push_back(n->label);
            }
            else if (winner == nullptr || *n->node.name < *winner->node.name)
            {
                winner = n;
            }
        }
        std::vector<std::string> stuck;
        std::vector<std::string> movers;
        std::vector<std::string> lowest; // the nodes with the lowest NAME
        for (const NodeOnNetwork* n : group)
        {
            if (winner != nullptr && n->node.name && *n->node.name == *winner->node.name)
            {
                lowest.push_back(n->label);
            }
            if (n != winner && n->node.name)
            {
                (n->node.arbitrary_address_capable() ? movers : stuck).push_back(n->label);
            }
        }
        const std::string asked = prefix + and_list(labels) + (labels.size() == 2 ? " both" : " all") +
                                  " ask for the address " + address_text(static_cast<j1939::Address>(address));
        if (!nameless.empty())
        {
            rules.add(Severity::Error, "address-conflict", network.line,
                asked + ", and " + and_list(nameless) +
                    (nameless.size() == 1 ? " has no NAME, so it does not take part in the address claim and keeps"
                                          : " have no NAME, so they do not take part in the address claim and keep") +
                    " sending from that address: give each node its own address",
                "", network.name);
        }
        else if (lowest.size() > 1)
        {
            // The same NAME: name-conflict says it, and that neither keeps the address.
        }
        else if (!stuck.empty())
        {
            rules.add(Severity::Error, "address-conflict", network.line,
                asked + "; " + winner->label + " wins the address claim (its NAME is the lowest), and " +
                    and_list(stuck) + (stuck.size() == 1 ? " is" : " are") +
                    " not arbitrary address capable, so it is left without an address (Cannot Claim): give each "
                    "node its own address",
                "", network.name);
        }
        else
        {
            rules.add(Severity::Warning, "address-shared", network.line,
                asked + "; " + winner->label + " keeps it (its NAME is the lowest) and " + and_list(movers) +
                    " will claim another address: give each its own address to know where they are",
                "", network.name);
        }
    }
    std::map<std::uint64_t, std::vector<const NodeOnNetwork*>> byName;
    for (const auto& n : nodes)
    {
        if (n.node.name)
        {
            byName[n.node.name->value()].push_back(&n);
        }
    }
    for (const auto& [value, same] : byName)
    {
        if (same.size() < 2)
        {
            continue;
        }
        std::vector<std::string> labels;
        bool oneAddress = same.front()->node.address != j1939::kNullAddress;
        for (const NodeOnNetwork* n : same)
        {
            labels.push_back(n->label);
            oneAddress = oneAddress && n->node.address == same.front()->node.address;
        }
        // An equal NAME loses the address claim: on one address, none of them keeps it.
        const std::string address =
            oneAddress ? ", and they ask for the same address " + address_text(same.front()->node.address) +
                             ", which none of them keeps (an equal NAME loses the address claim)"
                       : std::string{};
        rules.add(Severity::Error, "name-conflict", network.line,
            prefix + and_list(labels) + " have the same J1939 NAME " + vehicle_sil::detail::hex64(value) + address +
                ": each node needs its own NAME (for a second copy of an ECU, change its identity number)",
            "", network.name);
    }
}

/// The rules of a LIN network: one master.
void check_lin_network(const Rules& rules, const NetworkConfig& network, const std::string& prefix,
                       const std::vector<std::string>& masters, const std::vector<std::string>& slaves, bool sayAll)
{
    // LIN: one master.
    std::vector<std::string> distinctMasters;
    for (const auto& m : masters)
    {
        if (std::find(distinctMasters.begin(), distinctMasters.end(), m) == distinctMasters.end())
        {
            distinctMasters.push_back(m);
        }
    }
    if (distinctMasters.size() > 1)
    {
        rules.add(Severity::Error, "lin-masters", network.line,
            prefix + and_list(distinctMasters) + (distinctMasters.size() == 2 ? " are both" : " are all") +
                " LIN masters, but a LIN network has one master: make all but one of them slaves",
            "", network.name);
    }
    else if (masters.size() > 1)
    {
        rules.add(Severity::Error, "lin-masters", network.line,
            prefix + masters.front() + " is a LIN master on it " + std::to_string(masters.size()) +
                " times, but a LIN network has one master: call lin_master() once for this network",
            "", network.name);
    }
    if (masters.empty() && !slaves.empty() && sayAll)
    {
        rules.add(rules.unchecked ? Severity::Note : Severity::Error, "lin-no-master", network.line,
            prefix + and_list(slaves) + (slaves.size() == 1 ? " is a LIN slave" : " are LIN slaves") +
                ", but no participant is the master" + rules.not_counted + ": add the participant that is the master",
            "", network.name);
    }
}

/// The network `network` (of a valid kind): who is on it, then the rules of its kind.
void check_network(const Rules& rules, const NetworkConfig& network)
{
    const std::string prefix = "network '" + network.name + "': ";
    std::vector<std::string> users;
    std::vector<NodeOnNetwork> nodes;
    std::vector<std::string> masters;
    std::vector<std::string> slaves;
    for (std::size_t i = 0; i < rules.config.participants.size() && i < rules.results.size(); ++i)
    {
        if (!is_on_any(rules.config.participants[i], rules.results[i]))
        {
            continue;
        }
        const std::string& who = rules.config.participants[i].name;
        std::vector<J1939NodeDescription> own;
        for (const auto& used : rules.results[i].ecu->description.networks)
        {
            if (used.name != network.name || used.kind() != network.kind)
            {
                continue;
            }
            if (std::find(users.begin(), users.end(), who) == users.end())
            {
                users.push_back(who);
            }
            own.insert(own.end(), used.nodes.begin(), used.nodes.end());
            if (used.use == NetworkUse::LinMaster)
            {
                masters.push_back(who);
            }
            else if (used.use == NetworkUse::LinSlave && std::find(slaves.begin(), slaves.end(), who) == slaves.end())
            {
                slaves.push_back(who);
            }
        }
        for (std::size_t n = 0; n < own.size(); ++n)
        {
            nodes.push_back(
                NodeOnNetwork{own.size() == 1 ? who : who + " (node " + std::to_string(n + 1) + ")", own[n]});
        }
    }
    const bool sayAll = !rules.incomplete;
    const Severity weight = rules.unchecked ? Severity::Note : Severity::Warning;
    if (users.empty())
    {
        if (sayAll)
        {
            rules.add(weight, "unused-network", network.line,
                prefix + "no participant uses it" + rules.not_counted +
                    ": remove it, or check the network names in the ECUs",
                "", network.name);
        }
        return;
    }
    if (users.size() == 1 && sayAll)
    {
        rules.add(weight, "single-participant", network.line,
            prefix + "only " + users.front() + " is on it, so what it sends there reaches no one" + rules.not_counted +
                ": add the other participants of this network, or remove it",
            "", network.name);
    }
    // The rules of the network's kind; a new kind needs its own (or none, but a case here). Only
    // j1939 uses have nodes (read_description() refuses nodes on another use), and only LIN uses
    // are masters or slaves, so the rules of the other kinds would find nothing here.
    switch (network.kind)
    {
    case NetworkKind::Can:
        check_can_network(rules, network, prefix, nodes);
        break;
    case NetworkKind::Lin:
        check_lin_network(rules, network, prefix, masters, slaves, sayAll);
        break;
    }
}

} // namespace

std::vector<Finding> check_participants(const SimulationConfig& config, const std::vector<ParticipantResult>& results,
                                        std::string_view launcher_version)
{
    std::vector<Finding> out;
    std::map<std::string, const NetworkConfig*> networks;
    std::vector<std::string> networkNames;
    for (const auto& network : config.networks)
    {
        networks.emplace(network.name, &network);
        networkNames.push_back(network.name);
    }
    const auto any_enabled = [&](ParticipantStatus status) {
        for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
        {
            if (config.participants[i].enabled && results[i].status == status)
            {
                return true;
            }
        }
        return false;
    };
    Rules rules{config, results, out};
    rules.incomplete = any_enabled(ParticipantStatus::Failed) || any_enabled(ParticipantStatus::Skipped);
    rules.unchecked = any_enabled(ParticipantStatus::NotChecked);
    rules.not_counted = rules.unchecked ? " (participants with describe: false are not counted)" : "";

    // Each participant against its entry.
    for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
    {
        check_participant(rules, i, config.participants[i], results[i], networks, networkNames, launcher_version);
    }
    // Each network: who is on it, the rules of its kind.
    for (const auto& network : config.networks)
    {
        if (network.valid)
        {
            check_network(rules, network);
        }
    }
    return out;
}

} // namespace vehicle_sil::launch
