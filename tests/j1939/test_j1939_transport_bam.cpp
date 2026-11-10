// SPDX-License-Identifier: MIT
//
// Broadcast transfers (BAM) of the J1939 stack against a tester that plays the remote
// nodes frame by frame, with the choices of the design: announcement-to-first-packet at least
// the interval, invalid announcements do not end a running broadcast, announcements of 8 bytes
// or fewer are ignored.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/j1939/network.hpp"

#include <array>
#include <string_view>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::unit::throws;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::TransferResult;

namespace {

constexpr Address kUs = 0x80;
constexpr Address kTool = 0x90;
constexpr Address kOther = 0x91;
constexpr std::uint32_t kPgn = 0xFECA; ///< PDU2 group sent by broadcast

/// Options without the warning about steps above 50 ms.
NetworkOptions quiet()
{
    NetworkOptions options;
    options.warning = [](std::string_view) {};
    return options;
}

/// Records the messages a node receives.
struct Inbox
{
    std::vector<Seen> seen;
    void attach(Node& node)
    {
        node.on_any_message([this](const Message& m) { seen.push_back(seen_of(m)); });
    }
};

/// Sends `message` by broadcast from `node` in the first step.
void broadcast_at_start(Bench& bench, Node& node, const Bytes& message, std::uint32_t pgn = kPgn)
{
    bench.app = [&node, message, pgn](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(node.send(Pgn{pgn}, message) == SendStatus::Ok);
        }
    };
}

/// The frames of a broadcast of `message` from `sa`.
std::vector<J1939Frame> broadcast_frames(Address sa, const Bytes& message, std::uint32_t pgn = kPgn)
{
    return std::vector<J1939Frame>{bam(sa, message.size(), pgn)} + dts(sa, kGlobal, message, 1, packet_count(message.size()));
}

void check_sends(std::size_t length)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    const Bytes message = payload(length);
    broadcast_at_start(bench, us, message);
    const int packets = packet_count(length);
    bench.steps(5 * packets + 2);
    CHECK_FRAMES(bench.take_sent(), broadcast_frames(kUs, message));
    // Announcement at 0, then one packet every 50 ms.
    for (int k = 0; k <= packets; ++k)
    {
        CHECK(bench.sent_times[static_cast<std::size_t>(k)] == at(std::chrono::milliseconds(50 * k)));
    }
    CHECK_EQ(ends.list.size(), 1u);
    CHECK(ends.list[0].result == TransferResult::Completed);
    CHECK_EQ(ends.list[0].destination, kGlobal);
    CHECK_EQ(ends.list[0].length, length);
    CHECK(ends.list[0].time == at(std::chrono::milliseconds(50 * packets)));
    CHECK_EQ(bench.network.statistics().transport_messages_sent, 1u);
}

/// Injects a broadcast of `message` from kTool: announcement at `start`, packet k at start + k * gap.
void inject_broadcast(Bench& bench, const Bytes& message, SimTime start, SimDuration gap, std::uint32_t pgn = kPgn)
{
    bench.inject(start, bam(kTool, message.size(), pgn));
    for (int k = 1; k <= packet_count(message.size()); ++k)
    {
        bench.inject(start + gap * k, dt(kTool, kGlobal, static_cast<std::uint8_t>(k), message));
    }
}

void check_receives(std::size_t length)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(length, 3);
    inject_broadcast(bench, message, at(0ms), 50ms);
    bench.steps(5 * packet_count(length) + 3);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0] == (Seen{kPgn, kTool, kGlobal, 7, at(std::chrono::milliseconds(50 * packet_count(length))), message}));
    CHECK_EQ(bench.network.statistics().transport_messages_received, 1u);
    CHECK_J1939(bench.take_sent(), {}); // no acknowledgement or abort for a broadcast
}

} // namespace

// --- sending ------------------------------------------------------------------------------

TEST_CASE(bam_sends_the_minimum_message_of_9_bytes) { check_sends(9); }
TEST_CASE(bam_sends_messages_at_the_packet_count_edges)
{
    check_sends(14);
    check_sends(15);
}
TEST_CASE(bam_sends_the_maximum_message_of_1785_bytes) { check_sends(1785); }

TEST_CASE(bam_sends_up_to_8_bytes_as_one_frame)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            const Bytes eight = payload(8);
            CHECK(us.send(Pgn{kPgn}, eight) == SendStatus::Ok);
        }
    };
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {frame(6, kPgn, kUs, kGlobal, payload(8))});
}

TEST_CASE(bam_packets_are_at_least_the_interval_apart_with_coarse_steps)
{
    // 30 ms steps: 50 ms rounds up to 60 ms, also between the announcement and the first packet.
    Bench bench({}, 30ms);
    Node& us = bench.add_node(kUs);
    broadcast_at_start(bench, us, payload(20));
    bench.steps(12);
    CHECK_FRAMES(bench.take_sent(), broadcast_frames(kUs, payload(20)));
    CHECK(bench.sent_times == (std::vector<SimTime>{at(0ms), at(60ms), at(120ms), at(180ms)}));
}

TEST_CASE(bam_interval_is_configurable)
{
    NetworkOptions options;
    options.bam_packet_interval = 100ms;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    broadcast_at_start(bench, us, payload(9));
    bench.steps(22);
    CHECK_FRAMES(bench.take_sent(), broadcast_frames(kUs, payload(9)));
    CHECK(bench.sent_times == (std::vector<SimTime>{at(0ms), at(100ms), at(200ms)}));
    NetworkOptions tooShort;
    tooShort.bam_packet_interval = 9ms;
    CHECK(throws<std::invalid_argument>([&] { vehicle_sil::j1939::Network n(tooShort); }));
    NetworkOptions tooLong;
    tooLong.bam_packet_interval = 201ms;
    CHECK(throws<std::invalid_argument>([&] { vehicle_sil::j1939::Network n(tooLong); }));
}

TEST_CASE(bam_send_refuses_a_second_broadcast_while_one_is_running)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    std::vector<SendStatus> results;
    bench.app = [&](SimTime now) {
        if (now == at(0ms) || now == at(50ms) || now == at(110ms))
        {
            results.push_back(us.send(Pgn{kPgn}, payload(9)));
        }
    };
    bench.steps(25);
    // The first broadcast ends at 100 ms (reported at the next step): the third one starts.
    CHECK(results == (std::vector<SendStatus>{SendStatus::Ok, SendStatus::Busy, SendStatus::Ok}));
    CHECK_EQ(ends.list.size(), 2u);
}

TEST_CASE(bam_send_runs_beside_a_connection_and_broadcasts_of_other_nodes)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Node& second = bench.add_node(0x81);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(9)) == SendStatus::Ok);
            CHECK(second.send(Pgn{kPgn}, payload(9, 1)) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgn}, payload(9, 2), kTool) == SendStatus::Ok); // RTS/CTS beside it
        }
    };
    bench.steps(12);
    // Control frames in queue order, then the packets in start order.
    CHECK_J1939(bench.take_sent(),
                {bam(kUs, 9, kPgn), bam(0x81, 9, kPgn), rts(kUs, kTool, 9, kPgn), dt(kUs, kGlobal, 1, payload(9)),
                 dt(0x81, kGlobal, 1, payload(9, 1)), dt(kUs, kGlobal, 2, payload(9)), dt(0x81, kGlobal, 2, payload(9, 1))});
}

TEST_CASE(bam_send_ignores_an_abort_for_its_broadcast)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Ends ends;
    ends.attach(us);
    broadcast_at_start(bench, us, payload(9));
    bench.steps(6);
    bench.inject(conn_abort(kTool, kUs, 3, kPgn));
    bench.steps(10);
    CHECK_FRAMES(bench.take_sent(), broadcast_frames(kUs, payload(9)));
    CHECK_EQ(ends.list.size(), 1u);
    CHECK(ends.list[0].result == TransferResult::Completed);
    CHECK_EQ(bench.network.statistics().transport_frames_ignored, 1u);
}

TEST_CASE(bam_send_rejects_more_than_1785_bytes)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.app = [&](SimTime) { CHECK(us.send(Pgn{kPgn}, payload(1786)) == SendStatus::TooLong); };
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(bam_send_without_a_free_session_is_refused)
{
    NetworkOptions options;
    options.bam_transmit_sessions = 1;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    Node& second = bench.add_node(0x81);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgn}, payload(9)) == SendStatus::Ok);
            CHECK(second.send(Pgn{kPgn}, payload(9)) == SendStatus::QueueFull);
        }
        if (now == at(110ms))
        {
            CHECK(second.send(Pgn{kPgn}, payload(9)) == SendStatus::Ok); // freed at the end of the first
        }
    };
    bench.steps(25);
    CHECK_EQ(bench.network.statistics().transport_messages_sent, 2u);
}

// --- receiving ----------------------------------------------------------------------------

TEST_CASE(bam_receives_the_minimum_message_of_9_bytes) { check_receives(9); }
TEST_CASE(bam_receives_messages_at_the_packet_count_edges)
{
    check_receives(14);
    check_receives(15);
}
TEST_CASE(bam_receives_the_maximum_message_of_1785_bytes) { check_receives(1785); }

TEST_CASE(bam_receives_packets_sent_back_to_back)
{
    // No lower bound on the interval when receiving; packets with the same time keep their order.
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    inject_broadcast(bench, payload(30), at(1ms), 0ms);
    bench.steps(2);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].data == payload(30));
}

TEST_CASE(bam_receives_packets_up_to_t1_apart)
{
    // T1 = 750 ms between packets: a packet exactly T1 after the previous one is taken.
    for (const SimDuration step : {1ms, 7ms, 10ms, 50ms, 200ms})
    {
        Bench bench(quiet(), step);
        Node& us = bench.add_node(kUs);
        Inbox inbox;
        inbox.attach(us);
        inject_broadcast(bench, payload(9), at(0ms), 750ms);
        bench.steps(static_cast<int>(1600ms / step) + 2);
        CHECK_EQ(inbox.seen.size(), 1u);
        CHECK_EQ(bench.network.statistics().transport_timeouts, 0u);
    }
}

TEST_CASE(bam_receive_times_out_t1_after_the_last_packet)
{
    for (const SimDuration step : {1ms, 7ms, 10ms, 50ms, 200ms})
    {
        Bench bench(quiet(), step);
        Node& us = bench.add_node(kUs);
        Inbox inbox;
        inbox.attach(us);
        const Bytes message = payload(9);
        bench.inject(at(0ms), bam(kTool, 9, kPgn));
        bench.inject(at(10ms), dt(kTool, kGlobal, 1, message));
        bench.inject(at(760ms) + 1ns, dt(kTool, kGlobal, 2, message)); // just after T1
        bench.steps(static_cast<int>(1000ms / step) + 2);
        CHECK(inbox.seen.empty());
        CHECK_EQ(bench.network.statistics().transport_timeouts, 1u);
        CHECK_J1939(bench.take_sent(), {}); // no abort for a broadcast
    }
}

TEST_CASE(bam_receive_times_out_t1_after_the_announcement)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(9);
    bench.inject(at(0ms), bam(kTool, 9, kPgn));
    bench.inject(at(751ms), dt(kTool, kGlobal, 1, message));
    bench.inject(at(760ms), dt(kTool, kGlobal, 2, message));
    bench.steps(100);
    CHECK(inbox.seen.empty());
    CHECK_EQ(bench.network.statistics().transport_timeouts, 1u);
}

namespace {

/// A broadcast of 21 bytes (3 packets) whose packets arrive as `sequences`; nothing is delivered.
void check_dropped(const std::vector<int>& sequences)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(21);
    bench.inject(at(0ms), bam(kTool, 21, kPgn));
    int k = 1;
    for (const int s : sequences)
    {
        bench.inject(at(std::chrono::milliseconds(50 * k++)), dt(kTool, kGlobal, static_cast<std::uint8_t>(s), message));
    }
    bench.steps(100);
    CHECK(inbox.seen.empty());
    CHECK_EQ(bench.network.statistics().transport_bam_dropped, 1u);
    CHECK_J1939(bench.take_sent(), {});
}

} // namespace

TEST_CASE(bam_lost_packet_drops_the_message) { check_dropped({1, 3}); }
TEST_CASE(bam_out_of_order_packets_drop_the_message) { check_dropped({2, 1, 3}); }
TEST_CASE(bam_duplicate_packet_drops_the_message) { check_dropped({1, 1, 2, 3}); }
TEST_CASE(bam_sequence_number_zero_drops_the_message) { check_dropped({0, 1, 2, 3}); }

TEST_CASE(bam_extra_packet_after_the_last_one_is_ignored)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(9);
    inject_broadcast(bench, message, at(0ms), 50ms);
    bench.inject(at(150ms), dt(kTool, kGlobal, 3, message));
    bench.steps(30);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK_EQ(bench.network.statistics().transport_frames_ignored, 1u);
}

TEST_CASE(bam_second_announcement_from_the_same_source_replaces_the_first)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes first = payload(21);
    const Bytes second = payload(9, 5);
    bench.inject(at(0ms), bam(kTool, 21, kPgn));
    bench.inject(at(50ms), dt(kTool, kGlobal, 1, first));
    inject_broadcast(bench, second, at(100ms), 50ms, 0xFECB);
    bench.inject(at(300ms), dt(kTool, kGlobal, 2, first)); // the first one is gone
    bench.steps(40);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].pgn == 0xFECB);
    CHECK(inbox.seen[0].data == second);
}

TEST_CASE(bam_invalid_announcement_does_not_end_the_running_one)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(21);
    bench.inject(at(0ms), bam(kTool, 21, kPgn));
    bench.inject(at(50ms), dt(kTool, kGlobal, 1, message));
    bench.inject(at(60ms), bam(kTool, 21, kPgn, 4));   // wrong packet count
    bench.inject(at(70ms), bam(kTool, 1786, kPgn));    // too long
    bench.inject(at(80ms), bam(kTool, 8, kPgn));       // fits in one frame
    bench.inject(at(100ms), dt(kTool, kGlobal, 2, message));
    bench.inject(at(150ms), dt(kTool, kGlobal, 3, message));
    bench.steps(30);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].data == message);
    CHECK_EQ(bench.network.statistics().transport_rejected, 3u);
}

TEST_CASE(bam_invalid_announcements_are_ignored)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(9);
    bench.inject(at(0ms), bam(kTool, 9, kPgn, 3));      // inconsistent packet count
    bench.inject(at(0ms), bam(kTool, 1786, kPgn, 255)); // above 1785
    bench.inject(at(0ms), bam(kTool, 8, kPgn, 2));      // 8 bytes or fewer
    bench.inject(at(0ms), bam(kTool, 9, 0xEF01));       // a PDU1 PGN with PS set
    bench.inject(at(0ms), bam(kTool, 9, 0x40000));      // above 18 bits
    bench.inject(at(1ms), dt(kTool, kGlobal, 1, message));
    bench.inject(at(2ms), dt(kTool, kGlobal, 2, message));
    bench.steps(10);
    CHECK(inbox.seen.empty());
    CHECK_EQ(bench.network.statistics().transport_rejected, 5u);
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(bam_to_a_specific_address_is_ignored)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(9);
    bench.inject(at(0ms), bam(kTool, 9, kPgn, -1, kUs));
    bench.inject(at(50ms), dt(kTool, kGlobal, 1, message));
    bench.inject(at(100ms), dt(kTool, kGlobal, 2, message));
    bench.steps(20);
    CHECK(inbox.seen.empty());
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(bam_from_a_sender_we_have_not_seen_claim_is_received)
{
    // Our node claims; the tester never does. Its broadcast is still taken.
    Bench bench;
    Node& us = bench.network.add_node({kUs, Name{0x1234}});
    Inbox inbox;
    inbox.attach(us);
    inject_broadcast(bench, payload(9), at(0ms), 50ms);
    bench.steps(20);
    CHECK_EQ(inbox.seen.size(), 1u);
}

TEST_CASE(bam_transport_frames_are_not_handed_to_the_handlers)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    bench.inject(at(0ms), bam(kTool, 9, kPgn));
    bench.inject(at(1ms), dt(kTool, kGlobal, 1, payload(9)));
    bench.steps(3);
    CHECK(inbox.seen.empty()); // only the complete message is
}

TEST_CASE(bam_receives_one_broadcast_per_sender_at_once)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    inject_broadcast(bench, payload(20, 1), at(0ms), 50ms);
    bench.inject(at(1ms), bam(kOther, 9, 0xFECB));
    bench.inject(at(51ms), dt(kOther, kGlobal, 1, payload(9, 2)));
    bench.inject(at(101ms), dt(kOther, kGlobal, 2, payload(9, 2)));
    bench.steps(30);
    CHECK_EQ(inbox.seen.size(), 2u);
    CHECK(inbox.seen[0].sa == kOther); // completed first (101 ms)
    CHECK(inbox.seen[1].sa == kTool);  // 150 ms
}

TEST_CASE(bam_receive_sessions_are_limited)
{
    NetworkOptions options;
    options.bam_receive_sessions = 2;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    for (Address sa = 0x10; sa < 0x13; ++sa)
    {
        bench.inject(at(0ms), bam(sa, 9, kPgn));
        bench.inject(at(50ms), dt(sa, kGlobal, 1, payload(9, sa)));
        bench.inject(at(100ms), dt(sa, kGlobal, 2, payload(9, sa)));
    }
    bench.steps(20);
    CHECK_EQ(inbox.seen.size(), 2u); // the third sender found no free session
    CHECK_EQ(bench.network.statistics().transport_rejected, 1u);
    // Sessions are free again afterwards.
    inject_broadcast(bench, payload(9), bench.last_time(), 10ms);
    bench.steps(5);
    CHECK_EQ(inbox.seen.size(), 3u);
}

TEST_CASE(bam_short_packet_counts_as_missing)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(10); // packet 2 carries 3 bytes
    bench.inject(at(0ms), bam(kTool, 10, kPgn));
    bench.inject(at(10ms), dt(kTool, kGlobal, 1, message));
    J1939Frame shortLast = frame(7, kPgnTpDt, kTool, kGlobal, {2, message[7], message[8], message[9]}); // all it needs
    bench.inject(at(20ms), shortLast);
    bench.steps(5);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK(inbox.seen[0].data == message);
    // A packet without all its bytes is not taken; the broadcast then misses it.
    bench.inject(bam(kTool, 10, kPgn));
    bench.inject(dt(kTool, kGlobal, 1, message));
    bench.inject(frame(7, kPgnTpDt, kTool, kGlobal, {2, message[7], message[8]}));
    bench.steps(100);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK_EQ(bench.network.statistics().transport_timeouts, 1u);
}

TEST_CASE(bam_packets_with_another_priority_are_taken)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Inbox inbox;
    inbox.attach(us);
    const Bytes message = payload(9);
    J1939Frame announcement = bam(kTool, 9, kPgn);
    announcement.priority = 3;
    J1939Frame first = dt(kTool, kGlobal, 1, message);
    first.priority = 0;
    bench.inject(at(0ms), announcement);
    bench.inject(at(1ms), first);
    bench.inject(at(2ms), dt(kTool, kGlobal, 2, message));
    bench.steps(2);
    CHECK_EQ(inbox.seen.size(), 1u);
    CHECK_EQ(inbox.seen[0].priority, 3); // the announcement's priority
}

TEST_CASE(bam_between_local_nodes_reaches_the_other_nodes_only)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Node& second = bench.add_node(0x81);
    Inbox ours;
    Inbox theirs;
    ours.attach(us);
    theirs.attach(second);
    broadcast_at_start(bench, us, payload(30));
    bench.steps(30);
    CHECK(ours.seen.empty());
    CHECK_EQ(theirs.seen.size(), 1u);
    CHECK(theirs.seen[0].data == payload(30));
    CHECK(theirs.seen[0].time == at(250ms)); // the last packet (5 packets, 50 ms apart)
}
