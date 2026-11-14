// SPDX-License-Identifier: MIT
//
// Integration test driver. Runs a real SIL Kit registry process plus the sender and receiver
// peers as independent processes, then decides PASS/FAIL from their exit codes. Each process
// logs to its own file in --work-dir; on failure the launcher prints all logs.
//
// Scenarios:
//   address-claim-and-pgn  sender claims an address with the J1939 stack (vehicle_sil::Peer,
//                          real time) and broadcasts PGNs, receiver verifies CAN IDs and
//                          payloads. Both must exit 0.
//   receiver-timeout       receiver runs without a sender and must report a timeout
//                          (exit code 2) within its time limit.
//   receiver-detects-wrong-name     the receiver expects another NAME: it must fail (exit 1).
//   receiver-detects-wrong-payload  the sender corrupts one PGN payload: the receiver must fail
//                                   (exit 1). Both guard the receiver's verification itself.
#include "launcher_support.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/cli/args.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace vehicle_sil::test;
using vehicle_sil::cli::kExitOk;
using vehicle_sil::cli::kExitTimeout;

namespace {

// Sender NAME fields and the 64-bit NAME the receiver must see (SAE J1939-81 layout):
//   identity number 0x1234 (bits 0-20), manufacturer code 2032 = 0x7F0 (bits 21-31),
//   function 130 = 0x82 (bits 40-47), industry group 2 (bits 60-62), everything else 0.
constexpr const char* kExpectedName = "0x20008200FE001234";
constexpr const char* kSourceAddress = "0x80";
constexpr const char* kPgn = "0xFF10";
constexpr const char* kMessageCount = "5";

struct Options
{
    fs::path registry;
    fs::path sender;
    fs::path receiver;
    fs::path workDir;
    std::string scenario;
    std::chrono::milliseconds timeout{60000};
};

Options parse(int argc, char** argv)
{
    const vehicle_sil::cli::Args args(argc, argv);
    Options options;
    options.registry = args.required("registry");
    options.sender = args.required("sender");
    options.receiver = args.required("receiver");
    options.workDir = args.required("work-dir");
    options.scenario = args.required("scenario");
    // An upper bound of waiting: scaled by VEHICLE_SIL_TEST_TIMEOUT_SCALE.
    options.timeout = vehicle_sil::test::scaled_timeout(args.millis("timeout-ms", options.timeout));
    return options;
}

struct Outcome
{
    std::string name;
    std::optional<int> exitCode; // nullopt: still running at the deadline (killed)
    int expected;
};

bool evaluate(const std::vector<Outcome>& outcomes)
{
    bool ok = true;
    for (const auto& outcome : outcomes)
    {
        const bool pass = outcome.exitCode && (*outcome.exitCode == outcome.expected);
        ok = expect(pass, outcome.name + " exit code: " + exit_code_text(outcome.exitCode) + ", expected " +
                              std::to_string(outcome.expected)) &&
             ok;
    }
    return ok;
}

/// Sender and receiver exchange the address claim and the PGNs. The receiver expects
/// `expectedName` and must exit with `receiverExit`; `senderExtra` adds sender options.
int run_exchange(const Options& options, const std::string& expectedName, int receiverExit,
                 const std::vector<std::string>& senderExtra, const std::string& receiverMustLog)
{
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());

    const fs::path receiverLog = options.workDir / "receiver.log";
    const fs::path senderLog = options.workDir / "sender.log";
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    // Peers give up a little before the launcher does, so their own diagnostics end up in the logs.
    const std::string peerTimeout = std::to_string(std::max<std::chrono::milliseconds>(options.timeout - 10s, 5s).count());

    ChildProcess receiver(options.receiver,
                          {"--registry-uri", registry.uri(), "--participant", "J1939Receiver", "--network", "J1939_CAN1",
                           "--expect-source-address", kSourceAddress, "--expect-name", expectedName, "--pgn", kPgn,
                           "--count", kMessageCount, "--timeout-ms", peerTimeout},
                          receiverLog);
    std::vector<std::string> senderArgs{"--registry-uri", registry.uri(), "--participant", "J1939Sender", "--network",
                                        "J1939_CAN1", "--wait-for", "J1939Receiver", "--linger-for", "J1939Receiver",
                                        "--source-address", kSourceAddress, "--identity-number", "0x1234",
                                        "--manufacturer-code", "2032", "--function-code", "130", "--industry-group", "2",
                                        "--pgn", kPgn, "--count", kMessageCount, "--timeout-ms", peerTimeout};
    senderArgs.insert(senderArgs.end(), senderExtra.begin(), senderExtra.end());
    ChildProcess sender(options.sender, senderArgs, senderLog);

    const auto receiverCode = wait_or_kill(receiver, deadline);
    const auto senderCode = wait_or_kill(sender, deadline);
    registry.stop();

    bool ok = evaluate({{"receiver", receiverCode, receiverExit}, {"sender", senderCode, kExitOk}});
    if (!receiverMustLog.empty())
    {
        ok = expect(read_file(receiverLog).find(receiverMustLog) != std::string::npos,
                    "receiver reported '" + receiverMustLog + "'") &&
             ok;
    }
    if (!ok)
    {
        dump_log(registry.log_file());
        dump_log(receiverLog);
        dump_log(senderLog);
    }
    else
    {
        // Show the receiver's verification summary even on success; it is the evidence.
        dump_log(receiverLog);
    }
    return ok ? 0 : 1;
}

int run_receiver_timeout(const Options& options)
{
    RegistryProcess registry(options.registry, options.workDir);
    log("registry listening on " + registry.uri());

    const fs::path receiverLog = options.workDir / "receiver.log";
    constexpr auto kReceiverTimeout = 1500ms;
    const auto started = std::chrono::steady_clock::now();
    ChildProcess receiver(options.receiver,
                          {"--registry-uri", registry.uri(), "--participant", "J1939Receiver", "--network",
                           "J1939_CAN_TIMEOUT", "--expect-name", kExpectedName, "--timeout-ms",
                           std::to_string(kReceiverTimeout.count())},
                          receiverLog);
    const auto receiverCode = wait_or_kill(receiver, started + options.timeout);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    registry.stop();

    bool ok = evaluate({{"receiver", receiverCode, kExitTimeout}});
    // Start-up and shutdown add to the receive timeout; anything far beyond it means the
    // timeout did not work.
    const bool timely = elapsed >= kReceiverTimeout && elapsed < kReceiverTimeout + 15s;
    ok = expect(timely, "receiver gave up after " + std::to_string(elapsed.count()) + " ms (timeout " +
                            std::to_string(kReceiverTimeout.count()) + " ms)") &&
         ok;
    if (!ok)
    {
        dump_log(registry.log_file());
    }
    dump_log(receiverLog);
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
        if (options.scenario == "address-claim-and-pgn")
        {
            result = run_exchange(options, kExpectedName, kExitOk, {}, "");
        }
        else if (options.scenario == "receiver-detects-wrong-name")
        {
            result = run_exchange(options, "0x20008200FE001235", vehicle_sil::cli::kExitFailed, {}, "NAME mismatch");
        }
        else if (options.scenario == "receiver-detects-wrong-payload")
        {
            result = run_exchange(options, kExpectedName, vehicle_sil::cli::kExitFailed, {"--corrupt-message", "2"},
                                  "PGN payload mismatch for message 2");
        }
        else if (options.scenario == "receiver-timeout")
        {
            result = run_receiver_timeout(options);
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
