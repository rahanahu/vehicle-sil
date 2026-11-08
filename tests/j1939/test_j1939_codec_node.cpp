// SPDX-License-Identifier: MIT
//
// Typed J1939 messages on a node (codec_node.hpp): the periodic sender with the message's
// priority and length, typed handlers (also several types of one PGN told apart by their
// selector), one-off sends, and the round trip between two networks with the transport protocol.

#include "codec_test_messages.hpp"
#include "j1939_bench.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/codec_node.hpp"
#include "vehicle_sil/j1939/monitor.hpp"
#include "vehicle_sil/testkit/local_can.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using namespace codec_test;
using vehicle_sil::SimDuration;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Monitor;
using vehicle_sil::j1939::MonitorOptions;
using vehicle_sil::j1939::MonitorState;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::PeriodicSenderOptions;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::codec::SignalState;
namespace codec = vehicle_sil::j1939::codec;
namespace testkit = vehicle_sil::testkit;

namespace {

constexpr Address kUs = 0x20;
constexpr Address kPeer = 0x30;

template <typename M>
Bytes bytes_of(const M& message)
{
    const auto b = message.bytes();
    return Bytes(b.begin(), b.end());
}

} // namespace

TEST_CASE(attach_sender_sends_a_new_message_with_its_priority_every_period)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    auto& sender = codec::attach_sender<Status>(us, 100ms, [&](Status& s) {
        CHECK(s.speed.state() == SignalState::NotAvailable); // a new message every time
        if (calls % 2 == 0)
        {
            s.speed.set(100.0 * calls);
        }
        ++calls;
    });
    CHECK(sender.pgn() == Status::kPgn);
    bench.steps(30); // 0 .. 290 ms
    const auto frames = bench.take_sent();
    CHECK_EQ(frames.size(), 3u);
    Status first;
    first.speed.set(0.0);
    Status second; // not set: not available
    Status third;
    third.speed.set(200.0);
    CHECK(frames[0] == frame(3, 0xFF10, kUs, kGlobal, bytes_of(first)));
    CHECK(frames[1] == frame(3, 0xFF10, kUs, kGlobal, bytes_of(second)));
    CHECK(frames[2] == frame(3, 0xFF10, kUs, kGlobal, bytes_of(third)));
}

TEST_CASE(attach_sender_takes_the_priority_and_length_from_the_message)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    // Another priority than the definition's is refused; max_length grows to the message (20
    // bytes: the transport protocol, frames of priority 7).
    CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>(
        [&] { codec::attach_sender<Status>(us, 1s, [](Status&) {}, PeriodicSenderOptions{.priority = 2}); },
        "the message's definition gives priority 3, options.priority is 2"));
    codec::attach_sender<Status>(us, 1s, [](Status&) {}, PeriodicSenderOptions{.priority = 3}).pause();
    auto& sender = codec::attach_sender<Wide>(us, 1s, [](Wide& w) { w.total.set_raw(0x0102030405060708); });
    bench.steps(20);
    CHECK_EQ(sender.statistics().sent, 1u);
    CHECK_EQ(sender.statistics().failures, 0u);
    const auto frames = bench.take_sent();
    CHECK(!frames.empty());
    CHECK_EQ(frames.front().pgn, 0xEC00u); // TP.CM (BAM)
    CHECK_EQ(frames.front().data[0], 0x20); // BAM
    CHECK_EQ(frames.front().data[1] | (frames.front().data[2] << 8), 20);

    // A sender made by hand with a short buffer fails the network at the first send.
    Bench short_buffer;
    Node& node = short_buffer.add_node(kUs);
    node.attach<vehicle_sil::j1939::PeriodicSender>(Wide::kPgn, 1s, codec::encoder<Wide>([](Wide&) {}));
    CHECK(vehicle_sil::unit::throws_with<std::length_error>([&] { short_buffer.steps(1); }, "needs 20 bytes, the buffer has 8"));
    CHECK(short_buffer.network.failed());
}

TEST_CASE(typed_handlers_receive_their_own_messages_only)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<SetPoint> set_points;
    std::vector<std::pair<Reset, Address>> resets;
    codec::on_message<SetPoint>(us, [&](const SetPoint& p) { set_points.push_back(p); });
    const auto id = codec::on_message<Reset>(us, [&](const Reset& r, const Message& m) { resets.emplace_back(r, m.source); });

    SetPoint p;
    p.target.set(42.0);
    Reset r;
    r.what.set(Reset::What::all);
    bench.inject(frame(6, 0xEF00, kPeer, kUs, bytes_of(p)));
    bench.inject(frame(6, 0xEF00, kPeer, kUs, bytes_of(r)));
    bench.inject(frame(6, 0xEF00, kPeer, kUs, {0x01, 0x02}));                          // too short
    bench.inject(frame(6, 0xEF00, kPeer, kUs, {0x03, 0, 0, 0, 0, 0, 0, 0}));           // another selector
    bench.inject(frame(6, 0xEF00, kPeer, 0x55, bytes_of(p)));                          // to another node
    bench.steps(2);
    CHECK_EQ(set_points.size(), 1u);
    CHECK(set_points[0] == p);
    CHECK(set_points[0].target.value() == std::optional<double>(42.0));
    CHECK_EQ(resets.size(), 1u);
    CHECK(resets[0].first.what.value() == std::optional<Reset::What>(Reset::What::all));
    CHECK_EQ(resets[0].second, kPeer);

    us.remove_handler(id);
    bench.inject(frame(6, 0xEF00, kPeer, kUs, bytes_of(r)));
    bench.steps(1);
    CHECK_EQ(resets.size(), 1u);
}

TEST_CASE(send_uses_the_pgn_and_priority_of_the_message)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    SetPoint p;
    p.target.set(-1.0);
    std::vector<SendStatus> results;
    bench.app = [&](vehicle_sil::SimTime) {
        if (results.empty())
        {
            results.push_back(codec::send(us, p, kPeer));
            Status s;
            results.push_back(codec::send(us, s));
        }
    };
    bench.steps(2);
    CHECK(results == (std::vector<SendStatus>{SendStatus::Ok, SendStatus::Ok}));
    const auto frames = bench.take_sent();
    CHECK_EQ(frames.size(), 2u);
    CHECK(frames[0] == frame(6, 0xEF00, kUs, kPeer, bytes_of(p)));
    CHECK(frames[1] == frame(3, 0xFF10, kUs, kGlobal, bytes_of(Status{})));
}

TEST_CASE(typed_messages_between_two_networks)
{
    // An engine sends Status (8 bytes) and Wide (20 bytes, BAM); a cab reads them with a typed
    // handler and with a Monitor.
    testkit::LocalCan bus;
    Node& engine = bus.add_j1939("engine", 10ms).add_node({.address = 0x00});
    Node& cab = bus.add_j1939("cab", 5ms).add_node({.address = 0x80});
    double rpm = 600.0;
    std::uint64_t odometer = 0;
    codec::attach_sender<Status>(engine, 20ms, [&](Status& s) {
        s.speed.set(rpm);
        s.mode.set(Status::Mode::on);
    });
    // A BAM of 3 packets takes 150 ms at least: one every 250 ms.
    auto& wide = codec::attach_sender<Wide>(engine, 250ms, [&](Wide& w) {
        w.distance.set(12.5);
        w.total.set_raw(odometer++);
        w.delta.set_raw(-5);
    });
    bus.on_step("engine", [&](vehicle_sil::SimTime) { rpm += 1.0; });

    std::vector<Wide> wides;
    codec::on_message<Wide>(cab, [&](const Wide& w) { wides.push_back(w); });
    auto& status = cab.attach<Monitor>(Status::kPgn, MonitorOptions{.source = 0x00, .timeout = 100ms});
    bus.run_for(1s);

    CHECK(status.state() == MonitorState::Receiving);
    const auto latest = Status::decode(status.data());
    CHECK(latest.has_value());
    CHECK(latest->mode.value() == std::optional<Status::Mode>(Status::Mode::on));
    CHECK(latest->speed.value_or(0.0) > 600.0);
    CHECK(!Status::decode(std::span<const std::uint8_t>{}).has_value()); // what a Waiting monitor has

    CHECK_EQ(wide.statistics().failures, 0u);
    CHECK_EQ(wides.size(), 4u); // 0, 250, 500 and 750 ms
    for (std::size_t i = 0; i < wides.size(); ++i)
    {
        CHECK(wides[i].distance.value() == std::optional<double>(12.5));
        CHECK_EQ(wides[i].total.raw(), static_cast<std::uint64_t>(i));
        CHECK_EQ(wides[i].delta.raw(), std::int64_t{-5});
        CHECK(wides[i].custom.state() == SignalState::NotAvailable);
    }
}
