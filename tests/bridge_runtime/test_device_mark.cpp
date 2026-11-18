// SPDX-License-Identifier: MIT
//
// The device mark (device_mark.hpp): the names, the hash, reading /proc/net/unix, the link line's
// keys and the texts with given values; and real marks of this process on Linux and Windows (each
// test on a device of its own, so that tests that run at the same time do not see each other).
#include "test_harness.hpp"

#include "vehicle_sil/bridge_runtime/device_mark.hpp"

#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace vehicle_sil::bridge_runtime;
namespace detail = vehicle_sil::bridge_runtime::detail;

namespace {

bool same(const std::string& actual, const std::string& expected)
{
    if (actual != expected)
    {
        std::printf("    actual:   %s\n    expected: %s\n", actual.c_str(), expected.c_str());
    }
    return actual == expected;
}

#define CHECK_SAME(actual, expected) CHECK(same((actual), (expected)))

/// A device no other test, and no other run of this test, uses.
std::string own_device(const std::string& test)
{
    std::random_device random;
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = ::getpid();
#endif
    return "test-" + test + "-" + std::to_string(pid) + "-" + std::to_string(random());
}

#if defined(__linux__) || defined(_WIN32)
constexpr bool kMarksHere = true;
#else
constexpr bool kMarksHere = false;
#endif

void require_marks()
{
    if (!kMarksHere)
    {
        throw vehicle_sil::unit::Skipped("this operating system has no device marks");
    }
}

std::uint32_t this_pid()
{
#ifdef _WIN32
    return static_cast<std::uint32_t>(_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

/// The process ids a look gives on this OS: Linux names them, Windows does not.
std::vector<std::uint32_t> pids_here(std::vector<std::uint32_t> linux_pids)
{
#ifdef __linux__
    return linux_pids;
#else
    (void)linux_pids;
    return {};
#endif
}

} // namespace

TEST_CASE(device_mark_hash_is_fnv1a_of_kind_and_identity)
{
    CHECK_SAME(detail::device_hash("", ""), std::string("af63a24c860189fe")); // FNV-1a 64 of "/"
    CHECK(detail::device_hash("ethernet", "eth0") == detail::device_hash("ethernet", "eth0"));
    CHECK(detail::device_hash("ethernet", "eth0") != detail::device_hash("can", "eth0"));
    CHECK(detail::device_hash("ethernet", "eth0") != detail::device_hash("ethernet", "eth1"));
    CHECK_EQ(detail::device_hash("ethernet", "eth0").size(), std::size_t{16});
}

TEST_CASE(device_mark_linux_mark_name_keeps_a_plain_identity_and_hashes_the_rest)
{
    CHECK_SAME(detail::linux_mark_prefix("ethernet", "enp3s0"), std::string("vehicle-sil/device/ethernet/enp3s0/"));
    CHECK_SAME(detail::linux_mark_prefix("can", "xl-virtual@pid12"), std::string("vehicle-sil/device/can/xl-virtual@pid12/"));
    // A slash, a space or a backslash would make the name ambiguous: the hash instead.
    CHECK_SAME(detail::linux_mark_prefix("ethernet", "a/b"),
             "vehicle-sil/device/ethernet/" + detail::device_hash("ethernet", "a/b") + "/");
    CHECK_SAME(detail::linux_mark_prefix("ethernet", "\\Device\\NPF_{X}"),
             "vehicle-sil/device/ethernet/" + detail::device_hash("ethernet", "\\Device\\NPF_{X}") + "/");
    CHECK_SAME(detail::linux_mark_prefix("ethernet", ""), "vehicle-sil/device/ethernet/" + detail::device_hash("ethernet", "") + "/");
    // The longest name (pid of 10 digits) fits the 107 bytes of an abstract name; one more byte is hashed.
    const std::string fits(107 - std::string("vehicle-sil/device/ethernet/").size() - 1 - 27, 'x');
    CHECK_SAME(detail::linux_mark_prefix("ethernet", fits), "vehicle-sil/device/ethernet/" + fits + "/");
    CHECK_EQ(detail::linux_mark_name(detail::linux_mark_prefix("ethernet", fits), 4294967295U, 0).size(), std::size_t{107});
    CHECK_SAME(detail::linux_mark_prefix("ethernet", fits + "x"),
             "vehicle-sil/device/ethernet/" + detail::device_hash("ethernet", fits + "x") + "/");
    CHECK_SAME(detail::linux_mark_name("vehicle-sil/device/ethernet/eth0/", 4321, 0xabc),
             std::string("vehicle-sil/device/ethernet/eth0/4321/0000000000000abc"));
}

TEST_CASE(device_mark_proc_net_unix_gives_the_other_marks_of_the_device)
{
    const std::string text =
        "Num       RefCount Protocol Flags    Type St Inode Path\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 1001 /run/systemd/notify\n"
        "0000000000000000: 00000002 00000000 00000000 0002 01 1002\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 1003 @vehicle-sil/device/ethernet/eth0/4400/00000000000000aa\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 1004 @vehicle-sil/device/ethernet/eth0/4321/00000000000000bb\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 1005 @vehicle-sil/device/ethernet/eth0/77/00000000000000cc\n"
        // Another device whose name begins like this one's, a malformed name, another kind.
        "0000000000000000: 00000002 00000000 00000000 0001 01 1006 @vehicle-sil/device/ethernet/eth0/1/2/00000000000000dd\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 1007 @vehicle-sil/device/ethernet/eth0/x/00000000000000ee\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 1008 @vehicle-sil/device/ethernet/eth0/5/short\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 1009 @vehicle-sil/device/can/eth0/6/00000000000000ff\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 1010 @vehicle-sil/device/ethernet/eth00/7/00000000000000ff\r\n";
    const std::string prefix = "vehicle-sil/device/ethernet/eth0/";
    // The one who looks is left out by its whole name.
    const auto others = detail::read_proc_net_unix(text, prefix, "vehicle-sil/device/ethernet/eth0/4321/00000000000000bb");
    CHECK_EQ(others.count, std::size_t{2});
    CHECK(others.pids == (std::vector<std::uint32_t>{77, 4400}));
    // Looking without a mark: all of them.
    const auto all = detail::read_proc_net_unix(text, prefix, {});
    CHECK_EQ(all.count, std::size_t{3});
    CHECK(all.pids == (std::vector<std::uint32_t>{77, 4321, 4400}));
    CHECK(detail::read_proc_net_unix("", prefix, {}) == DevicePeers{});
}

TEST_CASE(device_mark_proc_net_unix_counts_two_marks_of_one_process_once_in_pids)
{
    const std::string text =
        "0000000000000000: 00000002 00000000 00000000 0001 01 1 @vehicle-sil/device/can/xl-virtual/9/0000000000000001\n"
        "0000000000000000: 00000002 00000000 00000000 0001 01 2 @vehicle-sil/device/can/xl-virtual/9/0000000000000002\n";
    const auto peers = detail::read_proc_net_unix(text, "vehicle-sil/device/can/xl-virtual/", {});
    CHECK_EQ(peers.count, std::size_t{2});
    CHECK(peers.pids == std::vector<std::uint32_t>{9});
}

TEST_CASE(device_mark_windows_mark_name_has_the_space_the_hash_and_the_number)
{
    CHECK_SAME(detail::windows_mark_name("Global\\", "0123456789abcdef", 0), std::string("Global\\vehicle-sil-device-0123456789abcdef-0"));
    CHECK_SAME(detail::windows_mark_name("Local\\", "0123456789abcdef", 15), std::string("Local\\vehicle-sil-device-0123456789abcdef-15"));
}

TEST_CASE(device_mark_keys_of_the_link_line)
{
    CHECK_SAME(detail::format_device_keys("0123456789abcdef", true, {}), std::string(" device=0123456789abcdef marked=1 shared=0"));
    CHECK_SAME(detail::format_device_keys("0123456789abcdef", false, {1, {}}), std::string(" device=0123456789abcdef marked=0 shared=1"));
    CHECK_SAME(detail::format_device_keys("0123456789abcdef", true, {2, {4321, 4400}}),
             std::string(" device=0123456789abcdef marked=1 shared=2 peers=4321,4400"));
}

TEST_CASE(device_mark_shared_warning_comes_when_more_bridges_share_the_device)
{
    CHECK(!detail::shared_warning("interface 'eth0'", {}, {}).has_value());
    CHECK_SAME(detail::shared_warning("interface 'eth0'", {}, {1, {4321}}).value_or(""),
             std::string("interface 'eth0' is also used by another vehicle-sil bridge (process 4321): the frames of both "
                         "mix there: if that is not meant, connect them to different devices, or stop a bridge left over "
                         "from an earlier run"));
    // Only the processes that came.
    CHECK_SAME(detail::shared_warning("XL 'Virtual Channel 1'", {1, {4321}}, {3, {4321, 4400, 4500}}).value_or(""),
             std::string("XL 'Virtual Channel 1' is also used by 2 more vehicle-sil bridges (processes 4400, 4500): the frames of "
                         "both mix there: if that is not meant, connect them to different devices, or stop a bridge left "
                         "over from an earlier run"));
    // Windows: no process ids.
    CHECK(detail::shared_warning("interface 'Ethernet'", {}, {1, {}}).value_or("").starts_with(
        "interface 'Ethernet' is also used by another vehicle-sil bridge: "));
    // Fewer, or as many without a new process: nothing.
    CHECK(!detail::shared_warning("interface 'eth0'", {2, {1, 2}}, {1, {2}}).has_value());
    CHECK(!detail::shared_warning("interface 'eth0'", {1, {1}}, {1, {1}}).has_value());
    CHECK(!detail::shared_warning("interface 'Ethernet'", {1, {}}, {1, {}}).has_value());
    // One bridge left and another came: the count is the same, the warning comes.
    CHECK_SAME(detail::shared_warning("interface 'eth0'", {1, {1}}, {1, {2}}).value_or(""),
             std::string("interface 'eth0' is also used by another vehicle-sil bridge (process 2): the frames of both "
                         "mix there: if that is not meant, connect them to different devices, or stop a bridge left over "
                         "from an earlier run"));
}

TEST_CASE(device_mark_in_use_text_of_the_lists)
{
    CHECK_SAME(in_use_text({}), std::string());
    CHECK_SAME(in_use_text({1, {4321}}), std::string("in use by a vehicle-sil bridge (process 4321)"));
    CHECK_SAME(in_use_text({2, {4321, 4400}}), std::string("in use by 2 vehicle-sil bridges (processes 4321, 4400)"));
    CHECK_SAME(in_use_text({1, {}}), std::string("in use by a vehicle-sil bridge"));
}

// --- Real marks of this process.

TEST_CASE(device_mark_marks_on_one_device_find_each_other_and_go_with_their_owner)
{
    require_marks();
    const std::string device = own_device("share");
    const std::uint32_t pid = this_pid();
    std::optional<DeviceMark> first = DeviceMark::place(kEthernetDevice, device);
    CHECK(first->marked());
    CHECK(first->why_not().empty());
    CHECK_SAME(first->device(), detail::device_hash(kEthernetDevice, device));
    CHECK(first->others() == DevicePeers{});

    std::optional<DeviceMark> second = DeviceMark::place(kEthernetDevice, device);
    CHECK(second->marked());
    // Both count the other one, not themselves: the first finds the second, which came later.
    CHECK(first->others() == (DevicePeers{1, pids_here({pid})}));
    CHECK(second->others() == (DevicePeers{1, pids_here({pid})}));

    // The first ends: its mark is gone (on Windows: the second did not keep the first's number open).
    first.reset();
    CHECK(second->others() == DevicePeers{});

    // A third finds the second, and the second the third.
    const DeviceMark third = DeviceMark::place(kEthernetDevice, device);
    CHECK(third.marked());
    CHECK(third.others() == (DevicePeers{1, pids_here({pid})}));
    CHECK(second->others() == (DevicePeers{1, pids_here({pid})}));
    CHECK(look_for_marks(kEthernetDevice, device) == (DevicePeers{2, pids_here({pid})}));

    second.reset();
    CHECK(third.others() == DevicePeers{});
}

TEST_CASE(device_mark_marks_of_other_devices_and_kinds_are_not_counted)
{
    require_marks();
    const std::string device = own_device("other");
    const DeviceMark mark = DeviceMark::place(kEthernetDevice, device);
    const DeviceMark otherDevice = DeviceMark::place(kEthernetDevice, device + "x");
    const DeviceMark otherKind = DeviceMark::place(kCanDevice, device);
    CHECK(mark.others() == DevicePeers{});
    CHECK(otherDevice.others() == DevicePeers{});
    CHECK(otherKind.others() == DevicePeers{});
    CHECK(mark.device() != otherKind.device());
}

TEST_CASE(device_mark_looking_for_marks_places_none)
{
    require_marks();
    const std::string device = own_device("look");
    CHECK(look_for_marks(kCanDevice, device) == DevicePeers{});
    // A look leaves nothing behind that a mark placed afterwards would count.
    const DeviceMark mark = DeviceMark::place(kCanDevice, device);
    CHECK(look_for_marks(kCanDevice, device) == (DevicePeers{1, pids_here({this_pid()})}));
    CHECK(mark.others() == DevicePeers{});
}

TEST_CASE(device_mark_a_long_identity_is_marked_under_its_hash)
{
    require_marks();
    const std::string device = own_device("long") + std::string(200, 'y') + "/\\ {}";
    const DeviceMark first = DeviceMark::place(kEthernetDevice, device);
    const DeviceMark second = DeviceMark::place(kEthernetDevice, device);
    CHECK(first.marked() && second.marked());
    CHECK_EQ(first.others().count, std::size_t{1});
}

TEST_CASE(device_mark_share_watch_keys_and_warnings)
{
    require_marks();
    const std::string device = own_device("watch");
    [[maybe_unused]] const std::string pid = std::to_string(this_pid());
    DeviceShareWatch watch(DeviceMark::place(kEthernetDevice, device), "interface 'test0'");
    const std::string hash = detail::device_hash(kEthernetDevice, device);
    CHECK_SAME(watch.link_keys(), " device=" + hash + " marked=1 shared=0");
    CHECK(watch.take_warnings().empty());

    std::optional<DeviceMark> other = DeviceMark::place(kEthernetDevice, device);
    // Not counted again yet: the keys stay as they were.
    CHECK_SAME(watch.link_keys(), " device=" + hash + " marked=1 shared=0");
    watch.count();
#ifdef __linux__
    CHECK_SAME(watch.link_keys(), " device=" + hash + " marked=1 shared=1 peers=" + pid);
#else
    CHECK_SAME(watch.link_keys(), " device=" + hash + " marked=1 shared=1");
#endif
    auto warnings = watch.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(warnings.at(0).starts_with("interface 'test0' is also used by another vehicle-sil bridge"));
    // As many as before: no new warning.
    watch.count();
    CHECK(watch.take_warnings().empty());

    other.reset();
    watch.count();
    CHECK_SAME(watch.link_keys(), " device=" + hash + " marked=1 shared=0");
    CHECK(watch.take_warnings().empty());
}

TEST_CASE(device_mark_share_watch_warns_at_once_when_the_device_is_shared_from_the_start)
{
    require_marks();
    const std::string device = own_device("start");
    const DeviceMark before = DeviceMark::place(kCanDevice, device);
    DeviceShareWatch watch(DeviceMark::place(kCanDevice, device), "XL 'Virtual Channel 1'");
    const auto warnings = watch.take_warnings();
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK(warnings.at(0).starts_with("XL 'Virtual Channel 1' is also used by another vehicle-sil bridge"));
}
