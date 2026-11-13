// SPDX-License-Identifier: MIT
//
// Ecu::describe(): the networks of every use in the order added, the J1939 nodes as they were
// added, the variables of every form in the order declared; the same description on the local
// buses and on SIL Kit, before and after a run and from a step; the declarations stay open; a
// Peer's monitoring outputs are left out; a Peer in real time has no step.
#include "test_harness.hpp"

#include "vehicle_sil/local/local_bench.hpp"
#include "vehicle_sil/peer/peer.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::Ecu;
using vehicle_sil::EcuDescription;
using vehicle_sil::NetworkKind;
using vehicle_sil::NetworkUse;
using vehicle_sil::SilKitOptions;
using vehicle_sil::SimTime;
using vehicle_sil::Simulation;
using vehicle_sil::to_json;
namespace control = vehicle_sil::control;
namespace j1939 = vehicle_sil::j1939;
namespace lin = vehicle_sil::lin;

namespace {

const j1939::Name kName = j1939::Name{}.set_identity_number(7).set_manufacturer_code(0x7F0);
const j1939::Name kFlexibleName =
    j1939::Name{}.set_identity_number(8).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true);

enum class Mode : std::int16_t
{
    Off = -1,
    Idle = 0,
    Run = 2,
};

/// Every use of a network, interleaved with variables of every form.
class Full
{
public:
    explicit Full(Ecu& ecu)
    {
        auto& net = ecu.j1939("J1939_CAN");
        net.add_node({.address = 0x21});
        ecu.input("a.throttle", &throttle_, {.unit = "%", .min = 0.0, .max = 100.0, .step = 0.5, .description = "pedal"});
        net.add_node({.address = 0x22, .name = kName});
        // Claims a free address of 128..247: after a run it has another address than it asked for.
        net.add_node({.address = j1939::kNullAddress, .name = kFlexibleName});
        static_cast<void>(ecu.lin_slave("LIN2", {.subscriptions = {0x10}}));
        ecu.input("b.enabled", &enabled_, {.parameter = true});
        static_cast<void>(ecu.can("J1939_CAN"));
        ecu.input_enum("c.mode", &mode_, {{"off", -1}, {"idle", 0}, {"run", 2}});
        static_cast<void>(ecu.lin_master(
            "LIN1", {{0x10, 2, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Master}}));
        ecu.output("d.count", &count_, {.description = "steps"});
        ecu.output_enum("e.state", [this] { return static_cast<int>(count_ % 2); }, {{"even", 0}, {"odd", 1}});
        ecu.input("f.gain", [this] { return gain_; }, [this](double v) { gain_ = v; }, {.min = -1.0});
        static_cast<void>(ecu.can("CAN2"));
        ecu.on_step([this](SimTime) { ++count_; });
    }

private:
    double throttle_ = 0.0;
    bool enabled_ = false;
    Mode mode_ = Mode::Idle;
    std::uint32_t count_ = 0;
    double gain_ = 1.0;
};

/// Throws a Failure that shows both texts.
void check_text(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure(what + ": the descriptions differ.\n--- actual ---\n" + actual +
                                         "--- expected ---\n" + expected);
    }
}

std::optional<SilKitOptions> on(bool silkit)
{
    return silkit ? std::optional<SilKitOptions>(SilKitOptions{}) : std::nullopt;
}

} // namespace

TEST_CASE(describe_lists_networks_nodes_and_variables_in_order)
{
    Simulation sim;
    sim.add<Full>("Full", 5ms);
    const EcuDescription d = sim.ecu("Full").describe();
    CHECK(d.name == "Full");
    CHECK(d.step == std::optional<vehicle_sil::SimDuration>(5ms));

    CHECK_EQ(d.networks.size(), 5u);
    const std::vector<std::pair<std::string, NetworkUse>> expected{{"J1939_CAN", NetworkUse::J1939},
                                                                   {"LIN2", NetworkUse::LinSlave},
                                                                   {"J1939_CAN", NetworkUse::Raw},
                                                                   {"LIN1", NetworkUse::LinMaster},
                                                                   {"CAN2", NetworkUse::Raw}};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CHECK(d.networks[i].name == expected[i].first);
        CHECK(d.networks[i].use == expected[i].second);
    }
    CHECK(d.networks[0].kind() == NetworkKind::Can);
    CHECK(d.networks[1].kind() == NetworkKind::Lin);
    CHECK(d.networks[3].kind() == NetworkKind::Lin);
    CHECK(d.networks[4].kind() == NetworkKind::Can);
    for (std::size_t i = 1; i < d.networks.size(); ++i)
    {
        CHECK(d.networks[i].nodes.empty());
    }

    const auto& nodes = d.networks[0].nodes;
    CHECK_EQ(nodes.size(), 3u);
    CHECK_EQ(nodes[0].address, 0x21);
    CHECK(!nodes[0].name);
    CHECK(!nodes[0].arbitrary_address_capable());
    CHECK_EQ(nodes[1].address, 0x22);
    CHECK(nodes[1].name == std::optional<j1939::Name>(kName));
    CHECK(!nodes[1].arbitrary_address_capable());
    CHECK_EQ(nodes[2].address, j1939::kNullAddress);
    CHECK(nodes[2].name == std::optional<j1939::Name>(kFlexibleName));
    CHECK(nodes[2].arbitrary_address_capable());

    // ecu.connected first (every ECU has it), then the ECU's own in the order declared.
    CHECK_EQ(d.variables.size(), 7u);
    CHECK(d.variables[0].name == "ecu.connected" && d.variables[0].type == control::ValueType::Bool &&
          d.variables[0].causality == control::Causality::Input);
    const std::vector<control::VariableInfo> v(d.variables.begin() + 1, d.variables.end());
    CHECK(v[0].name == "a.throttle" && v[0].type == control::ValueType::Real && v[0].causality == control::Causality::Input);
    CHECK(v[0].unit == "%" && v[0].min == 0.0 && v[0].max == 100.0 && v[0].step == 0.5 && v[0].description == "pedal");
    CHECK(v[1].name == "b.enabled" && v[1].type == control::ValueType::Bool && v[1].causality == control::Causality::Parameter);
    CHECK(v[2].name == "c.mode" && v[2].type == control::ValueType::Enum && v[2].causality == control::Causality::Input);
    CHECK_EQ(v[2].items.size(), 3u);
    CHECK(v[2].items[0].name == "off" && v[2].items[0].value == -1 && v[2].items[2].name == "run" && v[2].items[2].value == 2);
    CHECK(v[3].name == "d.count" && v[3].type == control::ValueType::Integer && v[3].causality == control::Causality::Output);
    CHECK(v[3].description == "steps");
    CHECK(v[4].name == "e.state" && v[4].type == control::ValueType::Enum && v[4].causality == control::Causality::Output);
    CHECK(v[5].name == "f.gain" && v[5].type == control::ValueType::Real && v[5].causality == control::Causality::Input);
    CHECK(v[5].min == -1.0 && !v[5].max);
}

TEST_CASE(describe_is_the_same_on_local_and_silkit_before_and_after_a_run)
{
    std::optional<std::string> reference;
    for (const bool silkit : {false, true})
    {
        const std::string where = silkit ? "SIL Kit" : "local";
        Simulation sim(on(silkit));
        sim.add<Full>("Full", 5ms);
        Ecu& ecu = sim.ecu("Full");
        const std::string before = to_json(ecu.describe());
        if (!reference)
        {
            reference = before;
        }
        check_text(before, *reference, where + ", before the run");
        sim.run_for(600ms);
        check_text(to_json(ecu.describe()), *reference, where + ", after the run");
        // What the description leaves out did change: the flexible node claimed an address, and a
        // Peer's agent got its monitoring outputs.
        const j1939::Network& net = silkit ? sim.peer("Full").network("J1939_CAN") : sim.local().network("Full", "J1939_CAN");
        CHECK(net.node(2).address() != j1939::kNullAddress);
        CHECK_EQ(net.node(2).requested_address(), j1939::kNullAddress);
        CHECK(silkit ? ecu.agent().schema().size() > 7u : ecu.agent().schema().size() == 7u);
    }
}

TEST_CASE(describe_works_from_a_step)
{
    Simulation sim;
    auto& full = sim.add<Full>("Full", 5ms);
    static_cast<void>(full);
    Ecu& ecu = sim.ecu("Full");
    std::vector<std::string> seen;
    ecu.at(SimTime{} + 10ms, [&](SimTime) { seen.push_back(to_json(ecu.describe())); });
    const std::string before = to_json(ecu.describe());
    sim.run_for(20ms);
    CHECK_EQ(seen.size(), 1u);
    check_text(seen.at(0), before, "from a step");
}

TEST_CASE(describe_leaves_the_declarations_open)
{
    for (const bool silkit : {false, true})
    {
        Simulation sim(on(silkit));
        Ecu& ecu = sim.add_ecu("Open", 10ms);
        static_cast<void>(ecu.j1939("CAN").add_node({.address = 0x30}));
        double a = 0.0;
        double b = 0.0;
        ecu.input("a", &a);
        CHECK_EQ(ecu.describe().variables.size(), 2u); // ecu.connected and a
        // Still open: one more variable, network and node after describe().
        ecu.output("b", &b);
        static_cast<void>(ecu.can("RAW"));
        static_cast<void>(ecu.j1939("CAN2").add_node({.address = 0x31}));
        const EcuDescription d = ecu.describe();
        CHECK_EQ(d.variables.size(), 3u);
        CHECK(d.variables[2].name == "b");
        CHECK_EQ(d.networks.size(), 3u);
        CHECK(d.networks[2].name == "CAN2");
        sim.run_for(30ms);
        CHECK_EQ(ecu.describe().variables.size(), 3u);
    }
}

TEST_CASE(describe_an_ecu_without_variables)
{
    Simulation sim;
    Ecu& plain = sim.add_ecu("Plain", 1ms);
    static_cast<void>(plain.can("CAN"));
    // Only the variable every ECU has, before its agent is made and after.
    const auto connected_only = [](const EcuDescription& d) {
        return d.variables.size() == 1 && d.variables[0].name == "ecu.connected";
    };
    CHECK(connected_only(plain.describe()));
    const std::string before = to_json(plain.describe());
    static_cast<void>(plain.agent());
    CHECK(connected_only(plain.describe()));
    CHECK(to_json(plain.describe()) == before);
    Ecu& empty = sim.add_ecu("Empty", 2ms);
    const EcuDescription d = empty.describe();
    CHECK(d.name == "Empty" && d.networks.empty() && connected_only(d));
    CHECK(d.step == std::optional<vehicle_sil::SimDuration>(2ms));
}

TEST_CASE(describe_a_real_time_peer_has_no_step)
{
    Simulation sim(SilKitOptions{}); // a registry of this process
    vehicle_sil::PeerOptions options;
    options.participant = "Bridge";
    options.registry_uri = sim.registry_uri();
    options.virtual_time = std::nullopt;
    vehicle_sil::Peer peer(options);
    static_cast<void>(peer.j1939("CAN").add_node({.address = 0x40}));
    const EcuDescription d = peer.describe();
    CHECK(d.name == "Bridge");
    CHECK(!d.step);
    CHECK_EQ(d.networks.size(), 1u);
    CHECK(d.networks[0].use == NetworkUse::J1939);
}

TEST_CASE(describe_reserved_variable_names_are_refused_alike)
{
    // "peer." is reserved for a Peer's monitoring outputs on both implementations, at the
    // declaration, and "ecu." for the variables every ECU has; the message names the ECU, the
    // variable and the rule. Nothing is declared.
    for (const bool silkit : {false, true})
    {
        Simulation sim(on(silkit));
        Ecu& ecu = sim.add_ecu("Named", 10ms);
        int x = 0;
        for (const auto& declare : std::vector<std::function<void()>>{
                 [&] { ecu.input("peer.steps", &x); },
                 [&] { ecu.output("peer.mine", &x); },
                 [&] { ecu.output("peer.", [] { return 1; }); },
                 [&] { ecu.input_enum("peer.mode", &x, {{"a", 0}}); },
                 [&] { ecu.output_enum("peer.state", [] { return 0; }, {{"a", 0}}); },
                 [&] { ecu.input("peer.get", [] { return 1.0; }, [](double) {}); }})
        {
            CHECK(vehicle_sil::unit::throws_if<std::invalid_argument>(declare, [](const std::invalid_argument& e) {
                const std::string what = e.what();
                return what.find("\"Named\"") != std::string::npos && what.find("starts with \"peer.\"") != std::string::npos &&
                       what.find("reserved for the monitoring outputs of a Peer") != std::string::npos;
            }));
        }
        for (const auto& declare : std::vector<std::function<void()>>{[&] { ecu.input("ecu.connected", &x); },
                                                                       [&] { ecu.output("ecu.mine", &x); }})
        {
            CHECK(vehicle_sil::unit::throws_if<std::invalid_argument>(declare, [](const std::invalid_argument& e) {
                const std::string what = e.what();
                return what.find("\"Named\"") != std::string::npos && what.find("starts with \"ecu.\"") != std::string::npos &&
                       what.find("reserved for the variables vehicle-sil gives every ECU") != std::string::npos;
            }));
        }
        CHECK_EQ(ecu.describe().variables.size(), 1u); // ecu.connected
        // Other names that only look alike are the ECU's.
        ecu.output("peers.count", &x);
        ecu.output("app.peer.x", &x);
        ecu.output("peer", &x);
        ecu.output("ecu", &x);
        CHECK_EQ(ecu.describe().variables.size(), 5u);
    }
}
