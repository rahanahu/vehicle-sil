// SPDX-License-Identifier: MIT
//
// NetworkOptions::timing: each time and count of the transport protocol and the wait after an
// address claim takes effect when it is set to another value than its default (the defaults are
// covered by the transport and address claim tests), and values that cannot work are refused.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/j1939/network.hpp"

#include <functional>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::ProtocolTiming;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::TransferResult;
using vehicle_sil::unit::throws_with;

namespace {

constexpr Address kUs = 0x80;
constexpr Address kTool = 0x90;
constexpr std::uint32_t kPgn = 0xEF00; ///< PDU1 group to one destination
constexpr std::uint64_t kAacName = 0x8000'1000'0000'0100u;

NetworkOptions quiet()
{
    NetworkOptions options;
    options.warning = [](std::string_view) {};
    return options;
}

NetworkOptions with_timing(const std::function<void(ProtocolTiming&)>& change)
{
    NetworkOptions options = quiet();
    change(options.timing);
    return options;
}

struct Inbox
{
    std::vector<Seen> seen;
    void attach(Node& node)
    {
        node.on_any_message([this](const Message& m) { seen.push_back(seen_of(m)); });
    }
};

void send_at_start(Bench& bench, Node& node, const Bytes& message)
{
    bench.app = [&node, message](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(node.send(Pgn{kPgn}, message, kTool) == SendStatus::Ok);
        }
    };
    bench.step();
}

void run_until(Bench& bench, SimTime time)
{
    while (bench.last_time() < time)
    {
        bench.step();
    }
}

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

TEST_CASE(transport_timing_t2_sets_how_long_a_receiver_waits_after_its_cts)
{
    // The clear-to-send leaves in the step at 10 ms: the deadline is 310 ms instead of 1260 ms.
    Bench bench(with_timing([](ProtocolTiming& t) { t.t2 = 300ms; }));
    bench.add_node(kUs);
    bench.step();
    bench.inject(rts(kTool, kUs, 9, kPgn));
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(kUs, kTool, 2, 1, kPgn)});
    run_until(bench, at(400ms));
    CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 3, kPgn)});
    CHECK(bench.sent_times.back() == at(320ms));
    CHECK_EQ(bench.network.statistics().transport_timeouts, 1u);
}

TEST_CASE(transport_timing_t1_sets_how_long_a_receiver_waits_between_packets)
{
    Bench bench(with_timing([](ProtocolTiming& t) { t.t1 = 200ms; }));
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(21);
    bench.inject(at(0ms), rts(kTool, kUs, 21, kPgn));
    bench.step();
    bench.step(); // clear-to-send at 10 ms
    bench.inject(at(50ms), dt(kTool, kUs, 1, message));
    bench.inject(at(250ms), dt(kTool, kUs, 2, message));       // exactly T1 later: taken
    bench.inject(at(450ms) + 1ns, dt(kTool, kUs, 3, message)); // just after T1: too late
    run_until(bench, at(600ms));
    CHECK_J1939(bench.take_sent(), {cts(kUs, kTool, 3, 1, kPgn), conn_abort(kUs, kTool, 3, kPgn)});
    CHECK(bench.sent_times.back() == at(460ms));
    CHECK(inbox.seen.empty());
}

TEST_CASE(transport_timing_t1_sets_how_long_a_broadcast_receiver_waits)
{
    for (const bool atDeadline : {true, false})
    {
        Bench bench(with_timing([](ProtocolTiming& t) { t.t1 = 100ms; }));
        Node& us = bench.add_node(kUs);
        Inbox inbox;
        inbox.attach(us);
        const Bytes message = payload(9);
        bench.inject(at(0ms), bam(kTool, 9, kPgn));
        bench.inject(at(10ms), dt(kTool, kGlobal, 1, message));
        bench.inject(at(110ms) + (atDeadline ? 0ns : 1ns), dt(kTool, kGlobal, 2, message));
        run_until(bench, at(300ms));
        CHECK_EQ(inbox.seen.size(), atDeadline ? 1u : 0u);
        CHECK_EQ(bench.network.statistics().transport_timeouts, atDeadline ? 0u : 1u);
    }
}

TEST_CASE(transport_timing_t3_sets_how_long_a_sender_waits_for_an_answer)
{
    Bench bench(with_timing([](ProtocolTiming& t) { t.t3 = 400ms; }));
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    send_at_start(bench, us, payload(9)); // RTS in the step at 0 ms
    run_until(bench, at(600ms));
    CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 9, kPgn), conn_abort(kUs, kTool, 3, kPgn)});
    CHECK(bench.sent_times.back() == at(410ms));
    CHECK(ends.list.at(0).result == TransferResult::Aborted);
    CHECK(ends.list.at(0).reason == AbortReason::Timeout);
    CHECK(ends.list.at(0).time == at(400ms));
}

TEST_CASE(transport_timing_t4_sets_how_long_a_sender_waits_after_a_hold)
{
    for (const bool atDeadline : {true, false})
    {
        Bench bench(with_timing([](ProtocolTiming& t) { t.t4 = 300ms; }));
        Node& us = bench.add_node(kUs);
        Ends ends;
        ends.attach(us);
        const Bytes message = payload(21);
        send_at_start(bench, us, message);
        bench.take_sent();
        bench.inject(at(5ms), cts(kTool, kUs, 2, 1, kPgn));
        bench.step();
        bench.take_sent();
        bench.inject(at(15ms), cts(kTool, kUs, 0, 0xFF, kPgn)); // hold after a window
        bench.inject(at(315ms) + (atDeadline ? 0ns : 1ns), cts(kTool, kUs, 1, 3, kPgn));
        run_until(bench, at(400ms));
        if (atDeadline)
        {
            CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 3, 3));
            CHECK(ends.list.empty());
        }
        else
        {
            CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 3, kPgn)});
            CHECK(ends.list.at(0).reason == AbortReason::Timeout);
            CHECK(ends.list.at(0).time == at(315ms));
        }
    }
}

TEST_CASE(transport_timing_max_retransmit_requests_sets_the_requests_per_window)
{
    // Packet 2 never arrives: N requests for it, then an abort.
    for (const std::uint8_t limit : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{4}})
    {
        Bench bench(with_timing([limit](ProtocolTiming& t) { t.max_retransmit_requests = limit; }));
        Node& us = bench.add_node(kUs);
        Inbox inbox;
        inbox.attach(us);
        const Bytes message = payload(21);
        bench.step();
        bench.inject(rts(kTool, kUs, 21, kPgn));
        bench.step();
        bench.take_sent();
        for (int round = 0; round <= limit; ++round)
        {
            for (int k = round == 0 ? 1 : 2; k <= 3; ++k)
            {
                if (k != 2)
                {
                    bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(k), message));
                }
            }
            bench.step();
            const auto sent = bench.take_sent();
            if (round < limit)
            {
                CHECK_J1939(sent, {cts(kUs, kTool, 2, 2, kPgn)});
            }
            else
            {
                CHECK_J1939(sent, {conn_abort(kUs, kTool, 5, kPgn)});
            }
        }
        CHECK(inbox.seen.empty());
        CHECK_EQ(bench.network.statistics().transport_retransmit_requests, std::uint64_t{limit});
    }
}

TEST_CASE(transport_timing_address_claim_wait_sets_the_wait_after_the_claim)
{
    // An arbitrary address capable node waits after its claim (sent in the step at 0 ms); what
    // it sends during the wait leaves when the wait ends.
    Bench bench(with_timing([](ProtocolTiming& t) { t.address_claim_wait = 100ms; }));
    Node& us = bench.network.add_node({kUs, Name{kAacName}});
    send_at_start(bench, us, payload(9));
    bench.steps(9); // up to 90 ms
    CHECK_J1939(bench.take_sent(), {claim_frame(kUs, kAacName)});
    bench.step(); // 100 ms
    CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 9, kPgn)});
    CHECK(bench.sent_times.back() == at(100ms));
}

TEST_CASE(network_refuses_timing_that_cannot_work_and_says_the_default)
{
    const auto make = [](const std::function<void(ProtocolTiming&)>& change) {
        return [options = with_timing(change)] { Network network(options); };
    };
    CHECK(throws_with<std::invalid_argument>(make([](ProtocolTiming& t) { t.t1 = 0ms; }),
                                             "vehicle_sil::j1939::Network: NetworkOptions::timing.t1 is 0 ns. It must "
                                             "be greater than 0 (the default is 750 ms)"));
    CHECK(throws_with<std::invalid_argument>(make([](ProtocolTiming& t) { t.t2 = -1ms; }),
                                             "NetworkOptions::timing.t2 is -1000000 ns. It must be greater than 0 (the "
                                             "default is 1250 ms)"));
    CHECK(throws_with<std::invalid_argument>(make([](ProtocolTiming& t) { t.t3 = 0ms; }),
                                             "NetworkOptions::timing.t3 is 0 ns. It must be greater than 0 (the default "
                                             "is 1250 ms)"));
    CHECK(throws_with<std::invalid_argument>(make([](ProtocolTiming& t) { t.t4 = -1ns; }),
                                             "NetworkOptions::timing.t4 is -1 ns. It must be greater than 0 (the default "
                                             "is 1050 ms)"));
    CHECK(throws_with<std::invalid_argument>(make([](ProtocolTiming& t) { t.address_claim_wait = 0ms; }),
                                             "NetworkOptions::timing.address_claim_wait is 0 ns. It must be greater "
                                             "than 0 (the default is 250 ms)"));
    // The smallest values that work, and no retransmission request at all.
    const Network fine(with_timing([](ProtocolTiming& t) {
        t = ProtocolTiming{.t1 = 1ns, .t2 = 1ns, .t3 = 1ns, .t4 = 1ns, .max_retransmit_requests = 0, .address_claim_wait = 1ns};
    }));
    CHECK(fine.timing() == (ProtocolTiming{.t1 = 1ns, .t2 = 1ns, .t3 = 1ns, .t4 = 1ns, .max_retransmit_requests = 0,
                                           .address_claim_wait = 1ns}));
    CHECK(Network(quiet()).timing() == ProtocolTiming{});
}
