// SPDX-License-Identifier: MIT
//
// The broadcast suppression part (DM13) of the J1939 stack: stop and start for this
// node's network, the 6 s without DM13, the suspend signal of others (not a command), this
// node's own suspension and its announcement, and one state per node that the DM1 follows.
#include "j1939_parts_util.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/broadcast_suppression.hpp"
#include "vehicle_sil/j1939/diagnostics.hpp"

#include <chrono>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::BroadcastSuppression;
using vehicle_sil::j1939::BroadcastSuppressionOptions;
using vehicle_sil::j1939::Diagnostics;
using vehicle_sil::j1939::Dm13Network;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::SuspendScope;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kUs = 0x20;
constexpr Address kTool = 0x90;

J1939Frame dm13_from(Address sa, Address da, Bytes data) { return frame(6, kPgnDm13, sa, da, std::move(data)); }
J1939Frame req(Address to, std::uint32_t pgn) { return frame(6, kPgnRequest, kTool, to, request(pgn)); }

} // namespace

TEST_CASE(dm13_stop_and_start_for_our_network_and_requests_are_still_answered)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    us.attach<Diagnostics>();
    auto& suppression = BroadcastSuppression::of(us);
    CHECK(&suppression == us.find<BroadcastSuppression>());
    bench.steps(150); // 0 .. 1490: DM1 at 0 and 1000
    (void)bench.take_sent();

    bench.inject(dm13_from(kTool, kGlobal, dm13(kNetwork1, kStop)));
    bench.step();
    CHECK(suppression.suspended());
    CHECK(suppression.stopped_by_dm13());
    bench.inject(req(kUs, kPgnDm1));
    auto frames = record(bench, 300); // 3 s: no periodic DM1, the answer only
    CHECK_EQ(only(frames, kPgnDm1).size(), 1u);
    CHECK_EQ(only(frames, kPgnDm1)[0].ms, 1510);
    CHECK_EQ(us.find<Diagnostics>()->statistics().broadcasts_suppressed, 3u);

    bench.inject(dm13_from(kTool, kUs, dm13(kNetwork1, kStart)));
    bench.step(); // 4510
    CHECK(!suppression.suspended());
    frames = record(bench, 100);
    // The period went on: the next DM1 at 5000.
    CHECK_EQ(only(frames, kPgnDm1).size(), 1u);
    CHECK_EQ(only(frames, kPgnDm1)[0].ms, 5000);
}

TEST_CASE(dm13_stop_ends_six_seconds_after_the_last_dm13_whatever_the_step_size)
{
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{10ms}, SimDuration{40ms}})
    {
        Bench bench({}, step);
        Node& us = bench.add_node(kUs);
        auto& suppression = BroadcastSuppression::of(us);
        bench.inject(at(100ms), dm13_from(kTool, kGlobal, dm13(kNetwork1, kStop)));
        // A hold signal (no command for us) at 3 s and a DM13 for another network exactly 6 s
        // later: both count again from their time.
        bench.inject(at(3100ms), dm13_from(kTool, kGlobal, dm13(-1, 0, 0xF, 0xFFFF, 0x0)));
        bench.inject(at(9100ms), dm13_from(kTool, kGlobal, dm13(kJ1587, kStop)));
        std::optional<long long> resumed;
        while (!resumed && bench.next_time() <= at(20s))
        {
            bench.step();
            if (bench.last_time() > at(100ms) && !suppression.suspended())
            {
                resumed = to_ns(bench.last_time()) / 1000000;
            }
            if (bench.last_time() > at(100ms) && bench.last_time() <= at(15100ms))
            {
                CHECK(suppression.suspended() || bench.last_time() <= at(100ms));
            }
        }
        // The first step after 15.1 s (a DM13 exactly at the deadline would still keep it).
        const long long stepMs = step.count() / 1000000;
        CHECK(resumed.has_value());
        CHECK_EQ(*resumed, (15100 / stepMs + 1) * stepMs);
    }
}

TEST_CASE(dm13_reaches_our_network_wherever_its_bits_are_and_the_current_data_link_stops_every_network)
{
    for (const auto& [network, index] : {std::pair{Dm13Network::J1939Network1, kNetwork1},
                                         std::pair{Dm13Network::J1939Network2, kNetwork2},
                                         std::pair{Dm13Network::ProprietaryNetwork1, kProprietary1}})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        bench.step(); // frames injected from here on reach the network in the next step
        auto& suppression = us.attach<BroadcastSuppression>(BroadcastSuppressionOptions{network});
        bench.inject(dm13_from(kTool, kGlobal, dm13(kJ1587, kStop)));
        bench.step();
        CHECK(!suppression.suspended());
        if (network != Dm13Network::J1939Network1)
        {
            bench.inject(dm13_from(kTool, kGlobal, dm13(kNetwork1, kStop)));
            bench.step();
            CHECK(!suppression.suspended());
        }
        bench.inject(dm13_from(kTool, kGlobal, dm13(index, kStop)));
        bench.step();
        CHECK(suppression.suspended());
        bench.inject(dm13_from(kTool, kGlobal, dm13(index, kStart)));
        bench.step();
        CHECK(!suppression.suspended());
        bench.inject(dm13_from(kTool, kGlobal, dm13(kCurrentDataLink, kStop)));
        bench.step();
        CHECK(suppression.suspended());
        bench.inject(dm13_from(kTool, kGlobal, dm13(kCurrentDataLink, kStart)));
        bench.step();
        CHECK(!suppression.suspended());
    }
}

TEST_CASE(dm13_suspend_signal_of_another_node_is_not_a_command)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.step(); // frames injected from here on reach the network in the next step
    auto& suppression = BroadcastSuppression::of(us);
    // Only the sender's own temporary suspension for 3 s: no stop for us.
    bench.inject(dm13_from(kTool, kGlobal, dm13(-1, 0, 0x2, 3)));
    bench.step();
    CHECK(!suppression.suspended());
    // A stop with the sender's suspension of 3 s: stopped until 6 s without DM13, not 3 s.
    bench.inject(dm13_from(kTool, kGlobal, dm13(kNetwork1, kStop, 0x2, 3))); // at 10 ms
    bench.steps(400); // 20 .. 4010
    CHECK(suppression.suspended());
    bench.steps(200); // .. 6010: the timeout at 6010 waits for a later step
    CHECK(suppression.suspended());
    bench.step(); // 6020
    CHECK(!suppression.suspended());
}

TEST_CASE(dm13_shorter_than_eight_bytes_or_to_another_node_is_ignored)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.step(); // frames injected from here on reach the network in the next step
    auto& suppression = BroadcastSuppression::of(us);
    Bytes shortOne = dm13(kNetwork1, kStop);
    shortOne.resize(7);
    bench.inject(dm13_from(kTool, kGlobal, shortOne));
    bench.inject(dm13_from(kTool, 0x91, dm13(kNetwork1, kStop)));
    bench.step();
    CHECK(!suppression.suspended());
}

TEST_CASE(our_own_suspension_is_announced_and_ends_after_its_duration_or_on_resume)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    us.attach<Diagnostics>();
    auto& suppression = BroadcastSuppression::of(us);
    bench.steps(51); // 0 .. 500
    (void)bench.take_sent();
    suppression.suspend(std::chrono::seconds(3)); // between steps: announced in the next one
    CHECK(suppression.suspended());
    CHECK(!suppression.stopped_by_dm13());
    auto frames = record(bench, 400); // 510 .. 4500
    CHECK_EQ(only(frames, kPgnDm13).size(), 1u);
    CHECK_EQ(only(frames, kPgnDm13)[0].ms, 510);
    CHECK(only(frames, kPgnDm13)[0].frame == frame(6, kPgnDm13, kUs, kGlobal, {0xFF, 0xFF, 0xFF, 0xF2, 0x03, 0x00, 0xFF, 0xFF}));
    // Suspended from 500 to 3500 (counted from the step time when suspend() was called): the
    // DM1 at 1000, 2000 and 3000 are suppressed, the one at 4000 goes out.
    const auto dm1 = only(frames, kPgnDm1);
    CHECK_EQ(dm1.size(), 1u);
    CHECK_EQ(dm1[0].ms, 4000);

    suppression.suspend(std::nullopt, SuspendScope::Some);
    frames = record(bench, 200);
    CHECK(only(frames, kPgnDm13)[0].frame == frame(6, kPgnDm13, kUs, kGlobal, {0xFF, 0xFF, 0xFF, 0xF3, 0xFF, 0xFF, 0xFF, 0xFF}));
    CHECK(only(frames, kPgnDm1).empty());
    suppression.resume();
    CHECK(!suppression.suspended());
    CHECK_EQ(only(record(bench, 100), kPgnDm1).size(), 1u);
    CHECK(throws<std::invalid_argument>([&] { suppression.suspend(std::chrono::seconds(64256)); }));
    CHECK(throws<std::invalid_argument>([&] { suppression.suspend(std::chrono::seconds(-1)); }));
}

TEST_CASE(a_node_has_one_broadcast_suppression)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Node& other = bench.add_node(0x21);
    auto& ours = BroadcastSuppression::of(us);
    CHECK(&BroadcastSuppression::of(us) == &ours);
    CHECK(&BroadcastSuppression::of(other) != &ours);
    CHECK(throws<std::logic_error>([&] { us.attach<BroadcastSuppression>(); }));
    // Every part of the node reads the same state.
    auto& diagnostics = us.attach<Diagnostics>();
    static_cast<void>(diagnostics);
    ours.suspend(std::nullopt);
    bench.steps(5);
    CHECK_EQ(us.find<Diagnostics>()->statistics().broadcasts_suppressed, 1u);
    CHECK(other.find<Diagnostics>() == nullptr);
}

TEST_CASE(a_suspension_resumed_before_it_was_announced_is_not_announced)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& suppression = BroadcastSuppression::of(us);
    bench.step();
    suppression.suspend(std::chrono::seconds(5));
    suppression.resume();
    CHECK(only(record(bench, 10), kPgnDm13).empty());
}
