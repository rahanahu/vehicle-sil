// SPDX-License-Identifier: MIT
//
// The check of vehicle_sil_launch without processes: reading --describe's JSON, what a run of
// --describe means, every rule of check_participants() with a case that passes and one that
// fails (the message whole), the order of the findings, and the output for people and as JSON.
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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
                      NetworkDescription{"Seat", NetworkUse::LinSlave, {}}},
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
    check_text(refused(head + R"([{"name": "N", "kind": "can", "use": "ethernet", "nodes": []}]})"),
               "networks[0].use is \"ethernet\", not raw, j1939, lin_master or lin_slave");
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
    check_text(error(head + R"("networks": [{"name": "Eth", "kind": "ethernet", "use": "ethernet", "nodes": []}]})"),
               start + "networks[0].use \"ethernet\", which this vehicle_sil_launch does not know (it knows raw, j1939, "
                       "lin_master and lin_slave): " + versions + tail);
    check_text(error(head + R"("networks": [{"name": "N", "kind": "flexray", "use": "raw", "nodes": []}]})"),
               start + "networks[0].kind \"flexray\", which this vehicle_sil_launch does not know (it knows can and "
                       "lin): " + versions + tail);
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
                       "lin_master and lin_slave): the executable does not say which vehicle-sil it was built with, and "
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
    check_text(s.check(),
               "E 6 unknown-network: participant 'Engine': the executable uses the network 'J1939CAN' (j1939), which is "
               "not in networks: add it to networks with kind: can, or correct the network's name in the ECU (did you "
               "mean 'J1939_CAN'?)\n"
               "E 6 kind-mismatch: participant 'Engine': the executable uses the network 'Body' as can (raw), but networks "
               "says kind: lin (line 5): correct the kind, or the network in the ECU\n"
               "E 6 unknown-network: participant 'Engine': the executable uses the network 'Diag' (raw), which is not in "
               "networks: add it to networks with kind: can, or correct the network's name in the ECU\n"
               "W 5 unused-network: network 'Body': no participant uses it: remove it, or check the network names in the "
               "ECUs\n");
    // A network whose kind could not be read is not checked (its own error says why).
    s.config.networks[1].valid = false;
    check_text(s.check(), "E 6 unknown-network: participant 'Engine': the executable uses the network 'J1939CAN' (j1939), "
                          "which is not in networks: add it to networks with kind: can, or correct the network's name in "
                          "the ECU (did you mean 'J1939_CAN'?)\n"
                          "E 6 unknown-network: participant 'Engine': the executable uses the network 'Diag' (raw), which "
                          "is not in networks: add it to networks with kind: can, or correct the network's name in the "
                          "ECU\n");
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

TEST_CASE(j1939_addresses_must_differ_on_a_network)
{
    // A node without a NAME does not claim: it keeps sending from the address.
    Setup s = good_pair();
    s.results[1].ecu->description.networks[0].nodes = {node(0x00), node(0x80)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard (node 1) both ask for the "
                          "address 0x00 (0), and Dashboard (node 1) has no NAME, so it does not take part in the address "
                          "claim and keeps sending from that address: give each node its own address\n");
    // Two that cannot move: the lowest NAME wins, the other has no address.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kEngineName - 1)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0); Dashboard wins the address claim (its NAME is the lowest), and Engine is not arbitrary "
                          "address capable, so it is left without an address (Cannot Claim): give each node its own "
                          "address\n");
    // One that cannot move and one that can: the first wins (the arbitrary address bit is the
    // highest of the NAME), the other moves.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 5)};
    check_text(s.check(), "W 3 address-shared: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0); Engine keeps it (its NAME is the lowest) and Dashboard will claim another address: give "
                          "each its own address to know where they are\n");
    // The same lowest NAME twice: an equal NAME loses too, nobody keeps the address. One
    // finding says both.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kEngineName)};
    check_text(s.check(), "E 3 name-conflict: network 'J1939_CAN': Engine and Dashboard have the same J1939 NAME "
                          "0x00000000FE000001, and they ask for the same address 0x00 (0), which none of them keeps (an "
                          "equal NAME loses the address claim): each node needs its own NAME (for a second copy of an "
                          "ECU, change its identity number)\n");
    // The same NAME on other addresses: only the NAME.
    s.results[1].ecu->description.networks[0].nodes = {node(0x01, kEngineName)};
    check_text(s.check(), "E 3 name-conflict: network 'J1939_CAN': Engine and Dashboard have the same J1939 NAME "
                          "0x00000000FE000001: each node needs its own NAME (for a second copy of an ECU, change its "
                          "identity number)\n");
    // One that cannot move and two that can: the first keeps it, both others move.
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 5), node(0x00, kArbitraryBit | 6)};
    check_text(s.check(), "W 3 address-shared: network 'J1939_CAN': Engine, Dashboard (node 1) and Dashboard (node 2) all "
                          "ask for the address 0x00 (0); Engine keeps it (its NAME is the lowest) and Dashboard (node 1) "
                          "and Dashboard (node 2) will claim another address: give each its own address to know where "
                          "they are\n");
    // Two without a NAME, and one without a NAME next to one that could move: errors.
    s.results[0].ecu->description.networks[0].nodes = {node(0x00)};
    s.results[1].ecu->description.networks[0].nodes = {node(0x00)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0), and Engine and Dashboard have no NAME, so they do not take part in the address claim and "
                          "keep sending from that address: give each node its own address\n");
    s.results[1].ecu->description.networks[0].nodes = {node(0x00, kArbitraryBit | 5)};
    check_text(s.check(), "E 3 address-conflict: network 'J1939_CAN': Engine and Dashboard both ask for the address 0x00 "
                          "(0), and Engine has no NAME, so it does not take part in the address claim and keeps sending "
                          "from that address: give each node its own address\n");
    s.results[0].ecu->description.networks[0].nodes = {node(0x00, kEngineName)};
    // All may move: a warning. 254 (any free address) is never a conflict.
    s.results[0].ecu->description.networks[0].nodes = {node(0x80, kEngineName | kArbitraryBit),
                                                        node(j1939::kNullAddress, kArbitraryBit | 7)};
    s.results[1].ecu->description.networks[0].nodes = {node(0x80, kArbitraryBit | 9), node(j1939::kNullAddress, kArbitraryBit | 8)};
    check_text(s.check(), "W 3 address-shared: network 'J1939_CAN': Engine (node 1) and Dashboard (node 1) both ask for the "
                          "address 0x80 (128); Dashboard (node 1) keeps it (its NAME is the lowest) and Engine (node 1) "
                          "will claim another address: give each its own address to know where they are\n");
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
                              "node its own address\n");
}

TEST_CASE(j1939_names_must_differ_on_a_network)
{
    Setup s = good_pair();
    s.described(participant("Engine2", 12), ecu("Engine2", 10ms, {on_j1939("J1939_CAN", {node(0x01, kEngineName)})}));
    check_text(s.check(), "E 3 name-conflict: network 'J1939_CAN': Engine and Engine2 have the same J1939 NAME "
                          "0x00000000FE000001: each node needs its own NAME (for a second copy of an ECU, change its "
                          "identity number)\n");
    s.results[2].ecu->description.networks[0].nodes[0].name = j1939::Name{kEngineName + 1};
    check_text(s.check(), "");
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
    check_text(refused(R"({"transport": "silkit", "wire_version": 70000})"), "control.wire_version is 70000, not a version");
}

TEST_CASE(variables_that_are_not_offered_are_a_note_and_another_version_of_the_messages_a_warning)
{
    // Variables, offered in this launcher's version of the messages: nothing to say.
    Setup s = good_pair();
    s.results[0].ecu->description.variables = {variable("x")};
    s.results[0].ecu->description.control = ControlOffer{"silkit", 1};
    // No variables: nothing to offer, whatever the executable says.
    s.results[1].ecu->description.control.reset();
    check_text(s.check(), "");
    // Variables that the executable does not offer (--no-control, an earlier vehicle-sil).
    s.results[1].ecu->description.variables = {variable("y")};
    check_text(s.check(), "N 9 control-link-not-offered: participant 'Dashboard': it describes variables, but does not offer "
                          "them to the simulation (--no-control, or an executable of a vehicle-sil that does not yet): "
                          "vehicle_sil_launch run cannot read or write them\n");
    // Offered in another version of the messages: known before anything is started.
    s.results[1].ecu->description.control = ControlOffer{"silkit", 2};
    check_text(s.check(), "W 9 control-link-version: participant 'Dashboard': it offers its variables as silkit messages of "
                          "version 2, and this vehicle_sil_launch speaks silkit messages of version 1, so vehicle_sil_launch "
                          "run cannot read or write them: build the ECU and vehicle_sil_launch from the same version of "
                          "vehicle-sil\n");
    // The check's JSON says what each executable offers.
    CheckReport r;
    r.file = "sim.yaml";
    r.config = s.config;
    r.participants = s.results;
    const std::string json = format_json(r);
    CHECK(json.find("      \"control\": {\n        \"transport\": \"silkit\",\n        \"wire_version\": 1\n      }\n") !=
          std::string::npos);
    CHECK(json.find("\"wire_version\": 2\n") != std::string::npos);
    // A disabled participant is not in the simulation: nothing about its variables.
    s.config.participants[1].enabled = false;
    CHECK(s.check().find("control-link") == std::string::npos);
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
               "error: dir/simulation.yaml:13: participant 'Cluster \xC3\xA9': it failed\n"
               "    the end of its standard error:\n"
               "    boom\n"
               "    bang\n"
               "error: dir/simulation.yaml:15: participants[3] has no name\n"
               "warning: dir/simulation.yaml:5: network 'Body': only Engine\n"
               "warning: dir/simulation.yaml: no line\n"
               "note: dir/simulation.yaml:18: participant 'Logger': not checked\n"
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
    check_text(format_text(unreadable), "error: x.yaml:2: syntax error: missing terminating ]\n"
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
            ]
          },
          {
            "name": "Body",
            "kind": "lin",
            "use": "lin_master",
            "nodes": []
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
            ]
          },
          {
            "name": "Body",
            "kind": "can",
            "use": "raw",
            "nodes": []
          }
        ]
      },
      "variables": [],
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
      "step_ms": null,
      "status": "failed",
      "reported": null,
      "variables": null,
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
      ]
    },
    {
      "name": "Body",
      "kind": "lin",
      "line": 5,
      "participants": [
        "Engine"
      ]
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
    CHECK(format_json(r).find("\n        ]\n      },\n      \"variables\": null,\n      \"control\": null\n    },\n    {\n      \"name\": \"Cluster") !=
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
               "the other participants of this network, or remove it\n"
               "note: sim.yaml:9: participant 'Dashboard': disabled (enabled: false, or --disable): it is neither checked "
               "nor started, and the other findings are about the simulation without it\n"
               "\n"
               "passed: 0 errors, 1 warning, 1 note\n");
    // The note does not fail --strict (the warning does).
    r.strict = true;
    r.findings.erase(r.findings.begin());
    CHECK(r.passed());
    const std::string json = format_json(r);
    CHECK(json.find("      \"required\": true,\n      \"enabled\": true,\n      \"manual\": false,\n      \"step_ms\": 10,\n      \"status\": "
                    "\"described\",\n") != std::string::npos);
    CHECK(json.find("      \"required\": true,\n      \"enabled\": false,\n      \"manual\": false,\n      \"step_ms\": null,\n      \"status\": "
                    "\"disabled\",\n      \"reported\": null,\n      \"variables\": null,\n      \"control\": null\n") != std::string::npos);
    CHECK(json.find("      \"participants\": [\n        \"Engine\"\n      ]\n") != std::string::npos);
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
    CHECK(format_json(r).find("      \"enabled\": false,\n      \"manual\": false,\n      \"step_ms\": null,\n      \"status\": \"disabled\",\n") !=
          std::string::npos);
}
