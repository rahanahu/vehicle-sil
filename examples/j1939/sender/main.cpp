// SPDX-License-Identifier: MIT
//
// vehicle_sil_j1939_sender: a J1939 peer on the vehicle-sil J1939 stack, in real time.
//
// It joins a SIL Kit simulation as a vehicle_sil::Peer, waits until the receiver runs, then
// adds a J1939 node that claims its address with the given NAME and, once the address is its
// own, broadcasts a series of single-frame PDU2 messages. Every CAN frame leaves through the
// peer's SIL Kit CAN port; a read-only tap on the port counts the frames the port accepted.
#include "../common/peer_common.hpp"

#include "vehicle_sil/can/tap_port.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace vehicle_sil;
using namespace vehicle_sil::j1939_example;
using Clock = std::chrono::steady_clock;

constexpr const char* kTag = "sender";

bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (!condition())
    {
        if (Clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

/// Counts the frames of one source address that the port accepted (on the driver's thread; read
/// from the main thread).
class SentFrames final : public can::TapListener
{
public:
    SentFrames(j1939::Address source, j1939::Pgn pgn) : source_(source), pgn_(pgn) {}

    void on_tap(const can::TapRecord& record) override
    {
        if (record.direction != can::TapRecord::Direction::Sent || record.result != can::SendResult::Accepted)
        {
            return;
        }
        const auto id = j1939::Identifier::decode(record.frame);
        if (!id || id->source != source_)
        {
            return;
        }
        if (id->pgn == j1939::kPgnAddressClaimed)
        {
            claims.fetch_add(1);
        }
        else if (id->pgn == pgn_)
        {
            messages.fetch_add(1);
        }
    }

    std::atomic<std::uint32_t> claims{0};
    std::atomic<std::uint32_t> messages{0};

private:
    j1939::Address source_;
    j1939::Pgn pgn_;
};

void print_usage()
{
    std::fputs(
        "usage: vehicle_sil_j1939_sender --registry-uri <silkit://host:port> [options]\n"
        "  --participant <name>          SIL Kit participant name (J1939Sender)\n"
        "  --network <name>              SIL Kit CAN network (J1939_CAN1)\n"
        "  --wait-for <participant>      wait until this participant is Running before claiming\n"
        "  --linger-for <participant>    after sending, wait until this participant has finished\n"
        "  --source-address <sa>         J1939 source address to claim (0x80)\n"
        "  --identity-number <n>         NAME identity number (0x1234)\n"
        "  --manufacturer-code <n>       NAME manufacturer code (2032)\n"
        "  --function-code <n>           NAME function (130)\n"
        "  --function-instance <n>       NAME function instance (0)\n"
        "  --ecu-instance <n>            NAME ECU instance (0)\n"
        "  --device-class <n>            NAME device class / vehicle system (0)\n"
        "  --device-class-instance <n>   NAME device class instance (0)\n"
        "  --industry-group <n>          NAME industry group (2)\n"
        "  --arbitrary-address-capable <0|1>  NAME arbitrary address capable bit (0)\n"
        "  --pgn <pgn>                   PDU2 PGN to broadcast (0xFF10)\n"
        "  --count <n>                   number of messages (5)\n"
        "  --corrupt-message <n>         send message n with a wrong payload byte (tests the receiver)\n"
        "  --interval-ms <ms>            delay between messages (20)\n"
        "  --timeout-ms <ms>             timeout of each waiting step (15000)\n",
        stderr);
}

int run(const Args& args)
{
    const std::string registryUri = args.required("registry-uri");
    const std::string participantName = args.str("participant", "J1939Sender");
    const std::string network = args.str("network", "J1939_CAN1");
    const std::string waitFor = args.str("wait-for", "");
    const std::string lingerFor = args.str("linger-for", "");
    const auto sourceAddress = static_cast<j1939::Address>(args.u64("source-address", 0x80, 0xFD));
    const j1939::Pgn pgn{static_cast<std::uint32_t>(args.u64("pgn", 0xFF10, 0x3FFFF))};
    const auto count = static_cast<std::uint32_t>(args.u64("count", 5, 1000));
    const auto corrupt = args.optional_u64("corrupt-message", 999);
    const auto interval = std::chrono::duration_cast<SimDuration>(args.millis("interval-ms", std::chrono::milliseconds(20)));
    const auto timeout = args.millis("timeout-ms", std::chrono::milliseconds(15000));

    if (((pgn.value() >> 8) & 0xFFu) < 0xF0u)
    {
        throw UsageError("--pgn must be a PDU2 (broadcast) PGN, PF >= 0xF0");
    }

    const auto name = j1939::Name{}
                          .set_identity_number(static_cast<std::uint32_t>(args.u64("identity-number", 0x1234, 0x1FFFFF)))
                          .set_manufacturer_code(static_cast<std::uint16_t>(args.u64("manufacturer-code", 0x7F0, 0x7FF)))
                          .set_function(static_cast<std::uint8_t>(args.u64("function-code", 130, 0xFF)))
                          .set_function_instance(static_cast<std::uint8_t>(args.u64("function-instance", 0, 0x1F)))
                          .set_ecu_instance(static_cast<std::uint8_t>(args.u64("ecu-instance", 0, 0x7)))
                          .set_vehicle_system(static_cast<std::uint8_t>(args.u64("device-class", 0, 0x7F)))
                          .set_vehicle_system_instance(static_cast<std::uint8_t>(args.u64("device-class-instance", 0, 0xF)))
                          .set_industry_group(static_cast<std::uint8_t>(args.u64("industry-group", 2, 0x7)))
                          .set_arbitrary_address_capable(args.u64("arbitrary-address-capable", 0, 1) != 0);

    // Used by the steps: declared before the peer, so they outlive it.
    SentFrames sent(sourceAddress, pgn);
    std::atomic<bool> claimed{false};
    j1939::Node* node = nullptr; // added in a step once the receiver runs
    std::uint32_t queued = 0;
    SimTime nextSend{};

    PeerOptions peerOptions;
    peerOptions.participant = participantName;
    peerOptions.registry_uri = registryUri;
    peerOptions.lifecycle = LifecycleMode::Autonomous;
    peerOptions.virtual_time = std::nullopt; // real time: the peer's driver steps the stack
    std::unique_ptr<Peer> peer;
    try
    {
        peer = std::make_unique<Peer>(peerOptions);
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: cannot connect to SIL Kit registry %s: %s", registryUri.c_str(), e.what());
        return kExitSetupFailed;
    }
    J1939ChannelOptions channel;
    channel.controller_name = "J1939CAN";
    auto& bus = peer->j1939(network, channel);
    peer->tap(network).set_listener(&sent);
    // The messages: once the address is the node's own, one per interval.
    peer->on_step([&](SimTime now) {
        if (node == nullptr || node->address_state() != j1939::AddressState::Claimed || queued >= count || now < nextSend)
        {
            return;
        }
        auto payload = test_payload(queued);
        if (corrupt && *corrupt == queued)
        {
            payload[1] ^= 0xFF;
        }
        const auto status = node->send(pgn, payload);
        if (status == j1939::SendStatus::Ok)
        {
            ++queued;
            nextSend = now + interval;
        }
        else if (status != j1939::SendStatus::QueueFull)
        {
            throw std::runtime_error("the stack refused message " + std::to_string(queued));
        }
    });
    try
    {
        peer->start(timeout);
    }
    catch (const std::exception& e)
    {
        log(kTag, "FAIL setup: %s", e.what());
        return kExitSetupFailed;
    }
    log(kTag, "participant '%s' running on network '%s' via %s", participantName.c_str(), network.c_str(), registryUri.c_str());

    if (!waitFor.empty())
    {
        if (!peer->runtime().wait_for_peer(waitFor, is_running, timeout))
        {
            log(kTag, "FAIL timeout: participant '%s' did not reach Running within %lld ms", waitFor.c_str(),
                static_cast<long long>(timeout.count()));
            return kExitTimeout;
        }
        log(kTag, "peer '%s' is running", waitFor.c_str());
    }

    // The node claims its address in its first step, so the receiver sees the claim.
    peer->host().post([&] {
        node = &bus.add_node({sourceAddress, name});
        node->on_address_change([&](const j1939::AddressChange& change) {
            // The address asked for: an arbitrary address capable NAME may end up elsewhere.
            claimed.store(change.state == j1939::AddressState::Claimed && change.address == sourceAddress);
        });
    });
    log(kTag, "claiming address 0x%02X with NAME 0x%016" PRIX64, sourceAddress, name.value());

    if (!wait_until([&] { return peer->failed() || (claimed.load() && sent.claims.load() > 0); }, timeout))
    {
        log(kTag, "FAIL timeout: address claim not completed (claimed=%d, claim frames sent=%u) %s", claimed.load() ? 1 : 0,
            sent.claims.load(), peer->error().c_str());
        return kExitTimeout;
    }
    if (peer->failed())
    {
        log(kTag, "FAIL: %s", peer->error().c_str());
        return kExitVerificationFailed;
    }
    log(kTag, "address 0x%02X claimed", sourceAddress);

    if (!wait_until([&] { return peer->failed() || sent.messages.load() >= count; }, timeout))
    {
        log(kTag, "FAIL timeout: only %u of %u PGN 0x%05X frames were sent %s", sent.messages.load(), count, pgn.value(),
            peer->error().c_str());
        return kExitTimeout;
    }

    if (peer->failed())
    {
        log(kTag, "FAIL: %s", peer->error().c_str());
        return kExitVerificationFailed;
    }
    log(kTag, "sent %u x PGN 0x%05X", count, pgn.value());

    if (!lingerFor.empty())
    {
        // Keep the participant connected until the peer has consumed the traffic.
        if (!peer->runtime().wait_for_peer(lingerFor, has_finished, timeout))
        {
            log(kTag, "FAIL timeout: participant '%s' did not finish within %lld ms", lingerFor.c_str(),
                static_cast<long long>(timeout.count()));
            return kExitTimeout;
        }
        log(kTag, "peer '%s' finished", lingerFor.c_str());
    }

    peer->stop("sender done");
    if (peer->failed())
    {
        log(kTag, "FAIL: %s", peer->error().c_str());
        return kExitVerificationFailed;
    }
    // After stop(): the transmit acknowledgements of the last frames have been counted.
    const auto stats = peer->silkit_port(network)->statistics();
    log(kTag, "port stats: sent=%" PRIu64 " received=%" PRIu64 " receive_overflows=%" PRIu64
        " send_failed=%" PRIu64 " send_queue_full=%" PRIu64 " tx_canceled=%" PRIu64 " tx_queue_full=%" PRIu64,
        stats.sent, stats.received, stats.receive_overflows, stats.send_failed, stats.send_queue_full,
        stats.tx_canceled, stats.tx_queue_full);
    if (stats.send_failed != 0 || stats.send_queue_full != 0 || stats.tx_canceled != 0 || stats.tx_queue_full != 0)
    {
        log(kTag, "FAIL verification: the port reported transmit errors");
        return kExitVerificationFailed;
    }
    log(kTag, "PASS");
    return kExitOk;
}

} // namespace

int main(int argc, char** argv)
{
    return cli::run_main(argc, argv, kTag, print_usage, run);
}
