// SPDX-License-Identifier: MIT
//
// The two places that apply j1939_nodes agree: the check (seen_networks(), on the nodes an
// executable described) and the network of the executable (j1939::Network with
// NetworkOptions::node_overrides, as its code adds the nodes). Each scene goes through both; the
// values of the nodes after the entries, and whether an entry cannot be applied, must be the same.
#include "launch_test_util.hpp"

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/config/check.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;

namespace {

constexpr std::uint64_t kName = 0x00000000FE000001; // identity number 1, Manufacturer Code 0x7F0
constexpr std::uint64_t kCapable = std::uint64_t{1} << 63;

struct Scene
{
    const char* what;
    std::vector<J1939NodeDescription> nodes; ///< as the code adds them
    std::vector<j1939::NodeOverride> entries;
};

J1939NodeDescription node(j1939::Address address, std::optional<std::uint64_t> name = std::nullopt)
{
    return J1939NodeDescription{address, name ? std::optional(j1939::Name{*name}) : std::nullopt};
}

j1939::NodeOverride entry(std::optional<j1939::Address> from, std::optional<j1939::Address> address,
                          std::optional<std::uint32_t> identity = std::nullopt,
                          std::optional<std::uint8_t> function_instance = std::nullopt,
                          std::optional<std::uint8_t> ecu_instance = std::nullopt)
{
    return j1939::NodeOverride{from, address, identity, function_instance, ecu_instance};
}

std::string values_text(j1939::Address address, const std::optional<j1939::Name>& name)
{
    char text[32];
    std::snprintf(text, sizeof text, "0x%02X/%016llX", address,
                  static_cast<unsigned long long>(name ? name->value() : 0));
    return std::string(text) + (name ? "" : "(no NAME)");
}

/// What the check makes of the scene: the values of each node, or "refused" if an entry takes no node.
std::string by_the_check(const Scene& scene)
{
    ParticipantConfig p;
    p.name = "Engine2";
    std::size_t line = 10;
    for (const j1939::NodeOverride& e : scene.entries)
    {
        p.j1939_nodes.push_back(J1939NodeEntry{{"VehicleCAN", e}, line++});
    }
    const ReportedEcu ecu{"1.0.0", EcuDescription{"Engine2", std::nullopt,
                                                  {NetworkDescription{"VehicleCAN", NetworkUse::J1939, scene.nodes}}, {}}};
    const std::vector<SeenNetwork> seen = seen_networks(p, ecu);
    for (const J1939NodeMatch& m : seen[0].j1939_matches)
    {
        if (m.result.match != j1939::NodeOverrideMatch::Applied && m.result.match != j1939::NodeOverrideMatch::Skipped)
        {
            return "refused";
        }
    }
    std::string out;
    for (const J1939NodeDescription& n : seen[0].network.nodes)
    {
        out += values_text(n.address, n.name) + " ";
    }
    return out;
}

/// What a j1939::Network makes of the scene as the nodes are added: their values, or "refused" if
/// add_node() refuses an entry or an entry is left that no node took.
std::string by_the_network(const Scene& scene)
{
    can::VirtualCanBus bus;
    can::VirtualCanBus::Endpoint& port = bus.attach();
    j1939::NetworkOptions options;
    options.node_overrides = scene.entries;
    j1939::Network network(options);
    can::Host host;
    host.add(network, port);
    std::vector<j1939::Node*> nodes;
    for (const J1939NodeDescription& n : scene.nodes)
    {
        j1939::NodeOptions node;
        node.address = n.address;
        node.name = n.name;
        try
        {
            nodes.push_back(&network.add_node(node));
        }
        catch (const std::invalid_argument&)
        {
            return "refused";
        }
    }
    if (!network.unused_node_overrides().empty())
    {
        return "refused";
    }
    std::string out;
    for (const j1939::Node* n : nodes)
    {
        out += values_text(n->requested_address(), n->name()) + " ";
    }
    return out;
}

} // namespace

TEST_CASE(j1939_nodes_match_like_the_network_does)
{
    const Scene scenes[] = {
        {"no node on the network", {}, {entry(std::nullopt, 0x01)}},
        {"no node at from", {node(0x00, kName)}, {entry(0x10, 0x01)}},
        {"no from, two addresses", {node(0x00, kName), node(0x21, kName + 1)}, {entry(std::nullopt, 0x05)}},
        {"from on two nodes", {node(0xFE, kCapable | 1), node(0xFE, kCapable | 2)}, {entry(0xFE, 0x05)}},
        {"no from, every node on one address", {node(0x10, kName), node(0x10, kName + 1)}, {entry(std::nullopt, 0x05)}},
        {"a field of the NAME without a NAME", {node(0x00)}, {entry(std::nullopt, std::nullopt, 2)}},
        {"any free address, not capable", {node(0x00, kName)}, {entry(std::nullopt, 0xFE)}},
        {"any free address, no NAME", {node(0x00)}, {entry(std::nullopt, 0xFE)}},
        {"two entries swap two addresses", {node(0x00, kName), node(0x01, kName + 1)}, {entry(0x00, 0x01), entry(0x01, 0x00)}},
        {"an entry that changes nothing", {node(0x00, kName), node(0x21)}, {entry(0x00, std::nullopt)}},
        {"every field of the NAME", {node(0x00, kName)}, {entry(std::nullopt, std::nullopt, 2097151, 31, 7)}},
        {"any free address in the code, to one", {node(0xFE, kCapable | 3)}, {entry(0xFE, 0x30)}},
        {"to any free address", {node(0x30, kCapable | 3)}, {entry(std::nullopt, 0xFE, 4)}},
        {"one node, no from", {node(0x10, kName)}, {entry(std::nullopt, 0x11, 9)}},
        {"a node without a NAME, its address", {node(0x10), node(0x20, kName)}, {entry(0x10, 0x12), entry(0x20, std::nullopt, 0, 0, 0)}},
    };
    std::string differ;
    std::size_t refused = 0;
    for (const Scene& scene : scenes)
    {
        const std::string check = by_the_check(scene);
        const std::string net = by_the_network(scene);
        refused += check == "refused" ? 1 : 0;
        if (check != net)
        {
            differ += std::string(scene.what) + ": the check says " + check + ", the network " + net + "\n";
        }
    }
    check_text(differ, "");
    // The scenes have both outcomes (a scene that both refuse by mistake would agree too).
    CHECK_EQ(refused, std::size_t{8});
    check_text(by_the_check(scenes[8]), "0x01/00000000FE000001 0x00/00000000FE000002 ");
    check_text(by_the_check(scenes[10]), "0x00/000000FFFE1FFFFF ");
}
