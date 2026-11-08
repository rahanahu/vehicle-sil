// SPDX-License-Identifier: MIT
//
// Integration test peer for vehicle_sil_vector_xl_bridge. Joins the SIL Kit CAN network, waits
// for the bridge, sends every frame of frame_matrix() and expects exactly one answer per frame
// from the fake XL echo node behind the bridge (same identifier; data frames: inverted bytes;
// remote frames: a data frame with bytes 0,1,2,...). Anything extra (a forwarding loop) fails.
//
// Exit codes: 0 pass, 1 verification failed, 2 timeout, 3 setup failed, 4 usage.
#include "frame_matrix.hpp"

#include "vehicle_sil/cli/console.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/can/all.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace can = SilKit::Services::Can;
using namespace std::chrono_literals;
using namespace vehicle_sil;

struct Received
{
    std::uint32_t id;
    can::CanFrameFlagMask flags;
    std::uint16_t dlc;
    std::vector<std::uint8_t> data;
};

std::string describe(std::uint32_t id, can::CanFrameFlagMask flags, std::uint16_t dlc, const std::vector<std::uint8_t>& data)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "id=0x%X flags=0x%X dlc=%u data=", static_cast<unsigned>(id),
                  static_cast<unsigned>(flags), static_cast<unsigned>(dlc));
    std::string text = buffer;
    for (const auto byte : data)
    {
        std::snprintf(buffer, sizeof(buffer), "%02X ", byte);
        text += buffer;
    }
    return text;
}

void print_usage()
{
    std::fputs("usage: xl_bridge_it_peer --registry-uri <uri> --network <name> --bridge <participant> [--timeout-ms n]\n", stderr);
}

int run(const cli::Args& args)
{
    const std::string registryUri = args.required("registry-uri");
    const std::string network = args.required("network");
    const std::string bridge = args.required("bridge");
    const auto timeout = args.millis("timeout-ms", std::chrono::milliseconds(20000));

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Received> inbox;

    ParticipantRuntime runtime({"XlItPeer", registryUri});
    auto& controller = *runtime.participant().CreateCanController("CAN", network);
    const auto handler = controller.AddFrameHandler([&](can::ICanController*, const can::CanFrameEvent& event) {
        {
            const std::lock_guard lock(mutex);
            inbox.push_back({event.frame.canId, event.frame.flags, event.frame.dlc,
                             std::vector<std::uint8_t>(event.frame.dataField.begin(), event.frame.dataField.end())});
        }
        cv.notify_all();
    });
    controller.Start();
    runtime.start(timeout);
    if (!runtime.wait_for_peer(bridge, is_running, timeout))
    {
        std::printf("[peer] FAIL timeout: bridge participant '%s' not running\n", bridge.c_str());
        return cli::kExitTimeout;
    }

    const auto matrix = test::frame_matrix();
    for (const auto& frame : matrix)
    {
        controller.SendFrame(frame.to_silkit());
        std::this_thread::sleep_for(2ms);
    }
    std::printf("[peer] sent=%zu\n", matrix.size());
    std::fflush(stdout);

    int result = cli::kExitOk;
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, timeout, [&] { return inbox.size() >= matrix.size(); }))
        {
            std::printf("[peer] FAIL timeout: %zu of %zu answers\n", inbox.size(), matrix.size());
            result = cli::kExitTimeout;
        }
    }
    // Grace period: a forwarding loop would keep producing frames.
    std::this_thread::sleep_for(500ms);
    // Stop receiving before taking the inbox lock: RemoveFrameHandler waits for a running handler,
    // which itself takes that lock.
    controller.RemoveFrameHandler(handler);

    const std::lock_guard lock(mutex);
    for (std::size_t i = 0; result == 0 && i < matrix.size(); ++i)
    {
        const auto& sent = matrix[i];
        std::vector<std::uint8_t> expected(std::min<std::size_t>(sent.dlc, 8));
        for (std::size_t b = 0; b < expected.size(); ++b)
        {
            expected[b] = sent.remote ? static_cast<std::uint8_t>(b) : static_cast<std::uint8_t>(~sent.data[b]);
        }
        const auto expectedFlags =
            static_cast<can::CanFrameFlagMask>(sent.extended ? static_cast<can::CanFrameFlagMask>(can::CanFrameFlag::Ide) : 0u);
        const auto& got = inbox[i];
        // The bridge sends DLC 9..15 (8 data bytes) as DLC 8: xlCanTransmit takes 0..8.
        const auto expectedDlc = std::min<std::uint16_t>(sent.dlc, 8);
        if (got.id != sent.id || got.flags != expectedFlags || got.dlc != expectedDlc || got.data != expected)
        {
            std::printf("[peer] FAIL verification: answer %zu is %s, expected %s\n", i,
                        describe(got.id, got.flags, got.dlc, got.data).c_str(),
                        describe(sent.id, expectedFlags, expectedDlc, expected).c_str());
            result = cli::kExitFailed;
        }
    }
    if (result == 0 && inbox.size() != matrix.size())
    {
        std::printf("[peer] FAIL verification: %zu frames received, expected exactly %zu (forwarding loop?)\n",
                    inbox.size(), matrix.size());
        result = cli::kExitFailed;
    }
    std::printf("[peer] CHECK answers=%zu expected=%zu -> %s\n", inbox.size(), matrix.size(), result == 0 ? "ok" : "MISMATCH");
    runtime.stop("peer done");
    std::printf("[peer] %s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return cli::run_main(argc, argv, "peer", print_usage, run);
}
