// SPDX-License-Identifier: MIT
//
// The PeriodicSender part of the J1939 stack: the period on the virtual-time grid, missed
// deadlines (Coalesce, CatchUp), the node's address, DM13, trigger(), pause(), long messages,
// changing the period, destination and priority, encoders that append to a vector, creating
// senders in a handler within the free timers, and destroying them in handlers and timers.

#include "j1939_bench.hpp"
#include "j1939_parts_util.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/address_commander.hpp"
#include "vehicle_sil/j1939/broadcast_suppression.hpp"
#include "vehicle_sil/j1939/diagnostics.hpp"
#include "vehicle_sil/j1939/exchange.hpp"
#include "vehicle_sil/j1939/monitor.hpp"
#include "vehicle_sil/j1939/observer.hpp"
#include "vehicle_sil/j1939/periodic_sender.hpp"
#include "vehicle_sil/j1939/requester.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::BroadcastSuppression;
using vehicle_sil::j1939::MissedDeadlines;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::PeriodicSender;
using vehicle_sil::j1939::PeriodicSenderOptions;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

namespace {

constexpr Address kUs = 0x20;
constexpr std::uint32_t kPgnStatus = 0xFF20; // PDU2
const Bytes kData{1, 2, 3, 4, 5, 6, 7, 8};

/// The times of the frames of `pgn` the network sent (Bench::take_sent() order).
std::vector<SimTime> sent_times_of(Bench& bench, std::uint32_t pgn)
{
    const std::size_t first = bench.sent_times.size();
    const auto frames = bench.take_sent();
    std::vector<SimTime> out;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i].pgn == pgn)
        {
            out.push_back(bench.sent_times[first + i]);
        }
    }
    return out;
}

std::vector<SimTime> grid(SimDuration first, SimDuration period, int count)
{
    std::vector<SimTime> out;
    for (int i = 0; i < count; ++i)
    {
        out.push_back(at(first + i * period));
    }
    return out;
}

Name name_of(std::uint32_t identity, bool arbitrary = false)
{
    return Name{}.set_identity_number(identity).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(arbitrary);
}

} // namespace

TEST_CASE(periodic_sender_sends_every_period_from_the_first_step)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.steps(100); // 0 .. 990 ms
    const auto frames = bench.take_sent();
    CHECK_EQ(frames.size(), 10u);
    CHECK(frames.front() == frame(6, kPgnStatus, kUs, kGlobal, kData));
    CHECK(bench.sent_times == grid(0ms, 100ms, 10));
    CHECK_EQ(sender.statistics().sent, 10u);
    CHECK_EQ(sender.statistics().failures, 0u);
}

TEST_CASE(periodic_sender_deadlines_do_not_depend_on_the_step_size_up_to_the_period)
{
    // With step <= period each step holds at most one deadline: one message per deadline, sent
    // in the first step at or after it, whatever the step.
    for (const SimDuration step : {1ms, 3ms, 7ms, 10ms})
    {
        Bench bench({}, step);
        Node& us = bench.add_node(kUs);
        auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 10ms, std::span<const std::uint8_t>(kData));
        while (bench.next_time() < at(1s))
        {
            bench.step();
        }
        const auto times = sent_times_of(bench, kPgnStatus);
        CHECK_EQ(times.size(), 100u);
        CHECK_EQ(sender.statistics().coalesced, 0u);
        for (std::size_t i = 0; i < times.size(); ++i)
        {
            const SimTime deadline = at(10ms * static_cast<int>(i));
            CHECK(times[i] >= deadline && times[i] < deadline + step);
        }
    }
}

TEST_CASE(periodic_sender_coalesces_the_deadlines_of_one_step_or_catches_up)
{
    // Period 10 ms, step 25 ms: deadlines 0 | 10 20 | 30 40 | 50 | 60 70 | ... per step.
    Bench coalesce({}, 25ms);
    Bench catchUp({}, 25ms);
    auto& a = coalesce.add_node(kUs).attach<PeriodicSender>(Pgn{kPgnStatus}, 10ms, std::span<const std::uint8_t>(kData));
    PeriodicSenderOptions options;
    options.missed_deadlines = MissedDeadlines::CatchUp;
    auto& b = catchUp.add_node(kUs).attach<PeriodicSender>(Pgn{kPgnStatus}, 10ms, std::span<const std::uint8_t>(kData), options);
    coalesce.steps(40); // 0 .. 975 ms: deadlines 0 .. 970, 98 of them
    catchUp.steps(40);
    const auto coalesced = sent_times_of(coalesce, kPgnStatus);
    const auto caughtUp = sent_times_of(catchUp, kPgnStatus);
    // Coalesce: once per step that has a deadline (each step of 25 ms has one or two).
    CHECK_EQ(coalesced.size(), 40u);
    CHECK_EQ(a.statistics().sent + a.statistics().coalesced, 98u);
    CHECK(coalesced[1] == at(25ms) && coalesced[2] == at(50ms));
    // CatchUp: once per deadline, several with the same step time.
    CHECK_EQ(caughtUp.size(), 98u);
    CHECK_EQ(b.statistics().coalesced, 0u);
    CHECK(caughtUp[1] == at(25ms) && caughtUp[2] == at(25ms) && caughtUp[3] == at(50ms));
}

namespace {

J1939Frame claim_of(Address sa, Name name)
{
    const auto bytes = name.to_bytes();
    return frame(6, kPgnAddressClaim, sa, kGlobal, Bytes(bytes.begin(), bytes.end()));
}

} // namespace

TEST_CASE(periodic_sender_starts_when_the_address_is_claimed_and_stops_when_it_is_lost)
{
    // 0x90 (128 .. 247): the node waits 250 ms after its claim.
    constexpr Address kWaiting = 0x90;
    Bench bench;
    Node& us = bench.network.add_node(NodeOptions{kWaiting, name_of(0x100)});
    us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.steps(30); // the claim at 0, claimed at 250 ms
    CHECK(us.address_state() == AddressState::Claimed);
    CHECK(sent_times_of(bench, kPgnStatus) == std::vector<SimTime>{at(250ms)}); // not during the wait
    // A rival with a lower NAME takes the address (handled at 300 ms): the node is not arbitrary
    // address capable, cannot claim, and stops.
    bench.inject(claim_of(kWaiting, Name{}.set_identity_number(1).set_manufacturer_code(1)));
    bench.steps(50);
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK(sent_times_of(bench, kPgnStatus).empty());
}

TEST_CASE(periodic_sender_of_a_node_that_does_not_wait_after_its_claim_starts_with_the_claim)
{
    // 0x20 without arbitrary address capability: the engine sends at once while Claiming.
    Bench bench;
    Node& us = bench.network.add_node(NodeOptions{kUs, name_of(0x100)});
    us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.steps(35);
    CHECK(us.address_state() == AddressState::Claimed);
    // Not again when it becomes Claimed at 250 ms.
    CHECK(sent_times_of(bench, kPgnStatus) == grid(0ms, 100ms, 4));
}

TEST_CASE(periodic_sender_moves_with_the_node_and_starts_again_from_the_new_address)
{
    Bench bench;
    Node& us = bench.network.add_node(NodeOptions{kUs, name_of(0x100, true)});
    us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.steps(36); // 250 and 350 ms
    CHECK_EQ(sent_times_of(bench, kPgnStatus).size(), 2u);
    bench.inject(claim_of(kUs, Name{}.set_identity_number(1).set_manufacturer_code(1))); // handled at 360 ms
    bench.steps(40); // 360 .. 750 ms: moves to 128 at 360, claimed at 610 ms
    CHECK(us.address() == 128);
    const auto frames = bench.take_sent();
    std::vector<J1939Frame> statuses;
    for (const auto& f : frames)
    {
        if (f.pgn == kPgnStatus)
        {
            statuses.push_back(f);
        }
    }
    CHECK_EQ(statuses.size(), 2u); // 610 and 710 ms, from the new address
    CHECK(statuses.size() == 2 && statuses[0].sa == 128 && statuses[1].sa == 128);
}

TEST_CASE(periodic_sender_is_quiet_while_dm13_suspends_the_broadcasts)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    PeriodicSenderOptions loud;
    loud.suppressible = false;
    auto& always = us.attach<PeriodicSender>(Pgn{kPgnStatus + 1}, 100ms, std::span<const std::uint8_t>(kData), loud);
    bench.steps(15); // 0, 100 ms
    bench.inject(frame(6, kPgnDm13, 0x90, kGlobal, dm13(kNetwork1, kStop))); // handled at 150 ms
    bench.steps(20); // 150 .. 340 ms
    bench.inject(frame(6, kPgnDm13, 0x90, kGlobal, dm13(kNetwork1, kStart))); // handled at 350 ms
    bench.steps(15); // .. 490 ms
    CHECK(sent_times_of(bench, kPgnStatus) == (std::vector<SimTime>{at(0ms), at(100ms), at(400ms)}));
    CHECK_EQ(sender.statistics().suppressed, 2u); // 200 and 300 ms, not made up for
    CHECK_EQ(always.statistics().sent, 5u);
    CHECK(BroadcastSuppression::of(us).stopped_by_dm13() == false);
}

TEST_CASE(periodic_sender_to_one_address_follows_dm13_only_when_asked)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    PeriodicSenderOptions toOne;
    toOne.destination = 0x30;
    auto& byDefault = us.attach<PeriodicSender>(Pgn{0xEF00}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    toOne.suppressible = true;
    auto& asked = us.attach<PeriodicSender>(Pgn{0xEF00}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    PeriodicSenderOptions toAll;
    auto& broadcast = us.attach<PeriodicSender>(Pgn{0xEF00}, 100ms, std::span<const std::uint8_t>(kData), toAll);
    BroadcastSuppression::of(us).suspend(std::chrono::seconds(1));
    bench.steps(10);
    CHECK_EQ(byDefault.statistics().sent, 1u);
    CHECK_EQ(byDefault.statistics().suppressed, 0u);
    CHECK_EQ(asked.statistics().suppressed, 1u);
    CHECK_EQ(broadcast.statistics().suppressed, 1u);
}

TEST_CASE(periodic_sender_does_not_send_its_first_message_while_suppressed)
{
    {
        // Stopped by DM13 during the wait after the claim: nothing when the node is Claimed.
        Bench bench;
        Node& us = bench.network.add_node(NodeOptions{0x90, name_of(0x100)});
        auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
        bench.steps(10);
        bench.inject(frame(6, kPgnDm13, 0x30, kGlobal, dm13(kNetwork1, kStop))); // at 90 ms
        bench.steps(26); // .. 350 ms
        CHECK(sent_times_of(bench, kPgnStatus).empty());
        CHECK_EQ(sender.statistics().suppressed, 2u); // 250 (the first) and 350 ms
    }
    {
        // A node that does not wait, suspended before it starts.
        Bench bench;
        Node& us = bench.network.add_node(NodeOptions{kUs, name_of(0x100)});
        auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
        BroadcastSuppression::of(us).suspend(std::chrono::seconds(5));
        bench.steps(30);
        CHECK(sent_times_of(bench, kPgnStatus).empty());
        CHECK_EQ(sender.statistics().suppressed, 3u);
    }
}

TEST_CASE(periodic_sender_restarted_from_the_application_counts_from_the_step_that_sends)
{
    // trigger() in the application stage at 500 ms with step == period: it goes at 600 ms and
    // replaces that deadline; nothing is merged, no two frames share a time.
    Bench bench({}, 100ms);
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.app = [&](SimTime now) {
        if (now == at(500ms))
        {
            CHECK(sender.trigger());
        }
    };
    bench.steps(10);
    CHECK(sent_times_of(bench, kPgnStatus) == grid(0ms, 100ms, 10));
    CHECK_EQ(sender.statistics().coalesced, 0u);
    // Created outside a step, after the steps at 0 .. 900 ms, catching up: one frame at 1000 ms.
    PeriodicSenderOptions catchUp;
    catchUp.missed_deadlines = MissedDeadlines::CatchUp;
    us.attach<PeriodicSender>(Pgn{kPgnStatus + 1}, 100ms, std::span<const std::uint8_t>(kData), catchUp);
    bench.steps(3);
    CHECK(sent_times_of(bench, kPgnStatus + 1) == grid(1000ms, 100ms, 3));
}

TEST_CASE(periodic_sender_trigger_sends_at_the_next_chance_and_counts_the_period_from_there)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    // From a handler: the message's time, in the same step.
    us.on_message(Pgn{0xFF30}, [&](const vehicle_sil::j1939::Message&) { CHECK(sender.trigger()); });
    bench.steps(4); // 0 .. 30 ms: sent at 0
    bench.inject(frame(6, 0xFF30, 0x90, kGlobal, kData)); // time 30 ms, handled at 40 ms
    bench.steps(16); // 40 .. 190 ms: the period counts from 30 ms
    CHECK(sent_times_of(bench, kPgnStatus) == (std::vector<SimTime>{at(0ms), at(40ms), at(130ms)}));
    // From the application stage: the next step.
    bench.app = [&](SimTime now) {
        if (now == at(200ms))
        {
            CHECK(sender.trigger());
        }
    };
    bench.steps(15); // 200 .. 340 ms: the trigger at 200 goes at 210; then 300 (from 200)
    CHECK(sent_times_of(bench, kPgnStatus) == (std::vector<SimTime>{at(210ms), at(300ms)}));
}

TEST_CASE(periodic_sender_pause_and_resume)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.steps(15); // 0, 100 ms
    sender.pause();
    CHECK(sender.paused());
    CHECK(!sender.trigger());
    bench.steps(20); // 150 .. 340 ms
    sender.resume(); // outside a step: from the last step's time (340 ms), sent in the next step
    bench.steps(15); // 350 .. 490 ms
    CHECK(sent_times_of(bench, kPgnStatus) == (std::vector<SimTime>{at(0ms), at(100ms), at(350ms), at(440ms)}));
}

TEST_CASE(periodic_sender_encodes_at_each_send_and_long_messages_use_the_transport_protocol)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::uint8_t counter = 0;
    PeriodicSenderOptions options;
    options.max_length = 20;
    auto& sender = us.attach<PeriodicSender>(
        Pgn{kPgnStatus}, 100ms,
        [&](std::span<std::uint8_t> out) {
            CHECK_EQ(out.size(), 20u);
            out[0] = counter++;
            return counter % 2 == 1 ? std::size_t{8} : std::size_t{20};
        },
        options);
    bench.steps(5); // 0 .. 40: 8 bytes at 0
    auto frames = bench.take_sent();
    CHECK(frames.size() == 1 && frames[0].data[0] == 0);
    bench.steps(10); // 50 .. 140: 20 bytes at 100 ms, a BAM
    frames = bench.take_sent();
    CHECK(!frames.empty() && frames[0].pgn == kPgnTpCm && frames[0].data[0] == 0x20);
    CHECK_EQ(sender.statistics().sent, 2u);
}

TEST_CASE(periodic_sender_counts_a_transfer_that_is_still_going_as_a_failure)
{
    // 20 bytes every 50 ms: a BAM takes 3 packets 50 ms apart, so every other deadline finds the
    // one before still going (Busy).
    Bench bench;
    Node& us = bench.add_node(kUs);
    PeriodicSenderOptions options;
    options.max_length = 20;
    const Bytes data = payload(20);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 50ms, std::span<const std::uint8_t>(data), options);
    bench.steps(100);
    CHECK(sender.statistics().sent > 0u);
    CHECK(sender.statistics().failures > 0u);
    CHECK(sender.statistics().last_failure == SendStatus::Busy);
    CHECK_EQ(sender.statistics().sent + sender.statistics().failures, 20u);
}

TEST_CASE(periodic_sender_fails_the_network_when_the_encoder_writes_too_much)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    us.attach<PeriodicSender>(Pgn{kPgnStatus}, 10ms, [](std::span<std::uint8_t> out) { return out.size() + 1; });
    CHECK(throws<std::length_error>([&] { bench.step(); }));
    CHECK(bench.network.failed());
}

TEST_CASE(periodic_sender_refuses_invalid_settings)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    const std::span<const std::uint8_t> data(kData);
    CHECK(throws<std::invalid_argument>([&] { PeriodicSender s(us, Pgn{0x3FFFFu + 1}, 10ms, data); }));
    CHECK(throws<std::invalid_argument>([&] { PeriodicSender s(us, Pgn{kPgnStatus}, 0ms, data); }));
    PeriodicSenderOptions toOne;
    toOne.destination = 0x30;
    CHECK(throws<std::invalid_argument>([&] { PeriodicSender s(us, Pgn{kPgnStatus}, 10ms, data, toOne); }));
    PeriodicSenderOptions shortBuffer;
    shortBuffer.max_length = 4;
    CHECK(throws<std::length_error>([&] { PeriodicSender s(us, Pgn{kPgnStatus}, 10ms, data, shortBuffer); }));
    CHECK(throws<std::invalid_argument>([&] { PeriodicSender s(us, Pgn{kPgnStatus}, 10ms, PeriodicSender::Encoder{}); }));
    // PDU1 to one address is fine.
    PeriodicSenderOptions pdu1;
    pdu1.destination = 0x30;
    PeriodicSender toAddress(us, Pgn{0xEF00}, 10ms, data, pdu1);
    bench.step();
    const auto frames = bench.take_sent();
    CHECK(frames.size() == 1 && frames[0].pgn == 0xEF00 && frames[0].da == 0x30);
}

TEST_CASE(periodic_sender_reserves_a_timer_and_says_which_option_to_raise)
{
    vehicle_sil::j1939::NetworkOptions options;
    options.timer_capacity = 1;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    PeriodicSenderOptions plain;
    plain.suppressible = false; // DM13's part would take timers too
    PeriodicSender first(us, Pgn{kPgnStatus}, 10ms, std::span<const std::uint8_t>(kData), plain);
    bool named = false;
    try
    {
        PeriodicSender second(us, Pgn{kPgnStatus + 1}, 10ms, std::span<const std::uint8_t>(kData), plain);
    }
    catch (const std::length_error& e)
    {
        named = std::string(e.what()).find("timer_capacity") != std::string::npos;
    }
    CHECK(named);
}

// --- Changing the period, the destination and the priority

namespace {

struct Sent
{
    SimTime time;
    J1939Frame frame;
};

/// The frames of `pgn` (or of `other` too) the network sent, with their times (Bench::take_sent()
/// order).
std::vector<Sent> sent_of(Bench& bench, std::uint32_t pgn, std::uint32_t other = 0x40000)
{
    const std::size_t first = bench.sent_times.size();
    const auto frames = bench.take_sent();
    std::vector<Sent> out;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i].pgn == pgn || frames[i].pgn == other)
        {
            out.push_back({bench.sent_times[first + i], frames[i]});
        }
    }
    return out;
}

std::vector<SimTime> times_of(const std::vector<Sent>& sent)
{
    std::vector<SimTime> out;
    for (const auto& s : sent)
    {
        out.push_back(s.time);
    }
    return out;
}

std::vector<SimTime> ms_list(std::initializer_list<int> values)
{
    std::vector<SimTime> out;
    for (const int v : values)
    {
        out.push_back(at(std::chrono::milliseconds(v)));
    }
    return out;
}

constexpr std::uint32_t kPgnPdu1 = 0xEF00; // Proprietary A: to one address

} // namespace

TEST_CASE(periodic_sender_set_period_counts_the_new_period_from_the_call)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    // In the application stage at 250 ms: from the step time.
    bench.app = [&](SimTime now) {
        if (now == at(250ms))
        {
            sender.set_period(30ms);
        }
    };
    bench.steps(40); // 0 .. 390 ms
    CHECK(sent_times_of(bench, kPgnStatus) == ms_list({0, 100, 200, 280, 310, 340, 370}));
    CHECK(sender.period() == 30ms);
    // In a handler: from the message's time (390 ms, handled at 400 ms). The deadline of 400 ms
    // is not sent.
    us.on_message(Pgn{0xFF30}, [&](const vehicle_sil::j1939::Message&) { sender.set_period(100ms); });
    bench.inject(frame(6, 0xFF30, 0x90, kGlobal, kData));
    bench.steps(30); // 400 .. 690 ms
    CHECK(sent_times_of(bench, kPgnStatus) == ms_list({490, 590, 690}));
    // After trigger(): the send at the next chance stays, the new period counts from it.
    bench.app = [&](SimTime now) {
        if (now == at(700ms))
        {
            CHECK(sender.trigger());
            sender.set_period(50ms);
        }
    };
    bench.steps(11); // 700 .. 800 ms
    CHECK(sent_times_of(bench, kPgnStatus) == ms_list({710, 750, 800}));
    CHECK_EQ(sender.statistics().coalesced, 0u);
}

TEST_CASE(periodic_sender_set_period_shorter_than_the_step_coalesces_or_catches_up)
{
    // Steps of 25 ms; at 100 ms (application stage) the period becomes 10 ms: deadlines 110 120 |
    // 130 140 150 | 160 170 | 180 190 200 in the steps at 125, 150, 175 and 200 ms.
    Bench coalesce({}, 25ms);
    Bench catchUp({}, 25ms);
    auto& a = coalesce.add_node(kUs).attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    PeriodicSenderOptions options;
    options.missed_deadlines = MissedDeadlines::CatchUp;
    auto& b = catchUp.add_node(kUs).attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData), options);
    coalesce.app = [&](SimTime now) {
        if (now == at(100ms))
        {
            a.set_period(10ms);
        }
    };
    catchUp.app = [&](SimTime now) {
        if (now == at(100ms))
        {
            b.set_period(10ms);
        }
    };
    coalesce.steps(9); // 0 .. 200 ms
    catchUp.steps(9);
    CHECK(sent_times_of(coalesce, kPgnStatus) == ms_list({0, 100, 125, 150, 175, 200}));
    CHECK_EQ(a.statistics().coalesced, 6u); // 120 | 140 150 | 170 | 190 200
    CHECK(sent_times_of(catchUp, kPgnStatus) == ms_list({0, 100, 125, 125, 150, 150, 150, 175, 175, 200, 200, 200}));
    CHECK_EQ(b.statistics().coalesced, 0u);
}

TEST_CASE(periodic_sender_changed_while_paused_resumes_with_the_new_values)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    PeriodicSenderOptions toOne;
    toOne.destination = 0x30;
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    bench.steps(15); // 0, 100 ms
    sender.pause();
    sender.set_period(50ms);
    sender.set_destination(0x31);
    sender.set_priority(3);
    bench.steps(10); // 150 .. 240 ms: nothing
    CHECK(sent_of(bench, kPgnPdu1).size() == 2u);
    sender.resume(); // outside a step: from the last step's time (240 ms), sent in the next step
    bench.steps(16); // 250 .. 400 ms
    const auto sent = sent_of(bench, kPgnPdu1);
    CHECK(times_of(sent) == ms_list({250, 290, 340, 390}));
    for (const auto& s : sent)
    {
        CHECK(s.frame == frame(3, kPgnPdu1, kUs, 0x31, kData));
    }
}

TEST_CASE(periodic_sender_set_destination_and_priority_take_effect_at_the_next_send_on_the_same_grid)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    BroadcastSuppression::of(us); // to go to all nodes later
    PeriodicSenderOptions toOne;
    toOne.destination = 0x30;
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    bench.steps(15); // 0, 100 ms
    sender.set_destination(0x31); // outside a step, between deadlines
    CHECK_EQ(sender.destination(), 0x31);
    bench.steps(10); // 150 .. 240 ms: 200 ms
    // From a handler (time 240 ms, handled at 250 ms): to all nodes, priority 2.
    us.on_message(Pgn{0xFF30}, [&](const vehicle_sil::j1939::Message&) {
        sender.set_destination(kGlobal);
        sender.set_priority(2);
    });
    bench.inject(frame(6, 0xFF30, 0x90, kGlobal, kData));
    bench.steps(10); // 250 .. 340 ms: 300 ms
    CHECK_EQ(sender.priority(), 2);
    const auto sent = sent_of(bench, kPgnPdu1);
    CHECK(times_of(sent) == ms_list({0, 100, 200, 300}));
    CHECK(sent.size() == 4 && sent[0].frame.da == 0x30 && sent[1].frame.da == 0x30);
    CHECK(sent.size() == 4 && sent[2].frame == frame(6, kPgnPdu1, kUs, 0x31, kData));
    CHECK(sent.size() == 4 && sent[3].frame == frame(2, kPgnPdu1, kUs, kGlobal, kData));
}

TEST_CASE(periodic_sender_set_destination_decides_again_whether_dm13_stops_it)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    PeriodicSenderOptions toOne;
    toOne.destination = 0x30;
    auto& byDefault = us.attach<PeriodicSender>(Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    toOne.suppressible = false;
    auto& never = us.attach<PeriodicSender>(Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    toOne.suppressible = true;
    auto& always = us.attach<PeriodicSender>(Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    bench.step(); // 0 ms: all three sent
    // Another node stops the broadcasts while the messages go to one address: seen anyway.
    bench.inject(frame(6, kPgnDm13, 0x90, kGlobal, dm13(kNetwork1, kStop))); // handled at 10 ms
    bench.steps(10); // 10 .. 100 ms
    CHECK_EQ(byDefault.statistics().sent, 2u);
    CHECK_EQ(never.statistics().sent, 2u);
    CHECK_EQ(always.statistics().sent, 1u);
    CHECK_EQ(always.statistics().suppressed, 1u);
    // To all nodes: the default follows DM13 now, `false` still does not.
    byDefault.set_destination(kGlobal);
    never.set_destination(kGlobal);
    bench.steps(10); // 110 .. 200 ms
    CHECK_EQ(byDefault.statistics().sent, 2u);
    CHECK_EQ(byDefault.statistics().suppressed, 1u);
    CHECK_EQ(never.statistics().sent, 3u);
    // Back to one address: sent again; `true` stays suppressed.
    byDefault.set_destination(0x30);
    always.set_destination(0x31);
    bench.steps(10); // 210 .. 300 ms
    CHECK_EQ(byDefault.statistics().sent, 3u);
    CHECK_EQ(byDefault.statistics().suppressed, 1u);
    CHECK_EQ(always.statistics().sent, 1u);
    CHECK_EQ(always.statistics().suppressed, 3u);
}

TEST_CASE(periodic_sender_refuses_invalid_changes_and_keeps_its_values)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& broadcast = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    PeriodicSenderOptions toOne;
    toOne.destination = 0x30;
    toOne.priority = 4;
    auto& addressed = us.attach<PeriodicSender>(Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    bench.steps(5); // 0 ms
    CHECK(throws_with<std::invalid_argument>([&] { broadcast.set_period(0ms); }, "the period must be > 0, not 0 ns"));
    CHECK(throws<std::invalid_argument>([&] { broadcast.set_period(-10ms); }));
    CHECK(throws_with<std::invalid_argument>([&] { broadcast.set_destination(0x30); },
                                             "PGN 0xFF20): destination 0x30: a PDU2 group goes to all nodes"));
    CHECK(throws_with<std::invalid_argument>([&] { addressed.set_destination(vehicle_sil::j1939::kNullAddress); },
                                             "destination 0xFE (kNullAddress)"));
    CHECK(throws_with<std::invalid_argument>([&] { addressed.set_priority(8); }, "priority 8 is above 7"));
    CHECK(broadcast.period() == 100ms);
    CHECK_EQ(broadcast.destination(), kGlobal);
    CHECK_EQ(addressed.destination(), 0x30);
    CHECK_EQ(addressed.priority(), 4);
    bench.steps(10); // 50 .. 140 ms: 100 ms, as before
    const auto sent = sent_of(bench, kPgnStatus, kPgnPdu1);
    CHECK((times_of(sent) == ms_list({0, 0, 100, 100})));
    CHECK(sent.size() == 4 && sent[0].frame == frame(6, kPgnStatus, kUs, kGlobal, kData));
    CHECK(sent.size() == 4 && sent[1].frame == frame(4, kPgnPdu1, kUs, 0x30, kData));
    CHECK(sent.size() == 4 && sent[3].frame == frame(4, kPgnPdu1, kUs, 0x30, kData));
}

TEST_CASE(periodic_sender_changed_while_the_node_moves_starts_with_the_new_values_at_the_new_address)
{
    Bench bench;
    Node& us = bench.network.add_node(NodeOptions{kUs, name_of(0x100, true)});
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.steps(36); // 250 and 350 ms
    CHECK_EQ(sent_times_of(bench, kPgnStatus).size(), 2u);
    bench.inject(claim_of(kUs, Name{}.set_identity_number(1).set_manufacturer_code(1))); // handled at 360 ms
    bench.app = [&](SimTime now) {
        if (now == at(400ms)) // moved to 128, waiting after its claim: it cannot send
        {
            CHECK(!us.can_send_now());
            sender.set_period(50ms);
            sender.set_priority(3);
        }
    };
    bench.steps(40); // 360 .. 750 ms: claimed at 610 ms
    const auto sent = sent_of(bench, kPgnStatus);
    CHECK(times_of(sent) == ms_list({610, 660, 710}));
    for (const auto& s : sent)
    {
        CHECK(s.frame == frame(3, kPgnStatus, 128, kGlobal, kData));
    }
}

// --- Encoders that append to a vector

TEST_CASE(periodic_sender_vector_encoder_sends_a_length_that_changes_from_send_to_send)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    PeriodicSenderOptions options;
    options.max_length = 20;
    const std::vector<std::size_t> lengths{3, 8, 9, 20, 5};
    std::size_t call = 0;
    bool fresh = true;
    auto& sender = us.attach<PeriodicSender>(
        Pgn{kPgnStatus}, 200ms,
        [&](std::vector<std::uint8_t>& out) {
            fresh = fresh && out.empty() && out.capacity() >= 20; // the sender's vector, room for max_length
            for (std::size_t i = 0; i < lengths[call % lengths.size()]; ++i)
            {
                out.push_back(static_cast<std::uint8_t>(call * 16 + i));
            }
            ++call;
        },
        options);
    bench.steps(85); // 0 .. 840 ms: 0, 200, 400, 600, 800 ms
    const auto frames = bench.take_sent();
    std::vector<std::size_t> single;
    std::vector<std::size_t> announced; // BAM: the total size of the message
    for (const auto& f : frames)
    {
        if (f.pgn == kPgnStatus)
        {
            single.push_back(f.data.size());
        }
        else if (f.pgn == kPgnTpCm && f.data[0] == 0x20)
        {
            announced.push_back(static_cast<std::size_t>(f.data[1] | (f.data[2] << 8)));
        }
    }
    CHECK(fresh);
    CHECK((single == std::vector<std::size_t>{3, 8, 5}));
    CHECK((announced == std::vector<std::size_t>{9, 20}));
    CHECK_EQ(sender.statistics().sent, 5u);
    CHECK_EQ(sender.statistics().failures, 0u);
    // The first message carries what the encoder appended.
    CHECK(!frames.empty() && frames[0] == frame(6, kPgnStatus, kUs, kGlobal, {0, 1, 2}));
}

TEST_CASE(periodic_sender_vector_encoder_longer_than_max_length_fails_the_network)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    PeriodicSenderOptions options;
    options.max_length = 4;
    us.attach<PeriodicSender>(Pgn{kPgnStatus}, 10ms, [](std::vector<std::uint8_t>& out) { out.assign(5, 0xAA); }, options);
    CHECK(throws_with<std::length_error>([&] { bench.step(); }, "the encoder wrote 5 bytes, more than max_length (4)"));
    CHECK(bench.network.failed());
    CHECK(bench.take_sent().empty());
    // An empty one is refused, a typed one is taken.
    CHECK(throws_with<std::invalid_argument>(
        [&] { PeriodicSender s(us, Pgn{kPgnStatus}, 10ms, PeriodicSender::VectorEncoder{}); }, "empty encoder"));
}

// --- Free timers, and parts destroyed in handlers

TEST_CASE(periodic_senders_made_in_a_handler_check_the_free_timers_and_refuse_the_rest)
{
    // A node that starts a stream per requester, from the handler, with the default options. With 4
    // timers, 10 requesters at once: 4 streams, 6 refused, and the network goes on. A Stop frees a
    // timer for the next.
    vehicle_sil::j1939::NetworkOptions options;
    options.timer_capacity = 4;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    std::map<Address, std::unique_ptr<PeriodicSender>> streams;
    std::size_t refused = 0;
    us.on_message(Pgn{kPgnPdu1}, [&](const vehicle_sil::j1939::Message& m) {
        if (m.data[0] == 0) // Stop
        {
            streams.erase(m.source);
            return;
        }
        if (us.network().timers_free() < PeriodicSender::kTimers)
        {
            ++refused;
            return;
        }
        PeriodicSenderOptions toRequester; // the defaults: to one address, DM13 not followed
        toRequester.destination = m.source;
        streams[m.source] = std::make_unique<PeriodicSender>(us, Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData),
                                                             toRequester);
    });
    for (Address a = 0x40; a < 0x4A; ++a)
    {
        bench.inject(frame(6, kPgnPdu1, a, kUs, {1}));
    }
    bench.steps(2);
    CHECK(!bench.network.failed());
    CHECK_EQ(streams.size(), 4u);
    CHECK_EQ(refused, 6u);
    CHECK_EQ(bench.network.timers_free(), 0u);
    bench.inject(frame(6, kPgnPdu1, 0x40, kUs, {0}));
    bench.inject(frame(6, kPgnPdu1, 0x50, kUs, {1}));
    bench.steps(2);
    CHECK(!bench.network.failed());
    CHECK(streams.contains(0x50) && !streams.contains(0x40));
    CHECK_EQ(refused, 6u);
}

TEST_CASE(periodic_sender_owned_by_the_application_may_be_destroyed_in_handlers_and_timers)
{
    {
        // In a message handler: nothing more from it, its timer is free again.
        Bench bench;
        Node& us = bench.add_node(kUs);
        const std::size_t free = bench.network.timers_free();
        PeriodicSenderOptions plain;
        plain.suppressible = false;
        auto sender = std::make_unique<PeriodicSender>(us, Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData), plain);
        us.on_message(Pgn{0xFF30}, [&](const vehicle_sil::j1939::Message&) { sender.reset(); });
        bench.steps(15); // 0, 100 ms
        bench.inject(frame(6, 0xFF30, 0x90, kGlobal, kData)); // handled at 150 ms
        bench.steps(20);
        CHECK(sender == nullptr);
        CHECK(sent_times_of(bench, kPgnStatus) == ms_list({0, 100}));
        CHECK_EQ(bench.network.timers_free(), free);
        CHECK(!bench.network.failed());
    }
    {
        // In an address change handler that runs before the sender's own, in the same delivery:
        // the sender's handler is not called (it would send at once when the claim starts).
        Bench bench;
        Node& us = bench.network.add_node(NodeOptions{kUs, name_of(0x100)});
        std::unique_ptr<PeriodicSender> sender;
        vehicle_sil::j1939::Subscription first{us, us.on_address_change([&](const vehicle_sil::j1939::AddressChange&) {
                                                   sender.reset();
                                               })};
        PeriodicSenderOptions plain;
        plain.suppressible = false;
        sender = std::make_unique<PeriodicSender>(us, Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData), plain);
        bench.steps(5);
        CHECK(sender == nullptr);
        CHECK(sent_times_of(bench, kPgnStatus).empty());
        CHECK(!bench.network.failed());
    }
    {
        // In the callback of another Timer due at the same deadline and started first: the
        // sender's deadline does not fire.
        Bench bench;
        Node& us = bench.add_node(kUs);
        PeriodicSenderOptions plain;
        plain.suppressible = false;
        std::unique_ptr<PeriodicSender> sender;
        vehicle_sil::j1939::Timer killer(us, vehicle_sil::j1939::TimerKind::Scheduled, [&] { sender.reset(); });
        killer.start_at(at(100ms));
        sender = std::make_unique<PeriodicSender>(us, Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData), plain);
        bench.steps(20);
        CHECK(sender == nullptr);
        CHECK(sent_times_of(bench, kPgnStatus) == ms_list({0}));
        CHECK(!bench.network.failed());
    }
}

// --- DM13 with a sender to one address, which constructor is taken, encoders that destroy
// their sender, and the Timers each part uses

TEST_CASE(periodic_sender_to_one_address_by_default_leaves_dm13_to_the_application)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    const std::size_t free = bench.network.timers_free();
    PeriodicSenderOptions toOne;
    toOne.destination = 0x30;
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnPdu1}, 100ms, std::span<const std::uint8_t>(kData), toOne);
    CHECK_EQ(free - bench.network.timers_free(), PeriodicSender::kTimers); // no BroadcastSuppression
    CHECK(us.find<BroadcastSuppression>() == nullptr);
    // To all nodes needs the node's BroadcastSuppression: refused, nothing changes.
    CHECK(throws_with<std::logic_error>([&] { sender.set_destination(kGlobal); },
                                        "call BroadcastSuppression::of(node) at set-up, or set "
                                        "PeriodicSenderOptions::suppressible"));
    CHECK_EQ(sender.destination(), 0x30);
    // Attached with options of its own after the sender (it used to be taken by the sender).
    vehicle_sil::j1939::BroadcastSuppressionOptions own;
    own.network = vehicle_sil::j1939::Dm13Network::J1939Network2;
    auto& suppression = BroadcastSuppression::attach(us, own);
    sender.set_destination(kGlobal);
    suppression.suspend(std::chrono::seconds(1));
    bench.steps(15); // 0, 100 ms
    CHECK_EQ(sender.statistics().suppressed, 2u);
    CHECK_EQ(sender.statistics().sent, 0u);
}

namespace {

template <typename F>
constexpr bool sender_from = std::is_constructible_v<PeriodicSender, Node&, Pgn, SimDuration, F>;

// Used only in decltype(): [[maybe_unused]] for Clang (-Wunneeded-internal-declaration).
[[maybe_unused]] std::size_t span_function(std::span<std::uint8_t> out) { return out.size(); }
[[maybe_unused]] void vector_function(std::vector<std::uint8_t>& out) { out.push_back(1); }

[[maybe_unused]] const auto kSpanLambda = [](std::span<std::uint8_t> out) { return out.size(); };
[[maybe_unused]] const auto kSpanOnlyGeneric = [](auto out) { return out.first(1).size_bytes(); }; // span members only
[[maybe_unused]] const auto kVectorLambda = [](std::vector<std::uint8_t>& out) { out.push_back(1); };
[[maybe_unused]] const auto kVectorGeneric = [](auto& out) { out.push_back(1); };
[[maybe_unused]] const auto kMutableVector = [n = 0](std::vector<std::uint8_t>& out) mutable { out.push_back(static_cast<std::uint8_t>(n++)); };
[[maybe_unused]] const auto kSpanWithoutReturn = [](std::span<std::uint8_t> out) { out[0] = 1; };
[[maybe_unused]] const auto kVectorCopy = [](std::vector<std::uint8_t> out) { out.push_back(1); };
[[maybe_unused]] const auto kVectorConst = [](const std::vector<std::uint8_t>& out) { static_cast<void>(out.size()); };
[[maybe_unused]] const auto kVectorWithLength = [](std::vector<std::uint8_t>& out) { return out.size(); };

static_assert(sender_from<decltype(kSpanLambda)>);
static_assert(sender_from<decltype(kSpanOnlyGeneric)>); // not tried with a vector (it would not compile)
static_assert(sender_from<decltype(kVectorLambda)>);
static_assert(sender_from<decltype(kVectorGeneric)>);
static_assert(sender_from<std::remove_const_t<decltype(kMutableVector)>>); // passed by value: not const
static_assert(sender_from<decltype(&span_function)>);
static_assert(sender_from<decltype(&vector_function)>);
static_assert(sender_from<PeriodicSender::Encoder>);
static_assert(sender_from<const PeriodicSender::Encoder&>);
static_assert(sender_from<PeriodicSender::VectorEncoder>);
static_assert(sender_from<const PeriodicSender::VectorEncoder&>);
// Lvalues, as users pass them: a named lambda, a function's name, a non-const Encoder or VectorEncoder.
static_assert(sender_from<std::remove_const_t<decltype(kSpanLambda)>&>);
static_assert(sender_from<std::remove_const_t<decltype(kVectorLambda)>&>);
static_assert(sender_from<decltype(span_function)&>);
static_assert(sender_from<decltype(vector_function)&>);
static_assert(sender_from<PeriodicSender::Encoder&>);
static_assert(sender_from<PeriodicSender::VectorEncoder&>);
static_assert(!sender_from<decltype(kSpanWithoutReturn)>); // a forgotten return stays an error
static_assert(!sender_from<decltype(kVectorCopy)>);
static_assert(!sender_from<decltype(kVectorConst)>);
static_assert(!sender_from<decltype(kVectorWithLength)>);

} // namespace

TEST_CASE(periodic_sender_takes_a_generic_encoder_by_its_parameter)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, [](auto& out) { out.assign(3, 7); }); // appends to the vector
    us.attach<PeriodicSender>(Pgn{kPgnStatus + 1}, 100ms, [](auto out) {                    // writes into the span
        out[0] = 9;
        return out.first(2).size();
    });
    bench.step();
    const auto frames = bench.take_sent();
    CHECK(frames.size() == 2 && frames[0] == frame(6, kPgnStatus, kUs, kGlobal, {7, 7, 7}));
    CHECK(frames.size() == 2 && frames[1].data.size() == 2 && frames[1].data[0] == 9);
}

TEST_CASE(periodic_sender_may_be_destroyed_by_its_encoder)
{
    {
        // At a deadline, by an Encoder and by a VectorEncoder: that message is not sent, what
        // the encoder captured and `out` are still there after the destruction.
        Bench bench;
        Node& us = bench.add_node(kUs);
        std::unique_ptr<PeriodicSender> span;
        std::unique_ptr<PeriodicSender> vector;
        std::vector<std::string> log;
        int spanCalls = 0;
        int vectorCalls = 0;
        span = std::make_unique<PeriodicSender>(
            us, Pgn{kPgnStatus}, 100ms, [&, note = std::string(40, 's')](std::span<std::uint8_t> out) {
                if (++spanCalls == 2)
                {
                    span.reset();
                    log.push_back(note);
                }
                out[0] = 1; // still valid after the sender went: lent to the encoder for the call
                return std::size_t{1};
            });
        vector = std::make_unique<PeriodicSender>(
            us, Pgn{kPgnStatus + 1}, 100ms, [&, note = std::string(40, 'v')](std::vector<std::uint8_t>& out) {
                if (++vectorCalls == 2)
                {
                    vector.reset();
                    log.push_back(note);
                }
                out.assign(8, 2); // still valid after the sender went
            });
        bench.steps(30);
        CHECK(span == nullptr);
        CHECK(vector == nullptr);
        CHECK((log == std::vector<std::string>{std::string(40, 's'), std::string(40, 'v')}));
        const auto sent = sent_of(bench, kPgnStatus, kPgnStatus + 1);
        CHECK(times_of(sent) == ms_list({0, 0})); // both at 0 ms; at 100 ms neither
        CHECK(!bench.network.failed());
    }
    {
        // The first message, when the node gets its address (the address change handler).
        Bench bench;
        Node& us = bench.network.add_node(NodeOptions{kUs, name_of(0x100)});
        std::unique_ptr<PeriodicSender> sender;
        sender = std::make_unique<PeriodicSender>(us, Pgn{kPgnStatus}, 100ms, [&](std::span<std::uint8_t>) {
            sender.reset();
            return std::size_t{1};
        });
        bench.steps(30);
        CHECK(sender == nullptr);
        CHECK(sent_times_of(bench, kPgnStatus).empty());
        CHECK(!bench.network.failed());
    }
}

TEST_CASE(periodic_sender_set_period_from_a_timer_and_from_the_encoder)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& byTimer = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    vehicle_sil::j1939::Timer timer(us, vehicle_sil::j1939::TimerKind::Scheduled, [&] { byTimer.set_period(30ms); });
    timer.start_at(at(150ms)); // from its deadline: 180, 210, ...
    PeriodicSender* self = nullptr;
    int calls = 0;
    auto& byEncoder = us.attach<PeriodicSender>(Pgn{kPgnStatus + 1}, 100ms, [&](std::span<std::uint8_t> out) {
        if (++calls == 2)
        {
            self->set_period(30ms); // at the 100 ms deadline: the next one 30 ms after it
        }
        out[0] = 0;
        return std::size_t{1};
    });
    self = &byEncoder;
    bench.steps(25); // 0 .. 240 ms
    const auto sent = sent_of(bench, kPgnStatus, kPgnStatus + 1);
    std::vector<SimTime> fromTimer;
    std::vector<SimTime> fromEncoder;
    for (const auto& x : sent)
    {
        (x.frame.pgn == kPgnStatus ? fromTimer : fromEncoder).push_back(x.time);
    }
    CHECK(fromTimer == ms_list({0, 100, 180, 210, 240}));
    CHECK(fromEncoder == ms_list({0, 100, 130, 160, 190, 220}));
}

TEST_CASE(periodic_sender_vector_encoder_that_appends_nothing_sends_an_empty_frame)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, [](std::vector<std::uint8_t>&) {});
    bench.step();
    const auto frames = bench.take_sent();
    CHECK(frames.size() == 1 && frames[0] == frame(6, kPgnStatus, kUs, kGlobal, {}));
    CHECK_EQ(sender.statistics().sent, 1u);
}

TEST_CASE(periodic_sender_may_be_destroyed_in_a_responder_a_transfer_end_and_its_owner_s_timer)
{
    PeriodicSenderOptions plain;
    plain.suppressible = false;
    {
        // A responder of a Request to all nodes.
        Bench bench;
        Node& us = bench.add_node(kUs);
        auto sender = std::make_unique<PeriodicSender>(us, Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData), plain);
        us.on_request(Pgn{0xFF30}, [&](vehicle_sil::j1939::RequestContext&) { sender.reset(); });
        bench.steps(5); // 0 ms
        bench.inject(frame(6, kPgnRequest, 0x90, kGlobal, {0x30, 0xFF, 0x00})); // handled at 50 ms
        bench.steps(20);
        CHECK(sender == nullptr);
        CHECK(sent_times_of(bench, kPgnStatus) == ms_list({0}));
        CHECK(!bench.network.failed());
    }
    {
        // The end of its own transfer (a BAM).
        Bench bench;
        Node& us = bench.add_node(kUs);
        PeriodicSenderOptions longOne = plain;
        longOne.max_length = 20;
        const Bytes data = payload(20);
        auto sender = std::make_unique<PeriodicSender>(us, Pgn{kPgnStatus}, 300ms, std::span<const std::uint8_t>(data), longOne);
        us.on_transfer_end([&](const vehicle_sil::j1939::TransferEnd&) { sender.reset(); });
        bench.steps(100); // 0 .. 990 ms
        CHECK(sender == nullptr);
        std::size_t announcements = 0;
        for (const auto& f : bench.take_sent())
        {
            announcements += f.pgn == kPgnTpCm ? 1u : 0u;
        }
        CHECK_EQ(announcements, 1u);
        CHECK(!bench.network.failed());
    }
    {
        // A Timer of the object that owns it (a lease), which goes with it.
        struct Stream
        {
            std::unique_ptr<PeriodicSender> sender;
            std::unique_ptr<vehicle_sil::j1939::Timer> lease;
        };
        Bench bench;
        Node& us = bench.add_node(kUs);
        auto stream = std::make_unique<Stream>();
        stream->sender = std::make_unique<PeriodicSender>(us, Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData), plain);
        stream->lease = std::make_unique<vehicle_sil::j1939::Timer>(us, vehicle_sil::j1939::TimerKind::Timeout, [&] { stream.reset(); });
        stream->lease->start_at(at(150ms));
        const std::size_t free = bench.network.timers_free();
        bench.steps(40);
        CHECK(stream == nullptr);
        CHECK(sent_times_of(bench, kPgnStatus) == ms_list({0, 100}));
        CHECK_EQ(bench.network.timers_free(), free + 2);
        CHECK(!bench.network.failed());
    }
}

TEST_CASE(parts_use_the_timers_their_k_timers_say)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    const auto uses = [&](auto make) {
        const std::size_t before = bench.network.timers_free();
        make();
        return before - bench.network.timers_free();
    };
    CHECK_EQ(uses([&] { BroadcastSuppression::of(us); }), BroadcastSuppression::kTimers);
    CHECK_EQ(uses([&] { us.attach<vehicle_sil::j1939::Diagnostics>(); }), vehicle_sil::j1939::Diagnostics::kTimers);
    CHECK_EQ(uses([&] { us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData)); }),
             PeriodicSender::kTimers);
    CHECK_EQ(uses([&] { vehicle_sil::j1939::Requester::of(us); }), vehicle_sil::j1939::Requester::kTimers);
    CHECK_EQ(uses([&] { us.attach<vehicle_sil::j1939::Exchange>(); }), vehicle_sil::j1939::Exchange::kTimers);
    CHECK_EQ(uses([&] { us.attach<vehicle_sil::j1939::AddressCommander>(); }), vehicle_sil::j1939::AddressCommander::kTimers);
    vehicle_sil::j1939::MonitorOptions timed;
    timed.timeout = 100ms;
    CHECK_EQ(uses([&] { us.attach<vehicle_sil::j1939::Monitor>(Pgn{kPgnStatus}, timed); }), vehicle_sil::j1939::Monitor::kTimers);
    CHECK_EQ(uses([&] { us.attach<vehicle_sil::j1939::Monitor>(Pgn{kPgnStatus}); }), 0u);
}

TEST_CASE(periodic_sender_set_period_from_an_observer_throws_and_changes_nothing)
{
    // An Observer may not start timers: while the sender's timer runs, set_period() throws (and
    // fails the network), and the sender keeps the period it had.
    struct Changer final : vehicle_sil::j1939::Observer
    {
        PeriodicSender* sender = nullptr;
        bool threw = false;
        SimDuration before{};
        int calls = 0;
        void on_event(const vehicle_sil::j1939::Event&) override
        {
            if (sender != nullptr && !threw)
            {
                before = sender->period();
                threw = throws<std::logic_error>([&] { sender->set_period(30ms + std::chrono::milliseconds(++calls)); });
            }
        }
    };
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& sender = us.attach<PeriodicSender>(Pgn{kPgnStatus}, 100ms, std::span<const std::uint8_t>(kData));
    bench.step(); // running
    Changer changer;
    changer.sender = &sender;
    bench.network.set_observer(&changer);
    static_cast<void>(throws<std::logic_error>([&] { bench.steps(10); })); // the frame at 100 ms is an event
    bench.network.set_observer(nullptr);
    CHECK(changer.threw);
    CHECK(sender.period() == changer.before);
}
