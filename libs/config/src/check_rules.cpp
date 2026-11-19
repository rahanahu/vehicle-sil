// SPDX-License-Identifier: MIT
//
// The rules of the check: what a run of --describe means (interpret_describe()) and the comparison
// of the descriptions with the configuration (check_participants()): each participant against
// its entry, then each network by its kind. Pure functions.
#include "vehicle_sil/config/check.hpp"

#include "vehicle_sil/config/detail/check_words.hpp"
#include "vehicle_sil/config/detail/seen_ecu.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/core/registry_uri.hpp"
#include "vehicle_sil/json/json_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace vehicle_sil::config {

namespace {

using detail::and_list;
using detail::hex2;
using detail::step_text;

std::string participant_prefix(const std::string& name) { return "participant '" + name + "': "; }

using detail::dll_start_failure;
using detail::status_hex;

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

/// One J1939 node on a network: who has it, for messages, and where its values come from.
struct NodeOnNetwork
{
    /// "Engine", or "Engine (node 2)" if Engine has more than one there; with " (set by
    /// j1939_nodes, line 14)" after it if an entry of j1939_nodes gave it its values.
    std::string label;
    J1939NodeDescription node; ///< the values after j1939_nodes
    std::size_t participant = 0; ///< the index of its participant in the configuration
    std::size_t index = 0;       ///< its index among its participant's nodes on the network
    j1939::Address coded_address = j1939::kNullAddress; ///< its address in the executable's code
    std::size_t line = 0;        ///< the line of the j1939_nodes entry that gave it its values; 0: none
    std::size_t own_nodes = 1;   ///< how many nodes its participant has on the network
    bool has_entries = false;    ///< its participant has j1939_nodes for the network
    bool takes_entries = false;  ///< its executable says it takes --j1939-node ("j1939-node" in features)
    /// Another node of its participant there has the same address in the code: an entry of
    /// j1939_nodes cannot pick it (j1939-node-indistinct).
    bool shares_coded_address = false;
};

/// The values that the fixes of the findings on one network suggest, in the order of the findings:
/// a later fix does not suggest them again, so that every fix of a network can be written at once.
struct SuggestedValues
{
    std::vector<j1939::Address> addresses{};
    std::vector<j1939::Name> names{};
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
        const bool bridge = participant.role == ParticipantRole::Bridge;
        if (run.signal)
        {
            ended = "was ended by the signal " + std::to_string(*run.signal);
            hint = "it crashed or was killed while it built the ECU: fix the ECU (its standard error may say where)";
        }
        else if (!dll_start_failure(*run.exit_code).empty())
        {
            // Windows did not start it: the same for an ECU and a bridge, and nothing on its
            // standard error says which DLL.
            ended = dll_start_failure(*run.exit_code);
            hint = detail::kDllFix;
            if (bridge)
            {
                hint += "; if this computer does not use the bridge, leave it out with vehicle_sil_launch --disable " +
                        participant.name;
            }
        }
#ifdef _WIN32
        else if (static_cast<std::uint32_t>(*run.exit_code) >= 0xC0000000u)
        {
            // A Windows exception code, such as 0xC0000005.
            ended = "ended with the exception code " + status_hex(static_cast<std::uint32_t>(*run.exit_code));
            hint = "it crashed while it built the ECU: fix the ECU (its standard error may say where)";
        }
#endif
        else
        {
            ended = "ended with exit code " + std::to_string(*run.exit_code);
            if (*run.exit_code != ecu_executable::kExitUsage)
            {
                hint = "its set-up failed (see its standard error): fix the ECU, or set describe: false if it is not "
                       "made with ecu_main";
            }
            else if (bridge)
            {
                // A bridge takes --describe and --name: exit code 4 is about the rest of its command.
                hint = "the bridge refused its arguments (its standard error says why): fix the command; "
                       "vehicle_sil_launch adds --name itself, so do not write --participant or --name in the command";
            }
            else
            {
                hint = "it did not take --describe or --name: if it is not made with ecu_main, set describe: false";
            }
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
    /// The networks of each participant that counts (is_on_any()) as the simulation sees them
    /// (seen_networks()); empty for the others. Every rule reads the networks from here.
    std::vector<std::vector<SeenNetwork>> seen{};
    /// The networks of the file (simulation's names) that a line of network_names of a participant
    /// that counts gives to a network its executable does not have (network-map-unknown). That
    /// participant is meant to be on them and is not seen there, so who is on them is not known:
    /// the rules that count the participants of a network do not speak of these.
    std::set<std::string> held{};
    /// The participants (indexes) and networks (simulation's names) where an entry of j1939_nodes
    /// takes no node: the participant's nodes there are not in the rules of the addresses and NAMEs
    /// (the entry's own error comes first, and the values the file means are not known).
    std::set<std::pair<std::size_t, std::string>> j1939_held{};
    /// The version of vehicle_sil_launch, for the messages that say which version to build with.
    std::string launcher_version{};
    /// The networks that --connect connects to a real bus: their bridge (bridge_participant()) is
    /// on them, though it is not a participant of the file.
    std::set<std::string> outside{};

    void add(Severity severity, const char* code, std::size_t line, std::string message, const std::string& participant,
             const std::string& network) const
    {
        out.push_back(Finding{severity, code, std::move(message), line, participant, network});
    }
};

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
        const auto& used = rules.seen[i];
        if (std::any_of(used.begin(), used.end(),
                        [&](const SeenNetwork& n) { return n.network.name == name && n.network.kind() == kind; }))
        {
            users.push_back(p.name);
        }
    }
    return users;
}

/// The lines of the entry's network_names whose value is no network of its executable (`seen`: its
/// networks), in the file's order: network-map-unknown.
std::vector<NetworkName> unknown_lines(const ParticipantConfig& p, const std::vector<SeenNetwork>& seen)
{
    std::vector<NetworkName> out;
    for (const NetworkName& entry : renamed_networks(p))
    {
        if (std::none_of(seen.begin(), seen.end(), [&entry](const SeenNetwork& n) { return n.executable_name == entry.from; }))
        {
            out.push_back(entry);
        }
    }
    return out;
}

/// true if a line of network_names with a wrong value (unknown_lines()) is why the executable's
/// network `network` is not under a name of the file: the value is a typing error of its name; or
/// the value is like none of the executable's names, and the network is of the kind of the line's
/// key (it may be the one that is meant). unknown-network is then not said of it: the line's own
/// error says what to correct, and "add network_names: { <key>: ... }" would write the key twice.
bool waits_for_a_line(const std::vector<NetworkName>& unknown, const std::vector<SeenNetwork>& seen,
                      const NetworkDescription& network, const std::map<std::string, const NetworkConfig*>& networks)
{
    std::vector<std::string> names;
    for (const SeenNetwork& n : seen)
    {
        names.push_back(n.executable_name);
    }
    for (const NetworkName& entry : unknown)
    {
        const std::string guess = detail::closest(entry.from, names);
        const auto key = networks.find(entry.name);
        if (guess.empty() ? key != networks.end() && key->second->valid && key->second->kind == network.kind()
                          : guess == network.name)
        {
            return true;
        }
    }
    return false;
}

/// "'VehicleCAN'", and with the executable's name for a network that the entry renames:
/// "'VehicleCAN' ('J1939_CAN' in the executable)".
std::string both_names(const SeenNetwork& seen)
{
    return "'" + seen.network.name + "'" +
           (seen.renamed() ? " ('" + seen.executable_name + "' in the executable)" : std::string{});
}

/// How to fix unknown-network: entry `i` uses `used` (under the executable's name: a network that
/// the entry renames is in networks), which is not in networks. When another network of the same
/// kind that entry `i` is not on has participants (in networks, or used by another participant
/// without being in networks), the two names may be meant for one bus: name those networks and
/// say what to write in this file (network_names of the participant; the executable stays as it
/// is).
std::string unknown_network_fix(const Rules& rules, std::size_t i, const NetworkDescription& used,
                                const std::map<std::string, const NetworkConfig*>& networks,
                                const std::vector<std::string>& networkNames)
{
    const NetworkKind kind = used.kind();
    const std::string kindText = to_string(kind);
    const std::string who = rules.config.participants[i].name;
    std::set<std::string> seen;
    for (const auto& n : rules.seen[i])
    {
        seen.insert(n.network.name);
    }
    // Nor a network that a line of the entry names already (with a value the executable does not
    // have: network-map-unknown): a second line with that key is not the fix.
    for (const NetworkName& entry : renamed_networks(rules.config.participants[i]))
    {
        seen.insert(entry.name);
    }
    std::vector<std::string> others;
    std::vector<std::string> listed; // those of `others` that are in networks
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
                listed.push_back(network.name);
            }
        }
    }
    for (std::size_t j = 0; j < rules.config.participants.size() && j < rules.results.size(); ++j)
    {
        if (j == i || !is_on_any(rules.config.participants[j], rules.results[j]))
        {
            continue;
        }
        for (const auto& n : rules.seen[j])
        {
            const std::string& name = n.network.name;
            if (n.network.kind() == kind && !networks.contains(name) && seen.insert(name).second)
            {
                others.push_back("'" + name + "' (not in networks either, used by " +
                                 and_list(users_of(rules, name, kind, i, false)) + ")");
            }
        }
    }
    if (others.empty())
    {
        const std::string guess = detail::closest(used.name, networkNames);
        return ": add it to networks with kind: " + kindText +
               ", or, if it is a network of this file under another name, add network_names: { <name in networks>: " +
               used.name + " } to this participant" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)");
    }
    const std::string start = ". " +
                              (others.size() == 1 ? "Another network of kind " + kindText + " in this simulation is "
                                                  : "Other networks of kind " + kindText + " in this simulation are ") +
                              and_list(others) + ": if '" + used.name + "' is meant to be " +
                              (others.size() == 1 ? "that bus" : "one of those buses");
    const std::string own = "; if '" + used.name + "' is a bus of its own, add it to networks with kind: " + kindText;
    if (listed.empty())
    {
        // No name to map to yet: one of the names goes to networks first.
        return start + ", choose one name for the bus: add it to networks with kind: " + kindText +
               ", and add network_names: { <that name>: <the executable's name> } to each participant whose executable "
               "names the bus differently (<simulation's name>: <executable's name>; the executables stay as they are)" +
               own;
    }
    return start + ", say so in this file: add network_names: { " +
           (others.size() == 1 ? listed.front() : std::string("<one of them, in networks>")) + ": " + used.name +
           " } to the participant '" + who +
           "' (<simulation's name>: <executable's name>; the executable stays as it is)" + own;
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

/// The network_names of entry `i` against what its executable described: whether it takes
/// --map-network, whether it has each network the lines name, and whether two of its networks
/// end up under one name. The rules after it read the networks with the lines applied, also
/// when one of these errors is there: their findings are about the simulation the file means.
void check_network_names(const Rules& rules, std::size_t i, const ParticipantConfig& p, const ReportedEcu& reported,
                         std::string_view launcher_version)
{
    const std::vector<NetworkName> renames = renamed_networks(p);
    if (renames.empty())
    {
        return;
    }
    const std::string prefix = participant_prefix(p.name);
    const std::vector<SeenNetwork>& seen = rules.seen[i];
    if (!SeenEcu(p, reported).takes(ecu_executable::kFeatureMapNetwork))
    {
        std::vector<std::string> own;
        for (const NetworkName& entry : renames)
        {
            own.push_back("'" + entry.from + "' instead of '" + entry.name + "'");
        }
        rules.add(Severity::Error, "network-map-unsupported", renames.front().line,
            prefix + "network_names needs an executable that takes --map-network, and this one does not say that it "
                     "does (no \"map-network\" in the features of its --describe" +
                (reported.vehicle_sil_version.empty() ? std::string{}
                                                      : "; it was built with vehicle-sil " + reported.vehicle_sil_version) +
                "): have its author build it again with vehicle-sil " + std::string(launcher_version) +
                " or later, or remove network_names and use the executable's own " +
                (own.size() == 1 ? "name" : "names") + " in networks (" + and_list(own) +
                "), with network_names for the other participants on " + (own.size() == 1 ? "it" : "them"),
            p.name, "");
    }
    // What the executable has, under its own names, once each.
    std::vector<std::string> names;
    std::vector<std::string> shown;
    for (const SeenNetwork& n : seen)
    {
        if (std::find(names.begin(), names.end(), n.executable_name) == names.end())
        {
            names.push_back(n.executable_name);
        }
        const std::string text = "'" + n.executable_name + "' (" + to_string(n.network.use) + ")";
        if (std::find(shown.begin(), shown.end(), text) == shown.end())
        {
            shown.push_back(text);
        }
    }
    for (const NetworkName& entry : renames)
    {
        if (std::find(names.begin(), names.end(), entry.from) == names.end())
        {
            const std::string guess = detail::closest(entry.from, names);
            rules.add(Severity::Error, "network-map-unknown", entry.line,
                prefix + "network_names says that '" + entry.name + "' is the executable's network '" + entry.from +
                    "', but the executable has no network of that name (" +
                    (shown.empty() ? std::string("it has no networks") : "its networks: " + detail::join(shown)) +
                    "): correct the value" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)") +
                    ", or remove the line",
                p.name, entry.name);
            continue;
        }
        // The executable's own network of the simulation's name, which no line gives another name.
        const bool taken = std::any_of(seen.begin(), seen.end(), [&entry](const SeenNetwork& n) {
            return !n.renamed() && n.network.name == entry.name;
        });
        if (taken)
        {
            rules.add(Severity::Error, "network-map-collision", entry.line,
                prefix + "with network_names, the executable's '" + entry.from + "' becomes '" + entry.name +
                    "', and the executable also has a network it calls '" + entry.name +
                    "' itself, so two of its networks would be one bus: give the executable's own '" + entry.name +
                    "' another name of networks too (network_names: { <another name in networks>: " + entry.name +
                    " }), or remove this line",
                p.name, entry.name);
        }
    }
}

/// The addresses of `nodes` in the code, each once, in their order: "0x00", "any (254)".
std::vector<std::string> coded_addresses(const std::vector<J1939NodeDescription>& nodes)
{
    std::vector<std::string> out;
    for (const J1939NodeDescription& node : nodes)
    {
        const std::string text = j1939::coded_address_text(node.address);
        if (std::find(out.begin(), out.end(), text) == out.end())
        {
            out.push_back(text);
        }
    }
    return out;
}

/// The number of `nodes` whose address in the code is `address`.
std::size_t nodes_at(const std::vector<J1939NodeDescription>& nodes, j1939::Address address)
{
    return static_cast<std::size_t>(
        std::count_if(nodes.begin(), nodes.end(), [address](const J1939NodeDescription& n) { return n.address == address; }));
}

/// The settings of the NAME that `entry` gives, as written: "identity_number and ecu_instance".
std::string name_settings(const j1939::NodeOverride& entry)
{
    std::vector<std::string> out;
    if (entry.identity_number)
    {
        out.emplace_back("identity_number");
    }
    if (entry.function_instance)
    {
        out.emplace_back("function_instance");
    }
    if (entry.ecu_instance)
    {
        out.emplace_back("ecu_instance");
    }
    return and_list(out);
}

/// The finding of an entry of j1939_nodes that takes no node of the J1939 network `seen` of
/// entry `i` (`match`: what apply_node_overrides() said).
void j1939_entry_not_applied(const Rules& rules, const ParticipantConfig& p, const SeenNetwork& seen,
                             const J1939NodeMatch& match)
{
    const J1939NodeEntry& entry = p.j1939_nodes[match.entry];
    const std::string& network = seen.network.name;
    const std::string at = participant_prefix(p.name) + "j1939_nodes (line " + std::to_string(match.line) + ") ";
    const std::vector<J1939NodeDescription>& nodes = seen.coded_nodes;
    const auto add = [&](const char* code, std::string message) {
        rules.add(Severity::Error, code, match.line, std::move(message), p.name, network);
    };
    switch (match.result.match)
    {
    case j1939::NodeOverrideMatch::Applied:
    case j1939::NodeOverrideMatch::Skipped:
        return;
    case j1939::NodeOverrideMatch::NoNodes:
        add("j1939-node-unknown",
            at + "changes a node on '" + network + "', but the executable has no node there (nodes that it adds after "
                 "it starts are not described and cannot be changed): remove the entry, or correct network");
        return;
    case j1939::NodeOverrideMatch::NoNodeAtFrom:
    {
        const std::vector<std::string> addresses = coded_addresses(nodes);
        // The addresses that pick one node (from can name them), and those that hold more than one.
        std::vector<j1939::Address> single;
        std::vector<std::string> shared;
        for (const J1939NodeDescription& node : nodes)
        {
            const std::size_t count = nodes_at(nodes, node.address);
            const std::string text = "the " + std::to_string(count) + " nodes at " + j1939::coded_address_text(node.address);
            if (count == 1)
            {
                single.push_back(node.address);
            }
            else if (std::find(shared.begin(), shared.end(), text) == shared.end())
            {
                shared.push_back(text);
            }
        }
        std::string fix = ": write from: " + (single.size() == 1 ? j1939::node_override_address_text(single.front())
                                                                : std::string("with one of them"));
        if (single.empty())
        {
            fix = ". T" + and_list(shared).substr(1) + " cannot be told apart, so this file cannot change them: remove the entry";
        }
        else if (!shared.empty())
        {
            fix += " (" + and_list(shared) + " cannot be told apart, and this file cannot change them)";
        }
        add("j1939-node-unknown",
            at + "changes the node that the executable's code puts at " + j1939::coded_address_text(*entry.node.from) +
                " on '" + network + "', but the executable has no node at that address there (its nodes there: " +
                detail::join(addresses) + "; nodes that it adds after it starts are not described and cannot be "
                "changed). from is the address in the executable's code (the one the table shows without j1939_nodes), "
                "not the new address" + fix);
        return;
    }
    case j1939::NodeOverrideMatch::Ambiguous:
    {
        // The addresses that pick one node, and those that hold more than one.
        std::vector<std::string> single;
        std::vector<std::string> shared;
        std::vector<j1939::Address> seenAddresses;
        j1939::Address example = j1939::kNullAddress;
        for (const J1939NodeDescription& node : nodes)
        {
            if (std::find(seenAddresses.begin(), seenAddresses.end(), node.address) != seenAddresses.end())
            {
                continue;
            }
            seenAddresses.push_back(node.address);
            const std::size_t count = nodes_at(nodes, node.address);
            if (count == 1)
            {
                if (single.empty())
                {
                    example = node.address;
                }
                single.push_back(j1939::coded_address_text(node.address));
            }
            else
            {
                shared.push_back("the " + std::to_string(count) + " nodes at " + j1939::coded_address_text(node.address));
            }
        }
        add("j1939-node-ambiguous",
            at + "has no from, and the executable has " + std::to_string(nodes.size()) + " nodes on '" + network + "'" +
                (single.empty() ? std::string{} : " (" + detail::join(single) + ")") +
                (shared.empty() ? std::string{}
                                : "; " + and_list(shared) + " cannot be told apart, and this file cannot change them") +
                ": say which one with from: <the address in the executable's code>" +
                (single.empty() ? std::string{} : ", for example from: " + j1939::node_override_address_text(example)));
        return;
    }
    case j1939::NodeOverrideMatch::Indistinct:
    {
        const j1939::Address address = entry.node.from ? *entry.node.from : nodes.front().address;
        add("j1939-node-indistinct",
            at + "cannot pick a node: the executable's code puts " + std::to_string(nodes_at(nodes, address)) +
                " nodes at " + j1939::coded_address_text(address) + " on '" + network +
                "', and j1939_nodes tells nodes apart by that address, so this file cannot change them: remove the "
                "entry, and ask the ECU's author to give the nodes addresses of their own in its code, or to take them "
                "as arguments");
        return;
    }
    case j1939::NodeOverrideMatch::CannotApply:
        break;
    }
    const std::string node = j1939::coded_address_text(nodes[match.result.node].address);
    switch (match.result.error.value_or(j1939::NodeOverrideError::FieldOutOfRange))
    {
    case j1939::NodeOverrideError::NameFieldWithoutName:
    {
        const std::string fields = name_settings(entry.node);
        add("j1939-node-no-name",
            at + "sets " + fields + ", but the node at " + node + " on '" + network +
                "' has no NAME (it does not claim its address): remove " + fields +
                "; for this node only address can be changed");
        return;
    }
    case j1939::NodeOverrideError::AnyAddressNotCapable:
        add("j1939-node-any-address",
            at + "sets address: 254 (any free address), but the NAME of the node at " + node + " on '" + network +
                "' is not arbitrary address capable, which this file cannot change: write an address of 0..253");
        return;
    case j1939::NodeOverrideError::AnyAddressWithoutName:
        add("j1939-node-any-address",
            at + "sets address: 254 (any free address), but the node at " + node + " on '" + network +
                "' has no NAME, so it cannot claim a free address: write an address of 0..253");
        return;
    case j1939::NodeOverrideError::FieldOutOfRange:
    case j1939::NodeOverrideError::AddressOutOfRange:
        // Not from a file (its reader refuses such a value).
        add("value", at + "has " + j1939::node_override_range_problem(entry.node));
        return;
    }
}

/// The entry `e` of j1939_nodes names a network on which the executable has no J1939 network.
void j1939_entry_without_network(const Rules& rules, const ParticipantConfig& p, const std::vector<SeenNetwork>& seen,
                                 std::size_t e)
{
    const J1939NodeEntry& entry = p.j1939_nodes[e];
    const std::string at = participant_prefix(p.name) + "j1939_nodes (line " + std::to_string(entry.line) +
                           ") changes a node on '" + entry.network + "', but the executable has no J1939 network";
    std::vector<std::string> names;
    std::vector<std::string> own; // the networks that keep the executable's name
    for (const SeenNetwork& n : seen)
    {
        if (n.network.use != NetworkUse::J1939)
        {
            continue;
        }
        names.push_back("'" + n.network.name + "'" + (n.renamed() ? " (" + n.executable_name + " in its code)" : std::string{}));
        if (!n.renamed())
        {
            own.push_back(n.network.name);
        }
    }
    std::string message;
    if (names.empty())
    {
        message = at + ": remove the entry";
    }
    else
    {
        message = at + " '" + entry.network + "' (its J1939 networks: " + detail::join(names) + "): " +
                  (own.size() == 1 ? "if its '" + own.front() + "' is the simulation's '" + entry.network +
                                         "', add network_names: { " + entry.network + ": " + own.front() +
                                         " }; otherwise correct network"
                                   : std::string("correct network"));
    }
    rules.add(Severity::Error, "j1939-node-unknown", entry.line, std::move(message), p.name, entry.network);
}

/// The note "j1939-node-changed": what the entries of j1939_nodes of entry `i`, all applied, change.
void note_changed_nodes(const Rules& rules, const ParticipantConfig& p, const std::vector<SeenNetwork>& seen)
{
    std::vector<std::string> clauses;
    std::vector<std::string> movedFrom;
    std::vector<std::string> oldNames;
    std::vector<std::string> networks;
    for (const SeenNetwork& n : seen)
    {
        if (n.network.use != NetworkUse::J1939)
        {
            continue;
        }
        bool first = true;
        for (std::size_t k = 0; k < n.network.nodes.size(); ++k)
        {
            const J1939NodeDescription& now = n.network.nodes[k];
            const J1939NodeDescription& coded = n.coded_nodes[k];
            const bool moved = now.address != coded.address;
            // An entry cannot give a node without a NAME one (j1939-node-no-name).
            const bool renamed = now.name && coded.name && *now.name != *coded.name;
            if (n.node_lines[k] == 0 || (!moved && !renamed))
            {
                continue;
            }
            const bool any = coded.address == j1939::kNullAddress;
            const std::string names = renamed ? vehicle_sil::json::hex64(now.name->value()) + " (" +
                                                    vehicle_sil::json::hex64(coded.name->value()) + " in its code)"
                                              : std::string{};
            std::string clause = first ? "on '" + n.network.name + "', " : std::string{};
            if (moved)
            {
                clause += (any ? std::string("the node that takes any free address in its code")
                               : "the node at " + hex2(coded.address) + " in its code") +
                          " is at " + j1939::coded_address_text(now.address) +
                          (renamed ? ", with the NAME " + names : std::string{});
            }
            else
            {
                clause += (any ? std::string("the node that takes any free address") : "the node at " + hex2(coded.address)) +
                          " has the NAME " + names;
            }
            clauses.push_back(std::move(clause));
            if (first)
            {
                networks.push_back(n.network.name);
            }
            first = false;
            if (moved && !any)
            {
                movedFrom.push_back(hex2(coded.address));
            }
            if (renamed)
            {
                oldNames.push_back(vehicle_sil::json::hex64(coded.name->value()));
            }
        }
    }
    if (clauses.empty())
    {
        return; // every value is the code's
    }
    std::vector<std::string> relied;
    if (!movedFrom.empty())
    {
        const std::string was = detail::or_list(movedFrom);
        relied.push_back("an ECU whose code has " + was + " written in it for " + p.name +
                         " (as a destination, or to tell senders apart) still uses " + was + ", and " + p.name +
                         "'s own code may still have " + was + " in a filter or in the data it sends");
    }
    if (!oldNames.empty())
    {
        relied.push_back("an ECU that looks " + p.name + " up by its whole NAME " + detail::or_list(oldNames) +
                         " does not find it");
    }
    std::string message = participant_prefix(p.name) + "j1939_nodes changes its nodes: ";
    for (std::size_t c = 0; c < clauses.size(); ++c)
    {
        message += (c == 0 ? "" : "; ") + clauses[c];
    }
    if (!relied.empty())
    {
        message += ". Only " + p.name + " changes, and the check cannot see who relies on the old values: " +
                   and_list(relied, ", and ") + "; if messages do not reach " + p.name +
                   " or are taken for another ECU's, the author of that code has to change it";
    }
    const auto first = std::find_if(p.j1939_nodes.begin(), p.j1939_nodes.end(),
                                    [](const J1939NodeEntry& e) { return e.node.changes(); });
    rules.add(Severity::Note, "j1939-node-changed", first->line, std::move(message), p.name,
              networks.size() == 1 ? networks.front() : std::string{});
}

/// The j1939_nodes of entry `i` against what its executable described: whether it takes
/// --j1939-node, and whether each entry that changes something takes a node (the values are
/// applied in seen_networks(); this reads what it found, it does not match again).
void check_j1939_node_entries(const Rules& rules, std::size_t i, const ParticipantConfig& p, const ReportedEcu& reported,
                              const std::map<std::string, const NetworkConfig*>& networks, std::string_view launcher_version)
{
    if (j1939_node_arguments(p).empty())
    {
        return;
    }
    const std::optional<Finding> unsupported = j1939_nodes_unsupported(p, reported, launcher_version);
    if (unsupported)
    {
        rules.out.push_back(*unsupported);
    }
    const std::vector<SeenNetwork>& seen = rules.seen[i];
    bool allApplied = true;
    for (const SeenNetwork& n : seen)
    {
        for (const J1939NodeMatch& match : n.j1939_matches)
        {
            if (match.result.match != j1939::NodeOverrideMatch::Applied && match.result.match != j1939::NodeOverrideMatch::Skipped)
            {
                allApplied = false;
                j1939_entry_not_applied(rules, p, n, match);
            }
        }
    }
    // A network that a wrong line of network_names keeps away: network-map-unknown says it. The
    // only J1939 network of the executable under its own name, which is not in networks: a missing
    // line of network_names, which unknown-network says (one cause, one finding).
    const std::vector<NetworkName> unknown = unknown_lines(p, seen);
    std::vector<const SeenNetwork*> own;
    for (const SeenNetwork& n : seen)
    {
        if (n.network.use == NetworkUse::J1939 && !n.renamed())
        {
            own.push_back(&n);
        }
    }
    const bool unmapped = own.size() == 1 && !networks.contains(own.front()->network.name) &&
                          !waits_for_a_line(unknown, seen, own.front()->network, networks);
    for (const std::size_t e : unmatched_j1939_entries(p, seen))
    {
        allApplied = false;
        if (!unmapped && std::none_of(unknown.begin(), unknown.end(),
                                      [&](const NetworkName& line) { return line.name == p.j1939_nodes[e].network; }))
        {
            j1939_entry_without_network(rules, p, seen, e);
        }
    }
    if (allApplied && !unsupported)
    {
        note_changed_nodes(rules, p, seen);
    }
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
    const SeenEcu ecu(p, *result.ecu);
    if (ecu.name() != p.name)
    {
        rules.add(Severity::Error, "name-mismatch", p.line,
            prefix + "the executable reports the name '" + ecu.name() + "' although it was given --name " + p.name +
                ": its main() must pass all of its arguments to ecu_main",
            p.name, "");
    }
    if (p.step_ms)
    {
        const SimDuration wanted = std::chrono::milliseconds(*p.step_ms);
        if (!ecu.step())
        {
            rules.add(Severity::Error, "step-mismatch", p.step_line,
                prefix + "step_ms is " + std::to_string(*p.step_ms) +
                    ", but the executable reports no step (it runs in real time): remove step_ms",
                p.name, "");
        }
        else if (*ecu.step() != wanted)
        {
            const auto ns = ecu.step()->count();
            rules.add(Severity::Error, "step-mismatch", p.step_line,
                prefix + "step_ms is " + std::to_string(*p.step_ms) + ", but the executable's step is " +
                    step_text(*ecu.step()) +
                    (ns % 1'000'000 == 0 ? ": write step_ms: " + std::to_string(ns / 1'000'000) + ", or remove step_ms"
                                         : ": remove step_ms (it takes whole milliseconds)"),
                p.name, "");
        }
    }
    check_network_names(rules, i, p, *result.ecu, launcher_version);
    check_j1939_node_entries(rules, i, p, *result.ecu, networks, launcher_version);
    const std::vector<NetworkName> unknown = unknown_lines(p, rules.seen[i]);
    std::set<std::string> reported;
    for (const SeenNetwork& seen : rules.seen[i])
    {
        const NetworkDescription& network = seen.network;
        if (!reported.insert(network.name + "\n" + to_string(network.kind())).second)
        {
            continue;
        }
        const auto it = networks.find(network.name);
        if (it == networks.end() && waits_for_a_line(unknown, rules.seen[i], network, networks))
        {
            // One cause, one finding: network-map-unknown of the line says it.
        }
        else if (it == networks.end())
        {
            rules.add(Severity::Error, "unknown-network", p.line,
                prefix + "the executable uses the network '" + network.name + "' (" + to_string(network.use) +
                    "), which is not in networks" + unknown_network_fix(rules, i, network, networks, networkNames),
                p.name, network.name);
        }
        else if (it->second->valid && it->second->kind != network.kind())
        {
            rules.add(Severity::Error, "kind-mismatch", p.line,
                prefix + "the executable uses the network " + both_names(seen) + " as " + to_string(network.kind()) +
                    " (" + to_string(network.use) + "), but networks says kind: " + to_string(it->second->kind) +
                    " (line " + std::to_string(it->second->line) + "): correct the kind, " +
                    (seen.renamed() ? "the line of network_names (line " + std::to_string(seen.line) + "), " : "") +
                    "or the network in the ECU",
                p.name, network.name);
        }
    }
    // The variables: whether vehicle_sil_launch run can read and write them (run_outcome.hpp, ControlLink).
    if (ecu.has_variables())
    {
        const auto& offer = ecu.control();
        if (!offer)
        {
            rules.add(Severity::Note, "control-link-not-offered", p.line,
                prefix + "it describes variables, but does not offer them to the simulation (--no-control, or an "
                         "executable of a vehicle-sil that does not yet): vehicle_sil_launch run cannot read or write "
                         "them",
                p.name, "");
        }
        else if (offer->transport != "silkit")
        {
            rules.add(Severity::Warning, "control-link-version", p.line,
                prefix + "it offers its variables over the transport " + offer->transport +
                    ", and this vehicle_sil_launch reads and writes variables over silkit only, so vehicle_sil_launch run "
                    "cannot read or write them: make the executable offer them over silkit (as ecu_main does), or use a "
                    "vehicle_sil_launch that knows the transport " + offer->transport,
                p.name, "");
        }
        else if (offer->wire_version != static_cast<int>(control::wire::kVersion))
        {
            rules.add(Severity::Warning, "control-link-version", p.line,
                prefix + "it offers its variables as silkit messages of version " + std::to_string(offer->wire_version) +
                    ", and this vehicle_sil_launch speaks version " + std::to_string(control::wire::kVersion) +
                    ", so vehicle_sil_launch run cannot read or write them: " + detail::control_version_fix(offer->wire_version),
                p.name, "");
        }
        else if (offer->wire_revision < static_cast<int>(control::wire::kRevision))
        {
            // Both sides skip what the other's revision added, so the variables work; only what
            // the newer revisions added is missing (revision 1: the availability).
            rules.add(Severity::Note, "control-link-revision", p.line,
                prefix + "it offers its variables as silkit messages of revision " + std::to_string(offer->wire_revision) +
                    ", older than revision " + std::to_string(control::wire::kRevision) +
                    " of this vehicle_sil_launch: its variables are read and written as usual, but none of them can "
                    "report its availability, so every one of them looks Active: if it should report one, build "
                    "the executable with this vehicle-sil and declare those variables with "
                    "VariableOptions::availability",
                p.name, "");
        }
    }
    // The variable map: every mapping must fit what the executable describes (the run applies the
    // same rules to what its agent announces).
    if (!p.variables.empty())
    {
        const control::MappedSchema seen = ecu.variables();
        for (const control::MapProblem& problem : seen.problems())
        {
            std::string more;
            if (problem.kind == control::MapProblemKind::Unknown)
            {
                std::vector<std::string> names;
                for (const auto& v : ecu.own_variables())
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

/// The entry of j1939_nodes of `n`'s participant on `network` that a fix for `n` changes: the one
/// that gave `n` its values, else one that changes nothing there, with `n`'s address in the code as
/// its from or without from (the fix then adds from if the participant has more nodes there).
struct EntryToChange
{
    std::size_t line = 0; ///< 0: none, the fix is a new entry
    bool add_from = false;
};

EntryToChange entry_to_change(const Rules& rules, const NetworkConfig& network, const NodeOnNetwork& n)
{
    if (n.line != 0)
    {
        return EntryToChange{n.line, false};
    }
    const std::vector<J1939NodeEntry>& entries = rules.config.participants[n.participant].j1939_nodes;
    for (const bool withFrom : {true, false})
    {
        for (const J1939NodeEntry& entry : entries)
        {
            if (entry.network == network.name && (withFrom ? entry.node.from == n.coded_address : !entry.node.from))
            {
                return EntryToChange{entry.line, !withFrom && n.own_nodes > 1};
            }
        }
    }
    return EntryToChange{};
}

/// How to make the nodes `group` of a conflict on `network` differ in this file, after the message
/// of the rule: give one of them (the last one in the file whose executable takes j1939_nodes and
/// that an entry can pick) another address (`address`) and/or identity number (`identity`), with
/// values that no node on the network (`all`, the values after j1939_nodes), no entry of
/// j1939_nodes for the network and no earlier fix there (`suggested`, which this adds to) has.
/// If the node has an entry already (also one that changes nothing), the fix is a change of that
/// entry: a second entry for the node would be j1939-node-duplicate.
std::string conflict_fix(const Rules& rules, const NetworkConfig& network, const std::vector<NodeOnNetwork>& all,
                         const std::vector<const NodeOnNetwork*>& group, bool address, bool identity,
                         SuggestedValues& suggested)
{
    const NodeOnNetwork* example = nullptr;
    bool anyTakes = false;
    for (const NodeOnNetwork* n : group)
    {
        anyTakes = anyTakes || n->takes_entries;
        if (n->takes_entries && !n->shares_coded_address &&
            (example == nullptr || n->participant > example->participant ||
             (n->participant == example->participant && n->index > example->index)))
        {
            example = n;
        }
    }
    if (example == nullptr && anyTakes)
    {
        return "this file cannot change them, as j1939_nodes tells a participant's nodes apart by their addresses in "
               "the code, and the code of each executable here that takes j1939_nodes puts another of its nodes at the "
               "same address: have the author of one of them change it in its code";
    }
    if (example == nullptr)
    {
        return "this file cannot change them, as their executables do not take j1939_nodes (no \"j1939-node\" in the "
               "features of their --describe): build one of them again with vehicle-sil " +
               rules.launcher_version + " or later and set it in j1939_nodes, or have its author change it in its code";
    }
    std::vector<std::string> settings;
    if (address)
    {
        // Also not the address of an entry whose participant is not counted (describe: false, or an
        // entry that takes no node).
        std::vector<j1939::Address> written;
        for (const ParticipantConfig& p : rules.config.participants)
        {
            for (const J1939NodeEntry& entry : p.j1939_nodes)
            {
                if (p.enabled && entry.network == network.name && entry.node.address)
                {
                    written.push_back(*entry.node.address);
                }
            }
        }
        std::string free = "<a free address>";
        for (unsigned a = 0; a < j1939::kNullAddress; ++a)
        {
            const auto taken = [a](const std::vector<j1939::Address>& in) {
                return std::find(in.begin(), in.end(), a) != in.end();
            };
            if (std::none_of(all.begin(), all.end(), [a](const NodeOnNetwork& n) { return n.node.address == a; }) &&
                !taken(written) && !taken(suggested.addresses))
            {
                free = j1939::node_override_address_text(static_cast<j1939::Address>(a));
                suggested.addresses.push_back(static_cast<j1939::Address>(a));
                break;
            }
        }
        settings.push_back("address: " + free);
    }
    if (identity && example->node.name)
    {
        std::string other = "<another number>";
        for (std::uint32_t id = example->node.name->identity_number() + 1; id <= 2097151; ++id)
        {
            j1939::Name name = *example->node.name;
            name.set_identity_number(id);
            if (std::none_of(all.begin(), all.end(), [&name](const NodeOnNetwork& n) { return n.node.name == name; }) &&
                std::find(suggested.names.begin(), suggested.names.end(), name) == suggested.names.end())
            {
                other = std::to_string(id);
                suggested.names.push_back(name);
                break;
            }
        }
        settings.push_back("identity_number: " + other);
    }
    const std::string& who = rules.config.participants[example->participant].name;
    const std::string from = "from: " + j1939::node_override_address_text(example->coded_address);
    const EntryToChange entry = entry_to_change(rules, network, *example);
    if (entry.line != 0)
    {
        if (entry.add_from)
        {
            settings.insert(settings.begin(), from);
        }
        return "to do it in this file, set " + detail::join(settings) + " in the entry of j1939_nodes on line " +
               std::to_string(entry.line) + " (participant '" + who + "'), keeping its other settings";
    }
    if (example->own_nodes > 1 || example->has_entries)
    {
        settings.insert(settings.begin(), from);
    }
    return "to do it in this file, write under participant '" + who + "': j1939_nodes: [{ network: " + network.name +
           ", " + detail::join(settings) + " }]";
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
    SuggestedValues suggested;
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
                    " sending from that address: give each node its own address; " +
                    conflict_fix(rules, network, nodes, group, true, false, suggested),
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
                    "node its own address; " + conflict_fix(rules, network, nodes, group, true, false, suggested),
                "", network.name);
        }
        else
        {
            rules.add(Severity::Warning, "address-shared", network.line,
                asked + "; " + winner->label + " keeps it (its NAME is the lowest) and " + and_list(movers) +
                    " will claim another address: give each its own address to know where they are; " +
                    conflict_fix(rules, network, nodes, group, true, false, suggested),
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
            prefix + and_list(labels) + " have the same J1939 NAME " + vehicle_sil::json::hex64(value) + address +
                ": each node needs its own NAME; " + conflict_fix(rules, network, nodes, same, oneAddress, true, suggested),
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
    const auto talked_on = [&] {
        for (const SeenNetwork& seen : rules.seen[talker])
        {
            if (seen.network.name == network.name && seen.network.kind() == network.kind && !seen.network.nodes.empty())
            {
                return both_names(seen);
            }
        }
        return "'" + network.name + "'";
    };
    const std::string consequence =
        ": what it sends there goes out on the real bus too, its address claims among it, and a real ECU with the same "
        "address fights it for the address: let it only listen there (no J1939 nodes), or ";
    if (rules.outside.contains(network.name))
    {
        const ParticipantConfig& p = rules.config.participants[talker];
        rules.add(Severity::Warning, "test-participant-talks-on-bridged-network", p.line,
                  participant_prefix(p.name) + "it has role: test and J1939 nodes on network " + talked_on() +
                      ", and --connect connects that network to a real bus" + consequence +
                      "do not connect the network in runs with it (leave --connect " + network.name + " out)",
                  p.name, network.name);
        return;
    }
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
        participant_prefix(p.name) + "it has role: test and J1939 nodes on network " + talked_on() + ", and " +
            where + consequence + "leave the bridge out of runs with it (" + fix + ", or enabled: false in a variant)" +
            (rules.bridges_marked
                 ? std::string{}
                 : "; if " + and_list(maybe) +
                       (maybe.size() == 1 ? " is not a bridge, write role: ecu for it"
                                          : " are not bridges, write role: ecu for them") +
                       " (and role: bridge for a bridge)"),
        p.name, network.name);
}

/// For single-participant: what to write so that `network`, which only the ECU `ecu` is on, is the
/// bus `other` of networks ("": one of several of them): "add network_names: { J1939_CAN: VehicleCAN } to
/// the participant 'Dashboard'". The value is the executable's name; a line that renames the
/// network already is said to be replaced.
std::string same_bus_lines(const Rules& rules, const NetworkConfig& network, const std::string& ecu, const std::string& other)
{
    std::vector<std::string> lines;
    std::vector<std::string> replaced;
    for (std::size_t i = 0; i < rules.config.participants.size() && i < rules.seen.size(); ++i)
    {
        if (rules.config.participants[i].name != ecu)
        {
            continue;
        }
        for (const SeenNetwork& seen : rules.seen[i])
        {
            const std::string line = (other.empty() ? std::string("<that network>") : other) + ": " + seen.executable_name;
            if (seen.network.name == network.name && seen.network.kind() == network.kind &&
                std::find(lines.begin(), lines.end(), line) == lines.end())
            {
                lines.push_back(line);
                if (seen.renamed())
                {
                    replaced.push_back(seen.network.name + ": " + seen.executable_name);
                }
            }
        }
    }
    return "add network_names: { " + detail::join(lines) + " } to the participant '" + ecu + "'" +
           (replaced.empty() ? std::string{} : " (in place of " + detail::join(replaced) + ")");
}

/// A network that --connect connects to a real bus (Rules::outside), and an enabled role: bridge
/// participant that carries it (`users`: who described itself on it), or may (describe: false).
void check_connect_loops(const Rules& rules, const NetworkConfig& network, const std::vector<std::string>& users)
{
    const std::string fix = ": leave one of the two bridges out (--disable ";
    for (std::size_t i = 0; i < rules.config.participants.size() && i < rules.results.size(); ++i)
    {
        const ParticipantConfig& p = rules.config.participants[i];
        if (!p.enabled || p.role != ParticipantRole::Bridge)
        {
            continue;
        }
        const std::string leave = fix + p.name + ", or leave --connect " + network.name + " out)";
        if (rules.results[i].status == ParticipantStatus::NotChecked)
        {
            rules.add(Severity::Warning, "connect-maybe-loop", p.line,
                      participant_prefix(p.name) + "it has role: bridge and describe: false, so the check cannot see "
                      "whether it carries network '" + network.name + "', which --connect connects to a real bus: if it "
                      "does, frames go round between the two bridges" + leave,
                      p.name, network.name);
        }
        else if (std::find(users.begin(), users.end(), p.name) != users.end())
        {
            rules.add(Severity::Error, "connect-loop", p.line,
                      participant_prefix(p.name) + "it has role: bridge and carries network '" + network.name +
                          "' to a real bus, and --connect connects the network too: frames would go round between the "
                          "two bridges" + leave,
                      p.name, network.name);
        }
    }
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
        const ParticipantConfig& p = rules.config.participants[i];
        const std::string& who = p.name;
        const bool test = p.role == ParticipantRole::Test;
        std::vector<std::pair<const SeenNetwork*, std::size_t>> own;
        for (const SeenNetwork& seen : rules.seen[i])
        {
            const NetworkDescription& used = seen.network;
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
            for (std::size_t k = 0; k < used.nodes.size(); ++k)
            {
                own.emplace_back(&seen, k);
            }
            if (used.use == NetworkUse::LinMaster)
            {
                masters.push_back(who);
            }
            else if (used.use == NetworkUse::LinSlave && std::find(slaves.begin(), slaves.end(), who) == slaves.end())
            {
                slaves.push_back(who);
            }
            // A bridge (role: bridge) without an address carries the frames of others: it has no
            // address of its own to compare (no ethernet-mac-unknown for it).
            else if (used.use == NetworkUse::Ethernet && (used.mac || p.role != ParticipantRole::Bridge))
            {
                macs.emplace_back(who, used.mac);
            }
        }
        // Not while an entry of j1939_nodes takes no node here: its own error says what to correct.
        if (!rules.j1939_held.contains({i, network.name}))
        {
            const bool takes = SeenEcu(p, *rules.results[i].ecu).takes(ecu_executable::kFeatureJ1939Node);
            const bool entries = std::any_of(p.j1939_nodes.begin(), p.j1939_nodes.end(),
                                             [&network](const J1939NodeEntry& e) { return e.network == network.name; });
            for (std::size_t n = 0; n < own.size(); ++n)
            {
                const auto& [seen, k] = own[n];
                const std::size_t line = seen->node_lines[k];
                const j1939::Address coded = seen->coded_nodes[k].address;
                const bool shared = std::any_of(own.begin(), own.end(), [&](const auto& other) {
                    return (other.first != seen || other.second != k) && other.first->coded_nodes[other.second].address == coded;
                });
                nodes.push_back(NodeOnNetwork{
                    (own.size() == 1 ? who : who + " (node " + std::to_string(n + 1) + ")") +
                        (line != 0 ? " (set by j1939_nodes, line " + std::to_string(line) + ")" : std::string{}),
                    seen->network.nodes[k], i, n, coded, line, own.size(), entries, takes, shared});
            }
        }
        if (test && !own.empty())
        {
            talkingTests.push_back(i);
        }
    }
    const bool connected = rules.outside.contains(network.name);
    if (connected)
    {
        check_connect_loops(rules, network, users);
    }
    // Not while a line of network_names with a wrong value keeps a participant from it.
    const bool sayAll = !rules.incomplete && !rules.held.contains(network.name);
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
    // The bridge of --connect is on a connected network too: what is sent there reaches the real bus.
    const std::size_t ecus = users.size() - tests.size();
    if (ecus <= 1 && sayAll && !connected)
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
        std::vector<std::string> aloneNames;
        if (ecus == 1)
        {
            for (const auto& other : rules.config.networks)
            {
                if (&other == &network || !other.valid || other.kind != network.kind || rules.outside.contains(other.name))
                {
                    continue;
                }
                const auto them = users_of(rules, other.name, other.kind, rules.config.participants.size(), true);
                if (them.size() == 1 && them.front() != ecu)
                {
                    alone.push_back("'" + other.name + "' (line " + std::to_string(other.line) + ", only " +
                                    them.front() + ")");
                    aloneNames.push_back(other.name);
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
                      ", keep one of the two names: remove '" + network.name + "' from networks and " +
                      same_bus_lines(rules, network, ecu, aloneNames.size() == 1 ? aloneNames.front() : std::string{});
        const std::string who =
            tests.empty() ? "only " + ecu + " is on it"
            : ecus == 1   ? "only " + ecu + " is on it besides the participants for testing (role: test: " +
                              and_list(tests) + ")"
                          : "only participants for testing (role: test: " + and_list(tests) + ") are on it";
        const std::string lead =
            tests.empty() ? who + ", so what it sends there reaches no one"
            : ecus == 1   ? who + ", so what it sends there reaches no other ECU"
                          : who + ", so there is no ECU for them there";
        if (network.external)
        {
            // The network is written for a run that connects it: there the outside is the other side.
            rules.add(Severity::Note, "single-participant", network.line,
                prefix + who + rules.not_counted +
                    "; it has external (the settings for connecting it outside), so what it sends there reaches an "
                    "outside party in a run that connects it, and no one in this run: connect the network outside to "
                    "use it, or ignore this in a run that does not",
                "", network.name);
        }
        else
        {
            rules.add(weight, "single-participant", network.line,
                prefix + lead + rules.not_counted +
                    (tests.empty() ? ": add the other participants of this network, or remove it"
                     : ecus == 1   ? ": add the other ECUs of this network, or remove it"
                                   : ": add the ECUs of this network, or remove it") +
                    "; if it talks only to equipment outside, connect it outside to run it (external says what passes)" +
                    sameBus,
                "", network.name);
        }
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

std::optional<Finding> j1939_nodes_unsupported(const ParticipantConfig& participant, const ReportedEcu& ecu,
                                               std::string_view launcher_version,
                                               std::span<const J1939NodeSettingFeature> table)
{
    const SeenEcu seen(participant, ecu);
    const auto has = [&seen](std::string_view word) { return seen.takes(word); };
    const std::string prefix = participant_prefix(participant.name);
    const std::string build = "have its author build it again with vehicle-sil " + std::string(launcher_version) + " or later";
    const std::vector<J1939NodeFeatureNeed> needs = required_j1939_node_features(participant, table);
    if (needs.empty())
    {
        return std::nullopt;
    }
    if (!has(ecu_executable::kFeatureJ1939Node))
    {
        return Finding{Severity::Error, "j1939-node-unsupported",
                       prefix + "j1939_nodes needs an executable that takes --j1939-node, and this one does not say that "
                                "it does (no \"j1939-node\" in the features of its --describe" +
                           (ecu.vehicle_sil_version.empty() ? std::string{}
                                                            : "; it was built with vehicle-sil " + ecu.vehicle_sil_version) +
                           "): " + build + "; until then its addresses and NAMEs change only in its code",
                       needs.front().line, participant.name, ""};
    }
    for (const J1939NodeFeatureNeed& need : needs)
    {
        if (!has(need.feature))
        {
            return Finding{Severity::Error, "j1939-node-unsupported",
                           prefix + "j1939_nodes (line " + std::to_string(need.line) + ") sets " + need.setting +
                               ", which needs \"" + need.feature + "\" in the features of the executable's --describe, "
                               "and this one does not have it: " + build + ", or leave " + need.setting + " out",
                           need.line, participant.name, ""};
        }
    }
    return std::nullopt;
}

std::vector<Finding> check_participants(const SimulationConfig& config, const std::vector<ParticipantResult>& results,
                                        std::string_view launcher_version, std::span<const std::string> outside)
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
    rules.launcher_version = std::string(launcher_version);
    rules.outside.insert(outside.begin(), outside.end());
    rules.seen.resize(config.participants.size());
    for (std::size_t i = 0; i < config.participants.size() && i < results.size(); ++i)
    {
        if (is_on_any(config.participants[i], results[i]))
        {
            rules.seen[i] = SeenEcu(config.participants[i], *results[i].ecu).seen_networks();
            for (const NetworkName& entry : unknown_lines(config.participants[i], rules.seen[i]))
            {
                rules.held.insert(entry.name);
            }
            for (const SeenNetwork& seen : rules.seen[i])
            {
                if (std::any_of(seen.j1939_matches.begin(), seen.j1939_matches.end(), [](const J1939NodeMatch& m) {
                        return m.result.match != j1939::NodeOverrideMatch::Applied &&
                               m.result.match != j1939::NodeOverrideMatch::Skipped;
                    }))
                {
                    rules.j1939_held.emplace(i, seen.network.name);
                }
            }
        }
    }
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

} // namespace vehicle_sil::config
