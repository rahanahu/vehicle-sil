// SPDX-License-Identifier: MIT
//
// Connection transfers (RTS/CTS) of the J1939 stack against a tester that plays the
// remote node frame by frame. The cases follow the former AgIsoStack++ tests
// (formerly tests/unit/test_j1939_tp_cmdt.cpp) with the differences recorded in the design: a missing,
// reordered or zero-numbered packet is asked for again with a clear-to-send (at most twice in a
// row, then an abort), a repeated packet is ignored, an RTS for another group from the same
// sender is refused while the running connection goes on, and the originator aborts on T3/T4.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/j1939/network.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::RequestContext;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::TransferResult;

namespace {

constexpr Address kUs = 0x80;
constexpr Address kTool = 0x90;
constexpr std::uint32_t kPgn = 0xEF00;  ///< PDU1 group to one destination
constexpr std::uint32_t kPgn2 = 0xEE00 - 0x100; ///< another PDU1 group (0xED00)

NetworkOptions quiet()
{
    NetworkOptions options;
    options.warning = [](std::string_view) {};
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

/// Starts the responder side: the tester's RTS reaches the network in the next step.
void start_receive(Bench& bench, std::size_t size, std::uint8_t maxPerCts = 0xFF, std::uint32_t pgn = kPgn)
{
    bench.step();
    bench.inject(rts(kTool, kUs, size, pgn, maxPerCts));
    bench.step();
}

/// Plays the originator of `message` to the end: answers every clear-to-send with the packets it
/// asks for. Returns the clear-to-sends seen (count, next).
/// `taken`: frames the test already took from the bench (answered first).
std::vector<std::pair<int, int>> play_originator(Bench& bench, const Bytes& message, std::uint32_t pgn = kPgn,
                                                 std::vector<J1939Frame> taken = {})
{
    std::vector<std::pair<int, int>> asked;
    for (int guard = 0; guard < 600; ++guard)
    {
        const auto sent = guard == 0 ? taken : bench.take_sent();
        bool ended = false;
        for (const auto& f : sent)
        {
            CHECK(f.pgn == kPgnTpCm);
            if (f.data[0] == 17)
            {
                asked.emplace_back(f.data[1], f.data[2]);
                for (int k = f.data[2]; k < f.data[2] + f.data[1]; ++k)
                {
                    bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(k), message));
                }
            }
            else
            {
                ended = true;
            }
        }
        if (ended)
        {
            CHECK_FRAMES(sent, (std::vector<J1939Frame>{eoma(kUs, kTool, message.size(), pgn)}));
            return asked;
        }
        bench.step();
    }
    throw vehicle_sil::unit::Failure("no end of message");
}

void check_receives(std::size_t size, std::uint8_t maxPerCts, std::uint8_t window, std::uint8_t expectedWindow)
{
    NetworkOptions options = quiet();
    options.cts_window = window;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(size, 9);
    start_receive(bench, size, maxPerCts);
    const auto asked = play_originator(bench, message);
    const int packets = packet_count(size);
    std::vector<std::pair<int, int>> expected;
    for (int next = 1; next <= packets; next += expectedWindow)
    {
        expected.emplace_back(std::min<int>(expectedWindow, packets - next + 1), next);
    }
    CHECK(asked == expected);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].data == message);
    CHECK_EQ(inbox.seen[0].da, kUs);
    CHECK_EQ(inbox.seen[0].sa, kTool);
    CHECK_EQ(inbox.seen[0].pgn, kPgn);
    CHECK_EQ(bench.network.statistics().transport_messages_received, 1u);
}

/// Sends `message` from our node to the tester in the first step.
void send_at_start(Bench& bench, Node& node, const Bytes& message, std::uint32_t pgn = kPgn)
{
    bench.app = [&node, message, pgn](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(node.send(Pgn{pgn}, message, kTool) == SendStatus::Ok);
        }
    };
    bench.step();
}

} // namespace

// === responder ==========================================================================

TEST_CASE(rts_receives_the_minimum_message_of_9_bytes)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(9);
    start_receive(bench, 9);
    CHECK_J1939(bench.take_sent(), {cts(kUs, kTool, 2, 1, kPgn)});
    CHECK(bench.sent_times.back() == at(10ms));
    bench.inject(dt(kTool, kUs, 1, message));
    bench.inject(dt(kTool, kUs, 2, message));
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 9, kPgn)});
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0] == (Seen{kPgn, kTool, kUs, 7, at(10ms), message}));
}

TEST_CASE(rts_receives_messages_at_the_packet_count_edges)
{
    check_receives(14, 0xFF, 16, 2);
    check_receives(15, 0xFF, 16, 3);
}

TEST_CASE(rts_receives_the_maximum_message_of_1785_bytes_in_windows_of_16)
{
    check_receives(1785, 0xFF, 16, 16);
}

TEST_CASE(rts_responder_keeps_to_the_window_of_the_rts) { check_receives(100, 5, 16, 5); }
TEST_CASE(rts_responder_limits_the_window_to_its_configuration) { check_receives(200, 0xFF, 7, 7); }
TEST_CASE(rts_responder_takes_255_packets_in_one_window) { check_receives(1785, 0xFF, 255, 255); }

TEST_CASE(rts_responder_aborts_t2_after_its_cts_without_data)
{
    // T2 = 1250 ms from the step that sent the clear-to-send (10 ms). A packet at exactly
    // 1260 ms is taken; nothing by then aborts in the first step after it.
    for (const bool packetAtDeadline : {true, false})
    {
        Bench bench;
        bench.add_node(kUs);
        const Bytes message = payload(9);
        start_receive(bench, 9);
        bench.take_sent();
        if (packetAtDeadline)
        {
            bench.inject(at(1260ms), dt(kTool, kUs, 1, message));
            bench.inject(at(1260ms), dt(kTool, kUs, 2, message));
        }
        while (bench.last_time() < at(1300ms))
        {
            bench.step();
        }
        if (packetAtDeadline)
        {
            CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 9, kPgn)});
        }
        else
        {
            CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 3, kPgn)});
            CHECK(bench.sent_times.back() == at(1270ms));
            CHECK_EQ(bench.network.statistics().transport_timeouts, 1u);
        }
    }
}

TEST_CASE(rts_responder_aborts_t1_between_data_packets_and_ignores_late_ones)
{
    for (const SimDuration step : {1ms, 10ms, 50ms, 200ms})
    {
        Bench bench(quiet(), step);
        Node& us = bench.add_node(kUs);
        Inbox inbox;
        inbox.attach(us);
        const Bytes message = payload(21);
        bench.inject(at(0ms), rts(kTool, kUs, 21, kPgn));
        bench.step();
        bench.step(); // CTS at `step`
        bench.inject(at(300ms), dt(kTool, kUs, 1, message));
        bench.inject(at(1050ms), dt(kTool, kUs, 2, message));       // exactly T1 later: taken
        bench.inject(at(1800ms) + 1ns, dt(kTool, kUs, 3, message)); // just after T1: too late
        while (bench.last_time() < at(2200ms))
        {
            bench.step();
        }
        auto sent = bench.take_sent();
        CHECK_J1939(sent, {cts(kUs, kTool, 3, 1, kPgn), conn_abort(kUs, kTool, 3, kPgn)});
        CHECK(inbox.seen.empty());
        CHECK_EQ(bench.network.statistics().transport_frames_ignored, 1u); // the late packet
    }
}

TEST_CASE(rts_responder_asks_again_for_a_lost_packet)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(35); // 5 packets
    start_receive(bench, 35);
    bench.take_sent();
    for (const int k : {1, 2, 4, 5})
    {
        bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(k), message));
    }
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(kUs, kTool, 3, 3, kPgn)}); // from the first missing one
    for (const int k : {3, 4, 5})
    {
        bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(k), message));
    }
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 35, kPgn)});
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].data == message);
    CHECK_EQ(bench.network.statistics().transport_retransmit_requests, 1u);
}

TEST_CASE(rts_responder_asks_again_for_reordered_and_zero_numbered_packets)
{
    for (const std::vector<int>& order : {std::vector<int>{2, 1, 3}, std::vector<int>{1, 0, 3}, std::vector<int>{1, 3, 2}})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Inbox inbox;
        inbox.attach(us);
        const Bytes message = payload(21);
        start_receive(bench, 21);
        bench.take_sent();
        for (const int k : order)
        {
            bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(k), message));
        }
        bench.step();
        const auto sent = bench.take_sent();
        CHECK_EQ(sent.size(), 1u);
        CHECK_EQ(sent[0].data[0], 17); // a clear-to-send ...
        const int first = sent[0].data[2];
        CHECK(first == (order[0] == 1 ? 2 : 1)); // ... from the first packet not taken
        play_originator(bench, message, kPgn, {sent.back()});
        CHECK_EQ(inbox.seen.size(), 1u);
        CHECK(inbox.seen[0].data == message);
    }
}

TEST_CASE(rts_responder_ignores_a_repeated_packet)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(21);
    start_receive(bench, 21);
    bench.take_sent();
    for (const int k : {1, 1, 2, 2, 3})
    {
        bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(k), message));
    }
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 21, kPgn)});
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK_EQ(bench.network.statistics().transport_retransmit_requests, 0u);
}

TEST_CASE(rts_responder_aborts_after_two_retransmission_requests_in_a_row)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(21);
    start_receive(bench, 21);
    bench.take_sent();
    // Packet 2 never arrives: two requests for it, then an abort.
    for (int round = 0; round < 3; ++round)
    {
        const int first = round == 0 ? 1 : 2;
        for (int k = first; k <= 3; ++k)
        {
            if (k != 2)
            {
                bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(k), message));
            }
        }
        bench.step();
        const auto sent = bench.take_sent();
        if (round < 2)
        {
            CHECK_J1939(sent, {cts(kUs, kTool, 2, 2, kPgn)});
        }
        else
        {
            CHECK_J1939(sent, {conn_abort(kUs, kTool, 5, kPgn)});
        }
    }
    CHECK(inbox.seen.empty());
    CHECK_EQ(bench.network.statistics().transport_retransmit_requests, 2u);
}

TEST_CASE(rts_responder_counts_retransmission_requests_per_window)
{
    // One request in each window: a full window resets the count.
    NetworkOptions options;
    options.cts_window = 2;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(42); // 6 packets, 3 windows
    start_receive(bench, 42);
    bench.take_sent();
    for (int window = 0; window < 3; ++window)
    {
        const int first = 1 + window * 2;
        bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(first + 1), message)); // the first one is lost
        bench.step();
        CHECK_J1939(bench.take_sent(), {cts(kUs, kTool, 2, static_cast<std::uint8_t>(first), kPgn)});
        bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(first), message));
        bench.inject(dt(kTool, kUs, static_cast<std::uint8_t>(first + 1), message));
        bench.step();
        bench.take_sent();
    }
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK_EQ(bench.network.statistics().transport_retransmit_requests, 3u);
}

TEST_CASE(rts_responder_restarts_on_a_repeated_rts_for_the_same_pgn)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes first = payload(21);
    const Bytes second = payload(14, 4);
    start_receive(bench, 21);
    bench.inject(dt(kTool, kUs, 1, first));
    bench.step();
    bench.inject(rts(kTool, kUs, 14, kPgn));
    bench.step();
    const auto sent = bench.take_sent();
    CHECK_J1939(sent, {cts(kUs, kTool, 3, 1, kPgn), cts(kUs, kTool, 2, 1, kPgn)});
    play_originator(bench, second, kPgn, {sent.back()});
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].data == second);
}

TEST_CASE(rts_responder_sends_one_cts_for_two_rts_in_the_same_step)
{
    // The replaced connection's clear-to-send is still queued: it is not sent.
    Bench bench;
    bench.add_node(kUs);
    bench.step();
    bench.inject(rts(kTool, kUs, 21, kPgn));
    bench.inject(rts(kTool, kUs, 14, kPgn));
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(kUs, kTool, 2, 1, kPgn)});
    CHECK_EQ(bench.network.statistics().transport_stale_dropped, 1u);
}

TEST_CASE(rts_responder_refuses_an_rts_for_another_pgn_and_keeps_the_running_one)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(21);
    start_receive(bench, 21);
    bench.take_sent();
    bench.inject(rts(kTool, kUs, 14, kPgn2));
    bench.inject(dt(kTool, kUs, 1, message));
    bench.step();
    CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 1, kPgn2)});
    bench.inject(dt(kTool, kUs, 2, message));
    bench.inject(dt(kTool, kUs, 3, message));
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 21, kPgn)});
    CHECK_EQ(inbox.seen.size(), 1u);
}

TEST_CASE(rts_responder_closes_the_session_on_an_abort_from_the_originator)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(21);
    start_receive(bench, 21);
    bench.take_sent();
    bench.inject(dt(kTool, kUs, 1, message));
    bench.inject(conn_abort(kTool, kUs, 250, kPgn2)); // another group: ignored
    bench.step();
    bench.inject(dt(kTool, kUs, 2, message));
    bench.inject(conn_abort(kTool, kUs, 250, kPgn));
    bench.step();
    bench.inject(dt(kTool, kUs, 3, message));
    bench.steps(200);
    CHECK_J1939(bench.take_sent(), {}); // no answer to the abort, no timeout later
    CHECK(inbox.seen.empty());
    CHECK_EQ(bench.network.statistics().transport_aborts_received, 1u);
    CHECK_EQ(bench.network.statistics().transport_timeouts, 0u);
    // Ignored: the abort for the other group and packet 3 (packet 2 still belonged to the session).
    CHECK_EQ(bench.network.statistics().transport_frames_ignored, 2u);
}

TEST_CASE(rts_responder_refuses_invalid_requests)
{
    struct Case
    {
        std::size_t size;
        int packets;
        std::uint8_t maxPerCts;
        std::uint32_t pgn;
        std::uint8_t reason;
    };
    for (const Case c : {Case{21, 4, 0xFF, kPgn, 250},        // inconsistent packet count
                         Case{1786, 255, 0xFF, kPgn, 9},       // above 1785
                         Case{65535, 255, 0xFF, kPgn, 9},      // the packet count cannot carry it
                         Case{8, 2, 0xFF, kPgn, 250},          // fits in one frame
                         Case{21, -1, 0, kPgn, 250},           // no packet per clear-to-send
                         Case{21, -1, 0xFF, 0xEF01, 250}})     // invalid PGN
    {
        Bench bench;
        bench.add_node(kUs);
        bench.step();
        bench.inject(rts(kTool, kUs, c.size, c.pgn, c.maxPerCts, c.packets));
        bench.step();
        CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, c.reason, c.pgn)});
        CHECK_EQ(bench.network.statistics().transport_rejected, 1u);
    }
}

TEST_CASE(rts_to_global_or_to_another_node_is_ignored)
{
    Bench bench;
    bench.add_node(kUs);
    bench.step();
    bench.inject(rts(kTool, kGlobal, 21, kPgn));
    bench.inject(rts(kTool, 0x55, 21, kPgn));
    bench.inject(dt(kTool, 0x55, 1, payload(21)));
    bench.steps(200);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(bench.network.statistics().transport_frames_ignored, 1u); // only the one to global
}

TEST_CASE(rts_responder_ignores_cts_and_eoma_from_the_originator)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(9);
    start_receive(bench, 9);
    bench.take_sent();
    bench.inject(cts(kTool, kUs, 2, 1, kPgn));
    bench.inject(eoma(kTool, kUs, 9, kPgn));
    bench.inject(dt(kTool, kUs, 1, message));
    bench.inject(dt(kTool, kUs, 2, message));
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 9, kPgn)});
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK_EQ(bench.network.statistics().transport_frames_ignored, 2u);
}

TEST_CASE(rts_receive_sessions_are_limited_and_free_again)
{
    NetworkOptions options;
    options.rts_receive_sessions = 2;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    bench.step();
    for (Address sa = 0x10; sa < 0x13; ++sa)
    {
        bench.inject(rts(sa, kUs, 9, kPgn));
    }
    bench.step();
    CHECK_J1939(bench.take_sent(),
                {cts(kUs, 0x10, 2, 1, kPgn), cts(kUs, 0x11, 2, 1, kPgn), conn_abort(kUs, 0x12, 1, kPgn)});
    for (Address sa = 0x10; sa < 0x12; ++sa)
    {
        bench.inject(dt(sa, kUs, 1, payload(9, sa)));
        bench.inject(dt(sa, kUs, 2, payload(9, sa)));
    }
    bench.step();
    bench.inject(rts(0x12, kUs, 9, kPgn));
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, 0x10, 9, kPgn), eoma(kUs, 0x11, 9, kPgn), cts(kUs, 0x12, 2, 1, kPgn)});
    CHECK_EQ(inbox.seen.size(), 2u);
}

TEST_CASE(rts_and_bam_from_the_same_sender_run_at_once)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes connection = payload(14, 1);
    const Bytes broadcast = payload(14, 2);
    start_receive(bench, 14);
    bench.take_sent();
    bench.inject(bam(kTool, 14, 0xFECA));
    bench.inject(dt(kTool, kUs, 1, connection));
    bench.inject(dt(kTool, kGlobal, 1, broadcast));
    bench.inject(dt(kTool, kUs, 2, connection));
    bench.inject(dt(kTool, kGlobal, 2, broadcast));
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 14, kPgn)});
    CHECK_EQ(inbox.seen.size(), 2u);
    CHECK(inbox.seen[0].data == connection);
    CHECK(inbox.seen[1].data == broadcast);
}

TEST_CASE(rts_responder_takes_a_pdu2_group_to_its_address)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    start_receive(bench, 9, 0xFF, 0xFECA);
    play_originator(bench, payload(9), 0xFECA);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK_EQ(inbox.seen[0].pgn, 0xFECAu);
    CHECK_EQ(inbox.seen[0].da, kUs);
}

// === originator =========================================================================

TEST_CASE(rts_sends_the_minimum_message_of_9_bytes)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(9);
    send_at_start(bench, us, message);
    CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 9, kPgn)});
    bench.inject(cts(kTool, kUs, 2, 1, kPgn));
    bench.step();
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 1, 2));
    CHECK(bench.sent_times.back() == at(10ms));
    bench.inject(eoma(kTool, kUs, 9, kPgn));
    bench.step();
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(ends.list.size(), 1u);
    CHECK(ends.list[0].result == TransferResult::Completed);
    CHECK_EQ(ends.list[0].destination, kTool);
    CHECK(ends.list[0].time == at(10ms));
}

namespace {

/// Plays the responder of a transfer of `size` bytes with windows of `window` packets; checks the
/// packets and returns the number of clear-to-sends.
void check_sends(std::size_t size, int window)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(size, 6);
    send_at_start(bench, us, message);
    CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, size, kPgn)});
    const int packets = packet_count(size);
    for (int next = 1; next <= packets; next += window)
    {
        const int count = std::min(window, packets - next + 1);
        bench.inject(cts(kTool, kUs, static_cast<std::uint8_t>(count), static_cast<std::uint8_t>(next), kPgn));
        bench.step();
        CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, next, next + count - 1));
    }
    bench.inject(eoma(kTool, kUs, size, kPgn));
    bench.step();
    CHECK_EQ(ends.list.size(), 1u);
    CHECK(ends.list[0].result == TransferResult::Completed);
}

} // namespace

TEST_CASE(rts_sends_messages_at_the_packet_count_edges)
{
    check_sends(14, 16);
    check_sends(15, 16);
}
TEST_CASE(rts_sends_the_maximum_message_of_1785_bytes) { check_sends(1785, 16); }
TEST_CASE(rts_sends_in_the_windows_the_responder_asks_for)
{
    check_sends(100, 7);
    check_sends(100, 1);
    check_sends(1785, 255);
}

TEST_CASE(rts_originator_sends_no_packet_past_the_end_of_the_message)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    const Bytes message = payload(21);
    send_at_start(bench, us, message);
    bench.take_sent();
    bench.inject(cts(kTool, kUs, 10, 1, kPgn));
    bench.step();
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 1, 3));
}

TEST_CASE(rts_originator_aborts_on_a_cts_above_its_rts_limit)
{
    NetworkOptions options;
    options.rts_max_packets_per_cts = 4;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(70);
    send_at_start(bench, us, message);
    CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 70, kPgn, 4)});
    bench.inject(cts(kTool, kUs, 5, 1, kPgn));
    bench.step();
    CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 250, kPgn)});
    CHECK(ends.list.at(0).result == TransferResult::Aborted);
    CHECK(ends.list.at(0).reason == AbortReason::Other);
}

TEST_CASE(rts_originator_waits_while_the_responder_holds)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(21);
    send_at_start(bench, us, message);
    bench.take_sent();
    // Hold at 0, again at 500 and 1000 ms (each restarts T4 = 1050 ms), then the packets.
    bench.inject(at(0ms), cts(kTool, kUs, 0, 0xFF, kPgn));
    bench.inject(at(500ms), cts(kTool, kUs, 0, 0xFF, kPgn));
    bench.inject(at(1000ms), cts(kTool, kUs, 0, 0xFF, kPgn));
    bench.inject(at(2000ms), cts(kTool, kUs, 3, 1, kPgn));
    while (bench.last_time() < at(2010ms))
    {
        bench.step();
    }
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 1, 3));
    bench.inject(eoma(kTool, kUs, 21, kPgn));
    bench.step();
    CHECK(ends.list.at(0).result == TransferResult::Completed);
}

TEST_CASE(rts_originator_gives_up_t4_after_a_hold)
{
    // T4 = 1050 ms from the hold's frame time; a clear-to-send at exactly T4 is taken.
    for (const bool atDeadline : {true, false})
    {
        Bench bench;
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
        bench.inject(at(1065ms) + (atDeadline ? 0ns : 1ns), cts(kTool, kUs, 1, 3, kPgn));
        while (bench.last_time() < at(1100ms))
        {
            bench.step();
        }
        if (atDeadline)
        {
            CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 3, 3));
            CHECK(ends.list.empty());
        }
        else
        {
            CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 3, kPgn)});
            CHECK(ends.list.at(0).reason == AbortReason::Timeout);
            CHECK(ends.list.at(0).time == at(1065ms));
        }
    }
}

TEST_CASE(rts_originator_accepts_the_eoma_after_a_hold_that_follows_the_last_packet)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(14);
    send_at_start(bench, us, message);
    bench.inject(cts(kTool, kUs, 2, 1, kPgn));
    bench.step();
    bench.inject(cts(kTool, kUs, 0, 0xFF, kPgn));
    bench.step();
    bench.inject(eoma(kTool, kUs, 14, kPgn));
    bench.step();
    CHECK(ends.list.at(0).result == TransferResult::Completed);
}

TEST_CASE(rts_originator_aborts_t3_after_the_rts_without_an_answer)
{
    // T3 = 1250 ms from the step that sent the RTS (0 ms). The design follows the 2022 revision
    // of J1939-21 and sends an abort (reason 3).
    for (const SimDuration step : {1ms, 10ms, 50ms, 200ms})
    {
        Bench bench(quiet(), step);
        Node& us = bench.add_node(kUs);
        Ends ends;
        ends.attach(us);
        send_at_start(bench, us, payload(9));
        while (bench.last_time() < at(1500ms))
        {
            bench.step();
        }
        CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 9, kPgn), conn_abort(kUs, kTool, 3, kPgn)});
        CHECK(bench.sent_times.back() > at(1250ms));
        CHECK(bench.sent_times.back() <= at(1250ms) + step);
        CHECK(ends.list.at(0).time == at(1250ms));
    }
}

TEST_CASE(rts_originator_takes_a_cts_at_exactly_t3)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    const Bytes message = payload(9);
    send_at_start(bench, us, message);
    bench.take_sent();
    bench.inject(at(1250ms), cts(kTool, kUs, 2, 1, kPgn));
    while (bench.last_time() < at(1270ms))
    {
        bench.step();
    }
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 1, 2));
}

TEST_CASE(rts_originator_aborts_t3_after_a_window_or_the_last_packet)
{
    for (const int window : {2, 3})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Ends ends;
        ends.attach(us);
        send_at_start(bench, us, payload(21));
        bench.inject(cts(kTool, kUs, static_cast<std::uint8_t>(window), 1, kPgn));
        bench.step(); // packets at 10 ms; T3 from there
        bench.take_sent();
        while (bench.last_time() < at(1300ms))
        {
            bench.step();
        }
        CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 3, kPgn)});
        CHECK(bench.sent_times.back() == at(1270ms));
        CHECK(ends.list.at(0).result == TransferResult::Aborted);
    }
}

TEST_CASE(rts_originator_sends_the_packets_a_repeated_cts_asks_for)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(35);
    send_at_start(bench, us, message);
    bench.take_sent();
    bench.inject(cts(kTool, kUs, 3, 1, kPgn));
    bench.step();
    bench.take_sent();
    bench.inject(cts(kTool, kUs, 2, 2, kPgn)); // packets 2 and 3 again
    bench.step();
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 2, 3));
    bench.inject(cts(kTool, kUs, 2, 4, kPgn));
    bench.step();
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 4, 5));
    bench.inject(cts(kTool, kUs, 1, 5, kPgn)); // after the last packet, instead of the EoMA
    bench.step();
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 5, 5));
    bench.inject(eoma(kTool, kUs, 35, kPgn));
    bench.step();
    CHECK(ends.list.at(0).result == TransferResult::Completed);
}

TEST_CASE(rts_originator_aborts_on_a_cts_for_packets_it_cannot_send)
{
    // 10 packets. Packet 0, past the end, skipping packets, before the last window (already
    // acknowledged by asking for it), skipping after windows.
    struct Case
    {
        std::uint8_t first;
        bool afterWindows;
    };
    for (const Case c : {Case{0, false}, Case{11, false}, Case{2, false}, Case{1, true}, Case{6, true}})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Ends ends;
        ends.attach(us);
        send_at_start(bench, us, payload(70));
        bench.take_sent();
        if (c.afterWindows)
        {
            bench.inject(cts(kTool, kUs, 2, 1, kPgn));
            bench.step();
            bench.inject(cts(kTool, kUs, 2, 3, kPgn)); // packets 1..4 sent, window starts at 3
            bench.step();
            bench.take_sent();
        }
        bench.inject(cts(kTool, kUs, 1, c.first, kPgn));
        bench.step();
        CHECK_J1939(bench.take_sent(), {conn_abort(kUs, kTool, 7, kPgn)});
        CHECK(ends.list.at(0).reason == AbortReason::BadSequence);
    }
}

TEST_CASE(rts_originator_aborts_on_a_cts_while_sending)
{
    // The port takes one packet per step: the window is not sent when the next CTS comes.
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    int accepted = 0;
    bench.port.set_fault_injector([&](const vehicle_sil::can::TimedFrame& f) {
        if (((f.frame.id >> 8) & 0x3FF00u) != kPgnTpDt)
        {
            return vehicle_sil::can::FaultAction::deliver();
        }
        return accepted++ % 2 == 0 ? vehicle_sil::can::FaultAction::deliver() : vehicle_sil::can::FaultAction::queue_full();
    });
    send_at_start(bench, us, payload(35));
    bench.take_sent();
    bench.inject(cts(kTool, kUs, 5, 1, kPgn));
    bench.step(); // packet 1
    bench.inject(cts(kTool, kUs, 5, 1, kPgn));
    bench.step();
    const auto sent = bench.take_sent();
    CHECK_EQ(sent.size(), 2u);
    CHECK(sent[1] == conn_abort(kUs, kTool, 4, kPgn));
    CHECK(ends.list.at(0).reason == AbortReason::CtsWhileSending);
}

TEST_CASE(rts_originator_stops_on_an_abort_from_the_responder)
{
    for (const bool beforeCts : {true, false})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Ends ends;
        ends.attach(us);
        send_at_start(bench, us, payload(35));
        bench.take_sent();
        if (!beforeCts)
        {
            bench.inject(cts(kTool, kUs, 2, 1, kPgn));
            bench.step();
            bench.take_sent();
        }
        bench.inject(conn_abort(kTool, kUs, 1, kPgn2)); // another group: ignored
        bench.inject(conn_abort(kTool, kUs, 1, kPgn));
        bench.steps(200);
        CHECK_J1939(bench.take_sent(), {});
        CHECK(ends.list.at(0).result == TransferResult::AbortedByPeer);
        CHECK(ends.list.at(0).reason == AbortReason::Busy);
        CHECK_EQ(bench.network.statistics().transport_aborts_received, 1u);
    }
}

TEST_CASE(rts_originator_ignores_an_early_eoma_and_unrelated_frames)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(21);
    send_at_start(bench, us, message);
    bench.take_sent();
    bench.inject(eoma(kTool, kUs, 21, kPgn));       // before any packet
    bench.inject(cts(kTool, kUs, 2, 1, kPgn2));     // another group
    bench.inject(cts(0x91, kUs, 2, 1, kPgn));       // another node
    bench.inject(cts(kTool, kUs, 2, 1, kPgn));
    bench.step();
    CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, message, 1, 2));
    bench.inject(eoma(kTool, kUs, 21, kPgn)); // before the last packet
    bench.step();
    CHECK(ends.list.empty());
    CHECK_EQ(bench.network.statistics().transport_frames_ignored, 4u);
}

TEST_CASE(rts_originator_refuses_a_second_transfer_to_the_same_node)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(9), kTool) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgn2}, payload(9), kTool) == SendStatus::Busy);
            CHECK(us.send(Pgn{kPgn2}, payload(9), 0x91) == SendStatus::Ok);
            CHECK(us.send(Pgn{0xFECA}, payload(9), 0x92) == SendStatus::Ok); // a PDU2 group to one node
        }
    };
    bench.step();
    CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 9, kPgn), rts(kUs, 0x91, 9, kPgn2), rts(kUs, 0x92, 9, 0xFECA)});
}

TEST_CASE(rts_transmit_sessions_are_limited)
{
    NetworkOptions options;
    options.rts_transmit_sessions = 2;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(9), 0x10) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgn}, payload(9), 0x11) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgn}, payload(9), 0x12) == SendStatus::QueueFull);
        }
    };
    bench.step();
    CHECK_EQ(bench.take_sent().size(), 2u);
}

TEST_CASE(rts_sends_and_receives_with_the_same_node_at_once)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    Ends ends;
    ends.attach(us);
    const Bytes ours = payload(14, 1);
    const Bytes theirs = payload(14, 2);
    send_at_start(bench, us, ours);
    bench.inject(rts(kTool, kUs, 14, kPgn));
    bench.inject(cts(kTool, kUs, 2, 1, kPgn));
    bench.step();
    CHECK_FRAMES(bench.take_sent(), std::vector<J1939Frame>{rts(kUs, kTool, 14, kPgn), cts(kUs, kTool, 2, 1, kPgn)} +
                                        dts(kUs, kTool, ours, 1, 2));
    bench.inject(dt(kTool, kUs, 1, theirs));
    bench.inject(dt(kTool, kUs, 2, theirs));
    bench.inject(eoma(kTool, kUs, 14, kPgn));
    bench.step();
    CHECK_J1939(bench.take_sent(), {eoma(kUs, kTool, 14, kPgn)});
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].data == theirs);
    CHECK(ends.list.at(0).result == TransferResult::Completed);
}

// === responses ==========================================================================

TEST_CASE(rts_long_response_goes_to_the_requester_or_by_broadcast)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    us.on_request(Pgn{0xFECA}, [](RequestContext& c) { CHECK(c.respond(payload(20)) == SendStatus::Ok); });
    bench.step();
    bench.inject(frame(6, kPgnRequest, kTool, kUs, request(0xFECA)));
    bench.step();
    CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 20, 0xFECA)});
    bench.inject(frame(6, kPgnRequest, kTool, kGlobal, request(0xFECA)));
    bench.step();
    CHECK_J1939(bench.take_sent(), {bam(kUs, 20, 0xFECA)});
}

TEST_CASE(rts_long_responses_wait_in_the_response_queue)
{
    NetworkOptions options;
    options.response_queue_capacity = 2;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    std::vector<SendStatus> results;
    us.on_any_request([&](RequestContext& c) { results.push_back(c.respond(payload(9, static_cast<std::uint8_t>(c.pgn().value())))); });
    bench.step();
    // Four destination-specific requests from the same node in one step: one transfer runs,
    // two wait, the fourth finds the queue full.
    for (const std::uint32_t pgn : {0xFE01u, 0xFE02u, 0xFE03u, 0xFE04u})
    {
        bench.inject(frame(6, kPgnRequest, kTool, kUs, request(pgn)));
    }
    bench.step();
    CHECK(results == (std::vector<SendStatus>{SendStatus::Ok, SendStatus::Ok, SendStatus::Ok, SendStatus::QueueFull}));
    CHECK_EQ(bench.network.statistics().transport_responses_dropped, 1u);
    for (const std::uint32_t pgn : {0xFE01u, 0xFE02u, 0xFE03u})
    {
        CHECK_J1939(bench.take_sent(), {rts(kUs, kTool, 9, pgn)});
        bench.inject(cts(kTool, kUs, 2, 1, pgn));
        bench.step();
        CHECK_FRAMES(bench.take_sent(), dts(kUs, kTool, payload(9, static_cast<std::uint8_t>(pgn)), 1, 2));
        bench.inject(eoma(kTool, kUs, 9, pgn));
        bench.step(); // the next response starts in the same step
    }
    CHECK_EQ(ends.list.size(), 3u);
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(rts_packets_share_the_frames_per_step_limit_with_the_data_queue)
{
    NetworkOptions options;
    options.max_frames_per_step = 5;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    const Bytes message = payload(100); // 15 packets
    send_at_start(bench, us, message);
    bench.take_sent();
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            const std::array<std::uint8_t, 1> one{1};
            CHECK(us.send(Pgn{0xFECA}, one) == SendStatus::Ok); // a single frame takes one of the 5
        }
    };
    bench.inject(cts(kTool, kUs, 15, 1, kPgn));
    std::vector<std::size_t> perStep;
    for (int i = 0; i < 5; ++i)
    {
        bench.step();
        perStep.push_back(bench.take_sent().size());
    }
    CHECK(perStep == (std::vector<std::size_t>{5, 5, 5, 1, 0}));
    CHECK(bench.sent_times.size() == 17u); // the RTS, the single frame and 15 packets
}

TEST_CASE(rts_responder_waits_for_the_end_of_the_window_before_asking_again)
{
    // Packets spread over steps: the request for the missing one comes after the window's last
    // packet, not while the originator is still sending.
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(35);
    start_receive(bench, 35);
    bench.take_sent();
    bench.inject(dt(kTool, kUs, 1, message));
    bench.inject(dt(kTool, kUs, 3, message));
    bench.step();
    CHECK_J1939(bench.take_sent(), {});
    bench.inject(dt(kTool, kUs, 4, message));
    bench.step();
    CHECK_J1939(bench.take_sent(), {});
    bench.inject(dt(kTool, kUs, 5, message));
    bench.step();
    CHECK_J1939(bench.take_sent(), {cts(kUs, kTool, 4, 2, kPgn)});
}
