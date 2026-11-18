// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include "vehicle_sil/socketcan/bridge_options.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace vehicle_sil::socketcan;

BridgeOptions parse(std::vector<std::string_view> args)
{
    return parse_bridge_options(args);
}

bool usage_error(std::vector<std::string_view> args, const std::string& containing)
{
    return vehicle_sil::unit::throws_with<BridgeUsageError>([&] { (void)parse(std::move(args)); }, containing);
}

} // namespace

TEST_CASE(options_full_command_line)
{
    const auto options = parse({"--registry-uri", "silkit://127.0.0.1:8500", "--participant", "Bridge1", "--network", "CAN2",
                                "--interface", "can0", "--bitrate", "500000", "--tx-timeout-ms", "200", "--stats-every-ms", "500",
                                "--start-timeout-ms", "3000"});
    CHECK(options.registry_uri == std::string("silkit://127.0.0.1:8500"));
    CHECK(options.participant == std::string("Bridge1"));
    CHECK(options.network == std::string("CAN2"));
    CHECK(options.interface_name == std::string("can0"));
    CHECK(options.bitrate == std::uint32_t{500000});
    CHECK(options.tx_timeout == std::chrono::milliseconds(200));
    CHECK(options.statistics_interval == std::chrono::milliseconds(500));
    CHECK(options.start_timeout == std::chrono::milliseconds(3000));
}

TEST_CASE(options_defaults)
{
    const auto options = parse({"--registry-uri", "silkit://x:1", "--interface", "vcan0"});
    CHECK(options.participant == std::string("SocketCanBridge"));
    CHECK(options.network == std::string("J1939_CAN1"));
    CHECK(!options.bitrate);
    CHECK(!options.tx_timeout);
    CHECK(!options.statistics_interval);
}

TEST_CASE(options_name_and_participant_are_one_option)
{
    CHECK(parse({"--registry-uri", "r", "--interface", "can0", "--name", "A"}).participant == std::string("A"));
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--name", "A", "--participant", "B"}, "the same option"));
}

TEST_CASE(options_need_registry_and_interface)
{
    CHECK(usage_error({"--interface", "can0"}, "missing --registry-uri"));
    CHECK(usage_error({"--registry-uri", "r"}, "missing --interface (see --list-channels)"));
    CHECK(usage_error({"--registry-uri", "r", "--interface", "a_name_longer_than_fifteen"}, "longer than 15"));
}

TEST_CASE(options_describe_and_list_need_no_registry)
{
    CHECK(parse({"--describe", "--interface", "can0"}).describe);
    const auto list = parse({"--list-channels", "--json"});
    CHECK(list.list_channels && list.json);
    CHECK(usage_error({"--list-channels", "--describe"}, "exclude each other"));
    CHECK(usage_error({"--json", "--registry-uri", "r", "--interface", "can0"}, "--json is for --list-channels"));
}

TEST_CASE(options_bounds)
{
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--bitrate", "14999"}, "between 15000 and 1000000"));
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--bitrate", "1000001"}, "invalid value for --bitrate"));
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--tx-timeout-ms", "99"}, "between 100 and 60000"));
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--stats-every-ms", "99"}, "between 100 and 3600000"));
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--start-timeout-ms", "0"}, "between 1 and"));
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--bitrate", "fast"}, "invalid value for --bitrate: fast"));
}

TEST_CASE(options_unknown_and_missing_values)
{
    CHECK(usage_error({"--registry-uri", "r", "--interface", "can0", "--xl-target", "virtual"}, "unknown option: --xl-target"));
    CHECK(usage_error({"--registry-uri", "r", "--interface"}, "missing value for --interface"));
    CHECK(usage_error({"can0"}, "unexpected argument: can0"));
}

TEST_CASE(options_help)
{
    CHECK(parse({"--help"}).help);
    CHECK(bridge_usage().find("--interface <name>") != std::string::npos);
}
