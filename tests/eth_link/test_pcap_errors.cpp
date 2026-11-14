// SPDX-License-Identifier: MIT
//
// The pcap link's messages, word for word, for both libraries (Npcap on Windows, libpcap on
// Linux); how --interface chooses a device; the list --list-interfaces prints; and which
// functions the link asks a library for. Text and tables only, so every platform runs it.
#include "check_text.hpp"
#include "test_harness.hpp"

#include "pcap_errors.hpp"
#include "pcap_link.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::eth_link;
using namespace vehicle_sil::eth_link::detail;

const char* const kNpf = "\\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}";
const char* const kNpfOther = "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}";

std::vector<PcapDevice> windows_devices()
{
    return {{kNpf, "Microsoft KM-TEST Loopback Adapter", "Ethernet 2", kPcapIfUp | kPcapIfRunning | kPcapIfConnected},
            {kNpfOther, "Intel(R) Ethernet", "Ethernet", kPcapIfUp | kPcapIfDisconnected},
            {"\\Device\\NPF_Loopback", "Adapter for loopback traffic capture", "", kPcapIfUp | kPcapIfLoopback}};
}

template <typename Function>
std::string error_of(Function&& function, LinkErrorKind kind)
{
    try
    {
        function();
    }
    catch (const LinkError& e)
    {
        return e.kind() == kind ? e.what() : std::string("wrong kind (") + std::string(to_string(e.kind())) + "): " + e.what();
    }
    return "no LinkError";
}

} // namespace

TEST_CASE(pcap_error_library_missing_tells_how_to_install_it)
{
    const auto npcap = pcap_library_missing(PcapFlavor::Npcap, "C:\\Windows\\System32\\Npcap\\wpcap.dll", "Windows error 126");
    CHECK(npcap.kind() == LinkErrorKind::LibraryMissing);
    CHECK_TEXT(npcap.what(),
               "Npcap is not installed: cannot load C:\\Windows\\System32\\Npcap\\wpcap.dll (Windows error 126). Npcap is not "
               "part of vehicle-sil: install it yourself from https://npcap.com (see its licence for the limits of the free "
               "edition), then run --list-interfaces.");
    const auto libpcap = pcap_library_missing(PcapFlavor::Libpcap, "libpcap.so.1 or libpcap.so.0.8",
                                              "libpcap.so.0.8: cannot open shared object file: No such file or directory");
    CHECK(libpcap.kind() == LinkErrorKind::LibraryMissing);
    CHECK_TEXT(libpcap.what(),
               "libpcap is not installed: cannot load libpcap.so.1 or libpcap.so.0.8 (libpcap.so.0.8: cannot open shared "
               "object file: No such file or directory). libpcap is not part of vehicle-sil: install your distribution's "
               "package (Debian and Ubuntu: 'sudo apt install libpcap0.8'; Fedora: 'sudo dnf install libpcap'), or use "
               "--backend packet, which needs no library.");
}

TEST_CASE(pcap_error_functions_missing_names_the_library_and_the_functions)
{
    const std::vector<std::string> one = {"pcap_set_immediate_mode"};
    const auto npcap = pcap_functions_missing(PcapFlavor::Npcap, "C:\\Windows\\System32\\Npcap\\wpcap.dll", "WinPcap version 4.1.3", one);
    CHECK(npcap.kind() == LinkErrorKind::LibraryMissing);
    CHECK_TEXT(npcap.what(),
               "C:\\Windows\\System32\\Npcap\\wpcap.dll (WinPcap version 4.1.3) lacks the function pcap_set_immediate_mode, "
               "which the bridge needs: it is too old, or it is not Npcap. Install a current one. Npcap is not part of "
               "vehicle-sil: install it yourself from https://npcap.com (see its licence for the limits of the free edition), "
               "then run --list-interfaces.");
    const std::vector<std::string> two = {"pcap_create", "pcap_activate"};
    CHECK_TEXT(pcap_functions_missing(PcapFlavor::Libpcap, "libpcap.so.0.8", "", two).what(),
               "libpcap.so.0.8 lacks the functions pcap_create, pcap_activate, which the bridge needs: it is too old, or it is "
               "not libpcap. Install a current one. libpcap is not part of vehicle-sil: install your distribution's package "
               "(Debian and Ubuntu: 'sudo apt install libpcap0.8'; Fedora: 'sudo dnf install libpcap'), or use --backend "
               "packet, which needs no library.");
}

TEST_CASE(pcap_error_no_interface_and_more_than_one)
{
    const std::vector<std::string> names = {"\"Ethernet 2\"", "Ethernet"};
    const auto none = pcap_no_interface(PcapFlavor::Npcap, "Ethernet 9", names);
    CHECK(none.kind() == LinkErrorKind::NoSuchInterface);
    CHECK_TEXT(none.what(), "no interface 'Ethernet 9' for --backend npcap. Interfaces Npcap offers here: \"Ethernet 2\", Ethernet "
                            "(run --list-interfaces --backend npcap).");
    CHECK_TEXT(pcap_no_interface(PcapFlavor::Libpcap, "eth9", {}).what(),
               "no interface 'eth9' for --backend pcap. Interfaces libpcap offers here: none (run --list-interfaces --backend "
               "pcap).");
    const std::vector<std::string> two = {kNpf, kNpfOther};
    const auto many = pcap_ambiguous_interface(PcapFlavor::Npcap, "Ethernet", two);
    CHECK(many.kind() == LinkErrorKind::NoSuchInterface);
    CHECK_TEXT(many.what(), "'Ethernet' is the name of 2 interfaces for --backend npcap: "
                            "\\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}, "
                            "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}. Give one of these names as --interface instead.");
    const auto list = pcap_list_failed(PcapFlavor::Npcap, "PacketGetAdapterNames: Access is denied");
    CHECK(list.kind() == LinkErrorKind::Other);
    CHECK_TEXT(list.what(), "Npcap cannot list the interfaces: PacketGetAdapterNames: Access is denied. If Npcap was installed "
                            "for administrators only, run the bridge from a console started as administrator.");
    CHECK_TEXT(pcap_list_failed(PcapFlavor::Libpcap, "no memory").what(), "libpcap cannot list the interfaces: no memory.");
}

TEST_CASE(pcap_error_not_ethernet)
{
    const auto error = pcap_not_ethernet(PcapFlavor::Libpcap, "any", 113);
    CHECK(error.kind() == LinkErrorKind::NotEthernet);
    CHECK_TEXT(error.what(), "interface 'any' is not Ethernet (pcap link type 113): --backend pcap carries Ethernet frames only. "
                             "Choose an Ethernet interface (run --list-interfaces --backend pcap).");
}

TEST_CASE(pcap_error_open_failed_tells_the_fix_for_what_it_knows)
{
    const auto denied = pcap_open_failed(PcapFlavor::Libpcap, "enp3s0", "pcap_activate", kPcapErrorPermissionDenied,
                                         "socket: Operation not permitted", "/opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge");
    CHECK(denied.kind() == LinkErrorKind::PermissionDenied);
    CHECK_TEXT(denied.what(),
               "cannot open 'enp3s0' with libpcap (pcap_activate: result -8, socket: Operation not permitted). A network "
               "interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep /opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge' "
               "once, or run as root.");
    CHECK_TEXT(pcap_open_failed(PcapFlavor::Libpcap, "enp3s0", "pcap_activate", kPcapErrorPromiscuousPermissionDenied, "", "").what(),
               "cannot open 'enp3s0' with libpcap (pcap_activate: result -11). A network interface needs CAP_NET_RAW: run 'sudo "
               "setcap cap_net_raw+ep <this executable>' once, or run as root.");
    const auto down = pcap_open_failed(PcapFlavor::Libpcap, "va", "pcap_activate", kPcapErrorInterfaceNotUp, "va: That device is not up", "");
    CHECK(down.kind() == LinkErrorKind::Other);
    CHECK_TEXT(down.what(), "cannot open 'va' with libpcap (pcap_activate: result -9, va: That device is not up). The interface is "
                            "down: bring it up with 'sudo ip link set va up'.");
    const auto gone = pcap_open_failed(PcapFlavor::Libpcap, "va", "pcap_activate", kPcapErrorNoSuchDevice, "va: No such device exists", "");
    CHECK(gone.kind() == LinkErrorKind::NoSuchInterface);
    CHECK_TEXT(gone.what(), "cannot open 'va' with libpcap (pcap_activate: result -5, va: No such device exists). Run "
                            "--list-interfaces --backend pcap to see the interfaces.");
    // Npcap's captures are opened by pcap_open(), which tells a failure as a text only
    // (pcap_open_refused()): a step after it has no advice by its result.
    const auto other = pcap_open_failed(PcapFlavor::Npcap, "Ethernet 2", "pcap_setnonblock", -1, "", "");
    CHECK(other.kind() == LinkErrorKind::Other);
    CHECK_TEXT(other.what(), "cannot open 'Ethernet 2' with Npcap (pcap_setnonblock: result -1).");
    CHECK_TEXT(pcap_open_failed(PcapFlavor::Npcap, "Ethernet 2", "pcap_setnonblock", kPcapErrorPermissionDenied, "", "").what(),
               "cannot open 'Ethernet 2' with Npcap (pcap_setnonblock: result -8).");
}

TEST_CASE(pcap_error_open_refused_by_npcap_tells_the_reason_and_what_to_look_at)
{
    const auto refused = pcap_open_refused("Ethernet 2", "\\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}: failed to set hardware filter");
    CHECK(refused.kind() == LinkErrorKind::Other);
    CHECK_TEXT(refused.what(),
               "cannot open 'Ethernet 2' with Npcap (pcap_open: \\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}: failed to set "
               "hardware filter). Run --list-interfaces --backend npcap to see whether Npcap still offers the adapter; if Npcap "
               "was installed for administrators only, run the bridge from a console started as administrator.");
    CHECK_TEXT(pcap_open_refused("Ethernet 2", "").what(),
               "cannot open 'Ethernet 2' with Npcap (pcap_open gave no reason). Run --list-interfaces --backend npcap to see "
               "whether Npcap still offers the adapter; if Npcap was installed for administrators only, run the bridge from a "
               "console started as administrator.");
}

TEST_CASE(pcap_warnings)
{
    CHECK_TEXT(pcap_wireless_warning("Wi-Fi"),
               "interface 'Wi-Fi' is a wireless adapter: wireless adapters and access points often pass only frames from and to "
               "the adapter's own MAC address, and the devices of the SIL Kit side have other addresses. If frames do not pass, "
               "use a wired adapter.");
    CHECK_TEXT(pcap_activate_warning("va", kPcapWarningPromiscuousNotSupported, "not supported"),
               "interface 'va' cannot be put into promiscuous mode (result 2, not supported): only frames to its own address and "
               "to group addresses arrive, as without --promiscuous.");
    CHECK_TEXT(pcap_activate_warning("va", 1, ""), "interface 'va' was opened with a warning (result 1).");
    CHECK_TEXT(pcap_no_wake_warning(PcapFlavor::Npcap, "Ethernet 2", std::chrono::milliseconds(100)),
               "Npcap does not tell when frames arrive on interface 'Ethernet 2': the bridge finds them by looking every "
               "100 ms, so frames from this interface reach SIL Kit up to 100 ms late. None is lost by this. If the delay "
               "matters, install the current Npcap, or try another adapter.");
    CHECK_TEXT(pcap_no_wake_warning(PcapFlavor::Libpcap, "va", std::chrono::milliseconds(100)),
               "libpcap does not tell when frames arrive on interface 'va': the bridge finds them by looking every 100 ms, so "
               "frames from this interface reach SIL Kit up to 100 ms late. None is lost by this. If the delay matters, update "
               "libpcap, or use --backend packet.");
}

TEST_CASE(pcap_device_is_chosen_by_pcap_name_connection_name_or_identifier)
{
    const auto devices = windows_devices();
    CHECK_EQ(choose_pcap_device(devices, kNpfOther, PcapFlavor::Npcap), std::size_t{1});
    CHECK_EQ(choose_pcap_device(devices, "Ethernet 2", PcapFlavor::Npcap), std::size_t{0});
    CHECK_EQ(choose_pcap_device(devices, "Ethernet", PcapFlavor::Npcap), std::size_t{1});
    CHECK_EQ(choose_pcap_device(devices, "{4d36e972-e325-11ce-bfc1-08002be10318}", PcapFlavor::Npcap), std::size_t{0});
    CHECK_EQ(choose_pcap_device(devices, "0A1B2C3D-0000-1111-2222-333344445555", PcapFlavor::Npcap), std::size_t{1});
    CHECK_EQ(choose_pcap_device(devices, "\\Device\\NPF_Loopback", PcapFlavor::Npcap), std::size_t{2});
    // A connection name is not matched in part, and not in another case.
    CHECK_TEXT(error_of([&] { (void)choose_pcap_device(devices, "ethernet", PcapFlavor::Npcap); }, LinkErrorKind::NoSuchInterface),
               "no interface 'ethernet' for --backend npcap. Interfaces Npcap offers here: \"Ethernet 2\", Ethernet, "
               "\\Device\\NPF_Loopback (run --list-interfaces --backend npcap).");
    CHECK_TEXT(error_of([&] { (void)choose_pcap_device(devices, "", PcapFlavor::Npcap); }, LinkErrorKind::NoSuchInterface),
               "no interface '' for --backend npcap. Interfaces Npcap offers here: \"Ethernet 2\", Ethernet, "
               "\\Device\\NPF_Loopback (run --list-interfaces --backend npcap).");
}

TEST_CASE(pcap_device_name_that_fits_two_devices_is_refused_and_a_pcap_name_wins)
{
    auto devices = windows_devices();
    devices[1].connection = "Ethernet 2"; // two connections of one name
    CHECK_TEXT(error_of([&] { (void)choose_pcap_device(devices, "Ethernet 2", PcapFlavor::Npcap); }, LinkErrorKind::NoSuchInterface),
               "'Ethernet 2' is the name of 2 interfaces for --backend npcap: "
               "\\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}, \\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}. Give "
               "one of these names as --interface instead.");
    // On Linux pcap names are the interface names; a connection name equal to another device's pcap name loses.
    const std::vector<PcapDevice> linux_like = {{"va", "", "vb", kPcapIfUp}, {"vb", "", "", kPcapIfUp}};
    CHECK_EQ(choose_pcap_device(linux_like, "vb", PcapFlavor::Libpcap), std::size_t{1});
}

TEST_CASE(pcap_interface_list_shows_the_name_to_give_and_what_the_library_says)
{
    CHECK_TEXT(format_pcap_devices(windows_devices(), PcapFlavor::Npcap, "Npcap version 1.80, based on libpcap version 1.10.5"),
               "interfaces Npcap offers (Npcap version 1.80, based on libpcap version 1.10.5); --backend npcap takes the Ethernet "
               "ones:\n"
               "  --interface \"Ethernet 2\"          up, connected  (Microsoft KM-TEST Loopback Adapter; "
               "\\Device\\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318})\n"
               "  --interface Ethernet              up, disconnected  (Intel(R) Ethernet; "
               "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555})\n"
               "  --interface \\Device\\NPF_Loopback  up, loopback  (Adapter for loopback traffic capture)\n");
    const std::vector<PcapDevice> linux_like = {{"enp3s0", "", "", kPcapIfUp | kPcapIfRunning | kPcapIfConnected},
                                                {"wlan0", "", "", kPcapIfWireless},
                                                {"any", "Pseudo-device that captures on all interfaces", "", kPcapIfUp}};
    CHECK_TEXT(format_pcap_devices(linux_like, PcapFlavor::Libpcap, "libpcap version 1.10.4 (with TPACKET_V3)"),
               "interfaces libpcap offers (libpcap version 1.10.4 (with TPACKET_V3)); --backend pcap takes the Ethernet ones:\n"
               "  --interface enp3s0  up, connected\n"
               "  --interface wlan0   down, wireless\n"
               "  --interface any     up  (Pseudo-device that captures on all interfaces)\n");
    CHECK_TEXT(format_pcap_devices({}, PcapFlavor::Libpcap, ""),
               "interfaces libpcap offers; --backend pcap takes the Ethernet ones:\n  none\n");
    CHECK_TEXT(format_pcap_devices({}, PcapFlavor::Npcap, ""),
               "interfaces Npcap offers; --backend npcap takes the Ethernet ones:\n  none. If Npcap was installed for "
               "administrators only, run this from a console started as administrator.\n");
}

TEST_CASE(pcap_api_names_every_function_it_does_not_find)
{
    static int something = 0;
    std::vector<std::string> asked;
    auto resolve = [&](const char* name) -> void* {
        asked.emplace_back(name);
        return std::strcmp(name, "pcap_set_immediate_mode") == 0 || std::strcmp(name, "pcap_getevent") == 0 ? nullptr : &something;
    };
    const auto [eventApi, eventMissing] = bind_pcap_api(resolve, PcapWaitKind::Event, PcapFlavor::Libpcap);
    CHECK_EQ(eventMissing.size(), std::size_t{2});
    CHECK_TEXT(eventMissing.at(0), "pcap_set_immediate_mode");
    CHECK_TEXT(eventMissing.at(1), "pcap_getevent");
    CHECK(eventApi.set_immediate_mode == nullptr && eventApi.activate != nullptr && eventApi.get_selectable_fd == nullptr);
    // The way of waiting of the other operating system is not asked for.
    CHECK(std::find(asked.begin(), asked.end(), "pcap_get_selectable_fd") == asked.end());
    asked.clear();
    const auto [fdApi, fdMissing] = bind_pcap_api(resolve, PcapWaitKind::Descriptor, PcapFlavor::Libpcap);
    CHECK_EQ(fdMissing.size(), std::size_t{1});
    CHECK(fdApi.get_selectable_fd != nullptr && fdApi.getevent == nullptr && fdApi.open == nullptr);
    CHECK(std::find(asked.begin(), asked.end(), "pcap_getevent") == asked.end());
    // libpcap is not asked for pcap_open(), which not every libpcap has.
    CHECK(std::find(asked.begin(), asked.end(), "pcap_open") == asked.end());
    CHECK_EQ(asked.size(), std::size_t{17});

    // Npcap is: the link opens its captures with it.
    asked.clear();
    auto without_open = [&](const char* name) -> void* {
        asked.emplace_back(name);
        return std::strcmp(name, "pcap_open") == 0 ? nullptr : &something;
    };
    const auto [npcapApi, npcapMissing] = bind_pcap_api(without_open, PcapWaitKind::Event, PcapFlavor::Npcap);
    CHECK_EQ(npcapMissing.size(), std::size_t{1});
    CHECK_TEXT(npcapMissing.at(0), "pcap_open");
    CHECK(npcapApi.open == nullptr && npcapApi.create != nullptr);
    CHECK_EQ(asked.size(), std::size_t{18});
    const auto [fullApi, fullMissing] = bind_pcap_api([](const char*) -> void* { return &something; }, PcapWaitKind::Event, PcapFlavor::Npcap);
    CHECK(fullMissing.empty() && fullApi.open != nullptr);
}
