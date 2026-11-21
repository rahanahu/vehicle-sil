// SPDX-License-Identifier: MIT
//
// The participants, descriptions and configurations that the tests of the check build
// (check_participants()): a configuration with the ECUs that reported their descriptions, and the
// result of the check as lines. Shared by the rules (tests/config) and by the reports of the
// launcher's library (tests/launch), which run the same configurations through the outputs.
#pragma once

#include "launch_test_util.hpp"

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/config/connect.hpp"
#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/ecu/describe.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::config::test::check_setup {

using namespace std::chrono_literals;

constexpr std::uint64_t kEngineName = 0x00000000FE000001; // identity number 1, Manufacturer Code 0x7F0
constexpr std::uint64_t kArbitraryBit = std::uint64_t{1} << 63;
/// The end of address-conflict, address-shared and name-conflict when no executable of the nodes
/// takes j1939_nodes (as the descriptions of these tests, which have no "features").
const std::string kNoEntries = "this file cannot change them, as their executables do not take j1939_nodes (no "
                               "\"j1939-node\" in the features of their --describe): build one of them again with "
                               "vehicle-sil 1.0.0 or later and set it in j1939_nodes, or have its author change it in its "
                               "code";

inline NetworkDescription on_j1939(const std::string& name, std::vector<J1939NodeDescription> nodes)
{
    return NetworkDescription{name, NetworkUse::J1939, std::move(nodes)};
}

inline J1939NodeDescription node(j1939::Address address, std::optional<std::uint64_t> name = std::nullopt)
{
    return J1939NodeDescription{address, name ? std::optional(j1939::Name{*name}) : std::nullopt};
}

inline ReportedEcu ecu(const std::string& name, std::optional<SimDuration> step, std::vector<NetworkDescription> networks,
                const std::string& version = "1.0.0")
{
    return ReportedEcu{version, EcuDescription{name, step, std::move(networks), {}}};
}

inline ParticipantConfig participant(const std::string& name, std::size_t line, std::optional<std::int64_t> step_ms = std::nullopt,
                              bool describe = true)
{
    ParticipantConfig p;
    p.name = name;
    p.command = {name};
    p.describe = describe;
    p.step_ms = step_ms;
    p.line = line;
    p.command_line = line + 1;
    p.step_line = line + 2;
    return p;
}

inline ParticipantConfig with_role(ParticipantConfig p, ParticipantRole role)
{
    p.role = role;
    p.role_written = true;
    return p;
}

/// A configuration with `networks` and the participants `entries` (each with what it reported,
/// or nothing for describe: false).
struct Setup
{
    SimulationConfig config;
    std::vector<ParticipantResult> results;

    Setup& network(const std::string& name, NetworkKind kind, std::size_t line)
    {
        config.networks.push_back(NetworkConfig{name, kind, line, true});
        return *this;
    }
    Setup& described(ParticipantConfig p, ReportedEcu reported)
    {
        config.participants.push_back(std::move(p));
        results.push_back(ParticipantResult{ParticipantStatus::Described, std::move(reported)});
        return *this;
    }
    Setup& unchecked(const std::string& name, std::size_t line)
    {
        config.participants.push_back(participant(name, line, std::nullopt, false));
        results.push_back(ParticipantResult{ParticipantStatus::NotChecked, std::nullopt});
        return *this;
    }
    [[nodiscard]] std::string check() const { return lines(check_participants(config, results, "1.0.0")); }
};

/// Two ECUs on one CAN network, every rule satisfied.
inline Setup good_pair()
{
    Setup s;
    s.network("J1939_CAN", NetworkKind::Can, 3)
        .described(participant("Engine", 6, 10), ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})}))
        .described(participant("Dashboard", 9), ecu("Dashboard", 5ms, {on_j1939("J1939_CAN", {node(0x28)})}));
    return s;
}

inline control::VariableInfo variable(const std::string& name)
{
    control::VariableInfo v{};
    v.name = name;
    return v;
}

inline DescribeRun run(std::optional<int> code, std::string out = "", std::string err = "")
{
    DescribeRun r;
    r.command_text = "engine/engine_ecu --describe --name Engine";
    r.exit_code = code;
    r.standard_output = std::move(out);
    r.standard_error = std::move(err);
    r.timeout = 10000ms;
    return r;
}

/// The good pair with Dashboard disabled.
inline Setup without_dashboard()
{
    Setup s = good_pair();
    s.config.participants[1].enabled = false;
    s.results[1] = ParticipantResult{ParticipantStatus::Disabled, std::nullopt};
    return s;
}


/// `reported`, of an executable that takes --map-network (as ecu_main says in "features").
inline ReportedEcu taking_map(ReportedEcu reported)
{
    reported.description.features = {"map-network"};
    return reported;
}

/// `p` with the lines of network_names `names` (simulation's name, executable's name), one per
/// line of the file from `line` on.
inline ParticipantConfig named(ParticipantConfig p, std::size_t line, const std::vector<std::pair<std::string, std::string>>& names)
{
    for (const auto& [name, from] : names)
    {
        p.network_names.push_back(NetworkName{name, from, line++});
    }
    return p;
}

inline NetworkDescription raw(const std::string& name) { return NetworkDescription{name, NetworkUse::Raw, {}}; }

inline std::string network_of(const Finding& f) { return f.code + "(" + f.participant + "," + f.network + ")"; }


/// `reported` of an executable that takes --map-network and --j1939-node.
inline ReportedEcu taking_nodes(ReportedEcu reported)
{
    reported.description.features = {"map-network", "j1939-node"};
    return reported;
}

/// `p` with the entries of j1939_nodes `entries` (network, settings), one per line of the file from
/// `line` on.
inline ParticipantConfig with_nodes(ParticipantConfig p, std::size_t line,
                             const std::vector<std::pair<std::string, j1939::NodeOverride>>& entries)
{
    for (const auto& [network, node] : entries)
    {
        p.j1939_nodes.push_back(J1939NodeEntry{{network, node}, line++});
    }
    return p;
}

inline j1939::NodeOverride change(std::optional<j1939::Address> from, std::optional<j1939::Address> address,
                           std::optional<std::uint32_t> identity = std::nullopt)
{
    return j1939::NodeOverride{from, address, identity, std::nullopt, std::nullopt};
}

/// Two copies of one engine (the same NAME at 0x00, on J1939_CAN) and a dashboard (0x28, no NAME):
/// Engine at line 6, Dashboard at 9 (good_pair()), Engine2 at 12 with `entries` from line 14.
inline Setup two_engines(const std::vector<std::pair<std::string, j1939::NodeOverride>>& entries = {})
{
    Setup s = good_pair();
    s.results[0].ecu = taking_nodes(*s.results[0].ecu);
    s.described(with_nodes(participant("Engine2", 12, 10), 14, entries),
                taking_nodes(ecu("Engine2", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})})));
    return s;
}

/// The lines of the findings that have `part` in them; with `part` "E ", the errors.
inline std::string only(const std::string& findings, const std::string& part)
{
    std::istringstream in(findings);
    std::string out;
    for (std::string line; std::getline(in, line);)
    {
        if (part == "E " ? line.rfind(part, 0) == 0 : line.find(part) != std::string::npos)
        {
            out += line + "\n";
        }
    }
    return out;
}

/// The entries of the participant's j1939_nodes: "<network>=<settings>@<line> " each.
inline std::string nodes_of(const ParticipantConfig& p)
{
    std::string out;
    for (const J1939NodeEntry& e : p.j1939_nodes)
    {
        out += e.network + "=" + j1939::format_node_override(e.node) + "@" + std::to_string(e.line) + " ";
    }
    return out;
}

/// Reads "key: value, key: value" of a fix into `entry` (numbers as written: 0x01, 254, 2).
inline void read_settings(const std::string& text, J1939NodeEntry& entry)
{
    std::istringstream in(text);
    for (std::string item; std::getline(in, item, ',');)
    {
        item.erase(0, item.find_first_not_of(' '));
        const std::size_t colon = item.find(": ");
        const std::string key = item.substr(0, colon);
        const std::string value = item.substr(colon + 2);
        if (key == "network")
        {
            entry.network = value;
            continue;
        }
        const auto number = static_cast<std::uint32_t>(std::stoul(value, nullptr, 0));
        if (key == "from")
        {
            entry.node.from = static_cast<j1939::Address>(number);
        }
        else if (key == "address")
        {
            entry.node.address = static_cast<j1939::Address>(number);
        }
        else if (key == "identity_number")
        {
            entry.node.identity_number = number;
        }
        else
        {
            throw unit::Failure("a setting that the fixes do not write: " + key);
        }
    }
}

/// Writes every fix that address-conflict, address-shared and name-conflict of `s` give into its
/// configuration, all at once, as a person or an agent would: "write under participant 'P':
/// j1939_nodes: [{ ... }]" adds that entry (on lines 100, 101, ...), "set ... in the entry of
/// j1939_nodes on line N (participant 'P'), keeping its other settings" changes that entry. The
/// number of fixes written.
inline std::size_t write_fixes(Setup& s)
{
    const auto entries_of = [&s](const std::string& message, std::size_t at) -> std::vector<J1939NodeEntry>& {
        const std::size_t start = message.find('\'', at) + 1;
        const std::string who = message.substr(start, message.find('\'', start) - start);
        for (ParticipantConfig& p : s.config.participants)
        {
            if (p.name == who)
            {
                return p.j1939_nodes;
            }
        }
        throw unit::Failure("a fix for a participant that is not there: " + who);
    };
    std::size_t written = 0;
    for (const Finding& f : check_participants(s.config, s.results, "1.0.0"))
    {
        if (f.code != "address-conflict" && f.code != "address-shared" && f.code != "name-conflict")
        {
            continue;
        }
        const std::string& m = f.message;
        const std::string add = "write under participant '";
        const std::string set = "to do it in this file, set ";
        const std::string in = " in the entry of j1939_nodes on line ";
        if (const std::size_t at = m.find(add); at != std::string::npos)
        {
            const std::size_t open = m.find("[{ ", at) + 3;
            J1939NodeEntry entry;
            entry.line = 100 + written;
            read_settings(m.substr(open, m.find(" }]", open) - open), entry);
            entries_of(m, at + add.size() - 1).push_back(entry);
        }
        else if (const std::size_t from = m.find(set); from != std::string::npos)
        {
            const std::size_t middle = m.find(in, from);
            const std::size_t line = std::stoul(m.substr(middle + in.size()));
            std::vector<J1939NodeEntry>& entries = entries_of(m, m.find("(participant ", middle) + 12);
            const auto it = std::find_if(entries.begin(), entries.end(), [line](const J1939NodeEntry& e) { return e.line == line; });
            CHECK(it != entries.end());
            if (it != entries.end())
            {
                read_settings(m.substr(from + set.size(), middle - from - set.size()), *it);
            }
        }
        else
        {
            continue;
        }
        ++written;
    }
    return written;
}


inline NetworkDescription on_ethernet(const std::string& name, std::optional<eth::MacAddress> mac)
{
    return NetworkDescription{name, NetworkUse::Ethernet, {}, mac};
}

inline eth::MacAddress mac_of(std::uint8_t last) { return eth::MacAddress{{0x02, 0, 0, 0, 0, last}}; }

/// An Ethernet network ETH (line 3) with Camera (MAC ...:01), a CAN network CAN1 (line 5) with
/// Engine, and a LIN network BODY (line 7).
inline Setup connect_setup()
{
    Setup s;
    s.network("ETH", NetworkKind::Ethernet, 3).network("CAN1", NetworkKind::Can, 5).network("BODY", NetworkKind::Lin, 7);
    s.described(participant("Camera", 10), ecu("Camera", 10ms, {on_ethernet("ETH", mac_of(1))}));
    s.described(participant("Engine", 13), ecu("Engine", 10ms, {NetworkDescription{"CAN1", NetworkUse::Raw, {}}}));
    return s;
}

/// What a bridge of this repository writes for --describe (bridges/common/src/describe.cpp).
inline std::string bridge_description(const std::string& name, const std::string& network, const std::string& kind,
                               const std::string& version = vehicle_sil::version())
{
    const std::string versionKey = version.empty() ? std::string{} : R"("vehicle_sil": ")" + version + R"(", )";
    return R"({"vehicle_sil_ecu": 1, )" + versionKey + R"("name": ")" + name + R"(", "step_ns": null, "networks": [)" +
           R"({"name": ")" + network + R"(", "kind": ")" + kind + R"(", "use": ")" + (kind == "can" ? "raw" : "ethernet") +
           R"(", "nodes": [], "mac": null}], "variables": [], "control": null, "features": []})";
}

/// Tools that are found in /tools and answer --describe as the bridges of this repository do;
/// each run is kept.
struct FakeTools
{
    std::vector<std::string> missing;   ///< tools that are not found
    std::optional<DescribeRun> answer;  ///< instead of the right description
    std::vector<std::string> runs;      ///< "<tool path> <arguments>"
    Os os = Os::Windows;                ///< ConnectTools::os
    ConnectTools tools()
    {
        ConnectTools t;
        t.launcher_dir = "/opt/vsil/bin";
        t.os = os;
        t.find = [this](std::string_view name) -> std::optional<std::filesystem::path> {
            if (std::find(missing.begin(), missing.end(), name) != missing.end())
            {
                return std::nullopt;
            }
            return std::filesystem::path("/tools") / std::string(name);
        };
        t.describe = [this](const std::filesystem::path& tool, const std::vector<std::string>& arguments) {
            std::string line = tool.generic_string();
            for (const std::string& a : arguments)
            {
                line += " " + a;
            }
            runs.push_back(line);
            DescribeRun run;
            run.command_text = tool.filename().generic_string() + " ...";
            run.timeout = 10000ms;
            if (answer)
            {
                return *answer;
            }
            const std::string network = arguments.at(1);
            const bool can = tool.filename() != "vehicle_sil_ethernet_bridge";
            run.exit_code = 0;
            run.standard_output = bridge_description(arguments.back(), network, can ? "can" : "ethernet");
            return run;
        };
        return t;
    }
};

inline std::string connect_lines(const Setup& s, const std::vector<ConnectRequest>& requests, FakeTools& fake, bool with_tools = true)
{
    const ConnectTools tools = fake.tools();
    return lines(check_connections(s.config, s.results, requests, with_tools ? &tools : nullptr).findings);
}

inline std::string outside_text(const ConnectCheck& check)
{
    std::string out;
    for (const NetworkOutside& o : check.outside)
    {
        out += o.network + " " + o.tool + " [" + o.tool_path.generic_string() + "] " + o.target + " " + o.participant + ":";
        if (!o.arguments)
        {
            out += " (no command)";
        }
        for (const std::string& a : o.arguments.value_or(std::vector<std::string>{}))
        {
            out += " " + a;
        }
        out += "\n";
    }
    return out;
}


/// Two Ethernet networks A (line 3) and B (line 4), each with its own ECU.
inline Setup two_ethernets()
{
    Setup s;
    s.network("A", NetworkKind::Ethernet, 3).network("B", NetworkKind::Ethernet, 4);
    s.described(participant("EcuA", 10), ecu("EcuA", 10ms, {on_ethernet("A", mac_of(1))}));
    s.described(participant("EcuB", 11), ecu("EcuB", 10ms, {on_ethernet("B", mac_of(2))}));
    return s;
}

/// Two CAN networks C1 (line 3) and C2 (line 4), each with its own ECU.
inline Setup two_cans()
{
    Setup s;
    s.network("C1", NetworkKind::Can, 3).network("C2", NetworkKind::Can, 4);
    s.described(participant("EcuC1", 10), ecu("EcuC1", 10ms, {NetworkDescription{"C1", NetworkUse::Raw, {}}}));
    s.described(participant("EcuC2", 11), ecu("EcuC2", 10ms, {NetworkDescription{"C2", NetworkUse::Raw, {}}}));
    return s;
}

inline std::string shared_lines(const Setup& s, const std::vector<ConnectRequest>& requests, Os os = Os::Windows)
{
    FakeTools fake;
    fake.os = os;
    const ConnectTools tools = fake.tools();
    std::string out;
    for (const Finding& f : check_connections(s.config, s.results, requests, &tools).findings)
    {
        if (f.code.rfind("connect-target-shared", 0) == 0 || f.code.rfind("connect-shared-loop", 0) == 0)
        {
            out += std::string(f.severity == Severity::Error ? "E " : "W ") + f.code + ": " + f.message + "\n";
        }
    }
    return out;
}

} // namespace vehicle_sil::config::test::check_setup
