// SPDX-License-Identifier: MIT
//
// Process level tests of the bridge executable (built against the fake XL driver):
//   echo-roundtrip            registry + bridge (fake XL echo node on Virtual Channel 1) + SIL Kit
//                             peer. The peer checks every answer; the launcher checks the bridge's
//                             statistics (forwarded counts, no rejected frames). Without --bitrate
//                             the bridge has no init access, so it cannot switch TX receipts off:
//                             it must drop them (own_tx_dropped == sent) and the peer sees no loop.
//   echo-roundtrip-receipts   the same, with a driver that ignores xlCanSetChannelMode.
//   echo-roundtrip-physical   the same on a hardware CAN channel (fake "Test CAN Device Channel 1") selected
//                             with --xl-target physical, including --bitrate: with init access the
//                             receipts are switched off (own_tx_dropped == 0).
//   list-channels             --list-channels prints the CAN channels by target, not the LIN one.
//   bad-channel               a channel number that does not exist: exit 3 with the available list.
//   bad-usage                 an invalid --xl-target: exit 4.
//   no-driver                 the XL driver cannot be opened: exit 3.
#include "launcher_support.hpp"

#include "vehicle_sil/cli/args.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;
namespace cli = vehicle_sil::cli;

namespace {

/// Sets an environment variable for child processes started while it lives.
class ScopedEnv
{
public:
    ScopedEnv(std::string name, const std::string& value) : name_(std::move(name))
    {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    ~ScopedEnv()
    {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
};

struct Options
{
    fs::path registry, bridge, peer, workDir;
    std::string scenario;
    std::chrono::milliseconds timeout{60000};
};

Options parse(int argc, char** argv)
{
    const cli::Args args(argc, argv);
    Options options;
    options.registry = args.required("registry");
    options.bridge = args.required("bridge");
    options.peer = args.required("peer");
    options.workDir = args.required("work-dir");
    options.scenario = args.required("scenario");
    options.timeout = args.millis("timeout-ms", options.timeout);
    return options;
}

/// Value of "key=<number>" in `text`, or nullopt.
[[maybe_unused]] std::optional<unsigned long long> stat(const std::string& text, const std::string& key)
{
    const std::regex pattern("\\b" + key + "=([0-9]+)");
    std::smatch match;
    if (!std::regex_search(text, match, pattern))
    {
        return std::nullopt;
    }
    return std::stoull(match[1].str());
}

int run_echo(const Options& options, bool forceReceipts, bool physical)
{
    // Fake channel index 0 = "Virtual Channel 1", 2 = "Test CAN Device Channel 1" (see fake_xl::default_channels()).
    const ScopedEnv echo("VEHICLE_SIL_FAKE_XL_ECHO", physical ? "2" : "0");
    std::optional<ScopedEnv> receipts;
    if (forceReceipts)
    {
        receipts.emplace("VEHICLE_SIL_FAKE_XL_IGNORE_CHANNEL_MODE", "1");
    }
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());

    const fs::path bridgeLog = options.workDir / "bridge.log";
    const fs::path peerLog = options.workDir / "peer.log";
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::vector<std::string> bridgeArgs{"--registry-uri", registry.uri(), "--participant", "VectorXlBridge",
                                        "--network", "XL_IT_CAN", "--xl-target", physical ? "physical" : "virtual",
                                        "--xl-channel", "1"};
    if (physical)
    {
        bridgeArgs.insert(bridgeArgs.end(), {"--bitrate", "250000"});
    }
    ChildProcess bridge(options.bridge, bridgeArgs, bridgeLog);
    ChildProcess peer(options.peer,
                      {"--registry-uri", registry.uri(), "--network", "XL_IT_CAN", "--bridge", "VectorXlBridge",
                       "--timeout-ms", std::to_string(std::max<std::chrono::milliseconds>(options.timeout - 15s, 5s).count())},
                      peerLog);

    const auto peerCode = wait_or_kill(peer, deadline);
    // POSIX: kill() sends SIGTERM first; the bridge stops gracefully and prints its statistics.
    bridge.kill();
    [[maybe_unused]] const auto bridgeCode = bridge.poll(); // not checked on Windows (below)

    const std::string peerText = read_file(peerLog);
    const std::string bridgeText = read_file(bridgeLog);
    bool ok = expect(peerCode == 0, "peer exit code " + exit_code_text(peerCode) + ", expected 0");
    const std::string expectedChannel = physical ? "XL channel: physical 1 = \"Test CAN Device Channel 1\"" : "XL channel: virtual 1 = \"Virtual Channel 1\"";
    ok = expect(bridgeText.find(expectedChannel) != std::string::npos, "bridge selected '" + expectedChannel + "'") && ok;
    if (physical)
    {
        ok = expect(bridgeText.find("init access: yes, bit rate 250000") != std::string::npos,
                    "bridge got init access and set 250000 bit/s") && ok;
    }
#ifdef _WIN32
    log("bridge statistics not checked on Windows (the launcher cannot stop it gracefully)");
#else
    ok = expect(bridgeCode == 0, "bridge exit code " + exit_code_text(bridgeCode) + ", expected 0") && ok;
    const auto sent = stat(peerText, "sent");
    const auto toXl = stat(bridgeText, "silkit_to_xl");
    const auto toSilKit = stat(bridgeText, "xl_to_silkit");
    const auto dropped = stat(bridgeText, "own_tx_dropped");
    ok = expect(sent.has_value() && *sent > 0, "peer sent " + (sent ? std::to_string(*sent) : std::string("?")) + " frames") && ok;
    ok = expect(sent && toXl == sent, "bridge silkit_to_xl == sent") && ok;
    ok = expect(sent && toSilKit == sent, "bridge xl_to_silkit == sent (one answer per frame)") && ok;
    // TX receipts are off only with init access (--bitrate, the physical scenario) and a driver
    // that honours xlCanSetChannelMode.
    const bool receiptsExpected = forceReceipts || !physical;
    ok = expect(dropped == (receiptsExpected ? sent : std::optional<unsigned long long>(0)),
                std::string("bridge own_tx_dropped == ") + (receiptsExpected ? "sent" : "0")) && ok;
    for (const char* key : {"error_frames", "overruns", "silkit_rejected", "xl_rejected", "xl_queue_full",
                            "xl_tx_failures", "silkit_tx_failures"})
    {
        ok = expect(stat(bridgeText, key) == std::optional<unsigned long long>(0), std::string("bridge ") + key + " == 0") && ok;
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

/// Runs the bridge once with `args` and checks its exit code and output.
int run_single(const Options& options, const std::vector<std::string>& args, int expectedCode,
               const std::vector<std::string>& mustContain, const std::vector<std::string>& mustNotContain)
{
    const fs::path bridgeLog = options.workDir / "bridge.log";
    ChildProcess bridge(options.bridge, args, bridgeLog);
    const auto code = wait_or_kill(bridge, std::chrono::steady_clock::now() + options.timeout);
    const std::string text = read_file(bridgeLog);
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

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parse(argc, argv);
        fs::create_directories(options.workDir);
        log("scenario " + options.scenario + ", logs in " + options.workDir.string());
        int result = 1;
        if (options.scenario == "echo-roundtrip")
        {
            result = run_echo(options, false, false);
        }
        else if (options.scenario == "echo-roundtrip-physical")
        {
            result = run_echo(options, false, true);
        }
        else if (options.scenario == "echo-roundtrip-receipts")
        {
            result = run_echo(options, true, false);
        }
        else if (options.scenario == "list-channels")
        {
            result = run_single(options, {"--list-channels"}, cli::kExitOk,
                                {"virtual CAN channels (--xl-target virtual):", "--xl-channel 1  Virtual Channel 1",
                                 "--xl-channel 2  Virtual Channel 2", "physical CAN channels (--xl-target physical):",
                                 "--xl-channel 1  Test CAN Device Channel 1", "--xl-channel 2  Test CAN Device Channel 2"},
                                {"Test CAN Device Channel 3"});
        }
        else if (options.scenario == "bad-channel")
        {
            result = run_single(options,
                                {"--registry-uri", "silkit://127.0.0.1:1", "--xl-target", "physical", "--xl-channel", "3"}, cli::kExitSetupFailed,
                                {"no physical CAN channel 3; available: 1=\"Test CAN Device Channel 1\" 2=\"Test CAN Device Channel 2\""}, {});
        }
        else if (options.scenario == "bad-usage")
        {
            result = run_single(options, {"--registry-uri", "silkit://127.0.0.1:1", "--xl-target", "cancase", "--xl-channel", "1"},
                                cli::kExitUsage, {"--xl-target must be 'virtual' or 'physical'"}, {});
        }
        else if (options.scenario == "no-driver")
        {
            const ScopedEnv fail("VEHICLE_SIL_FAKE_XL_FAIL_OPEN", "1");
            result = run_single(options, {"--list-channels"}, cli::kExitSetupFailed, {"cannot open the Vector XL driver"}, {});
        }
        else
        {
            throw std::invalid_argument("unknown scenario " + options.scenario);
        }
        log(result == 0 ? "PASS" : "FAIL");
        return result;
    }
    catch (const std::exception& e)
    {
        log(std::string("FAIL: ") + e.what());
        return 1;
    }
}
