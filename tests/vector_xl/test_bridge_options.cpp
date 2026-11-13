// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include "vehicle_sil/vector_xl/bridge_options.hpp"
#include "vehicle_sil/vector_xl/channel_selection.hpp"

#include <chrono>
#include <optional>
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
                                "--rx-queue-size", "1024", "--tx-timeout-ms", "200"});
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
    CHECK(!options.list_channels);
}

TEST_CASE(options_defaults_and_special_modes)
{
    const auto options = parse({"--registry-uri", "silkit://h:1", "--xl-target", "virtual", "--xl-channel", "1"});
    CHECK(options.target == XlTarget::Virtual);
    CHECK(options.participant == std::string("VectorXlBridge"));
    CHECK(options.network == std::string("J1939_CAN1"));
    CHECK(!options.bitrate.has_value());
    CHECK_EQ(options.rx_queue_size, 16384u);
    CHECK(!options.tx_timeout.has_value());

    CHECK(!parse({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--no-bus-monitor"}).bus_monitor);
    CHECK(parse({"--list-channels"}).list_channels); // needs nothing else
    CHECK(parse({"--help"}).help);
}

TEST_CASE(options_reject_bad_command_lines)
{
    CHECK(usage_error({}));
    CHECK(usage_error({"--xl-target", "virtual", "--xl-channel", "1"}));                       // no registry
    CHECK(usage_error({"--registry-uri", "u", "--xl-channel", "1"}));                          // no target
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual"}));                     // no channel
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "cancase", "--xl-channel", "1"})); // bad target
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "0"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "x"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "100"}));
    // xlCanSetChannelBitrate accepts 15000 ... 1000000 bit/s.
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "14999"}));
    CHECK(!usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "15000"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--bitrate", "2000000"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--rx-queue-size", "1000"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--rx-queue-size", "65536"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--tx-timeout-ms", "9"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--tx-timeout-ms", "60001"}));
    CHECK(usage_error({"--registry-uri", "u", "--xl-target", "virtual", "--xl-channel", "1", "--unknown", "1"}));
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
