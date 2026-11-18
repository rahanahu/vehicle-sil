// SPDX-License-Identifier: MIT
#include "vehicle_sil/socketcan/can_interfaces.hpp"

#include "vehicle_sil/json/json_writer.hpp"

#include <linux/can/netlink.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <algorithm>
#include <cstring>

namespace vehicle_sil::socketcan {
namespace {

using Bytes = std::span<const std::uint8_t>;

struct Attribute
{
    std::uint16_t type = 0;
    Bytes payload;
};

std::size_t align4(std::size_t n) noexcept
{
    return (n + 3) & ~std::size_t{3};
}

/// The attributes (struct rtattr: length, type, payload, padded to 4 bytes) in `bytes`.
std::vector<Attribute> attributes(Bytes bytes)
{
    std::vector<Attribute> result;
    while (bytes.size() >= sizeof(rtattr))
    {
        rtattr header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.rta_len < sizeof(rtattr) || header.rta_len > bytes.size())
        {
            break;
        }
        result.push_back({static_cast<std::uint16_t>(header.rta_type & NLA_TYPE_MASK),
                          bytes.subspan(sizeof(rtattr), header.rta_len - sizeof(rtattr))});
        const std::size_t step = align4(header.rta_len);
        if (step >= bytes.size())
        {
            break;
        }
        bytes = bytes.subspan(step);
    }
    return result;
}

std::optional<std::uint32_t> as_u32(Bytes payload) noexcept
{
    if (payload.size() < sizeof(std::uint32_t))
    {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, payload.data(), sizeof(value));
    return value;
}

std::string as_text(Bytes payload)
{
    std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (const auto end = text.find('\0'); end != std::string::npos)
    {
        text.resize(end);
    }
    return text;
}

std::string state_text(std::uint32_t state)
{
    switch (state)
    {
    case CAN_STATE_ERROR_ACTIVE: return "error-active";
    case CAN_STATE_ERROR_WARNING: return "error-warning";
    case CAN_STATE_ERROR_PASSIVE: return "error-passive";
    case CAN_STATE_BUS_OFF: return "bus-off";
    case CAN_STATE_STOPPED: return "stopped";
    case CAN_STATE_SLEEPING: return "sleeping";
    default: return "unknown";
    }
}

/// One RTM_NEWLINK message (without its nlmsghdr) as a CAN interface, or nullopt.
std::optional<CanInterfaceInfo> interface_of(Bytes body)
{
    if (body.size() < NLMSG_ALIGN(sizeof(ifinfomsg)))
    {
        return std::nullopt;
    }
    ifinfomsg info{};
    std::memcpy(&info, body.data(), sizeof(info));
    if (info.ifi_type != ARPHRD_CAN)
    {
        return std::nullopt;
    }
    CanInterfaceInfo result;
    result.index = static_cast<unsigned>(info.ifi_index);
    result.up = (info.ifi_flags & IFF_UP) != 0;
    std::string kind;
    for (const Attribute& attribute : attributes(body.subspan(NLMSG_ALIGN(sizeof(ifinfomsg)))))
    {
        if (attribute.type == IFLA_IFNAME)
        {
            result.name = as_text(attribute.payload);
        }
        else if (attribute.type == IFLA_LINKINFO)
        {
            for (const Attribute& inner : attributes(attribute.payload))
            {
                if (inner.type == IFLA_INFO_KIND)
                {
                    kind = as_text(inner.payload);
                }
            }
            if (kind == "can")
            {
                for (const Attribute& inner : attributes(attribute.payload))
                {
                    if (inner.type != IFLA_INFO_DATA)
                    {
                        continue;
                    }
                    for (const Attribute& can : attributes(inner.payload))
                    {
                        if (can.type == IFLA_CAN_BITTIMING && can.payload.size() >= sizeof(can_bittiming))
                        {
                            can_bittiming timing{};
                            std::memcpy(&timing, can.payload.data(), sizeof(timing));
                            if (timing.bitrate != 0)
                            {
                                result.bitrate = timing.bitrate;
                            }
                        }
                        else if (can.type == IFLA_CAN_STATE)
                        {
                            if (const auto state = as_u32(can.payload))
                            {
                                result.state = state_text(*state);
                            }
                        }
                        else if (can.type == IFLA_CAN_BERR_COUNTER && can.payload.size() >= sizeof(can_berr_counter))
                        {
                            can_berr_counter counter{};
                            std::memcpy(&counter, can.payload.data(), sizeof(counter));
                            result.tx_errors = static_cast<std::uint8_t>(std::min<unsigned>(counter.txerr, 255));
                            result.rx_errors = static_cast<std::uint8_t>(std::min<unsigned>(counter.rxerr, 255));
                        }
                        else if (can.type == IFLA_CAN_RESTART_MS)
                        {
                            result.restart_ms = as_u32(can.payload);
                        }
                    }
                }
            }
        }
    }
    if (result.name.empty())
    {
        return std::nullopt;
    }
    // An empty kind is an older driver that does not say; every other name (vcan, vxcan, ...) is kept.
    result.kind = kind.empty() ? "can" : kind;
    return result;
}

std::string bitrate_text(const CanInterfaceInfo& info)
{
    return info.bitrate ? std::to_string(*info.bitrate) + " bit/s" : "no bit rate set";
}

std::string peers_text(const InterfaceInUse& in_use, const CanInterfaceInfo& info, bridge_runtime::DevicePeers& peers)
{
    peers = in_use ? in_use(info) : bridge_runtime::DevicePeers{};
    return bridge_runtime::in_use_text(peers);
}

std::string describe_interface(const CanInterfaceInfo& info)
{
    std::string text = info.is_virtual() ? "virtual CAN" : "CAN";
    text += info.up ? ", up" : ", down";
    if (!info.is_virtual())
    {
        text += ", " + bitrate_text(info);
        if (!info.state.empty())
        {
            text += ", " + info.state;
        }
    }
    return text;
}

} // namespace

can_bridge::ExternalBusState::Level bus_level_of(const CanInterfaceInfo& info) noexcept
{
    using Level = can_bridge::ExternalBusState::Level;
    if (info.state == "error-active")
    {
        return Level::ErrorActive;
    }
    if (info.state == "error-warning")
    {
        return Level::ErrorWarning;
    }
    if (info.state == "error-passive")
    {
        return Level::ErrorPassive;
    }
    if (info.state == "bus-off")
    {
        return Level::BusOff;
    }
    return Level::Unknown;
}

LinkMessages parse_link_messages(Bytes datagram)
{
    LinkMessages result;
    while (datagram.size() >= sizeof(nlmsghdr))
    {
        nlmsghdr header{};
        std::memcpy(&header, datagram.data(), sizeof(header));
        if (header.nlmsg_len < sizeof(nlmsghdr) || header.nlmsg_len > datagram.size())
        {
            break;
        }
        const Bytes body = datagram.subspan(NLMSG_HDRLEN, header.nlmsg_len - NLMSG_HDRLEN);
        if (header.nlmsg_type == NLMSG_DONE)
        {
            result.done = true;
            break;
        }
        if (header.nlmsg_type == NLMSG_ERROR)
        {
            result.done = true;
            int code = 0;
            if (body.size() >= sizeof(int))
            {
                std::memcpy(&code, body.data(), sizeof(code));
            }
            if (code != 0)
            {
                result.error = std::strerror(-code);
                result.error_code = -code;
            }
            break;
        }
        if (header.nlmsg_type == RTM_NEWLINK)
        {
            if (auto info = interface_of(body))
            {
                result.interfaces.push_back(std::move(*info));
            }
        }
        const std::size_t step = NLMSG_ALIGN(header.nlmsg_len);
        if (step >= datagram.size())
        {
            break;
        }
        datagram = datagram.subspan(step);
    }
    return result;
}

std::optional<CanInterfaceInfo> find_can_interface(std::string_view name)
{
    for (auto& info : read_can_interfaces())
    {
        if (info.name == name)
        {
            return std::move(info);
        }
    }
    return std::nullopt;
}

InterfaceCheck check_interface(const CanInterfaceInfo& info, std::optional<std::uint32_t> bitrate)
{
    InterfaceCheck check;
    const std::string& name = info.name;
    if (info.is_virtual())
    {
        if (bitrate)
        {
            check.warnings.push_back("--bitrate " + std::to_string(*bitrate) + " is ignored: " + name +
                                     " is a virtual CAN interface, which has no bit rate");
        }
    }
    else if (bitrate && info.bitrate != bitrate)
    {
        check.failure = "--bitrate " + std::to_string(*bitrate) + " does not match " + name + ", which has " + bitrate_text(info) +
                        ". The bridge sets nothing on an interface (that needs root). Set it, then start again: sudo ip link set " +
                        name + " down && sudo ip link set " + name + " type can bitrate " + std::to_string(*bitrate) +
                        " && sudo ip link set " + name + " up. Without --bitrate the bridge takes the interface as it is.";
        return check;
    }
    if (!info.up)
    {
        check.failure = "interface " + name + " is down. Bring it up, then start again: " +
                        (info.is_virtual() || info.bitrate ? std::string("sudo ip link set ") + name + " up"
                                                           : "sudo ip link set " + name + " type can bitrate <bit/s> && sudo ip link set " + name + " up");
        return check;
    }
    if (!info.is_virtual() && info.restart_ms && *info.restart_ms == 0)
    {
        check.warnings.push_back(name + " has restart-ms 0: after a bus-off it stays off the bus until someone restarts it. Let the kernel restart it: sudo ip link set " +
                                 name + " type can restart-ms 100 (the interface must be down to change it)");
    }
    return check;
}

std::string missing_interface_message(std::string_view name, const std::vector<CanInterfaceInfo>& interfaces)
{
    std::string text = "there is no CAN interface '" + std::string(name) + "' on this computer";
    if (interfaces.empty())
    {
        text += " (there is no CAN interface at all; a virtual one: sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && "
                "sudo ip link set vcan0 up)";
    }
    else
    {
        text += "; there are:";
        for (const auto& info : interfaces)
        {
            text += " " + info.name;
        }
        text += " (--list-channels tells more)";
    }
    return text;
}

std::string format_interface_list(const std::vector<CanInterfaceInfo>& interfaces, const InterfaceInUse& in_use)
{
    if (interfaces.empty())
    {
        return "CAN interfaces (--interface):\n  (none; a virtual one: sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && "
               "sudo ip link set vcan0 up)\n";
    }
    std::string text = "CAN interfaces (--interface):\n";
    for (const auto& info : interfaces)
    {
        text += "  " + info.name + "  " + describe_interface(info);
        bridge_runtime::DevicePeers peers;
        if (const std::string inUse = peers_text(in_use, info, peers); !inUse.empty())
        {
            text += "; " + inUse;
        }
        text += "\n";
    }
    return text;
}

namespace {

void begin_targets(json::JsonWriter& w)
{
    w.begin_object();
    w.integer("vehicle_sil_targets", 1);
    w.string("tool", "vehicle_sil_socketcan_bridge");
    w.null("backend");
    w.null("source");
    w.begin_array("targets");
}

std::string end_targets(json::JsonWriter& w)
{
    std::string out = w.take();
    out += '\n';
    return out;
}

void optional_integer(json::JsonWriter& w, std::string_view key, const std::optional<std::uint32_t>& value)
{
    if (value)
    {
        w.integer(key, *value);
    }
    else
    {
        w.null(key);
    }
}

} // namespace

std::string interfaces_json(const std::vector<CanInterfaceInfo>& interfaces, const InterfaceInUse& in_use)
{
    json::JsonWriter w;
    begin_targets(w);
    for (const auto& info : interfaces)
    {
        bridge_runtime::DevicePeers peers;
        const std::string inUse = peers_text(in_use, info, peers);
        w.begin_object();
        w.string("value", info.name);
        w.string("label", info.name);
        w.string("detail", describe_interface(info) + (inUse.empty() ? "" : "; " + inUse));
        w.boolean("usable", true);
        w.null("reason");
        w.boolean("in_use", peers.count > 0);
        w.begin_array("in_use_by");
        for (const auto pid : peers.pids)
        {
            w.integer("", pid);
        }
        w.end_array();
        w.integer("in_use_count", static_cast<std::int64_t>(peers.count));
        w.begin_object("can");
        w.string("kind", info.kind);
        w.boolean("up", info.up);
        optional_integer(w, "bitrate", info.bitrate);
        if (info.state.empty())
        {
            w.null("state");
        }
        else
        {
            w.string("state", info.state);
        }
        optional_integer(w, "restart_ms", info.restart_ms);
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.begin_array("notes");
    w.end_array();
    w.null("error");
    w.end_object();
    return end_targets(w);
}

std::string interfaces_error_json(std::string_view code, std::string_view message)
{
    json::JsonWriter w;
    begin_targets(w);
    w.end_array();
    w.begin_array("notes");
    w.end_array();
    w.begin_object("error");
    w.string("code", code);
    w.string("message", message);
    w.end_object();
    w.end_object();
    return end_targets(w);
}

} // namespace vehicle_sil::socketcan
