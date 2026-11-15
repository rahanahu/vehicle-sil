// SPDX-License-Identifier: MIT
//
// The networks of a participant as the simulation sees them: what its executable described, under
// the names its entry gives them (network_names). The rules and the outputs of the check read the
// networks of a description only through seen_networks(), so that a renamed network is under the
// simulation's name in every one of them; write_reported_networks() writes what the executable
// said, as it said it. tests/launch has a check (launch.described_networks_are_read_in_one_place)
// that no other source of the launcher reads them.
#include "vehicle_sil/launch/check.hpp"

#include "check_format.hpp"
#include "vehicle_sil/json/json_writer.hpp"

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
        out.push_back(std::move(seen));
    }
    return out;
}

void detail::write_reported_networks(vehicle_sil::json::JsonWriter& w, const EcuDescription& ecu)
{
    w.begin_array("networks");
    for (const auto& network : ecu.networks)
    {
        w.begin_object();
        w.string("name", network.name);
        w.string("kind", to_string(network.kind()));
        w.string("use", to_string(network.use));
        w.begin_array("nodes");
        for (const auto& node : network.nodes)
        {
            w.begin_object();
            w.integer("address", node.address);
            if (node.name)
            {
                w.string("j1939_name", vehicle_sil::json::hex64(node.name->value()));
            }
            else
            {
                w.null("j1939_name");
            }
            w.boolean("arbitrary_address_capable", node.arbitrary_address_capable());
            w.end_object();
        }
        w.end_array();
        if (network.mac)
        {
            w.string("mac", network.mac->to_string());
        }
        else
        {
            w.null("mac");
        }
        w.end_object();
    }
    w.end_array();
}

} // namespace vehicle_sil::launch
