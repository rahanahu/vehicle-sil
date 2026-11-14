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
#include <optional>
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
    catch (const DescriptionFormatError& e)
    {
        const std::string launcher = vehicle_sil::version();
        const std::string ecu = e.vehicle_sil_version();
        return finding("describe-version",
                       run.command_text + " wrote a description in format " + std::to_string(e.format()) +
                           (ecu.empty() ? std::string() : " (vehicle-sil " + ecu + ")") +
                           ", but this vehicle_sil_launch (vehicle-sil " + launcher + ") reads format " +
                           std::to_string(kEcuDescriptionFormat) +
                           (e.format() > kEcuDescriptionFormat
                                ? ": the executable is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil "
                                  "or a newer one"
                                : ": the executable is of an older vehicle-sil; build it again with this vehicle-sil, or "
                                  "use a vehicle_sil_launch of its vehicle-sil"),
                       "");
    }
    catch (const DescriptionValueError& e)
    {
        const std::string launcher = vehicle_sil::version();
        const std::string ecu = e.vehicle_sil_version();
        return finding("describe-unknown-value",
                       run.command_text + " wrote " + e.key() + " \"" + e.value() +
                           "\", which this vehicle_sil_launch does not know (it knows " + and_list(e.known()) + "): " +
                           (ecu.empty() ? "the executable does not say which vehicle-sil it was built with, and this "
                                          "vehicle_sil_launch is of vehicle-sil " + launcher
                                        : "the executable was built with vehicle-sil " + ecu +
                                              ", this vehicle_sil_launch with vehicle-sil " + launcher) +
                           ". A newer vehicle-sil may add such values without a new format: use a vehicle_sil_launch "
                           "of the executable's vehicle-sil or a newer one",
                       "");
    }
    catch (const DescriptionError& e)
    {
        const std::string what = e.what();
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
    /// The enabled participants (indexes) that may carry networks to real buses: those with
    /// role: bridge if there are any (`bridges_marked`), otherwise those with describe: false and
    /// no role. One that described itself carries the networks it uses; one with describe: false
    /// may carry any of them. A role: bridge whose description failed is left out (its error says
    /// why).
    std::vector<std::size_t> bridges{};
    bool bridges_marked = false;

    void add(Severity severity, const char* code, std::size_t line, std::string message, const std::string& participant,
             const std::string& network) const
    {
        out.push_back(Finding{severity, code, std::move(message), line, participant, network});
    }
};

/// The Ecu function that gives a network this use its name: "j1939()".
const char* ecu_call(NetworkUse use)
{
    switch (use)
    {
    case NetworkUse::Raw: return "can()";
    case NetworkUse::J1939: return "j1939()";
    case NetworkUse::LinMaster: return "lin_master()";
    case NetworkUse::LinSlave: return "lin_slave()";
    case NetworkUse::Ethernet: return "ethernet()";
    }
    return "";
}

/// The Ecu functions that name a network of `kind`, for messages: "can() or j1939()".
std::string ecu_calls(NetworkKind kind)
{
    std::vector<std::string> calls;
    for (const NetworkUse use : kNetworkUses)
    {
        if (kind_of(use) == kind)
        {
            calls.emplace_back(ecu_call(use));
        }
    }
    return detail::or_list(calls);
}

/// The enabled participants that described themselves and use the network `name` as `kind`, in
/// the order of the file, without entry `except`; only those without role: test if `ecus_only`.
std::vector<std::string> users_of(const Rules& rules, const std::string& name, NetworkKind kind, std::size_t except,
                                  bool ecus_only)
{
    std::vector<std::string> users;
    for (std::size_t i = 0; i < rules.config.participants.size() && i < rules.results.size(); ++i)
    {
        const ParticipantConfig& p = rules.config.participants[i];
        if (i == except || !is_on_any(p, rules.results[i]) || (ecus_only && p.role == ParticipantRole::Test))
        {
            continue;
        }
        const auto& used = rules.results[i].ecu->description.networks;
        if (std::any_of(used.begin(), used.end(),
                        [&](const NetworkDescription& n) { return n.name == name && n.kind() == kind; }))
        {
            users.push_back(p.name);
        }
    }
    return users;
}

/// How to fix unknown-network: entry `i` uses `used`, which is not in networks. When another
/// network of the same kind that entry `i` is not on has participants (in networks, or used by
/// another participant without being in networks), the two names may be meant for one bus: name
/// those networks and say who changes what (the configuration cannot rename a network).
std::string unknown_network_fix(const Rules& rules, std::size_t i, const NetworkDescription& used,
                                const std::map<std::string, const NetworkConfig*>& networks,
                                const std::vector<std::string>& networkNames)
{
    const NetworkKind kind = used.kind();
    const std::string kindText = to_string(kind);
    const auto& own = rules.results[i].ecu->description.networks;
    std::set<std::string> seen;
    for (const auto& n : own)
    {
        seen.insert(n.name);
    }
    std::vector<std::string> others;
    for (const auto& network : rules.config.networks)
    {
        if (network.valid && network.kind == kind && !seen.contains(network.name))
        {
            seen.insert(network.name);
            const auto users = users_of(rules, network.name, kind, i, false);
            if (!users.empty())
            {
                others.push_back("'" + network.name + "' (line " + std::to_string(network.line) + ", used by " +
                                 and_list(users) + ")");
            }
        }
    }
    for (std::size_t j = 0; j < rules.config.participants.size() && j < rules.results.size(); ++j)
    {
        if (j == i || !is_on_any(rules.config.participants[j], rules.results[j]))
        {
            continue;
        }
        for (const auto& n : rules.results[j].ecu->description.networks)
        {
            if (n.kind() == kind && !networks.contains(n.name) && seen.insert(n.name).second)
            {
                others.push_back("'" + n.name + "' (not in networks either, used by " +
                                 and_list(users_of(rules, n.name, kind, i, false)) + ")");
            }
        }
    }
    if (others.empty())
    {
        const std::string guess = detail::closest(used.name, networkNames);
        return ": add it to networks with kind: " + kindText +
               ", or have the ECU's author correct the name in its code" +
               (guess.empty() ? "" : " (did you mean '" + guess + "'?)");
    }
    return ". " +
           (others.size() == 1 ? "Another network of kind " + kindText + " in this simulation is "
                               : "Other networks of kind " + kindText + " in this simulation are ") +
           and_list(others) + ": if '" + used.name + "' is meant to be " +
           (others.size() == 1 ? "that bus" : "one of those buses") +
           ", the names must be the same, and the configuration cannot rename a network, so the author of one of the "
           "ECUs changes the name in its code (the name passed to " +
           ecu_calls(kind) + ") and builds it again; if '" + used.name +
           "' is a bus of its own, add it to networks with kind: " + kindText;
}

/// The finding's code of a mapping that does not fit the executable's variables.
const char* variable_map_code(control::MapProblemKind kind)
{
    switch (kind)
    {
    case control::MapProblemKind::Form: return "value";
    case control::MapProblemKind::Unknown: return "variable-map-unknown";
    case control::MapProblemKind::Type: return "variable-map-type";
    case control::MapProblemKind::BadValue: return "variable-map-value";
    case control::MapProblemKind::Collision: return "variable-map-collision";
    case control::MapProblemKind::Present: return "variable-map-absent";
    }
    return "value";
}

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
            rules.add(Severity::Error, "unknown-network", p.line,
                prefix + "the executable uses the network '" + network.name + "' (" + to_string(network.use) +
                    "), which is not in networks" + unknown_network_fix(rules, i, network, networks, networkNames),
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
    // The variable map: every mapping must fit what the executable describes (the run applies the
    // same rules to what its agent announces).
    if (!p.variables.empty())
    {
        const control::MappedSchema seen = seen_variables(p, *result.ecu);
        for (const control::MapProblem& problem : seen.problems())
        {
            std::string more;
            if (problem.kind == control::MapProblemKind::Unknown)
            {
                std::vector<std::string> names;
                for (const auto& v : ecu.variables)
                {
                    names.push_back(v.name);
                }
                const std::string guess = detail::closest(p.variables[problem.entry].from, names);
                more = ": " +
                       (guess.empty() ? (names.empty() ? std::string("it describes none.") : "its variables are " + detail::join(names) + ".")
                                      : "did you mean '" + guess + "'?") +
                       " Correct the name, or write { absent: true } if this executable does not have the variable";
            }
            rules.add(Severity::Error, variable_map_code(problem.kind), p.variable_lines[problem.entry],
                prefix + "variables: " + problem.message + more, p.name, "");
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

/// The rules of an Ethernet network: each participant has its own address. `macs` are the
/// participants on it with the address each reported (empty: none).
void check_ethernet_network(const Rules& rules, const NetworkConfig& network, const std::string& prefix,
                            const std::vector<std::pair<std::string, std::optional<eth::MacAddress>>>& macs)
{
    std::map<eth::MacAddress, std::vector<std::string>> byAddress;
    std::vector<std::string> unknown;
    for (const auto& [who, mac] : macs)
    {
        if (!mac)
        {
            unknown.push_back(who);
        }
        else if (auto& same = byAddress[*mac]; std::find(same.begin(), same.end(), who) == same.end())
        {
            same.push_back(who);
        }
    }
    for (const auto& [mac, same] : byAddress)
    {
        if (same.size() > 1)
        {
            rules.add(Severity::Error, "ethernet-duplicate-mac", network.line,
                prefix + and_list(same) + " have the same MAC address " + mac.to_string() +
                    ", so their frames cannot be told apart: give each its own address (eth::ChannelOptions::mac, "
                    "and the source address of the frames it sends). If they share the address on purpose, leave "
                    "ChannelOptions::mac unset on all but one of them: the check then only notes it "
                    "(ethernet-mac-unknown), and frames they send in the same step arrive in an order that can "
                    "differ between runs",
                "", network.name);
        }
    }
    if (!unknown.empty())
    {
        rules.add(Severity::Note, "ethernet-mac-unknown", network.line,
            prefix + and_list(unknown) + (unknown.size() == 1 ? " reports" : " report") +
                " no MAC address, so the check cannot tell whether two participants use the same one: pass the "
                "address to ethernet() (eth::ChannelOptions::mac), or make sure by hand that the addresses differ",
            "", network.name);
    }
}

/// A participant for testing (role: test, index `talker`) with J1939 nodes on `network`, which a
/// bridge may carry to a real bus (Rules::bridges; `users`: who described itself on the network).
void check_test_on_bridged_network(const Rules& rules, const NetworkConfig& network, std::size_t talker,
                                   const std::vector<std::string>& users)
{
    std::vector<std::string> on;    // bridges that described themselves on the network
    std::vector<std::string> maybe; // bridges with describe: false (their networks are not known)
    for (const std::size_t b : rules.bridges)
    {
        const std::string& name = rules.config.participants[b].name;
        if (rules.results[b].status == ParticipantStatus::NotChecked)
        {
            maybe.push_back(name);
        }
        else if (std::find(users.begin(), users.end(), name) != users.end())
        {
            on.push_back(name);
        }
    }
    if (on.empty() && maybe.empty())
    {
        return;
    }
    const ParticipantConfig& p = rules.config.participants[talker];
    const auto bridge_words = [](const std::vector<std::string>& names) {
        return (names.size() == 1 ? "the bridge " : "the bridges ") + and_list(names) + " (role: bridge)";
    };
    // Without role: bridge anywhere, `maybe` are participants that may be bridges (and `on` is empty).
    std::string where;
    if (!on.empty())
    {
        where = bridge_words(on) + (on.size() == 1 ? " is" : " are") + " on it too";
    }
    else if (rules.bridges_marked)
    {
        where = bridge_words(maybe) + " may carry it (describe: false, so the check cannot see " +
                (maybe.size() == 1 ? "its" : "their") + " networks)";
    }
    else
    {
        where = and_list(maybe) +
                (maybe.size() == 1 ? " has describe: false and no role, so it may be a bridge that carries"
                                   : " have describe: false and no role, so they may be bridges that carry") +
                " this network to a real bus";
    }
    const std::string fix = "--disable " + (on.empty() ? maybe : on).front();
    rules.add(Severity::Warning, "test-participant-talks-on-bridged-network", p.line,
        participant_prefix(p.name) + "it has role: test and J1939 nodes on network '" + network.name + "', and " +
            where + ": what it sends there goes out on the real bus too, its address claims among it, and a real "
            "ECU with the same address fights it for the address: let it only listen there (no J1939 nodes), or "
            "leave the bridge out of runs with it (" + fix + ", or enabled: false in a variant)" +
            (rules.bridges_marked
                 ? std::string{}
                 : "; if " + and_list(maybe) +
                       (maybe.size() == 1 ? " is not a bridge, write role: ecu for it"
                                          : " are not bridges, write role: ecu for them") +
                       " (and role: bridge for a bridge)"),
        p.name, network.name);
}

/// The network `network` (of a valid kind): who is on it, then the rules of its kind.
void check_network(const Rules& rules, const NetworkConfig& network)
{
    const std::string prefix = "network '" + network.name + "': ";
    std::vector<std::string> users;
    std::vector<std::string> tests;         // the users with role: test (not counted as ECUs)
    std::vector<std::size_t> talkingTests;  // role: test participants with J1939 nodes here
    std::vector<NodeOnNetwork> nodes;
    std::vector<std::string> masters;
    std::vector<std::string> slaves;
    std::vector<std::pair<std::string, std::optional<eth::MacAddress>>> macs;
    for (std::size_t i = 0; i < rules.config.participants.size() && i < rules.results.size(); ++i)
    {
        if (!is_on_any(rules.config.participants[i], rules.results[i]))
        {
            continue;
        }
        const std::string& who = rules.config.participants[i].name;
        const bool test = rules.config.participants[i].role == ParticipantRole::Test;
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
                if (test)
                {
                    tests.push_back(who);
                }
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
            else if (used.use == NetworkUse::Ethernet)
            {
                macs.emplace_back(who, used.mac);
            }
        }
        for (std::size_t n = 0; n < own.size(); ++n)
        {
            nodes.push_back(
                NodeOnNetwork{own.size() == 1 ? who : who + " (node " + std::to_string(n + 1) + ")", own[n]});
        }
        if (test && !own.empty())
        {
            talkingTests.push_back(i);
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
    // Participants for testing (role: test) are not counted: a spy on the network of one ECU
    // does not make that network right.
    const std::size_t ecus = users.size() - tests.size();
    if (ecus <= 1 && sayAll)
    {
        std::string ecu;
        for (const auto& who : users)
        {
            if (std::find(tests.begin(), tests.end(), who) == tests.end())
            {
                ecu = who;
            }
        }
        // Other networks of the same kind with one ECU each, another one: perhaps one bus that the
        // ECUs name differently.
        std::vector<std::string> alone;
        if (ecus == 1)
        {
            for (const auto& other : rules.config.networks)
            {
                if (&other == &network || !other.valid || other.kind != network.kind)
                {
                    continue;
                }
                const auto them = users_of(rules, other.name, other.kind, rules.config.participants.size(), true);
                if (them.size() == 1 && them.front() != ecu)
                {
                    alone.push_back("'" + other.name + "' (line " + std::to_string(other.line) + ", only " +
                                    them.front() + ")");
                }
            }
        }
        const std::string sameBus =
            alone.empty()
                ? std::string{}
                : "; " +
                      (alone.size() == 1 ? "the network " + alone.front() + " of kind " + to_string(network.kind) +
                                               " also has one ECU: if it is meant to be the same bus"
                                         : "the networks " + and_list(alone) + " of kind " + to_string(network.kind) +
                                               " also have one ECU each: if one of them is meant to be the same bus") +
                      ", the ECUs name it differently, and the configuration cannot rename a network, so the author "
                      "of one of them changes the name in its code";
        rules.add(weight, "single-participant", network.line,
            prefix +
                (tests.empty() ? "only " + ecu + " is on it, so what it sends there reaches no one" + rules.not_counted +
                                     ": add the other participants of this network, or remove it"
                 : ecus == 1
                     ? "only " + ecu + " is on it besides the participants for testing (role: test: " + and_list(tests) +
                           "), so what it sends there reaches no other ECU" + rules.not_counted +
                           ": add the other ECUs of this network, or remove it"
                     : "only participants for testing (role: test: " + and_list(tests) +
                           ") are on it, so there is no ECU for them there" + rules.not_counted +
                           ": add the ECUs of this network, or remove it") +
                sameBus,
            "", network.name);
    }
    for (const std::size_t talker : talkingTests)
    {
        check_test_on_bridged_network(rules, network, talker, users);
    }
    // The rules of the network's kind; a new kind needs its own (or none, but a case here). Only
    // j1939 uses have nodes (read_description() refuses nodes on another use), only LIN uses are
    // masters or slaves and only ethernet uses have a MAC address, so the rules of the other
    // kinds would find nothing here.
    switch (network.kind)
    {
    case NetworkKind::Can:
        check_can_network(rules, network, prefix, nodes);
        break;
    case NetworkKind::Lin:
        check_lin_network(rules, network, prefix, masters, slaves, sayAll);
        break;
    case NetworkKind::Ethernet:
        check_ethernet_network(rules, network, prefix, macs);
        break;
    }
}

} // namespace

control::MappedSchema seen_variables(const ParticipantConfig& participant, const ReportedEcu& ecu)
{
    return control::MappedSchema::make(ecu.description.variables, participant.variables);
}

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
    for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
    {
        rules.bridges_marked = rules.bridges_marked ||
                               (config.participants[i].enabled && config.participants[i].role == ParticipantRole::Bridge);
    }
    for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
    {
        const ParticipantConfig& p = config.participants[i];
        const bool known = results[i].status == ParticipantStatus::NotChecked ||
                           (results[i].status == ParticipantStatus::Described && results[i].ecu);
        if (p.enabled && known &&
            (rules.bridges_marked ? p.role == ParticipantRole::Bridge
                                  : results[i].status == ParticipantStatus::NotChecked && !p.role_written))
        {
            rules.bridges.push_back(i);
        }
    }

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
