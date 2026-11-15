// SPDX-License-Identifier: MIT
//
// The check of vehicle_sil_launch without processes: reading --describe's JSON, what a run of
// --describe means, every rule of check_participants() with a case that passes and one that
// fails (the message whole), the order of the findings, and the output for people and as JSON.
#include "launch_test_util.hpp"

#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/scenario.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;
using vehicle_sil::launch::test::lines;

namespace {

constexpr std::uint64_t kEngineName = 0x00000000FE000001; // identity number 1, Manufacturer Code 0x7F0
constexpr std::uint64_t kArbitraryBit = std::uint64_t{1} << 63;
/// The end of address-conflict, address-shared and name-conflict when no executable of the nodes
/// takes j1939_nodes (as the descriptions of these tests, which have no "features").
const std::string kNoEntries = "this file cannot change them, as their executables do not take j1939_nodes (no "
                               "\"j1939-node\" in the features of their --describe): build one of them again with "
                               "vehicle-sil 1.0.0 or later and set it in j1939_nodes, or have its author change it in its "
                               "code";

NetworkDescription on_j1939(const std::string& name, std::vector<J1939NodeDescription> nodes)
{
    return NetworkDescription{name, NetworkUse::J1939, std::move(nodes)};
}

J1939NodeDescription node(j1939::Address address, std::optional<std::uint64_t> name = std::nullopt)
{
    return J1939NodeDescription{address, name ? std::optional(j1939::Name{*name}) : std::nullopt};
}

ReportedEcu ecu(const std::string& name, std::optional<SimDuration> step, std::vector<NetworkDescription> networks,
                const std::string& version = "1.0.0")
{
    return ReportedEcu{version, EcuDescription{name, step, std::move(networks), {}}};
}

ParticipantConfig participant(const std::string& name, std::size_t line, std::optional<std::int64_t> step_ms = std::nullopt,
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

ParticipantConfig with_role(ParticipantConfig p, ParticipantRole role)
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
Setup good_pair()
{
    Setup s;
    s.network("J1939_CAN", NetworkKind::Can, 3)
        .described(participant("Engine", 6, 10), ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})}))
        .described(participant("Dashboard", 9), ecu("Dashboard", 5ms, {on_j1939("J1939_CAN", {node(0x28)})}));
    return s;
}

control::VariableInfo variable(const std::string& name)
{
    control::VariableInfo v{};
    v.name = name;
    return v;
}

DescribeRun run(std::optional<int> code, std::string out = "", std::string err = "")
{
    DescribeRun r;
    r.command_text = "engine/engine_ecu --describe --name Engine";
    r.exit_code = code;
    r.standard_output = std::move(out);
    r.standard_error = std::move(err);
    r.timeout = 10000ms;
    return r;
}

} // namespace

// --- reading --describe ---------------------------------------------------------------------

TEST_CASE(reads_what_to_json_writes)
{
    EcuDescription d{"Engine \xC3\xA9", 10ms,
                     {on_j1939("J1939_CAN", {node(0x00, kEngineName), node(j1939::kNullAddress, kEngineName | kArbitraryBit), node(0x28)}),
                      NetworkDescription{"J1939_CAN", NetworkUse::Raw, {}},
                      NetworkDescription{"Body \"LIN\"", NetworkUse::LinMaster, {}},
                      NetworkDescription{"Seat", NetworkUse::LinSlave, {}},
                      NetworkDescription{"Eth", NetworkUse::Ethernet, {}, eth::MacAddress{{0x02, 0, 0, 0, 0, 0x01}}},
                      NetworkDescription{"Eth2", NetworkUse::Ethernet, {}}},
                     {variable("x")}};
    const ReportedEcu read = read_description(to_json(d));
    CHECK(read.description == d);
    CHECK(read.variables_reported);
    CHECK(read.vehicle_sil_version == version());
    // Real time: no step.
    d.step.reset();
    CHECK(read_description(to_json(d)).description == d);
    // Every use (kNetworkUses: also one added later), with nodes where it has them.
    EcuDescription every{"All", 10ms, {}, {}};
    for (const NetworkUse use : kNetworkUses)
    {
        every.networks.push_back(NetworkDescription{std::string("N_") + to_string(use), use, {}});
        if (use == NetworkUse::J1939)
        {
            every.networks.back().nodes.push_back(node(0x00, kEngineName));
        }
        if (use == NetworkUse::Ethernet)
        {
            every.networks.back().mac = eth::MacAddress{{0x02, 0x00, 0x00, 0xAB, 0xCD, 0xEF}};
        }
    }
    CHECK(read_description(to_json(every)).description == every);
}

TEST_CASE(ignores_unknown_keys_of_a_description)
{
    const ReportedEcu read = read_description(
        R"({"vehicle_sil_ecu": 1, "later": {"x": [1]}, "vehicle_sil": "9.9.9", "name": "A", "step_ns": 1000,
            "networks": [{"name": "N", "kind": "can", "use": "raw", "nodes": [], "more": true}]})");
    CHECK(read.description.name == "A" && read.vehicle_sil_version == "9.9.9");
    CHECK(read.description.step == SimDuration{1000});
    CHECK(read.description.networks.size() == 1 && read.description.networks[0].use == NetworkUse::Raw);
    // Without "variables" (an ECU of a vehicle-sil before them): not known, not none.
    CHECK(!read.variables_reported && read.description.variables.empty());
}

TEST_CASE(refuses_what_is_not_a_description)
{
    const auto refused = [](const std::string& json) {
        try
        {
            (void)read_description(json);
        }
        catch (const DescriptionError& e)
        {
            return std::string(e.what());
        }
        return std::string("(read)");
    };
    const std::string head = R"({"vehicle_sil_ecu": 1, "vehicle_sil": "1", "name": "A", "step_ns": 1000, "networks": )";
    const std::string net = R"([{"name": "N", "kind": "can", "use": "j1939", "nodes": )";
    check_text(refused(""), "the output is empty");
    check_text(refused("usage: engine_ecu ...\n"), "the output is not a JSON object");
    CHECK(refused("{\"vehicle_sil_ecu\": 1,").rfind("the output is not valid JSON (line ", 0) == 0);
    check_text(refused(R"({"vehicle_sil_ecu": 2})"), "format 2");
    check_text(refused(R"({"name": "A"})"), "the key vehicle_sil_ecu is missing");
    check_text(refused(R"({"vehicle_sil_ecu": "1"})"), "vehicle_sil_ecu is not an integer");
    check_text(refused(R"({"vehicle_sil_ecu": 1, "name": 5, "step_ns": null, "networks": []})"), "name is not a string");
    check_text(refused(R"({"vehicle_sil_ecu": 1, "name": "A", "step_ns": 0, "networks": []})"), "step_ns is 0, not a step");
    check_text(refused(R"({"vehicle_sil_ecu": 1, "name": "A", "step_ns": null, "networks": {}})"), "networks is not an array");
    check_text(refused(head + R"([{"name": "N", "kind": "can", "use": "flexray", "nodes": []}]})"),
               "networks[0].use is \"flexray\", not raw, j1939, lin_master, lin_slave or ethernet");
    check_text(refused(head + R"([{"name": "N", "kind": "ethernet", "use": "ethernet", "nodes": [], "mac": "02:00:00:00:00"}]})"),
               "networks[0].mac is \"02:00:00:00:00\", not six pairs of hexadecimal digits like \"02:00:00:00:00:01\"");
    check_text(refused(head + R"([{"name": "N", "kind": "ethernet", "use": "ethernet", "nodes": [], "mac": 2}]})"),
               "networks[0].mac is not a string");
    check_text(refused(head + R"([{"name": "N", "kind": "can", "use": "raw", "nodes": [], "mac": "02:00:00:00:00:01"}]})"),
               "networks[0] has a mac, but only an ethernet network has one");
    check_text(refused(head + R"([{"name": "N", "kind": "lin", "use": "raw", "nodes": []}]})"),
               "networks[0].kind is \"lin\", but use \"raw\" is on can");
    check_text(refused(head + R"([{"name": "N", "kind": "can", "use": "raw"}]})"), "networks[0]: the key nodes is missing");
    check_text(refused(head + net + R"([{"address": 255, "j1939_name": null, "arbitrary_address_capable": false}]}]})"),
               "networks[0].nodes[0].address is 255, not 0 to 254");
    check_text(refused(head + net + R"([{"address": 1, "j1939_name": "0x12", "arbitrary_address_capable": false}]}]})"),
               "networks[0].nodes[0].j1939_name is \"0x12\", not \"0x\" and 16 hexadecimal digits");
    check_text(refused(head + R"([{"name": "N", "kind": "can", "use": "raw", "nodes": [{"address": 1, "j1939_name": null, )"
                              R"("arbitrary_address_capable": false}]}]})"),
               "networks[0] has nodes, but only a j1939 network has them");
    // The variables (for check --json): each key as to_json() writes it.
    const std::string vars = head + R"([], "variables": )";
    const std::string var = R"({"name": "v", "type": "real", "causality": "input", "unit": "", "min": null, "max": 1.5, )"
                            R"("step": null, "description": "", )";
    check_text(refused(vars + "{}}"), "variables is not an array");
    check_text(refused(vars + "[5]}"), "variables[0] is not an object");
    check_text(refused(vars + R"([{"name": "v"}]})"), "variables[0]: the key type is missing");
    check_text(refused(vars + R"([{"name": "v", "type": "text"}]})"),
               "variables[0].type is \"text\", not one of bool, integer, real, enum");
    check_text(refused(vars + R"([{"name": "v", "type": "real", "causality": "local"}]})"),
               "variables[0].causality is \"local\", not one of input, output, parameter");
    check_text(refused(vars + "[" + var + R"("items": {}}]})"), "variables[0].items is not an array");
    check_text(refused(vars + "[" + var + R"("items": [{"name": "a", "value": "1"}]}]})"),
               "variables[0].items[0].value is not an integer");
    check_text(refused(vars + R"([{"name": "v", "type": "real", "causality": "input", "unit": "", "min": "0", "max": null, )"
                              R"("step": null, "description": "", "items": []}]})"),
               "variables[0].min is not a number or null");
    const ReportedEcu read = read_description(vars + "[" + var + R"("items": [], "later": 1}]})");
    CHECK(read.variables_reported && read.description.variables.size() == 1);
    CHECK(read.description.variables[0].max == 1.5 && !read.description.variables[0].min);
}

// --- what a run of --describe means ---------------------------------------------------------

TEST_CASE(a_run_that_wrote_a_description_gives_it)
{
    const DescribeOutcome o = interpret_describe(participant("Engine", 6),
                                                 run(0, to_json(EcuDescription{"Engine", 10ms, {}, {}})));
    CHECK(o.ecu.has_value() && !o.error.has_value());
    CHECK(o.ecu->description.name == "Engine");
}

TEST_CASE(a_run_that_failed_is_an_error_with_the_end_of_its_standard_error)
{
    const auto error = [](const DescribeRun& r) { return lines({*interpret_describe(participant("Engine", 6), r).error}); };
    DescribeRun cannot = run(std::nullopt);
    cannot.start_error = "cannot start /x/engine_ecu: Exec format error";
    check_text(error(cannot), "E 7 describe-start: participant 'Engine': cannot start /x/engine_ecu: Exec format error: "
                              "check that the file is an executable for this computer\n");
    std::string err;
    for (int i = 1; i <= 12; ++i)
    {
        err += "line " + std::to_string(i) + "\r\n";
    }
    check_text(error(run(std::nullopt, "", "waiting for the bus\n")),
               "E 7 describe-timeout: participant 'Engine': engine/engine_ecu --describe --name Engine did not end within "
               "10000 ms and was stopped. --describe only builds the ECU, so something in its set-up waits (a connection, "
               "input): fix that, or give vehicle_sil_launch check a longer --describe-timeout-ms\n"
               "  | the end of its standard error:\nwaiting for the bus\n");
    check_text(error(run(4, "", "error: Engine: unknown argument --describe\n")),
               "E 7 describe-failed: participant 'Engine': engine/engine_ecu --describe --name Engine ended with exit code "
               "4; it did not take --describe or --name: if it is not made with ecu_main, set describe: false\n"
               "  | the end of its standard error:\nerror: Engine: unknown argument --describe\n");
    check_text(error(run(3, "", err)),
               "E 7 describe-failed: participant 'Engine': engine/engine_ecu --describe --name Engine ended with exit code "
               "3; its set-up failed (see its standard error): fix the ECU, or set describe: false if it is not made with "
               "ecu_main\n"
               "  | the end of its standard error:\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\nline 9\nline 10\nline "
               "11\nline 12\n");
    const std::string crashed = error(run(139));
    check_text(crashed.substr(crashed.find("  | ")), "  | its standard error is empty\n");
    check_text(error(run(0, "hello\n{\n")),
               "E 7 describe-output: participant 'Engine': engine/engine_ecu --describe --name Engine ended with exit code "
               "0, but its output is not a description: the output is not a JSON object. Is it made with ecu_main, and does "
               "nothing else write to its standard output? If it is not made with ecu_main, set describe: false\n"
               "  | the start of its standard output:\nhello\n{\n");
    const std::string mine = version();
    check_text(error(run(0, R"({"vehicle_sil_ecu": 2})")),
               "E 7 describe-version: participant 'Engine': engine/engine_ecu --describe --name Engine wrote a description "
               "in format 2, but this vehicle_sil_launch (vehicle-sil " + mine + ") reads format 1: the executable is of a "
               "newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a newer one\n");
    check_text(error(run(0, R"({"vehicle_sil_ecu": 0, "vehicle_sil": "0.0.1"})")),
               "E 7 describe-version: participant 'Engine': engine/engine_ecu --describe --name Engine wrote a description "
               "in format 0 (vehicle-sil 0.0.1), but this vehicle_sil_launch (vehicle-sil " + mine + ") reads format 1: the "
               "executable is of an older vehicle-sil; build it again with this vehicle-sil, or use a vehicle_sil_launch of "
               "its vehicle-sil\n");
}

TEST_CASE(a_value_of_a_later_vehicle_sil_is_its_own_error)
{
    // A key with a fixed set of values may get a new one without a new format: the error names
    // the key, the value, the versions on both sides and the fix (a newer vehicle_sil_launch).
    const auto error = [](const std::string& json) {
        return lines({*interpret_describe(participant("Engine", 6), run(0, json)).error});
    };
    const std::string mine = version();
    const std::string head = R"({"vehicle_sil_ecu": 1, "vehicle_sil": "9.0.0", "name": "Engine", "step_ns": 1000, )";
    const std::string tail = ". A newer vehicle-sil may add such values without a new format: use a vehicle_sil_launch "
                             "of the executable's vehicle-sil or a newer one\n";
    const std::string versions = "the executable was built with vehicle-sil 9.0.0, this vehicle_sil_launch with "
                                 "vehicle-sil " + mine;
    const std::string start = "E 7 describe-unknown-value: participant 'Engine': engine/engine_ecu --describe --name Engine "
                              "wrote ";
    check_text(error(head + R"("networks": [{"name": "Ray", "kind": "flexray", "use": "flexray", "nodes": []}]})"),
               start + "networks[0].use \"flexray\", which this vehicle_sil_launch does not know (it knows raw, j1939, "
                       "lin_master, lin_slave and ethernet): " + versions + tail);
    check_text(error(head + R"("networks": [{"name": "N", "kind": "flexray", "use": "raw", "nodes": []}]})"),
               start + "networks[0].kind \"flexray\", which this vehicle_sil_launch does not know (it knows can, lin and "
                       "ethernet): " + versions + tail);
    const std::string var = R"("networks": [], "variables": [{"name": "v", "type": "real", "causality": "input", )"
                            R"("unit": "", "min": null, "max": null, "step": null, "description": "", "items": []}, )";
    check_text(error(head + var + R"({"name": "w", "type": "text", "causality": "input"}]})"),
               start + "variables[1].type \"text\", which this vehicle_sil_launch does not know (it knows bool, integer, "
                       "real and enum): " + versions + tail);
    check_text(error(head + var + R"({"name": "w", "type": "bool", "causality": "local"}]})"),
               start + "variables[1].causality \"local\", which this vehicle_sil_launch does not know (it knows input, "
                       "output and parameter): " + versions + tail);
    // Without "vehicle_sil", the message says so.
    check_text(error(R"({"vehicle_sil_ecu": 1, "name": "Engine", "step_ns": 1000, "networks": [{"name": "N", )"
                     R"("kind": "can", "use": "fd", "nodes": []}]})"),
               start + "networks[0].use \"fd\", which this vehicle_sil_launch does not know (it knows raw, j1939, "
                       "lin_master, lin_slave and ethernet): the executable does not say which vehicle-sil it was built with, and "
                       "this vehicle_sil_launch is of vehicle-sil " + mine + tail);
    // A known kind that is not the use's: broken output, not a later value.
    CHECK(interpret_describe(participant("Engine", 6),
                             run(0, head + R"("networks": [{"name": "N", "kind": "lin", "use": "raw", "nodes": []}]})"))
              .error->code == "describe-output");
}

// --- the rules ------------------------------------------------------------------------------

TEST_CASE(a_good_pair_passes)
{
    check_text(good_pair().check(), "");
}

TEST_CASE(the_reported_name_must_be_the_participant_name)
{
    Setup s = good_pair();
    s.results[1].ecu->description.name = "Cluster";
    check_text(s.check(), "E 9 name-mismatch: participant 'Dashboard': the executable reports the name 'Cluster' although "
                          "it was given --name Dashboard: its main() must pass all of its arguments to ecu_main\n");
}

TEST_CASE(step_ms_must_be_the_reported_step)
{
    Setup s = good_pair();
    s.config.participants[0].step_ms = 20;
    s.config.participants[1].step_ms = 5; // the same: no finding
    check_text(s.check(), "E 8 step-mismatch: participant 'Engine': step_ms is 20, but the executable's step is 10 ms: "
                          "write step_ms: 10, or remove step_ms\n");
    s.results[0].ecu->description.step = 500us;
    check_text(s.check(), "E 8 step-mismatch: participant 'Engine': step_ms is 20, but the executable's step is 500 us: "
                          "remove step_ms (it takes whole milliseconds)\n");
    s.results[0].ecu->description.step.reset();
    check_text(s.check(), "E 8 step-mismatch: participant 'Engine': step_ms is 20, but the executable reports no step (it "
                          "runs in real time): remove step_ms\n");
    s.config.participants[0].step_ms.reset(); // no step_ms: any step
    check_text(s.check(), "");
}

TEST_CASE(every_network_of_an_ecu_must_be_in_networks_with_its_kind)
{
    Setup s = good_pair();
    s.network("Body", NetworkKind::Lin, 5);
    s.results[0].ecu->description.networks.push_back(on_j1939("J1939CAN", {}));
    s.results[0].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::Raw, {}});
    s.results[0].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::Raw, {}}); // once
    s.results[0].ecu->description.networks.push_back(NetworkDescription{"Diag", NetworkUse::Raw, {}});
    // Engine is on the only other CAN network itself: 'J1939CAN' is not another name for it.
    check_text(s.check(),
               "E 6 unknown-network: participant 'Engine': the executable uses the network 'J1939CAN' (j1939), which is "
               "not in networks: add it to networks with kind: can, or, if it is a network of this file under another "
               "name, add network_names: { <name in networks>: J1939CAN } to this participant (did you mean "
               "'J1939_CAN'?)\n"
               "E 6 kind-mismatch: participant 'Engine': the executable uses the network 'Body' as can (raw), but networks "
               "says kind: lin (line 5): correct the kind, or the network in the ECU\n"
               "E 6 unknown-network: participant 'Engine': the executable uses the network 'Diag' (raw), which is not in "
               "networks: add it to networks with kind: can, or, if it is a network of this file under another name, "
               "add network_names: { <name in networks>: Diag } to this participant\n"
               "W 5 unused-network: network 'Body': no participant uses it: remove it, or check the network names in the "
               "ECUs\n");
    // A network whose kind could not be read is not checked (its own error says why).
    s.config.networks[1].valid = false;
    check_text(s.check(), "E 6 unknown-network: participant 'Engine': the executable uses the network 'J1939CAN' (j1939), "
                          "which is not in networks: add it to networks with kind: can, or, if it is a network of this "
                          "file under another name, add network_names: { <name in networks>: J1939CAN } to this "
                          "participant (did you mean 'J1939_CAN'?)\n"
                          "E 6 unknown-network: participant 'Engine': the executable uses the network 'Diag' (raw), which "
                          "is not in networks: add it to networks with kind: can, or, if it is a network of this file "
                          "under another name, add network_names: { <name in networks>: Diag } to this participant\n");
}

TEST_CASE(an_unknown_network_names_the_networks_of_its_kind_that_may_be_the_same_bus)
{
    // Dashboard calls the bus that Engine is on 'VehicleCAN'.
    Setup s;
    s.network("J1939_CAN", NetworkKind::Can, 3)
        .described(participant("Engine", 6), ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})}))
        .described(participant("Dashboard", 9), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}));
    // The fix is written in the file, in the direction of network_names: the simulation's name
    // (in networks) first, then the name in Dashboard's executable.
    const std::string fix = ": if 'VehicleCAN' is meant to be that bus, say so in this file: add network_names: { "
                            "J1939_CAN: VehicleCAN } to the participant 'Dashboard' (<simulation's name>: <executable's "
                            "name>; the executable stays as it is); if 'VehicleCAN' is a bus of its own, add it to "
                            "networks with kind: can\n";
    // No name to map to while neither is in networks: one of them is added first.
    const auto neither = [](const std::string& name) {
        return ": if '" + name + "' is meant to be that bus, choose one name for the bus: add it to networks with kind: "
               "can, and add network_names: { <that name>: <the executable's name> } to each participant whose "
               "executable names the bus differently (<simulation's name>: <executable's name>; the executables stay as "
               "they are); if '" + name + "' is a bus of its own, add it to networks with kind: can\n";
    };
    check_text(s.check(), "E 9 unknown-network: participant 'Dashboard': the executable uses the network 'VehicleCAN' "
                          "(j1939), which is not in networks. Another network of kind can in this simulation is "
                          "'J1939_CAN' (line 3, used by Engine)" + fix +
                          "W 3 single-participant: network 'J1939_CAN': only Engine is on it, so what it sends there "
                          "reaches no one: add the other participants of this network, or remove it\n");

    // Neither is in networks: each names the other's.
    s.config.networks.clear();
    check_text(s.check(), "E 6 unknown-network: participant 'Engine': the executable uses the network 'J1939_CAN' (j1939), "
                          "which is not in networks. Another network of kind can in this simulation is 'VehicleCAN' (not "
                          "in networks either, used by Dashboard)" + neither("J1939_CAN") +
                          "E 9 unknown-network: participant 'Dashboard': the executable uses the network 'VehicleCAN' "
                          "(j1939), which is not in networks. Another network of kind can in this simulation is "
                          "'J1939_CAN' (not in networks either, used by Engine)" + neither("VehicleCAN"));

    // Two candidates (one used by a participant for testing too); a network of another kind and a
    // network in networks that no one uses are not.
    Setup t;
    t.network("J1939_CAN", NetworkKind::Can, 3)
        .network("Diag", NetworkKind::Can, 4)
        .network("Spare", NetworkKind::Can, 5)
        .network("Body", NetworkKind::Lin, 6)
        .described(participant("Engine", 8), ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})}))
        .described(with_role(participant("Tester", 10), ParticipantRole::Test),
                   ecu("Tester", 10ms, {NetworkDescription{"Diag", NetworkUse::Raw, {}}}))
        .described(participant("Door", 12), ecu("Door", 10ms, {NetworkDescription{"Body", NetworkUse::LinSlave, {}}}))
        .described(participant("Dashboard", 14), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}));
    const std::string got = t.check();
    CHECK(got.find("E 14 unknown-network: participant 'Dashboard': the executable uses the network 'VehicleCAN' (j1939), "
                   "which is not in networks. Other networks of kind can in this simulation are 'J1939_CAN' (line 3, used "
                   "by Engine) and 'Diag' (line 4, used by Tester): if 'VehicleCAN' is meant to be one of those buses, say "
                   "so in this file: add network_names: { <one of them, in networks>: VehicleCAN } to the participant "
                   "'Dashboard' (") != std::string::npos);
    CHECK(got.find("'Spare' (line") == std::string::npos && got.find("'Body' (line") == std::string::npos);
}

TEST_CASE(networks_without_participants_or_with_one_are_warnings)
{
    Setup s;
    s.network("A", NetworkKind::Can, 3).network("B", NetworkKind::Can, 5);
    s.described(participant("P", 8), ecu("P", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}}}));
    check_text(s.check(), "W 3 single-participant: network 'A': only P is on it, so what it sends there reaches no one: "
                          "add the other participants of this network, or remove it\n"
                          "W 5 unused-network: network 'B': no participant uses it: remove it, or check the network names "
                          "in the ECUs\n");
    // Twice on the same network (j1939 and raw) is still one participant. With a participant
    // that was not checked, these are notes (it may be on the networks).
    s.results[0].ecu->description.networks.push_back(on_j1939("A", {}));
    s.unchecked("Logger", 11);
    check_text(s.check(),
               "N 11 not-checked: participant 'Logger': not checked (describe: false): vehicle_sil_launch cannot compare "
               "its name, step and networks with this file\n"
               "N 3 single-participant: network 'A': only P is on it, so what it sends there reaches no one "
               "(participants with describe: false are not counted): add the other participants of this network, or "
               "remove it\n"
               "N 5 unused-network: network 'B': no participant uses it (participants with describe: false are not "
               "counted): remove it, or check the network names in the ECUs\n");
}

TEST_CASE(participants_for_testing_are_not_counted_as_the_ecus_of_a_network)
{
    // One ECU and a spy: still one ECU on the network.
    Setup s;
    s.network("A", NetworkKind::Can, 3);
    s.described(participant("Engine", 6), ecu("Engine", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}}}));
    s.described(with_role(participant("Spy", 9), ParticipantRole::Test),
                ecu("Spy", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}}}));
    check_text(s.check(), "W 3 single-participant: network 'A': only Engine is on it besides the participants for testing "
                          "(role: test: Spy), so what it sends there reaches no other ECU: add the other ECUs of this "
                          "network, or remove it\n");
    // Two ECUs (role: ecu written or not) and the spy: right.
    s.described(with_role(participant("Brake", 12), ParticipantRole::Ecu),
                ecu("Brake", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}}}));
    check_text(s.check(), "");
    // Participants for testing alone.
    Setup t;
    t.network("A", NetworkKind::Can, 3);
    t.described(with_role(participant("Spy", 6), ParticipantRole::Test),
                ecu("Spy", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}}}));
    t.described(with_role(participant("Tool", 9), ParticipantRole::Test),
                ecu("Tool", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}}}));
    check_text(t.check(), "W 3 single-participant: network 'A': only participants for testing (role: test: Spy and Tool) "
                          "are on it, so there is no ECU for them there: add the ECUs of this network, or remove it\n");
}

TEST_CASE(networks_of_one_kind_with_one_ecu_each_may_be_one_bus_named_twice)
{
    // Both names are in networks (perhaps after unknown-network said to add one): each network
    // has one ECU, so the warning of each names the other.
    Setup s;
    s.network("J1939_CAN", NetworkKind::Can, 3)
        .network("VehicleCAN", NetworkKind::Can, 5)
        .network("Body", NetworkKind::Lin, 7)
        .described(participant("Engine", 9), ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})}))
        .described(participant("Dashboard", 12), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)}),
                                                                         NetworkDescription{"Body", NetworkUse::LinMaster, {}}}));
    const auto rest = [](const std::string& removed, const std::string& kept, const std::string& who) {
        return ", keep one of the two names: remove '" + removed + "' from networks and add network_names: { " + kept +
               ": " + removed + " } to the participant '" + who + "'\n";
    };
    check_text(s.check(), "W 3 single-participant: network 'J1939_CAN': only Engine is on it, so what it sends there "
                          "reaches no one: add the other participants of this network, or remove it; the network "
                          "'VehicleCAN' (line 5, only Dashboard) of kind can also has one ECU: if it is meant to be the "
                          "same bus" + rest("J1939_CAN", "VehicleCAN", "Engine") +
                          "W 5 single-participant: network 'VehicleCAN': only Dashboard is on it, so what it sends there "
                          "reaches no one: add the other participants of this network, or remove it; the network "
                          "'J1939_CAN' (line 3, only Engine) of kind can also has one ECU: if it is meant to be the same "
                          "bus" + rest("VehicleCAN", "J1939_CAN", "Dashboard") +
                          "W 7 single-participant: network 'Body': only Dashboard is on it, so what it sends there "
                          "reaches no one: add the other participants of this network, or remove it\n");

    // Not when the other network's one ECU is the same one, nor when it has two.
    s.results[1].ecu->description.networks.push_back(on_j1939("J1939_CAN", {node(0x28)}));
    check_text(s.check(), "W 5 single-participant: network 'VehicleCAN': only Dashboard is on it, so what it sends there "
                          "reaches no one: add the other participants of this network, or remove it\n"
                          "W 7 single-participant: network 'Body': only Dashboard is on it, so what it sends there "
                          "reaches no one: add the other participants of this network, or remove it\n");

    // One ECU alone on two networks of one kind: they are two buses of that ECU, not one.
    Setup u;
    u.network("A", NetworkKind::Can, 3)
        .network("B", NetworkKind::Can, 4)
        .described(participant("P", 6), ecu("P", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}},
                                                         NetworkDescription{"B", NetworkUse::Raw, {}}}));
    check_text(u.check(), "W 3 single-participant: network 'A': only P is on it, so what it sends there reaches no one: add "
                          "the other participants of this network, or remove it\n"
                          "W 4 single-participant: network 'B': only P is on it, so what it sends there reaches no one: add "
                          "the other participants of this network, or remove it\n");

    // Three such networks: each names the two others.
    Setup t;
    t.network("A", NetworkKind::Can, 3)
        .network("B", NetworkKind::Can, 4)
        .network("C", NetworkKind::Can, 5)
        .described(participant("P", 7), ecu("P", 10ms, {NetworkDescription{"A", NetworkUse::Raw, {}}}))
        .described(participant("Q", 9), ecu("Q", 10ms, {NetworkDescription{"B", NetworkUse::Raw, {}}}))
        .described(participant("R", 11), ecu("R", 10ms, {NetworkDescription{"C", NetworkUse::Raw, {}}}));
    CHECK(t.check().find("W 3 single-participant: network 'A': only P is on it, so what it sends there reaches no one: add "
                         "the other participants of this network, or remove it; the networks 'B' (line 4, only Q) and 'C' "
                         "(line 5, only R) of kind can also have one ECU each: if one of them is meant to be the same bus, "
                         "keep one of the two names: remove 'A' from networks and add network_names: { <that network>: A "
                         "} to the participant 'P'\n") != std::string::npos);
}

TEST_CASE(a_test_participant_with_j1939_nodes_where_a_bridge_may_be_is_a_warning)
{
    constexpr std::uint64_t kToolName = 0x80000000FE000080; // arbitrary address capable, Manufacturer Code 0x7F0
    Setup base = good_pair();
    base.described(with_role(participant("Tool", 12), ParticipantRole::Test),
                   ecu("Tool", 10ms, {on_j1939("J1939_CAN", {node(0x80, kToolName)})}));
    check_text(base.check(), "");

    // A bridge that describes itself on the network.
    Setup on = base;
    on.network("Other", NetworkKind::Can, 4);
    on.described(with_role(participant("Gateway", 15), ParticipantRole::Bridge),
                 ecu("Gateway", 10ms, {NetworkDescription{"Other", NetworkUse::Raw, {}}}));
    on.described(participant("Seat", 18), ecu("Seat", 10ms, {NetworkDescription{"Other", NetworkUse::Raw, {}}}));
    check_text(on.check(), ""); // on another network
    on.results[3].ecu->description.networks.push_back(NetworkDescription{"J1939_CAN", NetworkUse::Raw, {}});
    check_text(on.check(),
               "W 12 test-participant-talks-on-bridged-network: participant 'Tool': it has role: test and J1939 nodes on "
               "network 'J1939_CAN', and the bridge Gateway (role: bridge) is on it too: what it sends there goes out on "
               "the real bus too, its address claims among it, and a real ECU with the same address fights it for the "
               "address: let it only listen there (no J1939 nodes), or leave the bridge out of runs with it (--disable "
               "Gateway, or enabled: false in a variant)\n");
    on.config.participants[3].enabled = false; // a bridge that is left out carries nothing
    on.results[3] = ParticipantResult{ParticipantStatus::Disabled, std::nullopt};
    CHECK(on.check().find("test-participant-talks-on-bridged-network") == std::string::npos);
    // Nor does it mark the bridges: a participant with describe: false and no role may be one.
    on.unchecked("Logger", 21);
    CHECK(on.check().find("and Logger has describe: false and no role") != std::string::npos);

    // A bridge with describe: false may carry any network.
    Setup marked = base;
    marked.unchecked("Gateway", 15);
    marked.config.participants.back().role = ParticipantRole::Bridge;
    marked.config.participants.back().role_written = true;
    marked.unchecked("Logger", 18); // no role, but there is a bridge: not one
    check_text(marked.check(),
               "N 15 not-checked: participant 'Gateway': not checked (describe: false): vehicle_sil_launch cannot "
               "compare its name, step and networks with this file\n"
               "N 18 not-checked: participant 'Logger': not checked (describe: false): vehicle_sil_launch cannot "
               "compare its name, step and networks with this file\n"
               "W 12 test-participant-talks-on-bridged-network: participant 'Tool': it has role: test and J1939 nodes on "
               "network 'J1939_CAN', and the bridge Gateway (role: bridge) may carry it (describe: false, so the check "
               "cannot see its networks): what it sends there goes out on the real bus too, its address claims among it, "
               "and a real ECU with the same address fights it for the address: let it only listen there (no J1939 "
               "nodes), or leave the bridge out of runs with it (--disable Gateway, or enabled: false in a variant)\n");

    // Without role: bridge, a participant with describe: false and no role may be one.
    Setup maybe = base;
    maybe.unchecked("Logger", 15);
    check_text(maybe.check(),
               "N 15 not-checked: participant 'Logger': not checked (describe: false): vehicle_sil_launch cannot "
               "compare its name, step and networks with this file\n"
               "W 12 test-participant-talks-on-bridged-network: participant 'Tool': it has role: test and J1939 nodes on "
               "network 'J1939_CAN', and Logger has describe: false and no role, so it may be a bridge that carries this "
               "network to a real bus: what it sends there goes out on the real bus too, its address claims among it, "
               "and a real ECU with the same address fights it for the address: let it only listen there (no J1939 "
               "nodes), or leave the bridge out of runs with it (--disable Logger, or enabled: false in a variant); if "
               "Logger is not a bridge, write role: ecu for it (and role: bridge for a bridge)\n");
    // With a role written, it is not one.
    maybe.config.participants.back().role_written = true;
    CHECK(maybe.check().find("test-participant-talks-on-bridged-network") == std::string::npos);

    // A participant for testing without J1939 nodes there (it may only listen): no warning.
    Setup quiet = good_pair();
    quiet.described(with_role(participant("Spy", 12), ParticipantRole::Test),
                    ecu("Spy", 10ms, {NetworkDescription{"J1939_CAN", NetworkUse::Raw, {}}}));
    quiet.unchecked("Logger", 15);
    CHECK(quiet.check().find("test-participant-talks-on-bridged-network") == std::string::npos);
    // An ECU with J1939 nodes beside a bridge is what a bridge is for: no warning.
    Setup ecus = good_pair();
    ecus.unchecked("Logger", 15);
    CHECK(ecus.check().find("test-participant-talks-on-bridged-network") == std::string::npos);
}

TEST_CASE(j1939_addresses_must_differ_on_a_network)
{
    // A node without a NAME does not claim: it keeps sending from the address.
    Setup s = good_pair();
    s.results[1].ecu->description.networks[0].nodes = {node(0x00), node(0x80)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard (node 1) both ask for the "
                          "address 0x00 (0), and Dashboard (node 1) has no NAME, so it does not take part in the address "
                          "claim and keeps sending from that address: give each node its own address; " + kNoEntries + "\n");
    // Two that cannot move: the lowest NAME wins, the other has no address.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kEngineName - 1)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0); Dashboard wins the address claim (its NAME is the lowest), and Engine is not arbitrary "
                          "address capable, so it is left without an address (Cannot Claim): give each node its own "
                          "address; " + kNoEntries + "\n");
    // One that cannot move and one that can: the first wins (the arbitrary address bit is the
    // highest of the NAME), the other moves.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 5)};
    check_text(s.check(), "W 3 address-shared: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0); Engine keeps it (its NAME is the lowest) and Dashboard will claim another address: give "
                          "each its own address to know where they are; " + kNoEntries + "\n");
    // The same lowest NAME twice: an equal NAME loses too, nobody keeps the address. One
    // finding says both.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kEngineName)};
    check_text(s.check(), "E 3 name-conflict: network 'J1939_CAN': Engine and Dashboard have the same J1939 NAME "
                          "0x00000000FE000001, and they ask for the same address 0x00 (0), which none of them keeps (an "
                          "equal NAME loses the address claim): each node needs its own NAME; " + kNoEntries + "\n");
    // The same NAME on other addresses: only the NAME.
    s.results[1].ecu->description.networks[0].nodes = {node(0x01, kEngineName)};
    check_text(s.check(), "E 3 name-conflict: network 'J1939_CAN': Engine and Dashboard have the same J1939 NAME "
                          "0x00000000FE000001: each node needs its own NAME; " + kNoEntries + "\n");
    // One that cannot move and two that can: the first keeps it, both others move.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 5), node(0x00, kArbitraryBit | 6)};
    check_text(s.check(), "W 3 address-shared: network 'J1939_CAN': Engine, Dashboard (node 1) and Dashboard (node 2) all "
                          "ask for the address 0x00 (0); Engine keeps it (its NAME is the lowest) and Dashboard (node 1) "
                          "and Dashboard (node 2) will claim another address: give each its own address to know where "
                          "they are; " + kNoEntries + "\n");
    // Two without a NAME, and one without a NAME next to one that could move: errors.
    s.results[0].ecu->description.networks[0].nodes = {node(0x00)};
    s.results[1].ecu->description.networks[0].nodes = {node(0x00)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0), and Engine and Dashboard have no NAME, so they do not take part in the address claim and "
                          "keep sending from that address: give each node its own address; " + kNoEntries + "\n");
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 5)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0), and Engine has no NAME, so it does not take part in the address claim and keeps sending "
                          "from that address: give each node its own address; " + kNoEntries + "\n");
    s.results[0].ecu->description.networks[0].nodes = {node(0x00, kEngineName)};
    // All may move: a warning. 254 (any free address) is never a conflict.
    s.results[0].ecu->description.networks[0].nodes = {node(0x80, kEngineName | kArbitraryBit),
                                                        node(j1939::kNullAddress, kArbitraryBit | 7)};
    s.results[1].ecu->description.networks[0].nodes = {node(0x80, kArbitraryBit | 9), node(j1939::kNullAddress, kArbitraryBit | 8)};
    check_text(s.check(), "W 3 address-shared: network 'J1939_CAN': Engine (node 1) and Dashboard (node 1) both ask for the "
                          "address 0x80 (128); Dashboard (node 1) keeps it (its NAME is the lowest) and Engine (node 1) "
                          "will claim another address: give each its own address to know where they are; " + kNoEntries + "\n");
    // Three on one address, on other networks no conflict.
    Setup three = good_pair();
    three.network("Other", NetworkKind::Can, 4);
    three.described(participant("Gateway", 12), ecu("Gateway", 10ms, {on_j1939("J1939_CAN", {node(0x28, kArbitraryBit | 3)}), on_j1939("Other", {node(0x00)})}));
    three.results[0].ecu->description.networks[0].nodes = {node(0x28, 2)};
    three.results[1].ecu->description.networks[0].nodes = {node(0x28, 1)};
    three.results[0].ecu->description.networks.push_back(on_j1939("Other", {node(0x01)}));
    check_text(three.check(), "E 3 address-conflict: network 'J1939_CAN': Engine, Dashboard and Gateway all ask for the "
                              "address 0x28 (40); Dashboard wins the address claim (its NAME is the lowest), and Engine is "
                              "not arbitrary address capable, so it is left without an address (Cannot Claim): give each "
                              "node its own address; " + kNoEntries + "\n");
}

TEST_CASE(j1939_names_must_differ_on_a_network)
{
    Setup s = good_pair();
    s.described(participant("Engine2", 12), ecu("Engine2", 10ms, {on_j1939("J1939_CAN", {node(0x01, kEngineName)})}));
    check_text(s.check(), "E 3 name-conflict: network 'J1939_CAN': Engine and Engine2 have the same J1939 NAME "
                          "0x00000000FE000001: each node needs its own NAME; " + kNoEntries + "\n");
    s.results[2].ecu->description.networks[0].nodes[0].name = j1939::Name{kEngineName + 1};
    check_text(s.check(), "");
}

TEST_CASE(an_ethernet_network_has_one_mac_address_per_participant)
{
    const auto on_eth = [](std::optional<eth::MacAddress> mac) {
        return NetworkDescription{"Backbone", NetworkUse::Ethernet, {}, mac};
    };
    const eth::MacAddress one{{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
    const eth::MacAddress two{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};
    Setup s;
    s.network("Backbone", NetworkKind::Ethernet, 3)
        .described(participant("Camera", 6), ecu("Camera", 5ms, {on_eth(one)}))
        .described(participant("Display", 9), ecu("Display", 5ms, {on_eth(two)}))
        .described(participant("Gateway", 12), ecu("Gateway", 5ms, {on_eth(std::nullopt)}));
    // Different addresses; the one without an address is a note.
    check_text(s.check(), "N 3 ethernet-mac-unknown: network 'Backbone': Gateway reports no MAC address, so the check "
                          "cannot tell whether two participants use the same one: pass the address to ethernet() "
                          "(eth::ChannelOptions::mac), or make sure by hand that the addresses differ\n");
    s.results[2].ecu->description.networks[0].mac = eth::MacAddress{{0x02, 0x00, 0x00, 0x00, 0x00, 0x03}};
    check_text(s.check(), "");
    // The same address twice: an error that names both and the address.
    s.results[1].ecu->description.networks[0].mac = one;
    check_text(s.check(), "E 3 ethernet-duplicate-mac: network 'Backbone': Camera and Display have the same MAC address "
                          "02:00:00:00:00:01, so their frames cannot be told apart: give each its own address "
                          "(eth::ChannelOptions::mac, and the source address of the frames it sends). If they share "
                          "the address on purpose, leave ChannelOptions::mac unset on all but one of them: the check "
                          "then only notes it (ethernet-mac-unknown), and frames they send in the same step arrive in "
                          "an order that can differ between runs\n");
    // Two without an address: one note for both, and no guess that they are the same.
    s.results[0].ecu->description.networks[0].mac.reset();
    s.results[1].ecu->description.networks[0].mac.reset();
    check_text(s.check(), "N 3 ethernet-mac-unknown: network 'Backbone': Camera and Display report no MAC address, so the "
                          "check cannot tell whether two participants use the same one: pass the address to ethernet() "
                          "(eth::ChannelOptions::mac), or make sure by hand that the addresses differ\n");
    // The same address on two networks is no conflict, and a disabled participant is not counted.
    Setup two_networks;
    two_networks.network("A", NetworkKind::Ethernet, 3).network("B", NetworkKind::Ethernet, 5);
    two_networks
        .described(participant("P", 8),
                   ecu("P", 5ms, {NetworkDescription{"A", NetworkUse::Ethernet, {}, one},
                                  NetworkDescription{"B", NetworkUse::Ethernet, {}, one}}))
        .described(participant("Q", 11),
                   ecu("Q", 5ms, {NetworkDescription{"A", NetworkUse::Ethernet, {}, two},
                                  NetworkDescription{"B", NetworkUse::Ethernet, {}, two}}));
    check_text(two_networks.check(), "");
    // A name that the file says is CAN, used as Ethernet: the kind is the error (no Ethernet rule).
    Setup wrong;
    wrong.network("Backbone", NetworkKind::Can, 3)
        .described(participant("Camera", 6), ecu("Camera", 5ms, {on_eth(one)}))
        .described(participant("Display", 9), ecu("Display", 5ms, {on_eth(one)}));
    CHECK(wrong.check().find("kind-mismatch: participant 'Camera': the executable uses the network 'Backbone' as ethernet "
                             "(ethernet), but networks says kind: can (line 3)") != std::string::npos);
    CHECK(wrong.check().find("ethernet-duplicate-mac") == std::string::npos);
}

TEST_CASE(a_bridge_without_a_mac_address_is_left_out_of_the_ethernet_address_rules)
{
    const auto on_eth = [](std::optional<eth::MacAddress> mac) {
        return NetworkDescription{"Backbone", NetworkUse::Ethernet, {}, mac};
    };
    const eth::MacAddress one{{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
    const eth::MacAddress two{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};
    // A bridge (role: bridge, in real time, as vehicle_sil_ethernet_bridge --describe says) carries
    // the frames of others: it has no address of its own to report.
    Setup s;
    s.network("Backbone", NetworkKind::Ethernet, 3)
        .described(participant("Camera", 6), ecu("Camera", 5ms, {on_eth(one)}))
        .described(with_role(participant("EthBridge", 9), ParticipantRole::Bridge), ecu("EthBridge", std::nullopt, {on_eth(std::nullopt)}));
    check_text(s.check(), "");
    // An ECU without an address is still a note, and a bridge that reports one is compared.
    s.described(participant("Gateway", 12), ecu("Gateway", 5ms, {on_eth(std::nullopt)}));
    CHECK(s.check().find("ethernet-mac-unknown: network 'Backbone': Gateway reports no MAC address") != std::string::npos);
    s.results[1].ecu->description.networks[0].mac = one;
    CHECK(s.check().find("ethernet-duplicate-mac: network 'Backbone': Camera and EthBridge have the same MAC address") !=
          std::string::npos);
    s.results[1].ecu->description.networks[0].mac = two;
    CHECK(s.check().find("EthBridge") == std::string::npos);
}

TEST_CASE(a_lin_network_has_one_master)
{
    const auto lin = [](NetworkUse use) { return NetworkDescription{"Body", use, {}}; };
    Setup s;
    s.network("Body", NetworkKind::Lin, 3)
        .described(participant("Gateway", 6), ecu("Gateway", 5ms, {lin(NetworkUse::LinMaster)}))
        .described(participant("Seat", 9), ecu("Seat", 5ms, {lin(NetworkUse::LinSlave)}))
        .described(participant("Door", 12), ecu("Door", 5ms, {lin(NetworkUse::LinSlave)}));
    check_text(s.check(), "");
    s.results[1].ecu->description.networks[0].use = NetworkUse::LinMaster;
    check_text(s.check(), "E 3 lin-masters: network 'Body': Gateway and Seat are both LIN masters, but a LIN network has "
                          "one master: make all but one of them slaves\n");
    s.results[0].ecu->description.networks[0].use = NetworkUse::LinSlave;
    s.results[1].ecu->description.networks[0].use = NetworkUse::LinSlave;
    check_text(s.check(), "E 3 lin-no-master: network 'Body': Gateway, Seat and Door are LIN slaves, but no participant is "
                          "the master: add the participant that is the master\n");
    // A participant that was not checked may be the master: a note.
    s.unchecked("Master", 15);
    check_text(s.check(), "N 15 not-checked: participant 'Master': not checked (describe: false): vehicle_sil_launch cannot "
                          "compare its name, step and networks with this file\n"
                          "N 3 lin-no-master: network 'Body': Gateway, Seat and Door are LIN slaves, but no participant is "
                          "the master (participants with describe: false are not counted): add the participant that is the "
                          "master\n");
}

TEST_CASE(reads_what_an_executable_says_about_offering_its_variables)
{
    EcuDescription d{"Engine", 10ms, {}, {variable("x")}};
    // Not said (an executable of an earlier vehicle-sil), or null (--no-control): not offered.
    CHECK(!read_description(to_json(d)).description.control.has_value());
    CHECK(!read_description(R"({"vehicle_sil_ecu": 1, "name": "A", "step_ns": 1000, "networks": []})").description.control.has_value());
    d.control = ControlOffer{"silkit", 3};
    const ReportedEcu read = read_description(to_json(d));
    CHECK(read.description == d);
    CHECK(read.description.control->transport == "silkit");
    CHECK_EQ(read.description.control->wire_version, 3);
    // Keys it does not know are ignored; what is there must be of the right kind.
    const std::string head = R"({"vehicle_sil_ecu": 1, "name": "A", "step_ns": 1000, "networks": [], "control": )";
    const ControlOffer other{"other", 2};
    CHECK(read_description(head + R"({"transport": "other", "wire_version": 2, "later": true}})").description.control == other);
    const auto refused = [&head](const std::string& control) {
        try
        {
            (void)read_description(head + control + "}");
        }
        catch (const DescriptionError& e)
        {
            return std::string(e.what());
        }
        return std::string("(read)");
    };
    check_text(refused("7"), "control is not an object");
    check_text(refused(R"({"wire_version": 1})"), "control: the key transport is missing");
    check_text(refused(R"({"transport": "silkit"})"), "control: the key wire_version is missing");
    check_text(refused(R"({"transport": "silkit", "wire_version": -1})"), "control.wire_version is -1, not a version");
    check_text(refused(R"({"transport": "silkit", "wire_version": 0})"), "control.wire_version is 0, not a version");
    check_text(refused(R"({"transport": "silkit", "wire_version": 70000})"), "control.wire_version is 70000, not a version");
}

TEST_CASE(variables_that_are_not_offered_are_a_note_and_another_version_of_the_messages_a_warning)
{
    // Variables, offered in the version of the messages this launcher speaks: nothing to say.
    Setup s = good_pair();
    s.results[0].ecu->description.variables = {variable("x")};
    // No variables: nothing to offer, whatever the executable says.
    s.results[1].ecu->description.control.reset();
    s.results[0].ecu->description.control = ControlOffer{"silkit", 2};
    check_text(s.check(), "");
    CHECK_EQ(control::wire::kVersion, std::uint16_t{2});
    // Variables that the executable does not offer (--no-control, an earlier vehicle-sil).
    s.results[1].ecu->description.variables = {variable("y")};
    check_text(s.check(), "N 9 control-link-not-offered: participant 'Dashboard': it describes variables, but does not offer "
                          "them to the simulation (--no-control, or an executable of a vehicle-sil that does not yet): "
                          "vehicle_sil_launch run cannot read or write them\n");
    // Offered in another version: known before anything is started, and the fix depends on which
    // side is the older one.
    s.results[1].ecu->description.control = ControlOffer{"silkit", 1};
    check_text(s.check(), "W 9 control-link-version: participant 'Dashboard': it offers its variables as silkit messages of "
                          "version 1, and this vehicle_sil_launch speaks version 2, so vehicle_sil_launch run cannot read or "
                          "write them: the ECU is of an older vehicle-sil; build it again with the vehicle-sil of this "
                          "vehicle_sil_launch\n");
    s.results[1].ecu->description.control = ControlOffer{"silkit", 3};
    check_text(s.check(), "W 9 control-link-version: participant 'Dashboard': it offers its variables as silkit messages of "
                          "version 3, and this vehicle_sil_launch speaks version 2, so vehicle_sil_launch run cannot read or "
                          "write them: the ECU is of a newer vehicle-sil; use a vehicle_sil_launch of its vehicle-sil or a "
                          "newer one\n");
    // ... or in another transport, whatever its version: not a matter of versions.
    for (const int version : {1, 2, 3})
    {
        s.results[1].ecu->description.control = ControlOffer{"other", version};
        check_text(s.check(), "W 9 control-link-version: participant 'Dashboard': it offers its variables over the transport "
                              "other, and this vehicle_sil_launch reads and writes variables over silkit only, so "
                              "vehicle_sil_launch run cannot read or write them: make the executable offer them over silkit "
                              "(as ecu_main does), or use a vehicle_sil_launch that knows the transport other\n");
    }
    s.results[0].ecu->description.control = ControlOffer{"silkit", 2};
    s.results[1].ecu->description.control = ControlOffer{"silkit", 3};
    // The check's JSON says what each executable offers.
    CheckReport r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string json = format_json(r);
    CHECK(json.find("      \"control\": {\n        \"transport\": \"silkit\",\n        \"wire_version\": 2\n      }\n") !=
          std::string::npos);
    CHECK(json.find("\"wire_version\": 3\n") != std::string::npos);
    // A disabled participant is not in the simulation: nothing about its variables.
    s.config.participants[1].enabled = false;
    CHECK(s.check().find("control-link") == std::string::npos);
}

TEST_CASE(every_mapping_of_variables_must_fit_what_the_executable_describes)
{
    control::VariableInfo rpm = variable("rpm_x10");
    rpm.type = control::ValueType::Integer;
    rpm.max = 80000;
    control::VariableInfo gear = variable("gear_raw");
    gear.type = control::ValueType::Enum;
    gear.items = {{"PARK", 0}, {"DRIVE", 3}};
    control::VariableInfo lamp = variable("lamp");
    lamp.type = control::ValueType::Bool;
    const auto mapping = [](const std::string& name, const std::string& from) {
        control::VariableMapping m;
        m.name = name;
        m.from = from;
        return m;
    };
    Setup s = good_pair();
    s.results[0].ecu->description.variables = {rpm, gear, lamp, variable("boost")};
    s.results[0].ecu->variables_reported = true;
    s.results[0].ecu->description.control = ControlOffer{"silkit", static_cast<int>(control::wire::kVersion)};
    ParticipantConfig& engine = s.config.participants[0];
    control::VariableMapping speed = mapping("engine.speed", "rpm_x10");
    speed.scale = 0.1;
    control::VariableMapping names = mapping("engine.gear", "gear_raw");
    names.map = std::vector<control::EnumItem>{{"P", 0}, {"D", 3}};
    control::VariableMapping turbo;
    turbo.name = "engine.turbo";
    turbo.absent = true;
    engine.variables = {speed, names, turbo, mapping("cabin.lamp", "lamp")};
    engine.variable_lines = {20, 21, 22, 23};
    check_text(s.check(), "");
    // What the simulation sees: the scenario's check, the JSON and the run's hub use the same.
    const control::MappedSchema seen = seen_variables(engine, *s.results[0].ecu);
    CHECK(seen.schema()[0].name == "engine.speed" && seen.schema()[0].type == control::ValueType::Real);
    CHECK(seen.schema()[0].max.has_value() && std::fabs(*seen.schema()[0].max - 8000.0) < 1e-6);
    CHECK(seen.schema()[1].name == "engine.gear" && seen.schema()[1].items.size() == 2);
    CHECK(seen.schema()[2].name == "cabin.lamp" && seen.schema()[3].name == "boost");

    control::VariableMapping lamp_scaled = mapping("cabin.lamp", "lamp");
    lamp_scaled.offset = 1.0;
    control::VariableMapping wrong_value = mapping("engine.gear", "gear_raw");
    wrong_value.map = std::vector<control::EnumItem>{{"P", 0}, {"D", 4}};
    control::VariableMapping there;
    there.name = "boost";
    there.absent = true;
    engine.variables = {mapping("engine.speed", "rpm_x1O"), lamp_scaled, wrong_value, mapping("lamp", "rpm_x10"),
                        mapping("engine.rpm", "rpm_x10"), there, mapping("engine.torque", "torque")};
    engine.variable_lines = {20, 21, 22, 23, 24, 25, 26};
    const std::string p = "participant 'Engine': variables: ";
    check_text(s.check(),
               "E 20 variable-map-unknown: " + p + "engine.speed: the executable has no variable 'rpm_x1O': did you mean "
               "'rpm_x10'? Correct the name, or write { absent: true } if this executable does not have the variable\n"
               "E 21 variable-map-type: " + p + "cabin.lamp: scale and offset need a number, and the executable's 'lamp' is "
               "a bool: remove them\n"
               "E 22 variable-map-value: " + p + "engine.gear: map gives D the value 4, which the executable's 'gear_raw' "
               "cannot take (its values are PARK (0), DRIVE (3)): correct the value\n"
               "E 23 variable-map-collision: " + p + "lamp: another variable of the executable is seen under the name "
               "'lamp' already: choose another name here, or map that variable to another name too\n"
               "E 24 variable-map-collision: " + p + "engine.rpm: the executable's 'rpm_x10' is mapped twice (also as "
               "lamp): keep one of the two\n"
               "E 25 variable-map-absent: " + p + "boost: it says absent: true, but the executable has a variable 'boost': "
               "remove this entry (a variable that is not listed is seen under its own name)\n"
               "E 26 variable-map-unknown: " + p + "engine.torque: the executable has no variable 'torque': its variables "
               "are rpm_x10, gear_raw, lamp, boost. Correct the name, or write { absent: true } if this executable does "
               "not have the variable\n");
    // A disabled participant's map is not compared.
    engine.enabled = false;
    CHECK(s.check().find("variable-map") == std::string::npos);
}

TEST_CASE(a_scenario_may_write_what_the_hub_takes_of_a_converted_variable)
{
    // The seen ends of a converted range are products, and may be off the number as written by a
    // rounding: 4.35 * 100 is 434.99999999999994, 3 * 0.1 is 0.30000000000000004. The scenario's
    // check asks the map, as the run's hub does.
    control::VariableInfo bar = variable("pressure_bar");
    bar.min = 0.0;
    bar.max = 4.35;
    control::VariableInfo raw = variable("raw");
    raw.type = control::ValueType::Integer;
    raw.min = 3;
    raw.max = 10;
    Setup s = good_pair();
    s.results[0].ecu->description.variables = {bar, raw};
    s.results[0].ecu->variables_reported = true;
    s.results[0].ecu->description.control = ControlOffer{"silkit", static_cast<int>(control::wire::kVersion)};
    control::VariableMapping pressure;
    pressure.name = "pressure";
    pressure.from = "pressure_bar";
    pressure.scale = 100.0;
    control::VariableMapping level;
    level.name = "level";
    level.from = "raw";
    level.scale = 0.1;
    s.config.participants[0].variables = {pressure, level};
    s.config.participants[0].variable_lines = {20, 21};
    check_text(s.check(), "");
    CheckReport report;
    report.file = "x.yaml";
    report.config = s.config;
    report.participants = s.results;
    const ScenarioCatalog catalog = scenario_catalog(report);
    const auto errors = [&catalog](const std::string& steps) {
        return lines(read_scenario("vehicle_sil_scenario: 1\nsteps:\n" + steps, catalog).findings);
    };
    check_text(errors("  - set: { Engine.pressure: 435, Engine.level: 0.3 }\n"
                      "  - set: { Engine.pressure: 0, Engine.level: 1 }\n"
                      "  - ramp: { Engine.pressure: { from: 435, to: 0, over_ms: 20 } }\n"
                      "  - ramp: { Engine.level: { from: 1, to: 0.3, over_ms: 20 } }\n"
                      // The executable's integer is the nearest one: 0.26 is 3, as the hub writes it.
                      "  - set: { Engine.level: 0.26 }\n"),
               "");
    // Beyond the ends: refused, with the range in the numbers one would write.
    check_text(errors("  - set: { Engine.pressure: 435.01 }\n"
                      "  - set: { Engine.level: 0.24 }\n"
                      "  - ramp: { Engine.pressure: { to: -0.01, over_ms: 20 } }\n"
                      "  - ramp: { Engine.level: { from: 1.06, to: 1, over_ms: 20 } }\n"),
               "E 3 scenario-value: the value of Engine.pressure (real) is 435.01, outside its range (from 0 to 435)\n"
               "E 4 scenario-value: the value of Engine.level (real) is 0.24, outside its range (from 0.3 to 1)\n"
               "E 5 scenario-value: to of the ramp of Engine.pressure is -0.01, outside its range (from 0 to 435)\n"
               "E 6 scenario-value: from of the ramp of Engine.level is 1.06, outside its range (from 0.3 to 1)\n");
}

TEST_CASE(another_vehicle_sil_version_is_a_note)
{
    Setup s = good_pair();
    s.results[1].ecu->vehicle_sil_version = "0.9.0";
    check_text(s.check(), "N 9 version-mismatch: participant 'Dashboard': the executable was built with vehicle-sil 0.9.0, "
                          "vehicle_sil_launch with 1.0.0: if they do not work together, build them from the same version\n");
}

TEST_CASE(participants_that_failed_or_were_skipped_are_not_compared)
{
    Setup s = good_pair();
    s.results[0] = ParticipantResult{ParticipantStatus::Failed, std::nullopt};
    s.results[1] = ParticipantResult{ParticipantStatus::Skipped, std::nullopt};
    // Who uses which network is not known: no rule that needs all of them.
    check_text(s.check(), "");
}

TEST_CASE(a_missing_description_holds_back_the_rules_that_need_all_of_them)
{
    // The LIN master is not built yet: its own error says so; no "no master", "only one", "unused".
    Setup s;
    s.network("Body", NetworkKind::Lin, 3).network("Spare", NetworkKind::Can, 5);
    s.described(participant("Seat", 6), ecu("Seat", 5ms, {NetworkDescription{"Body", NetworkUse::LinSlave, {}}}));
    s.config.participants.push_back(participant("Gateway", 9));
    s.results.push_back(ParticipantResult{ParticipantStatus::Failed, std::nullopt});
    check_text(s.check(), "");
    // The rules that need one participant still hold.
    s.results[0].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::LinMaster, {}});
    s.results[0].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::LinMaster, {}});
    s.results[0].ecu->description.networks[0].use = NetworkUse::LinMaster;
    check_text(s.check(), "E 3 lin-masters: network 'Body': Seat is a LIN master on it 3 times, but a LIN network has one "
                          "master: call lin_master() once for this network\n");
}

// --- the report -----------------------------------------------------------------------------

TEST_CASE(sorts_errors_before_warnings_each_by_line)
{
    std::vector<Finding> f{Finding{Severity::Warning, "w", "w5", 5, "", ""}, Finding{Severity::Error, "e", "e9", 9, "", ""},
                           Finding{Severity::Error, "e", "e2", 2, "", ""},   Finding{Severity::Warning, "w", "w1", 1, "", ""},
                           Finding{Severity::Error, "e", "e0", 0, "", ""},   Finding{Severity::Error, "e", "e2b", 2, "", ""}};
    sort_findings(f);
    std::string order;
    for (const auto& x : f)
    {
        order += x.message + " ";
    }
    check_text(order, "e0 e2 e2b e9 w1 w5 ");
}

namespace {

/// A report with a described, a failed, a skipped and an unchecked participant.
CheckReport mixed_report()
{
    Setup s = good_pair();
    s.network("Body", NetworkKind::Lin, 5);
    s.config.stop_at_ms = 1000;
    s.config.participants.push_back(participant("Cluster \xC3\xA9", 12));
    s.results.push_back(ParticipantResult{ParticipantStatus::Failed, std::nullopt});
    s.config.participants.push_back(participant("", 15));
    s.results.push_back(ParticipantResult{ParticipantStatus::Skipped, std::nullopt});
    s.unchecked("Logger", 18);
    s.results[0].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::LinMaster, {}});
    // On Body as CAN: not on the LIN network Body (the tables count as the rules do).
    s.results[1].ecu->description.networks.push_back(NetworkDescription{"Body", NetworkUse::Raw, {}});
    CheckReport r;
    r.file = "dir/simulation.yaml";
    r.config = s.config;
    r.participants = s.results;
    r.findings = {Finding{Severity::Error, "describe-failed", "participant 'Cluster \xC3\xA9': it failed", 13, "Cluster \xC3\xA9", "",
                          "the end of its standard error:\nboom\nbang"},
                  Finding{Severity::Error, "missing-key", "participants[3] has no name", 15, "", ""},
                  Finding{Severity::Warning, "single-participant", "network 'Body': only Engine", 5, "", "Body"},
                  Finding{Severity::Warning, "file-level", "no line", 0, "", ""},
                  Finding{Severity::Note, "not-checked", "participant 'Logger': not checked", 18, "Logger", ""}};
    return r;
}

} // namespace

TEST_CASE(the_text_report_shows_tables_then_the_findings)
{
    check_text(format_text(mixed_report()),
               "dir/simulation.yaml: 5 participants, 2 networks\n"
               "\n"
               "participant      step   networks\n"
               "Engine           10 ms  J1939_CAN (j1939 0x00), Body (lin_master)\n"
               "Dashboard        5 ms   J1939_CAN (j1939 0x28), Body (raw)\n"
               "Cluster \xC3\xA9        -      no description (see the errors)\n"
               "participants[3]  -      not read (see the errors)\n"
               "Logger           -      not checked (describe: false)\n"
               "\n"
               "network    kind  participants\n"
               "J1939_CAN  can   Engine, Dashboard\n"
               "Body       lin   Engine\n"
               "\n"
               "error: dir/simulation.yaml:13: participant 'Cluster \xC3\xA9': it failed [describe-failed]\n"
               "    the end of its standard error:\n"
               "    boom\n"
               "    bang\n"
               "error: dir/simulation.yaml:15: participants[3] has no name [missing-key]\n"
               "warning: dir/simulation.yaml:5: network 'Body': only Engine [single-participant]\n"
               "warning: dir/simulation.yaml: no line [file-level]\n"
               "note: dir/simulation.yaml:18: participant 'Logger': not checked [not-checked]\n"
               "\n"
               "failed: 2 errors, 2 warnings, 1 note\n");
}

TEST_CASE(the_text_report_summary_line)
{
    CheckReport r = mixed_report();
    r.findings.erase(r.findings.begin(), r.findings.begin() + 2);
    const std::string passed = format_text(r);
    check_text(passed.substr(passed.rfind('\n', passed.size() - 2) + 1), "passed: 0 errors, 2 warnings, 1 note\n");
    r.strict = true;
    const std::string strict = format_text(r);
    check_text(strict.substr(strict.rfind('\n', strict.size() - 2) + 1),
               "failed: 0 errors, 2 warnings, 1 note (--strict)\n");
    // A note does not fail --strict.
    r.findings.erase(r.findings.begin(), r.findings.begin() + 2);
    CHECK(r.passed());
    const std::string noted = format_text(r);
    check_text(noted.substr(noted.rfind('\n', noted.size() - 2) + 1), "passed: 0 errors, 0 warnings, 1 note\n");
    r.findings.clear();
    const std::string clean = format_text(r);
    check_text(clean.substr(clean.rfind("\n\n") + 2), "passed: 0 errors, 0 warnings, 0 notes\n");
    CheckReport unreadable;
    unreadable.file = "x.yaml";
    unreadable.findings = {Finding{Severity::Error, "syntax", "syntax error: missing terminating ]", 2, "", "",
                                   "2:9: networks: [a\n            ^"}};
    check_text(format_text(unreadable), "error: x.yaml:2: syntax error: missing terminating ] [syntax]\n"
                                        "    2:9: networks: [a\n"
                                        "                ^\n"
                                        "\n"
                                        "cannot read x.yaml: 1 error, 0 warnings, 0 notes\n");
}

TEST_CASE(the_json_report_has_every_key_in_a_fixed_order)
{
    CheckReport r = mixed_report();
    r.config->participants.resize(3);
    r.participants.resize(3);
    r.config->participants[1].command = {"dash board", "--x"};
    r.findings = {r.findings[0], r.findings[1], r.findings[4]};
    // Engine's variables as its description has them; Dashboard says it has none; Cluster failed.
    control::VariableInfo mode;
    mode.name = "engine.mode";
    mode.type = control::ValueType::Enum;
    mode.causality = control::Causality::Parameter;
    mode.min = 0;
    mode.description = "how it runs";
    mode.items = {{"off", 0}, {"on", 1}};
    r.participants[0].ecu->description.variables = {mode};
    r.participants[0].ecu->variables_reported = true;
    r.participants[1].ecu->variables_reported = true;
    const std::string v = version();
    check_text(format_json(r), R"({
  "vehicle_sil_check": 1,
  "vehicle_sil": ")" + v + R"(",
  "file": "dir/simulation.yaml",
  "result": "failed",
  "strict": false,
  "errors": 2,
  "warnings": 0,
  "notes": 1,
  "stop_at_ms": 1000,
  "start_timeout_ms": 30000,
  "stop_timeout_ms": 10000,
  "registry_uri": null,
  "participants": [
    {
      "name": "Engine",
      "line": 6,
      "command": [
        "Engine"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "role": "ecu",
      "step_ms": 10,
      "status": "described",
      "reported": {
        "vehicle_sil": "1.0.0",
        "name": "Engine",
        "step_ns": 10000000,
        "networks": [
          {
            "name": "J1939_CAN",
            "kind": "can",
            "use": "j1939",
            "nodes": [
              {
                "address": 0,
                "j1939_name": "0x00000000FE000001",
                "arbitrary_address_capable": false
              }
            ],
            "mac": null
          },
          {
            "name": "Body",
            "kind": "lin",
            "use": "lin_master",
            "nodes": [],
            "mac": null
          }
        ]
      },
      "variables": [
        {
          "name": "engine.mode",
          "type": "enum",
          "causality": "parameter",
          "unit": "",
          "min": 0,
          "max": null,
          "step": null,
          "description": "how it runs",
          "items": [
            {
              "name": "off",
              "value": 0
            },
            {
              "name": "on",
              "value": 1
            }
          ],
          "builtin": false
        }
      ],
      "network_map": null,
      "j1939_nodes": null,
      "control": null
    },
    {
      "name": "Dashboard",
      "line": 9,
      "command": [
        "dash board",
        "--x"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "role": "ecu",
      "step_ms": null,
      "status": "described",
      "reported": {
        "vehicle_sil": "1.0.0",
        "name": "Dashboard",
        "step_ns": 5000000,
        "networks": [
          {
            "name": "J1939_CAN",
            "kind": "can",
            "use": "j1939",
            "nodes": [
              {
                "address": 40,
                "j1939_name": null,
                "arbitrary_address_capable": false
              }
            ],
            "mac": null
          },
          {
            "name": "Body",
            "kind": "can",
            "use": "raw",
            "nodes": [],
            "mac": null
          }
        ]
      },
      "variables": [],
      "network_map": null,
      "j1939_nodes": null,
      "control": null
    },
    {
      "name": "Cluster é",
      "line": 12,
      "command": [
        "Cluster é"
      ],
      "describe": true,
      "required": true,
      "enabled": true,
      "manual": false,
      "role": "ecu",
      "step_ms": null,
      "status": "failed",
      "reported": null,
      "variables": null,
      "network_map": null,
      "j1939_nodes": null,
      "control": null
    }
  ],
  "networks": [
    {
      "name": "J1939_CAN",
      "kind": "can",
      "line": 3,
      "participants": [
        "Engine",
        "Dashboard"
      ],
      "outside": null
    },
    {
      "name": "Body",
      "kind": "lin",
      "line": 5,
      "participants": [
        "Engine"
      ],
      "outside": null
    }
  ],
  "findings": [
    {
      "severity": "error",
      "code": "describe-failed",
      "file": "dir/simulation.yaml",
      "line": 13,
      "participant": "Cluster é",
      "network": null,
      "message": "participant 'Cluster é': it failed",
      "detail": "the end of its standard error:\nboom\nbang"
    },
    {
      "severity": "error",
      "code": "missing-key",
      "file": "dir/simulation.yaml",
      "line": 15,
      "participant": null,
      "network": null,
      "message": "participants[3] has no name",
      "detail": null
    },
    {
      "severity": "note",
      "code": "not-checked",
      "file": "dir/simulation.yaml",
      "line": 18,
      "participant": "Logger",
      "network": null,
      "message": "participant 'Logger': not checked",
      "detail": null
    }
  ]
}
)");
    // A description without "variables" (an older ECU): not known.
    r.participants[1].ecu->variables_reported = false;
    CHECK(format_json(r).find("\n        ]\n      },\n      \"variables\": null,\n      \"network_map\": null,\n      \"j1939_nodes\": null,\n      \"control\": null\n    },\n    {\n      \"name\": \"Cluster") !=
          std::string::npos);
}

TEST_CASE(the_json_report_of_a_file_that_cannot_be_read)
{
    CheckReport r;
    r.file = "x.yaml";
    r.findings = {Finding{Severity::Error, "file", "the file does not exist: check the path", 0, "", ""}};
    check_text(format_json(r), R"({
  "vehicle_sil_check": 1,
  "vehicle_sil": ")" + std::string(version()) +
                                   R"(",
  "file": "x.yaml",
  "result": "unreadable",
  "strict": false,
  "errors": 1,
  "warnings": 0,
  "notes": 0,
  "stop_at_ms": null,
  "start_timeout_ms": null,
  "stop_timeout_ms": null,
  "registry_uri": null,
  "participants": [],
  "networks": [],
  "findings": [
    {
      "severity": "error",
      "code": "file",
      "file": "x.yaml",
      "line": null,
      "participant": null,
      "network": null,
      "message": "the file does not exist: check the path",
      "detail": null
    }
  ]
}
)");
}

TEST_CASE(the_json_report_of_a_wrong_command_line)
{
    CheckReport r;
    r.usage = true;
    r.findings = {Finding{Severity::Error, "usage", "unknown option --bogus", 0, "", ""}};
    CHECK(!r.passed());
    const std::string json = format_json(r);
    check_text(json.substr(json.find("\n  \"file\"")), R"(
  "file": null,
  "result": "usage",
  "strict": false,
  "errors": 1,
  "warnings": 0,
  "notes": 0,
  "stop_at_ms": null,
  "start_timeout_ms": null,
  "stop_timeout_ms": null,
  "registry_uri": null,
  "participants": [],
  "networks": [],
  "findings": [
    {
      "severity": "error",
      "code": "usage",
      "file": null,
      "line": null,
      "participant": null,
      "network": null,
      "message": "unknown option --bogus",
      "detail": null
    }
  ]
}
)");
}

TEST_CASE(a_run_ended_by_a_signal_or_an_exception_says_so)
{
    const auto error = [](const DescribeRun& r) { return interpret_describe(participant("Engine", 6), r).error->message; };
    DescribeRun crashed = run(128 + 11, "", "");
    crashed.signal = 11;
    check_text(error(crashed), "participant 'Engine': engine/engine_ecu --describe --name Engine was ended by the signal 11; "
                               "it crashed or was killed while it built the ECU: fix the ECU (its standard error may say "
                               "where)");
#ifdef _WIN32
    check_text(error(run(static_cast<int>(0xC0000005u))),
               "participant 'Engine': engine/engine_ecu --describe --name Engine ended with the exception code 0xC0000005; "
               "it crashed while it built the ECU: fix the ECU (its standard error may say where)");
#else
    // Not a Windows exception elsewhere (e.g. -1 when waiting failed).
    check_text(error(run(-1)), "participant 'Engine': engine/engine_ecu --describe --name Engine ended with exit code -1; "
                               "its set-up failed (see its standard error): fix the ECU, or set describe: false if it is "
                               "not made with ecu_main");
#endif
}

TEST_CASE(a_bridge_that_refused_its_arguments_is_told_to_fix_its_command)
{
    DescribeRun refused = run(4, "", "error: EthBridge: --name and --participant are the same option: give one of them\n");
    refused.command_text = "vehicle_sil_ethernet_bridge --participant EthBridge --describe --name EthBridge";
    const auto bridge = with_role(participant("EthBridge", 6), ParticipantRole::Bridge);
    check_text(lines({*interpret_describe(bridge, refused).error}),
               "E 7 describe-failed: participant 'EthBridge': vehicle_sil_ethernet_bridge --participant EthBridge "
               "--describe --name EthBridge ended with exit code 4; the bridge refused its arguments (its standard error "
               "says why): fix the command; vehicle_sil_launch adds --name itself, so do not write --participant or "
               "--name in the command\n"
               "  | the end of its standard error:\nerror: EthBridge: --name and --participant are the same option: give "
               "one of them\n");
    // Another exit code of a bridge, and exit code 4 of an ECU (role: ecu), keep their words.
    const auto message = [](const ParticipantConfig& p, int code) { return interpret_describe(p, run(code)).error->message; };
    CHECK(message(bridge, 3).find("its set-up failed (see its standard error)") != std::string::npos);
    CHECK(message(with_role(participant("Engine", 6), ParticipantRole::Ecu), 4)
              .find("it did not take --describe or --name: if it is not made with ecu_main, set describe: false") !=
          std::string::npos);
}

TEST_CASE(a_run_that_windows_could_not_start_for_a_dll_says_so)
{
    // Windows ends such a process before main() with these exit statuses; no other system gives
    // them, so the words are the same on every system.
    const auto error = [](const ParticipantConfig& p, unsigned code) {
        return interpret_describe(p, run(static_cast<int>(code))).error->message;
    };
    const auto engine = participant("Engine", 6);
    check_text(error(engine, 0xC0000135u),
               "participant 'Engine': engine/engine_ecu --describe --name Engine could not start (exit status "
               "0xC0000135): Windows did not find a DLL that it needs; put that DLL, of the version the executable was "
               "built with, next to the executable or in a folder on PATH");
    check_text(error(engine, 0xC0000139u),
               "participant 'Engine': engine/engine_ecu --describe --name Engine could not start (exit status "
               "0xC0000139): a DLL that it needs is of another version (a function in it is missing); put that DLL, of "
               "the version the executable was built with, next to the executable or in a folder on PATH");
    const auto bridge = with_role(participant("XlBridge", 6), ParticipantRole::Bridge);
    check_text(error(bridge, 0xC0000135u),
               "participant 'XlBridge': engine/engine_ecu --describe --name Engine could not start (exit status "
               "0xC0000135): Windows did not find a DLL that it needs; put that DLL, of the version the executable was "
               "built with, next to the executable or in a folder on PATH; if this computer does not use the bridge, "
               "leave it out with vehicle_sil_launch --disable XlBridge");
    const auto finding = *interpret_describe(bridge, run(static_cast<int>(0xC0000139u))).error;
    CHECK(finding.code == "describe-failed");
    CHECK(finding.message.find("--disable XlBridge") != std::string::npos);
}

TEST_CASE(a_start_the_system_refused_is_not_blamed_on_the_executable)
{
    DescribeRun refused = run(std::nullopt);
    refused.start_error = "cannot start /x/engine_ecu: AssignProcessToJobObject failed with Win32 error 5";
    refused.start_refused_by_system = true;
    check_text(interpret_describe(participant("Engine", 6), refused).error->message,
               "participant 'Engine': cannot start /x/engine_ecu: AssignProcessToJobObject failed with Win32 error 5: the "
               "system did not let vehicle_sil_launch start a process (see the reason); this is not about the "
               "executable");
}

TEST_CASE(a_cut_standard_error_is_said_where_its_end_is_shown)
{
    DescribeRun failed = run(3, "", "last line\n");
    failed.error_truncated = true;
    check_text(interpret_describe(participant("Engine", 6), failed).error->detail,
               "the end of its standard error (longer than 1 MiB; only its end was kept):\nlast line");
    failed.error_truncated = false;
    failed.output_truncated = true; // standard output does not matter here
    check_text(interpret_describe(participant("Engine", 6), failed).error->detail,
               "the end of its standard error:\nlast line");
}

TEST_CASE(broken_json_is_one_line_with_the_place_below)
{
    DescribeRun broken = run(0, "{\"vehicle_sil_ecu\": 1,\n  \"name\": [\n");
    broken.output_truncated = true;
    const Finding f = *interpret_describe(participant("Engine", 6), broken).error;
    CHECK(f.code == "describe-output");
    CHECK(f.message.find('\n') == std::string::npos);
    CHECK(f.message.find("its output is not a description: the output is not valid JSON (line ") != std::string::npos);
    CHECK(f.message.find("ERROR") == std::string::npos);
    CHECK(f.detail.rfind("its standard output was longer than 1 MiB and was cut there\n", 0) == 0);
    CHECK(f.detail.find("the start of its standard output:\n{\"vehicle_sil_ecu\": 1,\n  \"name\": [") != std::string::npos);
    // The parser's view of the place is between the two.
    CHECK(f.detail.find(":") < f.detail.find("the start of its standard output"));
}

TEST_CASE(the_arbitrary_address_bit_must_match_the_name)
{
    try
    {
        (void)read_description(R"({"vehicle_sil_ecu": 1, "vehicle_sil": "1", "name": "A", "step_ns": 1000, "networks": [)"
                               R"({"name": "N", "kind": "can", "use": "j1939", "nodes": [{"address": 1, )"
                               R"("j1939_name": "0x0000000000000001", "arbitrary_address_capable": true}]}]})");
        CHECK(false);
    }
    catch (const DescriptionError& e)
    {
        check_text(e.what(), "networks[0].nodes[0].arbitrary_address_capable is true, but the bit of its j1939_name says "
                             "the other");
    }
}

TEST_CASE(long_lines_are_cut_between_utf8_sequences)
{
    std::string line(299, 'a');
    line += "\xC3\xA9\xC3\xA9"; // the cut at 300 falls inside the first
    const Finding f = *interpret_describe(participant("Engine", 6), run(3, "", line + "\n")).error;
    check_text(f.detail, "the end of its standard error:\n" + std::string(299, 'a') + "...");
}

TEST_CASE(the_json_report_writes_null_for_what_is_not_there)
{
    SimulationConfig config;
    config.participants.push_back(participant("", 4));
    CheckReport r;
    r.file = "x.yaml";
    r.config = config;
    r.participants = {ParticipantResult{ParticipantStatus::Skipped, std::nullopt}};
    const std::string json = format_json(r);
    CHECK(json.find("\n      \"name\": null,\n      \"line\": 4,\n") != std::string::npos);
    r.participants = {ParticipantResult{ParticipantStatus::Described, ecu("", 10ms, {}, "")}};
    const std::string reported = format_json(r);
    CHECK(reported.find("\n      \"reported\": {\n        \"vehicle_sil\": null,\n        \"name\": null,\n") != std::string::npos);
}

TEST_CASE(the_reports_show_the_mac_address_of_an_ethernet_network)
{
    Setup s;
    s.network("Backbone", NetworkKind::Ethernet, 3)
        .described(participant("Camera", 6),
                   ecu("Camera", 5ms,
                       {NetworkDescription{"Backbone", NetworkUse::Ethernet, {}, eth::MacAddress{{0x02, 0, 0, 0, 0xAB, 0x01}}}}))
        .described(participant("Display", 9), ecu("Display", 5ms, {NetworkDescription{"Backbone", NetworkUse::Ethernet, {}}}));
    CheckReport r;
    r.file = "x.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string text = format_text(r);
    CHECK(text.find("Camera       5 ms  Backbone (ethernet 02:00:00:00:ab:01)\n"
                    "Display      5 ms  Backbone (ethernet)\n") != std::string::npos);
    CHECK(text.find("network   kind      participants\n"
                    "Backbone  ethernet  Camera, Display\n") != std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("\"kind\": \"ethernet\",\n            \"use\": \"ethernet\",\n            \"nodes\": [],\n"
                    "            \"mac\": \"02:00:00:00:ab:01\"\n") != std::string::npos);
    CHECK(json.find("\"nodes\": [],\n            \"mac\": null\n") != std::string::npos);
}

TEST_CASE(the_json_report_shows_the_seen_variables_and_the_map_of_an_entry_that_has_one)
{
    CheckReport r = mixed_report();
    control::VariableInfo rpm;
    rpm.name = "rpm_x10";
    rpm.type = control::ValueType::Integer;
    rpm.causality = control::Causality::Output;
    rpm.unit = "0.1 rpm";
    rpm.max = 80000;
    control::VariableInfo gear;
    gear.name = "gear_raw";
    gear.type = control::ValueType::Integer;
    r.participants[0].ecu->description.variables = {rpm, gear};
    r.participants[0].ecu->variables_reported = true;
    control::VariableMapping speed;
    speed.name = "engine.speed";
    speed.from = "rpm_x10";
    speed.scale = 0.5;
    speed.unit = "rpm";
    control::VariableMapping names;
    names.name = "engine.gear";
    names.from = "gear_raw";
    names.map = std::vector<control::EnumItem>{{"P", 0}, {"D", 3}};
    control::VariableMapping turbo;
    turbo.name = "engine.turbo";
    turbo.absent = true;
    r.config->participants[0].variables = {speed, names, turbo};
    r.config->participants[0].variable_lines = {7, 8, 9};
    const std::string json = format_json(r);
    const auto from = json.find("      \"variables\": [");
    const auto to = json.find("      \"control\":", from);
    CHECK(from != std::string::npos && to != std::string::npos);
    check_text(json.substr(from, to - from),
               R"(      "variables": [
        {
          "name": "engine.speed",
          "type": "real",
          "causality": "output",
          "unit": "rpm",
          "min": null,
          "max": 40000,
          "step": null,
          "description": "",
          "items": [],
          "builtin": false
        },
        {
          "name": "engine.gear",
          "type": "enum",
          "causality": "input",
          "unit": "",
          "min": null,
          "max": null,
          "step": null,
          "description": "",
          "items": [
            {
              "name": "P",
              "value": 0
            },
            {
              "name": "D",
              "value": 3
            }
          ],
          "builtin": false
        }
      ],
      "variable_map": [
        {
          "name": "engine.speed",
          "line": 7,
          "from": "rpm_x10",
          "absent": false,
          "scale": 0.5,
          "offset": 0,
          "unit": "rpm",
          "map": null
        },
        {
          "name": "engine.gear",
          "line": 8,
          "from": "gear_raw",
          "absent": false,
          "scale": null,
          "offset": null,
          "unit": null,
          "map": [
            {
              "name": "P",
              "value": 0
            },
            {
              "name": "D",
              "value": 3
            }
          ]
        },
        {
          "name": "engine.turbo",
          "line": 9,
          "from": null,
          "absent": true,
          "scale": null,
          "offset": null,
          "unit": null,
          "map": null
        }
      ],
      "network_map": null,
      "j1939_nodes": null,
)");
    // An entry without variables has no "variable_map" (the key says that the entry has a map).
    r.config->participants[0].variables.clear();
    CHECK(format_json(r).find("variable_map") == std::string::npos);
}

// --- disabled participants --------------------------------------------------------------------

namespace {

/// The good pair with Dashboard disabled.
Setup without_dashboard()
{
    Setup s = good_pair();
    s.config.participants[1].enabled = false;
    s.results[1] = ParticipantResult{ParticipantStatus::Disabled, std::nullopt};
    return s;
}

} // namespace

TEST_CASE(a_disabled_participant_is_a_note_and_the_rules_are_about_the_rest)
{
    // Not on its network any more: what follows from that is said (the rules do not wait for it).
    check_text(without_dashboard().check(),
               "N 9 participant-disabled: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither "
               "checked nor started, and the other findings are about the simulation without it\n"
               "W 3 single-participant: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: "
               "add the other participants of this network, or remove it\n");
    // Whatever its result says (an entry with an error, or a description): disabled is decisive,
    // and it holds back no rule.
    for (const ParticipantStatus status : {ParticipantStatus::Skipped, ParticipantStatus::Described,
                                           ParticipantStatus::NotChecked, ParticipantStatus::Failed})
    {
        Setup other = good_pair();
        other.config.participants[1].enabled = false;
        other.config.participants[1].name.clear();
        other.results[1].status = status;
        check_text(other.check(),
                   "N 9 participant-disabled: participants[1]: disabled (enabled: false, or --disable): it is neither "
                   "checked nor started, and the other findings are about the simulation without it\n"
                   "W 3 single-participant: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no "
                   "one: add the other participants of this network, or remove it\n");
    }
}

TEST_CASE(the_reports_show_a_disabled_participant)
{
    const Setup s = without_dashboard();
    CheckReport r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    r.findings = check_participants(s.config, s.results, "1.0.0");
    sort_findings(r.findings);
    check_text(format_text(r),
               "sim.yaml: 2 participants (1 disabled), 1 network\n"
               "\n"
               "participant  step   networks\n"
               "Engine       10 ms  J1939_CAN (j1939 0x00)\n"
               "Dashboard    -      disabled\n"
               "\n"
               "network    kind  participants\n"
               "J1939_CAN  can   Engine\n"
               "\n"
               "warning: sim.yaml:3: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add "
               "the other participants of this network, or remove it [single-participant]\n"
               "note: sim.yaml:9: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked "
               "nor started, and the other findings are about the simulation without it [participant-disabled]\n"
               "\n"
               "passed: 0 errors, 1 warning, 1 note\n");
    // The note does not fail --strict (the warning does).
    r.strict = true;
    r.findings.erase(r.findings.begin());
    CHECK(r.passed());
    const std::string json = format_json(r);
    CHECK(json.find("      \"required\": true,\n      \"enabled\": true,\n      \"manual\": false,\n"
                    "      \"role\": \"ecu\",\n      \"step_ms\": 10,\n      \"status\": \"described\",\n") != std::string::npos);
    CHECK(json.find("      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n"
                    "      \"role\": \"ecu\",\n      \"step_ms\": null,\n      \"status\": \"disabled\",\n"
                    "      \"reported\": null,\n      \"variables\": null,\n      \"network_map\": null,\n      \"j1939_nodes\": null,\n      \"control\": null\n") != std::string::npos);
    CHECK(json.find("      \"participants\": [\n        \"Engine\"\n      ],\n      \"outside\": null\n") != std::string::npos);
}

TEST_CASE(the_reports_show_a_role_that_is_not_ecu)
{
    Setup s = good_pair();
    s.described(with_role(participant("Spy", 12), ParticipantRole::Test),
                ecu("Spy", 10ms, {NetworkDescription{"J1939_CAN", NetworkUse::Raw, {}}}));
    s.unchecked("Gateway", 15);
    s.config.participants.back().role = ParticipantRole::Bridge;
    CheckReport r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string text = format_text(r);
    CHECK(text.find("Engine                  10 ms  J1939_CAN (j1939 0x00)\n") != std::string::npos);
    CHECK(text.find("Spy (role: test)        10 ms  J1939_CAN (raw)\n") != std::string::npos);
    CHECK(text.find("Gateway (role: bridge)  -      not checked (describe: false)\n") != std::string::npos);
    // The network table names the participant only.
    CHECK(text.find("J1939_CAN  can   Engine, Dashboard, Spy\n") != std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("      \"manual\": false,\n      \"role\": \"test\",\n      \"step_ms\": null,\n") != std::string::npos);
    CHECK(json.find("      \"manual\": false,\n      \"role\": \"bridge\",\n      \"step_ms\": null,\n") != std::string::npos);
}

TEST_CASE(check_file_neither_looks_for_nor_runs_a_disabled_executable)
{
    const auto dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "disabled";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "sim.yaml";
    std::ofstream(file) << "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                           "  - name: Engine\n    command: [missing/engine]\n"
                           "  - name: Ghost\n    command: [missing/ghost]\n    enabled: false\n";
    const CheckReport r = check_file(file, "sim.yaml", CheckOptions{});
    CHECK(r.config.has_value());
    CHECK(r.participants.size() == 2 && r.participants[0].status == ParticipantStatus::Failed &&
          r.participants[1].status == ParticipantStatus::Disabled && r.participants[1].executable.empty());
    std::string codes;
    for (const auto& f : r.findings)
    {
        codes += f.code + "(" + f.participant + ") ";
    }
    check_text(codes, "executable(Engine) participant-disabled(Ghost) ");
    // --disable and --enable change the file's choice; a name that the file does not have is a usage error.
    CheckOptions swapped;
    swapped.overrides = EnabledOverrides{{"Engine"}, {"Ghost"}};
    const CheckReport s = check_file(file, "sim.yaml", swapped);
    CHECK(!s.usage);
    CHECK(s.participants[0].status == ParticipantStatus::Disabled && s.participants[1].status == ParticipantStatus::Failed);
    CheckOptions wrong;
    wrong.overrides = EnabledOverrides{{"Gost"}, {}};
    const CheckReport u = check_file(file, "sim.yaml", wrong);
    CHECK(u.usage && !u.config && !u.passed());
    check_text(lines(u.findings), "E 0 usage: --disable Gost: the file has no participant with this name (did you mean "
                                  "'Ghost'?); its participants are Engine, Ghost\n");
    // The JSON of a usage error names the file.
    const std::string json = format_json(u);
    CHECK(json.find("\n  \"file\": \"sim.yaml\",\n  \"result\": \"usage\",\n") != std::string::npos);
    // A name given to both is a usage error before the file is read (here: there is none).
    CheckOptions both;
    both.overrides = EnabledOverrides{{"Ghost"}, {"Ghost"}};
    const CheckReport b = check_file(dir / "none.yaml", "none.yaml", both);
    CHECK(b.usage);
    check_text(lines(b.findings), "E 0 usage: --disable and --enable both name Ghost: give one of them\n");
}

TEST_CASE(check_file_a_disabled_entry_with_errors_and_an_unreadable_enabled)
{
    const auto dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "disabled-errors";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "sim.yaml";
    // Ghost is disabled and has no command; Maybe's enabled cannot be read: neither is looked for.
    std::ofstream(file) << "vehicle_sil_simulation: 1\nnetworks: []\nparticipants:\n"
                           "  - name: Engine\n    command: [missing/engine]\n"
                           "  - name: Ghost\n    enabled: false\n"
                           "  - name: Maybe\n    command: [missing/maybe]\n    enabled: no\n";
    const CheckReport r = check_file(file, "sim.yaml", CheckOptions{});
    CHECK(r.participants.size() == 3 && r.participants[1].status == ParticipantStatus::Disabled &&
          r.participants[2].status == ParticipantStatus::Skipped);
    std::string codes;
    for (const auto& f : r.findings)
    {
        codes += f.code + "(" + f.participant + ") ";
    }
    check_text(codes, "executable(Engine) missing-key(Ghost) value(Maybe) participant-disabled(Ghost) ");
    // The JSON says disabled for Ghost.
    CHECK(format_json(r).find("      \"enabled\": false,\n      \"manual\": false,\n      \"role\": \"ecu\",\n      \"step_ms\": null,\n      \"status\": \"disabled\",\n") !=
          std::string::npos);
}

// --- network_names -----------------------------------------------------------------------------

namespace {

/// `reported`, of an executable that takes --map-network (as ecu_main says in "features").
ReportedEcu taking_map(ReportedEcu reported)
{
    reported.description.features = {"map-network"};
    return reported;
}

/// `p` with the lines of network_names `names` (simulation's name, executable's name), one per
/// line of the file from `line` on.
ParticipantConfig named(ParticipantConfig p, std::size_t line, const std::vector<std::pair<std::string, std::string>>& names)
{
    for (const auto& [name, from] : names)
    {
        p.network_names.push_back(NetworkName{name, from, line++});
    }
    return p;
}

NetworkDescription raw(const std::string& name) { return NetworkDescription{name, NetworkUse::Raw, {}}; }

std::string network_of(const Finding& f) { return f.code + "(" + f.participant + "," + f.network + ")"; }

} // namespace

TEST_CASE(reads_the_features_of_a_description_and_keeps_the_names_it_does_not_know)
{
    const std::string head = R"({"vehicle_sil_ecu": 1, "name": "A", "step_ns": null, "networks": [])";
    // An open list: a name of a later vehicle-sil is no error.
    const ReportedEcu read = read_description(head + R"(, "features": ["map-network", "something-later"]})");
    CHECK(read.description.features == std::vector<std::string>({"map-network", "something-later"}));
    // An executable of a vehicle-sil before the key, and one that takes nothing: none.
    CHECK(read_description(head + "}").description.features.empty());
    CHECK(read_description(head + R"(, "features": []})").description.features.empty());
    for (const std::string bad : {R"(, "features": "map-network"})", R"(, "features": [1]})"})
    {
        bool refused = false;
        try
        {
            static_cast<void>(read_description(head + bad));
        }
        catch (const DescriptionError&)
        {
            refused = true;
        }
        CHECK(refused);
    }
}

TEST_CASE(a_renamed_network_is_under_the_simulations_name_in_every_rule)
{
    // Engine's code says J1939_CAN, the file says VehicleCAN, which is what Dashboard's code says.
    const auto pair = [](std::vector<std::pair<std::string, std::string>> names) {
        Setup s;
        s.network("VehicleCAN", NetworkKind::Can, 3)
            .described(named(participant("Engine", 6), 8, std::move(names)),
                       taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})})))
            .described(participant("Dashboard", 10), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}));
        return s;
    };
    Setup s = pair({{"VehicleCAN", "J1939_CAN"}});
    check_text(s.check(), "");
    // seen_networks() is what every rule reads: the simulation's name, with the executable's beside it.
    const std::vector<SeenNetwork> seen = seen_networks(s.config.participants[0], *s.results[0].ecu);
    CHECK(seen.size() == 1 && seen[0].network.name == "VehicleCAN" && seen[0].executable_name == "J1939_CAN" &&
          seen[0].renamed() && seen[0].line == 8 && seen[0].network.nodes.size() == 1);
    // Without the line, the names are two networks, as before.
    CHECK(pair({}).check().find("E 6 unknown-network: participant 'Engine': the executable uses the network 'J1939_CAN' "
                                "(j1939), which is not in networks. Another network of kind can in this simulation is "
                                "'VehicleCAN' (line 3, used by Dashboard): if 'J1939_CAN' is meant to be that bus, say so in "
                                "this file: add network_names: { VehicleCAN: J1939_CAN } to the participant 'Engine' (") == 0);
    // A line whose two names are the same is no line.
    CHECK(pair({{"J1939_CAN", "J1939_CAN"}}).check() == pair({}).check());

    // The rules of the kind: the nodes of both executables are on the one network.
    s.results[1].ecu->description.networks[0].nodes[0].address = 0x00;
    check_text(s.check(), "E 3 address-conflict: network 'VehicleCAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0), and Dashboard has no NAME, so it does not take part in the address claim and keeps sending "
                          "from that address: give each node its own address; " + kNoEntries + "\n");

    // The kind is compared under the simulation's name, and the message has both names.
    Setup k;
    k.network("VehicleCAN", NetworkKind::Can, 3)
        .network("Body", NetworkKind::Lin, 4)
        .described(named(participant("Engine", 6), 8, {{"Body", "J1939_CAN"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {})})));
    CHECK(k.check().find("E 6 kind-mismatch: participant 'Engine': the executable uses the network 'Body' ('J1939_CAN' in "
                         "the executable) as can (j1939), but networks says kind: lin (line 4): correct the kind, the line "
                         "of network_names (line 8), or the network in the ECU\n") == 0);
    CHECK(check_participants(k.config, k.results, "1.0.0")[0].network == "Body");

    // One master on a LIN network, whatever the executables call it.
    Setup l;
    l.network("Body", NetworkKind::Lin, 3)
        .described(named(participant("Door", 5), 7, {{"Body", "LIN_A"}}),
                   taking_map(ecu("Door", 10ms, {NetworkDescription{"LIN_A", NetworkUse::LinMaster, {}}})))
        .described(participant("Seat", 9), ecu("Seat", 10ms, {NetworkDescription{"Body", NetworkUse::LinMaster, {}}}));
    check_text(l.check(), "E 3 lin-masters: network 'Body': Door and Seat are both LIN masters, but a LIN network has one "
                          "master: make all but one of them slaves\n");

    // j1939() and can() of one name are both renamed; two names may be swapped.
    Setup w;
    w.network("A", NetworkKind::Can, 3)
        .network("B", NetworkKind::Can, 4)
        .described(named(participant("P", 6), 8, {{"A", "B"}, {"B", "A"}}),
                   taking_map(ecu("P", 10ms, {on_j1939("A", {node(0x10)}), raw("A"), raw("B")})))
        .described(participant("Q", 11), ecu("Q", 10ms, {on_j1939("B", {node(0x10)}), raw("A")}));
    check_text(w.check(), "E 4 address-conflict: network 'B': P and Q both ask for the address 0x10 (16), and P and Q have "
                          "no NAME, so they do not take part in the address claim and keep sending from that address: give "
                          "each node its own address; " + kNoEntries + "\n");
    CheckReport r;
    r.file = "sim.yaml";
    r.config = w.config;
    r.participants = w.results;
    CHECK(format_text(r).find("P            10 ms  B <- A (j1939 0x10), B <- A (raw), A <- B (raw)\n"
                              "Q            10 ms  B (j1939 0x10), A (raw)\n") != std::string::npos);
}

TEST_CASE(a_network_alone_under_a_renamed_name_says_which_line_to_replace)
{
    // Engine's J1939_CAN is VehicleCAN by a line of the file, and no one else is there; Dashboard
    // is alone on BodyCAN. The fix names the executable's name, and the line it replaces.
    Setup s;
    s.network("VehicleCAN", NetworkKind::Can, 3)
        .network("BodyCAN", NetworkKind::Can, 4)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAN"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)}), raw("Diag")})))
        .described(participant("Dashboard", 10), ecu("Dashboard", 10ms, {on_j1939("BodyCAN", {node(0x28)})}));
    check_text(s.check(),
               // Engine is on VehicleCAN itself, so that is not a candidate for its 'Diag'.
               "E 6 unknown-network: participant 'Engine': the executable uses the network 'Diag' (raw), which is not in "
               "networks. Another network of kind can in this simulation is 'BodyCAN' (line 4, used by Dashboard): if "
               "'Diag' is meant to be that bus, say so in this file: add network_names: { BodyCAN: Diag } to the "
               "participant 'Engine' (<simulation's name>: <executable's name>; the executable stays as it is); if 'Diag' "
               "is a bus of its own, add it to networks with kind: can\n"
               "W 3 single-participant: network 'VehicleCAN': only Engine is on it, so what it sends there reaches no "
               "one: add the other participants of this network, or remove it; the network 'BodyCAN' (line 4, only "
               "Dashboard) of kind can also has one ECU: if it is meant to be the same bus, keep one of the two names: "
               "remove 'VehicleCAN' from networks and add network_names: { BodyCAN: J1939_CAN } to the participant "
               "'Engine' (in place of VehicleCAN: J1939_CAN)\n"
               "W 4 single-participant: network 'BodyCAN': only Dashboard is on it, so what it sends there reaches no "
               "one: add the other participants of this network, or remove it; the network 'VehicleCAN' (line 3, only "
               "Engine) of kind can also has one ECU: if it is meant to be the same bus, keep one of the two names: "
               "remove 'BodyCAN' from networks and add network_names: { VehicleCAN: BodyCAN } to the participant "
               "'Dashboard'\n");
}

TEST_CASE(network_names_must_fit_what_the_executable_describes)
{
    // An executable of a vehicle-sil before --map-network (no "features" in its description): it
    // would end with a usage error when started. The other rules go on with the names applied.
    Setup old;
    old.network("VehicleCAN", NetworkKind::Can, 3)
        .network("Body", NetworkKind::Lin, 4)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAN"}, {"Body", "LIN"}}),
                   ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)}),
                                         NetworkDescription{"LIN", NetworkUse::LinMaster, {}}}, "0.9.0"))
        .described(participant("Dashboard", 12), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)}),
                                                                         NetworkDescription{"Body", NetworkUse::LinSlave, {}}}));
    check_text(old.check(),
               "E 8 network-map-unsupported: participant 'Engine': network_names needs an executable that takes "
               "--map-network, and this one does not say that it does (no \"map-network\" in the features of its "
               "--describe; it was built with vehicle-sil 0.9.0): have its author build it again with vehicle-sil 1.0.0 "
               "or later, or remove network_names and use the executable's own names in networks ('J1939_CAN' instead of "
               "'VehicleCAN' and 'LIN' instead of 'Body'), with network_names for the other participants on them\n"
               "N 6 version-mismatch: participant 'Engine': the executable was built with vehicle-sil 0.9.0, "
               "vehicle_sil_launch with 1.0.0: if they do not work together, build them from the same version\n");
    CHECK(network_of(check_participants(old.config, old.results, "1.0.0")[0]) == "network-map-unsupported(Engine,)");
    // A name of a later vehicle-sil beside it changes nothing; the name alone is what is asked for.
    old.results[0].ecu->description.features = {"something-later"};
    CHECK(old.check().find("E 8 network-map-unsupported: ") == 0);
    old.results[0].ecu->description.features = {"something-later", "map-network"};
    CHECK(old.check().find("network-map-unsupported") == std::string::npos);
    // An executable without the feature and an entry without lines (or with lines that rename nothing): as before.
    old.results[0].ecu->description.features = {};
    old.config.participants[0].network_names = {NetworkName{"VehicleCAN", "VehicleCAN", 8}};
    CHECK(old.check().find("network-map-") == std::string::npos);

    // The value is a network of the executable.
    Setup typo;
    typo.network("VehicleCAN", NetworkKind::Can, 3)
        .network("Body", NetworkKind::Lin, 4)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAM"}, {"Body", "Elsewhere"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)}), raw("J1939_CAN"),
                                                    NetworkDescription{"BodyLIN", NetworkUse::LinMaster, {}}})));
    const std::vector<Finding> found = check_participants(typo.config, typo.results, "1.0.0");
    CHECK(lines(found).find(
              "E 8 network-map-unknown: participant 'Engine': network_names says that 'VehicleCAN' is the executable's "
              "network 'J1939_CAM', but the executable has no network of that name (its networks: 'J1939_CAN' (j1939), "
              "'J1939_CAN' (raw), 'BodyLIN' (lin_master)): correct the value (did you mean 'J1939_CAN'?), or remove the "
              "line\n"
              "E 9 network-map-unknown: participant 'Engine': network_names says that 'Body' is the executable's network "
              "'Elsewhere', but the executable has no network of that name (its networks: 'J1939_CAN' (j1939), "
              "'J1939_CAN' (raw), 'BodyLIN' (lin_master)): correct the value, or remove the line\n") == 0);
    CHECK(network_of(found[0]) == "network-map-unknown(Engine,VehicleCAN)" && network_of(found[1]) == "network-map-unknown(Engine,Body)");
    // One cause, one finding: the line with the wrong value is what to correct. Without it the
    // executable's J1939_CAN is not in networks, and VehicleCAN has Dashboard alone; neither is
    // said (following "add network_names: { VehicleCAN: J1939_CAN }" would write the key twice).
    // What the line has nothing to do with is said as ever: the network 'Diag' of the same
    // executable, and the network Body that no one uses.
    Setup one;
    one.network("VehicleCAN", NetworkKind::Can, 3)
        .network("Body", NetworkKind::Lin, 4)
        .described(named(participant("Engine", 6), 9, {{"VehicleCAN", "J1939_CAM"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})})))
        .described(participant("Dashboard", 11), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}));
    check_text(one.check(),
               "E 9 network-map-unknown: participant 'Engine': network_names says that 'VehicleCAN' is the executable's "
               "network 'J1939_CAM', but the executable has no network of that name (its networks: 'J1939_CAN' (j1939)): "
               "correct the value (did you mean 'J1939_CAN'?), or remove the line\n"
               "W 4 unused-network: network 'Body': no participant uses it: remove it, or check the network names in the "
               "ECUs\n");
    one.results[0].ecu->description.networks.push_back(raw("Diag"));
    const std::string more = one.check();
    CHECK(more.find("E 9 network-map-unknown: ") != std::string::npos);
    CHECK(more.find("E 6 unknown-network: participant 'Engine': the executable uses the network 'Diag' (raw), which is not "
                    "in networks") != std::string::npos);
    CHECK(more.find("the network 'J1939_CAN'") == std::string::npos && more.find("single-participant") == std::string::npos);
    CHECK(more.find("W 4 unused-network: network 'Body'") != std::string::npos);
    // A value that is like none of the executable's names: its networks of the key's kind may be
    // the one that is meant, so they wait for the line; a network of another kind does not.
    Setup far;
    far.network("VehicleCAN", NetworkKind::Can, 3)
        .described(named(participant("Engine", 6), 9, {{"VehicleCAN", "Elsewhere"}}),
                   taking_map(ecu("Engine", 10ms, {raw("CAN_1"), raw("CAN_2"), NetworkDescription{"LIN_1", NetworkUse::LinMaster, {}}})));
    const std::string distant = far.check();
    CHECK(distant.find("E 9 network-map-unknown: ") == 0);
    CHECK(distant.find("'CAN_1' (raw), which") == std::string::npos && distant.find("'CAN_2' (raw), which") == std::string::npos);
    CHECK(distant.find("E 6 unknown-network: participant 'Engine': the executable uses the network 'LIN_1' (lin_master)") != std::string::npos);

    Setup none;
    none.network("VehicleCAN", NetworkKind::Can, 3)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAN"}}), taking_map(ecu("Engine", 10ms, {})));
    CHECK(none.check().find("but the executable has no network of that name (it has no networks): correct the value, or "
                            "remove the line\n") != std::string::npos);

    // Two networks of the executable under one name would be one bus.
    Setup twice;
    twice.network("VehicleCAN", NetworkKind::Can, 3)
        .network("BodyCAN", NetworkKind::Can, 4)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAN"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)}), raw("VehicleCAN")})))
        .described(participant("Dashboard", 10), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}));
    const std::vector<Finding> clash = check_participants(twice.config, twice.results, "1.0.0");
    CHECK(lines(clash).find(
              "E 8 network-map-collision: participant 'Engine': with network_names, the executable's 'J1939_CAN' becomes "
              "'VehicleCAN', and the executable also has a network it calls 'VehicleCAN' itself, so two of its networks "
              "would be one bus: give the executable's own 'VehicleCAN' another name of networks too (network_names: { "
              "<another name in networks>: VehicleCAN }), or remove this line\n") == 0);
    CHECK(network_of(clash[0]) == "network-map-collision(Engine,VehicleCAN)");
    // With the other one renamed as well, nothing is wrong.
    twice.config.participants[0].network_names.push_back(NetworkName{"BodyCAN", "VehicleCAN", 9});
    CHECK(twice.check().find("network-map-") == std::string::npos);
}

TEST_CASE(a_disabled_participant_with_network_names_is_not_compared)
{
    // Its lines would be wrong for its executable (no such network, no --map-network): it is not
    // in the simulation, so the check does not say so, and the rules are about the rest.
    Setup s;
    s.network("VehicleCAN", NetworkKind::Can, 3)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAM"}}),
                   ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})}))
        .described(participant("Dashboard", 10), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}));
    s.config.participants[0].enabled = false;
    s.results[0] = ParticipantResult{ParticipantStatus::Disabled, std::nullopt};
    check_text(s.check(), "N 6 participant-disabled: participant 'Engine': disabled (enabled: false, or --disable): it is "
                          "neither checked nor started, and the other findings are about the simulation without it\n"
                          "W 3 single-participant: network 'VehicleCAN': only Dashboard is on it, so what it sends there "
                          "reaches no one: add the other participants of this network, or remove it\n");
}

TEST_CASE(the_reports_show_both_names_of_a_renamed_network)
{
    Setup s;
    s.network("VehicleCAN", NetworkKind::Can, 3)
        .network("Body", NetworkKind::Lin, 4)
        .described(named(participant("Engine", 6), 8, {{"VehicleCAN", "J1939_CAN"}, {"Body", "Body"}}),
                   taking_map(ecu("Engine", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)}),
                                                    NetworkDescription{"Body", NetworkUse::LinMaster, {}}})))
        .described(participant("Dashboard", 11), ecu("Dashboard", 10ms, {on_j1939("VehicleCAN", {node(0x28)})}))
        .unchecked("Door", 13);
    s.config.participants[2].network_names = {NetworkName{"Body", "DoorLIN", 15}};
    CheckReport r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    // For people: the simulation's name, then the executable's; the network table has the simulation's only.
    const std::string text = format_text(r);
    CHECK(text.find("participant  step   networks\n"
                    "Engine       10 ms  VehicleCAN <- J1939_CAN (j1939 0x00), Body (lin_master)\n"
                    "Dashboard    10 ms  VehicleCAN (j1939 0x28)\n"
                    "Door         -      not checked (describe: false)\n"
                    "\n"
                    "network     kind  participants\n"
                    "VehicleCAN  can   Engine, Dashboard\n"
                    "Body        lin   Engine\n") != std::string::npos);
    // For programs: "reported" is what the executable said, under its names; network_map has the
    // lines as written (also the one that renames nothing), null for an entry that has none; the
    // networks' participants are under the simulation's names.
    const std::string json = format_json(r);
    CHECK(json.find("        \"networks\": [\n"
                    "          {\n"
                    "            \"name\": \"J1939_CAN\",\n"
                    "            \"kind\": \"can\",\n"
                    "            \"use\": \"j1939\",\n") != std::string::npos);
    CHECK(json.find("      \"network_map\": [\n"
                    "        {\n"
                    "          \"name\": \"VehicleCAN\",\n"
                    "          \"line\": 8,\n"
                    "          \"from\": \"J1939_CAN\"\n"
                    "        },\n"
                    "        {\n"
                    "          \"name\": \"Body\",\n"
                    "          \"line\": 9,\n"
                    "          \"from\": \"Body\"\n"
                    "        }\n"
                    "      ],\n") != std::string::npos);
    CHECK(json.find("      \"network_map\": [\n"
                    "        {\n"
                    "          \"name\": \"Body\",\n"
                    "          \"line\": 15,\n"
                    "          \"from\": \"DoorLIN\"\n"
                    "        }\n"
                    "      ],\n") != std::string::npos);
    CHECK(json.find("      \"name\": \"VehicleCAN\",\n      \"kind\": \"can\",\n      \"line\": 3,\n      \"participants\": [\n"
                    "        \"Engine\",\n        \"Dashboard\"\n      ]") != std::string::npos);
    // An entry without network_names has the key, null: every participant has it.
    const auto count = [](const std::string& in, const std::string& what) {
        std::size_t n = 0;
        for (auto at = in.find(what); at != std::string::npos; at = in.find(what, at + 1))
        {
            ++n;
        }
        return n;
    };
    CHECK_EQ(count(json, "\"network_map\": ["), std::size_t{2});
    CHECK_EQ(count(json, "\"network_map\""), r.config->participants.size());
    const std::string mixed = format_json(mixed_report());
    CHECK(count(mixed, "\"network_map\": null") > 0);
    CHECK_EQ(count(mixed, "\"network_map\": null"), count(mixed, "\"network_map\""));
}

// --- j1939_nodes ---------------------------------------------------------------------------------

namespace {

/// `reported` of an executable that takes --map-network and --j1939-node.
ReportedEcu taking_nodes(ReportedEcu reported)
{
    reported.description.features = {"map-network", "j1939-node"};
    return reported;
}

/// `p` with the entries of j1939_nodes `entries` (network, settings), one per line of the file from
/// `line` on.
ParticipantConfig with_nodes(ParticipantConfig p, std::size_t line,
                             const std::vector<std::pair<std::string, j1939::NodeOverride>>& entries)
{
    for (const auto& [network, node] : entries)
    {
        p.j1939_nodes.push_back(J1939NodeEntry{{network, node}, line++});
    }
    return p;
}

j1939::NodeOverride change(std::optional<j1939::Address> from, std::optional<j1939::Address> address,
                           std::optional<std::uint32_t> identity = std::nullopt)
{
    return j1939::NodeOverride{from, address, identity, std::nullopt, std::nullopt};
}

/// Two copies of one engine (the same NAME at 0x00, on J1939_CAN) and a dashboard (0x28, no NAME):
/// Engine at line 6, Dashboard at 9 (good_pair()), Engine2 at 12 with `entries` from line 14.
Setup two_engines(const std::vector<std::pair<std::string, j1939::NodeOverride>>& entries = {})
{
    Setup s = good_pair();
    s.results[0].ecu = taking_nodes(*s.results[0].ecu);
    s.described(with_nodes(participant("Engine2", 12, 10), 14, entries),
                taking_nodes(ecu("Engine2", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName)})})));
    return s;
}

/// The lines of the findings that have `part` in them; with `part` "E ", the errors.
std::string only(const std::string& findings, const std::string& part)
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
std::string nodes_of(const ParticipantConfig& p)
{
    std::string out;
    for (const J1939NodeEntry& e : p.j1939_nodes)
    {
        out += e.network + "=" + j1939::format_node_override(e.node) + "@" + std::to_string(e.line) + " ";
    }
    return out;
}

/// Reads "key: value, key: value" of a fix into `entry` (numbers as written: 0x01, 254, 2).
void read_settings(const std::string& text, J1939NodeEntry& entry)
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
std::size_t write_fixes(Setup& s)
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

} // namespace

TEST_CASE(j1939_nodes_apply_before_the_address_and_name_rules)
{
    // Without an entry: the two copies have the same NAME and address. The way to fix it is a line
    // of this file (the executables take j1939_nodes), for the last of them in the file.
    check_text(two_engines().check(),
               "E 3 name-conflict: network 'J1939_CAN': Engine and Engine2 have the same J1939 NAME 0x00000000FE000001, "
               "and they ask for the same address 0x00 (0), which none of them keeps (an equal NAME loses the address "
               "claim): each node needs its own NAME; to do it in this file, write under participant 'Engine2': "
               "j1939_nodes: [{ network: J1939_CAN, address: 0x01, identity_number: 2 }]\n");
    // That line: no error, a note of what changes.
    const std::string note = "N 14 j1939-node-changed: participant 'Engine2': j1939_nodes changes its nodes: on "
                             "'J1939_CAN', the node at 0x00 in its code is at 0x01";
    const std::string relied = ". Only Engine2 changes, and the check cannot see who relies on the old values: an ECU "
                               "whose code has 0x00 written in it for Engine2 (as a destination, or to tell senders "
                               "apart) still uses 0x00, and Engine2's own code may still have 0x00 in a filter or in the "
                               "data it sends";
    const std::string end = "; if messages do not reach Engine2 or are taken for another ECU's, the author of that code "
                            "has to change it\n";
    check_text(two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}}).check(),
               note + ", with the NAME 0x00000000FE000002 (0x00000000FE000001 in its code)" + relied +
                   ", and an ECU that looks Engine2 up by its whole NAME 0x00000000FE000001 does not find it" + end);
    // The address alone: the NAMEs are still the same. The node's name says where its values come from.
    check_text(two_engines({{"J1939_CAN", change(std::nullopt, 0x01)}}).check(),
               note + relied + end +
                   "E 3 name-conflict: network 'J1939_CAN': Engine and Engine2 (set by j1939_nodes, line 14) have the "
                   "same J1939 NAME 0x00000000FE000001: each node needs its own NAME; to do it in this file, set "
                   "identity_number: 2 in the entry of j1939_nodes on line 14 (participant 'Engine2'), keeping its other "
                   "settings\n");
    // Onto another participant's address: that is the conflict now.
    check_text(only(two_engines({{"J1939_CAN", change(0x00, 0x28, 2)}}).check(), "E "),
               "E 3 address-conflict: network 'J1939_CAN': Dashboard and Engine2 (set by j1939_nodes, line 14) both ask "
               "for the address 0x28 (40), and Dashboard has no NAME, so it does not take part in the address claim and "
               "keeps sending from that address: give each node its own address; to do it in this file, set address: 0x01 "
               "in the entry of j1939_nodes on line 14 (participant 'Engine2'), keeping its other settings\n");
    // With network_names: the entry names the simulation's network, the executable has its own name.
    Setup renamed = two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}});
    renamed.config.participants[2].network_names = {NetworkName{"J1939_CAN", "ENGINE_CAN", 13}};
    renamed.results[2].ecu->description.networks[0].name = "ENGINE_CAN";
    check_text(only(renamed.check(), "E "), "");
    CHECK(only(renamed.check(), "j1939-node-changed").find("on 'J1939_CAN', the node at 0x00 in its code is at 0x01") !=
          std::string::npos);
}

TEST_CASE(j1939_nodes_are_matched_against_the_code_addresses_only)
{
    // Two entries swap the addresses of two nodes: each is matched against the code's address.
    Setup s = good_pair();
    s.described(with_nodes(participant("Gateway", 12), 14, {{"J1939_CAN", change(0x00, 0x01)}, {"J1939_CAN", change(0x01, 0x00)}}),
                taking_nodes(ecu("Gateway", 10ms, {on_j1939("J1939_CAN", {node(0x00, kEngineName + 10), node(0x01, kEngineName + 11)})})));
    s.results[0].ecu->description.networks[0].nodes = {node(0x30, kEngineName)};
    check_text(s.check(),
               "N 14 j1939-node-changed: participant 'Gateway': j1939_nodes changes its nodes: on 'J1939_CAN', the node at "
               "0x00 in its code is at 0x01; the node at 0x01 in its code is at 0x00. Only Gateway changes, and the check "
               "cannot see who relies on the old values: an ECU whose code has 0x00 or 0x01 written in it for Gateway (as "
               "a destination, or to tell senders apart) still uses 0x00 or 0x01, and Gateway's own code may still have "
               "0x00 or 0x01 in a filter or in the data it sends; if messages do not reach Gateway or are taken for "
               "another ECU's, the author of that code has to change it\n");
    const std::vector<SeenNetwork> seen = seen_networks(s.config.participants[2], *s.results[2].ecu);
    CHECK(seen[0].network.nodes[0].address == 0x01 && seen[0].network.nodes[1].address == 0x00);
    CHECK(seen[0].coded_nodes[0].address == 0x00 && seen[0].coded_nodes[1].address == 0x01);
    CHECK(seen[0].node_lines == std::vector<std::size_t>({14, 15}));
}

TEST_CASE(a_j1939_nodes_entry_that_takes_no_node_says_which_nodes_there_are)
{
    // Gateway (line 12) with its networks, one entry on line 14; the findings about j1939_nodes.
    const auto findings = [](std::vector<NetworkDescription> networks, const std::string& network, j1939::NodeOverride entry,
                             const std::vector<NetworkName>& names = {}) {
        Setup s = good_pair();
        s.network("VehicleCAN", NetworkKind::Can, 4);
        ParticipantConfig p = with_nodes(participant("Gateway", 12), 14, {{network, entry}});
        p.network_names = names;
        s.described(p, taking_nodes(ecu("Gateway", 10ms, std::move(networks))));
        const std::vector<Finding> all = check_participants(s.config, s.results, "1.0.0");
        std::string out;
        for (const Finding& f : all)
        {
            if (f.code.rfind("j1939-node", 0) == 0)
            {
                CHECK(f.participant == "Gateway" && f.network == network && f.line == 14);
                out += lines({f});
            }
        }
        // The entry's error comes alone: Gateway's nodes are not compared on that network.
        CHECK(only(lines(all), "-conflict").empty() && only(lines(all), "address-shared").empty());
        return out;
    };
    const std::string later = "nodes that it adds after it starts are not described and cannot be changed";
    // No node at from.
    check_text(findings({on_j1939("J1939_CAN", {node(0x00, kEngineName)})}, "J1939_CAN", change(0x10, 0x01)),
               "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes the node that the "
               "executable's code puts at 0x10 on 'J1939_CAN', but the executable has no node at that address there "
               "(its nodes there: 0x00; " + later + "). from is the address in the executable's code (the one the table "
               "shows without j1939_nodes), not the new address: write from: 0x00\n");
    check_text(findings({on_j1939("J1939_CAN", {node(0x00, kEngineName), node(0x21)})}, "J1939_CAN", change(0x10, 0x01)),
               "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes the node that the "
               "executable's code puts at 0x10 on 'J1939_CAN', but the executable has no node at that address there "
               "(its nodes there: 0x00, 0x21; " + later + "). from is the address in the executable's code (the one the "
               "table shows without j1939_nodes), not the new address: write from: with one of them\n");
    // Nodes that share an address in the code cannot be named with from.
    const J1939NodeDescription anyA = node(j1939::kNullAddress, kArbitraryBit | 1);
    const J1939NodeDescription anyB = node(j1939::kNullAddress, kArbitraryBit | 2);
    check_text(findings({on_j1939("J1939_CAN", {anyA, anyB})}, "J1939_CAN", change(0x10, 0x01)),
               "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes the node that the "
               "executable's code puts at 0x10 on 'J1939_CAN', but the executable has no node at that address there "
               "(its nodes there: any (254); " + later + "). from is the address in the executable's code (the one the "
               "table shows without j1939_nodes), not the new address. The 2 nodes at any (254) cannot be told apart, "
               "so this file cannot change them: remove the entry\n");
    check_text(findings({on_j1939("J1939_CAN", {anyA, node(0x00, kEngineName), anyB})}, "J1939_CAN", change(0x10, 0x01)),
               "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes the node that the "
               "executable's code puts at 0x10 on 'J1939_CAN', but the executable has no node at that address there "
               "(its nodes there: any (254), 0x00; " + later + "). from is the address in the executable's code (the one "
               "the table shows without j1939_nodes), not the new address: write from: 0x00 (the 2 nodes at any (254) "
               "cannot be told apart, and this file cannot change them)\n");
    // No node on the network, with or without from.
    const std::string none = "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes a node on "
                             "'J1939_CAN', but the executable has no node there (" + later +
                             "): remove the entry, or correct network\n";
    check_text(findings({on_j1939("J1939_CAN", {})}, "J1939_CAN", change(std::nullopt, 0x01)), none);
    check_text(findings({on_j1939("J1939_CAN", {})}, "J1939_CAN", change(0x00, 0x01)), none);
    // No J1939 network of that name: another one, a renamed one, none.
    // (Gateway's nodes on J1939_CAN are compared there as before: the entry is not for them.)
    check_text(findings({on_j1939("J1939_CAN", {node(0x05)})}, "VehicleCAN", change(std::nullopt, 0x01)),
               "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes a node on 'VehicleCAN', but "
               "the executable has no J1939 network 'VehicleCAN' (its J1939 networks: 'J1939_CAN'): if its 'J1939_CAN' is "
               "the simulation's 'VehicleCAN', add network_names: { VehicleCAN: J1939_CAN }; otherwise correct network\n");
    check_text(findings({on_j1939("GW_CAN", {node(0x05)})}, "VehicleCAN", change(std::nullopt, 0x01), {NetworkName{"J1939_CAN", "GW_CAN", 13}}),
               "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes a node on 'VehicleCAN', but "
               "the executable has no J1939 network 'VehicleCAN' (its J1939 networks: 'J1939_CAN' (GW_CAN in its code)): "
               "correct network\n");
    check_text(findings({raw("VehicleCAN")}, "VehicleCAN", change(std::nullopt, 0x01)),
               "E 14 j1939-node-unknown: participant 'Gateway': j1939_nodes (line 14) changes a node on 'VehicleCAN', but "
               "the executable has no J1939 network: remove the entry\n");
    // No from, and nodes at different addresses: from tells them apart. Two at 254 cannot be.
    check_text(findings({on_j1939("J1939_CAN", {node(0x00, kEngineName), node(0x21)})}, "J1939_CAN", change(std::nullopt, 0x05)),
               "E 14 j1939-node-ambiguous: participant 'Gateway': j1939_nodes (line 14) has no from, and the executable "
               "has 2 nodes on 'J1939_CAN' (0x00, 0x21): say which one with from: <the address in the executable's code>, "
               "for example from: 0x00\n");
    const J1939NodeDescription any1 = node(j1939::kNullAddress, kArbitraryBit | 1);
    const J1939NodeDescription any2 = node(j1939::kNullAddress, kArbitraryBit | 2);
    check_text(findings({on_j1939("J1939_CAN", {node(0x10, kEngineName), any1, any2})}, "J1939_CAN", change(std::nullopt, 0x05)),
               "E 14 j1939-node-ambiguous: participant 'Gateway': j1939_nodes (line 14) has no from, and the executable "
               "has 3 nodes on 'J1939_CAN' (0x10); the 2 nodes at any (254) cannot be told apart, and this file cannot "
               "change them: say which one with from: <the address in the executable's code>, for example from: 0x10\n");
    // Two nodes at the address the entry picks by: only the ECU's author can tell them apart.
    const std::string indistinct =
        "E 14 j1939-node-indistinct: participant 'Gateway': j1939_nodes (line 14) cannot pick a node: the executable's "
        "code puts 2 nodes at any (254) on 'J1939_CAN', and j1939_nodes tells nodes apart by that address, so this file "
        "cannot change them: remove the entry, and ask the ECU's author to give the nodes addresses of their own in its "
        "code, or to take them as arguments\n";
    check_text(findings({on_j1939("J1939_CAN", {any1, node(0x10, kEngineName), any2})}, "J1939_CAN", change(0xFE, 0x05)), indistinct);
    check_text(findings({on_j1939("J1939_CAN", {any1, any2})}, "J1939_CAN", change(std::nullopt, 0x05)), indistinct);
    // What a node cannot be given.
    check_text(findings({on_j1939("J1939_CAN", {node(0x00)})}, "J1939_CAN",
                        j1939::NodeOverride{std::nullopt, std::nullopt, 2, std::nullopt, 1}),
               "E 14 j1939-node-no-name: participant 'Gateway': j1939_nodes (line 14) sets identity_number and "
               "ecu_instance, but the node at 0x00 on 'J1939_CAN' has no NAME (it does not claim its address): remove "
               "identity_number and ecu_instance; for this node only address can be changed\n");
    check_text(findings({on_j1939("J1939_CAN", {node(0x00, kEngineName)})}, "J1939_CAN", change(std::nullopt, 0xFE)),
               "E 14 j1939-node-any-address: participant 'Gateway': j1939_nodes (line 14) sets address: 254 (any free "
               "address), but the NAME of the node at 0x00 on 'J1939_CAN' is not arbitrary address capable, which this "
               "file cannot change: write an address of 0..253\n");
    check_text(findings({on_j1939("J1939_CAN", {node(0x00)})}, "J1939_CAN", change(std::nullopt, 0xFE)),
               "E 14 j1939-node-any-address: participant 'Gateway': j1939_nodes (line 14) sets address: 254 (any free "
               "address), but the node at 0x00 on 'J1939_CAN' has no NAME, so it cannot claim a free address: write an "
               "address of 0..253\n");
}

TEST_CASE(one_mistake_in_j1939_nodes_is_one_finding)
{
    // A: Engine2's entry takes no node. Its nodes are not compared on that network, so there is no
    // name-conflict; the conflict of two other participants there is said as before.
    Setup a = two_engines({{"J1939_CAN", change(0x10, 0x01, 2)}});
    a.described(participant("Tool", 16), ecu("Tool", 10ms, {on_j1939("J1939_CAN", {node(0x28)})}));
    check_text(a.check(),
               "E 14 j1939-node-unknown: participant 'Engine2': j1939_nodes (line 14) changes the node that the "
               "executable's code puts at 0x10 on 'J1939_CAN', but the executable has no node at that address there (its "
               "nodes there: 0x00; nodes that it adds after it starts are not described and cannot be changed). from is "
               "the address in the executable's code (the one the table shows without j1939_nodes), not the new address: "
               "write from: 0x00\n"
               "E 3 address-conflict: network 'J1939_CAN': Dashboard and Tool both ask for the address 0x28 (40), and "
               "Dashboard and Tool have no NAME, so they do not take part in the address claim and keep sending from "
               "that address: give each node its own address; " + kNoEntries + "\n");
    // B: Engine2's executable does not take --j1939-node, the entry is right. The values it would
    // have are compared (no name-conflict), and nothing is noted as changed.
    Setup b = two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}});
    b.results[2].ecu->description.features = {"map-network"};
    check_text(b.check(),
               "E 14 j1939-node-unsupported: participant 'Engine2': j1939_nodes needs an executable that takes "
               "--j1939-node, and this one does not say that it does (no \"j1939-node\" in the features of its "
               "--describe; it was built with vehicle-sil 1.0.0): have its author build it again with vehicle-sil 1.0.0 "
               "or later; until then its addresses and NAMEs change only in its code\n");
    // C: a wrong line of network_names keeps the network that the entry names from Engine2: the
    // line's error says it, not the entry.
    Setup c = two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}});
    c.config.participants[2].network_names = {NetworkName{"J1939_CAN", "ENGINE_CAM", 13}};
    c.results[2].ecu->description.networks[0].name = "ENGINE_CAN";
    check_text(c.check(),
               "E 13 network-map-unknown: participant 'Engine2': network_names says that 'J1939_CAN' is the executable's "
               "network 'ENGINE_CAM', but the executable has no network of that name (its networks: 'ENGINE_CAN' "
               "(j1939)): correct the value (did you mean 'ENGINE_CAN'?), or remove the line\n");
    // D: no line of network_names at all, and the executable's only J1939 network has a name that is
    // not in networks: unknown-network says to add the line, the entry does not say it again.
    Setup d = two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}});
    d.results[2].ecu->description.networks[0].name = "ENGINE_CAN";
    const std::string missing = d.check();
    CHECK(only(missing, "j1939-node").empty());
    CHECK(only(missing, "E ").find("E 12 unknown-network: participant 'Engine2': the executable uses the network "
                                   "'ENGINE_CAN' (j1939), which is not in networks") == 0);
    CHECK(only(missing, "E ").find("add network_names: { J1939_CAN: ENGINE_CAN } to the participant 'Engine2'") !=
          std::string::npos);
    CHECK(only(missing, "E ").find('\n') == only(missing, "E ").size() - 1);
    // Written as it says, the entry takes its node.
    d.config.participants[2].network_names = {NetworkName{"J1939_CAN", "ENGINE_CAN", 13}};
    check_text(only(d.check(), "E "), "");
    CHECK(!only(d.check(), "N 14 j1939-node-changed").empty());
}

TEST_CASE(j1939_nodes_need_an_executable_that_takes_them)
{
    Setup s = two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}});
    s.results[2].ecu->description.features = {};
    s.results[2].ecu->vehicle_sil_version.clear();
    check_text(only(s.check(), "unsupported"),
               "E 14 j1939-node-unsupported: participant 'Engine2': j1939_nodes needs an executable that takes "
               "--j1939-node, and this one does not say that it does (no \"j1939-node\" in the features of its "
               "--describe): have its author build it again with vehicle-sil 1.0.0 or later; until then its addresses "
               "and NAMEs change only in its code\n");
    // An entry that changes nothing is not given to the executable: nothing to take.
    s.config.participants[2].j1939_nodes = {J1939NodeEntry{{"J1939_CAN", change(0x00, std::nullopt)}, 14}};
    CHECK(only(s.check(), "j1939-node").empty());
    // A setting added later has its own word: an executable that takes j1939-node but not it.
    const J1939NodeSettingFeature table[] = {{"from", "j1939-node"}, {"address", "j1939-node"}, {"identity_number", "j1939-node.identity_number"}};
    ParticipantConfig p = with_nodes(participant("Engine2", 12), 14, {{"J1939_CAN", change(0x00, 0x01)}});
    const ReportedEcu takes = taking_nodes(ecu("Engine2", 10ms, {}));
    CHECK(!j1939_nodes_unsupported(p, takes, "1.0.0", table));
    p.j1939_nodes.push_back(J1939NodeEntry{{"J1939_CAN", change(0x10, 0x11, 2)}, 15});
    p.j1939_nodes.push_back(J1939NodeEntry{{"J1939_CAN", change(0x20, 0x21, 3)}, 16});
    const auto later = j1939_nodes_unsupported(p, takes, "1.0.0", table);
    CHECK(later.has_value());
    check_text(later ? lines({*later}) : std::string{},
               "E 15 j1939-node-unsupported: participant 'Engine2': j1939_nodes (line 15) sets identity_number, which "
               "needs \"j1939-node.identity_number\" in the features of the executable's --describe, and this one does "
               "not have it: have its author build it again with vehicle-sil 1.0.0 or later, or leave identity_number "
               "out\n");
    // Without j1939-node at all, that is what is said.
    CHECK(j1939_nodes_unsupported(p, ecu("Engine2", 10ms, {}), "1.0.0", table)->message.find("needs an executable that "
                                                                                              "takes --j1939-node") !=
          std::string::npos);
}

TEST_CASE(the_address_and_name_rules_say_how_to_fix_it_in_this_file)
{
    // A participant with two nodes there: the entry needs from, the address in the code.
    Setup two = two_engines();
    two.results[2].ecu->description.networks[0].nodes.push_back(node(0x30, kEngineName + 7));
    check_text(two.check(),
               "E 3 name-conflict: network 'J1939_CAN': Engine and Engine2 (node 1) have the same J1939 NAME "
               "0x00000000FE000001, and they ask for the same address 0x00 (0), which none of them keeps (an equal NAME "
               "loses the address claim): each node needs its own NAME; to do it in this file, write under participant "
               "'Engine2': j1939_nodes: [{ network: J1939_CAN, from: 0x00, address: 0x01, identity_number: 2 }]\n");
    // A participant with an entry for that network (for another node): from too.
    Setup other = two_engines({{"J1939_CAN", change(0x30, 0x31)}});
    other.results[2].ecu->description.networks[0].nodes.push_back(node(0x30, kEngineName + 7));
    CHECK(only(other.check(), "E 3 name-conflict").find("write under participant 'Engine2': j1939_nodes: [{ network: "
                                                         "J1939_CAN, from: 0x00, address: 0x01, identity_number: 2 }]\n") !=
          std::string::npos);
    // Two executables of different authors that both kept the default NAME: the same fix, and no
    // talk of a second copy. The free values: neither address nor NAME of any node there.
    Setup authors = good_pair();
    authors.results[0].ecu = taking_nodes(*authors.results[0].ecu);
    authors.described(participant("Brake", 12), taking_nodes(ecu("Brake", 10ms, {on_j1939("J1939_CAN", {node(0x0B, kEngineName)})})));
    authors.described(participant("Lamp", 14), ecu("Lamp", 10ms, {on_j1939("J1939_CAN", {node(0x01, kEngineName + 1), node(0x02, kEngineName + 2)})}));
    check_text(authors.check(),
               "E 3 name-conflict: network 'J1939_CAN': Engine and Brake have the same J1939 NAME 0x00000000FE000001: "
               "each node needs its own NAME; to do it in this file, write under participant 'Brake': j1939_nodes: [{ "
               "network: J1939_CAN, identity_number: 4 }]\n");
    // The same address: the lowest that no node asks for.
    authors.results[2].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 9)};
    authors.results[0].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 8)};
    check_text(authors.check(),
               "W 3 address-shared: network 'J1939_CAN': Engine and Brake both ask for the address 0x00 (0); Engine "
               "keeps it (its NAME is the lowest) and Brake will claim another address: give each its own address to "
               "know where they are; to do it in this file, write under participant 'Brake': j1939_nodes: [{ network: "
               "J1939_CAN, address: 0x03 }]\n");
    // Only the executables that take j1939_nodes can be the example; the last of them in the file.
    authors.results[2].ecu->description.features.clear();
    CHECK(authors.check().find("write under participant 'Engine': j1939_nodes: [{ network: J1939_CAN, address: 0x03 }]") !=
          std::string::npos);
}

TEST_CASE(the_fixes_of_the_address_and_name_rules_can_all_be_written_at_once)
{
    // Two copies each of two ECUs (Engine: NAME identity number 1 at 0x00; Brake: identity number 2
    // at 0x0B). The two fixes suggest different addresses and identity numbers: Engine2 cannot take
    // identity number 2 (Brake's NAME), Brake2 neither 0x01 nor 3 (the first fix's).
    Setup four = two_engines();
    four.described(participant("Brake", 16), taking_nodes(ecu("Brake", 10ms, {on_j1939("J1939_CAN", {node(0x0B, kEngineName + 1)})})));
    four.described(participant("Brake2", 18), taking_nodes(ecu("Brake2", 10ms, {on_j1939("J1939_CAN", {node(0x0B, kEngineName + 1)})})));
    const std::string before = four.check();
    CHECK(before.find("write under participant 'Engine2': j1939_nodes: [{ network: J1939_CAN, address: 0x01, "
                      "identity_number: 3 }]") != std::string::npos);
    CHECK(before.find("write under participant 'Brake2': j1939_nodes: [{ network: J1939_CAN, address: 0x02, "
                      "identity_number: 4 }]") != std::string::npos);
    CHECK(write_fixes(four) == 2);
    check_text(only(four.check(), "E ") + only(four.check(), "W "), "");

    // Two copies of an ECU with two nodes: one finding for each node, each with its own address.
    Setup pairs = two_engines();
    pairs.results[0].ecu->description.networks[0].nodes.push_back(node(0x21, kEngineName + 7));
    pairs.results[2].ecu->description.networks[0].nodes.push_back(node(0x21, kEngineName + 7));
    CHECK(write_fixes(pairs) == 2);
    check_text(only(pairs.check(), "E ") + only(pairs.check(), "W "), "");
    check_text(nodes_of(pairs.config.participants[2]),
               "J1939_CAN=from:0x00,address:0x01,identity_number:2@100 J1939_CAN=from:0x21,address:0x02,identity_number:9@101 ");

    // An entry that changes nothing for the node: the fix changes it (a second entry for the node
    // would be j1939-node-duplicate). With from, without from, and without from where the
    // participant has two nodes (the fix adds from).
    for (const auto& kept : {change(0x00, std::nullopt), change(std::nullopt, std::nullopt)})
    {
        Setup idle = two_engines({{"J1939_CAN", kept}});
        check_text(only(idle.check(), "E "),
                   "E 3 name-conflict: network 'J1939_CAN': Engine and Engine2 have the same J1939 NAME "
                   "0x00000000FE000001, and they ask for the same address 0x00 (0), which none of them keeps (an equal "
                   "NAME loses the address claim): each node needs its own NAME; to do it in this file, set address: "
                   "0x01, identity_number: 2 in the entry of j1939_nodes on line 14 (participant 'Engine2'), keeping its "
                   "other settings\n");
        CHECK(write_fixes(idle) == 1);
        check_text(only(idle.check(), "E "), "");
        CHECK(idle.config.participants[2].j1939_nodes.size() == 1);
    }
    Setup bare = two_engines({{"J1939_CAN", change(std::nullopt, std::nullopt)}});
    bare.results[2].ecu->description.networks[0].nodes.push_back(node(0x30, kEngineName + 7));
    CHECK(only(bare.check(), "E ").find("set from: 0x00, address: 0x01, identity_number: 2 in the entry of j1939_nodes on "
                                        "line 14 (participant 'Engine2')") != std::string::npos);
    CHECK(write_fixes(bare) == 1);
    check_text(only(bare.check(), "E "), "");

    // The address of an entry whose participant is not counted (describe: false) is not suggested.
    Setup door = two_engines();
    door.unchecked("Door", 16);
    door.config.participants[3].j1939_nodes = {J1939NodeEntry{{"J1939_CAN", change(std::nullopt, 0x01)}, 18}};
    CHECK(door.check().find("j1939_nodes: [{ network: J1939_CAN, address: 0x02, identity_number: 2 }]") != std::string::npos);
}

TEST_CASE(a_node_that_shares_its_address_in_the_code_is_not_the_example_of_a_fix)
{
    // Engine2's code puts two nodes at 0x00: from cannot pick one, so the fix is for Engine.
    Setup s = two_engines();
    s.results[2].ecu->description.networks[0].nodes.push_back(node(0x00, kEngineName + 7));
    CHECK(only(s.check(), "name-conflict").find("write under participant 'Engine': j1939_nodes: [{ network: J1939_CAN, "
                                                "address: 0x01, identity_number: 2 }]") != std::string::npos);
    // Engine does not take j1939_nodes: this file cannot change them.
    s.results[0].ecu->description.features.clear();
    CHECK(only(s.check(), "name-conflict").find(
              "each node needs its own NAME; this file cannot change them, as j1939_nodes tells a participant's nodes "
              "apart by their addresses in the code, and the code of each executable here that takes j1939_nodes puts "
              "another of its nodes at the same address: have the author of one of them change it in its code\n") !=
          std::string::npos);
}

TEST_CASE(a_participant_whose_nodes_change_gets_one_note)
{
    // Engine2 at 0x10 with a NAME of its own; Dashboard at 0x28 (two_engines() without the conflict).
    const auto notes = [](const std::vector<std::pair<std::string, j1939::NodeOverride>>& entries,
                          std::vector<NetworkDescription> networks = {}) {
        Setup s = two_engines(entries);
        s.results[2].ecu->description.networks[0].nodes = {node(0x10, kEngineName + 5)};
        for (NetworkDescription& n : networks)
        {
            s.results[2].ecu->description.networks.push_back(std::move(n));
        }
        return s;
    };
    const std::string only = ". Only Engine2 changes, and the check cannot see who relies on the old values: ";
    const std::string end = "; if messages do not reach Engine2 or are taken for another ECU's, the author of that code "
                            "has to change it\n";
    const std::string prefix = "N 14 j1939-node-changed: participant 'Engine2': j1939_nodes changes its nodes: on ";
    // The address: what other code may still use. The NAME: who looks it up by the whole NAME.
    check_text(notes({{"J1939_CAN", change(std::nullopt, 0x11)}}).check(),
               prefix + "'J1939_CAN', the node at 0x10 in its code is at 0x11" + only + "an ECU whose code has 0x10 written "
               "in it for Engine2 (as a destination, or to tell senders apart) still uses 0x10, and Engine2's own code "
               "may still have 0x10 in a filter or in the data it sends" + end);
    check_text(notes({{"J1939_CAN", j1939::NodeOverride{std::nullopt, std::nullopt, 7, 1, std::nullopt}}}).check(),
               prefix + "'J1939_CAN', the node at 0x10 has the NAME 0x00000008FE000007 (0x00000000FE000006 in its code)" +
                   only + "an ECU that looks Engine2 up by its whole NAME 0x00000000FE000006 does not find it" + end);
    // A node that takes any free address in its code: no other code can have its address.
    Setup any = notes({{"J1939_CAN", change(0xFE, 0x30)}});
    any.results[2].ecu->description.networks[0].nodes = {node(j1939::kNullAddress, kArbitraryBit | 5)};
    check_text(any.check(), prefix + "'J1939_CAN', the node that takes any free address in its code is at 0x30\n");
    // Two networks, in one note.
    Setup both = notes({{"J1939_CAN", change(std::nullopt, 0x11)}, {"Other", change(std::nullopt, 0x21)}},
                       {on_j1939("Other", {node(0x20)})});
    both.network("Other", NetworkKind::Can, 4);
    both.described(participant("Tool", 16), ecu("Tool", 10ms, {on_j1939("Other", {node(0x40)})}));
    const std::vector<Finding> found = check_participants(both.config, both.results, "1.0.0");
    check_text(lines(found),
               prefix + "'J1939_CAN', the node at 0x10 in its code is at 0x11; on 'Other', the node at 0x20 in its code "
               "is at 0x21" + only + "an ECU whose code has 0x10 or 0x20 written in it for Engine2 (as a destination, or "
               "to tell senders apart) still uses 0x10 or 0x20, and Engine2's own code may still have 0x10 or 0x20 in a "
               "filter or in the data it sends" + end);
    CHECK(found[0].network.empty() && found[0].participant == "Engine2");
    // Values that are the code's: nothing changes, nothing is noted.
    check_text(notes({{"J1939_CAN", change(0x10, 0x10, 6)}}).check(), "");
    // Left out of the simulation: not noted.
    Setup disabled = notes({{"J1939_CAN", change(std::nullopt, 0x11)}});
    disabled.config.participants[2].enabled = false;
    CHECK(disabled.check().find("j1939-node") == std::string::npos);
}

TEST_CASE(the_reports_show_both_addresses_of_a_changed_node)
{
    Setup s = two_engines({{"J1939_CAN", change(std::nullopt, 0x01, 2)}});
    s.network("Other", NetworkKind::Can, 4);
    s.config.participants[2].network_names = {NetworkName{"J1939_CAN", "ENGINE_CAN", 13}};
    s.results[2].ecu->description.networks = {on_j1939("ENGINE_CAN", {node(0x00, kEngineName), node(j1939::kNullAddress, kArbitraryBit | 4)}),
                                              on_j1939("Other", {node(0x20, kArbitraryBit | 3)})};
    s.config.participants[2].j1939_nodes = {
        J1939NodeEntry{{"J1939_CAN", change(0x00, 0x01, 2)}, 14},
        J1939NodeEntry{{"J1939_CAN", change(0xFE, 0x30)}, 15},
        J1939NodeEntry{{"Other", change(0x20, 0xFE)}, 16},
        J1939NodeEntry{{"Other", change(0x77, 0x01)}, 17},
        J1939NodeEntry{{"Other", change(0x21, std::nullopt)}, 18},
    };
    CheckReport r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    // For people: the value in the simulation, then the one in the code, no space around the arrow.
    const std::string text = format_text(r);
    CHECK(text.find("Engine2      10 ms  J1939_CAN <- ENGINE_CAN (j1939 0x01<-0x00 0x30<-any), Other (j1939 any<-0x20)\n") !=
          std::string::npos);
    // For programs: the entries as written, and the node each takes (null for none, and for one
    // that changes nothing); "reported" is what the executable said.
    const std::string json = format_json(r);
    CHECK(json.find("      \"j1939_nodes\": [\n"
                    "        {\n"
                    "          \"network\": \"J1939_CAN\",\n"
                    "          \"line\": 14,\n"
                    "          \"from\": 0,\n"
                    "          \"address\": 1,\n"
                    "          \"identity_number\": 2,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": {\n"
                    "            \"coded_address\": 0,\n"
                    "            \"address\": 1,\n"
                    "            \"j1939_name\": \"0x00000000FE000002\",\n"
                    "            \"arbitrary_address_capable\": false\n"
                    "          }\n"
                    "        },\n"
                    "        {\n"
                    "          \"network\": \"J1939_CAN\",\n"
                    "          \"line\": 15,\n"
                    "          \"from\": 254,\n"
                    "          \"address\": 48,\n"
                    "          \"identity_number\": null,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": {\n"
                    "            \"coded_address\": 254,\n"
                    "            \"address\": 48,\n"
                    "            \"j1939_name\": \"0x8000000000000004\",\n"
                    "            \"arbitrary_address_capable\": true\n"
                    "          }\n"
                    "        },\n") != std::string::npos);
    CHECK(json.find("          \"network\": \"Other\",\n"
                    "          \"line\": 18,\n"
                    "          \"from\": 33,\n"
                    "          \"address\": null,\n"
                    "          \"identity_number\": null,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": null\n") != std::string::npos);
    CHECK(json.find("          \"line\": 17,\n"
                    "          \"from\": 119,\n"
                    "          \"address\": 1,\n"
                    "          \"identity_number\": null,\n"
                    "          \"function_instance\": null,\n"
                    "          \"ecu_instance\": null,\n"
                    "          \"node\": null\n") != std::string::npos);
    CHECK(json.find("            \"name\": \"ENGINE_CAN\",\n"
                    "            \"kind\": \"can\",\n"
                    "            \"use\": \"j1939\",\n"
                    "            \"nodes\": [\n"
                    "              {\n"
                    "                \"address\": 0,\n"
                    "                \"j1939_name\": \"0x00000000FE000001\",\n") != std::string::npos);
    // Only the entry that has j1939_nodes has entries; the others have null.
    std::size_t count = 0;
    for (auto at = json.find("\"j1939_nodes\": ["); at != std::string::npos; at = json.find("\"j1939_nodes\": [", at + 1))
    {
        ++count;
    }
    CHECK_EQ(count, std::size_t{1});
    CHECK(format_json(mixed_report()).find("\"j1939_nodes\": [") == std::string::npos);
}

TEST_CASE(a_configuration_without_j1939_nodes_checks_as_before)
{
    // The tables and the JSON of a configuration without j1939_nodes have nothing of it (null in
    // the JSON); the three rules of the addresses and NAMEs end with how to fix it (here: in the
    // ECUs' code).
    const CheckReport r = mixed_report();
    CHECK(format_text(r).find("<-") == std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("\"j1939_nodes\": [") == std::string::npos && json.find("\"j1939_nodes\": null,\n") != std::string::npos);
    for (const ParticipantConfig& p : r.config->participants)
    {
        CHECK(j1939_node_arguments(p).empty());
    }
    Setup s = good_pair();
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kEngineName)};
    CHECK(s.check().find("each node needs its own NAME; this file cannot change them") != std::string::npos);
}

// --- --connect ------------------------------------------------------------------------------

namespace {

NetworkDescription on_ethernet(const std::string& name, std::optional<eth::MacAddress> mac)
{
    return NetworkDescription{name, NetworkUse::Ethernet, {}, mac};
}

eth::MacAddress mac_of(std::uint8_t last) { return eth::MacAddress{{0x02, 0, 0, 0, 0, last}}; }

/// An Ethernet network ETH (line 3) with Camera (MAC ...:01), a CAN network CAN1 (line 5) with
/// Engine, and a LIN network BODY (line 7).
Setup connect_setup()
{
    Setup s;
    s.network("ETH", NetworkKind::Ethernet, 3).network("CAN1", NetworkKind::Can, 5).network("BODY", NetworkKind::Lin, 7);
    s.described(participant("Camera", 10), ecu("Camera", 10ms, {on_ethernet("ETH", mac_of(1))}));
    s.described(participant("Engine", 13), ecu("Engine", 10ms, {NetworkDescription{"CAN1", NetworkUse::Raw, {}}}));
    s.config.speed = 1.0;
    return s;
}

/// What a bridge of this repository writes for --describe (bridges/common/src/describe.cpp).
std::string bridge_description(const std::string& name, const std::string& network, const std::string& kind)
{
    return R"({"vehicle_sil_ecu": 1, "vehicle_sil": "1.0.0", "name": ")" + name + R"(", "step_ns": null, "networks": [)" +
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
    ConnectTools tools()
    {
        ConnectTools t;
        t.launcher_dir = "/opt/vsil/bin";
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
            const bool can = tool.filename() == "vehicle_sil_vector_xl_bridge";
            run.exit_code = 0;
            run.standard_output = bridge_description(arguments.back(), network, can ? "can" : "ethernet");
            return run;
        };
        return t;
    }
};

std::string connect_lines(const Setup& s, const std::vector<ConnectRequest>& requests, FakeTools& fake, bool with_tools = true)
{
    const ConnectTools tools = fake.tools();
    return lines(check_connections(s.config, s.results, requests, with_tools ? &tools : nullptr).findings);
}

std::string outside_text(const ConnectCheck& check)
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

} // namespace

TEST_CASE(check_connections_finds_the_bridges_and_asks_them_describe)
{
    const Setup s = connect_setup();
    FakeTools fake;
    const ConnectTools tools = fake.tools();
    const ConnectCheck check =
        check_connections(s.config, s.results, {{"CAN1", "physical:2"}, {"ETH", "vEthernet (A)"}}, &tools);
    check_text(lines(check.findings), "");
    // In the file's order, whatever the order of the requests.
    check_text(outside_text(check),
               "ETH vehicle_sil_ethernet_bridge [/tools/vehicle_sil_ethernet_bridge] vEthernet (A) bridge.ETH: --network "
               "ETH --interface vEthernet (A) --src-mac 02:00:00:00:00:01\n"
               "CAN1 vehicle_sil_vector_xl_bridge [/tools/vehicle_sil_vector_xl_bridge] physical:2 bridge.CAN1: --network "
               "CAN1 --xl-target physical --xl-channel 2\n");
    check_text(fake.runs.at(0) + "\n" + fake.runs.at(1) + "\n",
               "/tools/vehicle_sil_ethernet_bridge --network ETH --interface vEthernet (A) --src-mac 02:00:00:00:00:01 "
               "--describe --name bridge.ETH\n"
               "/tools/vehicle_sil_vector_xl_bridge --network CAN1 --xl-target physical --xl-channel 2 --describe --name "
               "bridge.CAN1\n");
}

TEST_CASE(check_connections_without_a_target_connects_nothing)
{
    Setup s = connect_setup();
    s.config.speed.reset();
    FakeTools fake;
    const ConnectTools tools = fake.tools();
    const ConnectCheck check = check_connections(s.config, s.results, {{"ETH", "none"}, {"CAN1", "none"}, {"BODY", "none"}}, &tools);
    check_text(lines(check.findings), "");
    CHECK(check.outside.empty() && fake.runs.empty());
}

TEST_CASE(check_connections_names_what_is_wrong_with_a_request)
{
    Setup s = connect_setup();
    s.config.networks.push_back(NetworkConfig{"A=B", NetworkKind::Can, 9, true});
    // Disabled, but a --enable would take it in: the name is taken.
    s.config.participants.push_back(participant("bridge.CAN1", 16, std::nullopt, false));
    s.config.participants.back().enabled = false;
    s.results.push_back(ParticipantResult{ParticipantStatus::Disabled, std::nullopt});
    FakeTools fake;
    check_text(connect_lines(s, {{"ETX", "eth0"}, {"A", "B=physical:1"}, {"BODY", "x"}, {"CAN1", "usb:1"}}, fake),
               "E 0 connect-network: --connect names the network 'ETX', which the file does not have: did you mean 'ETH'? "
               "(the networks: ETH, CAN1, BODY, A=B)\n"
               "E 0 connect-network: --connect names the network 'A', which the file does not have: the network 'A=B' has "
               "'=' in its name, and --connect takes the network's name up to the first '=': rename the network (a name "
               "without '=')\n"
               "E 5 connect-target: network 'CAN1': --connect CAN1=usb:1: a CAN target is virtual:<n> or physical:<n>, "
               "with the channel number <n> from 1: give a channel as virtual:<n> or physical:<n> (vehicle_sil_launch "
               "connections <file> lists those of this computer), or none for SIL Kit only\n"
               "E 16 connect-name-taken: participant 'bridge.CAN1' has the name that --connect CAN1 gives the bridge of "
               "the network: rename the participant\n"
               "E 7 connect-kind: network 'BODY': it is lin: there is no bridge for LIN, so it runs on SIL Kit only: leave "
               "--connect BODY out (or give BODY=none)\n");
    // No tool is run for a network with an error.
    CHECK(fake.runs.empty());
    check_text(connect_lines(s, {{"ETH", "eth0\t\xFF"}}, fake),
               "E 3 connect-target: network 'ETH': --connect ETH=eth0\\x09\\xFF: the target is not UTF-8 text: give an "
               "adapter that vehicle_sil_launch connections <file> lists, or none for SIL Kit only\n");
    Setup empty;
    check_text(connect_lines(empty, {{"ETH", "eth0"}}, fake),
               "E 0 connect-network: --connect names the network 'ETH', which the file does not have: the file has no "
               "networks\n");
}

TEST_CASE(check_connections_needs_a_filter_for_every_participant_on_an_ethernet_network)
{
    FakeTools fake;
    Setup s = connect_setup();
    s.described(participant("Display", 16), ecu("Display", 10ms, {on_ethernet("ETH", std::nullopt)}));
    check_text(connect_lines(s, {{"ETH", "eth0"}}, fake),
               "E 3 connect-filter: network 'ETH': without external.ethertypes or external.all_frames, the bridge of "
               "--connect passes only the frames of the MAC addresses that the ECUs on the network report (not those of "
               "participants with role: test), and "
               "Display reports no MAC address on it: write external: { ethertypes: [0x88b5] } (the EtherTypes to pass, "
               "also for participants with role: test) or external: { all_frames: true } for the network, or give each "
               "ECU on it a MAC address (eth::ChannelOptions::mac)\n");
    // describe: false: its network is not known.
    Setup unknown = connect_setup();
    unknown.unchecked("Logger", 16);
    check_text(connect_lines(unknown, {{"ETH", "eth0"}}, fake),
               "E 3 connect-filter: network 'ETH': without external.ethertypes or external.all_frames, the bridge of "
               "--connect passes only the frames of the MAC addresses that the ECUs on the network report (not those of "
               "participants with role: test), and "
               "Logger has describe: false, so the check cannot see whether it is on it and with which address: write "
               "external: { ethertypes: [0x88b5] } (the EtherTypes to pass, also for participants with role: test) or "
               "external: { all_frames: true } for the network\n");
    // One with role: bridge (connect-loop is about it) or role: test does not count.
    unknown.config.participants.back().role = ParticipantRole::Bridge;
    check_text(connect_lines(unknown, {{"ETH", "eth0"}}, fake), "");
    unknown.config.participants.back().role = ParticipantRole::Test;
    check_text(connect_lines(unknown, {{"ETH", "eth0"}}, fake), "");
    // No ECU reports one.
    Setup alone = connect_setup();
    alone.results[0].ecu->description.networks = {NetworkDescription{"CAN1", NetworkUse::Raw, {}}};
    CHECK(connect_lines(alone, {{"ETH", "eth0"}}, fake).find("and no ECU on it reports a MAC address: write") !=
          std::string::npos);
    // Only the ECUs' frames go out: a participant for testing with a MAC address is left out of
    // --src-mac, and one that is alone on the network is no ECU.
    Setup tested = connect_setup();
    tested.described(with_role(participant("Stim", 16), ParticipantRole::Test),
                     ecu("Stim", 10ms, {on_ethernet("ETH", mac_of(0x99))}));
    tested.described(with_role(participant("Spy", 19), ParticipantRole::Test), ecu("Spy", 10ms, {on_ethernet("ETH", std::nullopt)}));
    {
        const ConnectTools tools = fake.tools();
        const ConnectCheck check = check_connections(tested.config, tested.results, {{"ETH", "eth0"}}, &tools);
        check_text(lines(check.findings), "");
        check_text(outside_text(check), "ETH vehicle_sil_ethernet_bridge [/tools/vehicle_sil_ethernet_bridge] eth0 "
                                        "bridge.ETH: --network ETH --interface eth0 --src-mac 02:00:00:00:00:01\n");
    }
    tested.results[0].ecu->description.networks.clear();
    check_text(connect_lines(tested, {{"ETH", "eth0"}}, fake),
               "E 3 connect-filter: network 'ETH': without external.ethertypes or external.all_frames, the bridge of "
               "--connect passes only the frames of the MAC addresses that the ECUs on the network report (not those of "
               "participants with role: test), and no ECU on it reports a MAC address (only participants with role: test "
               "do): write external: { ethertypes: [0x88b5] } (the EtherTypes to pass, also for participants with role: "
               "test) or external: { all_frames: true } for the network\n");
    // A participant without a description: its error comes first, and with the frames that pass
    // not known, the bridge is not asked (it would refuse a command without a filter).
    s.results.back() = ParticipantResult{ParticipantStatus::Failed, std::nullopt};
    fake.runs.clear();
    {
        const ConnectTools tools = fake.tools();
        const ConnectCheck check = check_connections(s.config, s.results, {{"ETH", "eth0"}}, &tools);
        check_text(lines(check.findings), "");
        CHECK(fake.runs.empty());
        check_text(outside_text(check),
                   "ETH vehicle_sil_ethernet_bridge [/tools/vehicle_sil_ethernet_bridge] eth0 bridge.ETH: (no command)\n");
    }
    // With ethertypes or all_frames, the participants' addresses do not matter.
    Setup typed = connect_setup();
    typed.described(participant("Display", 16), ecu("Display", 10ms, {on_ethernet("ETH", std::nullopt)}));
    typed.config.networks[0].external = NetworkExternal{2, std::nullopt, {0x88b5}, false};
    const ConnectTools tools = fake.tools();
    const ConnectCheck check = check_connections(typed.config, typed.results, {{"ETH", "eth0"}}, &tools);
    check_text(lines(check.findings), "");
    check_text(outside_text(check), "ETH vehicle_sil_ethernet_bridge [/tools/vehicle_sil_ethernet_bridge] eth0 bridge.ETH: "
                                    "--network ETH --interface eth0 --ethertype 0x88b5\n");
}

TEST_CASE(check_connections_says_when_a_bridge_is_missing_or_fails)
{
    Setup s = connect_setup();
    FakeTools fake;
    fake.missing = {"vehicle_sil_vector_xl_bridge"};
    check_text(connect_lines(s, {{"CAN1", "virtual:1"}}, fake),
               "E 5 connect-tool-missing: network 'CAN1': --connect needs vehicle_sil_vector_xl_bridge, which is neither "
               "next to vehicle_sil_launch (/opt/vsil/bin) nor on PATH: build it with the Vector XL Driver Library "
               "(docs/bridges.md) and put it next to vehicle_sil_launch, or its directory on PATH\n");
    fake.missing.clear();
    DescribeRun refused;
    refused.command_text = "vehicle_sil_vector_xl_bridge --network CAN1 ...";
    refused.exit_code = 2;
    refused.standard_error = "error: --bitrate must be between 15000 and 1000000 bit/s\r\nRun 'vehicle_sil_vector_xl_bridge "
                             "--help' for the options.\n\n";
    fake.answer = refused;
    check_text(connect_lines(s, {{"CAN1", "virtual:1"}}, fake),
               "E 5 connect-tool-failed: network 'CAN1': vehicle_sil_vector_xl_bridge --network CAN1 ... ended with exit "
               "code 2: error: --bitrate must be between 15000 and 1000000 bit/s: the bridge does "
               "not take what --connect gives it; correct the target or external of the network as the line says, or use "
               "the bridge built with the vehicle-sil of vehicle_sil_launch\n");
    // Without an "error:" line: the last line.
    refused.standard_error = "starting\nboom\n";
    fake.answer = refused;
    CHECK(connect_lines(s, {{"CAN1", "virtual:1"}}, fake).find("ended with exit code 2: boom: the bridge") != std::string::npos);
    DescribeRun slow;
    slow.command_text = "x";
    slow.timeout = 10000ms;
    fake.answer = slow;
    check_text(connect_lines(s, {{"CAN1", "virtual:1"}}, fake),
               "E 5 connect-tool-failed: network 'CAN1': x did not end within 10000 ms: the bridge answers --describe at "
               "once; use the bridge built with the vehicle-sil of vehicle_sil_launch\n");
    DescribeRun other;
    other.command_text = "x";
    other.exit_code = 0;
    other.standard_output = bridge_description("VectorXlBridge", "J1939_CAN1", "can");
    fake.answer = other;
    check_text(connect_lines(s, {{"CAN1", "virtual:1"}}, fake),
               "E 5 connect-tool-failed: network 'CAN1': x described itself as 'VectorXlBridge' on J1939_CAN1 (can), not "
               "as 'bridge.CAN1' on CAN1 (can): use the bridge built with the vehicle-sil of vehicle_sil_launch\n");
    other.standard_output = "hello";
    fake.answer = other;
    CHECK(connect_lines(s, {{"CAN1", "virtual:1"}}, fake).rfind("E 5 connect-tool-failed: network 'CAN1': x wrote no "
                                                               "description that vehicle_sil_launch reads (",
                                                               0) == 0);
}

TEST_CASE(check_connections_leaves_the_time_to_connect_time_findings)
{
    Setup s = connect_setup();
    s.config.speed.reset();
    FakeTools fake;
    check_text(connect_lines(s, {{"ETH", "eth0"}, {"CAN1", "virtual:1"}}, fake), "");
}

TEST_CASE(connect_time_findings_judge_the_speed_the_simulation_runs_at)
{
    const NetworkOutside eth{"ETH", "vehicle_sil_ethernet_bridge", {}, "eth0", "bridge.ETH", std::nullopt};
    const NetworkOutside can{"CAN1", "vehicle_sil_vector_xl_bridge", {}, "virtual:1", "bridge.CAN1", std::nullopt};
    const std::vector<NetworkOutside> both{eth, can};
    const std::vector<NetworkOutside> one{eth};
    using RunSpeed = std::optional<std::optional<double>>;
    // check, or run without --speed: the file's speed.
    check_text(lines(connect_time_findings(both, std::nullopt, RunSpeed{}, false)),
               "W 0 connect-not-real-time: --connect connects ETH and CAN1 to a real bus, which does not wait for the "
               "virtual time, and the file runs the simulation as fast as possible (no speed): the devices there get the "
               "messages at the wrong times. Run in real time: write speed: 1 in the file, or run with --speed 1 (--strict "
               "makes this a failure, for an automatic test)\n");
    check_text(lines(connect_time_findings(one, 2.0, RunSpeed{}, false)),
               "W 0 connect-not-real-time: --connect connects ETH to a real bus, which does not wait for the virtual time, "
               "and the file runs the simulation at speed 2: the devices there get the messages at the wrong times. Run in "
               "real time: write speed: 1 in the file, or run with --speed 1 (--strict makes this a failure, for an "
               "automatic test)\n");
    check_text(lines(connect_time_findings(one, 1.0, RunSpeed{}, false)), "");
    // run --speed: that speed, whatever the file has.
    check_text(lines(connect_time_findings(one, 1.0, RunSpeed{std::optional<double>(2.0)}, false)),
               "W 0 connect-not-real-time: --connect connects ETH to a real bus, which does not wait for the virtual time, "
               "and --speed 2 runs the simulation at that speed: the devices there get the messages at the wrong times. Run "
               "in real time: --speed 1 (--strict makes this a failure, for an automatic test)\n");
    check_text(lines(connect_time_findings(one, 1.0, RunSpeed{std::optional<double>{}}, false)),
               "W 0 connect-not-real-time: --connect connects ETH to a real bus, which does not wait for the virtual time, "
               "and --speed max runs the simulation as fast as possible: the devices there get the messages at the wrong "
               "times. Run in real time: --speed 1 (--strict makes this a failure, for an automatic test)\n");
    check_text(lines(connect_time_findings(one, std::nullopt, RunSpeed{std::optional<double>(1.0)}, false)), "");
    // --start-paused.
    check_text(lines(connect_time_findings(one, 1.0, RunSpeed{}, true)),
               "W 0 connect-start-paused: --connect connects ETH to a real bus, which does not stand still: with "
               "--start-paused, the devices there go on while the simulation waits for its first step. To stop the "
               "simulation and look at it, run it without --connect (on SIL Kit only)\n");
    // Nothing connected (no --connect, or only none): nothing to say.
    check_text(lines(connect_time_findings({}, std::nullopt, RunSpeed{std::optional<double>(2.0)}, true)), "");
}

TEST_CASE(check_connections_without_tools_reads_only_the_requests)
{
    Setup s = connect_setup();
    s.described(participant("Display", 16), ecu("Display", 10ms, {on_ethernet("ETH", std::nullopt)}));
    FakeTools fake;
    fake.missing = {"vehicle_sil_ethernet_bridge"};
    check_text(connect_lines(s, {{"ETH", "eth0"}, {"BODY", "x"}}, fake, false),
               "E 7 connect-kind: network 'BODY': it is lin: there is no bridge for LIN, so it runs on SIL Kit only: leave "
               "--connect BODY out (or give BODY=none)\n");
    CHECK(fake.runs.empty());
}

TEST_CASE(a_connected_network_has_its_bridge_on_it)
{
    Setup s = connect_setup();
    const std::vector<std::string> outside{"ETH", "CAN1"};
    // Camera alone on ETH and Engine alone on CAN1: the bridges are on them too.
    check_text(lines(check_participants(s.config, s.results, "1.0.0")),
               "W 3 single-participant: network 'ETH': only Camera is on it, so what it sends there reaches no one: add the "
               "other participants of this network, or remove it\n"
               "W 5 single-participant: network 'CAN1': only Engine is on it, so what it sends there reaches no one: add "
               "the other participants of this network, or remove it\n"
               "W 7 unused-network: network 'BODY': no participant uses it: remove it, or check the network names in the "
               "ECUs\n");
    check_text(lines(check_participants(s.config, s.results, "1.0.0", outside)),
               "W 7 unused-network: network 'BODY': no participant uses it: remove it, or check the network names in the "
               "ECUs\n");
    // A network that no participant is on stays unused.
    s.results[0].ecu->description.networks.clear();
    CHECK(lines(check_participants(s.config, s.results, "1.0.0", outside)).find("W 3 unused-network: network 'ETH'") !=
          std::string::npos);
}

TEST_CASE(a_test_participant_with_j1939_nodes_on_a_connected_network_is_a_warning)
{
    constexpr std::uint64_t kToolName = 0x80000000FE000080; // arbitrary address capable, Manufacturer Code 0x7F0
    Setup s = good_pair();
    s.described(with_role(participant("Tool", 12), ParticipantRole::Test),
                ecu("Tool", 10ms, {on_j1939("J1939_CAN", {node(0x80, kToolName)})}));
    check_text(s.check(), "");
    const std::vector<std::string> outside{"J1939_CAN"};
    check_text(lines(check_participants(s.config, s.results, "1.0.0", outside)),
               "W 12 test-participant-talks-on-bridged-network: participant 'Tool': it has role: test and J1939 nodes on "
               "network 'J1939_CAN', and --connect connects that network to a real bus: what it sends there goes out on "
               "the real bus too, its address claims among it, and a real ECU with the same address fights it for the "
               "address: let it only listen there (no J1939 nodes), or do not connect the network in runs with it (leave "
               "--connect J1939_CAN out)\n");
}

TEST_CASE(a_role_bridge_participant_on_a_connected_network_is_a_loop)
{
    Setup s = connect_setup();
    s.described(with_role(participant("EthBridge", 16), ParticipantRole::Bridge),
                ecu("EthBridge", std::nullopt, {on_ethernet("ETH", std::nullopt)}));
    const std::vector<std::string> outside{"ETH"};
    CHECK(lines(check_participants(s.config, s.results, "1.0.0", outside))
              .rfind("E 16 connect-loop: participant 'EthBridge': it has role: bridge and carries network 'ETH' to a real "
                     "bus, and --connect connects the network too: frames would go round between the two bridges: leave "
                     "one of the two bridges out (--disable EthBridge, or leave --connect ETH out)\n",
                     0) == 0);
    // On another network: no loop.
    const std::vector<std::string> can{"CAN1"};
    CHECK(lines(check_participants(s.config, s.results, "1.0.0", can)).find("connect-") == std::string::npos);
    // Disabled: not in the simulation.
    Setup off = s;
    off.config.participants.back().enabled = false;
    off.results.back() = ParticipantResult{ParticipantStatus::Disabled, std::nullopt};
    CHECK(lines(check_participants(off.config, off.results, "1.0.0", outside)).find("connect-") == std::string::npos);
    // describe: false: it may carry it.
    Setup maybe = connect_setup();
    maybe.unchecked("Gateway", 16);
    maybe.config.participants.back().role = ParticipantRole::Bridge;
    maybe.config.participants.back().role_written = true;
    check_text(lines(check_participants(maybe.config, maybe.results, "1.0.0", outside)),
               "N 16 not-checked: participant 'Gateway': not checked (describe: false): vehicle_sil_launch cannot compare "
               "its name, step and networks with this file\n"
               "W 16 connect-maybe-loop: participant 'Gateway': it has role: bridge and describe: false, so the check "
               "cannot see whether it carries network 'ETH', which --connect connects to a real bus: if it does, frames go "
               "round between the two bridges: leave one of the two bridges out (--disable Gateway, or leave --connect "
               "ETH out)\n"
               "N 5 single-participant: network 'CAN1': only Engine is on it, so what it sends there reaches no one "
               "(participants with describe: false are not counted): add the other participants of this network, or "
               "remove it\n"
               "N 7 unused-network: network 'BODY': no participant uses it (participants with describe: false are not "
               "counted): remove it, or check the network names in the ECUs\n");
}

TEST_CASE(the_outputs_show_where_a_network_is_connected)
{
    const Setup s = connect_setup();
    CheckReport r;
    r.file = "simulation.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string plainText = format_text(r);
    const std::string plainJson = format_json(r);
    // Without --connect: no column, and "outside": null on every network.
    CHECK(plainText.find("outside") == std::string::npos && plainJson.find("\"outside\": {") == std::string::npos &&
          plainJson.find("\"outside\": null") != std::string::npos);
    NetworkOutside eth{"ETH",
                       "vehicle_sil_ethernet_bridge",
                       "/opt/vsil/bin/vehicle_sil_ethernet_bridge",
                       "vEthernet (A)",
                       "bridge.ETH",
                       std::vector<std::string>{"--network", "ETH", "--interface", "vEthernet (A)", "--src-mac",
                                                "02:00:00:00:00:01"}};
    // Not found, and so no command (connect-tool-missing).
    NetworkOutside can{"CAN1", "vehicle_sil_vector_xl_bridge", {}, "physical:2", "bridge.CAN1", std::nullopt};
    r.outside = {eth, can};
    const std::string text = format_text(r);
    CHECK(text.find("network  kind      participants  outside\n"
                    "ETH      ethernet  Camera        vEthernet (A)\n"
                    "CAN1     can       Engine        physical:2\n"
                    "BODY     lin       -             -\n") != std::string::npos);
    const std::string json = format_json(r);
    CHECK(json.find("      \"participants\": [\n"
                    "        \"Camera\"\n"
                    "      ],\n"
                    "      \"outside\": {\n"
                    "        \"tool\": \"vehicle_sil_ethernet_bridge\",\n"
                    "        \"tool_path\": \"/opt/vsil/bin/vehicle_sil_ethernet_bridge\",\n"
                    "        \"target\": \"vEthernet (A)\",\n"
                    "        \"participant\": \"bridge.ETH\",\n"
                    "        \"command\": [\n"
                    "          \"vehicle_sil_ethernet_bridge\",\n"
                    "          \"--network\",\n"
                    "          \"ETH\",\n"
                    "          \"--interface\",\n"
                    "          \"vEthernet (A)\",\n"
                    "          \"--src-mac\",\n"
                    "          \"02:00:00:00:00:01\"\n"
                    "        ]\n"
                    "      }\n"
                    "    },\n") != std::string::npos);
    CHECK(json.find("        \"tool_path\": null,\n        \"target\": \"physical:2\",\n        \"participant\": "
                    "\"bridge.CAN1\",\n        \"command\": null\n      }\n") != std::string::npos);
    // A network that is not connected has "outside": null.
    CHECK(json.find("\"name\": \"BODY\",\n      \"kind\": \"lin\",\n      \"line\": 7,\n      \"participants\": [],\n"
                    "      \"outside\": null\n    }") != std::string::npos);
}

TEST_CASE(check_connections_says_when_windows_does_not_start_a_bridge_for_a_dll)
{
    // Windows ends an executable whose DLL is missing (0xC0000135) or of another version
    // (0xC0000139) before main(), with nothing on stderr; the exit codes are checked on every system.
    Setup s = connect_setup();
    FakeTools fake;
    DescribeRun missing;
    missing.command_text = "vehicle_sil_vector_xl_bridge ...";
    missing.exit_code = static_cast<int>(0xC0000135u);
    fake.answer = missing;
    check_text(connect_lines(s, {{"CAN1", "physical:1"}}, fake),
               "E 5 connect-tool-failed: network 'CAN1': vehicle_sil_vector_xl_bridge ... could not start (exit status "
               "0xC0000135): Windows did not find a DLL that it needs: vehicle_sil_vector_xl_bridge needs vxlapi64.dll of "
               "the Vector XL Driver Library: install the library, or put vxlapi64.dll next to the bridge; for another "
               "DLL, put that DLL, of the version the executable was built with, next to the executable or in a folder on "
               "PATH; to check without the real bus on this computer, leave --connect CAN1 out\n");
    DescribeRun old;
    old.command_text = "vehicle_sil_ethernet_bridge ...";
    old.exit_code = static_cast<int>(0xC0000139u);
    fake.answer = old;
    check_text(connect_lines(s, {{"ETH", "eth0"}}, fake),
               "E 3 connect-tool-failed: network 'ETH': vehicle_sil_ethernet_bridge ... could not start (exit status "
               "0xC0000139): a DLL that it needs is of another version (a function in it is missing): put that DLL, of "
               "the version the executable was built with, next to the executable or in a folder on PATH; to check "
               "without the real bus on this computer, leave --connect ETH out\n");
}

TEST_CASE(check_connections_gives_no_command_for_a_network_with_an_error)
{
    Setup s = connect_setup();
    FakeTools fake;
    const ConnectTools tools = fake.tools();
    const ConnectCheck check = check_connections(s.config, s.results, {{"CAN1", "usb:1"}, {"ETH", "eth0"}}, &tools);
    CHECK(lines(check.findings).rfind("E 5 connect-target: ", 0) == 0);
    check_text(outside_text(check),
               "ETH vehicle_sil_ethernet_bridge [/tools/vehicle_sil_ethernet_bridge] eth0 bridge.ETH: --network ETH "
               "--interface eth0 --src-mac 02:00:00:00:00:01\n"
               "CAN1 vehicle_sil_vector_xl_bridge [/tools/vehicle_sil_vector_xl_bridge] usb:1 bridge.CAN1: (no command)\n");
}
