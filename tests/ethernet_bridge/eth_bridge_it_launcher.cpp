// SPDX-License-Identifier: MIT
//
// Process level tests of the Ethernet bridge executable. The first ones use the executable built
// with fake links (fake_link_factory.cpp: --interface fake0 is a wire with a device that answers):
//   echo-roundtrip     registry + bridge + SIL Kit peer (eth_bridge_it_peer --mode ask). The peer
//                      checks every answer; the launcher checks the bridge's start line and its
//                      statistics (forwarded counts, every answer counted as from a device, the
//                      one frame of another EtherType filtered, no problem counted, no warning).
//   list-interfaces    --list-interfaces prints the factory's list: exit 0.
//   list-interfaces-json  --list-interfaces --json of the real executable on Linux (packet without
//                      --backend; lo is not usable), of a backend of another OS (exit 3 with the
//                      error in the JSON) and of the executable on fake links. Skipped elsewhere.
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
//   device-shared      two bridges on fake0: with the default scope of the fake devices they do
//                      not find each other; with one scope (VEHICLE_SIL_FAKE_DEVICE_SCOPE) the
//                      second finds the first at its start, the first finds the second while it
//                      runs and warns, both run on, --list-interfaces --json says fake0 is in use
//                      by 2 bridges, and once the first ends the second's shared is 0 again.
//   backend-not-here   the real executable (--real-bridge): a backend that cannot be used on this
//                      operating system ends with exit 3 and says what to use instead, before
//                      SIL Kit is touched; --list-interfaces lists the interfaces. The pcap
//                      library of this computer may be installed or not: with it the list is
//                      printed, without it the bridge says how to install it (exit 3).
// The pcap-* scenarios use the executable with its own links of the operating system and the fake
// pcap library in place of the installed one (--pcap-bridge; tests/eth_link/fake_pcap), with
// --backend npcap on Windows and --backend pcap on Linux:
//   pcap-roundtrip     as echo-roundtrip, through the fake library's device that answers.
//   pcap-own-address   as pcap-roundtrip on fake-self, where the answers come from the adapter's own
//                      address (this computer, not a device): forwarded, but ext_from_devices stays
//                      0. Linux only: the pcap link reads its own address from libpcap's list there;
//                      on Windows it asks the OS, which does not know the fake adapter (skipped).
//   pcap-nothing       fake1, where nothing answers, with --stats-every-ms 100 and the peer in
//                      --mode send: the bridge prints its statistics line while it runs, a line only
//                      where it changed, and the one with every frame sent out has
//                      ext_from_devices=0. The lines while running are the only ones checked, so
//                      that the bridge can be ended as on Windows (no stop request).
//   pcap-list          --list-interfaces prints the library's devices: exit 0.
//   pcap-list-json     --list-interfaces --json without --backend: the default is the pcap
//                      backend, stdout is one JSON object with the library's devices: exit 0.
//   pcap-no-library-json  --list-interfaces --json without the library: exit 3, the JSON has
//                      the error (library-missing) and stderr the FAIL line.
//   pcap-no-library    the library is not there: exit 3 with the way to install it, before
//                      SIL Kit is touched.
//   pcap-old-library   the library lacks a function the bridge needs: exit 3, the same.
//   pcap-no-interface  a device the library does not have: exit 3 with the devices it has.
//   pcap-fault         reading fails while the bridge runs: FAIL, the statistics, exit 1.
//   pcap-promiscuous   the promiscuous mode on fake0 (accepts it) and fake-refuses (cannot be
//                      opened in it): by default on where accepted, else off with one warning that
//                      says so (the start line says which); --no-promiscuous off without a
//                      warning; --promiscuous on fake-refuses fails at the start (exit 3);
//                      --promiscuous with --no-promiscuous is exit 4.
//   pcap-describe      --describe writes the JSON of an ecu_main executable and exits 0, also
//                      where the pcap library is not there (it is not loaded); a wrong argument
//                      is still exit 4.
//   pcap-launcher      the bridge in a configuration file of vehicle_sil_launch (role: bridge,
//                      describe: true, required: false, registry_uri with port 0), beside an ECU
//                      on its network (eth_bridge_it_ecu.cpp): check passes and lists the bridge on
//                      the network; run starts the bridge with --name, --registry-uri (a real port)
//                      and --start-timeout-ms, the ECU's frames go out through the bridge and the
//                      device's answers come back to the ECU (a scenario expects them), and when
//                      the run ends the launcher stops the bridge, which ends with exit code 0.
#include "launcher_support.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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
#else
#include <unistd.h>
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
const char* const kStatisticsKeys[] = {"silkit_to_ext", "ext_to_silkit", "ext_from_devices", "own_tx_dropped", "loop_suspected", "mac_unguarded",
                                       "filtered_to_ext", "filtered_from_ext", "oversize", "malformed", "ext_overruns",
                                       "ext_queue_full", "ext_tx_failures", "silkit_tx_failures",
                                       "ext_checksum_incomplete"};

struct Options
{
    fs::path registry, bridge, realBridge, pcapBridge, fakePcapDir, peer, ecu, launch, workDir;
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
    options.ecu = args.required("ecu");
    options.launch = args.required("launch");
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

/// `bridge` with `backend` on the device `interface_name`, which answers; `link`: how the start line
/// names it. `answerer`: the address the answers come from; `fromDevice`: whether that is a device
/// (false: the adapter's own address, this computer).
int run_echo(const Options& options, const fs::path& bridgeExecutable, const std::string& backend, const std::string& link,
             const std::string& interface_name = "fake0", const std::string& answerer = "02:00:00:00:00:02",
             bool fromDevice = true)
{
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());

    const fs::path bridgeLog = options.workDir / "bridge.log";
    const fs::path peerLog = options.workDir / "peer.log";
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    ChildProcess bridge(bridgeExecutable, bridge_args(registry.uri(), interface_name, {"--ethertype", "0x88b5"}, backend), bridgeLog);
    ChildProcess peer(options.peer,
                      {"--registry-uri", registry.uri(), "--network", kNetwork, "--bridge", kParticipant, "--mode", "ask",
                       "--device-mac", answerer, "--timeout-ms",
                       std::to_string(std::max<std::chrono::milliseconds>(options.timeout - 15s, 5s).count())},
                      peerLog);

    const auto peerCode = wait_or_kill(peer, deadline);
    // POSIX: kill() sends SIGTERM first; the bridge stops gracefully and prints its statistics.
    bridge.kill();
    [[maybe_unused]] const auto bridgeCode = bridge.poll(); // not checked on Windows (below)

    const std::string peerText = read_text(peerLog);
    const std::string bridgeText = read_text(bridgeLog);
    bool ok = expect(peerCode == 0, "peer exit code " + exit_code_text(peerCode) + ", expected 0");
    const std::string startLine = "[eth-bridge] bridging SIL Kit network 'ETH_IT' (participant 'EthernetBridge', " + registry.uri() +
                                  ") <-> " + link + ": EtherType 0x88b5 only, both ways; promiscuous on; MAC guard on; Ctrl+C to stop";
    ok = expect(bridgeText.find(startLine) != std::string::npos, "bridge printed '" + startLine + "'") && ok;
    ok = expect(bridgeText.find("warning:") == std::string::npos, "bridge printed no warning") && ok;
#ifdef _WIN32
    (void)fromDevice;
    log("bridge statistics not checked on Windows (the launcher cannot stop it gracefully)");
#else
    ok = expect(bridgeCode == 0, "bridge exit code " + exit_code_text(bridgeCode) + ", expected 0") && ok;
    const auto sent = stat(peerText, "sent");
    ok = expect(sent.has_value() && *sent > 0, "peer sent " + (sent ? std::to_string(*sent) : std::string("?")) + " frames") && ok;
    ok = expect(sent && stat(bridgeText, "silkit_to_ext") == sent, "bridge silkit_to_ext == sent") && ok;
    ok = expect(sent && stat(bridgeText, "ext_to_silkit") == sent, "bridge ext_to_silkit == sent (one answer per frame)") && ok;
    if (fromDevice)
    {
        ok = expect(sent && stat(bridgeText, "ext_from_devices") == sent, "bridge ext_from_devices == sent (every answer from the device)") && ok;
    }
    else
    {
        ok = expect(stat(bridgeText, "ext_from_devices") == std::optional<unsigned long long>(0),
                    "bridge ext_from_devices == 0 (every answer from the adapter's own address)") && ok;
    }
    ok = expect(stat(bridgeText, "filtered_to_ext") == std::optional<unsigned long long>(1),
                "bridge filtered_to_ext == 1 (the frame of another EtherType)") && ok;
    for (const char* key : kStatisticsKeys)
    {
        const std::string name = key;
        if (name != "silkit_to_ext" && name != "ext_to_silkit" && name != "ext_from_devices" && name != "filtered_to_ext")
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

/// Runs `executable` once with `args` for JSON on stdout: checks its exit code, that stdout is one
/// JSON object ending with a line break and holds every part of `inStdout`, and that stderr holds
/// every part of `inStderr` (nothing if that is empty).
int run_json(const Options& options, const fs::path& executable, const std::vector<std::string>& args, int expectedCode,
             const std::vector<std::string>& inStdout, const std::vector<std::string>& inStderr = {})
{
    vehicle_sil::process::CaptureOptions capture;
    capture.timeout = options.timeout;
    const auto run = vehicle_sil::process::run_captured(executable, args, capture);
    std::string out = run.standard_output;
    std::string err = run.standard_error;
    std::erase(out, '\r');
    std::erase(err, '\r');
    log("stdout:\n" + out + "stderr:\n" + err);
    bool ok = expect(run.exit_code == expectedCode,
                     "bridge exit code " + exit_code_text(run.exit_code) + ", expected " + std::to_string(expectedCode));
    ok = expect(out.starts_with("{\n") && out.ends_with("\n}\n") && out.find("\n}\n") == out.size() - 3,
                "stdout is one JSON object and a line break") && ok;
    for (const auto& needle : inStdout)
    {
        ok = expect(out.find(needle) != std::string::npos, "stdout contains '" + needle + "'") && ok;
    }
    ok = expect(inStderr.empty() == err.empty(), inStderr.empty() ? "nothing on stderr" : "stderr says why") && ok;
    for (const auto& needle : inStderr)
    {
        ok = expect(err.find(needle) != std::string::npos, "stderr contains '" + needle + "'") && ok;
    }
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

/// The statistics lines a bridge printed while it ran (before "stopping"), without the tag.
std::vector<std::string> running_statistics(const std::string& raw)
{
    std::string text = raw;
    std::erase(text, '\r');
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < text.size())
    {
        const auto end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line == "[eth-bridge] stopping")
        {
            break;
        }
        if (line.starts_with("[eth-bridge] stats "))
        {
            lines.push_back(line.substr(std::string("[eth-bridge] ").size()));
        }
    }
    return lines;
}

int run_pcap_nothing(const Options& options)
{
    use_fake_pcap(options, "vehicle_sil_fake_pcap");
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    StoppableBridge bridge(options.pcapBridge,
                           bridge_args(registry.uri(), "fake1", {"--ethertype", "0x88b5", "--stats-every-ms", "100"}, kPcapBackend));
    const fs::path peerLog = options.workDir / "peer.log";
    ChildProcess peer(options.peer,
                      {"--registry-uri", registry.uri(), "--network", kNetwork, "--bridge", kParticipant, "--mode", "send",
                       "--timeout-ms", std::to_string(std::max<std::chrono::milliseconds>(options.timeout - 15s, 5s).count())},
                      peerLog);
    const auto peerCode = wait_or_kill(peer, deadline);
    const auto sent = stat(read_text(peerLog), "sent");
    // The line that has every frame the peer sent going out: the bridge prints it once the count
    // changed, at its next interval.
    std::string complete;
    while (sent && std::chrono::steady_clock::now() < deadline && !bridge.process().poll())
    {
        const auto lines = running_statistics(bridge.lines());
        if (!lines.empty() && stat(lines.back(), "silkit_to_ext") == sent)
        {
            complete = lines.back();
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    bridge.process().kill_tree();
    const std::string text = bridge.all_lines();
    const auto lines = running_statistics(text);

    bool ok = expect(peerCode == 0, "peer exit code " + exit_code_text(peerCode) + ", expected 0 (no frame came back)");
    ok = expect(sent.has_value() && *sent > 0, "peer sent " + (sent ? std::to_string(*sent) : std::string("?")) + " frames") && ok;
    ok = expect(!complete.empty(), "bridge printed a statistics line with silkit_to_ext == sent while running") && ok;
    ok = expect(stat(complete, "ext_from_devices") == std::optional<unsigned long long>(0), "that line has ext_from_devices=0") && ok;
    ok = expect(stat(complete, "ext_to_silkit") == std::optional<unsigned long long>(0), "that line has ext_to_silkit=0") && ok;
    bool changedEachTime = true;
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        changedEachTime = changedEachTime && lines[i] != lines[i - 1];
    }
    ok = expect(changedEachTime, "bridge printed a statistics line while running only where it changed") && ok;
    for (const auto& line : lines)
    {
        // Every key, in the documented order.
        std::string keys = "stats";
        for (const char* key : kStatisticsKeys)
        {
            keys += std::string(" ") + key + "=";
        }
        ok = expect(std::regex_replace(line, std::regex("=[0-9]+"), "=") == keys, "'" + line + "' has every key in order") && ok;
    }
    if (!ok)
    {
        dump_log(registry.log_file());
    }
    log("bridge output:\n" + text);
    dump_log(peerLog);
    return ok ? 0 : 1;
}

/// Waits until `bridge` has printed `part` after position `from` of its output: where it begins, or
/// nullopt once the bridge has ended or `deadline` has passed.
std::optional<std::size_t> wait_for_text(StoppableBridge& bridge, const std::string& part, std::chrono::steady_clock::time_point deadline,
                                         std::size_t from = 0)
{
    for (;;)
    {
        const std::string text = bridge.lines();
        if (const auto at = text.find(part, from); at != std::string::npos)
        {
            return at;
        }
        if (bridge.process().poll() || std::chrono::steady_clock::now() >= deadline)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(10ms);
    }
}

long this_process_id()
{
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

/// " peers=<pid>" where the device marks name their processes (Linux), else nothing.
std::string peers_key(long pid)
{
#ifdef __linux__
    return " peers=" + std::to_string(pid);
#else
    (void)pid;
    return {};
#endif
}

/// Two bridges on fake0. With the default scope of the fake devices (this process's tests run at
/// the same time as others on fake0), they do not find each other. With one scope for both, the
/// second finds the first at its start, the first finds the second while it runs and warns, the
/// list says that two bridges use fake0, both run on, and once the first ends the second's shared
/// is 0 again.
int run_device_shared(const Options& options)
{
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    const auto args = [&registry](const std::string& participant) {
        std::vector<std::string> a = bridge_args(registry.uri(), "fake0");
        a[3] = participant;
        return a;
    };
    bool ok = true;
    {
        StoppableBridge a(options.bridge, args("DefaultScopeA"));
        ok = expect(wait_for_text(a, "Ctrl+C to stop", deadline).has_value(), "the first bridge started") && ok;
        StoppableBridge b(options.bridge, args("DefaultScopeB"));
        ok = expect(wait_for_text(b, "Ctrl+C to stop", deadline).has_value(), "the second bridge started") && ok;
        const std::string first = a.lines();
        const std::string second = b.lines();
        ok = expect(first.find(" marked=1 shared=0\n") != std::string::npos && second.find(" marked=1 shared=0\n") != std::string::npos &&
                        (first + second).find(" shared=1") == std::string::npos && (first + second).find("is also used") == std::string::npos,
                    "with the default scope the two bridges on fake0 do not find each other") && ok;
        log("default scope, first bridge:\n" + first + "second bridge:\n" + second);
        a.process().kill_tree();
        b.process().kill_tree();
    }

    set_environment("VEHICLE_SIL_FAKE_DEVICE_SCOPE", "device-shared-" + std::to_string(this_process_id()));
    StoppableBridge first(options.bridge, args("FirstBridge"));
    ok = expect(wait_for_text(first, "Ctrl+C to stop", deadline).has_value(), "the first bridge started") && ok;
    ok = expect(first.lines().find("[eth-bridge] link link=unknown device=") != std::string::npos &&
                    first.lines().find(" marked=1 shared=0\n") != std::string::npos,
                "the first bridge's first link line: link=unknown (a fake link does not tell) marked=1 shared=0") && ok;
    StoppableBridge second(options.bridge, args("SecondBridge"));
    ok = expect(wait_for_text(second, "Ctrl+C to stop", deadline).has_value(), "the second bridge started") && ok;
    ok = expect(second.lines().find(" marked=1 shared=1" + peers_key(first.process().id()) + "\n") != std::string::npos,
                "the second bridge's first link line: shared=1" + peers_key(first.process().id())) && ok;
    // The first finds the second while it runs (it looks once a second).
    ok = expect(wait_for_text(first, " marked=1 shared=1" + peers_key(second.process().id()) + "\n", deadline).has_value(),
                "the first bridge finds the second: shared=1" + peers_key(second.process().id())) && ok;
#ifdef __linux__
    const std::string who = " (process " + std::to_string(second.process().id()) + ")";
#else
    const std::string who;
#endif
    ok = expect(wait_for_text(first, "warning: interface 'fake0' is also used by another vehicle-sil bridge" + who + ": ", deadline)
                    .has_value(),
                "the first bridge warns") && ok;

    // The list: fake0 in use by both, fake-fault by none.
    vehicle_sil::process::CaptureOptions capture;
    capture.timeout = options.timeout;
    auto listed = vehicle_sil::process::run_captured(options.bridge, {"--list-interfaces", "--json"}, capture);
    std::erase(listed.standard_output, '\r');
    const std::string& json = listed.standard_output;
    const auto inUse = json.find("      \"in_use\": true,\n");
    ok = expect(listed.exit_code == 0 && inUse != std::string::npos && json.find("      \"in_use_count\": 2,\n", inUse) != std::string::npos &&
                    json.find("      \"in_use\": false,\n", inUse) != std::string::npos &&
                    json.find("in use by 2 vehicle-sil bridges") != std::string::npos,
                "--list-interfaces --json: fake0 in use by 2 bridges, fake-fault by none") && ok;
    log("list:\n" + json);

    ok = expect(!first.process().poll() && !second.process().poll(), "both bridges run on") && ok;
    first.process().kill_tree();
    const auto shared = second.lines().find(" marked=1 shared=1");
    ok = expect(shared != std::string::npos && wait_for_text(second, " marked=1 shared=0\n", deadline, shared).has_value(),
                "once the first bridge has ended, the second's shared is 0 again") && ok;
    second.process().kill_tree();
    set_environment("VEHICLE_SIL_FAKE_DEVICE_SCOPE", "");
    log("first bridge:\n" + first.all_lines() + "second bridge:\n" + second.all_lines());
    if (!ok)
    {
        dump_log(registry.log_file());
    }
    return ok ? 0 : 1;
}

/// Starts `executable` with `args`, waits until it has printed its start line and, if `until` is
/// not empty, a line with `until`, then ends it. What it printed.
std::string run_until_ready(const Options& options, const fs::path& executable, const std::vector<std::string>& args,
                            const std::string& until = {})
{
    StoppableBridge bridge(executable, args);
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    for (;;)
    {
        const std::string text = bridge.lines();
        if (text.find("Ctrl+C to stop") != std::string::npos && (until.empty() || text.find(until) != std::string::npos))
        {
            break;
        }
        if (bridge.process().poll() || std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    bridge.process().kill_tree();
    return bridge.all_lines();
}

int run_promiscuous(const Options& options)
{
    use_fake_pcap(options, "vehicle_sil_fake_pcap");
    RegistryProcess registry(options.registry, options.workDir);
    const auto args = [&](const std::string& device, std::vector<std::string> more) {
        more.insert(more.begin(), "--all-frames");
        return bridge_args(registry.uri(), device, more, kPcapBackend);
    };
    const std::string offText = "; promiscuous off (only frames to the interface's own address and to group addresses arrive);";
    const std::string refusedText =
        "; promiscuous off (the interface does not accept it: only frames to its own address and to group addresses arrive);";
    const std::string warning = "[eth-bridge] warning: interface 'fake-refuses' does not accept promiscuous mode (";
    const std::string meaning = "), so the bridge runs without it there: only frames to the interface's own address and to "
                                "group addresses arrive, and frames to the devices of the SIL Kit side (they have addresses "
                                "of their own) do not. If devices outside must reach them, use an adapter that accepts "
                                "promiscuous mode (a wired one usually does).\n";
    bool ok = true;

    const std::string accepted = run_until_ready(options, options.pcapBridge, args("fake0", {}));
    ok = expect(accepted.find("; promiscuous on;") != std::string::npos && accepted.find("warning:") == std::string::npos,
                "by default, fake0 is in promiscuous mode, without a warning") && ok;
    log("bridge output (fake0, default):\n" + accepted);

    const std::string refused = run_until_ready(options, options.pcapBridge, args("fake-refuses", {}), "warning:");
    ok = expect(refused.find(refusedText) != std::string::npos, "by default, fake-refuses runs without promiscuous mode, and the "
                                                                "start line says the interface does not accept it") && ok;
    ok = expect(count(refused, "warning:") == 1 && refused.find(warning) != std::string::npos &&
                    refused.find(meaning) != std::string::npos,
                "one warning says that fake-refuses runs without promiscuous mode, and what that means") && ok;
    log("bridge output (fake-refuses, default):\n" + refused);

    const std::string off = run_until_ready(options, options.pcapBridge, args("fake-refuses", {"--no-promiscuous"}));
    ok = expect(off.find(offText) != std::string::npos && off.find("warning:") == std::string::npos,
                "--no-promiscuous: off, without a warning") && ok;
    log("bridge output (fake-refuses, --no-promiscuous):\n" + off);

    ok = run_single(options, options.pcapBridge, args("fake-refuses", {"--promiscuous"}), cli::kExitSetupFailed,
                    {"[eth-bridge] FAIL setup: "}, {"Ctrl+C to stop"}) == 0 && ok;
    ok = run_single(options, options.pcapBridge, args("fake0", {"--promiscuous", "--no-promiscuous"}), cli::kExitUsage,
                    {"error: --promiscuous and --no-promiscuous contradict each other"}) == 0 && ok;
    return ok ? 0 : 1;
}

/// `path` for a YAML file: with '/' and in single quotes (no escapes there; a quote is doubled).
std::string yaml_path(const fs::path& path)
{
    std::string text = path.generic_string();
    std::string out = "'";
    for (const char c : text)
    {
        out += c == '\'' ? std::string("''") : std::string(1, c);
    }
    return out + "'";
}

/// The bridge in a configuration file of vehicle_sil_launch, beside an ECU on its network.
int run_launcher(const Options& options)
{
    use_fake_pcap(options, "vehicle_sil_fake_pcap");
    const fs::path config = options.workDir / "simulation.yaml";
    const fs::path scenario = options.workDir / "answers.yaml";
    {
        std::ofstream out(config, std::ios::binary);
        out << "vehicle_sil_simulation: 2\n"
               "stop_at_ms: 60000\n"
               "speed: 1\n"
               "registry_uri: silkit://127.0.0.1:0\n"
               "networks:\n"
               "  - name: ETH_EXT\n"
               "    kind: ethernet\n"
               "participants:\n"
               "  - name: Pinger\n"
               "    command: [" << yaml_path(options.ecu) << "]\n"
               "  - name: EthBridge\n"
               "    role: bridge\n"
               "    required: false\n"
               "    command: [" << yaml_path(options.pcapBridge) << ", --network, ETH_EXT, --backend, " << kPcapBackend
            << ",\n"
               "              --interface, fake0, --src-mac, \"02:00:00:00:00:01\"]\n";
    }
    {
        // The bridge joins in real time, outside the coordinated start: the answers come once it runs.
        std::ofstream out(scenario, std::ios::binary);
        out << "vehicle_sil_scenario: 1\n"
               "steps:\n"
               "  - expect: { Pinger.answers: { ge: 3 } }\n"
               "    within_ms: 55000\n";
    }

    const fs::path checkLog = options.workDir / "check.log";
    ChildProcess check(options.launch, {"check", config.string()}, checkLog);
    const auto checked = wait_or_kill(check, std::chrono::steady_clock::now() + options.timeout);
    const std::string checkText = read_text(checkLog);
    bool ok = expect(checked == 0, "check exit code " + exit_code_text(checked) + ", expected 0");
    ok = expect(checkText.find("EthBridge (role: bridge)  real time  ETH_EXT (ethernet)\n") != std::string::npos,
                "check lists the bridge on ETH_EXT, in real time") && ok;
    ok = expect(checkText.find("ETH_EXT  ethernet  Pinger, EthBridge\n") != std::string::npos,
                "check has the bridge among the participants of ETH_EXT") && ok;
    ok = expect(checkText.find("passed: 0 errors, 0 warnings, 0 notes\n") != std::string::npos,
                "check passes without a finding") && ok;
    dump_log(checkLog);
    if (!ok)
    {
        return 1;
    }

    const fs::path runLog = options.workDir / "run.log";
    ChildProcess run(options.launch, {"run", config.string(), "--scenario", scenario.string()}, runLog);
    const auto ran = wait_or_kill(run, std::chrono::steady_clock::now() + options.timeout);
    const std::string text = read_text(runLog);
    ok = expect(ran == 0, "run exit code " + exit_code_text(ran) + ", expected 0 (the scenario passed)");
    const std::regex started("starting EthBridge: [^\n]* --name EthBridge --registry-uri silkit://127\\.0\\.0\\.1:([0-9]+) "
                             "--start-timeout-ms [0-9]+\n");
    std::smatch match;
    ok = expect(std::regex_search(text, match, started) && match[1].str() != "0",
                "run starts the bridge with --name, the registry's real port and --start-timeout-ms") && ok;
    ok = expect(text.find("--interface fake0 --src-mac 02:00:00:00:00:01 --name EthBridge") != std::string::npos,
                "run passes the bridge's own arguments as written") && ok;
    ok = expect(text.find("promiscuous on;") != std::string::npos && text.find("Ctrl+C to stop") != std::string::npos,
                "the bridge started bridging, in promiscuous mode (the default where the interface accepts it)") && ok;
    for (const char* key : {"silkit_to_ext", "ext_to_silkit"})
    {
        const auto value = stat(text, key);
        ok = expect(value && *value >= 3, std::string("bridge ") + key + " >= 3") && ok;
    }
#ifndef _WIN32
    // On Windows the launcher can ask only where it shares a console with the bridge (Ctrl+Break),
    // and else ends it: either way, the bridge does not stay.
    ok = expect(text.find("[EthBridge] [eth-bridge] stopped\n") != std::string::npos,
                "the launcher stopped the bridge when the run ended") && ok;
    ok = expect(text.find("EthBridge    no        stopped (SIGTERM), exit code 0\n") != std::string::npos,
                "the bridge ended with exit code 0 after the stop request") && ok;
#endif
    ok = expect(text.find("left processes") == std::string::npos && text.find("ending EthBridge") == std::string::npos,
                "nothing of the bridge was left running") && ok;
    dump_log(runLog);
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
        else if (options.scenario == "device-shared")
        {
            result = run_device_shared(options);
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
        else if (options.scenario == "pcap-own-address")
        {
#ifdef __linux__
            use_fake_pcap(options, "vehicle_sil_fake_pcap");
            result = run_echo(options, options.pcapBridge, kPcapBackend, "pcap 'fake-self' (network interface through libpcap)",
                              "fake-self", "02:00:00:00:00:f2", false);
#else
            throw Skip("the pcap link reads its own address from the list of devices on Linux only; on Windows it asks the OS, "
                       "which does not know the fake adapter");
#endif
        }
        else if (options.scenario == "pcap-nothing")
        {
            result = run_pcap_nothing(options);
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
        else if (options.scenario == "pcap-list-json")
        {
            // No --backend: the executable's default is the pcap backend of this OS.
            use_fake_pcap(options, "vehicle_sil_fake_pcap");
            result = run_json(options, options.pcapBridge, {"--list-interfaces", "--json"}, cli::kExitOk,
                              {"  \"vehicle_sil_targets\": 1,\n  \"tool\": \"vehicle_sil_ethernet_bridge\",\n",
                               std::string("  \"backend\": \"") + kPcapBackend + "\",\n",
                               "  \"source\": \"fake pcap version 1.0 (TEST ONLY)\",\n",
                               "      \"value\": \"fake0\",\n      \"label\": \"fake0\",\n"
                               "      \"detail\": \"up, connected; Fake Ethernet adapter with a device that answers\",\n"
                               "      \"usable\": true,\n      \"reason\": null,\n",
                               "      \"value\": \"fake-raw\",\n", "  \"notes\": [],\n  \"error\": null\n}\n"});
        }
        else if (options.scenario == "pcap-no-library-json")
        {
            use_fake_pcap(options, "no_such_pcap");
            result = run_json(options, options.pcapBridge, {"--list-interfaces", "--json"}, cli::kExitSetupFailed,
                              {std::string("  \"backend\": \"") + kPcapBackend + "\",\n  \"source\": null,\n  \"targets\": [],\n",
                               "    \"code\": \"library-missing\",\n",
                               std::string("    \"message\": \"") + kPcapLibrary + " is not installed: cannot load ",
                               std::string(kPcapLibrary) + " is not part of vehicle-sil: install "},
                              {std::string("[eth-bridge] FAIL setup: ") + kPcapLibrary + " is not installed: cannot load "});
        }
        else if (options.scenario == "list-interfaces-json")
        {
#if defined(__linux__)
            // The real executable: packet without --backend, the interfaces of this computer.
            result = run_json(options, options.realBridge, {"--list-interfaces", "--json"}, cli::kExitOk,
                              {"  \"backend\": \"packet\",\n  \"source\": null,\n",
                               "      \"value\": \"lo\",\n      \"label\": \"lo\",\n", "      \"usable\": false,\n",
                               "      \"reason\": \"not Ethernet (link type 772)\",\n"});
            // A backend of another OS: the error in the JSON, the FAIL line for people.
            result |= run_json(options, options.realBridge, {"--list-interfaces", "--backend", "npcap", "--json"},
                               cli::kExitSetupFailed,
                               {"  \"backend\": \"npcap\",\n", "    \"code\": \"backend-unavailable\",\n",
                                "    \"message\": \"--backend npcap is for Windows. On Linux use --backend packet"},
                               {"[eth-bridge] FAIL setup: --backend npcap is for Windows."});
            // The executable on fake links: --json on its own list.
            result |= run_json(options, options.bridge, {"--list-interfaces", "--json"}, cli::kExitOk,
                               {"  \"backend\": \"packet\",\n", "      \"value\": \"fake0\",\n",
                                "      \"value\": \"fake-fault\",\n"});
#else
            throw Skip("the real executable lists the interfaces of the OS on Linux only");
#endif
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
        else if (options.scenario == "pcap-promiscuous")
        {
            result = run_promiscuous(options);
        }
        else if (options.scenario == "pcap-describe")
        {
            // Not loaded for --describe: a library that is not there makes no difference.
            use_fake_pcap(options, "no_such_pcap");
            result = run_single(options, options.pcapBridge,
                                {"--describe", "--name", "EthBridge", "--network", "ETH_EXT", "--backend", kPcapBackend,
                                 "--interface", "fake0", "--all-frames", "--promiscuous"},
                                cli::kExitOk,
                                {"\"vehicle_sil_ecu\": 1,\n", "\"name\": \"EthBridge\",\n", "\"step_ns\": null,\n",
                                 "\"name\": \"ETH_EXT\",\n", "\"kind\": \"ethernet\",\n", "\"control\": null,\n"},
                                {"[eth-bridge]", "not installed"});
            result |= run_single(options, options.pcapBridge,
                                 {"--describe", "--name", "EthBridge", "--network", "ETH_EXT", "--backend", kPcapBackend,
                                  "--interface", "fake0"},
                                 cli::kExitUsage, {"sends to a real adapter: say what may pass with"}, {"\"vehicle_sil_ecu\""});
        }
        else if (options.scenario == "pcap-launcher")
        {
            result = run_launcher(options);
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
