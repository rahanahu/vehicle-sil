// SPDX-License-Identifier: MIT
//
// The J1939 stack when one ECU loses its connection to the bus for a while (outages of
// the virtual bus), and the port events it counts (bus state, error frames, inbox overflow,
// asynchronous transmit failures).
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::can::BusState;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::OutageDirection;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::TransferEnd;
using vehicle_sil::j1939::TransferResult;

namespace {

constexpr Address kA = 0x10;
constexpr Address kB = 0x20;
constexpr std::uint32_t kPgn = 0xEF00;

NetworkOptions quiet()
{
    NetworkOptions options;
    options.warning = [](std::string_view) {};
    return options;
}

/// ECUs on one virtual bus, each a Network with its own host stepped every 10 ms.
struct Ecus
{
    struct Ecu
    {
        Ecu(vehicle_sil::can::VirtualCanBus& bus, const NetworkOptions& options) : port(bus.attach()), network(options)
        {
            host.add(network, port);
            host.set_application([this](SimTime now) {
                if (app)
                {
                    app(now);
                }
            });
        }
        vehicle_sil::can::VirtualCanBus::Endpoint& port;
        Network network;
        vehicle_sil::can::Host host;
        std::function<void(SimTime)> app;
        std::vector<Seen> seen;
        std::vector<TransferEnd> ends;
    };

    explicit Ecus(std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            ecus.push_back(std::make_unique<Ecu>(bus, quiet()));
            driver.add(ecus.back()->host, 10ms);
        }
    }

    Node& add(std::size_t ecu, const vehicle_sil::j1939::NodeOptions& options)
    {
        Ecu& e = *ecus[ecu];
        Node& node = e.network.add_node(options);
        node.on_any_message([&e](const Message& m) { e.seen.push_back(seen_of(m)); });
        node.on_transfer_end([&e](const TransferEnd& end) { e.ends.push_back(end); });
        return node;
    }

    Ecu& operator[](std::size_t i) { return *ecus[i]; }

    vehicle_sil::can::VirtualCanBus bus;
    std::vector<std::unique_ptr<Ecu>> ecus;
    vehicle_sil::can::StepDriver driver;
};

/// `from` sends `message` to `to` in the step at each of `times`.
void send_at(Ecus::Ecu& ecu, Node& from, const Bytes& message, Address to, std::vector<SimTime> times)
{
    ecu.app = [&from, message, to, times](SimTime now) {
        for (const SimTime t : times)
        {
            if (now == t)
            {
                CHECK(from.send(Pgn{kPgn}, message, to) == SendStatus::Ok);
            }
        }
    };
}

std::size_t messages_of(const std::vector<Seen>& seen, std::uint32_t pgn, std::size_t length)
{
    std::size_t n = 0;
    for (const auto& s : seen)
    {
        n += (s.pgn == pgn && s.data.size() == length) ? 1u : 0u;
    }
    return n;
}

} // namespace

// The receiver of an RTS/CTS transfer stops hearing the bus after its first clear-to-send
// (10 ms): it misses the packets (sent at 20 ms), and its T2 runs out (10 ms + 1250 ms, fires in
// the step at 1270 ms). Its Connection Abort reaches the originator, whose T3 would run out a
// step later. After the outage the same transfer completes.
TEST_CASE(transfer_to_an_ecu_that_stops_receiving_times_out_and_recovers)
{
    Ecus ecus(2);
    Node& a = ecus.add(0, {kA});
    ecus.add(1, {kB});
    ecus[1].port.add_outage(at(15ms), at(1s), OutageDirection::Receive);
    const Bytes message = payload(1785);
    send_at(ecus[0], a, message, kB, {at(0ms), at(2s)});
    ecus.driver.run_until(at(1500ms));

    CHECK_EQ(ecus[0].ends.size(), 1u);
    CHECK(ecus[0].ends[0].result == TransferResult::AbortedByPeer);
    CHECK(ecus[0].ends[0].reason == AbortReason::Timeout);
    CHECK(ecus[0].ends[0].time == at(1270ms));
    CHECK_EQ(messages_of(ecus[1].seen, kPgn, message.size()), 0u);
    CHECK_EQ(ecus[1].network.statistics().transport_timeouts, 1u);
    CHECK_EQ(ecus[1].network.statistics().transport_aborts_sent, 1u);
    CHECK(ecus[1].port.outage_dropped() > 0u);

    ecus.driver.run_until(at(5s));
    CHECK_EQ(ecus[0].ends.size(), 2u);
    CHECK(ecus[0].ends[1].result == TransferResult::Completed);
    CHECK_EQ(messages_of(ecus[1].seen, kPgn, message.size()), 1u);
}

// Disconnected both ways: the receiver's Connection Abort is lost as well, so the originator's
// own T3 (from its packets at 20 ms, deadline 1270 ms) runs out in the step at 1280 ms and it
// aborts the transfer.
TEST_CASE(transfer_to_a_disconnected_ecu_times_out_on_both_sides)
{
    Ecus ecus(2);
    Node& a = ecus.add(0, {kA});
    ecus.add(1, {kB});
    ecus[1].port.add_outage(at(15ms), at(2s), OutageDirection::Both);
    const Bytes message = payload(1785);
    send_at(ecus[0], a, message, kB, {at(0ms), at(2500ms)});
    ecus.driver.run_until(at(2s));

    CHECK_EQ(ecus[0].ends.size(), 1u);
    CHECK(ecus[0].ends[0].result == TransferResult::Aborted);
    CHECK(ecus[0].ends[0].reason == AbortReason::Timeout);
    CHECK(ecus[0].ends[0].time == at(1270ms)); // the deadline (the cause), handled at 1280 ms
    CHECK_EQ(ecus[0].network.statistics().transport_timeouts, 1u);
    CHECK_EQ(ecus[1].network.statistics().transport_timeouts, 1u);
    // The receiver's abort was sent during the outage: accepted, then a transmit failure.
    CHECK(ecus[1].port.outage_lost() > 0u);
    CHECK_EQ(ecus[1].network.statistics().transmit_failures, ecus[1].port.outage_lost());

    ecus.driver.run_until(at(6s));
    CHECK_EQ(ecus[0].ends.size(), 2u);
    CHECK(ecus[0].ends[1].result == TransferResult::Completed);
    CHECK_EQ(messages_of(ecus[1].seen, kPgn, message.size()), 1u);
}

// A broadcast (BAM, 15 packets every 50 ms) while one of two receivers does not hear the bus
// for 200 ms: it drops the broadcast when the packets after the gap arrive out of sequence; the
// other receiver gets it.
TEST_CASE(broadcast_reaches_the_connected_ecus_only)
{
    Ecus ecus(3);
    Node& a = ecus.add(0, {kA});
    ecus.add(1, {kB});
    ecus.add(2, {0x30});
    ecus[1].port.add_outage(at(200ms), at(400ms), OutageDirection::Receive);
    const Bytes message = payload(100);
    send_at(ecus[0], a, message, kGlobal, {at(0ms)});
    ecus.driver.run_until(at(3s));

    CHECK_EQ(ecus[0].ends.size(), 1u);
    CHECK(ecus[0].ends[0].result == TransferResult::Completed);
    CHECK_EQ(messages_of(ecus[2].seen, kPgn, message.size()), 1u);
    CHECK_EQ(messages_of(ecus[1].seen, kPgn, message.size()), 0u);
    CHECK_EQ(ecus[1].network.statistics().transport_bam_dropped, 1u);
}

// Two nodes with the same address, one disconnected while both claim it: neither hears the
// other's claim, and both believe they own the address. When the disconnected one comes back
// and the other sends from the address, it sees the violation and claims again; the lower NAME
// wins (it claims again in turn) and the other cannot claim.
TEST_CASE(address_conflict_hidden_by_a_disconnection_is_resolved_after_it)
{
    constexpr Address kShared = 0x80;
    Ecus ecus(2);
    Node& winner = ecus.add(0, {kShared, Name{0x1000}});
    Node& loser = ecus.add(1, {kShared, Name{0x2000}});
    ecus[1].port.add_outage(at(0ms), at(1s), OutageDirection::Both);
    send_at(ecus[0], winner, Bytes{1, 2, 3}, kGlobal, {at(1500ms)});

    ecus.driver.run_until(at(1s));
    CHECK(winner.address_state() == AddressState::Claimed);
    CHECK(loser.address_state() == AddressState::Claimed); // it never heard the other claim
    CHECK_EQ(loser.address(), kShared);

    ecus.driver.run_until(at(3s));
    CHECK(winner.address_state() == AddressState::Claimed);
    CHECK_EQ(winner.address(), kShared);
    CHECK(loser.address_state() == AddressState::CannotClaim);
    CHECK_EQ(ecus[1].network.statistics().address_violations, 1u);
}

// The events of the port are counted and the latest bus state is kept; the protocol goes on.
TEST_CASE(network_counts_port_events_and_keeps_the_bus_state)
{
    Bench bench(quiet());
    Node& node = bench.add_node(kA);
    PortEvent passive;
    passive.kind = PortEvent::Kind::BusStateChanged;
    passive.bus_state = BusState::ErrorPassive;
    passive.time = at(1ms);
    PortEvent busOff = passive;
    busOff.bus_state = BusState::BusOff;
    busOff.time = at(2ms);
    PortEvent errorFrame;
    errorFrame.kind = PortEvent::Kind::ErrorFrame;
    errorFrame.time = at(3ms);
    CHECK(bench.port.inject_event(passive));
    CHECK(bench.port.inject_event(busOff));
    CHECK(bench.port.inject_event(errorFrame));
    CHECK(bench.network.bus_state() == std::nullopt);
    bench.steps(2);
    CHECK(bench.network.bus_state() == std::optional<BusState>(BusState::BusOff));
    const auto& stats = bench.network.statistics();
    CHECK_EQ(stats.bus_state_changes, 2u);
    CHECK_EQ(stats.error_frames, 1u);

    // An asynchronous transmit failure is counted; the frame is not sent again.
    bench.port.set_fault_injector([](const TimedFrame&) { return FaultAction::transmit_failed(); });
    bench.app = [&](SimTime now) {
        if (now == at(20ms))
        {
            CHECK(node.send(Pgn{0xFF00}, Bytes{1}) == SendStatus::Ok);
        }
    };
    bench.steps(2);
    CHECK_EQ(stats.frames_sent, 1u);
    CHECK_EQ(stats.transmit_failures, 1u);
    CHECK(bench.take_sent().empty());
}

// A port whose inbox overflows reports how many frames it lost; the network counts them apart
// from its own receive limit.
TEST_CASE(network_counts_frames_its_port_dropped)
{
    vehicle_sil::can::VirtualCanBusOptions busOptions;
    busOptions.inbox_capacity = 4;
    vehicle_sil::can::VirtualCanBus bus(busOptions);
    auto& tester = bus.attach();
    auto& port = bus.attach();
    Network network(quiet());
    network.add_node({kA});
    vehicle_sil::can::Host host;
    host.add(network, port);
    for (std::uint8_t i = 0; i < 10; ++i)
    {
        tester.send(TimedFrame{at(1ms), to_can(frame(6, 0xFF00, 0x50, kGlobal, {i}))});
    }
    host.step(at(10ms));
    const auto& stats = network.statistics();
    CHECK_EQ(stats.frames_received, 4u);
    CHECK_EQ(stats.port_overflow_events, 1u);
    CHECK_EQ(stats.port_receive_overflows, 6u);
    CHECK_EQ(stats.receive_overflows, 0u);
}
