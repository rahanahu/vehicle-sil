// SPDX-License-Identifier: MIT
//
// --connect without processes: reading a request, the form of a target, the arguments of a
// bridge, and where the launcher looks for a bridge (files in a scratch directory; PATH and the
// working directory are changed in the test and put back).
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/check.hpp"
#include "vehicle_sil/launch/connect.hpp"
#include "vehicle_sil/launch/probe.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name)
{
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "connect" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// A file `name` in `dir` (on Windows "<name>.exe"), runnable or not.
fs::path tool_file(const fs::path& dir, const std::string& name, bool runnable = true)
{
#ifdef _WIN32
    const fs::path path = dir / (name + ".exe");
#else
    const fs::path path = dir / name;
#endif
    std::ofstream(path) << "#!/bin/sh\n";
    fs::permissions(path, runnable ? fs::perms::owner_all : fs::perms::owner_read | fs::perms::owner_write);
    return path;
}

/// Sets PATH to `value` and the working directory to `cwd` (if not empty) until it ends.
class Environment
{
public:
    Environment(const std::string& value, const fs::path& cwd = {}) : cwd_(fs::current_path())
    {
#ifdef _WIN32
        const wchar_t* old = _wgetenv(L"PATH");
        old_ = old != nullptr ? std::optional<std::wstring>(old) : std::nullopt;
        _wputenv_s(L"PATH", process::path_from_utf8(value).wstring().c_str());
#else
        const char* old = std::getenv("PATH");
        old_ = old != nullptr ? std::optional<std::string>(old) : std::nullopt;
        setenv("PATH", value.c_str(), 1);
#endif
        if (!cwd.empty())
        {
            fs::current_path(cwd);
        }
    }
    ~Environment()
    {
        fs::current_path(cwd_);
#ifdef _WIN32
        _wputenv_s(L"PATH", old_ ? old_->c_str() : L"");
#else
        if (old_)
        {
            setenv("PATH", old_->c_str(), 1);
        }
        else
        {
            unsetenv("PATH");
        }
#endif
    }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

private:
    fs::path cwd_;
#ifdef _WIN32
    std::optional<std::wstring> old_;
#else
    std::optional<std::string> old_;
#endif
};

#ifdef _WIN32
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

std::string words(const std::vector<std::string>& arguments)
{
    std::string out;
    for (const std::string& a : arguments)
    {
        out += (out.empty() ? "" : " ") + a;
    }
    return out;
}

NetworkConfig network(const std::string& name, NetworkKind kind, std::optional<NetworkExternal> external = std::nullopt)
{
    NetworkConfig n{name, kind, 3, true};
    n.external = std::move(external);
    return n;
}

eth::MacAddress mac(const char* text) { return *eth::MacAddress::parse(text); }

} // namespace

TEST_CASE(parse_connect_splits_at_the_first_equals_sign)
{
    const auto plain = parse_connect("ETH_EXT=vEthernet (A)");
    CHECK(plain.has_value());
    check_text(plain->network + "|" + plain->target, "ETH_EXT|vEthernet (A)");
    const auto equals = parse_connect("ETH=a=b");
    CHECK(equals.has_value());
    check_text(equals->network + "|" + equals->target, "ETH|a=b");
    const auto none = parse_connect("CAN1=none");
    CHECK(none.has_value() && none->target == kNoTarget);
    const auto empty = parse_connect("CAN1=");
    CHECK(empty.has_value() && empty->target.empty());
    CHECK(!parse_connect("CAN1").has_value());
    CHECK(!parse_connect("=eth0").has_value());
    CHECK(!parse_connect("").has_value());
}

TEST_CASE(the_bridge_of_each_kind)
{
    CHECK(bridge_tool(NetworkKind::Lin, "xl").empty());
    check_text(bridge_participant("ETH_EXT"), "bridge.ETH_EXT");
    check_text(no_bridge_here(NetworkKind::Lin, {}, Os::Linux).value_or("-"), "there is no bridge for LIN");
    check_text(no_bridge_here(NetworkKind::Can, "xl", Os::Linux).value_or("-"), "the Vector XL bridge is for Windows");
    CHECK(!no_bridge_here(NetworkKind::Can, "xl", Os::Windows) && !no_bridge_here(NetworkKind::Can, "socketcan", Os::Linux) &&
          !no_bridge_here(NetworkKind::Ethernet, "tap", Os::Linux));
    check_text(std::string(bridge_tool(NetworkKind::Can, "socketcan")), "vehicle_sil_socketcan_bridge");
    check_text(std::string(bridge_tool(NetworkKind::Can, "xl")), "vehicle_sil_vector_xl_bridge");
    for (const char* backend : {"npcap", "packet", "tap"})
    {
        check_text(std::string(bridge_tool(NetworkKind::Ethernet, backend)), "vehicle_sil_ethernet_bridge", backend);
    }
    CHECK(bridge_tool(NetworkKind::Ethernet, "xl").empty() && bridge_tool(NetworkKind::Can, "tap").empty());
    CHECK(is_bridge_tool("vehicle_sil_socketcan_bridge") && is_can_bridge_tool("vehicle_sil_socketcan_bridge") &&
          is_can_bridge_tool("vehicle_sil_vector_xl_bridge") && !is_can_bridge_tool("vehicle_sil_ethernet_bridge") &&
          is_bridge_tool("vehicle_sil_ethernet_bridge") && !is_bridge_tool("sh"));
    // A bridge runs in its own directory, never the configuration file's.
    CHECK(bridge_working_directory(fs::path("/opt/vsil/bin/vehicle_sil_ethernet_bridge")) == fs::absolute(fs::path("/opt/vsil/bin")));
    CHECK(bridge_working_directory(fs::path("tools") / "vehicle_sil_vector_xl_bridge") == fs::absolute(fs::path("tools")));
}

TEST_CASE(a_target_reads_a_backend_only_where_the_table_has_the_word)
{
    const auto read = [](NetworkKind kind, std::string_view target, Os os = Os::Linux) {
        const ConnectTarget t = read_connect_target(kind, target, os);
        return std::string(t.none ? "none" : t.backend) + "|" + std::string(t.selector) + (t.prefixed ? "|prefixed" : "");
    };
    // The words of the table, lower case only; any other word before ':' is part of the selector.
    check_text(read(NetworkKind::Ethernet, "tap:vsil0"), "tap|vsil0|prefixed");
    check_text(read(NetworkKind::Ethernet, "packet:enp3s0"), "packet|enp3s0|prefixed");
    check_text(read(NetworkKind::Ethernet, "enp3s0"), "packet|enp3s0");
    check_text(read(NetworkKind::Ethernet, "eth0:1"), "packet|eth0:1");
    check_text(read(NetworkKind::Ethernet, "TAP:vsil0"), "packet|TAP:vsil0");
    check_text(read(NetworkKind::Ethernet, "pcap:x"), "packet|pcap:x");
    check_text(read(NetworkKind::Ethernet, "xl:x"), "packet|xl:x");  // a word of another kind
    check_text(read(NetworkKind::Ethernet, "npcap:tap:x"), "npcap|tap:x|prefixed");
    check_text(read(NetworkKind::Ethernet, "tap:"), "tap||prefixed");
    // none is the target that connects nothing only without a prefix.
    check_text(read(NetworkKind::Ethernet, "none"), "none|");
    check_text(read(NetworkKind::Ethernet, "packet:none"), "packet|none|prefixed");
    check_text(read(NetworkKind::Ethernet, "None"), "packet|None");
    // The default backend of each kind on each OS.
    check_text(read(NetworkKind::Ethernet, "Ethernet 2", Os::Windows), "npcap|Ethernet 2");
    check_text(read(NetworkKind::Can, "can0"), "socketcan|can0");
    check_text(read(NetworkKind::Can, "vcan0"), "socketcan|vcan0");
    check_text(read(NetworkKind::Can, "virtual:1"), "xl|virtual:1");
    check_text(read(NetworkKind::Can, "physical:2"), "xl|physical:2");
    check_text(read(NetworkKind::Can, "virtual:0"), "socketcan|virtual:0");
    check_text(read(NetworkKind::Can, "xl:virtual:1"), "xl|virtual:1|prefixed");
    check_text(read(NetworkKind::Can, "socketcan:can0"), "socketcan|can0|prefixed");
    check_text(read(NetworkKind::Can, "virtual:1", Os::Windows), "xl|virtual:1");
    check_text(read(NetworkKind::Can, "can0", Os::Windows), "xl|can0");
}

TEST_CASE(a_backend_that_the_os_cannot_use_is_a_backend_problem)
{
    const auto problem = [](NetworkKind kind, std::string_view target, Os os) {
        return backend_problem(kind, target, os).value_or("ok");
    };
    check_text(problem(NetworkKind::Ethernet, "tap:x", Os::Linux), "ok");
    check_text(problem(NetworkKind::Ethernet, "packet:x", Os::Linux), "ok");
    check_text(problem(NetworkKind::Ethernet, "x", Os::Windows), "ok");
    check_text(problem(NetworkKind::Ethernet, "none", Os::Windows), "ok");
    check_text(problem(NetworkKind::Ethernet, "tap:x", Os::Windows),
               "the backend 'tap' is not for Windows: the ethernet backends here are npcap (give the target without the "
               "prefix to take the default, npcap); vehicle_sil_launch connections <file> lists the targets of each");
    check_text(problem(NetworkKind::Ethernet, "npcap:Ethernet 2", Os::Linux),
               "the backend 'npcap' is not for Linux: the ethernet backends here are packet, tap (give the target without "
               "the prefix to take the default, packet); vehicle_sil_launch connections <file> lists the targets of each");
    check_text(problem(NetworkKind::Can, "socketcan:can0", Os::Windows),
               "the backend 'socketcan' is not for Windows: the can backends here are xl (give the target without the "
               "prefix to take the default, xl); vehicle_sil_launch connections <file> lists the targets of each");
    // The Vector XL backend is named on Linux too; the bridge for it is what is missing there.
    check_text(problem(NetworkKind::Can, "xl:virtual:1", Os::Linux), "ok");
    // target_problem() says it as well, before anything about the selector.
    CHECK(target_problem(NetworkKind::Ethernet, "tap:x", Os::Windows) == backend_problem(NetworkKind::Ethernet, "tap:x", Os::Windows));
}

TEST_CASE(target_problem_says_what_is_wrong_with_a_target)
{
    const auto problem = [](NetworkKind kind, std::string_view target) {
        return target_problem(kind, target, Os::Linux).value_or("ok");
    };
    check_text(problem(NetworkKind::Ethernet, "vEthernet (A)"), "ok");
    check_text(problem(NetworkKind::Ethernet, "\\Device\\NPF_{0A1B}"), "ok");
    check_text(problem(NetworkKind::Ethernet, "\xE3\x82\xA4\xE3\x83\xBC\xE3\x82\xB5\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88"), "ok");
    check_text(problem(NetworkKind::Ethernet, "none"), "ok");
    check_text(problem(NetworkKind::Ethernet, "packet:none"), "ok");
    check_text(problem(NetworkKind::Can, "none"), "ok");
    check_text(problem(NetworkKind::Ethernet, ""), "the target is empty");
    check_text(problem(NetworkKind::Ethernet, std::string(kMaxTargetBytes, 'a')), "ok");
    check_text(problem(NetworkKind::Ethernet, std::string(kMaxTargetBytes + 1, 'a')), "the target is longer than 256 bytes");
    check_text(problem(NetworkKind::Ethernet, "eth\xFF"), "the target is not UTF-8 text");
    check_text(problem(NetworkKind::Ethernet, "eth0\n"), "the target has a control character");
    check_text(problem(NetworkKind::Ethernet, "a\x7F"), "the target has a control character");
    // C1 and the controls of the direction of text: a terminal does not show them as they are.
    for (const char* control : {"a\xC2\x85", "a\xC2\x9F", "a\xE2\x80\xAE", "a\xE2\x80\xAA", "\xE2\x81\xA6x", "a\xE2\x81\xA9"})
    {
        check_text(problem(NetworkKind::Ethernet, control), "the target has a control character", control);
    }
    check_text(problem(NetworkKind::Ethernet, "a\xC2\xA0" "b\xE2\x80\xA9\xE2\x80\x9C"), "ok"); // no-break space, others

    check_text(problem(NetworkKind::Ethernet, "--all-frames"),
               "the target starts with '-', which the bridge would take for an option");
    check_text(problem(NetworkKind::Ethernet, "tap:-x"),
               "the target starts with '-', which the bridge would take for an option");
    check_text(problem(NetworkKind::Ethernet, "tap:"), "the target has nothing after 'tap:': give the interface name");
    check_text(problem(NetworkKind::Can, "socketcan:"),
               "the target has nothing after 'socketcan:': give the channel or interface name");
    check_text(problem(NetworkKind::Can, "virtual:1"), "ok");
    check_text(problem(NetworkKind::Can, "physical:12"), "ok");
    check_text(problem(NetworkKind::Can, "xl:physical:12"), "ok");
    // A SocketCAN interface needs no particular form.
    for (const char* interface : {"can0", "vcan0", "eth0", "usb:1", "socketcan:virtual:1", "Physical:1", "virtual:0"})
    {
        check_text(problem(NetworkKind::Can, interface), "ok", interface);
    }
    const std::string can = "a Vector XL target is virtual:<n> or physical:<n>, with the channel number <n> from 1";
    for (const char* wrong : {"xl:physical:0", "xl:physical:", "xl:physical", "xl:Physical:1", "xl:virtual:-1", "xl:virtual:1a",
                              "xl:usb:1", "xl:physical:1234567890", "xl:can0"})
    {
        check_text(problem(NetworkKind::Can, wrong), can, wrong);
    }
    // On Windows the default backend of a CAN network is the Vector XL one.
    check_text(target_problem(NetworkKind::Can, "can0", Os::Windows).value_or("ok"), can);
    check_text(target_problem(NetworkKind::Can, "virtual:1", Os::Windows).value_or("ok"), "ok");
}

TEST_CASE(bridge_arguments_of_an_ethernet_network)
{
    // The participants' MAC addresses, in their order, each once; the backend always.
    check_text(words(bridge_arguments(network("ETH", NetworkKind::Ethernet), "vEthernet (A)",
                                      {mac("02:00:00:00:00:02"), mac("02:00:00:00:00:01"), mac("02:00:00:00:00:02")},
                                      Os::Windows)),
               "--network ETH --backend npcap --interface vEthernet (A) --src-mac 02:00:00:00:00:02,02:00:00:00:00:01");
    check_text(words(bridge_arguments(network("ETH", NetworkKind::Ethernet), "eth0", {}, Os::Linux)),
               "--network ETH --backend packet --interface eth0");
    check_text(words(bridge_arguments(network("ETH", NetworkKind::Ethernet), "tap:vsil0", {}, Os::Linux)),
               "--network ETH --backend tap --interface vsil0");
    check_text(words(bridge_arguments(network("ETH", NetworkKind::Ethernet), "packet:tap", {}, Os::Linux)),
               "--network ETH --backend packet --interface tap");
    check_text(words(bridge_arguments(network("ETH", NetworkKind::Ethernet), "packet:none", {}, Os::Linux)),
               "--network ETH --backend packet --interface none");
    NetworkExternal types;
    types.ethertypes = {0x88b5, 0x0800};
    check_text(words(bridge_arguments(network("ETH", NetworkKind::Ethernet, types), "eth0", {mac("02:00:00:00:00:01")}, Os::Linux)),
               "--network ETH --backend packet --interface eth0 --ethertype 0x88b5,0x0800");
    NetworkExternal all;
    all.all_frames = true;
    check_text(words(bridge_arguments(network("ETH", NetworkKind::Ethernet, all), "eth0", {mac("02:00:00:00:00:01")}, Os::Linux)),
               "--network ETH --backend packet --interface eth0 --all-frames");
}

TEST_CASE(bridge_arguments_of_a_can_network)
{
    check_text(words(bridge_arguments(network("CAN1", NetworkKind::Can), "physical:2", {}, Os::Linux)),
               "--network CAN1 --xl-target physical --xl-channel 2");
    NetworkExternal rate;
    rate.bitrate = 250000;
    check_text(words(bridge_arguments(network("CAN1", NetworkKind::Can, rate), "virtual:1", {}, Os::Windows)),
               "--network CAN1 --xl-target virtual --xl-channel 1 --bitrate 250000");
    check_text(words(bridge_arguments(network("CAN1", NetworkKind::Can), "xl:virtual:1", {}, Os::Linux)),
               "--network CAN1 --xl-target virtual --xl-channel 1");
    // SocketCAN: the interface, then the bit rate if the file gives one.
    check_text(words(bridge_arguments(network("CAN1", NetworkKind::Can), "can0", {}, Os::Linux)), "--network CAN1 --interface can0");
    check_text(words(bridge_arguments(network("CAN1", NetworkKind::Can, rate), "socketcan:vcan0", {}, Os::Linux)),
               "--network CAN1 --interface vcan0 --bitrate 250000");
}

TEST_CASE(bridge_run_arguments_add_what_run_gives_after_the_checked_ones)
{
    // An adapter's name with a placeholder in it stays as it is (run replaces none for a bridge).
    NetworkOutside eth{"ETH", "vehicle_sil_ethernet_bridge", "/opt/vehicle_sil_ethernet_bridge", "lab {name} (2)", "bridge.ETH",
                       bridge_arguments(network("ETH", NetworkKind::Ethernet), "lab {name} (2)", {mac("02:00:00:00:00:01")}, Os::Linux)};
    check_text(words(bridge_run_arguments(eth, "silkit://127.0.0.1:41234", 30000)),
               "--network ETH --backend packet --interface lab {name} (2) --src-mac 02:00:00:00:00:01 --name bridge.ETH --registry-uri "
               "silkit://127.0.0.1:41234 --start-timeout-ms 30000 --stats-every-ms 1000");
    NetworkOutside can{"CAN1", "vehicle_sil_vector_xl_bridge", "/opt/vehicle_sil_vector_xl_bridge", "virtual:1", "bridge.CAN1",
                       bridge_arguments(network("CAN1", NetworkKind::Can), "virtual:1", {}, Os::Linux)};
    check_text(words(bridge_run_arguments(can, "silkit://127.0.0.1:8500", 5000)),
               "--network CAN1 --xl-target virtual --xl-channel 1 --name bridge.CAN1 --registry-uri silkit://127.0.0.1:8500 "
               "--start-timeout-ms 5000 --stats-every-ms 1000");
}

TEST_CASE(find_tool_looks_next_to_the_launcher_first_then_on_path)
{
    const fs::path base = scratch("order");
    const fs::path launcher = base / "launcher";
    const fs::path onPath = base / "on_path";
    fs::create_directories(launcher);
    fs::create_directories(onPath);
    const fs::path beside = tool_file(launcher, "vehicle_sil_ethernet_bridge");
    const fs::path listed = tool_file(onPath, "vehicle_sil_ethernet_bridge");
    const Environment path(process::utf8_from_path(onPath));
    CHECK(find_tool("vehicle_sil_ethernet_bridge", launcher) == beside);
    CHECK(find_tool("vehicle_sil_ethernet_bridge", {}) == listed);
    CHECK(find_tool("vehicle_sil_ethernet_bridge", base / "elsewhere") == listed);
    CHECK(!find_tool("vehicle_sil_vector_xl_bridge", launcher).has_value());
}

TEST_CASE(find_tool_skips_relative_path_entries_and_the_working_directory)
{
    const fs::path base = scratch("relative");
    fs::create_directories(base / "rel");
    static_cast<void>(tool_file(base / "rel", "vehicle_sil_vector_xl_bridge"));
    static_cast<void>(tool_file(base, "vehicle_sil_vector_xl_bridge"));
    // "rel" and "." would find the tool from the working directory: they are not looked at.
    const Environment path(std::string("rel") + kPathSeparator + "." + kPathSeparator, base);
    CHECK(!find_tool("vehicle_sil_vector_xl_bridge", {}).has_value());
    CHECK(!find_tool("vehicle_sil_vector_xl_bridge", base / "launcher").has_value());
}

TEST_CASE(find_tool_takes_a_runnable_file_only)
{
#ifndef _WIN32
    // A file without an execute permission next to the launcher does not hide the one on PATH.
    const fs::path base = scratch("not_runnable");
    fs::create_directories(base / "launcher");
    fs::create_directories(base / "bin");
    static_cast<void>(tool_file(base / "launcher", "vehicle_sil_ethernet_bridge", false));
    const fs::path listed = tool_file(base / "bin", "vehicle_sil_ethernet_bridge");
    const Environment path(process::utf8_from_path(base / "bin"));
    CHECK(find_tool("vehicle_sil_ethernet_bridge", base / "launcher") == listed);
#else
    // The name without ".exe" finds the executable.
    const fs::path base = scratch("exe");
    const fs::path beside = tool_file(base, "vehicle_sil_ethernet_bridge");
    const Environment path("");
    CHECK(find_tool("vehicle_sil_ethernet_bridge", base) == beside);
#endif
}

TEST_CASE(connect_device_key_names_the_device_a_target_uses)
{
    const auto key = [](NetworkKind kind, std::string_view target, Os os = Os::Linux) {
        return connect_device_key(kind, target, os);
    };
    CHECK(key(NetworkKind::Ethernet, "eth0") == "eth0");
    CHECK(key(NetworkKind::Ethernet, "eth0") != key(NetworkKind::Ethernet, "ETH0"));
    // Windows adapter names do not differ by case.
    CHECK(key(NetworkKind::Ethernet, "Ethernet 2", Os::Windows) == key(NetworkKind::Ethernet, "ETHERNET 2", Os::Windows));
    // The backend is not part of the device: one interface is one device through packet and tap.
    CHECK(key(NetworkKind::Ethernet, "enp3s0") == "enp3s0");
    CHECK(key(NetworkKind::Ethernet, "packet:enp3s0") == key(NetworkKind::Ethernet, "enp3s0"));
    CHECK(key(NetworkKind::Ethernet, "tap:enp3s0") == key(NetworkKind::Ethernet, "enp3s0"));
    CHECK(key(NetworkKind::Ethernet, "packet:none") == "none");
    CHECK(key(NetworkKind::Can, "physical:1") != key(NetworkKind::Can, "physical:2"));
    CHECK(key(NetworkKind::Can, "physical:1") == key(NetworkKind::Can, "physical:01"));
    CHECK(key(NetworkKind::Can, "physical:1") == key(NetworkKind::Can, "xl:physical:1"));
    CHECK(key(NetworkKind::Can, "virtual:1") == key(NetworkKind::Can, "virtual:2"));
    CHECK(key(NetworkKind::Can, "virtual:1") != key(NetworkKind::Can, "physical:1"));
    CHECK(key(NetworkKind::Can, "can0") == "can0" && key(NetworkKind::Can, "socketcan:can0") == "can0");
    CHECK(key(NetworkKind::Can, "can0") != key(NetworkKind::Can, "can1"));
    CHECK(key(NetworkKind::Ethernet, "none").empty());
    CHECK(key(NetworkKind::Can, "xl:usb:1").empty());
    CHECK(key(NetworkKind::Can, "can0", Os::Windows).empty());
    CHECK(key(NetworkKind::Ethernet, "tap:x", Os::Windows).empty());
    CHECK(key(NetworkKind::Lin, "x").empty());
}

TEST_CASE(networks_are_joined_through_participants)
{
    const std::vector<std::vector<std::string>> p{{"A", "C"}, {"C", "B"}, {"D"}};
    CHECK(networks_are_joined(p, "A", "C"));
    CHECK(networks_are_joined(p, "A", "B"));
    CHECK(!networks_are_joined(p, "A", "D"));
    CHECK(!networks_are_joined({}, "A", "B"));
}
