// SPDX-License-Identifier: MIT
//
// The transport protocol of the J1939 stack under faults and address changes: frames
// lost, repeated, delayed or reordered on the virtual bus, a port that is full or fails, nodes
// that lose or change their address during a transfer, several ECUs and sessions at once, and
// the same results whatever the step sizes.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::unit::throws;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::AddressChange;
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

[[nodiscard]] bool is_dt(const TimedFrame& f, int sequence = -1)
{
    return ((f.frame.id >> 8) & 0x3FF00u) == kPgnTpDt && (sequence < 0 || f.frame.data[0] == sequence);
}

[[nodiscard]] bool is_cm(const TimedFrame& f, std::uint8_t code)
{
    return ((f.frame.id >> 8) & 0x3FF00u) == kPgnTpCm && f.frame.data[0] == code;
}

struct Inbox
{
    std::vector<Seen> seen;
    void attach(Node& node)
    {
        node.on_any_message([this](const Message& m) { seen.push_back(seen_of(m)); });
    }
};

/// Networks A and B (each with one node, on its own host and step size) on one virtual bus, and
/// a tap endpoint that sees every frame.
struct Ecus
{
    explicit Ecus(std::vector<SimDuration> steps, NetworkOptions options = quiet())
    {
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            auto e = std::make_unique<Ecu>(bus, options);
            e->host.set_application([this, i](SimTime now) {
                if (apps.size() > i && apps[i])
                {
                    apps[i](now);
                }
            });
            driver.add(e->host, steps[i]);
            ecus.push_back(std::move(e));
        }
        apps.resize(steps.size());
    }

    struct Ecu
    {
        Ecu(vehicle_sil::can::VirtualCanBus& bus, const NetworkOptions& options) : port(bus.attach()), network(options)
        {
            host.add(network, port);
        }
        vehicle_sil::can::VirtualCanBus::Endpoint& port;
        Network network;
        vehicle_sil::can::Host host;
        Inbox inbox;
        Ends ends;
    };

    Node& add(std::size_t ecu, const vehicle_sil::j1939::NodeOptions& options)
    {
        Node& node = ecus[ecu]->network.add_node(options);
        ecus[ecu]->inbox.attach(node);
        ecus[ecu]->ends.attach(node);
        return node;
    }

    void run_until(SimTime end)
    {
        driver.run_until(end);
        tap.release_before(end, [this](const TimedFrame& f) { trace.push_back(f); });
    }

    [[nodiscard]] std::size_t count(bool (*match)(const TimedFrame&)) const
    {
        std::size_t n = 0;
        for (const auto& f : trace)
        {
            n += match(f) ? 1u : 0u;
        }
        return n;
    }

    vehicle_sil::can::VirtualCanBus bus;
    vehicle_sil::can::VirtualCanBus::Endpoint& tap = bus.attach();
    std::vector<std::unique_ptr<Ecu>> ecus;
    std::vector<std::function<void(SimTime)>> apps;
    vehicle_sil::can::StepDriver driver;
    std::vector<TimedFrame> trace;
};

/// A sends `message` to B (or to all with kGlobal) at 0 ms.
void send_at_start(Ecus& ecus, Node& from, const Bytes& message, Address to = kB)
{
    ecus.apps[0] = [&from, message, to](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(from.send(Pgn{kPgn}, message, to) == SendStatus::Ok);
        }
    };
}

struct Outcome
{
    std::vector<Seen> received;
    std::vector<TransferResult> results;
    std::vector<AbortReason> reasons;
    vehicle_sil::j1939::NetworkStatistics a;
    vehicle_sil::j1939::NetworkStatistics b;
};

/// One transfer of `size` bytes from A to B with `fault` on A's port and `faultB` on B's port.
Outcome transfer(std::vector<SimDuration> steps, std::size_t size, vehicle_sil::can::FaultInjector fault,
                 vehicle_sil::can::FaultInjector faultB = {}, SimTime end = at(5s))
{
    Ecus ecus(std::move(steps));
    Node& a = ecus.add(0, {kA});
    ecus.add(1, {kB});
    ecus.ecus[0]->port.set_fault_injector(std::move(fault));
    ecus.ecus[1]->port.set_fault_injector(std::move(faultB));
    send_at_start(ecus, a, payload(size));
    ecus.run_until(end);
    Outcome out;
    for (const auto& s : ecus.ecus[1]->inbox.seen)
    {
        Seen copy = s;
        copy.time = SimTime{}; // the times depend on the steps
        out.received.push_back(copy);
    }
    for (const TransferEnd& e : ecus.ecus[0]->ends.list)
    {
        out.results.push_back(e.result);
        out.reasons.push_back(e.reason);
    }
    out.a = ecus.ecus[0]->network.statistics();
    out.b = ecus.ecus[1]->network.statistics();
    return out;
}

const std::vector<std::vector<SimDuration>> kStepCombinations{{10ms, 10ms}, {1ms, 10ms}, {10ms, 1ms}, {3ms, 7ms}, {50ms, 20ms}};

/// Drops packet `sequence` the first `times` times it is sent.
vehicle_sil::can::FaultInjector drop_packet(int sequence, int times = 1)
{
    auto left = std::make_shared<int>(times);
    return [=](const TimedFrame& f) {
        if (is_dt(f, sequence) && *left > 0)
        {
            --*left;
            return FaultAction::drop();
        }
        return FaultAction::deliver();
    };
}

} // namespace

// --- the fault injection of the virtual bus ------------------------------------------------

TEST_CASE(virtual_bus_fault_injection_drops_duplicates_delays_and_refuses)
{
    vehicle_sil::can::VirtualCanBus bus;
    auto& sender = bus.attach();
    auto& receiver = bus.attach();
    int n = 0;
    sender.set_fault_injector([&](const TimedFrame&) {
        switch (n++)
        {
        case 0: return FaultAction::drop();
        case 1: return FaultAction::duplicate();
        case 2: return FaultAction::delay_by(5ms);
        case 3: return FaultAction::queue_full();
        case 4: return FaultAction::failed();
        default: return FaultAction::deliver();
        }
    });
    vehicle_sil::can::Frame f;
    f.length = 1;
    std::vector<vehicle_sil::can::SendResult> results;
    for (std::uint8_t i = 0; i < 6; ++i)
    {
        f.data[0] = i;
        results.push_back(sender.send(TimedFrame{at(1ms), f}));
    }
    using R = vehicle_sil::can::SendResult;
    CHECK(results == (std::vector<R>{R::Accepted, R::Accepted, R::Accepted, R::QueueFull, R::Failed, R::Accepted}));
    std::vector<std::pair<SimTime, int>> got;
    receiver.release_before(at(10ms), [&](const TimedFrame& t) { got.emplace_back(t.time, t.frame.data[0]); });
    CHECK(got == (std::vector<std::pair<SimTime, int>>{{at(1ms), 1}, {at(1ms), 1}, {at(1ms), 5}, {at(6ms), 2}}));
    sender.set_fault_injector([](const TimedFrame&) { return FaultAction::delay_by(SimDuration::zero()); });
    CHECK(throws<std::invalid_argument>([&] { sender.send(TimedFrame{at(20ms), f}); }));
}

// --- faults during a transfer (the result does not depend on the step sizes) ---------------

TEST_CASE(transport_recovers_a_lost_packet_whatever_the_steps)
{
    for (const auto& steps : kStepCombinations)
    {
        const Outcome out = transfer(steps, 100, drop_packet(3));
        CHECK_EQ(out.received.size(), 1u);
        CHECK(out.received[0].data == payload(100));
        CHECK(out.results == (std::vector<TransferResult>{TransferResult::Completed}));
        CHECK_EQ(out.b.transport_retransmit_requests, 1u);
    }
}

TEST_CASE(transport_recovers_a_lost_last_packet_of_a_window_only_by_timeout)
{
    // The receiver cannot tell the window ended: T1 aborts (reason 3).
    for (const auto& steps : kStepCombinations)
    {
        const Outcome out = transfer(steps, 200, drop_packet(16)); // 29 packets, windows of 16
        CHECK(out.received.empty());
        CHECK(out.results == (std::vector<TransferResult>{TransferResult::AbortedByPeer}));
        CHECK(out.reasons == (std::vector<AbortReason>{AbortReason::Timeout}));
    }
}

TEST_CASE(transport_aborts_when_a_packet_stays_lost)
{
    for (const auto& steps : kStepCombinations)
    {
        const Outcome out = transfer(steps, 100, drop_packet(5, 3));
        CHECK(out.received.empty());
        CHECK(out.reasons == (std::vector<AbortReason>{AbortReason::RetransmitLimit}));
        CHECK_EQ(out.b.transport_retransmit_requests, 2u);
    }
}

TEST_CASE(transport_ignores_repeated_packets)
{
    for (const auto& steps : kStepCombinations)
    {
        const Outcome out = transfer(steps, 100, [](const TimedFrame& f) {
            return is_dt(f) ? FaultAction::duplicate() : FaultAction::deliver();
        });
        CHECK_EQ(out.received.size(), 1u);
        CHECK(out.results == (std::vector<TransferResult>{TransferResult::Completed}));
        CHECK_EQ(out.b.transport_retransmit_requests, 0u);
    }
}

TEST_CASE(transport_asks_again_for_reordered_packets)
{
    // Packet 2 is delayed behind the rest of the window.
    for (const auto& steps : kStepCombinations)
    {
        auto once = std::make_shared<bool>(true);
        const Outcome out = transfer(steps, 100, [once](const TimedFrame& f) {
            if (is_dt(f, 2) && *once)
            {
                *once = false;
                return FaultAction::delay_by(1ns);
            }
            return FaultAction::deliver();
        });
        CHECK_EQ(out.received.size(), 1u);
        CHECK(out.received[0].data == payload(100));
        CHECK_EQ(out.b.transport_retransmit_requests, 1u);
    }
}

TEST_CASE(transport_repeated_cts_counts_as_a_cts_while_sending)
{
    for (const auto& steps : kStepCombinations)
    {
        const Outcome out = transfer(steps, 100, {}, [](const TimedFrame& f) {
            return is_cm(f, 17) ? FaultAction::duplicate() : FaultAction::deliver();
        });
        CHECK(out.received.empty());
        CHECK(out.reasons == (std::vector<AbortReason>{AbortReason::CtsWhileSending}));
    }
}

TEST_CASE(transport_late_acknowledgement_times_out_after_delivery)
{
    // The end-of-message acknowledgement arrives after T3: the receiver has the message, the
    // sender reports a timeout (and aborts).
    for (const auto& steps : kStepCombinations)
    {
        const Outcome out = transfer(steps, 20, {}, [](const TimedFrame& f) {
            return is_cm(f, 19) ? FaultAction::delay_by(1300ms) : FaultAction::deliver();
        });
        CHECK_EQ(out.received.size(), 1u);
        CHECK(out.reasons == (std::vector<AbortReason>{AbortReason::Timeout}));
        CHECK(out.results == (std::vector<TransferResult>{TransferResult::Aborted}));
    }
}

TEST_CASE(transport_full_port_delays_the_packets_but_keeps_them_whole)
{
    // The sender's port refuses every other frame: the packets leave one step later each, in
    // order, and the message arrives whole.
    for (const auto& steps : kStepCombinations)
    {
        auto n = std::make_shared<int>(0);
        const Outcome out = transfer(steps, 300, [n](const TimedFrame&) {
            return (*n)++ % 2 == 1 ? FaultAction::queue_full() : FaultAction::deliver();
        });
        CHECK_EQ(out.received.size(), 1u);
        CHECK(out.received[0].data == payload(300));
        CHECK(out.results == (std::vector<TransferResult>{TransferResult::Completed}));
        CHECK(out.a.send_queue_full > 0);
        CHECK_EQ(out.b.transport_retransmit_requests, 0u);
    }
}

TEST_CASE(transport_receiver_keeps_its_cts_and_eoma_while_its_port_is_full)
{
    for (const auto& steps : kStepCombinations)
    {
        auto n = std::make_shared<int>(0);
        const Outcome out = transfer(steps, 40, {}, [n](const TimedFrame&) {
            return (*n)++ < 3 ? FaultAction::queue_full() : FaultAction::deliver();
        });
        CHECK_EQ(out.received.size(), 1u);
        CHECK(out.results == (std::vector<TransferResult>{TransferResult::Completed}));
        CHECK_EQ(out.b.send_queue_full, 3u);
    }
}

TEST_CASE(transport_full_port_for_too_long_times_out)
{
    // The sender's port refuses the packets for 2 s: the receiver gives up after T2.
    const Outcome out = transfer({10ms, 10ms}, 40, [](const TimedFrame& f) {
        return is_dt(f) && f.time < at(2s) ? FaultAction::queue_full() : FaultAction::deliver();
    });
    CHECK(out.received.empty());
    CHECK(out.results == (std::vector<TransferResult>{TransferResult::AbortedByPeer}));
    CHECK(out.reasons == (std::vector<AbortReason>{AbortReason::Timeout}));
}

TEST_CASE(transport_failed_packet_is_dropped_and_asked_for_again)
{
    for (const auto& steps : kStepCombinations)
    {
        auto once = std::make_shared<bool>(true);
        const Outcome out = transfer(steps, 100, [once](const TimedFrame& f) {
            if (is_dt(f, 4) && *once)
            {
                *once = false;
                return FaultAction::failed();
            }
            return FaultAction::deliver();
        });
        CHECK_EQ(out.received.size(), 1u);
        CHECK_EQ(out.a.send_failures, 1u);
        CHECK_EQ(out.b.transport_retransmit_requests, 1u);
    }
}

TEST_CASE(transport_failed_rts_ends_in_a_timeout)
{
    const Outcome out = transfer({10ms, 10ms}, 40, [](const TimedFrame& f) {
        return is_cm(f, 16) ? FaultAction::failed() : FaultAction::deliver();
    });
    CHECK(out.received.empty());
    CHECK(out.reasons == (std::vector<AbortReason>{AbortReason::Timeout}));
    CHECK_EQ(out.a.send_failures, 1u);
    CHECK_EQ(out.a.transport_aborts_sent, 1u);
}

TEST_CASE(transport_lost_broadcast_packet_loses_the_message)
{
    for (const auto& steps : kStepCombinations)
    {
        Ecus ecus(steps);
        Node& a = ecus.add(0, {kA});
        ecus.add(1, {kB});
        ecus.ecus[0]->port.set_fault_injector(drop_packet(2));
        send_at_start(ecus, a, payload(40), kGlobal);
        ecus.run_until(at(2s));
        CHECK(ecus.ecus[1]->inbox.seen.empty());
        CHECK_EQ(ecus.ecus[1]->network.statistics().transport_bam_dropped, 1u);
        CHECK(ecus.ecus[0]->ends.list.at(0).result == TransferResult::Completed); // the sender cannot know
    }
}

// --- address changes during a transfer ------------------------------------------------------

namespace {

constexpr std::uint64_t kOurName = 0x0000'1000'0000'0100u;
constexpr std::uint64_t kAacName = 0x8000'1000'0000'0100u;

J1939Frame claim_frame(Address sa, std::uint64_t name)
{
    Bytes data(8);
    for (int i = 0; i < 8; ++i)
    {
        data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(name >> (8 * i));
    }
    return frame(6, kPgnAddressClaim, sa, kGlobal, data);
}

} // namespace

TEST_CASE(transport_sender_that_loses_its_address_cancels_its_transfer_without_a_frame)
{
    Bench bench;
    // Not arbitrary address capable at 0x20: no wait after the claim.
    Node& us = bench.network.add_node({0x20, Name{kOurName}});
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(100);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, message, kB) == SendStatus::Ok);
        }
    };
    bench.step();
    bench.inject(cts(kB, 0x20, 4, 1, kPgn));
    bench.step();
    bench.take_sent();
    bench.inject(claim_frame(0x20, kOurName - 1)); // a lower NAME takes 0x20
    bench.inject(cts(kB, 0x20, 4, 5, kPgn));
    bench.steps(300);
    const auto sent = bench.take_sent();
    CHECK_EQ(sent.size(), 1u);
    CHECK(sent[0].pgn == kPgnAddressClaim && sent[0].sa == kNull); // only Cannot Claim
    CHECK_EQ(ends.list.size(), 1u);
    CHECK(ends.list[0].result == TransferResult::Cancelled);
    CHECK(us.address_state() == AddressState::CannotClaim);
}

TEST_CASE(transport_receiver_that_moves_drops_its_session_silently)
{
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    Inbox inbox;
    inbox.attach(us);
    bench.steps(30); // claimed after 250 ms
    bench.take_sent();
    const Bytes message = payload(21);
    bench.inject(rts(kB, 0x80, 21, kPgn));
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(0x80, kB, 3, 1, kPgn)});
    bench.inject(dt(kB, 0x80, 1, message));
    bench.inject(claim_frame(0x80, kAacName - 1)); // we move to 0x81
    bench.step();
    bench.inject(dt(kB, 0x80, 2, message));
    bench.inject(dt(kB, 0x80, 3, message));
    bench.steps(300);
    const auto sent = bench.take_sent();
    CHECK_EQ(sent.size(), 1u);
    CHECK(sent[0] == claim_frame(0x81, kAacName)); // no abort, no acknowledgement
    CHECK(std::none_of(inbox.seen.begin(), inbox.seen.end(), [](const Seen& m) { return m.pgn == kPgn; }));
    CHECK_EQ(bench.network.statistics().transport_sessions_cancelled, 1u);
}

TEST_CASE(transport_ends_the_sessions_with_an_address_another_name_takes)
{
    Bench bench;
    Node& us = bench.add_node(kA);
    Ends ends;
    ends.attach(us);
    Inbox inbox;
    inbox.attach(us);
    bench.inject(claim_frame(kB, 0x5000));
    bench.step();
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(40), kB) == SendStatus::Ok);
        }
    };
    bench.inject(bam(kB, 30, 0xFECA));
    bench.inject(rts(kB, kA, 30, 0xED00));
    bench.step();
    bench.take_sent();
    bench.inject(claim_frame(kB, 0x4000)); // another control function at kB
    bench.step();
    bench.steps(200);
    CHECK_J1939(bench.take_sent(), {});
    CHECK(ends.list.at(0).result == TransferResult::Cancelled);
    CHECK_EQ(bench.network.statistics().transport_sessions_cancelled, 3u);
}

TEST_CASE(transport_waits_for_the_end_of_the_wait_after_the_claim)
{
    // Arbitrary address capable at 0x80: 250 ms after the claim. A transfer sent during the wait
    // starts when it ends; an RTS to the node during the wait is answered then.
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    Inbox inbox;
    inbox.attach(us);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(9), kB) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgn}, payload(9), kB) == SendStatus::Busy);
        }
    };
    bench.step();
    bench.inject(rts(kB, 0x80, 9, 0xED00));
    bench.steps(24);
    CHECK_J1939(bench.take_sent(), {claim_frame(0x80, kAacName)});
    bench.step(); // 250 ms
    // In the order the node held them.
    CHECK_J1939(bench.take_sent(), {rts(0x80, kB, 9, kPgn), cts(0x80, kB, 2, 1, 0xED00)});
    CHECK(bench.sent_times.back() == at(250ms));
}

TEST_CASE(transport_transfer_held_during_the_wait_is_cancelled_when_the_node_moves)
{
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    Ends ends;
    ends.attach(us);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(9), kB) == SendStatus::Ok);
        }
    };
    bench.step();
    bench.inject(claim_frame(0x80, kAacName - 1));
    bench.steps(40);
    CHECK_J1939(bench.take_sent(), {claim_frame(0x80, kAacName), claim_frame(0x81, kAacName)});
    CHECK(ends.list.at(0).result == TransferResult::Cancelled);
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
}

TEST_CASE(transport_delivers_commanded_address_by_broadcast)
{
    // Commanded Address is 9 bytes: it arrives by the transport protocol and moves the node.
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    bench.steps(30);
    bench.take_sent();
    Bytes command(9);
    for (int i = 0; i < 8; ++i)
    {
        command[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(kAacName >> (8 * i));
    }
    command[8] = 0x99;
    bench.inject(bam(kB, 9, 0xFED8));
    bench.inject(dt(kB, kGlobal, 1, command));
    bench.inject(dt(kB, kGlobal, 2, command));
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim_frame(0x99, kAacName)});
    CHECK_EQ(us.address(), 0x99);
}

TEST_CASE(transport_delivers_commanded_address_by_connection)
{
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    bench.steps(30);
    bench.take_sent();
    Bytes command(9);
    for (int i = 0; i < 8; ++i)
    {
        command[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(kAacName >> (8 * i));
    }
    command[8] = 0x99;
    bench.inject(rts(kB, 0x80, 9, 0xFED8));
    bench.step();
    bench.take_sent();
    bench.inject(dt(kB, 0x80, 1, command));
    bench.inject(dt(kB, 0x80, 2, command));
    bench.step();
    // The acknowledgement leaves from the old address (queued before the move): dropped with it.
    CHECK_J1939(bench.take_sent(), {claim_frame(0x99, kAacName)});
    CHECK_EQ(us.address(), 0x99);
}

// --- several ECUs and sessions ---------------------------------------------------------------

TEST_CASE(transport_several_ecus_send_to_one_receiver_at_once)
{
    for (const auto& steps : {std::vector<SimDuration>{10ms, 10ms, 10ms, 10ms}, std::vector<SimDuration>{1ms, 7ms, 3ms, 10ms}})
    {
        Ecus ecus(steps);
        Node& receiver = ecus.add(0, {kB});
        std::vector<Node*> senders;
        for (std::size_t i = 1; i < 4; ++i)
        {
            senders.push_back(&ecus.add(i, {static_cast<Address>(0x30 + i)}));
            const std::size_t k = i;
            Node* node = senders.back();
            ecus.apps[i] = [node, k](SimTime now) {
                if (now == at(0ms))
                {
                    CHECK(node->send(Pgn{kPgn}, payload(200, static_cast<std::uint8_t>(k)), kB) == SendStatus::Ok);
                    CHECK(node->send(Pgn{0xFECA}, payload(50, static_cast<std::uint8_t>(k + 10))) == SendStatus::Ok);
                }
            };
        }
        // The receiver also broadcasts to all of them.
        ecus.apps[0] = [&receiver](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(receiver.send(Pgn{0xFECB}, payload(100, 99)) == SendStatus::Ok);
            }
        };
        ecus.run_until(at(3s));
        const auto& got = ecus.ecus[0]->inbox.seen;
        CHECK_EQ(got.size(), 6u); // 3 connections, 3 broadcasts
        for (std::size_t i = 1; i < 4; ++i)
        {
            const auto sa = static_cast<Address>(0x30 + i);
            int connections = 0;
            int broadcasts = 0;
            for (const auto& s : got)
            {
                connections += (s.sa == sa && s.pgn == kPgn && s.data == payload(200, static_cast<std::uint8_t>(i))) ? 1 : 0;
                broadcasts += (s.sa == sa && s.pgn == 0xFECA && s.data == payload(50, static_cast<std::uint8_t>(i + 10))) ? 1 : 0;
            }
            CHECK_EQ(connections, 1);
            CHECK_EQ(broadcasts, 1);
            CHECK_EQ(ecus.ecus[i]->inbox.seen.size(), 3u); // the receiver's broadcast and the other two
            CHECK_EQ(ecus.ecus[i]->ends.list.size(), 2u);
        }
    }
}

TEST_CASE(transport_receiver_without_a_free_session_refuses_and_recovers)
{
    NetworkOptions options = quiet();
    options.rts_receive_sessions = 1;
    Ecus ecus({10ms, 10ms, 10ms}, options);
    ecus.add(0, {kB});
    Node& first = ecus.add(1, {0x31});
    Node& second = ecus.add(2, {0x32});
    ecus.apps[1] = [&first](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(first.send(Pgn{kPgn}, payload(100), kB) == SendStatus::Ok);
        }
    };
    ecus.apps[2] = [&second](SimTime now) {
        if (now == at(0ms) || now == at(1s))
        {
            CHECK(second.send(Pgn{kPgn}, payload(100, 1), kB) == SendStatus::Ok);
        }
    };
    ecus.run_until(at(2s));
    CHECK_EQ(ecus.ecus[0]->inbox.seen.size(), 2u);
    CHECK(ecus.ecus[2]->ends.list.at(0).reason == AbortReason::Busy);
    CHECK(ecus.ecus[2]->ends.list.at(1).result == TransferResult::Completed);
}

TEST_CASE(transport_between_nodes_of_the_same_network)
{
    Bench bench;
    Node& a = bench.add_node(kA);
    Node& b = bench.add_node(kB);
    Inbox inA;
    Inbox inB;
    inA.attach(a);
    inB.attach(b);
    Ends ends;
    ends.attach(a);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(a.send(Pgn{kPgn}, payload(300), kB) == SendStatus::Ok);
            CHECK(b.send(Pgn{kPgn}, payload(30, 1), kA) == SendStatus::Ok);
        }
    };
    bench.steps(20);
    CHECK_EQ(inB.seen.size(), 1u);
    CHECK(inB.seen[0].data == payload(300));
    CHECK_EQ(inA.seen.size(), 1u);
    CHECK(inA.seen[0].data == payload(30, 1));
    CHECK(ends.list.at(0).result == TransferResult::Completed);
}

TEST_CASE(transport_leaves_no_session_behind_after_many_transfers)
{
    // Completed, refused, aborted, timed out and cancelled transfers; afterwards every session
    // can be used again.
    NetworkOptions options = quiet();
    Ecus ecus({10ms, 10ms}, options);
    Node& a = ecus.add(0, {kA});
    ecus.add(1, {kB});
    // A fixed pseudo-random sequence: about 2 % of the packets lost, 0.5 % delayed past T1.
    auto state = std::make_shared<std::uint32_t>(12345u);
    std::size_t accepted = 0;
    ecus.ecus[0]->port.set_fault_injector([state](const TimedFrame& f) {
        *state = *state * 1664525u + 1013904223u;
        const std::uint32_t r = (*state >> 16) % 1000u;
        if (is_dt(f) && r < 20)
        {
            return FaultAction::drop();
        }
        if (is_dt(f) && r < 25)
        {
            return FaultAction::delay_by(800ms);
        }
        return FaultAction::deliver();
    });
    ecus.apps[0] = [&a, &accepted](SimTime now) {
        const auto ms = (now - SimTime{}) / 1ms;
        if (ms % 100 == 0 && ms < 20000)
        {
            const auto size = static_cast<std::size_t>(9 + (ms / 100) * 37 % 1700);
            accepted += a.send(Pgn{kPgn}, payload(size), kB) == SendStatus::Ok ? 1u : 0u;
            accepted += a.send(Pgn{0xFECA}, payload(size / 2 + 9)) == SendStatus::Ok ? 1u : 0u;
        }
    };
    ecus.run_until(at(25s));
    // Every accepted transfer ended exactly once, in all the ways a transfer can end.
    const auto& ends = ecus.ecus[0]->ends.list;
    CHECK_EQ(ends.size(), accepted);
    const auto count = [&](TransferResult r) {
        return std::count_if(ends.begin(), ends.end(), [r](const TransferEnd& e) { return e.result == r; });
    };
    CHECK(count(TransferResult::Completed) > static_cast<long>(accepted) / 2);
    CHECK(count(TransferResult::AbortedByPeer) + count(TransferResult::Aborted) > 0);
    // No session is left: no timer runs on either side.
    CHECK(ecus.ecus[0]->network.next_deadline() == std::nullopt);
    CHECK(ecus.ecus[1]->network.next_deadline() == std::nullopt);
}

TEST_CASE(transport_ends_the_sessions_with_a_peer_that_gives_up_its_address)
{
    // The peer sends Cannot Claim (its NAME leaves the address table).
    Bench bench;
    Node& us = bench.add_node(kA);
    Ends ends;
    ends.attach(us);
    bench.inject(claim_frame(kB, 0x5000));
    bench.step();
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(40), kB) == SendStatus::Ok);
        }
    };
    bench.step();
    bench.take_sent();
    bench.inject(at(13ms), claim_frame(kNull, 0x5000));
    bench.steps(200);
    CHECK_J1939(bench.take_sent(), {});
    CHECK(ends.list.at(0).result == TransferResult::Cancelled);
    CHECK(ends.list.at(0).time == at(13ms)); // the Cannot Claim's time, not the step's
}

TEST_CASE(transport_frame_from_our_address_is_an_address_violation)
{
    Bench bench;
    Node& us = bench.network.add_node({0x20, Name{kOurName}});
    bench.steps(30);
    bench.take_sent();
    bench.inject(bam(0x20, 9, 0xFECA)); // another sender with our address
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim_frame(0x20, kOurName)});
    CHECK_EQ(bench.network.statistics().address_violations, 1u);
    CHECK(us.address_state() == AddressState::Claimed);
}

TEST_CASE(transport_sent_before_the_first_claim_leaves_from_the_claimed_address)
{
    // An arbitrary address capable node without a configured address: the address is known only
    // when it claims; the transfer then starts from it.
    // The node is added in the application stage and sends at once: it claims in the next step.
    Bench bench;
    Node* us = nullptr;
    Ends ends;
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            us = &bench.network.add_node({kNull, Name{kAacName}});
            ends.attach(*us);
            CHECK(us->address_state() == AddressState::NotStarted);
            CHECK(us->send(Pgn{kPgn}, payload(9), kB) == SendStatus::Ok);
        }
    };
    bench.steps(27);
    CHECK_J1939(bench.take_sent(), {claim_frame(128, kAacName), rts(128, kB, 9, kPgn)});
    bench.inject(cts(kB, 128, 2, 1, kPgn));
    bench.step();
    CHECK_FRAMES(bench.take_sent(), dts(128, kB, payload(9), 1, 2));
    bench.inject(eoma(kB, 128, 9, kPgn));
    bench.step();
    CHECK(ends.list.at(0).result == TransferResult::Completed);
}

TEST_CASE(transport_handler_that_sends_again_after_a_cancel_keeps_the_new_transfer)
{
    // The node moves while its broadcast runs; the transfer end handler sends a connection at
    // once (its session comes after the broadcast's in the session table). The new transfer
    // (from the new address) is not cancelled with the old one.
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    std::vector<TransferResult> results;
    int resent = 0;
    us.on_transfer_end([&](const TransferEnd& e) {
        results.push_back(e.result);
        if (e.result == TransferResult::Cancelled && resent < 5)
        {
            ++resent;
            CHECK(us.send(Pgn{kPgn}, payload(9), kB) == SendStatus::Ok);
        }
    });
    bench.steps(30);
    bench.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            CHECK(us.send(Pgn{0xFECA}, payload(30)) == SendStatus::Ok);
        }
    };
    bench.step();
    bench.take_sent();
    bench.inject(claim_frame(0x80, kAacName - 1)); // we move to 0x81 and wait again
    bench.steps(30);
    CHECK_J1939(bench.take_sent(), {claim_frame(0x81, kAacName), rts(0x81, kB, 9, kPgn)});
    CHECK(results == (std::vector<TransferResult>{TransferResult::Cancelled}));
    CHECK_EQ(resent, 1);
}

TEST_CASE(transport_handler_that_sends_again_while_held_transfers_are_dropped)
{
    // A transfer held during the wait is cancelled when the node moves. The handler is told after
    // the move (it sees the new address and state, then on_address_change follows), so what it
    // sends again is held for the new address and kept.
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    std::vector<TransferResult> results;
    std::vector<std::string> order;
    int resent = 0;
    us.on_transfer_end([&](const TransferEnd& e) {
        results.push_back(e.result);
        order.push_back("end");
        CHECK(us.address() == 0x81);
        CHECK(us.address_state() == AddressState::Claiming);
        if (e.result == TransferResult::Cancelled && resent < 5)
        {
            ++resent;
            CHECK(us.send(Pgn{kPgn}, payload(9), kB) == SendStatus::Ok);
        }
    });
    us.on_address_change([&](const AddressChange& c) {
        if (c.previous_address == 0x80 && c.address == 0x81)
        {
            order.push_back("change");
        }
    });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(9), kB) == SendStatus::Ok);
        }
    };
    bench.step();
    bench.inject(claim_frame(0x80, kAacName - 1));
    bench.steps(30);
    CHECK_J1939(bench.take_sent(), {claim_frame(0x80, kAacName), claim_frame(0x81, kAacName), rts(0x81, kB, 9, kPgn)});
    CHECK(results == (std::vector<TransferResult>{TransferResult::Cancelled}));
    CHECK_EQ(resent, 1);
    CHECK(order == (std::vector<std::string>{"end", "change"}));
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
    CHECK_EQ(bench.network.statistics().transport_sessions_cancelled, 1u);
}

// --- after the independent review ----------------------------------------------------------

TEST_CASE(transport_held_cts_that_finds_the_control_queue_full_frees_its_session)
{
    // An RTS reaches the node during the wait after its claim; when the wait ends the control
    // queue is full (the port refused everything), so the clear-to-send is dropped. The session
    // must not stay: a later RTS for another group from the same sender is taken.
    NetworkOptions options = quiet();
    options.receive_capacity = 1;
    options.max_nodes = 1;
    options.hold_capacity = 2;
    options.bam_receive_sessions = 0;
    options.rts_receive_sessions = 1;
    options.bam_transmit_sessions = 0;
    options.rts_transmit_sessions = 0;
    options.response_queue_capacity = 0; // control queue: (1 + 1 * (2 + 2) + 1 + 0) * 2 = 12
    Bench bench(options);
    bench.network.add_node({0x80, Name{kAacName}});
    bench.port.set_fault_injector([](const TimedFrame& f) {
        return f.time < at(400ms) ? FaultAction::queue_full() : FaultAction::deliver();
    });
    bench.step();
    bench.inject(rts(kB, 0x80, 9, kPgn));
    bench.step();
    // Requests for Address Claimed fill the control queue with claims (not held).
    for (int i = 0; i < 20; ++i)
    {
        bench.inject(frame(6, kPgnRequest, kB, kGlobal, request(kPgnAddressClaim)));
        bench.step();
    }
    bench.steps(20); // the wait ends at 250 ms; the port takes frames from 400 ms
    bench.take_sent();
    CHECK(bench.network.statistics().control_frames_dropped > 0u);
    bench.inject(rts(kB, 0x80, 9, 0xED00));
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(0x80, kB, 2, 1, 0xED00)});
}

TEST_CASE(transport_cancel_time_is_the_time_of_the_claim_whatever_the_steps)
{
    for (const SimDuration step : {1ms, 10ms, 50ms})
    {
        Bench bench(quiet(), step);
        Node& us = bench.add_node(kA);
        Ends ends;
        ends.attach(us);
        bench.inject(at(0ms), claim_frame(kB, 0x5000));
        bench.step();
        bench.app = [&](SimTime now) {
            if (now == bench.last_time() && ends.list.empty() && now > at(0ms) && now < at(60ms))
            {
                static_cast<void>(us.send(Pgn{kPgn}, payload(40), kB));
            }
        };
        bench.step();
        bench.app = {};
        bench.inject(at(103ms), claim_frame(kB, 0x4000)); // another NAME takes kB
        while (bench.last_time() < at(300ms))
        {
            bench.step();
        }
        CHECK_EQ(ends.list.size(), 1u);
        CHECK(ends.list.at(0).result == TransferResult::Cancelled);
        CHECK(ends.list.at(0).time == at(103ms));
    }
}

namespace {

/// A node of `bench` that answers requests for 0xFECA and 0xFECB with 9 bytes; one transmit
/// connection, so the second response to the same requester waits in the response queue.
Node& responder(Bench& bench, Ends& ends, Address address, std::optional<Name> name)
{
    Node& us = bench.network.add_node({address, name});
    ends.attach(us);
    us.on_any_request([](vehicle_sil::j1939::RequestContext& c) { CHECK(c.respond(payload(9)) == SendStatus::Ok); });
    return us;
}

NetworkOptions one_connection()
{
    NetworkOptions options = quiet();
    options.rts_transmit_sessions = 1;
    return options;
}

} // namespace

TEST_CASE(transport_queued_response_is_cancelled_when_the_node_moves)
{
    Bench bench(one_connection());
    Ends ends;
    responder(bench, ends, 0x80, Name{kAacName});
    bench.steps(30);
    bench.take_sent();
    bench.inject(frame(6, kPgnRequest, kB, 0x80, request(0xFECA)));
    bench.inject(frame(6, kPgnRequest, kB, 0x80, request(0xFECB)));
    bench.step();
    CHECK_J1939(bench.take_sent(), {rts(0x80, kB, 9, 0xFECA)}); // the second waits
    bench.inject(claim_frame(0x80, kAacName - 1));
    bench.steps(40);
    CHECK_J1939(bench.take_sent(), {claim_frame(0x81, kAacName)});
    CHECK_EQ(ends.list.size(), 2u);
    CHECK(ends.list.at(0).result == TransferResult::Cancelled);
    CHECK(ends.list.at(1).result == TransferResult::Cancelled);
    CHECK(ends.list.at(1).pgn == Pgn{0xFECB});
    CHECK_EQ(bench.network.statistics().transport_sessions_cancelled, 2u);
}

TEST_CASE(transport_queued_response_is_cancelled_when_the_requester_address_changes_hands)
{
    Bench bench(one_connection());
    Ends ends;
    responder(bench, ends, kA, std::nullopt);
    bench.inject(claim_frame(kB, 0x5000));
    bench.step();
    bench.inject(frame(6, kPgnRequest, kB, kA, request(0xFECA)));
    bench.inject(frame(6, kPgnRequest, kB, kA, request(0xFECB)));
    bench.step();
    CHECK_J1939(bench.take_sent(), {rts(kA, kB, 9, 0xFECA)});
    bench.inject(claim_frame(kNull, 0x5000)); // the requester gives up its address
    bench.steps(200);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(ends.list.size(), 2u);
    CHECK(ends.list.at(1).result == TransferResult::Cancelled);
    CHECK(ends.list.at(1).pgn == Pgn{0xFECB});
}

TEST_CASE(transport_late_packet_of_an_earlier_window_does_not_use_up_a_request)
{
    NetworkOptions options = quiet();
    options.cts_window = 4;
    Bench bench(options);
    Node& us = bench.add_node(kA);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(56); // 8 packets
    bench.step();
    bench.inject(rts(kB, kA, 56, kPgn));
    bench.step();
    bench.take_sent();
    for (const int k : {1, 2, 4})
    {
        bench.inject(dt(kB, kA, static_cast<std::uint8_t>(k), message));
    }
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(kA, kB, 4, 3, kPgn)}); // from packet 3 again
    bench.inject(dt(kB, kA, 1, message)); // late, from before the window: ignored
    for (const int k : {3, 4, 5, 6})
    {
        bench.inject(dt(kB, kA, static_cast<std::uint8_t>(k), message));
    }
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(kA, kB, 2, 7, kPgn)});
    CHECK_EQ(bench.network.statistics().transport_retransmit_requests, 1u);
}

TEST_CASE(transport_does_not_take_address_claimed_or_request_from_a_transfer)
{
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    bench.steps(30);
    bench.take_sent();
    Bytes claim(9, 0);
    bench.inject(bam(0x80, 9, kPgnAddressClaim)); // also another sender with our address ...
    bench.inject(dt(0x80, kGlobal, 1, claim));
    bench.inject(dt(0x80, kGlobal, 2, claim));
    bench.step();
    // ... which is an address violation (claimed again), but not contention: we keep 0x80.
    CHECK_J1939(bench.take_sent(), {claim_frame(0x80, kAacName)});
    CHECK_EQ(us.address(), 0x80);
    CHECK_EQ(bench.network.address_table().size(), 0u);
}

TEST_CASE(transport_broadcast_whose_last_packet_left_completes_despite_a_move)
{
    Bench bench;
    Node& us = bench.network.add_node({0x80, Name{kAacName}});
    Ends ends;
    ends.attach(us);
    bench.steps(30);
    bench.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            CHECK(us.send(Pgn{0xFECA}, payload(9)) == SendStatus::Ok);
        }
    };
    while (bench.last_time() < at(400ms)) // last packet at 400 ms
    {
        bench.step();
    }
    bench.inject(claim_frame(0x80, kAacName - 1)); // handled at 410 ms, with the completion
    bench.steps(3);
    CHECK(ends.list.at(0).result == TransferResult::Completed);
}
