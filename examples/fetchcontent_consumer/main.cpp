// SPDX-License-Identifier: MIT
//
// Smoke test for a FetchContent consumer: one vehicle_sil::Simulation on SIL Kit (a registry of
// this process, virtual time) with one ECU that has two J1939 nodes on one network. The engine
// claims its address and broadcasts a typed message (consumer_messages.yaml); the other node
// receives it.
#include "consumer_messages.hpp"

#include <vehicle_sil/ecu/ecu.hpp>
#include <vehicle_sil/j1939/codec_node.hpp>
#include <vehicle_sil/j1939/monitor.hpp>
#include <vehicle_sil/simulation/simulation.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>

namespace j1939 = vehicle_sil::j1939;
using consumer::EngineTemperature;
using namespace std::chrono_literals;

/// An ECU application: it only knows the vehicle_sil::Ecu it is given.
class Consumer
{
public:
    explicit Consumer(vehicle_sil::Ecu& ecu)
    {
        auto& can = ecu.j1939("J1939_CAN1");
        engine_ = &can.add_node(
            {.address = 0x81, .name = j1939::Name{}.set_identity_number(1).set_manufacturer_code(0x7F0).set_industry_group(2)});
        auto& display = can.add_node({.address = 0x28});
        // Every 100 ms once claimed.
        j1939::codec::attach_sender<EngineTemperature>(*engine_, 100ms, [](EngineTemperature& m) { m.coolant.set(85.0); });
        seen_ = &display.attach<j1939::Monitor>(EngineTemperature::kPgn, j1939::MonitorOptions{.source = 0x81});
    }

    [[nodiscard]] bool ok() const
    {
        const auto latest = EngineTemperature::decode(seen_->data());
        return engine_->address_state() == j1939::AddressState::Claimed && engine_->address() == 0x81 &&
               seen_->count() > 0 && latest && latest->coolant.value() == std::optional<double>(85.0);
    }
    [[nodiscard]] int address() const { return engine_->address(); }
    [[nodiscard]] int received() const { return static_cast<int>(seen_->count()); }

private:
    j1939::Node* engine_ = nullptr;
    j1939::Monitor* seen_ = nullptr;
};

int main()
{
    try
    {
        vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{}); // no registry to start
        auto& consumer = sim.add<Consumer>("FetchContentConsumer", 10ms);
        sim.run_for(1s); // returns after the participant stopped; throws SimulationError on failure

        std::printf("address claim and message %s (address 0x%02X, messages received %d)\n",
                    consumer.ok() ? "ok" : "FAILED", consumer.address(), consumer.received());
        return consumer.ok() ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
