// SPDX-License-Identifier: MIT
//
// vehicle-sil's SIL Kit participant configuration (participant_runtime.hpp): the text it makes,
// and where a participant with it listens (the listening sockets of this process: Linux
// /proc/net/tcp*, Windows GetExtendedTcpTable()).
#ifdef _WIN32
// Before anything that may include <windows.h>.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on
#endif

#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace vehicle_sil;

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

void set_variable(const char* name, const char* value)
{
#ifdef _WIN32
    CHECK(_putenv_s(name, value == nullptr ? "" : value) == 0);
#else
    CHECK(value == nullptr ? ::unsetenv(name) == 0 : ::setenv(name, value, 1) == 0);
#endif
}

/// Where a listening socket listens.
enum class Listen
{
    Loopback,
    Any,
    Other,
};

#if defined(__linux__) || defined(_WIN32)
constexpr bool kCanSeeListeners = true;
#else
constexpr bool kCanSeeListeners = false;
#endif

#ifdef __linux__
/// The TCP sockets that this process listens on (the "socket:[inode]" descriptors of the process
/// that /proc/net/tcp and tcp6 list in state 0A, LISTEN).
std::vector<Listen> listening_sockets()
{
    std::set<std::string> inodes;
    for (const auto& fd : std::filesystem::directory_iterator("/proc/self/fd"))
    {
        std::error_code error;
        const std::string target = std::filesystem::read_symlink(fd.path(), error).string();
        if (!error && target.rfind("socket:[", 0) == 0)
        {
            inodes.insert(target.substr(8, target.size() - 9));
        }
    }
    std::vector<Listen> found;
    for (const char* table : {"/proc/net/tcp", "/proc/net/tcp6"})
    {
        std::ifstream in(table);
        std::string line;
        std::getline(in, line); // the heading
        while (std::getline(in, line))
        {
            std::istringstream fields(line);
            std::string slot, local, remote, state, queues, timer, retransmits, uid, timeout, inode;
            fields >> slot >> local >> remote >> state >> queues >> timer >> retransmits >> uid >> timeout >> inode;
            if (state != "0A" || inodes.count(inode) == 0)
            {
                continue;
            }
            const std::string address = local.substr(0, local.find(':'));
            // IPv4: one 32-bit word in the host's order (little-endian: the first byte last).
            // IPv6: four such words; ::1 is 0...0 01000000.
            if (address.find_first_not_of('0') == std::string::npos)
            {
                found.push_back(Listen::Any);
            }
            else if ((address.size() == 8 && address.substr(6) == "7F") ||
                     address == "00000000000000000000000001000000")
            {
                found.push_back(Listen::Loopback);
            }
            else
            {
                found.push_back(Listen::Other);
            }
        }
    }
    return found;
}
#elif defined(_WIN32)
/// The TCP sockets that this process listens on (GetExtendedTcpTable(), both families).
std::vector<Listen> listening_sockets()
{
    std::vector<Listen> found;
    const DWORD self = GetCurrentProcessId();
    for (const ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)})
    {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        std::vector<unsigned char> buffer(size + 1024);
        size = static_cast<DWORD>(buffer.size());
        if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        {
            throw std::runtime_error("GetExtendedTcpTable failed");
        }
        if (family == AF_INET)
        {
            const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
            {
                const auto& row = table->table[i];
                if (row.dwOwningPid != self)
                {
                    continue;
                }
                const auto* bytes = reinterpret_cast<const unsigned char*>(&row.dwLocalAddr); // network order
                found.push_back(row.dwLocalAddr == 0 ? Listen::Any : bytes[0] == 127 ? Listen::Loopback : Listen::Other);
            }
        }
        else
        {
            const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i)
            {
                const auto& row = table->table[i];
                if (row.dwOwningPid != self)
                {
                    continue;
                }
                const unsigned char* a = row.ucLocalAddr;
                bool zero = true;
                for (int k = 0; k < 15; ++k)
                {
                    zero = zero && a[k] == 0;
                }
                found.push_back(zero && a[15] == 0 ? Listen::Any : zero && a[15] == 1 ? Listen::Loopback : Listen::Other);
            }
        }
    }
    return found;
}
#else
std::vector<Listen> listening_sockets()
{
    return {};
}
#endif

std::size_t count(const std::vector<Listen>& found, Listen what)
{
    return static_cast<std::size_t>(std::count(found.begin(), found.end(), what));
}

} // namespace

TEST_CASE(a_registry_uri_is_loopback_by_its_written_address)
{
    // Every numeric form that getaddrinfo() reads, and localhost with or without the root's dot.
    for (const char* uri :
         {"silkit://127.0.0.1:8500", "silkit://127.1.2.3:0", "silkit://localhost:8500", "silkit://LocalHost:1", "silkit://localhost.:1",
          "silkit://[::1]:8500", "silkit://127.0.0.1:8500/simulation", "silkit://127.1:8500", "silkit://127.0.1:8500",
          "silkit://2130706433:8500", "silkit://0x7f.1:8500", "silkit://0177.0.0.1:8500", "silkit://[0:0:0:0:0:0:0:1]:8500",
          "silkit://[0000::0001]:1", "silkit://[::ffff:127.0.0.1]:8500", "silkit://[::FFFF:127.9.9.9]:1", "silkit://[::1%lo]:1", "silkit://127.65536:1"})
    {
        CHECK(is_loopback_registry_uri(uri));
    }
    for (const char* uri : {"silkit://0.0.0.0:8500", "silkit://[::]:8500", "silkit://192.168.1.10:8500", "silkit://host:8500",
                            "silkit://127.0.0.1.5:8500", "silkit://1270.0.0.1:8500", "silkit://localhost.example:1",
                            "tcp://127.0.0.1:8500", "", "silkit://[::1:8500", "silkit://128.0.0.1:1", "silkit://256.1:1",
                            "silkit://127.256.0.1:1", "silkit://127.0.0.256:1", "silkit://4294967296:1", "silkit://0x:1", "silkit://[::ffff:10.0.0.1]:1",
                            "silkit://[::2]:1", "silkit://[1::1]:1", "silkit://[::1::]:1", "silkit://[0:0:0:0:0:0:0:0:1]:1", "silkit://[0:0:0:0::0:0:0:1]:1",
                            "silkit://[::127.0.0.1]:1", "silkit://localhost..:1", "silkit://127.0.0.1.:1"})
    {
        CHECK(!is_loopback_registry_uri(uri));
    }
}

TEST_CASE(a_participant_given_no_configuration_gets_the_default_and_one_of_its_own_stays)
{
    set_variable(kListenOnNetworkVariable.data(), nullptr);
    // Loopback registry: loopback acceptors only (port 0), in the registry's address family.
    const std::string v4 = default_participant_configuration("silkit://127.0.0.1:8500");
    const std::string v6 = default_participant_configuration("silkit://[::1]:8500");
    CHECK(contains(v4, R"("AcceptorUris":[)") && contains(v4, R"("tcp://127.0.0.1:0")") && !contains(v4, "[::1]"));
    CHECK(contains(v6, R"("tcp://[::1]:0")") && !contains(v6, "127.0.0.1"));
    CHECK(!contains(v4, "0.0.0.0") && !contains(v6, "[::]"));
#ifdef _WIN32
    // Windows: TCP between the participants of a machine (no local-domain sockets).
    CHECK(v4 == R"({"Middleware":{"EnableDomainSockets":false,"AcceptorUris":["tcp://127.0.0.1:0"]}})");
    const std::string network = R"({"Middleware":{"EnableDomainSockets":false}})";
#else
    // Elsewhere the local-domain socket stays, under a new name each time.
    CHECK(v4.rfind(R"({"Middleware":{"AcceptorUris":["local://)", 0) == 0);
    CHECK(contains(v4, R"(.silkit","tcp://127.0.0.1:0"]}})"));
    CHECK(default_participant_configuration("silkit://127.0.0.1:8500") != v4);
    const std::string network = "{}";
#endif
    // A registry that other computers may reach: SIL Kit's acceptors (every address).
    CHECK(default_participant_configuration("silkit://0.0.0.0:8500") == network);
    CHECK(default_participant_configuration("silkit://192.168.1.10:8500") == network);
    CHECK(default_participant_configuration("silkit://127.0.0.1:8500", true) == network);
    set_variable(kListenOnNetworkVariable.data(), "1");
    CHECK(default_participant_configuration("silkit://127.0.0.1:8500") == network);
    // A value given decides alone.
    CHECK(contains(default_participant_configuration("silkit://127.0.0.1:8500", false), "AcceptorUris"));
    set_variable(kListenOnNetworkVariable.data(), "0");
    CHECK(contains(default_participant_configuration("silkit://127.0.0.1:8500"), "AcceptorUris"));
    CHECK(default_participant_configuration("silkit://127.0.0.1:8500", true) == network);
    // Only 1 and 0: anything else is an error that names the variable and the value.
    for (const char* value : {"yes", "true", "on", "1 ", "2"})
    {
        set_variable(kListenOnNetworkVariable.data(), value);
        CHECK(unit::throws_with<std::invalid_argument>([] { static_cast<void>(default_participant_configuration("silkit://127.0.0.1:1")); },
                                                       "VEHICLE_SIL_LISTEN_ON_NETWORK must be 1 (listen on every address) or 0"));
        // Not read when the URI or the caller decides.
        CHECK(default_participant_configuration("silkit://0.0.0.0:1") == network);
        CHECK(contains(default_participant_configuration("silkit://127.0.0.1:1", false), "AcceptorUris"));
    }
    set_variable(kListenOnNetworkVariable.data(), nullptr);
    // The address family of ::1 in any form; 127.x mapped to IPv6 listens on 127.0.0.1.
    CHECK(contains(default_participant_configuration("silkit://[0:0:0:0:0:0:0:1]:1"), R"("tcp://[::1]:0")"));
    CHECK(contains(default_participant_configuration("silkit://[::ffff:127.0.0.1]:1"), R"("tcp://127.0.0.1:0")"));
    CHECK(contains(default_participant_configuration("silkit://127.1:1"), R"("tcp://127.0.0.1:0")"));
    // A registry: never acceptors (it listens at its URI), and TCP only on every system.
    CHECK(std::string(default_registry_configuration()) == R"({"Middleware":{"EnableDomainSockets":false}})");

    CHECK(participant_configuration_or_default("{}", "silkit://0.0.0.0:1") == network);
    CHECK(participant_configuration_or_default("", "silkit://0.0.0.0:1") == network);
    CHECK(participant_configuration_or_default(" {}\r\n", "silkit://0.0.0.0:1") == network);
    CHECK(contains(participant_configuration_or_default("{}", "silkit://127.0.0.1:1"), "AcceptorUris"));
    const std::string own = "Logging:\n  Sinks:\n    - Type: Stdout\n";
    CHECK(participant_configuration_or_default(own, "silkit://127.0.0.1:1") == own);
    // With members of the caller's: the same default, in one object.
    const std::string logging = R"("Logging":{"Sinks":[{"Type":"Stdout","Level":"Info"}]})";
    const std::string with = default_participant_configuration_with("silkit://localhost:1", logging);
    CHECK(contains(with, R"("tcp://127.0.0.1:0"]},)" + logging + "}"));
    CHECK(default_participant_configuration_with("silkit://0.0.0.0:1", "") == network);
    CHECK(default_participant_configuration_with("silkit://0.0.0.0:1", logging) ==
          (network == "{}" ? "{" + logging + "}" : network.substr(0, network.size() - 1) + "," + logging + "}"));
    // SIL Kit reads each of them, on every system.
    CHECK(SilKit::Config::ParticipantConfigurationFromString(default_participant_configuration("silkit://127.0.0.1:1")) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(default_participant_configuration("silkit://[::1]:1")) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(default_participant_configuration("silkit://0.0.0.0:1")) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(
              default_participant_configuration_with("silkit://localhost:1", logging)) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(std::string(default_registry_configuration())) != nullptr);
}

TEST_CASE(a_participant_of_a_loopback_registry_listens_on_loopback_only)
{
    if (!kCanSeeListeners)
    {
        throw unit::Skipped("this system's listening sockets are not read here");
    }
    set_variable(kListenOnNetworkVariable.data(), nullptr);
    unit::InProcessRegistry registry; // at 127.0.0.1
    const std::size_t before = listening_sockets().size();
    ParticipantRuntimeOptions options;
    options.participant_name = "Listener";
    options.registry_uri = registry.uri;
    ParticipantRuntime participant(std::move(options));
    const auto found = listening_sockets();
    CHECK(found.size() > before); // the participant's own
    CHECK(count(found, Listen::Any) == 0);
    CHECK(count(found, Listen::Other) == 0);
}

TEST_CASE(a_participant_told_to_listen_on_the_network_listens_on_every_address)
{
    // That the check above sees a socket on every address when there is one. Not on Windows: there
    // its firewall would ask whether to let the test program listen.
#ifdef _WIN32
    throw unit::Skipped("listening on every address would make Windows's firewall ask");
#endif
    if (!kCanSeeListeners)
    {
        throw unit::Skipped("this system's listening sockets are not read here");
    }
    unit::InProcessRegistry registry;
    ParticipantRuntimeOptions options;
    options.participant_name = "Listener";
    options.registry_uri = registry.uri;
    options.listen_on_network = true;
    ParticipantRuntime participant(std::move(options));
    CHECK(count(listening_sockets(), Listen::Any) > 0);
}
