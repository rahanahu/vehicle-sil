// SPDX-License-Identifier: MIT
//
// What a bridge writes for --describe: the same bytes as vehicle_sil::to_json() of the same
// description (the form of an ecu_main executable), and what vehicle_sil_launch reads from it.
#include "test_harness.hpp"

#include "vehicle_sil/bridge_runtime/describe.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/config/check.hpp"

#include <string>

using vehicle_sil::EcuDescription;
using vehicle_sil::NetworkDescription;
using vehicle_sil::NetworkKind;
using vehicle_sil::NetworkUse;
using vehicle_sil::bridge_runtime::BridgedNetworkKind;
using vehicle_sil::bridge_runtime::BridgeDescription;
using vehicle_sil::bridge_runtime::describe_json;

namespace {

/// The description an ecu_main executable would write for the same participant.
std::string as_an_ecu_writes_it(const std::string& name, const std::string& network, NetworkUse use)
{
    EcuDescription description;
    description.name = name;
    description.networks.push_back(NetworkDescription{network, use, {}, {}});
    return vehicle_sil::to_json(description);
}

} // namespace

TEST_CASE(describe_of_an_ethernet_bridge_is_what_an_ecu_writes_for_its_network)
{
    const std::string json = describe_json(BridgeDescription{"EthBridge", "ETH_EXT", BridgedNetworkKind::Ethernet});
    CHECK(json == as_an_ecu_writes_it("EthBridge", "ETH_EXT", NetworkUse::Ethernet));
    CHECK(json == "{\n"
                     "  \"vehicle_sil_ecu\": 1,\n"
                     "  \"vehicle_sil\": \"" + std::string(vehicle_sil::version()) + "\",\n"
                     "  \"name\": \"EthBridge\",\n"
                     "  \"step_ns\": null,\n"
                     "  \"networks\": [\n"
                     "    {\n"
                     "      \"name\": \"ETH_EXT\",\n"
                     "      \"kind\": \"ethernet\",\n"
                     "      \"use\": \"ethernet\",\n"
                     "      \"nodes\": [],\n"
                     "      \"mac\": null\n"
                     "    }\n"
                     "  ],\n"
                     "  \"variables\": [],\n"
                     "  \"control\": null,\n"
                     "  \"features\": []\n"
                     "}\n");
}

TEST_CASE(describe_of_a_can_bridge_is_what_an_ecu_writes_for_a_raw_can_channel)
{
    CHECK(describe_json(BridgeDescription{"XlBridge", "VehicleCAN", BridgedNetworkKind::Can}) == as_an_ecu_writes_it("XlBridge", "VehicleCAN", NetworkUse::Raw));
}

TEST_CASE(vehicle_sil_launch_reads_a_bridge_as_a_participant_in_real_time_on_its_network)
{
    // A name and a network in another script: the bytes as given (UTF-8).
    const std::string name = "\xE3\x83\x96\xE3\x83\xAA\xE3\x83\x83\xE3\x82\xB8";
    const auto read = vehicle_sil::config::read_description(
        describe_json(BridgeDescription{name, "ETH \"ext\"", BridgedNetworkKind::Ethernet}));
    CHECK(read.vehicle_sil_version == vehicle_sil::version());
    CHECK(read.description.name == name);
    CHECK(!read.description.step.has_value());
    CHECK(read.description.networks.size() == 1);
    CHECK(read.description.networks.at(0).name == "ETH \"ext\"");
    CHECK(read.description.networks.at(0).kind() == NetworkKind::Ethernet);
    CHECK(!read.description.networks.at(0).mac.has_value());
    CHECK(read.variables_reported);
    CHECK(read.description.variables.empty());
    CHECK(!read.description.control.has_value());
    CHECK(read.description.features.empty());

    const auto can = vehicle_sil::config::read_description(
        describe_json(BridgeDescription{"XlBridge", "VehicleCAN", BridgedNetworkKind::Can}));
    CHECK(can.description.networks.size() == 1);
    CHECK(can.description.networks.at(0).use == NetworkUse::Raw);
}
