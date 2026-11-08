// SPDX-License-Identifier: MIT
//
// The PeriodicSender part of the J1939 stack: the period on the virtual-time grid, missed
// deadlines (Coalesce, CatchUp), the node's address, DM13, trigger(), pause(), long messages.

#include "j1939_bench.hpp"
#include "j1939_parts_util.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/broadcast_suppression.hpp"
#include "vehicle_sil/j1939/periodic_sender.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
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
