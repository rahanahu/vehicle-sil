// SPDX-License-Identifier: MIT
//
// The external links the operating system itself provides, without a library. Today both are on
// Linux only:
//
//   tap     a TAP device (/dev/net/tun): a virtual Ethernet interface inside this computer. The
//           bridge is the cable's far end: what it sends, the OS receives on the interface, and
//           what the OS (or a program) sends on the interface, the bridge receives.
//   packet  a network interface (a network adapter, a veth) through a packet socket (AF_PACKET).
//
// On other operating systems has_os_links() is false and the make functions throw LinkError.
// The link through the pcap library (Npcap on Windows, libpcap on Linux) is in pcap_link.hpp.
// No SIL Kit.
//
// The messages of the LinkError these links throw name the options of
// vehicle_sil_ethernet_bridge where an option is the fix (--tap-create, --list-interfaces).
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"

#include <memory>
#include <string>
#include <vector>

namespace vehicle_sil::eth_link {

/// One network interface of this computer.
struct InterfaceInfo
{
    std::string name;
    /// The interface carries Ethernet frames (the OS's link type for Ethernet).
    bool ethernet = false;
    /// The OS's number for the kind of link (Linux: ARPHRD_*).
    unsigned link_type = 0;
    /// Switched on by the administrator ("ip link set <name> up").
    bool up = false;
};

/// Whether make_tap_link() and make_packet_link() work on this operating system.
[[nodiscard]] bool has_os_links() noexcept;

/// The network interfaces of this computer, in the OS's order. Empty where has_os_links() is false.
[[nodiscard]] std::vector<InterfaceInfo> list_interfaces();

/// One line per interface, for --list-interfaces:
///   "  --interface enp3s0  Ethernet, up"
///   "  --interface lo      not Ethernet (link type 772), up"
[[nodiscard]] std::string format_interface_list(const std::vector<InterfaceInfo>& interfaces);

struct TapLinkOptions
{
    /// The TAP device's name.
    std::string name;
    /// false: attach to a TAP device that exists (made before, owned by this user).
    /// true: create it; it is gone again when the link is destroyed (needs CAP_NET_ADMIN).
    bool create = false;
};

struct PacketLinkOptions
{
    /// The interface's name.
    std::string name;
    /// Receive frames to every address while the link is started (the socket's own membership:
    /// the interface's flag is as before once the link stops).
    bool promiscuous = false;
};

/// A TAP device. start() attaches to it (or creates it), needs no privilege for a device that
/// belongs to the user, and neither brings it up nor gives it an address.
/// echoes_own_transmissions() is false: what the link sends goes to the OS, not back to the link.
[[nodiscard]] std::unique_ptr<ExternalLink> make_tap_link(TapLinkOptions options);

/// A network interface through a packet socket (needs CAP_NET_RAW). The link receives what
/// arrives at the interface and what other programs and the OS on this computer send on it; a VLAN
/// tag the OS took out of a received frame is put back where it was.
/// What the link sends goes out on the cable only: the OS of this computer does not receive it.
/// Its peers are the devices at the other end and programs that open the same interface with a
/// packet socket or pcap; for a program that uses the OS's sockets on this computer, use a TAP link.
/// A frame of this computer's OS whose TCP or UDP checksum is left to the network adapter is passed
/// on as it is, marked LinkEvent::checksum_incomplete, with one Warning before the first.
/// echoes_own_transmissions() is false: the socket does not get the frames it sent itself.
[[nodiscard]] std::unique_ptr<ExternalLink> make_packet_link(PacketLinkOptions options);

} // namespace vehicle_sil::eth_link
