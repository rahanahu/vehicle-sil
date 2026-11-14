// SPDX-License-Identifier: MIT
//
// TransferSniffer on frames written out by hand: what it reports for connections (RTS/CTS)
// and broadcasts (BAM) between other ECUs, in which order it decides (timeouts, a second
// announcement, an acknowledgement before the last packet), and what it counts. That it agrees
// with the engine's receiver on the same frames is in test_j1939_sniffer_faults.cpp.
#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/j1939/transfer_sniffer.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimTime;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::SniffDelivery;
using vehicle_sil::j1939::SniffedTransfer;
using vehicle_sil::j1939::SniffEnd;
using vehicle_sil::j1939::SnifferOptions;
using vehicle_sil::j1939::SnifferStatistics;
using vehicle_sil::j1939::SniffFrame;
using vehicle_sil::j1939::SniffSecondAnnouncement;
using vehicle_sil::j1939::TransferSniffer;
using vehicle_sil::unit::throws_with;

namespace {

constexpr Address kA = 0x10; // sends
constexpr Address kB = 0x20; // receives
constexpr Address kC = 0x30;
constexpr std::uint32_t kPgn = 0xEF00;
constexpr std::uint32_t kOther = 0xD300;
constexpr std::uint8_t kTimeoutReason = 3;
constexpr std::uint8_t kBusyReason = 1;

struct Heard
{
    SniffEnd end;
    std::uint32_t pgn;
    std::uint8_t sa;
    std::uint8_t da;
    std::size_t size;
    SimTime started;
    SimTime time;
    SimTime noticed;
    std::uint8_t reason;
    std::uint8_t priority;
    Bytes data;
};

struct Sniffer
{
    explicit Sniffer(SnifferOptions options = {}) : sniffer(options)
    {
        sniffer.on_transfer([this](const SniffedTransfer& t) {
            heard.push_back(Heard{t.end, t.pgn.value(), t.source, t.destination, t.size, t.started, t.time, t.noticed,
                                  t.reason, t.priority, Bytes(t.data.begin(), t.data.end())});
        });
    }
    void feed(SimTime time, const J1939Frame& f) { sniffer.feed(TimedFrame{time, to_can(f)}); }
    void feed(SimTime time, const std::vector<J1939Frame>& frames)
    {
        for (const auto& f : frames)
        {
            feed(time, f);
        }
    }
    [[nodiscard]] const SnifferStatistics& stats() const { return sniffer.statistics(); }
    [[nodiscard]] std::vector<SniffEnd> ends() const
    {
        std::vector<SniffEnd> out;
        for (const auto& h : heard)
        {
            out.push_back(h.end);
        }
        return out;
    }

    TransferSniffer sniffer;
    std::vector<Heard> heard;
};

using EndList = std::vector<SniffEnd>;

SnifferOptions with_broadcasts()
{
    SnifferOptions options;
    options.broadcasts = true;
    return options;
}

/// A to B: RTS at 0 ms, CTS at 10 ms.
void open(Sniffer& s, std::size_t size, std::uint32_t pgn = kPgn)
{
    s.feed(at(0ms), rts(kA, kB, size, pgn));
    s.feed(at(10ms), cts(kB, kA, packet_count(size), 1, pgn));
}

} // namespace

// --- a connection from its first frame to its last --------------------------------------------

TEST_CASE(sniffer_reports_a_connection_when_the_receiver_acknowledges)
{
    Sniffer s;
    const Bytes message = payload(20);
    J1939Frame announcement = rts(kA, kB, 20, kPgn);
    announcement.priority = 5; // the message has the priority of its announcement
    s.feed(at(0ms), announcement);
    s.feed(at(10ms), cts(kB, kA, 3, 1, kPgn));
    s.feed(at(20ms), dt(kA, kB, 1, message));
    s.feed(at(21ms), dt(kA, kB, 2, message));
    s.feed(at(22ms), dt(kA, kB, 3, message));
    CHECK(s.heard.empty());
    CHECK_EQ(s.sniffer.active(), 1u);
    s.feed(at(30ms), eoma(kB, kA, 20, kPgn));
    CHECK_EQ(s.heard.size(), 1u);
    const Heard& h = s.heard[0];
    CHECK(h.end == SniffEnd::Completed);
    CHECK_EQ(h.pgn, kPgn);
    CHECK_EQ(h.sa, kA);
    CHECK_EQ(h.da, kB);
    CHECK_EQ(h.priority, 5u);
    CHECK_EQ(h.size, 20u);
    CHECK(h.data == message);
    CHECK(h.started == at(0ms));
    CHECK(h.time == at(22ms)); // the last data packet, as the receiver's message
    CHECK(h.noticed == at(30ms));
    CHECK_EQ(h.reason, 0u);
    CHECK_EQ(s.sniffer.active(), 0u);
    SnifferStatistics expected;
    expected.started = 1;
    expected.completed = 1;
    CHECK(s.stats() == expected);
}

TEST_CASE(sniffer_transfer_reads_as_a_message)
{
    TransferSniffer sniffer;
    std::vector<Seen> seen;
    sniffer.on_transfer([&](const SniffedTransfer& t) {
        const Message m = t.message();
        seen.push_back(seen_of(m));
    });
    const Bytes message = payload(9);
    const auto feed = [&](SimTime time, const J1939Frame& f) { sniffer.feed(TimedFrame{time, to_can(f)}); };
    feed(at(0ms), rts(kA, kB, 9, kPgn));
    feed(at(1ms), cts(kB, kA, 2, 1, kPgn));
    feed(at(2ms), dt(kA, kB, 1, message));
    feed(at(3ms), dt(kA, kB, 2, message));
    feed(at(4ms), eoma(kB, kA, 9, kPgn));
    CHECK(seen == (std::vector<Seen>{Seen{kPgn, kA, kB, 7, at(3ms), message}}));
}

TEST_CASE(sniffer_takes_decoded_frames_and_raw_frames_alike)
{
    const Bytes message = payload(9);
    const std::vector<std::pair<SimTime, J1939Frame>> frames{{at(0ms), rts(kA, kB, 9, kPgn)},
                                                            {at(1ms), cts(kB, kA, 2, 1, kPgn)},
                                                            {at(2ms), dt(kA, kB, 1, message)},
                                                            {at(3ms), dt(kA, kB, 2, message)},
                                                            {at(4ms), eoma(kB, kA, 9, kPgn)}};
    TransferSniffer sniffer;
    std::vector<Bytes> got;
    sniffer.on_transfer([&](const SniffedTransfer& t) { got.emplace_back(t.data.begin(), t.data.end()); });
    for (const auto& [time, f] : frames)
    {
        sniffer.feed(SniffFrame{time, Pgn{f.pgn}, f.priority, f.sa, f.da, f.data});
    }
    CHECK(got == (std::vector<Bytes>{message}));
    // A group that is not of the transport protocol and an 11-bit frame only move the time.
    Sniffer s;
    s.feed(at(0ms), rts(kA, kB, 9, kPgn));
    s.feed(at(1400ms), frame(6, 0xFEF1, kC, kGlobal, Bytes(8, 0)));
    CHECK(s.heard.empty());
    vehicle_sil::can::Frame standard;
    standard.id = 0x123;
    standard.format = vehicle_sil::can::IdFormat::Standard;
    standard.length = 8;
    standard.data = {16, 9, 0, 2, 0xFF, 0x00, 0xEF, 0x00}; // would be an RTS in a 29-bit frame
    s.sniffer.feed(TimedFrame{at(1501ms), standard});
    CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
    CHECK_EQ(s.stats().started, 1u);
}

TEST_CASE(sniffer_stores_packets_by_number_and_keeps_the_first_of_a_repeated_one)
{
    Sniffer s;
    const Bytes message = payload(30); // 5 packets
    Bytes changed = message;
    changed[8] ^= 0xFF; // in packet 2
    open(s, 30);
    s.feed(at(20ms), dt(kA, kB, 4, message));
    s.feed(at(21ms), dt(kA, kB, 2, message));
    s.feed(at(22ms), dt(kA, kB, 2, message)); // the same again
    s.feed(at(23ms), dt(kA, kB, 2, changed)); // other bytes: the first one stays
    s.feed(at(24ms), dt(kA, kB, 5, message));
    s.feed(at(25ms), dt(kA, kB, 1, message));
    s.feed(at(26ms), dt(kA, kB, 3, message));
    s.feed(at(40ms), eoma(kB, kA, 30, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK(s.heard[0].data == message);
    CHECK_EQ(s.stats().repeated_packets, 2u);
    CHECK_EQ(s.stats().conflicting_packets, 1u);
    CHECK_EQ(s.stats().bad_packets, 0u);
}

TEST_CASE(sniffer_counts_packets_it_cannot_store)
{
    Sniffer s;
    const Bytes message = payload(16); // 3 packets, the last with 2 bytes
    open(s, 16);
    s.feed(at(20ms), dt(kA, kB, 0, message));
    s.feed(at(20ms), dt(kA, kB, 4, message));
    J1939Frame shortPacket = dt(kA, kB, 1, message);
    shortPacket.data.resize(5);
    s.feed(at(20ms), shortPacket);
    s.feed(at(20ms), frame(7, kPgnTpDt, kA, kB, {}));
    CHECK_EQ(s.stats().bad_packets, 4u);
    // The last packet needs only the bytes of the message.
    J1939Frame last = dt(kA, kB, 3, message);
    last.data.resize(3);
    s.feed(at(21ms), dts(kA, kB, message, 1, 2));
    s.feed(at(22ms), last);
    s.feed(at(30ms), eoma(kB, kA, 16, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK(s.heard[0].data == message);
    CHECK_EQ(s.stats().bad_packets, 4u);
}

TEST_CASE(sniffer_time_of_a_retransmitted_message_is_the_last_packet_the_receiver_asked_for)
{
    // Packet 3 is lost; the receiver asks again from it at the end of the window, and has the
    // message with the second packet 5.
    Sniffer s;
    const Bytes message = payload(30);
    open(s, 30);
    s.feed(at(20ms), dts(kA, kB, message, 1, 2));
    s.feed(at(20ms), dts(kA, kB, message, 4, 5));
    s.feed(at(30ms), cts(kB, kA, 3, 3, kPgn));
    s.feed(at(40ms), dt(kA, kB, 3, message)); // the last gap
    s.feed(at(41ms), dt(kA, kB, 4, message));
    s.feed(at(42ms), dt(kA, kB, 5, message));
    s.feed(at(50ms), eoma(kB, kA, 30, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK(s.heard[0].time == at(42ms));
    CHECK(s.heard[0].data == message);
    CHECK_EQ(s.stats().retransmit_requests, 1u);
    CHECK_EQ(s.stats().repeated_packets, 2u);
}

TEST_CASE(sniffer_time_does_not_move_with_a_packet_repeated_after_the_last_one)
{
    Sniffer s;
    const Bytes message = payload(14);
    open(s, 14);
    s.feed(at(20ms), dt(kA, kB, 1, message));
    s.feed(at(21ms), dt(kA, kB, 2, message)); // the receiver has the message here
    s.feed(at(25ms), dt(kA, kB, 2, message));
    s.feed(at(30ms), eoma(kB, kA, 14, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK(s.heard[0].time == at(21ms));
}

TEST_CASE(sniffer_time_for_a_receiver_that_asks_for_single_packets)
{
    // Another stack may ask for the missing packet only: it has the message with that packet.
    Sniffer s;
    const Bytes message = payload(30);
    open(s, 30);
    s.feed(at(20ms), dts(kA, kB, message, 1, 2));
    s.feed(at(20ms), dts(kA, kB, message, 4, 5));
    s.feed(at(30ms), cts(kB, kA, 1, 3, kPgn));
    s.feed(at(40ms), dt(kA, kB, 3, message));
    s.feed(at(50ms), eoma(kB, kA, 30, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK(s.heard[0].time == at(40ms));
}

TEST_CASE(sniffer_with_all_packets_seen_does_not_wait_for_the_acknowledgement)
{
    SnifferOptions options;
    options.delivery = SniffDelivery::WhenAllPacketsSeen;
    Sniffer s(options);
    const Bytes message = payload(20);
    open(s, 20);
    s.feed(at(20ms), dt(kA, kB, 3, message));
    s.feed(at(21ms), dt(kA, kB, 1, message));
    CHECK(s.heard.empty());
    s.feed(at(22ms), dt(kA, kB, 2, message));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK(s.heard[0].time == at(22ms));
    CHECK(s.heard[0].data == message);
    // What follows finds no transfer.
    s.feed(at(23ms), dt(kA, kB, 2, message));
    s.feed(at(30ms), eoma(kB, kA, 20, kPgn));
    CHECK_EQ(s.stats().orphan_data, 1u);
    CHECK_EQ(s.stats().orphan_control, 1u);
    CHECK_EQ(s.heard.size(), 1u);
}

// --- aborts -------------------------------------------------------------------------------------

TEST_CASE(sniffer_ends_a_transfer_on_an_abort_from_either_side)
{
    Sniffer s;
    const Bytes message = payload(30);
    open(s, 30);
    s.feed(at(20ms), dt(kA, kB, 1, message));
    s.feed(at(25ms), conn_abort(kB, kA, 5, kOther)); // another group: not this transfer
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().orphan_control, 1u);
    s.feed(at(30ms), conn_abort(kB, kA, 5, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::AbortedByReceiver}));
    CHECK_EQ(s.heard[0].reason, 5u);
    CHECK(s.heard[0].time == at(30ms));
    CHECK(s.heard[0].data.empty());

    s.feed(at(100ms), rts(kA, kB, 30, kPgn));
    s.feed(at(110ms), conn_abort(kA, kB, 250, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::AbortedByReceiver, SniffEnd::AbortedBySender}));
    CHECK_EQ(s.heard[1].reason, 250u);
    CHECK(s.heard[1].started == at(100ms));
    CHECK_EQ(s.stats().aborted_by_receiver, 1u);
    CHECK_EQ(s.stats().aborted_by_sender, 1u);
    CHECK_EQ(s.stats().orphan_control, 1u);
    CHECK_EQ(s.sniffer.active(), 0u);
}

TEST_CASE(sniffer_abort_between_two_ecus_that_send_to_each_other_ends_the_transfer_to_its_sender)
{
    // A to B and B to A, the same group. An abort from B can mean either; the transfer B
    // receives is ended first (not known from the standard).
    Sniffer s;
    s.feed(at(0ms), rts(kA, kB, 30, kPgn));
    s.feed(at(1ms), rts(kB, kA, 40, kPgn));
    s.feed(at(10ms), conn_abort(kB, kA, 2, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::AbortedByReceiver}));
    CHECK_EQ(s.heard[0].sa, kA);
    s.feed(at(11ms), conn_abort(kB, kA, 2, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::AbortedByReceiver, SniffEnd::AbortedBySender}));
    CHECK_EQ(s.heard[1].sa, kB);
}

// --- timeouts (no timer: the time comes with the frames and with advance()) --------------------

TEST_CASE(sniffer_times_out_a_connection_after_the_timeout_and_the_grace)
{
    Sniffer s;
    s.feed(at(100ms), rts(kA, kB, 30, kPgn));
    s.sniffer.advance(at(100ms + 1250ms + 250ms)); // exactly: still followed
    CHECK(s.heard.empty());
    CHECK_EQ(s.sniffer.active(), 1u);
    s.sniffer.advance(at(100ms + 1250ms + 250ms + 1ns));
    CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
    CHECK(s.heard[0].time == at(100ms + 1250ms)); // without the grace, whatever the step
    CHECK(s.heard[0].noticed == at(100ms + 1250ms + 250ms + 1ns));
    CHECK_EQ(s.stats().timed_out, 1u);
    CHECK_EQ(s.sniffer.active(), 0u);
    // An earlier time does nothing, and nothing is reported twice.
    s.sniffer.advance(at(0ms));
    s.sniffer.advance(at(10s));
    CHECK_EQ(s.heard.size(), 1u);
}

TEST_CASE(sniffer_counts_the_time_from_the_last_frame_of_the_transfer)
{
    Sniffer s;
    const Bytes message = payload(30);
    s.feed(at(0ms), rts(kA, kB, 30, kPgn));
    s.feed(at(1000ms), cts(kB, kA, 5, 1, kPgn));
    s.feed(at(2000ms), dt(kA, kB, 1, message));
    s.feed(at(3000ms), cts(kB, kA, 0, 0xFF, kPgn)); // holds the connection
    s.feed(at(4000ms), dt(kA, kB, 9, message));     // not a packet of the message, but a sign of life
    s.feed(at(5500ms), dt(kA, kB, 2, message));     // exactly at the timeout and the grace
    CHECK(s.heard.empty());
    s.feed(at(7001ms), dt(kA, kB, 3, message));
    CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
    CHECK(s.heard[0].time == at(5500ms + 1250ms));
    CHECK_EQ(s.stats().orphan_data, 1u); // the packet that came too late
}

TEST_CASE(sniffer_abort_within_the_grace_ends_the_transfer_as_aborted)
{
    // The ECU that gives up sends its abort after its own timeout: later than the timeout here.
    Sniffer s;
    s.feed(at(0ms), rts(kA, kB, 30, kPgn));
    s.feed(at(1260ms), conn_abort(kA, kB, kTimeoutReason, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::AbortedBySender}));
    CHECK_EQ(s.heard[0].reason, kTimeoutReason);
    CHECK(s.heard[0].time == at(1260ms));
    CHECK_EQ(s.stats().orphan_control, 0u);
    CHECK_EQ(s.stats().timed_out, 0u);
    // The same when the listener's step ends between the timeout and the abort.
    Sniffer stepped;
    stepped.feed(at(0ms), rts(kA, kB, 30, kPgn));
    stepped.sniffer.advance(at(1255ms));
    stepped.feed(at(1260ms), conn_abort(kA, kB, kTimeoutReason, kPgn));
    CHECK(stepped.ends() == (EndList{SniffEnd::AbortedBySender}));
    CHECK_EQ(stepped.stats().orphan_control, 0u);
}

TEST_CASE(sniffer_abort_after_the_grace_finds_no_transfer_whatever_the_steps)
{
    for (const bool stepped : {false, true})
    {
        Sniffer s;
        s.feed(at(0ms), rts(kA, kB, 30, kPgn));
        if (stepped)
        {
            s.sniffer.advance(at(1400ms));
            s.sniffer.advance(at(1600ms));
        }
        s.feed(at(1700ms), conn_abort(kA, kB, kTimeoutReason, kPgn));
        CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
        CHECK(s.heard[0].time == at(1250ms));
        CHECK_EQ(s.stats().orphan_control, 1u);
    }
    // A longer grace covers it.
    SnifferOptions options;
    options.grace = 500ms;
    Sniffer s(options);
    s.feed(at(0ms), rts(kA, kB, 30, kPgn));
    s.sniffer.advance(at(1600ms));
    s.feed(at(1700ms), conn_abort(kA, kB, kTimeoutReason, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::AbortedBySender}));
}

TEST_CASE(sniffer_timeouts_can_be_changed)
{
    SnifferOptions options;
    options.timeout = 100ms;
    options.grace = 0ms;
    options.broadcast_timeout = 50ms;
    options.broadcasts = true;
    Sniffer s(options);
    s.feed(at(0ms), rts(kA, kB, 30, kPgn));
    s.feed(at(0ms), bam(kC, 30, kOther));
    s.sniffer.advance(at(50ms));
    CHECK(s.heard.empty());
    s.sniffer.advance(at(51ms));
    CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
    CHECK_EQ(s.heard[0].sa, kC);
    CHECK(s.heard[0].time == at(50ms));
    s.sniffer.advance(at(100ms));
    CHECK_EQ(s.heard.size(), 1u);
    s.sniffer.advance(at(101ms));
    CHECK_EQ(s.heard.size(), 2u);
    CHECK(s.heard[1].time == at(100ms));
}

TEST_CASE(sniffer_broadcast_packet_exactly_at_the_timeout_is_taken_and_the_next_instant_is_not)
{
    const Bytes message = payload(14);
    {
        Sniffer s(with_broadcasts());
        s.feed(at(0ms), bam(kA, 14, kPgn));
        s.feed(at(750ms), dt(kA, kGlobal, 1, message));
        s.feed(at(1500ms), dt(kA, kGlobal, 2, message));
        CHECK(s.ends() == (EndList{SniffEnd::Completed}));
        CHECK(s.heard[0].data == message);
        CHECK(s.heard[0].time == at(1500ms));
        CHECK_EQ(s.heard[0].da, kGlobal);
    }
    {
        Sniffer s(with_broadcasts());
        s.feed(at(0ms), bam(kA, 14, kPgn));
        s.feed(at(750ms), dt(kA, kGlobal, 1, message));
        s.feed(at(1500ms + 1ns), dt(kA, kGlobal, 2, message));
        CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
        CHECK(s.heard[0].time == at(1500ms)); // the last packet + the timeout: no grace
        CHECK_EQ(s.stats().orphan_data, 1u);
    }
}

// --- broadcasts ---------------------------------------------------------------------------------

TEST_CASE(sniffer_follows_broadcasts_only_when_asked)
{
    const Bytes message = payload(14);
    Sniffer off;
    off.feed(at(0ms), bam(kA, 14, kPgn));
    off.feed(at(50ms), dts(kA, kGlobal, message, 1, 2));
    CHECK(off.heard.empty());
    CHECK_EQ(off.stats().started, 0u);
    CHECK_EQ(off.stats().orphan_data, 2u);

    Sniffer on(with_broadcasts());
    on.feed(at(0ms), bam(kA, 14, kPgn));
    on.feed(at(50ms), dt(kA, kGlobal, 1, message));
    on.feed(at(100ms), dt(kA, kGlobal, 2, message));
    CHECK(on.ends() == (EndList{SniffEnd::Completed}));
    CHECK(on.heard[0].data == message);
    CHECK(on.heard[0].started == at(0ms));
    CHECK(on.heard[0].time == at(100ms));
    CHECK(on.heard[0].noticed == at(100ms));
}

TEST_CASE(sniffer_loses_a_broadcast_with_a_missing_repeated_or_reordered_packet)
{
    const Bytes message = payload(30);
    const std::vector<std::vector<int>> orders{{1, 3}, {1, 1}, {2}, {1, 2, 3, 5}};
    for (const auto& order : orders)
    {
        Sniffer s(with_broadcasts());
        s.feed(at(0ms), bam(kA, 30, kPgn));
        SimTime time = at(50ms);
        for (const int k : order)
        {
            CHECK(s.heard.empty());
            time += 50ms;
            s.feed(time, dt(kA, kGlobal, static_cast<std::uint8_t>(k), message));
        }
        CHECK(s.ends() == (EndList{SniffEnd::Lost}));
        CHECK(s.heard[0].time == time);
        CHECK_EQ(s.stats().lost, 1u);
        // The rest of it finds no transfer.
        s.feed(time + 50ms, dt(kA, kGlobal, 5, message));
        CHECK_EQ(s.stats().orphan_data, 1u);
    }
    // Too short for its data: as if it had not arrived (the packet can still come).
    Sniffer s(with_broadcasts());
    s.feed(at(0ms), bam(kA, 30, kPgn));
    J1939Frame shortPacket = dt(kA, kGlobal, 1, message);
    shortPacket.data.resize(4);
    s.feed(at(50ms), shortPacket);
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().bad_packets, 1u);
    s.feed(at(100ms), dt(kA, kGlobal, 1, message));
    CHECK(s.heard.empty());
}

TEST_CASE(sniffer_new_broadcast_of_a_sender_replaces_the_running_one)
{
    Sniffer s(with_broadcasts());
    const Bytes first = payload(30);
    const Bytes second = payload(14, 9);
    s.feed(at(0ms), bam(kA, 30, kPgn));
    s.feed(at(50ms), dt(kA, kGlobal, 1, first));
    s.feed(at(60ms), bam(kA, 30, kPgn, 9)); // a wrong packet count: the running one goes on
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().announcements_malformed, 1u);
    s.feed(at(100ms), bam(kA, 14, kOther));
    CHECK(s.ends() == (EndList{SniffEnd::Replaced}));
    CHECK_EQ(s.heard[0].pgn, kPgn);
    CHECK(s.heard[0].time == at(100ms));
    s.feed(at(150ms), dt(kA, kGlobal, 1, second));
    s.feed(at(200ms), dt(kA, kGlobal, 2, second));
    CHECK(s.ends() == (EndList{SniffEnd::Replaced, SniffEnd::Completed}));
    CHECK_EQ(s.heard[1].pgn, kOther);
    CHECK(s.heard[1].data == second);
    // A broadcast and a connection of the same sender run side by side.
    s.feed(at(300ms), bam(kA, 14, kOther));
    s.feed(at(300ms), rts(kA, kB, 14, kPgn));
    CHECK_EQ(s.sniffer.active(), 2u);
}

// --- a second announcement ----------------------------------------------------------------------

TEST_CASE(sniffer_new_rts_for_the_same_group_replaces_the_transfer)
{
    Sniffer s;
    const Bytes first = payload(30);
    const Bytes second = payload(30, 100);
    open(s, 30);
    s.feed(at(20ms), dts(kA, kB, first, 1, 3));
    s.feed(at(50ms), rts(kA, kB, 30, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Replaced}));
    CHECK(s.heard[0].time == at(50ms));
    s.feed(at(60ms), cts(kB, kA, 5, 1, kPgn));
    s.feed(at(70ms), dts(kA, kB, second, 1, 5));
    s.feed(at(80ms), eoma(kB, kA, 30, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Replaced, SniffEnd::Completed}));
    CHECK(s.heard[1].data == second); // nothing of the first one is left
    CHECK(s.heard[1].started == at(50ms));
    // As the engine's receiver: a malformed RTS for the same group also ends the running one.
    s.feed(at(100ms), rts(kA, kB, 30, kPgn));
    s.feed(at(110ms), rts(kA, kB, 30, kPgn, 0xFF, 9));
    CHECK(s.ends() == (EndList{SniffEnd::Replaced, SniffEnd::Completed, SniffEnd::Replaced}));
    CHECK_EQ(s.stats().announcements_malformed, 1u);
    CHECK_EQ(s.sniffer.active(), 0u);
}

TEST_CASE(sniffer_ignores_an_rts_for_another_group_while_a_transfer_runs)
{
    Sniffer s;
    const Bytes message = payload(30);
    open(s, 30);
    s.feed(at(20ms), dts(kA, kB, message, 1, 2));
    s.feed(at(30ms), rts(kA, kB, 100, kOther));
    s.feed(at(40ms), conn_abort(kB, kA, kBusyReason, kOther)); // the receiver refuses it
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().announcements_ignored, 1u);
    CHECK_EQ(s.stats().orphan_control, 0u);
    s.feed(at(50ms), dts(kA, kB, message, 3, 5));
    s.feed(at(60ms), eoma(kB, kA, 30, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK(s.heard[0].data == message);
    CHECK_EQ(s.heard[0].pgn, kPgn);
}

TEST_CASE(sniffer_after_an_ignored_rts_only_an_acknowledgement_completes_the_transfer)
{
    // The first message stops after two packets and the sender announces another one. Its
    // packets fill the gaps of the first: without the rule, "the first group with the bytes of
    // the second" would be reported as soon as every number was seen.
    SnifferOptions options;
    options.delivery = SniffDelivery::WhenAllPacketsSeen;
    Sniffer s(options);
    const Bytes first = payload(30);
    const Bytes second = payload(30, 100);
    open(s, 30);
    s.feed(at(20ms), dts(kA, kB, first, 1, 2));
    s.feed(at(30ms), rts(kA, kB, 30, kOther));
    s.feed(at(40ms), dts(kA, kB, second, 1, 5));
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().announcements_ignored, 1u);
    s.feed(at(50ms), eoma(kB, kA, 30, kOther)); // for the group that was not followed
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().orphan_control, 1u);
    s.sniffer.advance(at(5s));
    CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
    CHECK_EQ(s.heard[0].pgn, kPgn);
    // With the acknowledgement for its group it completes.
    Sniffer t(options);
    open(t, 30);
    t.feed(at(20ms), dts(kA, kB, first, 1, 2));
    t.feed(at(30ms), rts(kA, kB, 30, kOther));
    t.feed(at(40ms), dts(kA, kB, first, 3, 5));
    CHECK(t.heard.empty());
    t.feed(at(50ms), eoma(kB, kA, 30, kPgn));
    CHECK(t.ends() == (EndList{SniffEnd::Completed}));
    CHECK(t.heard[0].data == first);
}

TEST_CASE(sniffer_can_replace_the_transfer_on_an_rts_for_another_group)
{
    SnifferOptions options;
    options.other_group = SniffSecondAnnouncement::Replace;
    Sniffer s(options);
    const Bytes second = payload(14, 100);
    open(s, 30);
    s.feed(at(20ms), rts(kA, kB, 14, kOther, 0)); // malformed (no packets per clear-to-send): no effect
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().announcements_malformed, 1u);
    s.feed(at(30ms), rts(kA, kB, 14, kOther));
    CHECK(s.ends() == (EndList{SniffEnd::Replaced}));
    CHECK_EQ(s.heard[0].pgn, kPgn);
    s.feed(at(40ms), dts(kA, kB, second, 1, 2));
    s.feed(at(50ms), eoma(kB, kA, 14, kOther));
    CHECK(s.ends() == (EndList{SniffEnd::Replaced, SniffEnd::Completed}));
    CHECK_EQ(s.heard[1].pgn, kOther);
    CHECK(s.heard[1].data == second);
    CHECK_EQ(s.stats().announcements_ignored, 0u);
}

// --- groups that are single-frame only ----------------------------------------------------------

TEST_CASE(sniffer_refuses_a_group_that_is_single_frame_only)
{
    for (const std::uint32_t pgn : {kPgnRequest, kPgnAddressClaim})
    {
        Sniffer s(with_broadcasts());
        const Bytes message = payload(14);
        open(s, 14, pgn);
        s.feed(at(20ms), dts(kA, kB, message, 1, 2));
        s.feed(at(30ms), eoma(kB, kA, 14, pgn));
        CHECK(s.ends() == (EndList{SniffEnd::Refused}));
        CHECK(s.heard[0].data.empty());
        CHECK(s.heard[0].time == at(20ms));
        CHECK_EQ(s.heard[0].pgn, pgn);
        s.feed(at(100ms), bam(kA, 14, pgn));
        s.feed(at(150ms), dt(kA, kGlobal, 1, message));
        s.feed(at(200ms), dt(kA, kGlobal, 2, message));
        CHECK(s.ends() == (EndList{SniffEnd::Refused, SniffEnd::Refused}));
        CHECK_EQ(s.stats().refused, 2u);
        CHECK_EQ(s.stats().completed, 0u);
    }
}

// --- an acknowledgement while packets are missing -----------------------------------------------

TEST_CASE(sniffer_decides_an_early_acknowledgement_when_the_time_moves_on)
{
    const Bytes message = payload(20);
    {
        // Frames with the same time can arrive in either order: the last packet after the
        // acknowledgement still completes the message.
        Sniffer s;
        open(s, 20);
        s.feed(at(20ms), dts(kA, kB, message, 1, 2));
        s.feed(at(30ms), eoma(kB, kA, 20, kPgn));
        s.sniffer.advance(at(30ms)); // the same time: not decided yet
        CHECK(s.heard.empty());
        CHECK_EQ(s.sniffer.active(), 1u);
        s.feed(at(30ms), dt(kA, kB, 3, message));
        CHECK(s.ends() == (EndList{SniffEnd::Completed}));
        CHECK(s.heard[0].data == message);
        CHECK(s.heard[0].time == at(30ms));
    }
    {
        // The time moves on with advance(): the packets were missed.
        Sniffer s;
        open(s, 20);
        s.feed(at(20ms), dts(kA, kB, message, 1, 2));
        s.feed(at(30ms), eoma(kB, kA, 20, kPgn));
        s.sniffer.advance(at(31ms));
        CHECK(s.ends() == (EndList{SniffEnd::Incomplete}));
        CHECK(s.heard[0].time == at(30ms));
        CHECK(s.heard[0].noticed == at(31ms));
        CHECK(s.heard[0].data.empty());
        CHECK_EQ(s.stats().incomplete, 1u);
    }
    {
        // The time moves on with any later frame; a packet that comes then is too late.
        Sniffer s;
        open(s, 20);
        s.feed(at(20ms), dts(kA, kB, message, 1, 2));
        s.feed(at(30ms), eoma(kB, kA, 20, kPgn));
        s.feed(at(31ms), dt(kA, kB, 3, message));
        CHECK(s.ends() == (EndList{SniffEnd::Incomplete}));
        CHECK_EQ(s.stats().orphan_data, 1u);
    }
}

// --- joining late, places -----------------------------------------------------------------------

TEST_CASE(sniffer_drops_a_transfer_it_joined_in_the_middle_and_takes_the_next)
{
    Sniffer s;
    const Bytes message = payload(20);
    s.feed(at(0ms), cts(kB, kA, 3, 1, kPgn));
    s.feed(at(10ms), dts(kA, kB, message, 1, 3));
    s.feed(at(20ms), eoma(kB, kA, 20, kPgn));
    s.feed(at(30ms), conn_abort(kB, kA, 1, kPgn));
    CHECK(s.heard.empty());
    CHECK_EQ(s.stats().orphan_control, 3u);
    CHECK_EQ(s.stats().orphan_data, 3u);
    s.feed(at(100ms), rts(kA, kB, 20, kPgn));
    s.feed(at(110ms), cts(kB, kA, 3, 1, kPgn));
    s.feed(at(120ms), dts(kA, kB, message, 1, 3));
    s.feed(at(130ms), eoma(kB, kA, 20, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed}));
    CHECK_EQ(s.stats().started, 1u);
}

TEST_CASE(sniffer_without_a_free_place_counts_the_announcement_and_stopped_transfers_make_room)
{
    SnifferOptions options;
    options.sessions = 2;
    Sniffer s(options);
    s.feed(at(0ms), rts(kA, kB, 30, kPgn));
    s.feed(at(100ms), rts(kA, kC, 30, kPgn));
    s.feed(at(200ms), rts(kC, kB, 30, kPgn));
    CHECK_EQ(s.stats().no_session, 1u);
    CHECK_EQ(s.stats().started, 2u);
    CHECK_EQ(s.sniffer.active(), 2u);
    // The first one has stopped: it is cleared before the new one looks for a place.
    s.feed(at(1501ms), rts(kC, kB, 30, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::TimedOut}));
    CHECK_EQ(s.heard[0].da, kB);
    CHECK_EQ(s.stats().no_session, 1u);
    CHECK_EQ(s.stats().started, 3u);
    CHECK_EQ(s.sniffer.active(), 2u);
}

TEST_CASE(sniffer_follows_transfers_that_run_at_the_same_time)
{
    Sniffer s(with_broadcasts());
    const Bytes ab = payload(20, 1);
    const Bytes ba = payload(20, 2);
    const Bytes cb = payload(20, 3);
    const Bytes all = payload(20, 4);
    s.feed(at(0ms), rts(kA, kB, 20, kPgn));
    s.feed(at(0ms), rts(kB, kA, 20, kPgn));
    s.feed(at(0ms), rts(kC, kB, 20, kOther));
    s.feed(at(0ms), bam(kC, 20, kPgn));
    for (std::uint8_t k = 1; k <= 3; ++k)
    {
        const SimTime time = at(50ms * k);
        s.feed(time, dt(kA, kB, k, ab));
        s.feed(time, dt(kB, kA, k, ba));
        s.feed(time, dt(kC, kB, k, cb));
        s.feed(time, dt(kC, kGlobal, k, all));
    }
    s.feed(at(200ms), eoma(kB, kC, 20, kOther));
    s.feed(at(200ms), eoma(kA, kB, 20, kPgn));
    s.feed(at(200ms), eoma(kB, kA, 20, kPgn));
    CHECK(s.ends() == (EndList{SniffEnd::Completed, SniffEnd::Completed, SniffEnd::Completed, SniffEnd::Completed}));
    CHECK(s.heard[0].data == all);
    CHECK(s.heard[1].data == cb);
    CHECK(s.heard[2].data == ba);
    CHECK(s.heard[3].data == ab);
}

// --- announcements that are not followed --------------------------------------------------------

TEST_CASE(sniffer_does_not_follow_an_announcement_the_engine_would_not_accept)
{
    Sniffer s(with_broadcasts());
    s.feed(at(0ms), rts(kA, kB, 8, kPgn));             // fits a single frame
    s.feed(at(0ms), rts(kA, kB, 1786, kPgn, 0xFF, 0)); // too large
    s.feed(at(0ms), rts(kA, kB, 30, kPgn, 0xFF, 4));   // 30 bytes are 5 packets
    s.feed(at(0ms), rts(kA, kB, 30, kPgn, 0));         // no packet per clear-to-send
    s.feed(at(0ms), rts(kA, kB, 30, 0x40000));         // not a group
    s.feed(at(0ms), rts(kA, kGlobal, 30, kPgn));       // a connection to all
    s.feed(at(0ms), bam(kA, 8, kPgn));
    s.feed(at(0ms), bam(kA, 1786, kPgn, 0));
    s.feed(at(0ms), bam(kA, 30, kPgn, 6));
    s.feed(at(0ms), bam(kA, 30, 0x40000));
    s.feed(at(0ms), bam(kA, 30, kPgn, -1, kB)); // a broadcast to one
    CHECK_EQ(s.stats().announcements_malformed, 11u);
    CHECK_EQ(s.stats().started, 0u);
    CHECK_EQ(s.sniffer.active(), 0u);
    // The receiver's refusal then finds no transfer.
    s.feed(at(1ms), conn_abort(kB, kA, 250, kPgn));
    CHECK_EQ(s.stats().orphan_control, 1u);
    // The largest and the smallest message are followed.
    s.feed(at(2ms), rts(kA, kB, 1785, kPgn));
    s.feed(at(2ms), bam(kA, 9, kPgn));
    CHECK_EQ(s.stats().started, 2u);
    // Frames of the group with other content are left alone.
    s.feed(at(3ms), frame(7, kPgnTpCm, kA, kB, {16, 30, 0}));
    s.feed(at(3ms), frame(7, kPgnTpCm, kA, kB, cm(99, 0, 0, 0, 0, kPgn)));
    s.feed(at(3ms), cts(kB, kGlobal, 1, 1, kPgn));
    s.feed(at(3ms), conn_abort(kB, kGlobal, 1, kPgn));
    s.feed(at(3ms), eoma(kB, kGlobal, 1785, kPgn));
    CHECK_EQ(s.stats().orphan_control, 4u);
    CHECK_EQ(s.sniffer.active(), 2u);
    CHECK(s.heard.empty());
}

// --- the object ---------------------------------------------------------------------------------

TEST_CASE(sniffer_rejects_options_that_cannot_work_and_says_what_to_give)
{
    const auto make = [](auto change) {
        SnifferOptions options;
        change(options);
        return [options] { TransferSniffer sniffer(options); };
    };
    CHECK(throws_with<std::invalid_argument>(make([](SnifferOptions& o) { o.sessions = 0; }),
                                             "SnifferOptions::sessions is 0. Give the number of transfers to follow"));
    CHECK(throws_with<std::invalid_argument>(make([](SnifferOptions& o) { o.timeout = 0ms; }),
                                             "SnifferOptions::timeout is 0 ns. It must be greater than 0"));
    CHECK(throws_with<std::invalid_argument>(make([](SnifferOptions& o) { o.broadcast_timeout = -1ms; }),
                                             "SnifferOptions::broadcast_timeout is -1000000 ns. It must be greater than 0"));
    CHECK(throws_with<std::invalid_argument>(make([](SnifferOptions& o) { o.grace = -1ns; }),
                                             "SnifferOptions::grace is -1 ns. It must be 0 or more"));
    SnifferOptions fine;
    fine.sessions = 1;
    fine.grace = 0ms;
    TransferSniffer sniffer(fine);
    CHECK_EQ(sniffer.options().sessions, 1u);
    // The defaults are the engine's timeouts.
    CHECK(SnifferOptions{}.timeout == 1250ms);
    CHECK(SnifferOptions{}.broadcast_timeout == 750ms);
    CHECK(SnifferOptions{}.grace == 250ms);
    CHECK_EQ(SnifferOptions{}.sessions, 32u);
    CHECK(!SnifferOptions{}.broadcasts);
}

TEST_CASE(sniffer_handler_may_not_call_back_into_the_sniffer)
{
    TransferSniffer sniffer;
    std::string said;
    int calls = 0;
    sniffer.on_transfer([&](const SniffedTransfer&) {
        ++calls;
        try
        {
            sniffer.advance(at(10s));
        }
        catch (const std::logic_error& e)
        {
            said = e.what();
        }
        CHECK(throws_with<std::logic_error>([&] { sniffer.feed(TimedFrame{at(10s), to_can(rts(kA, kB, 30, kPgn))}); },
                                            "feed() was called from the on_transfer handler"));
        CHECK(throws_with<std::logic_error>([&] { sniffer.on_transfer({}); },
                                            "on_transfer() was called from the on_transfer handler"));
        CHECK_EQ(sniffer.active(), 0u); // the transfer is over when the handler hears of it
    });
    sniffer.feed(TimedFrame{at(0ms), to_can(rts(kA, kB, 30, kPgn))});
    sniffer.feed(TimedFrame{at(1ms), to_can(conn_abort(kA, kB, 2, kPgn))});
    CHECK_EQ(calls, 1);
    CHECK(said == "vehicle_sil::j1939::TransferSniffer: advance() was called from the on_transfer handler. "
                  "Copy what the handler needs and call it after feed() or advance() has returned");
    // Usable again afterwards, and without a handler transfers are only counted.
    sniffer.on_transfer({});
    sniffer.feed(TimedFrame{at(2ms), to_can(rts(kA, kB, 30, kPgn))});
    sniffer.advance(at(10s));
    CHECK_EQ(sniffer.statistics().timed_out, 1u);
    CHECK_EQ(calls, 1);
}

TEST_CASE(sniffer_handler_that_throws_leaves_the_sniffer_usable)
{
    TransferSniffer sniffer;
    int calls = 0;
    sniffer.on_transfer([&](const SniffedTransfer&) {
        ++calls;
        throw std::runtime_error("handler");
    });
    sniffer.feed(TimedFrame{at(0ms), to_can(rts(kA, kB, 30, kPgn))});
    sniffer.feed(TimedFrame{at(0ms), to_can(rts(kA, kC, 30, kPgn))});
    CHECK(throws_with<std::runtime_error>([&] { sniffer.advance(at(10s)); }, "handler"));
    CHECK_EQ(sniffer.active(), 1u);
    CHECK(throws_with<std::runtime_error>([&] { sniffer.advance(at(10s)); }, "handler"));
    CHECK_EQ(sniffer.active(), 0u);
    sniffer.advance(at(11s));
    CHECK_EQ(calls, 2);
    CHECK_EQ(sniffer.statistics().timed_out, 2u);
}
