// SPDX-License-Identifier: MIT
#include "pcap_errors.hpp"

namespace vehicle_sil::eth_link::detail {
namespace {

std::string quoted(std::string_view name)
{
    return "'" + std::string(name) + "'";
}

std::string joined(std::span<const std::string> names)
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

std::string list_command(PcapFlavor flavor)
{
    return "--list-interfaces --backend " + std::string(backend_name(flavor));
}

/// What to do about a library that is missing or too old.
std::string how_to_install(PcapFlavor flavor)
{
    if (flavor == PcapFlavor::Npcap)
    {
        return "Npcap is not part of vehicle-sil: install it yourself from https://npcap.com (see its licence for the limits "
               "of the free edition), then run --list-interfaces.";
    }
    return "libpcap is not part of vehicle-sil: install your distribution's package (Debian and Ubuntu: 'sudo apt install "
           "libpcap0.8'; Fedora: 'sudo dnf install libpcap'), or use --backend packet, which needs no library.";
}

} // namespace

std::string_view backend_name(PcapFlavor flavor) noexcept
{
    return flavor == PcapFlavor::Npcap ? "npcap" : "pcap";
}

std::string_view library_name(PcapFlavor flavor) noexcept
{
    return flavor == PcapFlavor::Npcap ? "Npcap" : "libpcap";
}

LinkError pcap_library_missing(PcapFlavor flavor, std::string_view tried, std::string_view why)
{
    return {LinkErrorKind::LibraryMissing, std::string(library_name(flavor)) + " is not installed: cannot load " +
                                               std::string(tried) + " (" + std::string(why) + "). " + how_to_install(flavor)};
}

LinkError pcap_functions_missing(PcapFlavor flavor, std::string_view path, std::string_view version,
                                 std::span<const std::string> names)
{
    std::string what = std::string(path);
    if (!version.empty())
    {
        what += " (" + std::string(version) + ")";
    }
    what += names.size() == 1 ? " lacks the function " : " lacks the functions ";
    what += joined(names) + ", which the bridge needs: it is too old, or it is not " + std::string(library_name(flavor)) +
            ". Install a current one. " + how_to_install(flavor);
    return {LinkErrorKind::LibraryMissing, what};
}

LinkError pcap_list_failed(PcapFlavor flavor, std::string_view why)
{
    std::string what = std::string(library_name(flavor)) + " cannot list the interfaces: " + std::string(why) + ".";
    if (flavor == PcapFlavor::Npcap)
    {
        what += " If Npcap was installed for administrators only, run the bridge from a console started as administrator.";
    }
    return {LinkErrorKind::Other, what};
}

LinkError pcap_no_interface(PcapFlavor flavor, std::string_view wanted, std::span<const std::string> names)
{
    return {LinkErrorKind::NoSuchInterface, "no interface " + quoted(wanted) + " for --backend " + std::string(backend_name(flavor)) +
                                                ". Interfaces " + std::string(library_name(flavor)) + " offers here: " +
                                                joined(names) + " (run " + list_command(flavor) + ")."};
}

LinkError pcap_ambiguous_interface(PcapFlavor flavor, std::string_view wanted, std::span<const std::string> names)
{
    return {LinkErrorKind::NoSuchInterface, quoted(wanted) + " is the name of " + std::to_string(names.size()) +
                                                " interfaces for --backend " + std::string(backend_name(flavor)) + ": " +
                                                joined(names) + ". Give one of these names as --interface instead."};
}

LinkError pcap_not_ethernet(PcapFlavor flavor, std::string_view name, int link_type)
{
    return {LinkErrorKind::NotEthernet, "interface " + quoted(name) + " is not Ethernet (pcap link type " + std::to_string(link_type) +
                                            "): --backend " + std::string(backend_name(flavor)) +
                                            " carries Ethernet frames only. Choose an Ethernet interface (run " +
                                            list_command(flavor) + ")."};
}

LinkError pcap_open_failed(PcapFlavor flavor, std::string_view name, std::string_view step, int status, std::string_view why,
                           std::string_view executable)
{
    std::string what = "cannot open " + quoted(name) + " with " + std::string(library_name(flavor)) + " (" + std::string(step) +
                       ": result " + std::to_string(status) + (why.empty() ? "" : ", " + std::string(why)) + ").";
    // Npcap's captures are opened by pcap_open() (pcap_open_refused()); with it, only steps
    // after the activation come here.
    if (flavor == PcapFlavor::Npcap)
    {
        return {LinkErrorKind::Other, what};
    }
    if (status == kPcapErrorPermissionDenied || status == kPcapErrorPromiscuousPermissionDenied)
    {
        what += " A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep " +
                (executable.empty() ? std::string("<this executable>") : std::string(executable)) + "' once, or run as root.";
        return {LinkErrorKind::PermissionDenied, what};
    }
    if (status == kPcapErrorInterfaceNotUp)
    {
        what += " The interface is down: bring it up with 'sudo ip link set " + std::string(name) + " up'.";
        return {LinkErrorKind::Other, what};
    }
    if (status == kPcapErrorNoSuchDevice)
    {
        return {LinkErrorKind::NoSuchInterface, what + " Run " + list_command(flavor) + " to see the interfaces."};
    }
    return {LinkErrorKind::Other, what};
}

LinkError pcap_open_refused(std::string_view name, std::string_view why)
{
    const std::string list = list_command(PcapFlavor::Npcap);
    return {LinkErrorKind::Other,
            "cannot open " + quoted(name) + " with Npcap (" + (why.empty() ? "pcap_open gave no reason" : "pcap_open: " + std::string(why)) +
                "). Run " + list + " to see whether Npcap still offers the adapter; if Npcap was installed for administrators only, "
                "run the bridge from a console started as administrator."};
}

std::string pcap_wireless_warning(std::string_view name)
{
    return "interface " + quoted(name) +
           " is a wireless adapter: wireless adapters and access points often pass only frames from and to the adapter's own "
           "MAC address, and the devices of the SIL Kit side have other addresses. If frames do not pass, use a wired adapter.";
}

std::string pcap_no_wake_warning(PcapFlavor flavor, std::string_view name, std::chrono::milliseconds period)
{
    const std::string time = std::to_string(period.count()) + " ms";
    return std::string(library_name(flavor)) + " does not tell when frames arrive on interface " + quoted(name) +
           ": the bridge finds them by looking every " + time + ", so frames from this interface reach SIL Kit up to " + time +
           " late. None is lost by this. If the delay matters, " +
           (flavor == PcapFlavor::Npcap ? "install the current Npcap, or try another adapter."
                                        : "update libpcap, or use --backend packet.");
}

std::string pcap_activate_warning(std::string_view name, int status, std::string_view why)
{
    const std::string detail = "result " + std::to_string(status) + (why.empty() ? "" : ", " + std::string(why));
    if (status == kPcapWarningPromiscuousNotSupported)
    {
        return "interface " + quoted(name) + " cannot be put into promiscuous mode (" + detail +
               "): only frames to its own address and to group addresses arrive, as without --promiscuous.";
    }
    return "interface " + quoted(name) + " was opened with a warning (" + detail + ").";
}

} // namespace vehicle_sil::eth_link::detail
