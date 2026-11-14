// SPDX-License-Identifier: MIT
//
// Process level tests of the Ethernet bridge executable. The first seven use the executable built
// with fake links (fake_link_factory.cpp: --interface fake0 is a wire with a device that answers):
//   echo-roundtrip     registry + bridge + SIL Kit peer (eth_bridge_it_peer --mode ask). The peer
//                      checks every answer; the launcher checks the bridge's start line and its
//                      statistics (forwarded counts, the one frame of another EtherType filtered,
//                      no problem counted, no warning).
//   list-interfaces    --list-interfaces prints the factory's list: exit 0.
//   bad-usage          an unknown --backend: exit 4, with the error and where the options are,
//                      not the whole usage text.
//   no-registry        no registry at the address: exit 3, with the address and what to do.
//   non-ascii-arguments  names in another script (the participant, the network, the interface)
//                      arrive as UTF-8, on Windows too: the link's message has the interface's
//                      name as it was given.
//   no-filter          --backend packet without --ethertype, --src-mac or --all-frames: exit 4.
//   no-interface       an interface the link cannot open: exit 3 with the link's message.
//   link-fault         the link reports a fault while the bridge runs: the bridge reports it and
//                      exits 1 on its own, with its statistics.
//   stop-request       registry + bridge: once the bridge says "Ctrl+C to stop", the launcher asks
//                      it to stop (POSIX SIGTERM, Windows Ctrl+Break to its process group). The
//                      bridge stops in order: one statistics line, "stopped", exit code 0. Skipped
//                      on Windows without a console the processes share.
//   backend-not-here   the real executable (--real-bridge): a backend that cannot be used on this
//                      operating system ends with exit 3 and says what to use instead, before
//                      SIL Kit is touched; --list-interfaces lists the interfaces. The pcap
//                      library of this computer may be installed or not: with it the list is
//                      printed, without it the bridge says how to install it (exit 3).
// The pcap-* scenarios use the executable with its own links of the operating system and the fake
// pcap library in place of the installed one (--pcap-bridge; tests/eth_link/fake_pcap), with
// --backend npcap on Windows and --backend pcap on Linux:
//   pcap-roundtrip     as echo-roundtrip, through the fake library's device that answers.
//   pcap-list          --list-interfaces prints the library's devices: exit 0.
//   pcap-no-library    the library is not there: exit 3 with the way to install it, before
//                      SIL Kit is touched.
//   pcap-old-library   the library lacks a function the bridge needs: exit 3, the same.
//   pcap-no-interface  a device the library does not have: exit 3 with the devices it has.
//   pcap-fault         reading fails while the bridge runs: FAIL, the statistics, exit 1.
#include "launcher_support.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;
namespace cli = vehicle_sil::cli;

namespace {

/// A log without '\r': what a process prints in text mode has "\r\n" on Windows, and the texts
/// this launcher looks for have "\n".
std::string read_text(const fs::path& path)
{
    std::string text = read_file(path);
    std::erase(text, '\r');
    return text;
}

/// The scenario cannot run on this machine: main() exits with 77 (CTest's SKIP_RETURN_CODE).
struct Skip : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

const char* const kNetwork = "ETH_IT";
const char* const kParticipant = "EthernetBridge";

/// Every key of the statistics line, in its order.
const char* const kStatisticsKeys[] = {"silkit_to_ext", "ext_to_silkit", "own_tx_dropped", "loop_suspected", "mac_unguarded",
                                       "filtered_to_ext", "filtered_from_ext", "oversize", "malformed", "ext_overruns",
                                       "ext_queue_full", "ext_tx_failures", "silkit_tx_failures",
                                       "ext_checksum_incomplete"};

struct Options
{
    fs::path registry, bridge, realBridge, pcapBridge, fakePcapDir, peer, workDir;
    std::string scenario;
    std::chrono::milliseconds timeout{60000};
};

Options parse(int argc, char** argv)
{
    const cli::Args args(argc, argv);
    Options options;
    options.registry = args.required("registry");
    options.bridge = args.required("bridge");
    options.realBridge = args.required("real-bridge");
    options.pcapBridge = args.required("pcap-bridge");
    options.fakePcapDir = args.required("fake-pcap-dir");
    options.peer = args.required("peer");
    options.workDir = args.required("work-dir");
    options.scenario = args.required("scenario");
    // An upper bound of waiting: scaled by VEHICLE_SIL_TEST_TIMEOUT_SCALE.
    options.timeout = vehicle_sil::test::scaled_timeout(args.millis("timeout-ms", options.timeout));
    return options;
}

/// Value of "key=<number>" in `text`, or nullopt.
std::optional<unsigned long long> stat(const std::string& text, const std::string& key)
{
    const std::regex pattern("\\b" + key + "=([0-9]+)");
    std::smatch match;
    if (!std::regex_search(text, match, pattern))
    {
        return std::nullopt;
    }
    return std::stoull(match[1].str());
}

std::vector<std::string> bridge_args(const std::string& registryUri, const std::string& interface_name,
                                     std::vector<std::string> more = {"--ethertype", "0x88b5"}, const std::string& backend = "packet")
{
    std::vector<std::string> args{"--registry-uri", registryUri, "--participant", kParticipant, "--network", kNetwork,
                                  "--backend",      backend,     "--interface",   interface_name};
    args.insert(args.end(), more.begin(), more.end());
    return args;
}

// The pcap backend of this operating system, and how its messages name the library.
#ifdef _WIN32
const char* const kPcapBackend = "npcap";
const char* const kPcapLibrary = "Npcap";
const char* const kPcapLink = "npcap 'fake0' (network adapter through Npcap)";
const char* const kLibrarySuffix = ".dll";
#else
const char* const kPcapBackend = "pcap";
const char* const kPcapLibrary = "libpcap";
const char* const kPcapLink = "pcap 'fake0' (network interface through libpcap)";
const char* const kLibrarySuffix = ".so";
#endif

/// Sets a variable in this process's environment: the processes it starts get it.
void set_environment(const std::string& name, const std::string& value)
{
#ifdef _WIN32
    const bool ok = _putenv_s(name.c_str(), value.c_str()) == 0;
#else
    const bool ok = ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
    if (!ok)
    {
        throw std::runtime_error("cannot set the environment variable " + name);
    }
}

/// Makes the bridge of the pcap-* scenarios load the fake pcap library `name` (without suffix).
void use_fake_pcap(const Options& options, const std::string& name)
{
#if !defined(_WIN32) && !defined(__linux__)
    (void)options;
    (void)name;
    throw Skip("the bridge has no pcap backend for this operating system");
#else
    set_environment("VEHICLE_SIL_TEST_PCAP_LIBRARY", (options.fakePcapDir / (name + kLibrarySuffix)).string());
#endif
}

/// `bridge` with `backend` on the device 'fake0', which answers; `link`: how the start line names it.
int run_echo(const Options& options, const fs::path& bridgeExecutable, const std::string& backend, const std::string& link)
{
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());

    const fs::path bridgeLog = options.workDir / "bridge.log";
    const fs::path peerLog = options.workDir / "peer.log";
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    ChildProcess bridge(bridgeExecutable, bridge_args(registry.uri(), "fake0", {"--ethertype", "0x88b5"}, backend), bridgeLog);
    ChildProcess peer(options.peer,
                      {"--registry-uri", registry.uri(), "--network", kNetwork, "--bridge", kParticipant, "--mode", "ask",
                       "--timeout-ms", std::to_string(std::max<std::chrono::milliseconds>(options.timeout - 15s, 5s).count())},
                      peerLog);

    const auto peerCode = wait_or_kill(peer, deadline);
    // POSIX: kill() sends SIGTERM first; the bridge stops gracefully and prints its statistics.
    bridge.kill();
    [[maybe_unused]] const auto bridgeCode = bridge.poll(); // not checked on Windows (below)

    const std::string peerText = read_text(peerLog);
    const std::string bridgeText = read_text(bridgeLog);
    bool ok = expect(peerCode == 0, "peer exit code " + exit_code_text(peerCode) + ", expected 0");
    const std::string startLine = "[eth-bridge] bridging SIL Kit network 'ETH_IT' (participant 'EthernetBridge', " + registry.uri() +
                                  ") <-> " + link + ": EtherType 0x88b5 only, both ways; promiscuous off (only frames "
                                  "to the interface's own address and to group addresses arrive); MAC guard on; Ctrl+C to stop";
    ok = expect(bridgeText.find(startLine) != std::string::npos, "bridge printed '" + startLine + "'") && ok;
    ok = expect(bridgeText.find("warning:") == std::string::npos, "bridge printed no warning") && ok;
#ifdef _WIN32
    log("bridge statistics not checked on Windows (the launcher cannot stop it gracefully)");
#else
    ok = expect(bridgeCode == 0, "bridge exit code " + exit_code_text(bridgeCode) + ", expected 0") && ok;
    const auto sent = stat(peerText, "sent");
    ok = expect(sent.has_value() && *sent > 0, "peer sent " + (sent ? std::to_string(*sent) : std::string("?")) + " frames") && ok;
    ok = expect(sent && stat(bridgeText, "silkit_to_ext") == sent, "bridge silkit_to_ext == sent") && ok;
    ok = expect(sent && stat(bridgeText, "ext_to_silkit") == sent, "bridge ext_to_silkit == sent (one answer per frame)") && ok;
    ok = expect(stat(bridgeText, "filtered_to_ext") == std::optional<unsigned long long>(1),
                "bridge filtered_to_ext == 1 (the frame of another EtherType)") && ok;
    for (const char* key : kStatisticsKeys)
    {
        const std::string name = key;
        if (name != "silkit_to_ext" && name != "ext_to_silkit" && name != "filtered_to_ext")
        {
            ok = expect(stat(bridgeText, name) == std::optional<unsigned long long>(0), "bridge " + name + " == 0") && ok;
        }
    }
#endif
    if (!ok)
    {
        dump_log(registry.log_file());
    }
    dump_log(bridgeLog);
    dump_log(peerLog);
    return ok ? 0 : 1;
}

/// Runs `executable` once with `args` and checks its exit code and output.
int run_single(const Options& options, const fs::path& executable, const std::vector<std::string>& args, int expectedCode,
               const std::vector<std::string>& mustContain, const std::vector<std::string>& mustNotContain = {})
{
    const fs::path bridgeLog = options.workDir / "bridge.log";
    ChildProcess bridge(executable, args, bridgeLog);
    const auto code = wait_or_kill(bridge, std::chrono::steady_clock::now() + options.timeout);
    const std::string text = read_text(bridgeLog);
    bool ok = expect(code == expectedCode, "bridge exit code " + exit_code_text(code) + ", expected " + std::to_string(expectedCode));
    for (const auto& needle : mustContain)
    {
        ok = expect(text.find(needle) != std::string::npos, "output contains '" + needle + "'") && ok;
    }
    for (const auto& needle : mustNotContain)
    {
        ok = expect(text.find(needle) == std::string::npos, "output does not contain '" + needle + "'") && ok;
    }
    dump_log(bridgeLog);
    return ok ? 0 : 1;
}

/// --list-interfaces of the real executable for the pcap backend of this operating system: the
/// library's list if the library is installed on this computer, else how to install it (exit 3).
int run_list_with_or_without_the_library(const Options& options, const std::vector<std::string>& args)
{
    const fs::path bridgeLog = options.workDir / "bridge.log";
    ChildProcess bridge(options.realBridge, args, bridgeLog);
    const auto code = wait_or_kill(bridge, std::chrono::steady_clock::now() + options.timeout);
    const std::string text = read_text(bridgeLog);
    const std::string library = kPcapLibrary;
    const bool listed = code == cli::kExitOk && text.find("interfaces " + library + " offers") != std::string::npos;
    const bool missing = code == cli::kExitSetupFailed &&
                         text.find("[eth-bridge] FAIL setup: " + library + " is not installed: cannot load ") != std::string::npos &&
                         text.find(library + " is not part of vehicle-sil: install ") != std::string::npos;
    const bool ok = expect(listed || missing, "the bridge lists what " + library + " offers (exit 0), or says that " + library +
                                                  " is not installed and how to install it (exit 3); exit code " +
                                                  exit_code_text(code));
    log(listed ? library + " is installed on this computer" : library + " is not installed on this computer");
    dump_log(bridgeLog);
    return ok ? 0 : 1;
}

int run_backend_not_here(const Options& options)
{
    const std::vector<std::string> common{"--registry-uri", "silkit://127.0.0.1:1", "--network", kNetwork, "--interface", "x",
                                          "--all-frames"};
    auto with_backend = [&](const std::string& backend) {
        auto args = common;
        args.insert(args.end(), {"--backend", backend});
        return args;
    };
    // The message comes before SIL Kit is touched: nothing listens at the registry's address.
#if defined(__linux__)
    int result = run_single(options, options.realBridge, with_backend("npcap"), cli::kExitSetupFailed,
                            {"[eth-bridge] FAIL setup: --backend npcap is for Windows. On Linux use --backend packet or --backend "
                             "pcap (a network interface), or --backend tap (a TAP device)."},
                            {"SIL Kit:"});
    result |= run_single(options, options.realBridge, {"--list-interfaces"}, cli::kExitOk,
                         {"interfaces of this computer (--backend packet and --backend pcap take the Ethernet ones; --backend tap "
                          "takes a TAP device):",
                          "  --interface lo"});
    result |= run_list_with_or_without_the_library(options, {"--list-interfaces", "--backend", "pcap"});
    return result;
#elif defined(_WIN32)
    int result = 0;
    for (const char* backend : {"tap", "packet", "pcap"})
    {
        result |= run_single(options, options.realBridge, with_backend(backend), cli::kExitSetupFailed,
                             {"[eth-bridge] FAIL setup: --backend " + std::string(backend) +
                              " is for Linux. On Windows use --backend npcap (a network adapter through Npcap)."},
                             {"SIL Kit:"});
    }
    result |= run_list_with_or_without_the_library(options, {"--list-interfaces"});
    result |= run_list_with_or_without_the_library(options, {"--list-interfaces", "--backend", "npcap"});
    return result;
#else
    const std::string why = " vehicle_sil_ethernet_bridge has no backend for this operating system (tap, packet and pcap are for "
                            "Linux; npcap is for Windows). Run the bridge on a Linux or Windows computer that reaches the same "
                            "SIL Kit registry.";
    int result = 0;
    for (const char* backend : {"tap", "packet", "npcap", "pcap"})
    {
        result |= run_single(options, options.realBridge, with_backend(backend), cli::kExitSetupFailed,
                             {"[eth-bridge] FAIL setup: --backend " + std::string(backend) + " cannot be used here:" + why},
                             {"SIL Kit:"});
    }
    result |= run_single(options, options.realBridge, {"--list-interfaces"}, cli::kExitSetupFailed,
                         {"[eth-bridge] FAIL setup: there is no interface to list here:" + why});
    return result;
#endif
}

std::size_t count(const std::string& text, const std::string& part)
{
    std::size_t n = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
    {
        ++n;
    }
    return n;
}

/// The bridge in a process group of its own (so that it can be asked to stop), its lines collected.
class StoppableBridge
{
public:
    StoppableBridge(const fs::path& executable, const std::vector<std::string>& args)
    {
        vehicle_sil::process::ProcessOptions processOptions;
        processOptions.on_line = [this](vehicle_sil::process::Stream, std::string_view line) {
            const std::lock_guard lock(mutex_);
            lines_ += std::string(line) + "\n";
        };
        process_ = std::make_unique<vehicle_sil::process::Process>(executable, args, std::move(processOptions));
    }

    vehicle_sil::process::Process& process() { return *process_; }

    /// What it wrote so far.
    std::string lines()
    {
        const std::lock_guard lock(mutex_);
        return lines_;
    }

    /// Everything it wrote, after its end.
    std::string all_lines()
    {
        process_->finish_output(10s);
        return lines();
    }

private:
    std::mutex mutex_;
    std::string lines_;
    std::unique_ptr<vehicle_sil::process::Process> process_;
};

int run_stop_request(const Options& options)
{
#ifdef _WIN32
    if (GetConsoleWindow() == nullptr && GetConsoleCP() == 0)
    {
        throw Skip("no console to send Ctrl+Break through");
    }
#endif
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    StoppableBridge bridge(options.bridge, bridge_args(registry.uri(), "fake0"));
    // The bridge says "Ctrl+C to stop" once its stop handlers are in place.
    while (bridge.lines().find("Ctrl+C to stop") == std::string::npos)
    {
        if (bridge.process().poll() || std::chrono::steady_clock::now() >= deadline)
        {
            const std::string text = bridge.all_lines();
            dump_log(registry.log_file());
            throw std::runtime_error("the bridge did not get ready to bridge:\n" + text);
        }
        std::this_thread::sleep_for(10ms);
    }
    if (!bridge.process().request_stop())
    {
        throw std::runtime_error("the stop request could not be sent");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const auto status = bridge.process().wait_for(std::max(remaining, std::chrono::milliseconds::zero()));
    const std::string text = bridge.all_lines();
    bool ok = expect(status && status->code == cli::kExitOk && !status->signal,
                     "bridge exit code " + (status ? std::to_string(status->code) : std::string("none (still running)")) +
                         ", expected 0");
    const auto stopping = text.find("stopping\n");
    const auto stats = text.find("stats silkit_to_ext=");
    const auto stopped = text.find("stopped\n");
    ok = expect(count(text, "stats silkit_to_ext=") == 1, "bridge printed one statistics line") && ok;
    ok = expect(stopping != std::string::npos && stats != std::string::npos && stopped != std::string::npos &&
                    stopping < stats && stats < stopped,
                "bridge printed 'stopping', the statistics and 'stopped', in this order") &&
         ok;
    // Nothing was bridged: every counter of the statistics line is 0.
    for (const char* key : kStatisticsKeys)
    {
        ok = expect(stat(text, key) == std::optional<unsigned long long>(0), std::string("bridge ") + key + " == 0") && ok;
    }
    ok = expect(text.find("FAIL") == std::string::npos, "bridge reported no failure") && ok;
    if (!ok)
    {
        dump_log(registry.log_file());
    }
    log("bridge output:\n" + text);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        vehicle_sil::test::check_timeout_scale();
        const Options options = parse(argc, argv);
        fs::create_directories(options.workDir);
        log("scenario " + options.scenario + ", logs in " + options.workDir.string());
        int result = 1;
        if (options.scenario == "echo-roundtrip")
        {
            result = run_echo(options, options.bridge, "packet", "fake 'fake0' (virtual)");
        }
        else if (options.scenario == "list-interfaces")
        {
            result = run_single(options, options.bridge, {"--list-interfaces"}, cli::kExitOk,
                                {"fake interfaces:", "  --interface fake0", "  --interface fake-fault"});
        }
        else if (options.scenario == "bad-usage")
        {
            result = run_single(options, options.bridge, bridge_args("silkit://127.0.0.1:1", "fake0", {"--backend", "usb"}),
                                cli::kExitUsage,
                                {"error: --backend must be tap, packet, npcap or pcap, got: usb\n",
                                 "Run 'vehicle_sil_ethernet_bridge --help' for the options."},
                                {"usage:", "--ethertype <type>"});
        }
        else if (options.scenario == "no-registry")
        {
            const std::string nowhere = "silkit://127.0.0.1:1";
            result = run_single(options, options.bridge, bridge_args(nowhere, "fake0"), cli::kExitSetupFailed,
                                {"[eth-bridge] FAIL setup: cannot connect to the SIL Kit registry at " + nowhere + " (",
                                 "). Start the registry first ('sil-kit-registry --listen-uri " + nowhere +
                                     "'), or give the address it listens at with --registry-uri."},
                                {"Ctrl+C to stop"});
        }
        else if (options.scenario == "non-ascii-arguments")
        {
            // "kairo 2" (line 2), "burijji" (bridge) and "sharyo-mo" (vehicle network) in Japanese, as UTF-8.
            const std::string interface_name = "\xE5\x9B\x9E\xE7\xB7\x9A 2";
            RegistryProcess registry(options.registry, options.workDir);
            result = run_single(options, options.bridge,
                                {"--registry-uri", registry.uri(), "--participant", "\xE3\x83\x96\xE3\x83\xAA\xE3\x83\x83\xE3\x82\xB8",
                                 "--network", "\xE8\xBB\x8A\xE4\xB8\xA1\xE7\xB6\xB2", "--backend", "packet", "--interface",
                                 interface_name, "--ethertype", "0x88b5"},
                                cli::kExitSetupFailed,
                                {"[eth-bridge] FAIL setup: no interface '" + interface_name +
                                 "' (fake). Interfaces here: fake0, fake-fault."},
                                {"Ctrl+C to stop"});
        }
        else if (options.scenario == "no-filter")
        {
            result = run_single(options, options.bridge, bridge_args("silkit://127.0.0.1:1", "fake0", {}), cli::kExitUsage,
                                {"error: --backend packet sends to a real adapter: say what may pass with --ethertype, --src-mac "
                                 "or --all-frames."});
        }
        else if (options.scenario == "no-interface")
        {
            RegistryProcess registry(options.registry, options.workDir);
            result = run_single(options, options.bridge, bridge_args(registry.uri(), "eth9"), cli::kExitSetupFailed,
                                {"[eth-bridge] FAIL setup: no interface 'eth9' (fake). Interfaces here: fake0, fake-fault."},
                                {"Ctrl+C to stop", "stats "});
        }
        else if (options.scenario == "link-fault")
        {
            RegistryProcess registry(options.registry, options.workDir);
            result = run_single(options, options.bridge, bridge_args(registry.uri(), "fake-fault"), cli::kExitFailed,
                                {"[eth-bridge] FAIL: interface 'fake-fault' is gone (fake)", "stats silkit_to_ext=", "stopped"});
        }
        else if (options.scenario == "stop-request")
        {
            result = run_stop_request(options);
        }
        else if (options.scenario == "backend-not-here")
        {
            result = run_backend_not_here(options);
        }
        else if (options.scenario == "pcap-roundtrip")
        {
            use_fake_pcap(options, "vehicle_sil_fake_pcap");
            result = run_echo(options, options.pcapBridge, kPcapBackend, kPcapLink);
        }
        else if (options.scenario == "pcap-list")
        {
            use_fake_pcap(options, "vehicle_sil_fake_pcap");
            result = run_single(options, options.pcapBridge, {"--list-interfaces", "--backend", kPcapBackend}, cli::kExitOk,
                                {std::string("interfaces ") + kPcapLibrary + " offers (fake pcap version 1.0 (TEST ONLY)); --backend " +
                                     kPcapBackend + " takes the Ethernet ones:",
                                 "  --interface fake0           up, connected  (Fake Ethernet adapter with a device that answers)",
                                 "  --interface fake-raw        up, connected\n"});
        }
        else if (options.scenario == "pcap-no-library")
        {
            // Nothing listens at the registry's address: the message comes before SIL Kit is touched.
            use_fake_pcap(options, "no_such_pcap");
            const std::string message = std::string("[eth-bridge] FAIL setup: ") + kPcapLibrary + " is not installed: cannot load ";
            const std::string advice = std::string(kPcapLibrary) + " is not part of vehicle-sil: install ";
            const std::vector<std::string> pcapArgs =
                bridge_args("silkit://127.0.0.1:1", "fake0", {"--ethertype", "0x88b5"}, kPcapBackend);
            result = run_single(options, options.pcapBridge, pcapArgs, cli::kExitSetupFailed, {message, "no_such_pcap", advice},
                                {"SIL Kit:", "Ctrl+C to stop"});
            result |= run_single(options, options.pcapBridge, {"--list-interfaces", "--backend", kPcapBackend}, cli::kExitSetupFailed,
                                 {message, advice});
        }
        else if (options.scenario == "pcap-old-library")
        {
            use_fake_pcap(options, "vehicle_sil_fake_pcap_old");
            result = run_single(options, options.pcapBridge,
                                bridge_args("silkit://127.0.0.1:1", "fake0", {"--ethertype", "0x88b5"}, kPcapBackend),
                                cli::kExitSetupFailed,
                                {"[eth-bridge] FAIL setup: ", "vehicle_sil_fake_pcap_old",
                                 std::string("(fake pcap version 0.9 (TEST ONLY)) lacks the function pcap_set_immediate_mode, which "
                                             "the bridge needs: it is too old, or it is not ") +
                                     kPcapLibrary + ". Install a current one."},
                                {"SIL Kit:", "Ctrl+C to stop"});
        }
        else if (options.scenario == "pcap-no-interface")
        {
            use_fake_pcap(options, "vehicle_sil_fake_pcap");
            RegistryProcess registry(options.registry, options.workDir);
            result = run_single(options, options.pcapBridge,
                                bridge_args(registry.uri(), "eth9", {"--ethertype", "0x88b5"}, kPcapBackend), cli::kExitSetupFailed,
                                {std::string("[eth-bridge] FAIL setup: no interface 'eth9' for --backend ") + kPcapBackend +
                                 ". Interfaces " + kPcapLibrary + " offers here: fake0, fake1, fake-echo, "},
                                {"Ctrl+C to stop", "stats "});
        }
        else if (options.scenario == "pcap-fault")
        {
            use_fake_pcap(options, "vehicle_sil_fake_pcap");
            RegistryProcess registry(options.registry, options.workDir);
            result = run_single(options, options.pcapBridge,
                                bridge_args(registry.uri(), "fake-fault", {"--ethertype", "0x88b5"}, kPcapBackend), cli::kExitFailed,
                                {"[eth-bridge] FAIL: reading from 'fake-fault' failed: The interface disappeared (fake)",
                                 "stats silkit_to_ext=", "stopped"});
        }
        else
        {
            throw std::invalid_argument("unknown scenario " + options.scenario);
        }
        log(result == 0 ? "PASS" : "FAIL");
        return result;
    }
    catch (const Skip& e)
    {
        log(std::string("SKIP: ") + e.what());
        return 77;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return 1;
    }
}
