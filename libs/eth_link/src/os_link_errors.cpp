// SPDX-License-Identifier: MIT
#include "os_link_errors.hpp"

#include <cerrno>
#include <system_error>

namespace vehicle_sil::eth_link::detail {
namespace {

std::string quoted(std::string_view name)
{
    return "'" + std::string(name) + "'";
}

bool is_permission(int error) noexcept
{
    return error == EPERM || error == EACCES;
}

/// "'sudo ip tuntap add dev <name> mode tap user $USER' and 'sudo ip link set <name> up'"
std::string make_tap_commands(std::string_view name)
{
    return "'sudo ip tuntap add dev " + std::string(name) + " mode tap user $USER' and 'sudo ip link set " +
           std::string(name) + " up'";
}

} // namespace

std::string error_text(int error)
{
    return std::generic_category().message(error);
}

std::string join_names(std::span<const std::string> names)
{
    if (names.empty())
    {
        return "none";
    }
    std::string text;
    for (const auto& name : names)
    {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

LinkError bad_interface_name(std::string_view name)
{
    return {LinkErrorKind::NoSuchInterface, "interface name " + quoted(name) + " cannot be used: Linux interface names have 1 to " +
                                                std::to_string(kMaxInterfaceNameBytes) +
                                                " characters. Give the name as --list-interfaces prints it."};
}

LinkError no_packet_interface(std::string_view name, std::span<const std::string> names)
{
    return {LinkErrorKind::NoSuchInterface, "no interface " + quoted(name) + " for --backend packet. Interfaces here: " +
                                                join_names(names) + " (run --list-interfaces)."};
}

LinkError no_tap_device(std::string_view name, std::span<const std::string> names)
{
    return {LinkErrorKind::NoSuchInterface,
            "no TAP device " + quoted(name) + ". Make it first: " + make_tap_commands(name) +
                "; or let the bridge make it while it runs with --tap-create (needs CAP_NET_ADMIN). Interfaces here: " +
                join_names(names) + "."};
}

LinkError not_ethernet(std::string_view name, unsigned link_type)
{
    return {LinkErrorKind::NotEthernet, "interface " + quoted(name) + " is not Ethernet (link type " + std::to_string(link_type) +
                                            "): --backend packet carries Ethernet frames only. Choose an Ethernet "
                                            "interface (run --list-interfaces)."};
}

LinkError packet_open_failed(std::string_view name, int error, std::string_view step, std::string_view executable)
{
    if (is_permission(error))
    {
        return {LinkErrorKind::PermissionDenied,
                "cannot open a packet socket on " + quoted(name) + ": " + error_text(error) +
                    ". A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep " +
                    (executable.empty() ? std::string("<this program>") : std::string(executable)) + "' once, or run as root."};
    }
    return {LinkErrorKind::Other,
            "cannot open a packet socket on " + quoted(name) + " (" + std::string(step) + "): " + error_text(error) + "."};
}

LinkError tun_open_failed(int error)
{
    const std::string what = "cannot open /dev/net/tun: " + error_text(error) + ". ";
    if (is_permission(error))
    {
        return {LinkErrorKind::PermissionDenied, what + "This user may not use TAP devices here: ask the administrator for read "
                                                        "and write access to /dev/net/tun, or run as root."};
    }
    if (error == ENOENT || error == ENODEV)
    {
        return {LinkErrorKind::Other, what + "The kernel's TUN/TAP driver is not there: load it with 'sudo modprobe tun'."};
    }
    return {LinkErrorKind::Other, what + "A TAP device is opened through it."};
}

LinkError tap_attach_failed(std::string_view name, int error)
{
    if (error == EINVAL)
    {
        return {LinkErrorKind::Other, "interface " + quoted(name) +
                                          " is not a TAP device. For a network interface use --backend packet; a TAP device is "
                                          "made with 'sudo ip tuntap add dev <name> mode tap user $USER'."};
    }
    const std::string what = "cannot attach to TAP " + quoted(name) + ": " + error_text(error) + ".";
    if (is_permission(error))
    {
        return {LinkErrorKind::PermissionDenied, what + " It belongs to another user. Make it yours first: 'sudo ip tuntap del dev " +
                                                     std::string(name) + " mode tap', then " + make_tap_commands(name) + "."};
    }
    if (error == EBUSY)
    {
        return {LinkErrorKind::Other, what + " Another program has it open, and a TAP device serves one program: stop that "
                                             "program, or use another TAP device."};
    }
    return {LinkErrorKind::Other, what};
}

LinkError tap_exists(std::string_view name)
{
    return {LinkErrorKind::Other, "cannot create TAP " + quoted(name) +
                                      ": an interface with that name exists. Leave out --tap-create to attach to it, or choose "
                                      "another name."};
}

LinkError tap_create_failed(std::string_view name, int error)
{
    const std::string what = "cannot create TAP " + quoted(name) + ": " + error_text(error) + ".";
    if (is_permission(error))
    {
        return {LinkErrorKind::PermissionDenied, what + " Creating a TAP device needs CAP_NET_ADMIN: make it first with " +
                                                     make_tap_commands(name) + " and leave out --tap-create, or run as root."};
    }
    return {LinkErrorKind::Other, what};
}

std::string interface_down_warning(std::string_view name)
{
    return "interface " + quoted(name) + " is down: no frame passes until it is up. Bring it up with 'sudo ip link set " +
           std::string(name) + " up'.";
}

std::string checksum_incomplete_warning(std::string_view name, bool sent_here)
{
    std::string what = "interface " + quoted(name) + ": ";
    if (sent_here)
    {
        what += "a frame this computer's OS sent on it has no TCP/UDP checksum yet: the OS leaves the checksum to the "
                "network adapter (checksum offload), and the bridge sees the frame before that. ";
    }
    else
    {
        what += "a frame received on it has no TCP/UDP checksum: this computer's OS sent it through a virtual interface "
                "(the other end of a veth pair) and left the checksum to a network adapter that is not there (checksum "
                "offload). ";
    }
    what += "The bridge passes such frames on as they are, and a receiver in SIL Kit "
            "that checks checksums drops them. Turn the offload off ";
    if (sent_here)
    {
        what += "with 'sudo ethtool -K " + std::string(name) + " tx off'";
    }
    else
    {
        what += "on the interface the frames are sent from (for a veth pair the other end, not " + quoted(name) +
                "): 'sudo ethtool -K <that interface> tx off'";
    }
    what += "; or, if the peer is this computer's OS, use --backend tap instead. This warning is given once.";
    return what;
}

} // namespace vehicle_sil::eth_link::detail
