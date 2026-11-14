// SPDX-License-Identifier: MIT
//
// The message and the kind of each way a TAP device or a packet socket fails to open, word for
// word, and the list --list-interfaces prints. Text only, so every platform runs it; the kernel's
// side is held by the tests ethernet_bridge.netns.* on Linux.
#include "check_text.hpp"
#include "test_harness.hpp"

#include "os_link_errors.hpp"
#include "vehicle_sil/eth_link/os_links.hpp"

#include <cerrno>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::eth_link;
using namespace vehicle_sil::eth_link::detail;

const std::vector<std::string> kNames = {"lo", "enp3s0", "vsil0"};

} // namespace

TEST_CASE(os_link_error_no_packet_interface_names_the_interfaces_here)
{
    const auto error = no_packet_interface("eth9", kNames);
    CHECK(error.kind() == LinkErrorKind::NoSuchInterface);
    CHECK_TEXT(error.what(),
               "no interface 'eth9' for --backend packet. Interfaces here: lo, enp3s0, vsil0 (run --list-interfaces).");
    CHECK_TEXT(no_packet_interface("eth9", {}).what(),
               "no interface 'eth9' for --backend packet. Interfaces here: none (run --list-interfaces).");
}

TEST_CASE(os_link_error_no_tap_device_tells_how_to_make_one)
{
    const auto error = no_tap_device("vsil9", kNames);
    CHECK(error.kind() == LinkErrorKind::NoSuchInterface);
    CHECK_TEXT(error.what(),
               "no TAP device 'vsil9'. Make it first: 'sudo ip tuntap add dev vsil9 mode tap user $USER' and 'sudo ip link set "
               "vsil9 up'; or let the bridge make it while it runs with --tap-create (needs CAP_NET_ADMIN). Interfaces here: "
               "lo, enp3s0, vsil0.");
}

TEST_CASE(os_link_error_bad_interface_name)
{
    const auto error = bad_interface_name("an-interface-name-too-long");
    CHECK(error.kind() == LinkErrorKind::NoSuchInterface);
    CHECK_TEXT(error.what(), "interface name 'an-interface-name-too-long' cannot be used: Linux interface names have 1 to 15 "
                             "characters. Give the name as --list-interfaces prints it.");
}

TEST_CASE(os_link_error_not_ethernet_names_the_link_type)
{
    const auto error = not_ethernet("lo", 772);
    CHECK(error.kind() == LinkErrorKind::NotEthernet);
    CHECK_TEXT(error.what(), "interface 'lo' is not Ethernet (link type 772): --backend packet carries Ethernet frames only. "
                             "Choose an Ethernet interface (run --list-interfaces).");
}

TEST_CASE(os_link_error_packet_socket_without_the_capability)
{
    for (const int denied : {EPERM, EACCES})
    {
        const auto error = packet_open_failed("enp3s0", denied, "socket", "/opt/vsil/vehicle_sil_ethernet_bridge");
        CHECK(error.kind() == LinkErrorKind::PermissionDenied);
        CHECK_TEXT(error.what(), "cannot open a packet socket on 'enp3s0': " + error_text(denied) +
                                     ". A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep "
                                     "/opt/vsil/vehicle_sil_ethernet_bridge' once, or run as root.");
    }
    CHECK_TEXT(packet_open_failed("enp3s0", EPERM, "socket", "").what(),
               "cannot open a packet socket on 'enp3s0': " + error_text(EPERM) +
                   ". A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep <this program>' once, or run as "
                   "root.");
}

TEST_CASE(os_link_error_packet_socket_other_failures_name_the_step)
{
    const auto error = packet_open_failed("enp3s0", ENOMEM, "bind", "/x");
    CHECK(error.kind() == LinkErrorKind::Other);
    CHECK_TEXT(error.what(), "cannot open a packet socket on 'enp3s0' (bind): " + error_text(ENOMEM) + ".");
}

TEST_CASE(os_link_error_tun_device_file)
{
    const auto denied = tun_open_failed(EACCES);
    CHECK(denied.kind() == LinkErrorKind::PermissionDenied);
    CHECK_TEXT(denied.what(), "cannot open /dev/net/tun: " + error_text(EACCES) +
                                  ". This user may not use TAP devices here: ask the administrator for read and write access "
                                  "to /dev/net/tun, or run as root.");
    const auto missing = tun_open_failed(ENOENT);
    CHECK(missing.kind() == LinkErrorKind::Other);
    CHECK_TEXT(missing.what(), "cannot open /dev/net/tun: " + error_text(ENOENT) +
                                   ". The kernel's TUN/TAP driver is not there: load it with 'sudo modprobe tun'.");
    CHECK_TEXT(tun_open_failed(EMFILE).what(),
               "cannot open /dev/net/tun: " + error_text(EMFILE) + ". A TAP device is opened through it.");
}

TEST_CASE(os_link_error_tap_of_another_user)
{
    const auto error = tap_attach_failed("vsil0", EPERM);
    CHECK(error.kind() == LinkErrorKind::PermissionDenied);
    CHECK_TEXT(error.what(), "cannot attach to TAP 'vsil0': " + error_text(EPERM) +
                                 ". It belongs to another user. Make it yours first: 'sudo ip tuntap del dev vsil0 mode tap', "
                                 "then 'sudo ip tuntap add dev vsil0 mode tap user $USER' and 'sudo ip link set vsil0 up'.");
}

TEST_CASE(os_link_error_tap_in_use_and_not_a_tap)
{
    const auto busy = tap_attach_failed("vsil0", EBUSY);
    CHECK(busy.kind() == LinkErrorKind::Other);
    CHECK_TEXT(busy.what(), "cannot attach to TAP 'vsil0': " + error_text(EBUSY) +
                                ". Another program has it open, and a TAP device serves one program: stop that program, or "
                                "use another TAP device.");
    const auto other = tap_attach_failed("lo", EINVAL);
    CHECK(other.kind() == LinkErrorKind::Other);
    CHECK_TEXT(other.what(), "interface 'lo' is not a TAP device. For a network interface use --backend packet; a TAP device "
                             "is made with 'sudo ip tuntap add dev <name> mode tap user $USER'.");
    CHECK_TEXT(tap_attach_failed("vsil0", ENOMEM).what(), "cannot attach to TAP 'vsil0': " + error_text(ENOMEM) + ".");
}

TEST_CASE(os_link_error_tap_create)
{
    const auto exists = tap_exists("vsil0");
    CHECK(exists.kind() == LinkErrorKind::Other);
    CHECK_TEXT(exists.what(), "cannot create TAP 'vsil0': an interface with that name exists. Leave out --tap-create to attach "
                              "to it, or choose another name.");
    const auto denied = tap_create_failed("vsil1", EPERM);
    CHECK(denied.kind() == LinkErrorKind::PermissionDenied);
    CHECK_TEXT(denied.what(),
               "cannot create TAP 'vsil1': " + error_text(EPERM) +
                   ". Creating a TAP device needs CAP_NET_ADMIN: make it first with 'sudo ip tuntap add dev vsil1 mode tap "
                   "user $USER' and 'sudo ip link set vsil1 up' and leave out --tap-create, or run as root.");
    CHECK_TEXT(tap_create_failed("vsil1", ENOMEM).what(), "cannot create TAP 'vsil1': " + error_text(ENOMEM) + ".");
}

TEST_CASE(os_link_interface_down_warning)
{
    CHECK_TEXT(interface_down_warning("vsil0"),
               "interface 'vsil0' is down: no frame passes until it is up. Bring it up with 'sudo ip link set vsil0 up'.");
}

TEST_CASE(os_link_interface_list_is_one_aligned_line_per_interface)
{
    const std::vector<InterfaceInfo> interfaces = {{"lo", false, 772, true}, {"enp3s0", true, 1, true}, {"vsil0", true, 1, false}};
    CHECK_TEXT(format_interface_list(interfaces), "  --interface lo      not Ethernet (link type 772), up\n"
                                                  "  --interface enp3s0  Ethernet, up\n"
                                                  "  --interface vsil0   Ethernet, down\n");
    CHECK_TEXT(format_interface_list({}), "");
}

TEST_CASE(os_links_exist_on_linux_only)
{
#if defined(__linux__)
    CHECK(has_os_links());
    // Every Linux has the loopback interface, and it is not Ethernet.
    bool loopback = false;
    for (const auto& entry : list_interfaces())
    {
        loopback = loopback || (entry.name == "lo" && !entry.ethernet);
    }
    CHECK(loopback);
#else
    CHECK(!has_os_links());
    CHECK(list_interfaces().empty());
    try
    {
        (void)make_packet_link({"eth0", false});
        CHECK(false);
    }
    catch (const LinkError& e)
    {
        CHECK_TEXT(e.what(), "a packet link works on Linux only: run the bridge on a Linux computer.");
    }
    try
    {
        (void)make_tap_link({"vsil0", false});
        CHECK(false);
    }
    catch (const LinkError& e)
    {
        CHECK_TEXT(e.what(), "a TAP link works on Linux only: run the bridge on a Linux computer.");
    }
#endif
}
