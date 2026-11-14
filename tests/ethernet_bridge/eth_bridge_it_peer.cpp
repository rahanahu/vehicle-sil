// SPDX-License-Identifier: MIT
//
// The SIL Kit side of the tests of vehicle_sil_ethernet_bridge: a participant with an Ethernet
// controller of SIL Kit itself (no ECU port). It waits for the bridge, then
//
//   --mode ask     sends frames (EtherType 0x88B5, sizes from the smallest to 1514 bytes, to the
//                  device and to the broadcast address) and one frame of another EtherType, and
//                  expects exactly one answer per frame of the first kind, from the device behind
//                  the bridge: to this peer, the bytes behind the header inverted. Anything else
//                  (a missing answer, a frame too many: a forwarding loop) fails.
//   --mode answer  prints every frame it receives ("[peer] rx <hex>") and sends it back to its
//                  source from this peer's address, the rest unchanged, until SIGTERM or SIGINT;
//                  then prints "[peer] answered=<n>".
//
// 0x88B5 is an EtherType known to be free for local experiments (not checked against the standard).
//
// Exit codes: 0 pass, 1 verification failed, 2 timeout, 3 setup failed, 4 usage.
#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/eth_link/mac_address.hpp"

#include <silkit/services/ethernet/all.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ethernet = SilKit::Services::Ethernet;
using namespace std::chrono_literals;
using namespace vehicle_sil;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kExperimental = 0x88B5;
constexpr std::uint16_t kOtherEtherType = 0x88B6;

volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_stop_signal(int)
{
    g_stop = 1;
}

std::string hex(const Bytes& bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes)
    {
        text += digits[byte >> 4];
        text += digits[byte & 0x0F];
    }
    return text;
}

eth_link::MacAddress mac_option(const cli::Args& args, const std::string& key, const std::string& fallback)
{
    const auto text = args.str(key, fallback);
    const auto mac = eth_link::parse_mac_address(text);
    if (!mac)
    {
        throw cli::UsageError("--" + key + " is not a MAC address: " + text);
    }
    return *mac;
}

Bytes make_frame(const eth_link::MacAddress& destination, const eth_link::MacAddress& source, std::uint16_t ether_type,
                 std::size_t payload_bytes, std::uint8_t first)
{
    Bytes frame(destination.bytes.begin(), destination.bytes.end());
    frame.insert(frame.end(), source.bytes.begin(), source.bytes.end());
    frame.push_back(static_cast<std::uint8_t>(ether_type >> 8));
    frame.push_back(static_cast<std::uint8_t>(ether_type & 0xFF));
    for (std::size_t i = 0; i < payload_bytes; ++i)
    {
        frame.push_back(static_cast<std::uint8_t>(first + i));
    }
    return frame;
}

void print_usage()
{
    std::fputs("usage: eth_bridge_it_peer --registry-uri <uri> --network <name> --bridge <participant> --mode <ask|answer>\n"
               "                          [--mac <mac>] [--device-mac <mac>] [--timeout-ms n]\n",
               stderr);
}

int run(const cli::Args& args)
{
    const std::string registryUri = args.required("registry-uri");
    const std::string network = args.required("network");
    const std::string bridge = args.required("bridge");
    const std::string mode = args.required("mode");
    if (mode != "ask" && mode != "answer")
    {
        throw cli::UsageError("--mode must be ask or answer, got: " + mode);
    }
    const auto own = mac_option(args, "mac", "02:00:00:00:00:01");
    const auto device = mac_option(args, "device-mac", "02:00:00:00:00:02");
    const auto timeout = args.millis("timeout-ms", std::chrono::milliseconds(20000));

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Bytes> inbox;

    ParticipantRuntime runtime({"EthItPeer", registryUri});
    auto& controller = *runtime.participant().CreateEthernetController("ETH", network);
    const auto handler = controller.AddFrameHandler([&](ethernet::IEthernetController*, const ethernet::EthernetFrameEvent& event) {
        {
            const std::lock_guard lock(mutex);
            inbox.emplace_back(event.frame.raw.begin(), event.frame.raw.end());
        }
        cv.notify_all();
    });
    controller.Activate();
    runtime.start(timeout);
    if (!runtime.wait_for_peer(bridge, is_running, timeout))
    {
        std::printf("[peer] FAIL timeout: bridge participant '%s' not running\n", bridge.c_str());
        return cli::kExitTimeout;
    }

    if (mode == "answer")
    {
        std::signal(SIGTERM, on_stop_signal);
        std::signal(SIGINT, on_stop_signal);
        std::printf("[peer] ready\n");
        std::fflush(stdout);
        std::size_t answered = 0;
        while (g_stop == 0)
        {
            std::vector<Bytes> frames;
            {
                std::unique_lock lock(mutex);
                cv.wait_for(lock, 50ms, [&] { return !inbox.empty(); });
                frames.swap(inbox);
            }
            for (auto& frame : frames)
            {
                std::printf("[peer] rx %s\n", hex(frame).c_str());
                std::fflush(stdout);
                if (frame.size() < eth_link::kEthernetHeaderBytes)
                {
                    continue;
                }
                for (std::size_t i = 0; i < 6; ++i)
                {
                    frame[i] = frame[6 + i];
                    frame[6 + i] = own.bytes[i];
                }
                controller.SendFrame(ethernet::EthernetFrame{frame});
                ++answered;
            }
        }
        controller.RemoveFrameHandler(handler);
        std::printf("[peer] answered=%zu\n", answered);
        runtime.stop("peer done");
        return cli::kExitOk;
    }

    // ask
    const eth_link::MacAddress broadcast{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    std::vector<Bytes> sent;
    for (std::size_t i = 0; i < 22; ++i)
    {
        // 60 bytes (SIL Kit pads shorter frames for the receiver) up to 1514.
        const std::size_t payload = i == 21 ? 1500 : 46 + i * 70;
        sent.push_back(make_frame(i % 4 == 3 ? broadcast : device, own, kExperimental, payload, static_cast<std::uint8_t>(i)));
    }
    for (std::size_t i = 0; i < sent.size(); ++i)
    {
        if (i == 5)
        {
            // Not one of the EtherTypes the bridge lets out: no answer.
            controller.SendFrame(ethernet::EthernetFrame{make_frame(device, own, kOtherEtherType, 46, 0)});
        }
        controller.SendFrame(ethernet::EthernetFrame{sent[i]});
        std::this_thread::sleep_for(2ms);
    }
    std::printf("[peer] sent=%zu\n", sent.size());
    std::fflush(stdout);

    int result = cli::kExitOk;
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, timeout, [&] { return inbox.size() >= sent.size(); }))
        {
            std::printf("[peer] FAIL timeout: %zu of %zu answers\n", inbox.size(), sent.size());
            result = cli::kExitTimeout;
        }
    }
    // Grace period: a forwarding loop would keep producing frames.
    std::this_thread::sleep_for(500ms);
    // Stop receiving before taking the inbox lock: RemoveFrameHandler waits for a running handler,
    // which itself takes that lock.
    controller.RemoveFrameHandler(handler);

    const std::lock_guard lock(mutex);
    for (std::size_t i = 0; result == 0 && i < sent.size(); ++i)
    {
        Bytes expected = sent[i];
        for (std::size_t b = 0; b < 6; ++b)
        {
            expected[b] = own.bytes[b];
            expected[6 + b] = device.bytes[b];
        }
        for (std::size_t b = eth_link::kEthernetHeaderBytes; b < expected.size(); ++b)
        {
            expected[b] = static_cast<std::uint8_t>(~expected[b]);
        }
        if (inbox[i] != expected)
        {
            std::printf("[peer] FAIL verification: answer %zu is %s, expected %s\n", i, hex(inbox[i]).c_str(), hex(expected).c_str());
            result = cli::kExitFailed;
        }
    }
    if (result == 0 && inbox.size() != sent.size())
    {
        std::printf("[peer] FAIL verification: %zu frames received, expected exactly %zu (forwarding loop?)\n", inbox.size(),
                    sent.size());
        result = cli::kExitFailed;
    }
    std::printf("[peer] CHECK answers=%zu expected=%zu -> %s\n", inbox.size(), sent.size(), result == 0 ? "ok" : "MISMATCH");
    runtime.stop("peer done");
    std::printf("[peer] %s\n", result == 0 ? "PASS" : "FAIL");
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return cli::run_main(argc, argv, "peer", print_usage, run);
}
