// SPDX-License-Identifier: MIT
//
// The bridge's command line: what it accepts, and the exact message for each mistake.
#include "check_text.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace vehicle_sil::ethernet_bridge;
using Args = std::vector<std::string_view>;

BridgeOptions parse(Args args)
{
    return parse_bridge_options(args);
}

/// The message of the BridgeUsageError `args` cause ("" if none).
std::string usage_error(Args args)
{
    try
    {
        (void)parse(std::move(args));
    }
    catch (const BridgeUsageError& e)
    {
        return e.what();
    }
    return "";
}

/// The arguments every bridge needs, before the ones a test adds.
Args with(Args extra, std::string_view backend = "tap")
{
    Args args = {"--registry-uri", "silkit://127.0.0.1:8500", "--network", "ETH1", "--backend", backend, "--interface", "vsil0"};
    args.insert(args.end(), extra.begin(), extra.end());
    return args;
}

} // namespace

TEST_CASE(options_minimal_tap_bridge_passes_every_frame)
{
    const auto o = parse(with({}));
    CHECK(!o.help);
    CHECK(!o.list_interfaces);
    CHECK_TEXT(o.registry_uri, "silkit://127.0.0.1:8500");
    CHECK_TEXT(o.network, "ETH1");
    CHECK_TEXT(o.participant, "EthernetBridge");
    CHECK(o.backend == Backend::Tap);
    CHECK_TEXT(o.interface_name, "vsil0");
    CHECK(o.bridge.mac_guard);
    CHECK_EQ(o.bridge.mac_table_size, std::size_t{1024});
    CHECK(o.bridge.filter.ether_types.empty());
    CHECK(o.bridge.filter.silkit_macs.empty());
    CHECK_EQ(o.bridge.filter.max_frame_bytes, std::size_t{1522});
    CHECK(o.bridge.filter.vlan_tpids == (std::vector<std::uint16_t>{0x8100, 0x88A8}));
    CHECK(!o.promiscuous);
    CHECK(!o.tap_create);
}

TEST_CASE(options_every_option)
{
    const auto o = parse(with({"--participant", "Gateway", "--ethertype", "0x88B5,0x0800", "--ethertype", "0x0806",
                               "--src-mac", "02:00:00:00:00:01,02-00-00-00-00-02", "--vlan-tpid", "0x9100",
                               "--max-frame-bytes", "9018", "--mac-table-size", "4096", "--no-mac-guard",
                               "--promiscuous"},
                              "packet"));
    CHECK_TEXT(o.participant, "Gateway");
    CHECK(o.backend == Backend::Packet);
    CHECK(o.bridge.filter.ether_types == (std::vector<std::uint16_t>{0x88B5, 0x0800, 0x0806}));
    CHECK_EQ(o.bridge.filter.silkit_macs.size(), std::size_t{2});
    CHECK_TEXT(vehicle_sil::eth_link::to_string(o.bridge.filter.silkit_macs.at(1)), "02:00:00:00:00:02");
    CHECK(o.bridge.filter.vlan_tpids == (std::vector<std::uint16_t>{0x9100}));
    CHECK_EQ(o.bridge.filter.max_frame_bytes, std::size_t{9018});
    CHECK_EQ(o.bridge.mac_table_size, std::size_t{4096});
    CHECK(!o.bridge.mac_guard);
    CHECK(o.promiscuous);

    const auto all = parse(with({"--all-frames"}, "npcap"));
    CHECK(all.all_frames);
    CHECK(parse(with({"--tap-create"})).tap_create);
}

TEST_CASE(options_help_and_list_interfaces_need_nothing_else)
{
    CHECK(parse({"--help"}).help);
    CHECK(parse({"--network", "ETH1", "-h"}).help);
    const auto list = parse({"--list-interfaces"});
    CHECK(list.list_interfaces);
    CHECK(!list.backend.has_value());
    CHECK(parse({"--list-interfaces", "--backend", "pcap"}).backend == Backend::Pcap);
    CHECK(bridge_usage().find("--ethertype <hex>,...") != std::string::npos);
}

TEST_CASE(options_backends)
{
    CHECK_TEXT(to_string(Backend::Npcap), "npcap");
    CHECK(parse_backend("pcap") == Backend::Pcap);
    CHECK(!parse_backend("TAP").has_value());
    CHECK(!reaches_real_adapters(Backend::Tap));
    CHECK(reaches_real_adapters(Backend::Packet));
    CHECK(reaches_real_adapters(Backend::Npcap));
    CHECK(reaches_real_adapters(Backend::Pcap));
}

TEST_CASE(options_a_real_adapter_needs_to_be_told_what_may_pass)
{
    CHECK_TEXT(usage_error(with({}, "npcap")),
               "--backend npcap sends to a real adapter: say what may pass with --ethertype, --src-mac or --all-frames.");
    CHECK_TEXT(usage_error(with({}, "packet")),
               "--backend packet sends to a real adapter: say what may pass with --ethertype, --src-mac or --all-frames.");
    CHECK_TEXT(usage_error(with({"--ethertype", "0x88b5"}, "pcap")), "");
    CHECK_TEXT(usage_error(with({"--src-mac", "02:00:00:00:00:01"}, "pcap")), "");
    CHECK_TEXT(usage_error(with({"--all-frames", "--ethertype", "0x88b5"}, "packet")),
               "--all-frames passes every frame, so it cannot be combined with --ethertype or --src-mac: keep one or "
               "the other");
}

TEST_CASE(options_missing_required_values)
{
    CHECK_TEXT(usage_error({"--network", "ETH1", "--backend", "tap", "--interface", "vsil0"}),
               "missing --registry-uri: give the SIL Kit registry, e.g. --registry-uri silkit://127.0.0.1:8500");
    CHECK_TEXT(usage_error({"--registry-uri", "silkit://127.0.0.1:8500", "--backend", "tap", "--interface", "vsil0"}),
               "missing --network: name the SIL Kit Ethernet network to bridge (there is no default, so that the "
               "bridge never joins a network by mistake)");
    CHECK_TEXT(usage_error({"--registry-uri", "silkit://127.0.0.1:8500", "--network", "ETH1", "--interface", "vsil0"}),
               "missing --backend: choose tap, packet, npcap or pcap");
    CHECK_TEXT(usage_error({"--registry-uri", "silkit://127.0.0.1:8500", "--network", "ETH1", "--backend", "tap"}),
               "missing --interface: name the interface to bridge (run --list-interfaces to see them)");
    CHECK_TEXT(usage_error(with({"--participant", ""})), "--participant must not be empty");
    CHECK_TEXT(usage_error(with({"--interface"})), "missing value for --interface");
}

TEST_CASE(options_bad_values)
{
    CHECK_TEXT(usage_error(with({"--backend", "wpcap"})), "--backend must be tap, packet, npcap or pcap, got: wpcap");
    CHECK_TEXT(usage_error(with({"--ethertype", "88b5"})),
               "invalid value for --ethertype: '88b5': give EtherTypes as hex numbers with 0x, separated by commas, "
               "e.g. 0x88b5,0x0800");
    for (const std::string_view bad : {"0x", "0x10000", "0x88b5,", "0xg", "0x88b5;0x0800"})
    {
        CHECK(usage_error(with({"--ethertype", bad})).starts_with("invalid value for --ethertype"));
    }
    CHECK_TEXT(usage_error(with({"--vlan-tpid", "8100"})),
               "invalid value for --vlan-tpid: '8100': give tag identifiers as hex numbers with 0x, separated by "
               "commas, e.g. 0x8100,0x88a8");
    CHECK_TEXT(usage_error(with({"--src-mac", "02:00:00:00:01"})),
               "invalid value for --src-mac: '02:00:00:00:01': give MAC addresses as six hex bytes, separated by "
               "commas, e.g. 02:00:00:00:00:01,02:00:00:00:00:02");
    CHECK_TEXT(usage_error(with({"--src-mac", "02:00:00:00:00:01,ff:ff:ff:ff:ff:ff"})),
               "--src-mac ff:ff:ff:ff:ff:ff is a group address: give the individual MAC addresses of the SIL Kit side");
    CHECK_TEXT(usage_error(with({"--max-frame-bytes", "59"})), "--max-frame-bytes must be between 60 and 65535, got: 59");
    CHECK_TEXT(usage_error(with({"--max-frame-bytes", "1.5k"})),
               "--max-frame-bytes must be between 60 and 65535, got: 1.5k");
    CHECK_TEXT(usage_error(with({"--mac-table-size", "70000"})),
               "--mac-table-size must be between 16 and 65536, got: 70000");
}

TEST_CASE(options_combinations_that_do_not_apply)
{
    CHECK_TEXT(usage_error(with({"--tap-create", "--all-frames"}, "packet")), "--tap-create needs --backend tap");
    CHECK_TEXT(usage_error(with({"--promiscuous"})),
               "--promiscuous needs --backend packet, npcap or pcap: a TAP device gets every frame the OS sends to it "
               "anyway");
}

TEST_CASE(options_unknown_and_unexpected_arguments)
{
    CHECK_TEXT(usage_error(with({"--xl-channel", "1"})), "unknown option: --xl-channel (see --help)");
    CHECK_TEXT(usage_error(with({"eth0"})), "unexpected argument: eth0 (options start with --; see --help)");
}
