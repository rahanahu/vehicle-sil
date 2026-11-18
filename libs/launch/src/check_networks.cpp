// SPDX-License-Identifier: MIT
//
// The networks of a participant as the simulation sees them: what its executable described, under
// the names its entry gives them (network_names), with the values its entry gives its J1939 nodes
// (j1939_nodes). The rules and the outputs of the check read the networks of a description only
// through seen_networks() (SeenEcu::seen_networks(), seen_ecu.hpp), so that a renamed network is
// under the simulation's name and a changed node has its new values in every one of them;
// write_reported_networks() (reported_networks.cpp) writes what the executable said, as it said it.
// tests/launch has a check (launch.described_networks_are_read_in_one_place) that no other source
// of the launcher reads them.
#include "vehicle_sil/launch/check.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::launch {

std::vector<SeenNetwork> seen_networks(const ParticipantConfig& participant, const ReportedEcu& ecu)
{
    const std::vector<NetworkName> renames = renamed_networks(participant);
    std::vector<SeenNetwork> out;
    out.reserve(ecu.description.networks.size());
    for (const NetworkDescription& described : ecu.description.networks)
    {
        SeenNetwork seen{described, described.name, 0};
        for (const NetworkName& entry : renames)
        {
            if (entry.from == described.name)
            {
                seen.network.name = entry.name;
                seen.line = entry.line;
                break;
            }
        }
        seen.coded_nodes = described.nodes;
        seen.node_lines.assign(described.nodes.size(), 0);
        out.push_back(std::move(seen));
    }
    // The entries of j1939_nodes, on the first J1939 network of their name (after the renaming):
    // the same rules as the Network of the executable applies when its code adds the nodes.
    for (SeenNetwork& seen : out)
    {
        if (seen.network.use != NetworkUse::J1939 ||
            std::any_of(out.begin(), out.end(), [&seen](const SeenNetwork& other) {
                return &other < &seen && other.network.use == NetworkUse::J1939 && other.network.name == seen.network.name;
            }))
        {
            continue;
        }
        std::vector<std::size_t> indexes;
        std::vector<j1939::NodeOverride> entries;
        for (std::size_t e = 0; e < participant.j1939_nodes.size(); ++e)
        {
            const J1939NodeEntry& entry = participant.j1939_nodes[e];
            if (entry.network == seen.network.name && entry.node.changes())
            {
                indexes.push_back(e);
                entries.push_back(entry.node);
            }
        }
        if (entries.empty())
        {
            continue;
        }
        std::vector<j1939::NodeValues> coded;
        coded.reserve(seen.coded_nodes.size());
        for (const J1939NodeDescription& node : seen.coded_nodes)
        {
            coded.push_back(j1939::NodeValues{node.address, node.name});
        }
        const std::vector<j1939::NodeOverrideResult> results = j1939::apply_node_overrides(coded, entries);
        for (std::size_t r = 0; r < results.size(); ++r)
        {
            const J1939NodeEntry& entry = participant.j1939_nodes[indexes[r]];
            seen.j1939_matches.push_back(J1939NodeMatch{indexes[r], entry.line, results[r]});
            if (results[r].match == j1939::NodeOverrideMatch::Applied)
            {
                J1939NodeDescription& node = seen.network.nodes[results[r].node];
                node.address = results[r].values.address;
                node.name = results[r].values.name;
                seen.node_lines[results[r].node] = entry.line;
            }
        }
    }
    return out;
}

std::vector<std::size_t> unmatched_j1939_entries(const ParticipantConfig& participant, const std::vector<SeenNetwork>& seen)
{
    std::vector<std::size_t> out;
    for (std::size_t e = 0; e < participant.j1939_nodes.size(); ++e)
    {
        const J1939NodeEntry& entry = participant.j1939_nodes[e];
        if (entry.node.changes() && std::none_of(seen.begin(), seen.end(), [&entry](const SeenNetwork& n) {
                return n.network.use == NetworkUse::J1939 && n.network.name == entry.network;
            }))
        {
            out.push_back(e);
        }
    }
    return out;
}

} // namespace vehicle_sil::launch
