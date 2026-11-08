// SPDX-License-Identifier: MIT
//
// Smoke test for a FetchContent consumer: starts an in-process SIL Kit registry and runs one
// vehicle_sil::Peer on virtual time with two J1939 nodes on one network. The engine claims its
// address and broadcasts a message; the other node receives it.
#include <vehicle_sil/j1939/monitor.hpp>
#include <vehicle_sil/j1939/network.hpp>
#include <vehicle_sil/j1939/periodic_sender.hpp>
#include <vehicle_sil/peer/peer.hpp>

#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>

int main()
{
    using namespace std::chrono_literals;
    namespace j1939 = vehicle_sil::j1939;
    try
    {
        auto registry = SilKit::Vendor::Vector::CreateSilKitRegistry(SilKit::Config::ParticipantConfigurationFromString("{}"));
        const std::string uri = registry->StartListening("silkit://127.0.0.1:0");
        std::printf("registry listening on %s\n", uri.c_str());

        constexpr j1939::Pgn kPgn{0xFF10};
        vehicle_sil::PeerOptions options;
        options.participant = "FetchContentConsumer";
        options.registry_uri = uri;
        options.virtual_time = vehicle_sil::VirtualTimeOptions{10ms, std::nullopt, vehicle_sil::SimTime{} + 1s};
        vehicle_sil::Peer peer(options);
        auto& can = peer.j1939("J1939_CAN1");
        auto& engine = can.add_node(
            {.address = 0x81, .name = j1939::Name{}.set_identity_number(1).set_manufacturer_code(0x7F0).set_industry_group(2)});
        auto& display = can.add_node({.address = 0x28});
        const std::uint8_t data[8]{1, 2, 3, 4, 5, 6, 7, 8};
        engine.attach<j1939::PeriodicSender>(kPgn, 100ms, std::span<const std::uint8_t>(data)); // once claimed
        auto& seen = display.attach<j1939::Monitor>(kPgn, j1939::MonitorOptions{.source = 0x81});
        const auto state = peer.run(30s);

        const bool ok = state.has_value() && !peer.failed() && engine.address_state() == j1939::AddressState::Claimed &&
                        engine.address() == 0x81 && seen.count() > 0;
        std::printf("address claim and message %s (address 0x%02X, messages received %d)\n", ok ? "ok" : "FAILED",
                    engine.address(), static_cast<int>(seen.count()));
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
