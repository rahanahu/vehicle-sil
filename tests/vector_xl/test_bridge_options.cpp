// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include "vehicle_sil/vector_xl/bridge_options.hpp"
#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace vehicle_sil::vector_xl;

BridgeOptions parse(std::vector<std::string_view> args)
{
    return parse_bridge_options(args);
}

bool usage_error(std::vector<std::string_view> args)
{
    return vehicle_sil::unit::throws<BridgeUsageError>([&] { (void)parse(std::move(args)); });
}

XlChannelInfo channel(std::string name, bool isVirtual, bool canCapable, unsigned index)
{
    XlChannelInfo info;
    info.name = std::move(name);
    info.is_virtual = isVirtual;
    info.can_capable = canCapable;
    info.hw_type = isVirtual ? 100 : 200; // arbitrary, only passed through
    info.channel_index = index;
    info.channel_mask = std::uint64_t{1} << index;
    return info;
}

std::vector<XlChannelInfo> sample_channels()
{
    return {
        channel("Test CAN Device Channel 1", false, true, 0),
        channel("Virtual Channel 1", true, true, 1),
        channel("Test CAN Device Channel 3", false, false, 2), // LIN only
        channel("Virtual Channel 2", true, true, 3),
        channel("CANcaseXL Channel 1", false, true, 4),
    };
}

} // namespace

TEST_CASE(options_full_command_line)
{
    const auto options = parse({"--registry-uri", "silkit://127.0.0.1:8500", "--participant", "Bridge1", "--network",
                                "CAN2", "--xl-target", "physical", "--xl-channel", "2", "--bitrate", "500000",
                                "--rx-queue-size", "1024", "--tx-timeout-ms", "200", "--bus-off-restart-ms", "0"});
    CHECK(options.registry_uri == std::string("silkit://127.0.0.1:8500"));
    CHECK(options.participant == std::string("Bridge1"));
    CHECK(options.network == std::string("CAN2"));
    CHECK(options.target == XlTarget::Physical);
    CHECK_EQ(options.channel, 2u);
    CHECK(options.bitrate.has_value());
    CHECK_EQ(*options.bitrate, 500000u);
    CHECK_EQ(options.rx_queue_size, 1024u);
    CHECK(options.tx_timeout == std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(200)));
    CHECK(options.bus_monitor);
    CHECK(options.bus_off_restart == std::optional<std::chrono::milliseconds>(std::chrono::milliseconds(0)));
    CHECK(!options.list_channels);
}

TEST_CASE(options_defaults_and_special_modes)
{
    const auto options = parse({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1"});
    CHECK(options.target == XlTarget::Virtual);
    CHECK(options.participant == std::string("VectorXlBridge"));
    CHECK(options.network == std::string("CAN"));
    CHECK(!options.bitrate.has_value());
    CHECK(!options.bus_off_restart.has_value());
    CHECK_EQ(options.rx_queue_size, 16384u);
    CHECK(!options.tx_timeout.has_value());

    CHECK(!parse({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--no-bus-monitor"}).bus_monitor);
    CHECK(parse({"--list-channels"}).list_channels); // needs nothing else, not even --network
    CHECK(parse({"--help"}).help);
}

TEST_CASE(options_need_network_there_is_no_default)
{
    const auto missing = [](std::vector<std::string_view> args) {
        return vehicle_sil::unit::throws_with<BridgeUsageError>(
            [&] { (void)parse(std::move(args)); },
            "missing --network: name the SIL Kit CAN network to bridge (there is no default");
    };
    CHECK(missing({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1"}));
    CHECK(missing({"--describe", "--xl-target", "virtual", "--xl-channel", "1"}));
    CHECK(missing({"--registry-uri", "u", "--network", "", "--xl-target", "virtual", "--xl-channel", "1"}));
    CHECK(bridge_usage().find("(no default)") != std::string::npos);
    CHECK(bridge_usage().find("J1939_CAN1") == std::string::npos);
}

TEST_CASE(options_reject_bad_command_lines)
{
    CHECK(usage_error({}));
    CHECK(usage_error({"--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1"}));                       // no registry
    CHECK(usage_error({"--registry-uri", "u", "--xl-channel", "1"}));                          // no target
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual"}));                     // no channel
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "cancase", "--xl-channel", "1"})); // bad target
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "0"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "x"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "100"}));
    // xlCanSetChannelBitrate accepts 15000 ... 1000000 bit/s.
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "14999"}));
    CHECK(!usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "15000"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "2000000"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--rx-queue-size", "1000"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--rx-queue-size", "65536"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--tx-timeout-ms", "99"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--bus-off-restart-ms", "99"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--bus-off-restart-ms", "60001"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--tx-timeout-ms", "60001"}));
    CHECK(usage_error({"--registry-uri", "u", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--unknown", "1"}));
    CHECK(usage_error({"--registry-uri"}));
    CHECK(usage_error({"positional"}));
}

TEST_CASE(selection_numbers_can_channels_per_target)
{
    const auto channels = sample_channels();
    const auto virtuals = selectable_channels(channels, XlTarget::Virtual);
    CHECK_EQ(virtuals.size(), 2u);
    CHECK_EQ(virtuals[0].number, 1u);
    CHECK(virtuals[0].info.name == std::string("Virtual Channel 1"));
    CHECK(virtuals[1].info.name == std::string("Virtual Channel 2"));

    const auto physical = selectable_channels(channels, XlTarget::Physical);
    CHECK_EQ(physical.size(), 2u); // the LIN-only channel is not offered
    CHECK(physical[0].info.name == std::string("Test CAN Device Channel 1"));
    CHECK(physical[1].info.name == std::string("CANcaseXL Channel 1"));
    CHECK_EQ(physical[1].number, 2u);

    CHECK_EQ(select_channel(channels, XlTarget::Virtual, 2).channel_index, 3u);
    CHECK_EQ(select_channel(channels, XlTarget::Physical, 2).channel_index, 4u);
}

TEST_CASE(selection_error_lists_available_channels)
{
    const auto channels = sample_channels();
    std::string message;
    try
    {
        (void)select_channel(channels, XlTarget::Physical, 3);
    }
    catch (const ChannelSelectionError& e)
    {
        message = e.what();
    }
    CHECK(message.find("no physical CAN channel 3") != std::string::npos);
    CHECK(message.find("1=\"Test CAN Device Channel 1\"") != std::string::npos);
    CHECK(message.find("2=\"CANcaseXL Channel 1\"") != std::string::npos);
    CHECK(message.find("Channel 3") == std::string::npos);

    message.clear();
    try
    {
        (void)select_channel({channel("Virtual Channel 1", true, true, 0)}, XlTarget::Physical, 1);
    }
    catch (const ChannelSelectionError& e)
    {
        message = e.what();
    }
    CHECK(message.find("reports no physical CAN channels") != std::string::npos);
}

TEST_CASE(selection_channel_list_text)
{
    const auto text = format_channel_list(sample_channels());
    CHECK(text.find("virtual CAN channels (--xl-target virtual):") != std::string::npos);
    CHECK(text.find("--xl-channel 2  Virtual Channel 2") != std::string::npos);
    CHECK(text.find("--xl-channel 2  CANcaseXL Channel 1") != std::string::npos);
    CHECK(text.find("Test CAN Device Channel 3") == std::string::npos);
    CHECK(parse_xl_target("virtual") == XlTarget::Virtual);
    CHECK(parse_xl_target("physical") == XlTarget::Physical);
    CHECK(!parse_xl_target("Virtual").has_value());
}

TEST_CASE(options_name_describe_and_start_timeout)
{
    const auto named = parse({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--name",
                              "XlBridge", "--start-timeout-ms", "40000"});
    CHECK(named.participant == std::string("XlBridge"));
    CHECK(named.start_timeout == std::chrono::milliseconds(40000));
    CHECK(!named.describe);
    CHECK(parse({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1"}).start_timeout ==
          std::chrono::milliseconds(15000));
    CHECK(usage_error({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--name", "A",
                       "--participant", "B"}));
    CHECK(usage_error({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1",
                       "--start-timeout-ms", "0"}));
    CHECK(usage_error({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1",
                       "--start-timeout-ms", "1000000000001"}));

    // --describe needs no registry, and checks the rest as a run does.
    const auto described = parse({"--describe", "--name", "XlBridge", "--network", "VehicleCAN", "--xl-target",
                                   "physical", "--xl-channel", "2"});
    CHECK(described.describe);
    CHECK(described.registry_uri.empty());
    CHECK(described.network == std::string("VehicleCAN"));
    CHECK(usage_error({"--describe", "--network", "CAN", "--xl-target", "virtual"}));
    CHECK(parse({"--list-channels", "--json"}).list_channels); // the list forms need no --network
    CHECK(usage_error({"--describe", "--list-channels"}));
}

TEST_CASE(options_stats_every_is_how_often_the_statistics_line_comes_while_running)
{
    const auto args = [](std::string_view every) {
        return std::vector<std::string_view>{"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1",
                                             "--stats-every-ms", every};
    };
    CHECK(!parse({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1"}).statistics_interval);
    CHECK(parse(args("1000")).statistics_interval == std::chrono::milliseconds(1000));
    CHECK(parse(args("100")).statistics_interval == std::chrono::milliseconds(100));
    CHECK(parse(args("3600000")).statistics_interval == std::chrono::milliseconds(3600000));
    for (const std::string_view bad : {"99", "3600001", "0", "-1", "1s", ""})
    {
        std::string message;
        try
        {
            (void)parse(args(bad));
        }
        catch (const BridgeUsageError& e)
        {
            message = e.what();
        }
        CHECK(message == "--stats-every-ms must be between 100 and 3600000, got: " + std::string(bad));
    }
    CHECK(bridge_usage().find("--stats-every-ms <ms>") != std::string::npos);
}

TEST_CASE(options_json_is_for_list_channels_only)
{
    const auto list = parse({"--list-channels", "--json"});
    CHECK(list.list_channels);
    CHECK(list.json);
    CHECK(!parse({"--list-channels"}).json);
    const auto json_alone = [](std::vector<std::string_view> args) {
        return vehicle_sil::unit::throws_with<BridgeUsageError>([&] { (void)parse(std::move(args)); },
                                                                "--json is for --list-channels: it writes the list as JSON");
    };
    CHECK(json_alone({"--json"}));
    CHECK(json_alone({"--registry-uri", "silkit://h:1", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--json"}));
    CHECK(json_alone({"--describe", "--network", "CAN", "--xl-target", "virtual", "--xl-channel", "1", "--json"}));
}

TEST_CASE(selection_channel_list_json_numbers_the_can_channels_of_each_target)
{
    auto channels = sample_channels();
    channels[1].transceiver_name = "Virtual CAN";
    channels[1].hw_type = 1;
    channels[4].serial_number = 4711;
    const std::string json = channels_json(channels);
    CHECK(json ==
          "{\n"
          "  \"vehicle_sil_targets\": 1,\n"
          "  \"tool\": \"vehicle_sil_vector_xl_bridge\",\n"
          "  \"backend\": null,\n"
          "  \"source\": null,\n"
          "  \"targets\": [\n"
          "    {\n"
          "      \"value\": \"virtual:1\",\n"
          "      \"label\": \"Virtual Channel 1\",\n"
          "      \"detail\": \"virtual 1; hwType=1 hwIndex=0 hwChannel=0 serial=0 Virtual CAN\",\n"
          "      \"usable\": true,\n"
          "      \"reason\": null,\n"
          "      \"in_use\": false,\n"
          "      \"in_use_by\": [],\n"
          "      \"in_use_count\": 0,\n"
          "      \"can\": {\n"
          "        \"target\": \"virtual\",\n"
          "        \"number\": 1,\n"
          "        \"name\": \"Virtual Channel 1\",\n"
          "        \"hw_type\": 1,\n"
          "        \"hw_index\": 0,\n"
          "        \"hw_channel\": 0,\n"
          "        \"serial\": 0,\n"
          "        \"transceiver\": \"Virtual CAN\"\n"
          "      }\n"
          "    },\n"
          "    {\n"
          "      \"value\": \"virtual:2\",\n"
          "      \"label\": \"Virtual Channel 2\",\n"
          "      \"detail\": \"virtual 2; hwType=100 hwIndex=0 hwChannel=0 serial=0\",\n"
          "      \"usable\": true,\n"
          "      \"reason\": null,\n"
          "      \"in_use\": false,\n"
          "      \"in_use_by\": [],\n"
          "      \"in_use_count\": 0,\n"
          "      \"can\": {\n"
          "        \"target\": \"virtual\",\n"
          "        \"number\": 2,\n"
          "        \"name\": \"Virtual Channel 2\",\n"
          "        \"hw_type\": 100,\n"
          "        \"hw_index\": 0,\n"
          "        \"hw_channel\": 0,\n"
          "        \"serial\": 0,\n"
          "        \"transceiver\": null\n"
          "      }\n"
          "    },\n"
          "    {\n"
          "      \"value\": \"physical:1\",\n"
          "      \"label\": \"Test CAN Device Channel 1\",\n"
          "      \"detail\": \"physical 1; hwType=200 hwIndex=0 hwChannel=0 serial=0\",\n"
          "      \"usable\": true,\n"
          "      \"reason\": null,\n"
          "      \"in_use\": false,\n"
          "      \"in_use_by\": [],\n"
          "      \"in_use_count\": 0,\n"
          "      \"can\": {\n"
          "        \"target\": \"physical\",\n"
          "        \"number\": 1,\n"
          "        \"name\": \"Test CAN Device Channel 1\",\n"
          "        \"hw_type\": 200,\n"
          "        \"hw_index\": 0,\n"
          "        \"hw_channel\": 0,\n"
          "        \"serial\": 0,\n"
          "        \"transceiver\": null\n"
          "      }\n"
          "    },\n"
          "    {\n"
          "      \"value\": \"physical:2\",\n"
          "      \"label\": \"CANcaseXL Channel 1\",\n"
          "      \"detail\": \"physical 2; hwType=200 hwIndex=0 hwChannel=0 serial=4711\",\n"
          "      \"usable\": true,\n"
          "      \"reason\": null,\n"
          "      \"in_use\": false,\n"
          "      \"in_use_by\": [],\n"
          "      \"in_use_count\": 0,\n"
          "      \"can\": {\n"
          "        \"target\": \"physical\",\n"
          "        \"number\": 2,\n"
          "        \"name\": \"CANcaseXL Channel 1\",\n"
          "        \"hw_type\": 200,\n"
          "        \"hw_index\": 0,\n"
          "        \"hw_channel\": 0,\n"
          "        \"serial\": 4711,\n"
          "        \"transceiver\": null\n"
          "      }\n"
          "    }\n"
          "  ],\n"
          "  \"notes\": [],\n"
          "  \"error\": null\n"
          "}\n");
    // The LIN channel (no CAN) is not counted, and not listed.
    CHECK(json.find("Test CAN Device Channel 3") == std::string::npos);
}

TEST_CASE(selection_channel_list_json_of_no_channel_and_of_an_error)
{
    const std::string empty = channels_json({});
    CHECK(empty.find("  \"targets\": [],\n  \"notes\": [],\n  \"error\": null\n}\n") != std::string::npos);
    CHECK(channels_error_json("driver-missing", "cannot open the Vector XL driver: \"xlOpenDriver\" failed") ==
          "{\n"
          "  \"vehicle_sil_targets\": 1,\n"
          "  \"tool\": \"vehicle_sil_vector_xl_bridge\",\n"
          "  \"backend\": null,\n"
          "  \"source\": null,\n"
          "  \"targets\": [],\n"
          "  \"notes\": [],\n"
          "  \"error\": {\n"
          "    \"code\": \"driver-missing\",\n"
          "    \"message\": \"cannot open the Vector XL driver: \\\"xlOpenDriver\\\" failed\"\n"
          "  }\n"
          "}\n");
}

TEST_CASE(selection_device_identity_is_one_bus_for_virtual_can_and_the_channel_of_a_device_for_hardware)
{
    XlChannelInfo first = channel("Virtual Channel 1", true, true, 0);
    XlChannelInfo second = channel("Virtual Channel 2", true, true, 1);
    second.hw_channel = 1;
    CHECK(device_identity(first) == "xl-virtual");
    CHECK(device_identity(second) == "xl-virtual");
    XlChannelInfo hardware = channel("VN1630A Channel 2", false, true, 3);
    hardware.hw_type = 57;
    hardware.hw_index = 1;
    hardware.serial_number = 4711;
    hardware.hw_channel = 1;
    CHECK(device_identity(hardware) == "xl-57-1-4711-1");
}

TEST_CASE(selection_channel_list_says_which_channels_vehicle_sil_bridges_use)
{
    using vehicle_sil::bridge_runtime::DevicePeers;
    // Every virtual channel is on the one bus of Virtual CAN.
    const ChannelInUse inUse = [](const XlChannelInfo& info) {
        return device_identity(info) == "xl-virtual" ? DevicePeers{1, {4321}} : DevicePeers{};
    };
    const std::string json = channels_json(sample_channels(), inUse);
    const std::string used = "      \"usable\": true,\n"
                             "      \"reason\": null,\n"
                             "      \"in_use\": true,\n"
                             "      \"in_use_by\": [\n"
                             "        4321\n"
                             "      ],\n"
                             "      \"in_use_count\": 1,\n";
    CHECK(json.find("      \"detail\": \"virtual 1; hwType=100 hwIndex=0 hwChannel=0 serial=0; in use by a vehicle-sil bridge "
                    "(process 4321)\",\n" + used) != std::string::npos);
    CHECK(json.find("      \"detail\": \"virtual 2; hwType=100 hwIndex=0 hwChannel=0 serial=0; in use by a vehicle-sil bridge "
                    "(process 4321)\",\n" + used) != std::string::npos);
    CHECK(json.find("      \"detail\": \"physical 1; hwType=200 hwIndex=0 hwChannel=0 serial=0\",\n"
                    "      \"usable\": true,\n"
                    "      \"reason\": null,\n"
                    "      \"in_use\": false,\n"
                    "      \"in_use_by\": [],\n"
                    "      \"in_use_count\": 0,\n") != std::string::npos);
    const std::string text = format_channel_list(sample_channels(), inUse);
    CHECK(text.find("Virtual Channel 2                hwType=100 hwIndex=0 hwChannel=0 serial=0; in use by a vehicle-sil bridge "
                    "(process 4321)\n") != std::string::npos);
    CHECK(text.find("CANcaseXL Channel 1              hwType=200 hwIndex=0 hwChannel=0 serial=0 \n") != std::string::npos);
}
