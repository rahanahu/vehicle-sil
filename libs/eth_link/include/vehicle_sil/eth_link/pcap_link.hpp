// SPDX-License-Identifier: MIT
//
// An external link through the pcap library of the operating system: Npcap on Windows, libpcap
// on Linux. The library is neither built into vehicle-sil nor shipped with it: it is loaded when
// the link is made, and a computer without it gets a LinkError that says how to install it.
// No SIL Kit.
//
// The messages of the LinkError name the options of vehicle_sil_ethernet_bridge where an option
// is the fix (--list-interfaces, --backend).
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vehicle_sil::eth_link {

struct PcapLinkOptions
{
    /// The interface: its pcap name, or on Windows also the name of the network connection
    /// ("Ethernet 2") or the adapter's identifier ("{4D36E972-...}", with or without braces).
    std::string name;
    /// Receive frames to every address while the link is started. A refusal shows as libpcap's
    /// warning that the interface does not support it or its error that this user may not use it,
    /// and as a failure of Npcap's pcap_open() that does not come without promiscuous mode
    /// (On: start() fails; Auto: the link opens without it and warns). An adapter that opens
    /// without promiscuous mode and says nothing cannot be told. Off by default, as before there
    /// was a choice; vehicle_sil_ethernet_bridge gives Auto.
    Promiscuous promiscuous = Promiscuous::Off;
    /// Empty: the installed library. On Windows that is wpcap.dll in the Npcap directory of the
    /// system directory, loaded by its full path and looked for nowhere else; on Linux
    /// libpcap.so.1, then libpcap.so.0.8, as the system's loader finds them.
    /// Else: the file to load instead (tests give a fake library).
    std::string library;
};

/// Loads the library (throws LinkError, LibraryMissing, if it is not installed or lacks a function
/// the link needs) and returns the link, not started: the interface is opened by start().
///
/// The link receives what arrives at the interface and what this computer sends on it; it passes
/// frames on as the library gives them and cannot tell a frame whose checksum this computer's OS
/// left to the network adapter (LinkEvent::checksum_incomplete is never set). It reports the
/// frames the library dropped (Overrun). On Linux it also reports the interface's link going up
/// and down (LinkState) and a Fault once the interface is removed; on Windows it reports no
/// LinkState, and an interface that goes away shows only if reading fails.
///
/// echoes_own_transmissions() is false. On Linux, libpcap does not give the link the frames it
/// sent (the test ethernet_bridge.netns.pcap holds this). Npcap does give a capture its own
/// transmissions, so the link opens its capture with pcap_open() and the flag that keeps them
/// away (PCAP_OPENFLAG_NOCAPTURE_LOCAL); the frames other programs send on the same adapter
/// still arrive. A wpcap.dll without pcap_open() is refused when it is loaded.
[[nodiscard]] std::unique_ptr<ExternalLink> make_pcap_link(PcapLinkOptions options);

/// One interface the pcap library offers.
struct PcapDevice
{
    std::string name;        ///< the pcap name (Windows: "\Device\NPF_{...}"; Linux: the interface's name)
    std::string description; ///< the library's text, or empty
    std::string connection;  ///< the OS's name of the network connection (Windows), or empty
    std::uint32_t flags = 0; ///< the library's PCAP_IF_* flags
    /// The interface's MAC address: on Linux from the library's list, on Windows from the OS
    /// (with the connection name). nullopt where neither tells.
    std::optional<MacAddress> hardware_address{};

    [[nodiscard]] bool up() const noexcept;
    [[nodiscard]] bool loopback() const noexcept;
    [[nodiscard]] bool wireless() const noexcept;
    /// true: connected, false: disconnected, nullopt: the library does not tell.
    [[nodiscard]] std::optional<bool> connected() const noexcept;
};

/// What the pcap library offers: the data of --list-interfaces, which it prints as text
/// (format_pcap_interface_list()) or as JSON (vehicle_sil_ethernet_bridge --json).
struct PcapInterfaceList
{
    std::string library; ///< "Npcap" or "libpcap"
    std::string backend; ///< the --backend that uses it: "npcap" or "pcap"
    std::string version; ///< what the library says it is, or empty
    /// In the library's order, with the OS's names of the network connections.
    std::vector<PcapDevice> devices;
    /// What the list alone does not explain, one sentence each (Npcap that offers no network
    /// adapter, which happens where it was installed for administrators only).
    std::vector<std::string> notes;
};

/// Loads the library and asks it for its interfaces. `library`: as PcapLinkOptions::library.
/// Throws LinkError.
[[nodiscard]] PcapInterfaceList pcap_interfaces(const std::string& library = {});

/// The text of --list-interfaces: a heading with the library's version, then one line per
/// interface as --interface takes it ("  --interface <name>  <pcap_device_detail()>"), then the notes.
/// `device_notes`: by position in list.devices, a text that ends the device's line ("; <note>");
/// an empty one or none adds nothing.
[[nodiscard]] std::string format_pcap_interface_list(const PcapInterfaceList& list,
                                                     std::span<const std::string> device_notes = {});

/// format_pcap_interface_list(pcap_interfaces(library)).
[[nodiscard]] std::string pcap_interface_list(const std::string& library = {});

/// What a line of the list says after the name, its parts "; " apart: the state
/// ("up, connected"), the description, and the pcap name where the line shows the connection name.
[[nodiscard]] std::string pcap_device_detail(const PcapDevice& device);

/// The name for --interface that chooses `devices[index]` alone: its connection name where it has
/// one and no other device has the same one (whatever the case of its ASCII letters) or that pcap
/// name, else its pcap name.
[[nodiscard]] std::string pcap_interface_argument(std::span<const PcapDevice> devices, std::size_t index);

} // namespace vehicle_sil::eth_link
