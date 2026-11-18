// SPDX-License-Identifier: MIT
//
// --list-interfaces --json: the JSON that vehicle_sil_launch and the GUI read, made from lists
// given by hand (the devices of a pcap library, the interfaces of the OS) and from errors.
#include "check_text.hpp"
#include "test_harness.hpp"

#include "pcap_api.hpp"
#include "pcap_link.hpp"

#include "vehicle_sil/eth_link/external_link.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"
#include "vehicle_sil/ethernet_bridge/interface_list.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::ethernet_bridge;
using vehicle_sil::eth_link::InterfaceInfo;
using vehicle_sil::eth_link::LinkError;
using vehicle_sil::eth_link::LinkErrorKind;
using vehicle_sil::eth_link::PcapDevice;
using vehicle_sil::eth_link::PcapInterfaceList;
using namespace vehicle_sil::eth_link::detail;

const char* const kNpf = "\\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}";
const char* const kNpfOther = "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}";

PcapInterfaceList npcap_list(std::vector<PcapDevice> devices)
{
    return pcap_interface_list_of(std::move(devices), PcapFlavor::Npcap, "Npcap version 1.80, based on libpcap version 1.10.5");
}

bool contains(const std::string& text, const std::string& part)
{
    const bool found = text.find(part) != std::string::npos;
    if (!found)
    {
        std::printf("    not found: %s\n    in: %s\n", part.c_str(), text.c_str());
    }
    return found;
}

} // namespace

TEST_CASE(list_json_of_npcap_has_what_to_give_as_interface_and_what_cannot_be_used)
{
    const auto list = npcap_list({{kNpf, "Microsoft KM-TEST Loopback Adapter", "Ethernet 2", kPcapIfUp | kPcapIfConnected},
                                  {"\\Device\\NPF_Loopback", "Adapter for loopback traffic capture", "", kPcapIfUp | kPcapIfLoopback}});
    CHECK_TEXT(pcap_interfaces_json(Backend::Npcap, list),
               "{\n"
               "  \"vehicle_sil_targets\": 1,\n"
               "  \"tool\": \"vehicle_sil_ethernet_bridge\",\n"
               "  \"backend\": \"npcap\",\n"
               "  \"source\": \"Npcap version 1.80, based on libpcap version 1.10.5\",\n"
               "  \"targets\": [\n"
               "    {\n"
               "      \"value\": \"Ethernet 2\",\n"
               "      \"label\": \"Ethernet 2\",\n"
               "      \"detail\": \"up, connected; Microsoft KM-TEST Loopback Adapter; "
               "\\\\Device\\\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}\",\n"
               "      \"usable\": true,\n"
               "      \"reason\": null,\n"
               "      \"in_use\": false,\n"
               "      \"in_use_by\": [],\n"
               "      \"in_use_count\": 0,\n"
               "      \"ethernet\": {\n"
               "        \"pcap_name\": \"\\\\Device\\\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}\",\n"
               "        \"connection\": \"Ethernet 2\",\n"
               "        \"description\": \"Microsoft KM-TEST Loopback Adapter\",\n"
               "        \"up\": true,\n"
               "        \"connected\": true,\n"
               "        \"loopback\": false,\n"
               "        \"wireless\": false\n"
               "      }\n"
               "    },\n"
               "    {\n"
               "      \"value\": \"\\\\Device\\\\NPF_Loopback\",\n"
               "      \"label\": \"\\\\Device\\\\NPF_Loopback\",\n"
               "      \"detail\": \"up, loopback; Adapter for loopback traffic capture\",\n"
               "      \"usable\": false,\n"
               "      \"reason\": \"loopback: not an Ethernet adapter\",\n"
               "      \"in_use\": false,\n"
               "      \"in_use_by\": [],\n"
               "      \"in_use_count\": 0,\n"
               "      \"ethernet\": {\n"
               "        \"pcap_name\": \"\\\\Device\\\\NPF_Loopback\",\n"
               "        \"connection\": null,\n"
               "        \"description\": \"Adapter for loopback traffic capture\",\n"
               "        \"up\": true,\n"
               "        \"connected\": null,\n"
               "        \"loopback\": true,\n"
               "        \"wireless\": false\n"
               "      }\n"
               "    }\n"
               "  ],\n"
               "  \"notes\": [],\n"
               "  \"error\": null\n"
               "}\n");
}

TEST_CASE(list_json_value_is_the_connection_name_only_where_it_names_one_device)
{
    // Two connection names that differ only in case: --interface needs the exact case, so the
    // value is the pcap name of each. A device without a connection name: its pcap name.
    const auto list = npcap_list({{kNpf, "", "Ethernet 2", kPcapIfUp | kPcapIfDisconnected},
                                  {kNpfOther, "", "ETHERNET 2", kPcapIfUp},
                                  {"\\Device\\NPF_{11111111-2222-3333-4444-555555555555}", "", "", kPcapIfUp}});
    const std::string json = pcap_interfaces_json(Backend::Npcap, list);
    CHECK(contains(json, "\"value\": \"\\\\Device\\\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}\",\n"));
    CHECK(contains(json, "\"value\": \"\\\\Device\\\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}\",\n"));
    CHECK(contains(json, "\"value\": \"\\\\Device\\\\NPF_{11111111-2222-3333-4444-555555555555}\",\n"));
    CHECK(json.find("\"value\": \"Ethernet 2\"") == std::string::npos);
    // down and disconnected can still be used: the detail says it.
    CHECK(contains(json, "\"detail\": \"up, disconnected; \\\\Device\\\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}\",\n"
                         "      \"usable\": true,\n"
                         "      \"reason\": null,\n"));
    CHECK(contains(json, "\"connected\": false,\n"));
}

TEST_CASE(list_json_keeps_names_in_utf8_and_escapes_what_json_needs)
{
    // "Ethernet test" in katakana and kanji, and a description with quotes.
    const std::string japanese = "\xE3\x82\xA4\xE3\x83\xBC\xE3\x82\xB5\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88 \xE8\xA9\xA6\xE9\xA8\x93";
    const auto list = npcap_list({{kNpf, "Adapter \"A\"", japanese, kPcapIfUp | kPcapIfWireless}});
    const std::string json = pcap_interfaces_json(Backend::Npcap, list);
    CHECK(contains(json, "\"value\": \"" + japanese + "\",\n"));
    CHECK(contains(json, "\"connection\": \"" + japanese + "\",\n"));
    CHECK(contains(json, "\"description\": \"Adapter \\\"A\\\"\",\n"));
    CHECK(contains(json, "\"wireless\": true\n"));
}

TEST_CASE(list_json_of_libpcap_does_not_offer_any_or_the_loopback)
{
    const auto list = pcap_interface_list_of({{"enp3s0", "", "", kPcapIfUp | kPcapIfConnected},
                                              {"any", "Pseudo-device that captures on all interfaces", "", kPcapIfUp},
                                              {"lo", "", "", kPcapIfUp | kPcapIfLoopback}},
                                             PcapFlavor::Libpcap, "");
    const std::string json = pcap_interfaces_json(Backend::Pcap, list);
    CHECK(contains(json, "\"backend\": \"pcap\",\n  \"source\": null,\n"));
    CHECK(contains(json, "\"value\": \"enp3s0\",\n      \"label\": \"enp3s0\",\n      \"detail\": \"up, connected\",\n"
                         "      \"usable\": true,\n"));
    CHECK(contains(json, "\"value\": \"any\",\n      \"label\": \"any\",\n"
                         "      \"detail\": \"up; Pseudo-device that captures on all interfaces\",\n"
                         "      \"usable\": false,\n      \"reason\": \"not Ethernet\",\n"));
    CHECK(contains(json, "\"value\": \"lo\",\n      \"label\": \"lo\",\n      \"detail\": \"up, loopback\",\n"
                         "      \"usable\": false,\n      \"reason\": \"loopback: not an Ethernet adapter\",\n"));
    CHECK(contains(json, "\"notes\": [],\n"));
}

TEST_CASE(list_json_of_npcap_with_the_loopback_alone_has_the_note)
{
    const auto list = npcap_list({{"\\Device\\NPF_Loopback", "Adapter for loopback traffic capture", "", kPcapIfUp | kPcapIfLoopback}});
    CHECK(contains(pcap_interfaces_json(Backend::Npcap, list),
                   "  \"notes\": [\n"
                   "    \"Npcap offers no network adapter here: if it was installed for administrators only, run this from a "
                   "console started as administrator, or install Npcap again without that limit. A disabled adapter is not "
                   "offered either: enable it in Windows.\"\n"
                   "  ],\n"
                   "  \"error\": null\n"));
    CHECK(contains(pcap_interfaces_json(Backend::Npcap, npcap_list({})),
                   "  \"targets\": [],\n"
                   "  \"notes\": [\n"
                   "    \"If Npcap was installed for administrators only, run this from a console started as administrator.\"\n"
                   "  ],\n"));
}

TEST_CASE(list_json_of_the_os_interfaces_does_not_offer_what_is_not_ethernet)
{
    const std::vector<InterfaceInfo> interfaces = {{"lo", false, 772, true}, {"enp3s0", true, 1, false}};
    CHECK_TEXT(os_interfaces_json(Backend::Packet, interfaces),
               "{\n"
               "  \"vehicle_sil_targets\": 1,\n"
               "  \"tool\": \"vehicle_sil_ethernet_bridge\",\n"
               "  \"backend\": \"packet\",\n"
               "  \"source\": null,\n"
               "  \"targets\": [\n"
               "    {\n"
               "      \"value\": \"lo\",\n"
               "      \"label\": \"lo\",\n"
               "      \"detail\": \"not Ethernet (link type 772), up\",\n"
               "      \"usable\": false,\n"
               "      \"reason\": \"not Ethernet (link type 772)\",\n"
               "      \"in_use\": false,\n"
               "      \"in_use_by\": [],\n"
               "      \"in_use_count\": 0,\n"
               "      \"ethernet\": {\n"
               "        \"name\": \"lo\",\n"
               "        \"ethernet\": false,\n"
               "        \"link_type\": 772,\n"
               "        \"up\": true\n"
               "      }\n"
               "    },\n"
               "    {\n"
               "      \"value\": \"enp3s0\",\n"
               "      \"label\": \"enp3s0\",\n"
               "      \"detail\": \"Ethernet, down\",\n"
               "      \"usable\": true,\n"
               "      \"reason\": null,\n"
               "      \"in_use\": false,\n"
               "      \"in_use_by\": [],\n"
               "      \"in_use_count\": 0,\n"
               "      \"ethernet\": {\n"
               "        \"name\": \"enp3s0\",\n"
               "        \"ethernet\": true,\n"
               "        \"link_type\": 1,\n"
               "        \"up\": false\n"
               "      }\n"
               "    }\n"
               "  ],\n"
               "  \"notes\": [],\n"
               "  \"error\": null\n"
               "}\n");
    CHECK(contains(os_interfaces_json(Backend::Tap, {}), "\"backend\": \"tap\",\n  \"source\": null,\n  \"targets\": [],\n"));
}

TEST_CASE(list_json_of_an_error_has_its_code_and_the_message)
{
    const LinkError missing(LinkErrorKind::LibraryMissing, "Npcap is not installed: cannot load wpcap.dll. Install it.");
    CHECK_TEXT(interface_list_error_json(Backend::Npcap, missing),
               "{\n"
               "  \"vehicle_sil_targets\": 1,\n"
               "  \"tool\": \"vehicle_sil_ethernet_bridge\",\n"
               "  \"backend\": \"npcap\",\n"
               "  \"source\": null,\n"
               "  \"targets\": [],\n"
               "  \"notes\": [],\n"
               "  \"error\": {\n"
               "    \"code\": \"library-missing\",\n"
               "    \"message\": \"Npcap is not installed: cannot load wpcap.dll. Install it.\"\n"
               "  }\n"
               "}\n");
    CHECK_TEXT(interface_list_error_code(LinkError(LinkErrorKind::PermissionDenied, "x")), "permission-denied");
    CHECK_TEXT(interface_list_error_code(LinkError(LinkErrorKind::NoSuchInterface, "x")), "failed");
    CHECK_TEXT(interface_list_error_code(LinkError(LinkErrorKind::Other, "x")), "failed");
    CHECK_TEXT(interface_list_error_code(BackendUnavailableError("x")), "backend-unavailable");
    CHECK_TEXT(interface_list_error_code(std::runtime_error("x")), "failed");
    // Where no backend works on the OS, there is none to name.
    CHECK(contains(interface_list_error_json(std::nullopt, BackendUnavailableError("there is no interface to list here")),
                   "  \"backend\": null,\n"));
}

TEST_CASE(list_says_which_interfaces_vehicle_sil_bridges_use)
{
    using vehicle_sil::bridge_runtime::DevicePeers;
    const std::vector<InterfaceInfo> interfaces{{"eth0", true, 1, true}, {"eth1", true, 1, true}};
    const std::vector<DevicePeers> inUse{{2, {4321, 4400}}};
    const std::string json = os_interfaces_json(Backend::Packet, interfaces, inUse);
    CHECK(contains(json, "      \"detail\": \"Ethernet, up; in use by 2 vehicle-sil bridges (processes 4321, 4400)\",\n"
                         "      \"usable\": true,\n"
                         "      \"reason\": null,\n"
                         "      \"in_use\": true,\n"
                         "      \"in_use_by\": [\n"
                         "        4321,\n"
                         "        4400\n"
                         "      ],\n"
                         "      \"in_use_count\": 2,\n"));
    CHECK(contains(json, "      \"detail\": \"Ethernet, up\",\n"
                         "      \"usable\": true,\n"
                         "      \"reason\": null,\n"
                         "      \"in_use\": false,\n"
                         "      \"in_use_by\": [],\n"
                         "      \"in_use_count\": 0,\n"));
    CHECK(contains(format_os_interface_list(interfaces, inUse),
                   "  --interface eth0  Ethernet, up; in use by 2 vehicle-sil bridges (processes 4321, 4400)\n"
                   "  --interface eth1  Ethernet, up\n"));

    // Windows: the number of bridges without their processes.
    const auto list = npcap_list({{kNpf, "Intel(R) Ethernet", "Ethernet 2", kPcapIfUp | kPcapIfConnected}});
    const std::vector<DevicePeers> windows{{1, {}}};
    CHECK(contains(pcap_interfaces_json(Backend::Npcap, list, windows),
                   "\\\\Device\\\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}; in use by a vehicle-sil bridge\",\n"
                   "      \"usable\": true,\n"
                   "      \"reason\": null,\n"
                   "      \"in_use\": true,\n"
                   "      \"in_use_by\": [],\n"
                   "      \"in_use_count\": 1,\n"));
    CHECK(contains(format_pcap_list(list, windows), "(Intel(R) Ethernet; \\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}); "
                                                    "in use by a vehicle-sil bridge\n"));
}
