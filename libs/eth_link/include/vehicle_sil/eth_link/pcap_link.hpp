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

#include <memory>
#include <string>

namespace vehicle_sil::eth_link {

struct PcapLinkOptions
{
    /// The interface: its pcap name, or on Windows also the name of the network connection
    /// ("Ethernet 2") or the adapter's identifier ("{4D36E972-...}", with or without braces).
    std::string name;
    /// Receive frames to every address while the link is started.
    bool promiscuous = false;
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

/// What --list-interfaces prints for the pcap library: the interfaces it offers, one per line as
/// --interface takes them. `library`: as PcapLinkOptions::library. Throws LinkError.
[[nodiscard]] std::string pcap_interface_list(const std::string& library = {});

} // namespace vehicle_sil::eth_link
