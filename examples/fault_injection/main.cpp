// SPDX-License-Identifier: MIT
//
// Fault injection and observation. ECUs run on a Simulation with virtual buses; its test side
// (sim.local(), vehicle_sil::LocalBench) cuts an ECU off the bus, drops chosen frames, injects frames
// and records what the stack and the bus did. Each scene builds a Simulation, advances it in
// steps and checks what it saw. Time is virtual: nothing sleeps.
#include "vehicle_sil/j1939/monitor.hpp"
#include "vehicle_sil/j1939/periodic_sender.hpp"
#include "vehicle_sil/local/local_bench.hpp"
#include "vehicle_sil/simulation/simulation.hpp"
#include "vehicle_sil/testkit/faults.hpp"
#include "vehicle_sil/testkit/recorders.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>

using namespace std::chrono_literals;
namespace j1939 = vehicle_sil::j1939;
namespace can = vehicle_sil::can;
namespace faults = vehicle_sil::testkit::faults;
using vehicle_sil::SimTime;
using vehicle_sil::testkit::FrameLog;
using vehicle_sil::testkit::J1939Events;

namespace {
constexpr const char* kBus = "J1939_CAN";
constexpr j1939::Pgn kStatus{0xFF30}, kBlob{0xFF40}; // groups of this example
constexpr j1939::Pgn kTpData{0xEB00};                // the frames that carry a long message's packets

bool report(const char* scene, bool ok)
{
    std::printf("[%s] %s\n", ok ? "ok" : "FAILED", scene);
    return ok;
}

// The ECU "Engine" sends kStatus every 20 ms; "Display" watches it and calls it lost after 100 ms.
struct Link
{
    vehicle_sil::Simulation sim;
    j1939::Monitor* monitor = nullptr;
    Link()
    {
        auto& engine = sim.add_ecu("Engine", 10ms).j1939(kBus).add_node({.address = 0x10});
        engine.attach<j1939::PeriodicSender>(kStatus, 20ms, std::array<std::uint8_t, 2>{1, 2});
        monitor = &sim.add_ecu("Display", 5ms).j1939(kBus).add_node({.address = 0x80}).attach<j1939::Monitor>(kStatus, j1939::MonitorOptions{.timeout = 100ms});
    }
};

// 1. The engine drops off the bus for 300 ms: the display's Monitor reports Lost, then recovers.
bool link_outage()
{
    Link v;
    v.sim.run_for(500ms);
    const bool before = v.monitor->state() == j1939::MonitorState::Receiving;
    auto& engine = v.sim.local().endpoint("Engine");
    engine.add_outage(v.sim.now(), v.sim.now() + 300ms); // both directions
    v.sim.run_for(250ms);
    const bool during = v.monitor->state() == j1939::MonitorState::Lost;
    v.sim.run_for(450ms);
    return report("an outage makes the monitor report Lost, and it recovers",
                  before && during && v.monitor->state() == j1939::MonitorState::Receiving && v.monitor->losses() == 1 &&
                      engine.outage_lost() > 10);
}

// 2. A late ECU claims the first one's address with a NAME that wins: the first ends without one.
bool address_conflict()
{
    vehicle_sil::Simulation sim;
    auto& first = sim.add_ecu("First", 10ms).j1939(kBus).add_node(
        {.address = 0x20, .name = j1939::Name{}.set_identity_number(200).set_manufacturer_code(0x7F0)});
    auto& rival_ecu = sim.add_ecu("Rival", 10ms);
    auto& rival_net = rival_ecu.j1939(kBus);
    j1939::Node* rival = nullptr;
    rival_ecu.on_step([&](SimTime now) {
        if (rival == nullptr && now >= SimTime{} + 400ms) // joins late (nodes may be added from a step)
        {
            rival = &rival_net.add_node({.address = 0x20, .name = j1939::Name{}.set_identity_number(100).set_manufacturer_code(0x7F0)});
        }
    });
    J1939Events events(sim.local().network("First"));
    sim.run_for(1500ms);
    const auto* last = events.last(j1939::EventKind::AddressChanged);
    return report("the node with the lower NAME keeps the address, the other has none",
                  first.address_state() == j1939::AddressState::CannotClaim && rival != nullptr &&
                      rival->address_state() == j1939::AddressState::Claimed && last != nullptr &&
                      last->event.address.state == j1939::AddressState::CannotClaim);
}

// 3. A 20-byte broadcast loses its second packet: the receiver ends it as Lost and delivers
// nothing; the sender cannot know and reports Completed. The next broadcast arrives.
bool lost_packet()
{
    vehicle_sil::Simulation sim;
    auto& ecu = sim.add_ecu("Sender", 10ms);
    auto& sender = ecu.j1939(kBus).add_node({.address = 0x20});
    ecu.every(1s, [&](SimTime) { static_cast<void>(sender.send(kBlob, std::array<std::uint8_t, 20>{})); });
    sim.add_ecu("Receiver", 10ms).j1939(kBus).add_node({.address = 0x30});
    auto& bus = sim.local();
    bus.endpoint("Sender").set_fault_injector(faults::nth(2, can::FaultAction::drop(), faults::j1939_pgn(kTpData)));
    J1939Events sent(bus.network("Sender"));
    J1939Events received(bus.network("Receiver"));
    sim.run_for(2s); // broadcasts at 0 and 1 s
    const auto ends = received.of(j1939::EventKind::TransferEnded);
    const auto sender_completed = sent.count(j1939::EventKind::TransferEnded,
                                             [](const auto& r) { return r.event.result == j1939::TransferResult::Completed; });
    return report("a lost packet: the receiver sees Lost, the sender Completed",
                  ends.size() == 2 && ends[0].event.result == j1939::TransferResult::Lost &&
                      ends[0].event.reason == j1939::AbortReason::BadSequence && ends[1].event.result == j1939::TransferResult::Completed &&
                      received.count(j1939::EventKind::MessageReceived) == 1 && sender_completed == 2);
}

// 4. Injected frames reach the handler and the FrameLog; the J1939 stack ignores the 11-bit one.
bool injected_frames()
{
    vehicle_sil::Simulation sim;
    auto& node = sim.add_ecu("Receiver", 10ms).j1939(kBus).add_node({.address = 0x30});
    int received = 0;
    node.on_message(kStatus, [&](const j1939::Message& m) { received += m.source == 0x44 && m.data.size() == 3; });
    auto& bus = sim.local();
    FrameLog wire(bus.tap("Receiver"));
    can::Frame j1939_frame{.id = j1939::Identifier{6, kStatus, j1939::kGlobalAddress, 0x44}.encode(), .length = 3}; // from an address nobody has
    can::Frame standard{.id = 0x123, .format = can::IdFormat::Standard, .length = 1};
    bus.inject(kBus, SimTime{} + 50ms, j1939_frame);
    bus.inject(kBus, SimTime{} + 60ms, standard);
    bus.inject(kBus, SimTime{} + 100ms, j1939_frame);
    sim.run_for(200ms);
    return report("injected frames: two for the handler, three on the bus, one ignored by J1939",
                  received == 2 && wire.count(FrameLog::Direction::Received) == 3 &&
                      bus.network("Receiver").statistics().frames_ignored == 1);
}

} // namespace

int main()
{
    try
    {
        const bool results[] = {link_outage(), address_conflict(), lost_packet(), injected_frames()};
        bool ok = true;
        for (const bool r : results) ok = ok && r;
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
