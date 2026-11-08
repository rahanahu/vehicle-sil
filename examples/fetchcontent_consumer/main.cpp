// SPDX-License-Identifier: MIT
//
// Smoke test for a FetchContent consumer: starts an in-process SIL Kit registry and runs one
// vehicle_sil::Peer on virtual time with two J1939 nodes on one network. The engine claims its
// address and broadcasts a typed message (consumer_messages.yaml); the other node receives it.
#include "consumer_messages.hpp"

#include <vehicle_sil/j1939/codec_node.hpp>
#include <vehicle_sil/j1939/monitor.hpp>
#include <vehicle_sil/j1939/network.hpp>
#include <vehicle_sil/peer/peer.hpp>

#include <silkit/vendor/CreateSilKitRegistry.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
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

        using consumer::EngineTemperature;
        vehicle_sil::PeerOptions options;
        options.participant = "FetchContentConsumer";
        options.registry_uri = uri;
        options.virtual_time = vehicle_sil::VirtualTimeOptions{10ms, std::nullopt, vehicle_sil::SimTime{} + 1s};
        vehicle_sil::Peer peer(options);
        auto& can = peer.j1939("J1939_CAN1");
        auto& engine = can.add_node(
            {.address = 0x81, .name = j1939::Name{}.set_identity_number(1).set_manufacturer_code(0x7F0).set_industry_group(2)});
        auto& display = can.add_node({.address = 0x28});
        // Every 100 ms once claimed.
        vehicle_sil::j1939::codec::attach_sender<EngineTemperature>(engine, 100ms,
                                                                    [](EngineTemperature& m) { m.coolant.set(85.0); });
        auto& seen = display.attach<j1939::Monitor>(EngineTemperature::kPgn, j1939::MonitorOptions{.source = 0x81});
        const auto state = peer.run(30s);

        const auto latest = EngineTemperature::decode(seen.data());
        const bool ok = state.has_value() && !peer.failed() && engine.address_state() == j1939::AddressState::Claimed &&
                        engine.address() == 0x81 && seen.count() > 0 && latest &&
                        latest->coolant.value() == std::optional<double>(85.0);
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
