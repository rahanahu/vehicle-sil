// SPDX-License-Identifier: MIT
//
// The layers under Simulation, assembled by hand. Normally use Simulation: this is for people who
// build a new backend or a test base. The pieces, from the bus up:
//   VirtualCanBus -> Endpoint (a port) -> Host (one participant's step) <- StepDriver (time)
//   Host steps a j1939::Network and a can::RawChannel, each bound to its own endpoint.
// Two stations with different step sizes share one bus. A station's J1939 node claims an address;
// the engine sends a status message; the display sends 11-bit frames beside its J1939 node.
#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/core/periodic.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/observer.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>

using namespace std::chrono_literals;
namespace can = vehicle_sil::can;
namespace j1939 = vehicle_sil::j1939;
using vehicle_sil::SimTime;

namespace {

constexpr j1939::Pgn kStatus{0xFF60}; // a group of this example

constexpr long long ms(vehicle_sil::SimDuration d) { return d / 1ms; }

// Receives the network's events as they happen (read only).
class Events final : public j1939::Observer
{
public:
    void on_event(const j1939::Event& e) override
    {
        if (e.kind == j1939::EventKind::AddressChanged && e.address.state == j1939::AddressState::Claimed)
        {
            claimed = e.time;
        }
        received += e.kind == j1939::EventKind::MessageReceived && e.pgn == kStatus;
    }
    SimTime claimed{};
    int received = 0;
};

// One participant: a J1939 network and a raw channel, each on an endpoint of its own, in one host.
// Members are destroyed in reverse: the host first, then the network and the raw channel. The
// endpoints belong to the bus, which must outlive them all.
struct Station
{
    Station(can::VirtualCanBus& bus, j1939::Address address) : net_endpoint(bus.attach()), raw_endpoint(bus.attach())
    {
        node = &net.add_node({.address = address, .name = j1939::Name{}.set_identity_number(address)});
        host.add(net, net_endpoint); // the order of add() is the order of receive, process and send in a step
        host.add(raw, raw_endpoint);
        net.set_observer(&events);
    }
    can::VirtualCanBus::Endpoint &net_endpoint, &raw_endpoint;
    Events events;
    j1939::Node* node = nullptr;
    can::RawChannel raw;
    j1939::Network net;
    can::Host host;
};

} // namespace

int main()
{
    try
    {
        can::VirtualCanBus bus;                 // outlives everything that uses its endpoints
        Station engine(bus, 0x00);
        Station display(bus, 0x80);
        can::StepDriver driver;
        driver.add(engine.host, 10ms);          // the engine steps every 10 ms,
        driver.add(display.host, 4ms);          // the display every 4 ms, on one timeline

        // The application stage of a host runs after the network handled what it received.
        engine.host.set_application(vehicle_sil::periodic(50ms, [&](SimTime) {
            const std::array<std::uint8_t, 2> status{0x12, 0x34};
            if (engine.node->can_send_now())
            {
                static_cast<void>(engine.node->send(kStatus, status));
            }
        }));
        int raw_seen = 0;
        engine.raw.on_frame([&](const can::TimedFrame& f) { raw_seen += f.frame.format == can::IdFormat::Standard; });
        display.host.set_application(vehicle_sil::periodic(100ms, [&](SimTime) {
            can::Frame frame;
            frame.id = 0x123;
            frame.format = can::IdFormat::Standard;
            frame.length = 1;
            display.raw.send(frame); // only from a step
        }));

        // A timer for 123 ms: it fires in the first step at or after, and knows its own deadline.
        long long fired_at_step = -1, fired_for = -1;
        j1939::Timer timer(engine.net, j1939::TimerKind::Scheduled, [&] {
            fired_at_step = ms(*engine.net.now() - SimTime{});
            fired_for = ms(engine.net.event_time() - SimTime{});
        });
        timer.start_at(SimTime{} + 123ms);

        driver.run_until(SimTime{} + 1s);       // deterministic: no clock is read

        const auto claimed = [](const Station& s) { return ms(s.events.claimed - SimTime{}); };
        std::printf("address claimed: engine at %lld ms (step 10), display at %lld ms (step 4)\n", claimed(engine), claimed(display));
        std::printf("timer for 123 ms fired in the step at %lld ms, for time %lld ms\n", fired_at_step, fired_for);
        std::printf("display got %d status messages; the engine saw %d 11-bit frames, the display's J1939 network ignored %llu\n",
                    display.events.received, raw_seen,
                    static_cast<unsigned long long>(display.net.statistics().frames_ignored));
        const bool ok = claimed(engine) >= 250 && claimed(engine) < 260 && claimed(display) >= 250 && claimed(display) < 254 &&
                        fired_at_step == 130 && fired_for == 123 && display.events.received >= 15 && raw_seen >= 9 &&
                        display.net.statistics().frames_ignored >= 9;
        std::printf("%s\n", ok ? "[ok] the times are what the step sizes give" : "[FAILED] unexpected result");
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
