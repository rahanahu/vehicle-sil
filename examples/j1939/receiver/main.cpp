// SPDX-License-Identifier: MIT
//
// vehicle_sil_j1939_receiver: verifies the traffic of vehicle_sil_j1939_sender on the SIL Kit CAN bus.
//
// It deliberately does not use the J1939 stack. Frames are taken straight from the SIL Kit CAN
// controller and compared byte for byte with expectations derived from SAE J1939-21/-81, so
// the check is independent of the stack under test.
//
// Expected sequence from the sender (source address SA, NAME N, broadcast PGN P):
//   1. Address Claim              id = 0x18EEFF<SA> data = N (little endian, 8 bytes)
//   2. count x PGN P              id = 0x18<P><SA>  data = test_payload(0..count-1)
// The stack starts with its own claim; a Request for Address Claim is not expected.
#include "../common/peer_common.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/can/all.hpp>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <limits>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil;
using namespace vehicle_sil::j1939_example;
namespace can = SilKit::Services::Can;

constexpr const char* kTag = "receiver";

// --- SAE J1939-21 identifier layout (test oracle, intentionally independent of the stack)
constexpr std::uint32_t kPriority6 = 6;
constexpr std::uint8_t kGlobalAddress = 0xFF;
constexpr std::uint32_t kPgnAddressClaim = 0xEE00;

constexpr std::uint32_t pdu1_id(std::uint32_t priority, std::uint32_t pgn, std::uint8_t destination, std::uint8_t source)
{
    return (priority << 26) | ((pgn & 0x3FF00u) << 8) | (static_cast<std::uint32_t>(destination) << 8) | source;
}

constexpr std::uint32_t pdu2_id(std::uint32_t priority, std::uint32_t pgn, std::uint8_t source)
{
    return (priority << 26) | ((pgn & 0x3FFFFu) << 8) | source;
}

static_assert(pdu1_id(6, kPgnAddressClaim, kGlobalAddress, 0x80) == 0x18EEFF80u);
static_assert(pdu2_id(6, 0xFF10, 0x80) == 0x18FF1080u);

struct RxFrame
{
    std::uint32_t id = 0;
    can::CanFrameFlagMask flags = 0;
    std::uint16_t dlc = 0;
    std::vector<std::uint8_t> data;
};

std::string describe(const RxFrame& frame)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "id=0x%08" PRIX32 " flags=0x%" PRIX32 " dlc=%u data=", frame.id,
                  static_cast<std::uint32_t>(frame.flags), static_cast<unsigned>(frame.dlc));
    std::string text = buffer;
    for (const auto byte : frame.data)
    {
        std::snprintf(buffer, sizeof(buffer), "%02X ", byte);
        text += buffer;
    }
    return text;
}

std::string hex_bytes(const std::uint8_t* data, std::size_t size)
{
    std::string text;
    char buffer[4];
    for (std::size_t i = 0; i < size; ++i)
    {
        std::snprintf(buffer, sizeof(buffer), "%02X ", data[i]);
        text += buffer;
    }
    return text;
}

/// Bounded hand-off from the SIL Kit receive thread to the main thread.
class FrameInbox
{
public:
    explicit FrameInbox(std::size_t capacity) : capacity_(capacity) {}

    void push(const can::CanFrameEvent& event)
    {
        {
            const std::lock_guard lock(mutex_);
            if (frames_.size() >= capacity_)
            {
                overflowed_ = true;
                return;
            }
            RxFrame frame;
            frame.id = event.frame.canId;
            frame.flags = event.frame.flags;
            frame.dlc = event.frame.dlc;
            frame.data.assign(event.frame.dataField.begin(), event.frame.dataField.end());
            frames_.push_back(std::move(frame));
        }
        cv_.notify_one();
    }

    /// Waits until a frame is available or the deadline passes.
    bool pop(RxFrame& out, std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_until(lock, deadline, [this] { return !frames_.empty(); }))
        {
            return false;
        }
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

    bool overflowed() const
    {
        const std::lock_guard lock(mutex_);
        return overflowed_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<RxFrame> frames_;
    bool overflowed_ = false;
};

struct Expectations
{
    std::uint8_t sourceAddress;
    std::uint64_t name;
    std::uint32_t pgn;
    std::uint32_t count;
};

/// Checks frames in arrival order. Any deviation is a verification failure.
class Verifier
{
public:
    explicit Verifier(const Expectations& expect) :
      expect_(expect),
      claimId_(pdu1_id(kPriority6, kPgnAddressClaim, kGlobalAddress, expect.sourceAddress)),
      pgnId_(pdu2_id(kPriority6, expect.pgn, expect.sourceAddress))
    {
    }

    /// @returns false on the first mismatch (details in failure()).
    bool check(const RxFrame& frame)
    {
        if ((frame.flags & static_cast<can::CanFrameFlagMask>(can::CanFrameFlag::Ide)) == 0)
        {
            return fail("expected a 29-bit extended frame: " + describe(frame));
        }
        if (frame.id == claimId_)
        {
            if (claimSeen_)
            {
                return fail("a second Address Claim: " + describe(frame));
            }
            std::uint8_t expected[8];
            for (int i = 0; i < 8; ++i)
            {
                expected[i] = static_cast<std::uint8_t>(expect_.name >> (8 * i));
            }
            if (!payload_equals(frame, expected, sizeof(expected)))
            {
                return fail("Address Claim NAME mismatch: " + describe(frame) + " expected " +
                            hex_bytes(expected, sizeof(expected)));
            }
            claimSeen_ = true;
            return true;
        }
        if (frame.id == pgnId_)
        {
            if (!claimSeen_)
            {
                return fail("PGN frame arrived before the Address Claim: " + describe(frame));
            }
            if (pgnFrames_ >= expect_.count)
            {
                return fail("more PGN frames than expected: " + describe(frame));
            }
            const auto expected = test_payload(pgnFrames_);
            if (!payload_equals(frame, expected.data(), expected.size()))
            {
                return fail("PGN payload mismatch for message " + std::to_string(pgnFrames_) + ": " + describe(frame) +
                            " expected " + hex_bytes(expected.data(), expected.size()));
            }
            ++pgnFrames_;
            return true;
        }
        char ids[128];
        std::snprintf(ids, sizeof(ids), " (expected 0x%08" PRIX32 " or 0x%08" PRIX32 ")",
                      claimId_, pgnId_);
        return fail("unexpected CAN identifier: " + describe(frame) + ids);
    }

    [[nodiscard]] bool complete() const { return claimSeen_ && pgnFrames_ == expect_.count; }
    [[nodiscard]] const std::string& failure() const { return failure_; }

    void report() const
    {
        log(kTag, "CHECK address_claim id=0x%08" PRIX32 " name=0x%016" PRIX64 " : %s", claimId_, expect_.name,
            claimSeen_ ? "received" : "MISSING");
        log(kTag, "CHECK pgn id=0x%08" PRIX32 " : %u/%u frames with matching payload", pgnId_, pgnFrames_, expect_.count);
    }

private:
    static bool payload_equals(const RxFrame& frame, const std::uint8_t* expected, std::size_t size)
    {
        return frame.dlc == size && frame.data.size() == size && std::equal(frame.data.begin(), frame.data.end(), expected);
    }

    bool fail(std::string message)
    {
        failure_ = std::move(message);
        return false;
    }

    Expectations expect_;
    std::uint32_t claimId_;
    std::uint32_t pgnId_;
    bool claimSeen_ = false;
    std::uint32_t pgnFrames_ = 0;
    std::string failure_;
};

void print_usage()
{
    std::fputs(
        "usage: vehicle_sil_j1939_receiver --registry-uri <silkit://host:port> --expect-name <NAME> [options]\n"
        "  --participant <name>             SIL Kit participant name (J1939Receiver)\n"
        "  --network <name>                 SIL Kit CAN network (J1939_CAN1)\n"
        "  --expect-source-address <sa>     source address the sender claims (0x80)\n"
        "  --expect-name <NAME>             64-bit J1939 NAME expected in the Address Claim\n"
        "  --pgn <pgn>                      broadcast PGN to expect (0xFF10)\n"
        "  --count <n>                      number of PGN messages to expect (5)\n"
        "  --timeout-ms <ms>                overall time limit after start-up (20000)\n",
        stderr);
}

int run(const Args& args)
{
    const std::string registryUri = args.required("registry-uri");
    const std::string participantName = args.str("participant", "J1939Receiver");
    const std::string network = args.str("network", "J1939_CAN1");
    const Expectations expect{
        static_cast<std::uint8_t>(args.u64("expect-source-address", 0x80, 0xFD)),
        args.required_u64("expect-name", std::numeric_limits<std::uint64_t>::max()),
        static_cast<std::uint32_t>(args.u64("pgn", 0xFF10, 0x3FFFF)),
        static_cast<std::uint32_t>(args.u64("count", 5, 1000)),
    };
    const auto timeout = args.millis("timeout-ms", std::chrono::milliseconds(20000));

    // The inbox is declared before the runtime so that it outlives the participant whose frame
    // handler writes into it, even if an exception unwinds this function.
    FrameInbox inbox(256);
    std::unique_ptr<ParticipantRuntime> runtime;
    try
    {
        runtime = std::make_unique<ParticipantRuntime>(ParticipantRuntimeOptions{participantName, registryUri});
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: cannot connect to SIL Kit registry %s: %s", registryUri.c_str(), e.what());
        return kExitSetupFailed;
    }

    auto& controller = *runtime->participant().CreateCanController("J1939CAN", network);
    const auto handler = controller.AddFrameHandler(
        [&inbox](can::ICanController*, const can::CanFrameEvent& event) { inbox.push(event); });
    // Where SIL Kit wants a controller started: from the CommunicationReady handler.
    runtime->add_communication_ready_handler([&controller] { controller.Start(); });

    try
    {
        runtime->start(timeout);
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
        controller.RemoveFrameHandler(handler);
        return kExitSetupFailed;
    }
    log(kTag, "participant '%s' running on network '%s' via %s; waiting up to %lld ms", participantName.c_str(),
        network.c_str(), registryUri.c_str(), static_cast<long long>(timeout.count()));

    Verifier verifier(expect);
    int result = kExitOk;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!verifier.complete())
    {
        RxFrame frame;
        if (!inbox.pop(frame, deadline))
        {
            log(kTag, "FAIL timeout: expected traffic not complete after %lld ms", static_cast<long long>(timeout.count()));
            result = kExitTimeout;
            break;
        }
        log(kTag, "rx %s", describe(frame).c_str());
        if (!verifier.check(frame))
        {
            log(kTag, "FAIL verification: %s", verifier.failure().c_str());
            result = kExitVerificationFailed;
            break;
        }
    }
    if (inbox.overflowed())
    {
        log(kTag, "FAIL verification: receive inbox overflowed, frames were lost");
        result = kExitVerificationFailed;
    }

    verifier.report();
    // Stop receiving before the inbox goes out of scope; RemoveFrameHandler waits for a
    // handler call that is in progress.
    controller.RemoveFrameHandler(handler);
    controller.Stop();
    runtime->stop(result == kExitOk ? "receiver verified all frames" : "receiver failed");
    log(kTag, "%s", result == kExitOk ? "PASS" : "FAIL");
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return cli::run_main(argc, argv, kTag, print_usage, run);
}
