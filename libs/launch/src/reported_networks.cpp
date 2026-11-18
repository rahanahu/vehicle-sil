// SPDX-License-Identifier: MIT
//
// The networks of a participant as its executable described them (--describe), under the
// executable's names, for the check's JSON ("reported"). One of the sources that may read the
// networks of a description (check_networks.cpp has the rest; tests/launch:
// launch.described_networks_are_read_in_one_place).
#include "check_format.hpp"

#include "vehicle_sil/json/json_writer.hpp"

namespace vehicle_sil::launch {

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
