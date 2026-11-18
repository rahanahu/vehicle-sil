// SPDX-License-Identifier: MIT
//
// vehicle_sil_launch connections without processes: the bridges' lists read and passed on, the
// lists the launcher makes for a bridge it cannot ask, each bridge asked once, the quoting of the
// --connect lines, and the outputs for people and as JSON.
#include "launch_test_util.hpp"

#include "vehicle_sil/config/connect.hpp"
#include "vehicle_sil/launch/connections.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;
namespace fs = std::filesystem;

namespace {

/// The Ethernet bridge's list as docs/ethernet-bridge.md shows it (one adapter, the loopback).
const std::string kEthernetList = R"json({
  "vehicle_sil_targets": 1,
  "tool": "vehicle_sil_ethernet_bridge",
  "backend": "npcap",
  "source": "Npcap version 1.80",
  "targets": [
    {
      "value": "vEthernet (A)",
      "label": "vEthernet (A)",
      "detail": "up, connected; Hyper-V Virtual Ethernet Adapter; \\Device\\NPF_{0A1B}",
      "usable": true,
      "reason": null,
      "ethernet": {"pcap_name": "\\Device\\NPF_{0A1B}", "connection": "vEthernet (A)", "up": true, "connected": null}
    },
    {
      "value": "\\Device\\NPF_Loopback",
      "label": "\\Device\\NPF_Loopback",
      "detail": "up, loopback; Adapter for loopback traffic capture",
      "usable": false,
      "reason": "loopback: not an Ethernet adapter",
      "ethernet": {"pcap_name": "\\Device\\NPF_Loopback", "connection": null, "up": true, "connected": null}
    }
  ],
  "notes": [],
  "error": null
}
)json";

const std::string kCanList = R"({"vehicle_sil_targets": 1, "tool": "vehicle_sil_vector_xl_bridge", "backend": null, "source": null,
 "targets": [{"value": "virtual:1", "label": "Virtual Channel 1", "detail": "virtual 1", "usable": true, "reason": null,
              "can": {"target": "virtual", "number": 1, "serial": 0, "transceiver": null}}],
 "notes": [], "error": null})";

DescribeRun run(std::optional<int> code, std::string out = "", std::string err = "")
{
    DescribeRun r;
    r.command_text = "vehicle_sil_ethernet_bridge --list-interfaces --json";
    r.exit_code = code;
    r.standard_output = std::move(out);
    r.standard_error = std::move(err);
    r.timeout = std::chrono::milliseconds(30000);
    return r;
}

std::string error_of(const TargetList& list) { return list.error_code + ": " + list.error_message; }

NetworkConfig network(const std::string& name, NetworkKind kind, bool valid = true)
{
    return NetworkConfig{name, kind, 3, valid};
}

/// Tools for connections_of(): the bridges in `found` are at /bin/<name>; each answers its list
/// from `lists`, and `asked` counts the runs.
struct FakeTools
{
    std::map<std::string, std::string> lists;
    std::vector<std::string> found;
    std::vector<std::string> asked;
    Os os = Os::Windows;

    ConnectTools tools()
    {
        ConnectTools out;
        out.launcher_dir = fs::path("/opt/vsil/bin");
        out.os = os;
        out.find = [this](std::string_view name) -> std::optional<fs::path> {
            for (const std::string& f : found)
            {
                if (f == name)
                {
                    return fs::path("/bin") / f;
                }
            }
            return std::nullopt;
        };
        out.describe = [this](const fs::path& tool, const std::vector<std::string>& arguments) {
            const std::string name = tool.filename().string();
            std::string words = name;
            for (const std::string& a : arguments)
            {
                words += " " + a;
            }
            asked.push_back(words);
            DescribeRun r = run(0, lists.at(name));
            r.command_text = words;
            return r;
        };
        return out;
    }
};

} // namespace

TEST_CASE(connections_list_arguments_of_each_kind)
{
    CHECK(list_arguments(NetworkKind::Ethernet, "tap") ==
          std::vector<std::string>({"--list-interfaces", "--backend", "tap", "--json"}));
    CHECK(list_arguments(NetworkKind::Can, "socketcan") == std::vector<std::string>({"--list-channels", "--json"}));
    CHECK(list_arguments(NetworkKind::Can, "xl") == std::vector<std::string>({"--list-channels", "--json"}));
    CHECK(list_arguments(NetworkKind::Lin, "").empty());
}

TEST_CASE(connections_quote_a_target_for_every_shell_or_say_none_fits)
{
    const auto quoted = [](std::string_view word) { return quoted_for_shells(word).value_or("(none)"); };
    check_text(quoted("ETH_EXT=eth0"), "ETH_EXT=eth0");
    check_text(quoted("CAN1=physical:1"), "CAN1=physical:1");
    check_text(quoted("ETH=a/b.c+d-e"), "ETH=a/b.c+d-e");
    check_text(quoted("ETH_EXT=vEthernet (A)"), "\"ETH_EXT=vEthernet (A)\"");
    check_text(quoted("ETH=\\Device\\NPF_{0A1B}"), "\"ETH=\\Device\\NPF_{0A1B}\"");
    check_text(quoted("ETH=it's"), "\"ETH=it's\"");
    check_text(quoted("ETH=a,b"), "\"ETH=a,b\""); // PowerShell: an array without quotes
    check_text(quoted("ETH=a&b|c<d>e(f)^g;h"), "\"ETH=a&b|c<d>e(f)^g;h\"");
    check_text(quoted("ETH=\xE3\x82\xA4\xE3\x83\xBC\xE3\x82\xB5\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88 2"),
               "\"ETH=\xE3\x82\xA4\xE3\x83\xBC\xE3\x82\xB5\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88 2\"");
    // PowerShell takes U+201C, U+201D and U+201E for '"'.
    for (const char* none : {"ETH=a\"b", "ETH=$x", "ETH=a`b", "ETH=%PATH%", "ETH=a!b", "ETH=a\\", "ETH=a\\\\b",
                             "ETH=a\\$b", "ETH=a\tb", "ETH=Lab \xE2\x80\x9C" "A\xE2\x80\x9D", "ETH=\xE2\x80\x9E" "x"})
    {
        check_text(quoted(none), "(none)", none);
    }
    check_text(quoted("ETH=it\xE2\x80\x99" "s \xE2\x80\x98" "A\xE2\x80\x99"),
               "\"ETH=it\xE2\x80\x99" "s \xE2\x80\x98" "A\xE2\x80\x99\"");
    check_text(quoted_for_sh("ETH=a$b"), "'ETH=a$b'");
    check_text(quoted_for_sh("ETH=it's"), "'ETH=it'\\''s'");
}

TEST_CASE(connections_read_a_bridges_list)
{
    const TargetList list = read_target_list(kEthernetList);
    check_text(list.tool, "vehicle_sil_ethernet_bridge");
    check_text(list.backend.value_or("(null)") + "|" + list.source.value_or("(null)"), "npcap|Npcap version 1.80");
    CHECK(list.targets.size() == 2);
    CHECK(list.targets[0] == (ListedTarget{"vEthernet (A)", "vEthernet (A)",
                                           "up, connected; Hyper-V Virtual Ethernet Adapter; \\Device\\NPF_{0A1B}", true, "",
                                           "\\Device\\NPF_{0A1B}"}));
    CHECK(list.targets[1] == (ListedTarget{"\\Device\\NPF_Loopback", "\\Device\\NPF_Loopback",
                                           "up, loopback; Adapter for loopback traffic capture", false,
                                           "loopback: not an Ethernet adapter", "\\Device\\NPF_Loopback"}));
    CHECK(list.error_code.empty() && list.notes.empty());
    check_text(list.json, kEthernetList);

    const TargetList failed = read_target_list(R"({"vehicle_sil_targets": 1, "tool": "vehicle_sil_ethernet_bridge",
        "backend": "npcap", "source": null, "targets": [], "notes": ["Npcap offers no network adapter here"],
        "error": {"code": "library-missing", "message": "Npcap is not installed"}})");
    check_text(error_of(failed), "library-missing: Npcap is not installed");
    CHECK(failed.notes == std::vector<std::string>{"Npcap offers no network adapter here"});
    CHECK(!failed.source);

    const auto refused = [](const std::string& json) {
        try
        {
            static_cast<void>(read_target_list(json));
        }
        catch (const DescriptionError& e)
        {
            return std::string(e.what());
        }
        return std::string("read");
    };
    check_text(refused(""), "the output is empty");
    check_text(refused("Interfaces:\n  eth0"), "the output is not a JSON object");
    check_text(refused("{\"vehicle_sil_targets\": 2, \"targets\": []}"),
               "it is a list of version 2, and this vehicle_sil_launch reads version 1");
    check_text(refused("{\"vehicle_sil_ecu\": 1}"), "the key vehicle_sil_targets is missing");
    check_text(refused("{\"vehicle_sil_targets\": 1}"), "targets is missing or not an array");
    check_text(refused("{\"vehicle_sil_targets\": 1, \"targets\": [{\"label\": \"x\"}]}"), "targets[0].value is missing");
    check_text(refused("{\"vehicle_sil_targets\": 1, \"targets\": [{\"value\": 1}]}"), "targets[0].value is not a string");
    check_text(refused("{\"vehicle_sil_targets\": 1, \"targets\": [{\"value\": \"a\", \"usable\": \"yes\"}]}"),
               "targets[0].usable is not true or false");
    check_text(refused("{\"vehicle_sil_targets\": 1, \"targets\": [], \"error\": \"x\"}"), "error is not an object or null");
    CHECK(refused("{\"vehicle_sil_targets\": 1, \"targets\": [").rfind("the output is not valid JSON (line ", 0) == 0);
    // Nested deeper than the launcher writes back.
    const auto nested = [](std::size_t depth) {
        return "{\"vehicle_sil_targets\": 1, \"targets\": [], \"x\": " + std::string(depth - 1, '[') + std::string(depth - 1, ']') + "}";
    };
    check_text(refused(nested(kMaxListDepth)), "read");
    check_text(refused(nested(kMaxListDepth + 1)), "it is nested deeper than 64 levels");
}

TEST_CASE(connections_refuse_what_connect_does_not_take)
{
    TargetList list = read_target_list(R"({"vehicle_sil_targets": 1, "tool": "vehicle_sil_ethernet_bridge",
        "targets": [{"value": "none", "usable": true},
                    {"value": "-x", "usable": true, "reason": null},
                    {"value": "eth0", "usable": true},
                    {"value": "tap:x", "usable": true},
                    {"value": "lo", "usable": false, "reason": "loopback: not an Ethernet adapter"}]})");
    prefix_targets(NetworkKind::Ethernet, "packet", list, Os::Linux);
    refuse_targets("ETH", NetworkKind::Ethernet, list, Os::Linux);
    const auto said = [&list] {
        std::string out;
        for (const ListedTarget& t : list.targets)
        {
            out += t.value + ": " + (t.usable ? std::string("usable") : t.reason) + (t.refused ? " (refused)" : "") + "\n";
        }
        return out;
    };
    // An interface named none or like another backend's prefix is given with its own backend's prefix.
    check_text(said(), "packet:none: usable\n"
                       "-x: --connect does not take it: the target starts with '-', which the bridge would take for an "
                       "option (refused)\n"
                       "eth0: usable\n"
                       "packet:tap:x: usable\n"
                       "lo: loopback: not an Ethernet adapter\n");
    // The value of each backend: bare for the default of the OS, else with its prefix.
    TargetList tap = read_target_list(R"({"vehicle_sil_targets": 1, "targets": [{"value": "vsil0"}]})");
    prefix_targets(NetworkKind::Ethernet, "tap", tap, Os::Linux);
    check_text(tap.targets[0].value, "tap:vsil0");
    TargetList npcap = read_target_list(R"({"vehicle_sil_targets": 1, "targets": [{"value": "Ethernet 2"}]})");
    prefix_targets(NetworkKind::Ethernet, "npcap", npcap, Os::Windows);
    check_text(npcap.targets[0].value, "Ethernet 2");
    TargetList socketcan = read_target_list(R"({"vehicle_sil_targets": 1, "targets": [{"value": "can0"}, {"value": "virtual:1"}]})");
    prefix_targets(NetworkKind::Can, "socketcan", socketcan, Os::Linux);
    check_text(socketcan.targets[0].value + "|" + socketcan.targets[1].value, "can0|socketcan:virtual:1");
    // CAN: the form of virtual:<n> and physical:<n>.
    TargetList can = read_target_list(R"({"vehicle_sil_targets": 1, "targets": [{"value": "xl:usb:1"}, {"value": "virtual:1"}]})");
    refuse_targets("CAN1", NetworkKind::Can, can, Os::Windows);
    CHECK(!can.targets[0].usable && can.targets[0].refused && can.targets[1].usable);
    // A network whose name --connect cannot give: none of its targets.
    for (const char* name : {"A=B", "--ETH"})
    {
        TargetList named = read_target_list(R"({"vehicle_sil_targets": 1, "targets": [{"value": "eth0"}]})");
        refuse_targets(name, NetworkKind::Ethernet, named, Os::Linux);
        CHECK(!named.targets[0].usable);
    }
    TargetList equals = read_target_list(R"({"vehicle_sil_targets": 1, "targets": [{"value": "eth0"}]})");
    refuse_targets("A=B", NetworkKind::Ethernet, equals, Os::Linux);
    check_text(equals.targets[0].reason, "the network's name has '=', and --connect takes the network's name up to the first "
                                         "'=': rename the network (a name without '=')");
}

TEST_CASE(connections_make_a_list_for_a_bridge_that_gives_none)
{
    // A list with an error, with which the bridge ends with exit code 3: passed on as it is.
    const std::string libraryMissing = R"({"vehicle_sil_targets": 1, "tool": "vehicle_sil_ethernet_bridge", "targets": [],
        "error": {"code": "library-missing", "message": "Npcap is not installed"}})";
    const TargetList passed = interpret_list(NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", run(3, libraryMissing));
    check_text(error_of(passed), "library-missing: Npcap is not installed");
    check_text(passed.json, libraryMissing);

    DescribeRun notStarted = run(std::nullopt);
    notStarted.start_error = "Permission denied";
    check_text(error_of(interpret_list(NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", notStarted)),
               "tool-cannot-start: vehicle_sil_ethernet_bridge --list-interfaces --json could not be started (Permission "
               "denied): check that the bridge runs on this computer");
    check_text(error_of(interpret_list(NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", run(std::nullopt))),
               "tool-timeout: vehicle_sil_ethernet_bridge --list-interfaces --json did not end within 30000 ms, so it was "
               "stopped: run that command yourself to see what it waits for");
    // Windows did not start it: its DLL (the same words as the check's).
    DescribeRun dll = run(static_cast<int>(0xC0000135u));
    dll.command_text = "vehicle_sil_vector_xl_bridge --list-channels --json";
    check_text(error_of(interpret_list(NetworkKind::Can, "vehicle_sil_vector_xl_bridge", dll)),
               "tool-cannot-start: vehicle_sil_vector_xl_bridge --list-channels --json could not start (exit status "
               "0xC0000135): Windows did not find a DLL that it needs: vehicle_sil_vector_xl_bridge needs vxlapi64.dll of the "
               "Vector XL Driver Library: install the library, or put vxlapi64.dll next to the bridge; for another DLL, put "
               "that DLL, of the version the executable was built with, next to the executable or in a folder on PATH");
    check_text(error_of(interpret_list(NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", run(static_cast<int>(0xC0000139u)))),
               "tool-cannot-start: vehicle_sil_ethernet_bridge --list-interfaces --json could not start (exit status "
               "0xC0000139): a DLL that it needs is of another version (a function in it is missing): put that DLL, of the "
               "version the executable was built with, next to the executable or in a folder on PATH");
    // A bridge of before the lists as JSON: its own line says why.
    const TargetList old = interpret_list(NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge",
                                          run(4, "", "[eth-bridge] error: unknown option --json\nusage: ...\n"));
    check_text(error_of(old), "tool-bad-output: vehicle_sil_ethernet_bridge --list-interfaces --json wrote no list that "
                              "vehicle_sil_launch reads (the output is empty), and ended with exit code 4; it wrote: "
                              "[eth-bridge] error: unknown option --json: use the bridge built with the vehicle-sil of this "
                              "vehicle_sil_launch");
    DescribeRun killed = run(137, "{\"vehicle_sil_targets\": 1, \"tar");
    killed.signal = 9;
    const std::string cut = error_of(interpret_list(NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", killed));
    CHECK(cut.rfind("tool-bad-output: vehicle_sil_ethernet_bridge --list-interfaces --json wrote no list that "
                    "vehicle_sil_launch reads (the output is not valid JSON (line ",
                    0) == 0);
    CHECK(cut.find("), and was ended by signal 9: use the bridge built with") != std::string::npos);
    // A failed list is a list of the same form, as JSON too.
    const TargetList made = failed_list("vehicle_sil_vector_xl_bridge", "tool-missing", "not \"here\"");
    check_text(made.json, "{\"vehicle_sil_targets\":1,\"tool\":\"vehicle_sil_vector_xl_bridge\",\"backend\":null,\"source\":"
                          "null,\"targets\":[],\"notes\":[],\"error\":{\"code\":\"tool-missing\",\"message\":\"not \\\"here\\\"\"}}");
    const TargetList again = read_target_list(made.json);
    check_text(error_of(again), "tool-missing: not \"here\"");
}

TEST_CASE(connections_ask_each_bridge_once_and_list_every_network)
{
    SimulationConfig config;
    config.networks = {network("ETH_A", NetworkKind::Ethernet), network("CAN1", NetworkKind::Can),
                       network("BODY_LIN", NetworkKind::Lin), network("ETH_B", NetworkKind::Ethernet),
                       network("BROKEN", NetworkKind::Can, false)};
    FakeTools fake;
    fake.lists["vehicle_sil_ethernet_bridge"] = kEthernetList;
    fake.found = {"vehicle_sil_ethernet_bridge"};
    const std::vector<NetworkConnections> all = connections_of(config, std::nullopt, fake.tools());
    CHECK(fake.asked == std::vector<std::string>{"vehicle_sil_ethernet_bridge --list-interfaces --backend npcap --json"});
    CHECK(all.size() == 5);
    check_text(all[0].name + "|" + all[0].tool + "|" + all[0].backend + "|" + all[0].tool_path.generic_string(),
               "ETH_A|vehicle_sil_ethernet_bridge|npcap|/bin/vehicle_sil_ethernet_bridge");
    CHECK(all[0].list && all[0].list->targets.size() == 2 && all[0].list->error_code.empty());
    CHECK(all[3].list && all[3].list->json == all[0].list->json);
    check_text(all[1].tool + "|" + all[1].backend, "vehicle_sil_vector_xl_bridge|xl");
    CHECK(all[1].tool_path.empty());
    check_text(error_of(*all[1].list),
               "tool-missing: vehicle_sil_vector_xl_bridge is neither next to vehicle_sil_launch (" +
                   process::shown_path(fs::path("/opt/vsil/bin")) +
                   ") nor on PATH: build it with the Vector XL Driver Library (docs/bridges.md) and put it next to "
                   "vehicle_sil_launch, or its directory on PATH");
    CHECK(all[2].kind == NetworkKind::Lin && all[2].tool.empty() && !all[2].list &&
          all[2].no_bridge == "there is no bridge for LIN");
    CHECK(!all[4].kind && !all[4].list && all[4].tool.empty());

    // Linux: Ethernet is listed for each of packet and tap, CAN for socketcan only (Vector XL is for Windows).
    FakeTools linux_;
    linux_.os = Os::Linux;
    linux_.lists = {{"vehicle_sil_ethernet_bridge", kEthernetList}, {"vehicle_sil_socketcan_bridge", kCanList}};
    linux_.found = {"vehicle_sil_ethernet_bridge", "vehicle_sil_socketcan_bridge"};
    const std::vector<NetworkConnections> here = connections_of(config, std::nullopt, linux_.tools());
    const std::vector<std::string> expected{"vehicle_sil_ethernet_bridge --list-interfaces --backend packet --json",
                                            "vehicle_sil_ethernet_bridge --list-interfaces --backend tap --json",
                                            "vehicle_sil_socketcan_bridge --list-channels --json"};
    CHECK(linux_.asked == expected);
    // ETH_A (packet, tap), CAN1 (socketcan), BODY_LIN, ETH_B (packet, tap), BROKEN.
    CHECK(here.size() == 7);
    check_text(here[0].backend + "|" + here[1].backend + "|" + here[2].backend + "|" + here[2].tool, "packet|tap|socketcan|vehicle_sil_socketcan_bridge");
    // The value --connect takes: bare for the default backend, with the prefix for the other.
    check_text(here[0].list->targets[0].value, "vEthernet (A)");
    check_text(here[1].list->targets[0].value, "tap:vEthernet (A)");
    check_text(here[2].list->targets[0].value, "socketcan:virtual:1");  // a name that reads as a Vector XL target
    // Without a bridge on Linux: tool-missing for each backend, the same for both Ethernet networks.
    FakeTools bare;
    bare.os = Os::Linux;
    const std::vector<NetworkConnections> missing = connections_of(config, std::string("CAN1"), bare.tools());
    CHECK(missing.size() == 1 && missing[0].backend == "socketcan" && missing[0].list &&
          missing[0].list->error_code == "tool-missing" && bare.asked.empty());

    // --network: that one only; its bridge only is asked.
    FakeTools one;
    one.lists = {{"vehicle_sil_ethernet_bridge", kEthernetList}, {"vehicle_sil_vector_xl_bridge", kCanList}};
    one.found = {"vehicle_sil_ethernet_bridge", "vehicle_sil_vector_xl_bridge"};
    const std::vector<NetworkConnections> can = connections_of(config, std::string("CAN1"), one.tools());
    CHECK(one.asked == std::vector<std::string>{"vehicle_sil_vector_xl_bridge --list-channels --json"});
    CHECK(can.size() == 1 && can[0].name == "CAN1" && can[0].list && can[0].list->targets.size() == 1);
    CHECK(connections_of(config, std::string("BODY_LIN"), one.tools()).size() == 1);
    CHECK(one.asked.size() == 1);
}

TEST_CASE(connections_say_which_networks_the_file_has_for_an_unknown_network)
{
    SimulationConfig config;
    config.networks = {network("ETH_EXT", NetworkKind::Ethernet), network("CAN1", NetworkKind::Can)};
    CHECK(!unknown_network(config, "CAN1"));
    check_text(unknown_network(config, "ETH_EX")->message,
               "--network names the network 'ETH_EX', which the file does not have: did you mean 'ETH_EXT'? (the networks: "
               "ETH_EXT, CAN1)");
    check_text(unknown_network(config, "BODY")->message,
               "--network names the network 'BODY', which the file does not have: the networks are ETH_EXT, CAN1");
    check_text(unknown_network(config, "BODY")->code, "usage");
    check_text(unknown_network(SimulationConfig{}, "BODY")->message,
               "--network names the network 'BODY', which the file does not have: the file has no networks");
}

TEST_CASE(connections_text_has_lines_to_copy_and_why_the_others_cannot_be_used)
{
    ConnectionsReport report;
    report.file = "simulation.yaml";
    report.readable = true;
    NetworkConnections eth{"ETH_EXT", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"),
                           read_target_list(kEthernetList)};
    TargetList odd = read_target_list(R"({"vehicle_sil_targets": 1, "tool": "vehicle_sil_ethernet_bridge",
        "backend": "packet", "source": null,
        "targets": [{"value": "eth$1", "label": "eth$1", "detail": "up", "usable": true},
                    {"value": "none", "detail": "up", "usable": true},
                    {"value": "-x", "detail": "up", "usable": true},
                    {"value": "tap0", "detail": "", "usable": true}],
        "notes": ["Npcap offers no network adapter here"], "error": null})");
    prefix_targets(NetworkKind::Ethernet, "packet", odd, Os::Linux);
    refuse_targets("ETH_B", NetworkKind::Ethernet, odd, Os::Linux);
    NetworkConnections other{"ETH_B", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"), odd};
    TargetList unnamedList = read_target_list(kEthernetList);
    refuse_targets("A=B", NetworkKind::Ethernet, unnamedList, Os::Windows);
    NetworkConnections unnamed{"A=B", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"), unnamedList};
    // A bridge's words that a terminal would act on.
    NetworkConnections odder{"ETH_D", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"),
                             failed_list("vehicle_sil_ethernet_bridge", "\x1B[2J", "a\xE2\x80\xAE" "b\xC2\x85")};
    NetworkConnections can{"CAN1", NetworkKind::Can, "vehicle_sil_vector_xl_bridge", fs::path("/bin/xl"),
                           read_target_list(kCanList)};
    NetworkConnections missing{"CAN2", NetworkKind::Can, "vehicle_sil_vector_xl_bridge", {},
                               failed_list("vehicle_sil_vector_xl_bridge", "tool-missing", "it is not here")};
    NetworkConnections empty{"ETH_C", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"),
                             read_target_list(R"({"vehicle_sil_targets": 1, "tool": "vehicle_sil_ethernet_bridge",
                                 "backend": "packet", "targets": []})")};
    NetworkConnections lin{"BODY_LIN", NetworkKind::Lin, "", {}, std::nullopt, "there is no bridge for LIN"};
    NetworkConnections noXl{"CAN3", NetworkKind::Can, "", {}, std::nullopt, "the Vector XL bridge is for Windows"};
    NetworkConnections broken{"BROKEN", std::nullopt, "", {}, std::nullopt};
    report.per_network = {eth, other, can, missing, empty, lin, noXl, unnamed, odder, broken};
    check_text(format_connections_text(report),
               "simulation.yaml: where each network can be connected on this computer\n"
               "ETH_EXT (ethernet; vehicle_sil_ethernet_bridge, npcap: Npcap version 1.80):\n"
               "  --connect \"ETH_EXT=vEthernet (A)\"  up, connected; Hyper-V Virtual Ethernet Adapter; \\Device\\NPF_{0A1B}\n"
               "  cannot be used: \\Device\\NPF_Loopback  (loopback: not an Ethernet adapter)\n"
               "ETH_B (ethernet; vehicle_sil_ethernet_bridge, packet):\n"
               "  --connect 'ETH_B=eth$1'      up\n"
               "  --connect ETH_B=packet:none  none; up\n"
               "  --connect ETH_B=tap0\n"
               "  cannot be used: -x  (--connect does not take it: the target starts with '-', which the bridge would take "
               "for an option)\n"
               "  note: the line of eth$1 is quoted for sh: PowerShell and cmd read one of \" $ ` % ! \\ \xE2\x80\x9C "
               "\xE2\x80\x9D \xE2\x80\x9E in it otherwise, so quote it as your shell needs\n"
               "  note: Npcap offers no network adapter here\n"
               "CAN1 (can; vehicle_sil_vector_xl_bridge):\n"
               "  --connect CAN1=virtual:1  Virtual Channel 1; virtual 1\n"
               "CAN2 (can; vehicle_sil_vector_xl_bridge):\n"
               "  cannot list: it is not here [tool-missing]\n"
               "ETH_C (ethernet; vehicle_sil_ethernet_bridge, packet):\n"
               "  nothing listed: vehicle_sil_ethernet_bridge found nothing to connect to on this computer\n"
               "BODY_LIN (lin): SIL Kit only (there is no bridge for LIN)\n"
               "CAN3 (can): SIL Kit only (the Vector XL bridge is for Windows)\n"
               "A=B (ethernet; vehicle_sil_ethernet_bridge, npcap: Npcap version 1.80):\n"
               "  cannot be connected with --connect: the network's name has '=', and --connect takes the network's name up "
               "to the first '=': rename the network (a name without '=')\n"
               "ETH_D (ethernet; vehicle_sil_ethernet_bridge):\n"
               "  cannot list: a\\u{202E}b\\u{0085} [\\x1B[2J]\n"
               "BROKEN: its kind in the file has an error, so nothing is listed: vehicle_sil_launch check simulation.yaml says "
               "what is wrong\n");
    CHECK(report.incomplete() && report.exit_code() == 1);

    // No bridge here (tool-missing, none for the kind) is an answer; a bridge that cannot list is not.
    report.per_network = {eth, can, lin, missing, noXl};
    CHECK(!report.incomplete() && report.exit_code() == 0);
    report.per_network.push_back(odder);
    CHECK(report.incomplete() && report.exit_code() == 1);
    report.per_network = {eth};
    report.findings.push_back(Finding{Severity::Error, "unknown-key", "unknown key 'network'", 2, "", ""});
    check_text(format_connections_text(report),
               "simulation.yaml: where each network can be connected on this computer\n"
               "ETH_EXT (ethernet; vehicle_sil_ethernet_bridge, npcap: Npcap version 1.80):\n"
               "  --connect \"ETH_EXT=vEthernet (A)\"  up, connected; Hyper-V Virtual Ethernet Adapter; \\Device\\NPF_{0A1B}\n"
               "  cannot be used: \\Device\\NPF_Loopback  (loopback: not an Ethernet adapter)\n"
               "\nerror: simulation.yaml:2: unknown key 'network' [unknown-key]\n\n"
               "the file has errors: the list above has the networks that could be read\n");
    CHECK(report.incomplete() && report.exit_code() == 1);
    report.findings.clear();
    report.per_network.clear();
    check_text(format_connections_text(report), "simulation.yaml: the file has no networks, so there is nothing to connect\n");
    report.usage = true;
    CHECK(report.exit_code() == 4);
    ConnectionsReport unreadable;
    unreadable.file = "missing.yaml";
    unreadable.findings.push_back(Finding{Severity::Error, "file", "the file does not exist: check the path", 0, "", ""});
    CHECK(unreadable.exit_code() == 3);
    check_text(format_connections_text(unreadable),
               "error: missing.yaml: the file does not exist: check the path [file]\n\ncannot read missing.yaml\n");
}

TEST_CASE(connections_json_passes_each_list_on_with_every_key)
{
    ConnectionsReport report;
    report.file = "simulation.yaml";
    report.readable = true;
    report.per_network = {
        NetworkConnections{"CAN1", NetworkKind::Can, "vehicle_sil_vector_xl_bridge", fs::path("/bin/xl"),
                           read_target_list(kCanList)},
        NetworkConnections{"CAN2", NetworkKind::Can, "vehicle_sil_vector_xl_bridge", {},
                           failed_list("vehicle_sil_vector_xl_bridge", "tool-missing", "it is not here")},
        NetworkConnections{"BODY_LIN", NetworkKind::Lin, "", {}, std::nullopt},
        NetworkConnections{"BROKEN", std::nullopt, "", {}, std::nullopt}};
    report.per_network[0].backend = "xl";
    std::string json = format_connections_json(report);
    const std::string version = "\"vehicle_sil\": \"" + std::string(vehicle_sil::version()) + "\"";
    json.replace(json.find(version), version.size(), "\"vehicle_sil\": \"<version>\"");
    check_text(json, R"({
  "vehicle_sil_connections": 1,
  "vehicle_sil": "<version>",
  "file": "simulation.yaml",
  "result": "failed",
  "networks": [
    {
      "name": "CAN1",
      "kind": "can",
      "tool": "vehicle_sil_vector_xl_bridge",
      "backend": "xl",
      "tool_path": ")" + process::shown_path(fs::path("/bin/xl")) + R"(",
      "list": {
        "vehicle_sil_targets": 1,
        "tool": "vehicle_sil_vector_xl_bridge",
        "backend": null,
        "source": null,
        "targets": [
          {
            "value": "virtual:1",
            "label": "Virtual Channel 1",
            "detail": "virtual 1",
            "usable": true,
            "reason": null,
            "can": {
              "target": "virtual",
              "number": 1,
              "serial": 0,
              "transceiver": null
            }
          }
        ],
        "notes": [],
        "error": null
      }
    },
    {
      "name": "CAN2",
      "kind": "can",
      "tool": "vehicle_sil_vector_xl_bridge",
      "backend": null,
      "tool_path": null,
      "list": {
        "vehicle_sil_targets": 1,
        "tool": "vehicle_sil_vector_xl_bridge",
        "backend": null,
        "source": null,
        "targets": [],
        "notes": [],
        "error": {
          "code": "tool-missing",
          "message": "it is not here"
        }
      }
    },
    {
      "name": "BODY_LIN",
      "kind": "lin",
      "tool": null,
      "backend": null,
      "tool_path": null,
      "list": null
    },
    {
      "name": "BROKEN",
      "kind": null,
      "tool": null,
      "backend": null,
      "tool_path": null,
      "list": null
    }
  ],
  "findings": []
}
)");
    // Strings with escapes, numbers that are not integers, and objects in arrays, as the bridge wrote them.
    report.per_network = {NetworkConnections{"ETH", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"),
                                          read_target_list(kEthernetList)}};
    report.per_network[0].list->json = R"({"vehicle_sil_targets": 1, "targets": [], "x": [1.5, -2, "a\"b\\c", [true, false]]})";
    json = format_connections_json(report);
    CHECK(json.find("\"result\": \"listed\"") != std::string::npos);
    CHECK(json.find("        \"x\": [\n          1.5,\n          -2,\n          \"a\\\"b\\\\c\",\n          [\n"
                    "            true,\n            false\n          ]\n        ]\n") != std::string::npos);

    // What --connect does not take is not usable in the JSON either; the other keys stay.
    TargetList refused = read_target_list(R"({"vehicle_sil_targets": 1, "targets": [
        {"value": "none", "usable": true, "reason": null, "ethernet": {"pcap_name": "\\Device\\NPF_{9}"}},
        {"value": "-x", "label": "-x"}, {"value": "eth0", "usable": true, "reason": null}]})");
    prefix_targets(NetworkKind::Ethernet, "npcap", refused, Os::Windows);
    refuse_targets("ETH", NetworkKind::Ethernet, refused, Os::Windows);
    report.per_network = {NetworkConnections{"ETH", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"),
                                             refused}};
    json = format_connections_json(report);
    // The value is what --connect takes: an interface named none has its backend before it.
    CHECK(json.find("          {\n            \"value\": \"npcap:none\",\n            \"usable\": true,\n            \"reason\": "
                    "null,\n            \"ethernet\": {\n") != std::string::npos);
    CHECK(json.find("          {\n            \"value\": \"-x\",\n            \"label\": \"-x\",\n            \"usable\": false,\n"
                    "            \"reason\": \"--connect does not take it: the target starts with '-', which the bridge would "
                    "take for an option\"\n          }") != std::string::npos);
    CHECK(json.find("            \"value\": \"eth0\",\n            \"usable\": true,\n            \"reason\": null\n")
          != std::string::npos);

    ConnectionsReport usage;
    usage.file = "simulation.yaml";
    usage.usage = true;
    usage.findings.push_back(Finding{Severity::Error, "usage", "--network names the network 'X'", 0, "", ""});
    json = format_connections_json(usage);
    CHECK(json.find("\"result\": \"usage\",\n  \"networks\": [],\n  \"findings\": [\n    {\n      \"severity\": \"error\",\n"
                    "      \"code\": \"usage\",") != std::string::npos);
    ConnectionsReport unreadable;
    unreadable.file = "missing.yaml";
    CHECK(format_connections_json(unreadable).find("\"result\": \"unreadable\"") != std::string::npos);
}

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

/// What check_requested() says of `network=target` as "code" or "usable", on Linux with the Ethernet
/// bridge there and the SocketCAN bridge not.
std::string requested_as(const std::string& name, const std::string& target)
{
    SimulationConfig config;
    config.networks = {network("ETH", NetworkKind::Ethernet), network("CAN1", NetworkKind::Can),
                       network("BODY_LIN", NetworkKind::Lin), network("BROKEN", NetworkKind::Can, false)};
    FakeTools fake;
    fake.os = Os::Linux;
    fake.lists = {{"vehicle_sil_ethernet_bridge", kEthernetList}};
    fake.found = {"vehicle_sil_ethernet_bridge"};
    const std::vector<NetworkConnections> listed = connections_of(config, std::nullopt, fake.tools());
    const std::vector<ConnectRequest> requests{{name, target}};
    const std::vector<RequestedTarget> checked = check_requested(listed, requests, Os::Linux);
    if (checked.empty())
    {
        return "left out";
    }
    return checked.front().usable ? "usable" : checked.front().code;
}

} // namespace

TEST_CASE(connections_request_a_target_is_compared_by_backend_and_name)
{
    // The same target with and without the default backend's prefix; each backend has its own list.
    CHECK(requested_as("ETH", "vEthernet (A)") == "usable");
    CHECK(requested_as("ETH", "packet:vEthernet (A)") == "usable");
    CHECK(requested_as("ETH", "tap:vEthernet (A)") == "usable");
    CHECK(requested_as("ETH", "tap:vEthernet (B)") == "target-not-listed");
    // Not in the list; listed but not usable (the reason is the list's).
    CHECK(requested_as("ETH", "eth9") == "target-not-listed");
    CHECK(requested_as("ETH", "\\Device\\NPF_Loopback") == "target-unusable");
    // A backend that this OS has no list for, and one it cannot name.
    CHECK(requested_as("CAN1", "virtual:1") == "target-not-listed");
    CHECK(requested_as("ETH", "npcap:vEthernet (A)") == "target-bad");
    CHECK(requested_as("ETH", "packet:") == "target-bad");
    // The bridge is not there: its list could not be made.
    CHECK(requested_as("CAN1", "can0") == "list-failed");
    // LIN has no bridge; a network with an error in its kind has no list.
    CHECK(requested_as("BODY_LIN", "eth0") == "target-bad");
    CHECK(requested_as("BROKEN", "can0") == "list-failed");
    // none asks for nothing; a network that was not listed is not checked.
    CHECK(requested_as("ETH", "none") == "left out");
    CHECK(requested_as("OTHER", "eth0") == "left out");
}

TEST_CASE(connections_request_says_why_a_target_cannot_be_used_and_ends_with_exit_code_1)
{
    FakeTools fake;
    fake.os = Os::Linux;
    fake.lists = {{"vehicle_sil_ethernet_bridge", kEthernetList}};
    fake.found = {"vehicle_sil_ethernet_bridge"};
    SimulationConfig config;
    config.networks = {network("ETH", NetworkKind::Ethernet)};
    ConnectionsReport report;
    report.file = "simulation.yaml";
    report.readable = true;
    report.per_network = connections_of(config, std::nullopt, fake.tools());
    CHECK(report.exit_code() == 0);
    report.request = ConnectionsRequest{"bench.json", check_requested(report.per_network, std::vector<ConnectRequest>{{"ETH", "vEthernet (A)"}}, Os::Linux)};
    CHECK(!report.request_unusable() && report.exit_code() == 0);
    CHECK(has(format_connections_text(report), "ETH (ethernet; vehicle_sil_ethernet_bridge, npcap: Npcap version 1.80):\n"
                                                    "  requested in bench.json: --connect \"ETH=vEthernet (A)\"  usable\n"));
    CHECK(has(format_connections_json(report), "  \"request\": {\n    \"file\": \"bench.json\",\n    \"connect\": [\n      {\n"
                                                    "        \"network\": \"ETH\",\n        \"target\": \"vEthernet (A)\",\n"
                                                    "        \"usable\": true,\n        \"code\": null,\n        \"reason\": null\n"));

    report.request = ConnectionsRequest{"bench.json", check_requested(report.per_network, std::vector<ConnectRequest>{{"ETH", "eth9"}}, Os::Linux)};
    CHECK(report.request_unusable() && report.exit_code() == 1);
    const std::string text = format_connections_text(report);
    CHECK(has(text, "  requested in bench.json: --connect ETH=eth9  not usable: eth9 is not in the list of "
                         "vehicle_sil_ethernet_bridge (packet) for ETH: choose a target from the list, or fix the request file "
                         "[target-not-listed]\n"));
    const std::string json = format_connections_json(report);
    CHECK(has(json, "\"result\": \"failed\"") && has(json, "\"code\": \"target-not-listed\""));
    // Without --request: no key.
    report.request.reset();
    CHECK(!has(format_connections_json(report), "\"request\""));
    CHECK(report.exit_code() == 0);
}

TEST_CASE(connections_request_a_name_of_another_backend_is_not_the_target)
{
    // A list of packet that has a target written for tap (a value as the bridge wrote it, not prefixed): the name alone is not the target.
    NetworkConnections entry{"ETH", NetworkKind::Ethernet, "vehicle_sil_ethernet_bridge", fs::path("/bin/eth"),
                             read_target_list(R"({"vehicle_sil_targets": 1, "tool": "vehicle_sil_ethernet_bridge",
        "backend": "packet", "source": null,
        "targets": [{"value": "tap:eth0", "label": "eth0", "detail": "", "usable": true}],
        "notes": [], "error": null})")};
    entry.backend = "packet";
    const std::vector<RequestedTarget> checked =
        check_requested({entry}, std::vector<ConnectRequest>{{"ETH", "eth0"}, {"ETH", "packet:eth0"}}, Os::Linux);
    CHECK(checked.size() == 2 && !checked[0].usable && checked[0].code == "target-not-listed" && !checked[1].usable);
}
