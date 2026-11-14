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
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

TEST_CASE(a_participant_given_no_configuration_gets_the_default)
{
    set_variable(kListenOnNetworkVariable.data(), nullptr);
    // Always: no dynamic simulation steps (the fixed step is the participant's own).
    const std::string fixed_step = R"("Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}})";
    // Loopback registry: loopback acceptors only (port 0), in the registry's address family.
    const std::string v4 = default_participant_configuration("silkit://127.0.0.1:8500");
    const std::string v6 = default_participant_configuration("silkit://[::1]:8500");
    CHECK(contains(v4, R"("AcceptorUris":[)") && contains(v4, R"("tcp://127.0.0.1:0")") && !contains(v4, "[::1]"));
    CHECK(contains(v6, R"("tcp://[::1]:0")") && !contains(v6, "127.0.0.1"));
    CHECK(!contains(v4, "0.0.0.0") && !contains(v6, "[::]"));
#ifdef _WIN32
    // Windows: TCP between the participants of a machine (no local-domain sockets).
    CHECK(v4 == R"({"Middleware":{"EnableDomainSockets":false,"AcceptorUris":["tcp://127.0.0.1:0"]},)" + fixed_step + "}");
    const std::string network = R"({"Middleware":{"EnableDomainSockets":false},)" + fixed_step + "}";
#else
    // Elsewhere the local-domain socket stays, under a new name each time.
    CHECK(v4.rfind(R"({"Middleware":{"AcceptorUris":["local://)", 0) == 0);
    CHECK(contains(v4, R"(.silkit","tcp://127.0.0.1:0"]},)" + fixed_step + "}"));
    CHECK(default_participant_configuration("silkit://127.0.0.1:8500") != v4);
    const std::string network = "{" + fixed_step + "}";
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

    CHECK(merged_participant_configuration("{}", "P", "silkit://0.0.0.0:1") == network);
    CHECK(merged_participant_configuration("", "P", "silkit://0.0.0.0:1") == network);
    CHECK(merged_participant_configuration(" {}\r\n", "P", "silkit://0.0.0.0:1") == network);
    CHECK(contains(merged_participant_configuration("{}", "P", "silkit://127.0.0.1:1"), "AcceptorUris"));
    // With members of the caller's: the same default, in one object.
    const std::string logging = R"("Logging":{"Sinks":[{"Type":"Stdout","Level":"Info"}]})";
    const std::string with = default_participant_configuration_with("silkit://localhost:1", logging);
    CHECK(contains(with, R"("tcp://127.0.0.1:0"]},)" + fixed_step + "," + logging + "}"));
    CHECK(default_participant_configuration_with("silkit://0.0.0.0:1", "") == network);
    CHECK(default_participant_configuration_with("silkit://0.0.0.0:1", logging) ==
          network.substr(0, network.size() - 1) + "," + logging + "}");
    // SIL Kit reads each of them, on every system.
    CHECK(SilKit::Config::ParticipantConfigurationFromString(default_participant_configuration("silkit://127.0.0.1:1")) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(default_participant_configuration("silkit://[::1]:1")) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(default_participant_configuration("silkit://0.0.0.0:1")) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(
              default_participant_configuration_with("silkit://localhost:1", logging)) != nullptr);
    CHECK(SilKit::Config::ParticipantConfigurationFromString(std::string(default_registry_configuration())) != nullptr);
}

namespace {

std::size_t occurrences(std::string_view text, std::string_view part)
{
    std::size_t n = 0;
    for (auto at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size()))
    {
        ++n;
    }
    return n;
}

/// A new directory for the files of one test, removed with it.
struct TemporaryDirectory
{
    explicit TemporaryDirectory(const std::string& name) :
      path(std::filesystem::temp_directory_path() /
           ("vsil-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    void write(const std::string& file, const std::string& text) const
    {
        std::ofstream out(path / file, std::ios::binary);
        out << text;
        CHECK(out.good());
    }
    std::filesystem::path path;
};

/// What SIL Kit reads from the configuration that a participant given `configuration` is made
/// with, as SIL Kit's JSON (`"Key": value`; a boolean is 1 or 0; SIL Kit's defaults are left out).
std::string read_by_silkit(std::string_view configuration, std::string_view name, std::string_view uri,
                           std::optional<bool> listen_on_network = std::nullopt)
{
    return SilKit::Config::ParticipantConfigurationToJson(SilKit::Config::ParticipantConfigurationFromString(
        merged_participant_configuration(configuration, name, uri, listen_on_network)));
}

const std::string kFixedStep = R"("Experimental": {"TimeSynchronization": {"DynamicSimulationStep": 0}})";
const std::string kLogging = "Logging:\n  Sinks:\n    - Type: Stdout\n      Level: Warn\n      Format: Json\n";

} // namespace

TEST_CASE(a_configuration_of_the_callers_gets_the_defaults_for_what_it_leaves_out)
{
    set_variable(kListenOnNetworkVariable.data(), nullptr);
    // Logging only: the acceptors and the fixed step are vehicle-sil's, the sink is the caller's.
    const std::string loopback = read_by_silkit(kLogging, "Engine", "silkit://127.0.0.1:8500");
    CHECK(contains(loopback, R"("tcp://127.0.0.1:0"])"));
    CHECK(!contains(loopback, "0.0.0.0"));
    CHECK(contains(loopback, kFixedStep));
    CHECK(contains(loopback, R"("Logging": {"Sinks": [{"Type": "Stdout","Level": "Warn"}]})"));
    // The caller's text itself stays: also what SIL Kit's JSON does not write (the sink's format).
    const std::string text = merged_participant_configuration(kLogging, "Engine", "silkit://127.0.0.1:8500");
    CHECK(text.rfind(kLogging, 0) == 0);
    CHECK(occurrences(text, "Middleware") == 1 && occurrences(text, "Experimental") == 1);
    // The address family and the caller's choice, as without a configuration.
    CHECK(contains(read_by_silkit(kLogging, "Engine", "silkit://[::1]:8500"), R"("tcp://[::1]:0"])"));
    CHECK(!contains(read_by_silkit(kLogging, "Engine", "silkit://127.0.0.1:8500", true), "AcceptorUris"));
    CHECK(!contains(read_by_silkit(kLogging, "Engine", "silkit://192.168.1.10:8500"), "AcceptorUris"));
    CHECK(contains(read_by_silkit(kLogging, "Engine", "silkit://192.168.1.10:8500"), kFixedStep));
    set_variable(kListenOnNetworkVariable.data(), "1");
    CHECK(!contains(read_by_silkit(kLogging, "Engine", "silkit://127.0.0.1:8500"), "AcceptorUris"));
    CHECK(contains(read_by_silkit(kLogging, "Engine", "silkit://127.0.0.1:8500", false), "AcceptorUris"));
    set_variable(kListenOnNetworkVariable.data(), nullptr);
#ifdef _WIN32
    CHECK(contains(loopback, R"("EnableDomainSockets": 0)"));
    CHECK(contains(read_by_silkit(kLogging, "Engine", "silkit://192.168.1.10:8500"), R"("Middleware": {"EnableDomainSockets": 0})"));
    // SIL Kit's JSON leaves its default (true) out: a true written there becomes false as well.
    CHECK(contains(read_by_silkit("Middleware:\n  EnableDomainSockets: true\n", "Engine", "silkit://10.0.0.1:1"),
                   R"("EnableDomainSockets": 0)"));
#else
    CHECK(contains(loopback, R"("AcceptorUris": ["local://)"));
    CHECK(!contains(loopback, "EnableDomainSockets"));
    CHECK(!contains(read_by_silkit(kLogging, "Engine", "silkit://192.168.1.10:8500"), "Middleware"));
#endif

    // The caller's acceptors and the rest of its Middleware stay; no acceptor is added to them.
    const std::string own_text =
        "Middleware:\n  AcceptorUris: ['tcp://0.0.0.0:8601']\n  ConnectAttempts: 9\n  EnableDomainSockets: false\n"
        "HealthCheck:\n  SoftResponseTimeout: 700\n  HardResponseTimeout: 5000\n"
        "Logging:\n  Sinks:\n    - Type: File\n      Level: Debug\n      LogName: engine_log\n";
    const std::string own = read_by_silkit(own_text, "Engine", "silkit://127.0.0.1:8500");
    CHECK(contains(own, R"("AcceptorUris": ["tcp://0.0.0.0:8601"])"));
    CHECK(occurrences(own, "AcceptorUris") == 1 && !contains(own, "127.0.0.1") && !contains(own, "local://"));
    CHECK(contains(own, R"("ConnectAttempts": 9)") && occurrences(own, R"("EnableDomainSockets": 0)") == 1);
    CHECK(contains(own, R"("SoftResponseTimeout": 700)") && contains(own, R"("HardResponseTimeout": 5000)"));
    CHECK(contains(own, R"("Type": "File")") && contains(own, R"("LogName": "engine_log")") && contains(own, R"("Level": "Debug")"));
    CHECK(contains(own, kFixedStep));

    // A configuration that leaves nothing out is used as it is written.
    const std::string complete = "# every member that vehicle-sil would add\nMiddleware: {AcceptorUris: ['tcp://0.0.0.0:8601'], "
                                 "EnableDomainSockets: false}\nExperimental:\n  TimeSynchronization:\n    "
                                 "DynamicSimulationStep: false\n";
    CHECK(merged_participant_configuration(complete, "Engine", "silkit://127.0.0.1:8500") == complete);

    // JSON, members in one line, a member with nothing set yet: the added ones go where they belong.
    const std::string json =
        read_by_silkit(R"({"Logging":{"Sinks":[{"Type":"Stdout"}]},"Middleware":{"ConnectAttempts":3}})", "Engine", "silkit://127.0.0.1:1");
    CHECK(contains(json, R"("Middleware": {"ConnectAttempts": 3,)") && contains(json, R"("AcceptorUris": [)") && contains(json, kFixedStep));
    CHECK(contains(json, R"("Logging": {"Sinks": [{"Type": "Stdout"}]})"));
    const std::string flow = read_by_silkit("Middleware: {ConnectAttempts: 4}\nExperimental: {TimeSynchronization: {AnimationFactor: 2.0}}\n"
                                            "Description: \"a: 'b' # c\"\n",
                                            "Engine", "silkit://127.0.0.1:1");
    CHECK(contains(flow, R"("Middleware": {"ConnectAttempts": 4,)") && contains(flow, R"("AcceptorUris": [)"));
    CHECK(contains(flow, R"("Experimental": {"TimeSynchronization": {"AnimationFactor": 2,"DynamicSimulationStep": 0}})"));
    // An empty list of acceptors is none: it is replaced, not written twice.
    const std::string empty_list = merged_participant_configuration("Middleware:\n  AcceptorUris: []\n", "Engine", "silkit://127.0.0.1:1");
    CHECK(occurrences(empty_list, "AcceptorUris") == 1 && contains(empty_list, "tcp://127.0.0.1:0"));
    CHECK(contains(read_by_silkit("Middleware:\n  AcceptorUris: []\n", "Engine", "silkit://127.0.0.1:1"), R"("tcp://127.0.0.1:0"])"));
    // Only comments: the default.
    CHECK(contains(read_by_silkit("# nothing yet\n", "Engine", "silkit://127.0.0.1:1"), R"("tcp://127.0.0.1:0"])"));
    // A false of the caller's stays the only one.
    const std::string opted_out = merged_participant_configuration(
        R"({"Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}}})", "Engine", "silkit://10.0.0.1:1");
    CHECK(occurrences(opted_out, "DynamicSimulationStep") == 1);
    CHECK(contains(read_by_silkit(R"({"Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}}})", "Engine", "silkit://10.0.0.1:1"),
                   kFixedStep));

    // What SIL Kit refuses is SIL Kit's error.
    CHECK(unit::throws<SilKit::ConfigurationError>(
        [] { static_cast<void>(merged_participant_configuration("Logging: [not, a, map", "Engine", "silkit://10.0.0.1:1")); }));
    CHECK(unit::throws<SilKit::ConfigurationError>([] {
        static_cast<void>(merged_participant_configuration("Middleware:\n  ConnectAttempts: many\n", "Engine", "silkit://10.0.0.1:1"));
    }));
}

TEST_CASE(a_configuration_that_names_another_participant_or_registry_is_refused)
{
    set_variable(kListenOnNetworkVariable.data(), nullptr);
    const std::string uri = "silkit://192.168.1.10:8500";
    // The name: the same one passes (also UTF-8, and one that JSON escapes), another one is refused.
    CHECK(contains(read_by_silkit("ParticipantName: Engine\n", "Engine", uri), R"("ParticipantName": "Engine")"));
    CHECK(contains(read_by_silkit("ParticipantName: \"\xE3\x82\xA8\xE3\x83\xB3\xE3\x82\xB8\xE3\x83\xB3\"\n",
                                  "\xE3\x82\xA8\xE3\x83\xB3\xE3\x82\xB8\xE3\x83\xB3", uri),
                   "ParticipantName"));
    CHECK(contains(read_by_silkit(R"({"ParticipantName":"a \"b\" \\ c"})", R"(a "b" \ c)", uri), "ParticipantName"));
    CHECK(unit::throws_with<std::invalid_argument>(
        [&] { static_cast<void>(merged_participant_configuration("ParticipantName: Motor\n", "Engine", uri)); },
        "the SIL Kit configuration of the participant \"Engine\" sets ParticipantName to \"Motor\": SIL Kit would join the "
        "simulation under that name, and whatever waits for \"Engine\" (the controller's required participants, "
        "vehicle_sil_launch) would wait until its timeout. Remove ParticipantName from the configuration (the program gives "
        "the name; --name of an ECU's executable changes it), or set it to \"Engine\""));
    CHECK(unit::throws<std::invalid_argument>(
        [&] { static_cast<void>(merged_participant_configuration("ParticipantName: engine\n", "Engine", uri)); }));

    // The registry URI: the same one passes, another one is refused; not compared when none is known.
    const std::string with_uri = "Middleware:\n  RegistryUri: silkit://192.168.1.10:8500\n";
    CHECK(contains(read_by_silkit(with_uri, "Engine", uri), R"("RegistryUri": "silkit://192.168.1.10:8500")"));
    CHECK(contains(read_by_silkit(with_uri, "Engine", ""), R"("RegistryUri": "silkit://192.168.1.10:8500")"));
    CHECK(unit::throws_with<std::invalid_argument>(
        [&] { static_cast<void>(merged_participant_configuration(with_uri, "Engine", "silkit://127.0.0.1:8500")); },
        "the SIL Kit configuration of the participant \"Engine\" sets Middleware.RegistryUri to "
        "\"silkit://192.168.1.10:8500\", but the participant is told to join \"silkit://127.0.0.1:8500\": SIL Kit would "
        "connect to the configuration's registry. Remove Middleware.RegistryUri from the configuration (the program gives "
        "the URI: --registry-uri, or registry_uri of vehicle_sil_launch's configuration), or write the same URI in both"));

    // Dynamic simulation steps: asked for is refused before anything runs.
    const std::string dynamic = "Experimental:\n  TimeSynchronization:\n    DynamicSimulationStep: true\n";
    CHECK(unit::throws_with<std::invalid_argument>(
        [&] { static_cast<void>(merged_participant_configuration(dynamic, "Engine", uri)); },
        "the SIL Kit configuration of the participant \"Engine\" sets Experimental.TimeSynchronization.DynamicSimulationStep "
        "to true: SIL Kit would cut this participant's steps down to the smallest step of the simulation, and vehicle-sil's "
        "participants run on their own fixed step. Remove the key from the configuration (vehicle-sil then sets it to false "
        "for this participant), or set it to false"));

    // The participant itself refuses the same, before it connects (nothing listens at this URI).
    ParticipantRuntimeOptions options;
    options.participant_name = "Engine";
    options.registry_uri = "silkit://127.0.0.1:1";
    options.participant_configuration = "ParticipantName: Motor\n";
    CHECK(unit::throws_with<std::invalid_argument>([&] { ParticipantRuntime participant(options); },
                                                   "sets ParticipantName to \"Motor\""));
    options.participant_configuration = with_uri;
    CHECK(unit::throws_with<std::invalid_argument>([&] { ParticipantRuntime participant(options); },
                                                   "sets Middleware.RegistryUri to"));
    options.participant_configuration = dynamic;
    CHECK(unit::throws_with<std::invalid_argument>([&] { ParticipantRuntime participant(options); },
                                                   "DynamicSimulationStep to true"));
}

TEST_CASE(a_configuration_with_includes_gets_the_defaults_for_what_all_its_files_leave_out)
{
    set_variable(kListenOnNetworkVariable.data(), nullptr);
    const TemporaryDirectory directory("includes");
    directory.write("middleware.silkit.yaml", "Middleware:\n  AcceptorUris: ['tcp://127.0.0.1:8611']\n  ConnectAttempts: 7\n");
    directory.write("root.silkit.yaml", "Includes:\n  Files: [middleware.silkit.yaml]\nLogging:\n  Sinks:\n    - Type: Stdout\n");

    // As a text: the included file is found by the search path that the text gives. Its acceptors
    // count: none is added (SIL Kit would refuse acceptors in two files).
    const std::string text = "Includes:\n  SearchPathHints: ['" + directory.path.generic_string() +
                             "']\n  Files: [middleware.silkit.yaml]\nLogging:\n  Sinks:\n    - Type: Stdout\n";
    const std::string merged = read_by_silkit(text, "Engine", "silkit://127.0.0.1:8500");
    CHECK(contains(merged, R"("AcceptorUris": ["tcp://127.0.0.1:8611"])") && occurrences(merged, "AcceptorUris") == 1);
    CHECK(contains(merged, R"("ConnectAttempts": 7)") && contains(merged, R"("Type": "Stdout")") && contains(merged, kFixedStep));
    // A name in an included file is the configuration's name as well.
    directory.write("name.silkit.yaml", "ParticipantName: Motor\n");
    CHECK(unit::throws_with<std::invalid_argument>(
        [&] {
            static_cast<void>(merged_participant_configuration("Includes:\n  SearchPathHints: ['" + directory.path.generic_string() +
                                                                   "']\n  Files: [name.silkit.yaml]\n",
                                                               "Engine", "silkit://127.0.0.1:8500"));
        },
        "sets ParticipantName to \"Motor\""));

    // As a file: the included file is found beside it, from any working directory.
    const std::string from_file = participant_configuration_from_file((directory.path / "root.silkit.yaml").string());
    const std::string again = read_by_silkit(from_file, "Engine", "silkit://127.0.0.1:8500");
    CHECK(contains(again, R"("AcceptorUris": ["tcp://127.0.0.1:8611"])") && occurrences(again, "AcceptorUris") == 1);
    CHECK(contains(again, R"("ConnectAttempts": 7)") && contains(again, R"("Type": "Stdout")") && contains(again, kFixedStep));
    // A file without Includes is its text.
    directory.write("plain.silkit.yaml", kLogging);
    CHECK(participant_configuration_from_file((directory.path / "plain.silkit.yaml").string()) == kLogging);

    // No such file: said with the path.
    const std::string missing = (directory.path / "none.silkit.yaml").string();
    CHECK(unit::throws_with<std::invalid_argument>([&] { static_cast<void>(participant_configuration_from_file(missing)); },
                                                   "there is no SIL Kit participant configuration file \"" + missing +
                                                       "\": give the path of a YAML or JSON file that exists"));
    CHECK(unit::throws_with<std::invalid_argument>(
        [] { static_cast<void>(participant_configuration_from_file("vsil-none.silkit.yaml")); },
        "there is no SIL Kit participant configuration file \"vsil-none.silkit.yaml\" in the working directory \""));
    CHECK(unit::throws<std::invalid_argument>(
        [&] { static_cast<void>(participant_configuration_from_file(directory.path.string())); })); // a directory
    // A file that includes one that is not there: SIL Kit's error, when the text is used.
    directory.write("broken.silkit.yaml", "Includes:\n  Files: [not-there.silkit.yaml]\n");
    CHECK(unit::throws<SilKit::ConfigurationError>([&] {
        static_cast<void>(merged_participant_configuration(
            participant_configuration_from_file((directory.path / "broken.silkit.yaml").string()), "Engine", ""));
    }));
}

TEST_CASE(a_participant_with_a_configuration_of_its_own_listens_on_loopback_only)
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
    // Nothing about where to listen: without the merge, SIL Kit would listen on every address.
    options.participant_configuration = "ParticipantName: Listener\nMiddleware:\n  ConnectAttempts: 3\n";
    ParticipantRuntime participant(std::move(options));
    const auto found = listening_sockets();
    CHECK(found.size() > before);
    CHECK(count(found, Listen::Any) == 0);
    CHECK(count(found, Listen::Other) == 0);
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
