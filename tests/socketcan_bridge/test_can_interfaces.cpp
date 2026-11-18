// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include "vehicle_sil/socketcan/can_interfaces.hpp"
#include "vehicle_sil/socketcan/error_frame.hpp"

#include <linux/can/error.h>
#include <linux/can/netlink.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::socketcan;
using Bytes = std::vector<std::uint8_t>;
using Level = vehicle_sil::can_bridge::ExternalBusState::Level;

void pad(Bytes& bytes)
{
    while (bytes.size() % 4 != 0)
    {
        bytes.push_back(0);
    }
}

template <typename T>
void put(Bytes& out, const T& value)
{
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

/// One attribute: length, type, payload, padding.
Bytes attribute(std::uint16_t type, const Bytes& payload)
{
    Bytes out;
    put(out, static_cast<std::uint16_t>(sizeof(rtattr) + payload.size()));
    put(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    pad(out);
    return out;
}

Bytes text(const std::string& s)
{
    Bytes out(s.begin(), s.end());
    out.push_back(0);
    return out;
}

Bytes u32(std::uint32_t value)
{
    Bytes out;
    put(out, value);
    return out;
}

Bytes cat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const auto& part : parts)
    {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

Bytes message(std::uint16_t type, const Bytes& body)
{
    Bytes out;
    nlmsghdr header{};
    header.nlmsg_len = static_cast<std::uint32_t>(NLMSG_HDRLEN + body.size());
    header.nlmsg_type = type;
    put(out, header);
    out.insert(out.end(), body.begin(), body.end());
    pad(out);
    return out;
}

/// RTM_NEWLINK of one interface. `kind` empty: no linkinfo.
Bytes link_message(const std::string& name, unsigned short type, int index, bool up, const std::string& kind,
                   const Bytes& data = {})
{
    ifinfomsg info{};
    info.ifi_family = AF_UNSPEC;
    info.ifi_type = type;
    info.ifi_index = index;
    info.ifi_flags = up ? IFF_UP : 0;
    Bytes body;
    put(body, info);
    const Bytes name_attribute = attribute(IFLA_IFNAME, text(name));
    body.insert(body.end(), name_attribute.begin(), name_attribute.end());
    if (!kind.empty())
    {
        Bytes inner = attribute(IFLA_INFO_KIND, text(kind));
        if (!data.empty())
        {
            const Bytes info_data = attribute(static_cast<std::uint16_t>(IFLA_INFO_DATA | NLA_F_NESTED), data);
            inner.insert(inner.end(), info_data.begin(), info_data.end());
        }
        const Bytes linkinfo = attribute(static_cast<std::uint16_t>(IFLA_LINKINFO | NLA_F_NESTED), inner);
        body.insert(body.end(), linkinfo.begin(), linkinfo.end());
    }
    return message(RTM_NEWLINK, body);
}

Bytes can_data(std::uint32_t bitrate, std::uint32_t state, std::uint32_t restart_ms)
{
    can_bittiming timing{};
    timing.bitrate = bitrate;
    Bytes timing_bytes;
    put(timing_bytes, timing);
    return cat({attribute(IFLA_CAN_BITTIMING, timing_bytes), attribute(IFLA_CAN_STATE, u32(state)),
                attribute(IFLA_CAN_RESTART_MS, u32(restart_ms))});
}

CanInterfaceInfo vcan(bool up = true)
{
    CanInterfaceInfo info;
    info.name = "vcan0";
    info.index = 3;
    info.kind = "vcan";
    info.up = up;
    return info;
}

CanInterfaceInfo adapter(std::optional<std::uint32_t> bitrate, bool up = true, std::uint32_t restart_ms = 100)
{
    CanInterfaceInfo info;
    info.name = "can0";
    info.index = 4;
    info.up = up;
    info.bitrate = bitrate;
    info.state = "error-active";
    info.restart_ms = restart_ms;
    return info;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::array<std::uint8_t, 8> data_of(std::initializer_list<std::pair<int, std::uint8_t>> bytes)
{
    std::array<std::uint8_t, 8> data{};
    for (const auto& [index, value] : bytes)
    {
        data[static_cast<std::size_t>(index)] = value;
    }
    return data;
}

} // namespace

TEST_CASE(netlink_reads_vcan_and_adapters_and_leaves_other_links_out)
{
    const Bytes datagram = cat({link_message("eth0", ARPHRD_ETHER, 2, true, ""), link_message("vcan0", ARPHRD_CAN, 3, true, "vcan"),
                                link_message("can0", ARPHRD_CAN, 4, false, "can", can_data(250000, CAN_STATE_BUS_OFF, 100)),
                                message(NLMSG_DONE, Bytes(4, 0))});
    const auto result = parse_link_messages(datagram);
    CHECK(result.done);
    CHECK(result.error.empty());
    CHECK_EQ(result.interfaces.size(), std::size_t{2});
    CHECK(result.interfaces[0] == vcan());
    const auto& can0 = result.interfaces[1];
    CHECK(can0.name == std::string("can0") && can0.kind == std::string("can") && can0.index == 4u);
    CHECK(!can0.up);
    CHECK(can0.bitrate == std::uint32_t{250000});
    CHECK(can0.state == std::string("bus-off"));
    CHECK(can0.restart_ms == std::uint32_t{100});
}

TEST_CASE(netlink_adapter_without_bit_rate_or_linkinfo)
{
    const Bytes datagram = cat({link_message("can1", ARPHRD_CAN, 5, true, "can", can_data(0, CAN_STATE_STOPPED, 0)),
                                link_message("slcan0", ARPHRD_CAN, 6, true, "")});
    const auto result = parse_link_messages(datagram);
    CHECK(!result.done);
    CHECK_EQ(result.interfaces.size(), std::size_t{2});
    CHECK(!result.interfaces[0].bitrate);
    CHECK(result.interfaces[0].state == std::string("stopped"));
    CHECK(result.interfaces[0].restart_ms == std::uint32_t{0});
    CHECK(result.interfaces[1].kind == std::string("can") && result.interfaces[1].state.empty());
}

TEST_CASE(netlink_error_ends_the_answer_with_the_reason)
{
    Bytes body;
    put(body, -EPERM);
    const auto result = parse_link_messages(message(NLMSG_ERROR, body));
    CHECK(result.done);
    CHECK(contains(result.error, "not permitted"));
}

TEST_CASE(netlink_malformed_data_is_left_out)
{
    Bytes truncated = link_message("vcan0", ARPHRD_CAN, 3, true, "vcan");
    truncated.resize(truncated.size() - 6);
    CHECK(parse_link_messages(truncated).interfaces.empty());
    CHECK(parse_link_messages(Bytes{1, 2, 3}).interfaces.empty());
    CHECK(parse_link_messages(Bytes{}).interfaces.empty());
    Bytes zero_length(32, 0);
    CHECK(parse_link_messages(zero_length).interfaces.empty());
}

TEST_CASE(bus_level_of_the_controller_state)
{
    auto info = adapter(250000);
    CHECK(bus_level_of(info) == Level::ErrorActive);
    info.state = "error-warning";
    CHECK(bus_level_of(info) == Level::ErrorWarning);
    info.state = "error-passive";
    CHECK(bus_level_of(info) == Level::ErrorPassive);
    info.state = "bus-off";
    CHECK(bus_level_of(info) == Level::BusOff);
    info.state = "stopped";
    CHECK(bus_level_of(info) == Level::Unknown);
    CHECK(bus_level_of(vcan()) == Level::Unknown);
}

TEST_CASE(check_accepts_an_interface_that_is_up)
{
    const auto check = check_interface(adapter(250000), std::nullopt);
    CHECK(!check.failure && check.warnings.empty());
    CHECK(!check_interface(adapter(250000), 250000).failure);
    CHECK(!check_interface(vcan(), std::nullopt).failure);
}

TEST_CASE(check_refuses_another_bit_rate_with_the_way_to_fix_it)
{
    const auto check = check_interface(adapter(500000), 250000);
    CHECK(check.failure.has_value());
    CHECK(contains(*check.failure, "--bitrate 250000 does not match can0, which has 500000 bit/s"));
    CHECK(contains(*check.failure, "sudo ip link set can0 down && sudo ip link set can0 type can bitrate 250000 && sudo ip link set can0 up"));
    const auto none = check_interface(adapter(std::nullopt), 250000);
    CHECK(none.failure && contains(*none.failure, "no bit rate set"));
}

TEST_CASE(check_notes_a_bit_rate_on_vcan_and_ignores_it)
{
    const auto check = check_interface(vcan(), 250000);
    CHECK(!check.failure);
    CHECK_EQ(check.warnings.size(), std::size_t{1});
    CHECK(contains(check.warnings[0], "--bitrate 250000 is ignored: vcan0 is a virtual CAN interface"));
}

TEST_CASE(check_refuses_an_interface_that_is_down_with_the_way_to_fix_it)
{
    const auto vcan_down = check_interface(vcan(false), std::nullopt);
    CHECK(vcan_down.failure && *vcan_down.failure == "interface vcan0 is down. Bring it up, then start again: sudo ip link set vcan0 up");
    const auto can_down = check_interface(adapter(250000, false), std::nullopt);
    CHECK(can_down.failure && contains(*can_down.failure, "sudo ip link set can0 up"));
    const auto unconfigured = check_interface(adapter(std::nullopt, false), std::nullopt);
    CHECK(unconfigured.failure && contains(*unconfigured.failure, "sudo ip link set can0 type can bitrate <bit/s>"));
}

TEST_CASE(check_warns_where_the_kernel_never_restarts_after_bus_off)
{
    const auto check = check_interface(adapter(250000, true, 0), std::nullopt);
    CHECK(!check.failure);
    CHECK_EQ(check.warnings.size(), std::size_t{1});
    CHECK(contains(check.warnings[0], "restart-ms 0"));
    CHECK(contains(check.warnings[0], "sudo ip link set can0 type can restart-ms 100"));
    CHECK(check_interface(adapter(250000, true, 100), std::nullopt).warnings.empty());
    CHECK(check_interface(vcan(), std::nullopt).warnings.empty());
}

TEST_CASE(missing_interface_names_the_ones_there_are)
{
    CHECK(missing_interface_message("can9", {vcan(), adapter(250000)}) ==
          "there is no CAN interface 'can9' on this computer; there are: vcan0 can0 (--list-channels tells more)");
    CHECK(contains(missing_interface_message("can9", {}), "sudo modprobe vcan"));
}

TEST_CASE(list_text_says_kind_bit_rate_state_and_use)
{
    const auto in_use = [](const CanInterfaceInfo& info) {
        vehicle_sil::bridge_runtime::DevicePeers peers;
        if (info.name == "vcan0")
        {
            peers.count = 1;
            peers.pids = {4321};
        }
        return peers;
    };
    CHECK(format_interface_list({vcan(), adapter(250000), adapter(std::nullopt, false)}, in_use) ==
          "CAN interfaces (--interface):\n"
          "  vcan0  virtual CAN, up; in use by a vehicle-sil bridge (process 4321)\n"
          "  can0  CAN, up, 250000 bit/s, error-active\n"
          "  can0  CAN, down, no bit rate set, error-active\n");
    CHECK(contains(format_interface_list({}), "(none;"));
}

TEST_CASE(list_json_has_the_form_of_the_other_lists)
{
    const auto in_use = [](const CanInterfaceInfo& info) {
        vehicle_sil::bridge_runtime::DevicePeers peers;
        if (info.name == "can0")
        {
            peers.count = 2;
            peers.pids = {10, 20};
        }
        return peers;
    };
    const std::string json = interfaces_json({vcan(), adapter(250000, false)}, in_use);
    CHECK(json.back() == '\n');
    CHECK(contains(json, "\"vehicle_sil_targets\": 1"));
    CHECK(contains(json, "\"tool\": \"vehicle_sil_socketcan_bridge\""));
    CHECK(contains(json, "\"value\": \"vcan0\""));
    CHECK(contains(json, "\"value\": \"can0\""));
    // An interface that is down is usable, and the detail says down.
    CHECK(contains(json, "\"detail\": \"CAN, down, 250000 bit/s, error-active; in use by 2 vehicle-sil bridges (processes 10, 20)\""));
    CHECK(contains(json, "\"usable\": true"));
    CHECK(!contains(json, "\"usable\": false"));
    CHECK(contains(json, "\"in_use_count\": 2"));
    CHECK(contains(json, "\"kind\": \"vcan\""));
    CHECK(contains(json, "\"bitrate\": 250000"));
    CHECK(contains(json, "\"state\": \"error-active\""));
    CHECK(contains(json, "\"restart_ms\": 100"));
    CHECK(contains(json, "\"up\": false"));
    CHECK(contains(json, "\"error\": null"));
}

TEST_CASE(list_json_error_has_no_targets)
{
    const std::string json = interfaces_error_json("failed", "the kernel did not answer");
    CHECK(contains(json, "\"targets\": []"));
    CHECK(contains(json, "\"code\": \"failed\""));
    CHECK(contains(json, "\"message\": \"the kernel did not answer\""));
}

TEST_CASE(error_frame_bus_off_and_restart)
{
    const auto off = interpret_error_frame(kErrorFlag | CAN_ERR_BUSOFF, {});
    CHECK(off.level && *off.level == Level::BusOff);
    const auto restarted = interpret_error_frame(kErrorFlag | CAN_ERR_RESTARTED, {});
    CHECK(restarted.level && *restarted.level == Level::ErrorActive);
    // Bus-off wins over a controller flag in the same frame.
    const auto both = interpret_error_frame(kErrorFlag | CAN_ERR_BUSOFF | CAN_ERR_CRTL, data_of({{1, CAN_ERR_CRTL_TX_PASSIVE}}));
    CHECK(both.level && *both.level == Level::BusOff);
}

TEST_CASE(error_frame_controller_states)
{
    const auto passive_tx = interpret_error_frame(kErrorFlag | CAN_ERR_CRTL, data_of({{1, CAN_ERR_CRTL_TX_PASSIVE}}));
    CHECK(passive_tx.level && *passive_tx.level == Level::ErrorPassive);
    const auto passive_rx = interpret_error_frame(kErrorFlag | CAN_ERR_CRTL, data_of({{1, CAN_ERR_CRTL_RX_PASSIVE}}));
    CHECK(passive_rx.level && *passive_rx.level == Level::ErrorPassive);
    const auto warning = interpret_error_frame(kErrorFlag | CAN_ERR_CRTL, data_of({{1, CAN_ERR_CRTL_RX_WARNING}}));
    CHECK(warning.level && *warning.level == Level::ErrorWarning);
    const auto active = interpret_error_frame(kErrorFlag | CAN_ERR_CRTL, data_of({{1, CAN_ERR_CRTL_ACTIVE}}));
    CHECK(active.level && *active.level == Level::ErrorActive);
    // A controller problem that is not a state (overflow) says nothing of the state.
    const auto overflow = interpret_error_frame(kErrorFlag | CAN_ERR_CRTL, data_of({{1, CAN_ERR_CRTL_RX_OVERFLOW}}));
    CHECK(!overflow.level);
}

TEST_CASE(error_frame_without_state_and_with_counters)
{
    CHECK(!interpret_error_frame(kErrorFlag | CAN_ERR_BUSERROR, {}).level);
    CHECK(!interpret_error_frame(kErrorFlag | CAN_ERR_LOSTARB, {}).level);
    const auto counted = interpret_error_frame(kErrorFlag | CAN_ERR_CRTL | CAN_ERR_CNT,
                                               data_of({{1, CAN_ERR_CRTL_TX_PASSIVE}, {6, 128}, {7, 3}}));
    CHECK(counted.tx_errors == 128 && counted.rx_errors == 3);
    // Without CAN_ERR_CNT the bytes are not counters.
    const auto uncounted = interpret_error_frame(kErrorFlag | CAN_ERR_CRTL, data_of({{1, CAN_ERR_CRTL_TX_PASSIVE}, {6, 128}}));
    CHECK(uncounted.tx_errors == 0);
}
