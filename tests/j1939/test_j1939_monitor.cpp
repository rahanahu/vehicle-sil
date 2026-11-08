// SPDX-License-Identifier: MIT
//
// The Monitor and AddressCommander parts of the J1939 stack: the latest message and the loss of
// its sender (by address and by NAME), and sending Commanded Address.

#include "j1939_bench.hpp"
#include "j1939_parts_util.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/address_commander.hpp"
#include "vehicle_sil/j1939/monitor.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AddressCommander;
using vehicle_sil::j1939::CommandOutcome;
using vehicle_sil::j1939::CommandResult;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Monitor;
using vehicle_sil::j1939::MonitorChange;
using vehicle_sil::j1939::MonitorOptions;
using vehicle_sil::j1939::MonitorState;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Node;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kUs = 0x20;
constexpr std::uint32_t kPgnSpeed = 0xFEF1; // PDU2
const Bytes kSpeed{1, 2, 3, 4, 5, 6, 7, 8};
const Name kEngine = Name{}.set_identity_number(0x55).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true);

J1939Frame speed_from(Address sa, Bytes data = kSpeed) { return frame(6, kPgnSpeed, sa, kGlobal, std::move(data)); }

J1939Frame claim_of(Address sa, Name name)
{
    const auto bytes = name.to_bytes();
    return frame(6, kPgnAddressClaim, sa, kGlobal, Bytes(bytes.begin(), bytes.end()));
}

} // namespace

TEST_CASE(monitor_keeps_the_latest_message_and_reports_its_state)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<MonitorChange> changes;
    int messages = 0;
    MonitorOptions options;
    options.timeout = 100ms;
    Monitor* self = nullptr;
    options.on_state = [&](const MonitorChange& c) {
        CHECK(self->state() == c.state); // already changed when it is told
        changes.push_back(c);
    };
    options.on_message = [&](const Message&) { ++messages; };
    auto& monitor = us.attach<Monitor>(Pgn{kPgnSpeed}, options);
    self = &monitor;
    CHECK(monitor.state() == MonitorState::Waiting);
    CHECK(monitor.data().empty() && !monitor.time() && monitor.count() == 0);
    bench.steps(5); // 0 .. 40: nothing, still Waiting (the timeout counts from the first message)
    CHECK(monitor.state() == MonitorState::Waiting);
    bench.inject(speed_from(0x30)); // at 40 ms
    bench.steps(2);
    CHECK(monitor.state() == MonitorState::Receiving);
    CHECK(monitor.data().size() == 8 && monitor.data()[0] == 1);
    CHECK(monitor.time() == at(40ms));
    CHECK_EQ(monitor.source(), 0x30);
    bench.inject(at(140ms), speed_from(0x31, Bytes{9, 9})); // exactly the timeout later: in time
    bench.steps(20); // .. 250: Lost at 240 ms
    CHECK(monitor.state() == MonitorState::Lost);
    CHECK_EQ(monitor.count(), 2u);
    CHECK_EQ(monitor.losses(), 1u);
    CHECK(monitor.data().size() == 2 && monitor.source() == 0x31); // the latest stays readable
    bench.inject(speed_from(0x30));
    bench.steps(2);
    CHECK(monitor.state() == MonitorState::Receiving);
    CHECK_EQ(messages, 3);
    CHECK_EQ(changes.size(), 3u);
    CHECK(changes.size() == 3 && changes[0].state == MonitorState::Receiving && changes[0].previous == MonitorState::Waiting &&
          changes[0].time == at(40ms));
    CHECK(changes.size() == 3 && changes[1].state == MonitorState::Lost && changes[1].time == at(240ms));
    CHECK(changes.size() == 3 && changes[2].state == MonitorState::Receiving);
}

TEST_CASE(monitor_reports_the_loss_at_the_same_time_whatever_the_step)
{
    for (const SimDuration step : {1ms, 7ms, 25ms, 50ms})
    {
        Bench bench({}, step);
        Node& us = bench.add_node(kUs);
        MonitorOptions options;
        options.timeout = 100ms;
        SimTime lostAt{};
        options.on_state = [&](const MonitorChange& c) {
            if (c.state == MonitorState::Lost)
            {
                lostAt = c.time;
            }
        };
        Monitor monitor(us, Pgn{kPgnSpeed}, options);
        bench.inject(at(0ms), speed_from(0x30));
        bench.inject(at(60ms), speed_from(0x30));
        while (bench.next_time() < at(400ms))
        {
            bench.step();
        }
        CHECK(monitor.state() == MonitorState::Lost);
        CHECK(lostAt == at(160ms));
        CHECK_EQ(monitor.losses(), 1u);
    }
}

TEST_CASE(monitor_takes_messages_from_one_address_or_one_name)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    MonitorOptions fromAddress;
    fromAddress.source = 0x31;
    Monitor byAddress(us, Pgn{kPgnSpeed}, fromAddress);
    MonitorOptions fromName;
    fromName.name = kEngine;
    Monitor byName(us, Pgn{kPgnSpeed}, fromName);
    bench.step(); // frames at a step's time arrive in the step after it
    bench.inject(speed_from(0x30)); // nobody claimed with kEngine yet
    bench.inject(speed_from(0x31));
    bench.step();
    CHECK_EQ(byAddress.count(), 1u);
    CHECK_EQ(byName.count(), 0u);
    bench.inject(claim_of(0x30, kEngine));
    bench.step();
    bench.inject(speed_from(0x30));
    bench.inject(speed_from(0x31));
    bench.step();
    CHECK_EQ(byName.count(), 1u);
    CHECK_EQ(byName.source(), 0x30);
    bench.inject(claim_of(0x32, kEngine)); // the engine moves
    bench.step();
    bench.inject(speed_from(0x30));
    bench.inject(speed_from(0x32));
    bench.step();
    CHECK_EQ(byName.count(), 2u);
    CHECK_EQ(byName.source(), 0x32);
    CHECK_EQ(byAddress.count(), 2u);
}

TEST_CASE(monitor_follows_a_name_of_a_node_of_its_own_network)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Node& engine = bench.network.add_node({0x90, kEngine});
    MonitorOptions options;
    options.name = kEngine;
    Monitor monitor(us, Pgn{kPgnSpeed}, options);
    bench.steps(26); // claimed at 250 ms
    bench.app = [&](SimTime now) {
        if (now == at(260ms))
        {
            CHECK(engine.send(Pgn{kPgnSpeed}, kSpeed) == vehicle_sil::j1939::SendStatus::Ok);
        }
    };
    bench.steps(3);
    CHECK_EQ(monitor.count(), 1u);
    CHECK_EQ(monitor.source(), 0x90);
}

TEST_CASE(monitor_takes_only_what_accept_accepts)
{
    // A check of one's own (here: byte 0 is a counter that must go up by one, byte 7 a checksum
    // of bytes 0..6). Refused messages are not kept and do not keep the monitor Receiving.
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::optional<std::uint8_t> last;
    MonitorOptions options;
    options.timeout = SimDuration{std::chrono::milliseconds(50)};
    options.accept = [&](const Message& m) {
        std::uint8_t sum = 0;
        for (std::size_t i = 0; i < 7; ++i)
        {
            sum = static_cast<std::uint8_t>(sum + m.data[i]);
        }
        const bool ok = m.data.size() == 8 && m.data[7] == sum && (!last || m.data[0] == static_cast<std::uint8_t>(*last + 1));
        if (ok)
        {
            last = m.data[0];
        }
        return ok;
    };
    auto& monitor = us.attach<Monitor>(vehicle_sil::j1939::Pgn{kPgnSpeed}, options);
    const auto message = [](std::uint8_t counter, bool good) {
        Bytes data{counter, 1, 2, 3, 4, 5, 6, 0};
        std::uint8_t sum = 0;
        for (std::size_t i = 0; i < 7; ++i)
        {
            sum = static_cast<std::uint8_t>(sum + data[i]);
        }
        data[7] = good ? sum : static_cast<std::uint8_t>(sum + 1);
        return data;
    };
    bench.inject(at(5ms), speed_from(0x30, message(1, true)));
    bench.inject(at(25ms), speed_from(0x30, message(2, false))); // bad checksum
    bench.inject(at(45ms), speed_from(0x30, message(4, true)));  // a counter jump
    bench.steps(6);
    CHECK_EQ(monitor.count(), 1u);
    CHECK_EQ(monitor.rejected(), 2u);
    CHECK(monitor.data().size() == 8 && monitor.data()[0] == 1);
    bench.steps(3); // 50 ms after the only accepted message: Lost, whatever came since
    CHECK(monitor.state() == MonitorState::Lost);
    bench.inject(at(85ms), speed_from(0x30, message(2, true)));
    bench.steps(2);
    CHECK(monitor.state() == MonitorState::Receiving && monitor.count() == 2u);
}

TEST_CASE(monitor_keeps_up_to_max_length_bytes)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    MonitorOptions options;
    options.max_length = 4;
    Monitor monitor(us, Pgn{kPgnSpeed}, options);
    bench.inject(speed_from(0x30));
    bench.steps(2);
    CHECK(monitor.data().size() == 4 && monitor.data()[3] == 4);
    CHECK_EQ(monitor.length(), 8u);
}

TEST_CASE(monitor_refuses_invalid_settings)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    MonitorOptions both;
    both.source = 0x30;
    both.name = kEngine;
    CHECK(throws<std::invalid_argument>([&] { Monitor m(us, Pgn{kPgnSpeed}, both); }));
    MonitorOptions zero;
    zero.timeout = 0ms;
    CHECK(throws<std::invalid_argument>([&] { Monitor m(us, Pgn{kPgnSpeed}, zero); }));
    CHECK(throws<std::invalid_argument>([&] { Monitor m(us, Pgn{0xEF05}, {}); }));
}

TEST_CASE(address_commander_sends_the_command_and_reports_the_move)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    auto& commander = us.attach<AddressCommander>();
    std::vector<CommandResult> results;
    CHECK(commander.command(kEngine, 0x40, [&](const CommandResult& r) { results.push_back(r); }));
    CHECK_EQ(commander.pending(), 1u);
    bench.steps(3); // 0: BAM announcement, 50 and 100 ms: the packets (10 ms steps)
    bench.steps(8);
    const auto frames = bench.take_sent();
    const auto name = kEngine.to_bytes();
    Bytes command(name.begin(), name.end());
    command.push_back(0x40);
    CHECK(frames == (std::vector<J1939Frame>{bam(kUs, 9, 0xFED8)} + dts(kUs, kGlobal, command, 1, 2)));
    CHECK(results.empty());
    bench.inject(claim_of(0x41, kEngine)); // not the commanded address
    bench.step();
    CHECK(results.empty());
    bench.inject(claim_of(0x40, kEngine));
    bench.step();
    CHECK_EQ(results.size(), 1u);
    CHECK(results.size() == 1 && results[0].outcome == CommandOutcome::Moved && results[0].address == 0x40 &&
          results[0].target == kEngine);
    CHECK_EQ(commander.pending(), 0u);
}

TEST_CASE(address_commander_waits_for_a_transfer_of_the_node_under_way)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    AddressCommander commander(us);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{0xFF60}, payload(20)) == vehicle_sil::j1939::SendStatus::Ok); // a BAM
            CHECK(commander.command(kEngine, 0x40)); // its BAM waits for that one
        }
    };
    bench.steps(40);
    const auto frames = bench.take_sent();
    std::size_t announcements = 0;
    for (const auto& f : frames)
    {
        announcements += f.pgn == kPgnTpCm && f.data[0] == 0x20 ? 1 : 0;
    }
    CHECK_EQ(announcements, 2u);
    CHECK_EQ(commander.pending(), 1u); // sent, waiting for the claim
}

TEST_CASE(address_commander_times_out_counting_from_the_end_of_the_transfer)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    AddressCommander commander(us);
    std::vector<CommandResult> results;
    CHECK(commander.command(kEngine, 0x40, [&](const CommandResult& r) { results.push_back(r); }));
    for (int i = 0; i < 150; ++i) // the BAM ends with its last packet at 100 ms
    {
        bench.step();
    }
    CHECK(results.size() == 1 && results[0].outcome == CommandOutcome::Timeout && results[0].time == at(1350ms));
    CHECK(throws<std::invalid_argument>([&] { commander.command(kEngine, kNull); }));
}

TEST_CASE(address_commander_matches_each_transfer_end_to_its_own_command)
{
    // A is answered before its BAM ended; B's BAM starts after A's. B's wait counts from B's own
    // end, not from A's.
    Bench bench;
    Node& us = bench.add_node(kUs);
    AddressCommander commander(us);
    std::vector<CommandResult> results;
    const auto record = [&](const CommandResult& r) { results.push_back(r); };
    CHECK(commander.command(kEngine, 0x91, record));
    CHECK(commander.command(Name{}.set_identity_number(0x66).set_manufacturer_code(1), 0x92, record));
    bench.step();
    bench.inject(claim_of(0x91, kEngine)); // at 0 ms, handled at 10 ms
    std::vector<SimTime> ends;
    us.on_transfer_end([&](const vehicle_sil::j1939::TransferEnd& e) { ends.push_back(e.time); });
    for (int i = 0; i < 160; ++i)
    {
        bench.step();
    }
    CHECK_EQ(ends.size(), 2u);
    CHECK_EQ(results.size(), 2u);
    CHECK(results.size() == 2 && results[0].outcome == CommandOutcome::Moved && results[0].address == 0x91);
    CHECK(results.size() == 2 && ends.size() == 2 && results[1].outcome == CommandOutcome::Timeout &&
          results[1].time == ends[1] + 1250ms);
    CHECK_EQ(commander.pending(), 0u);
}

TEST_CASE(address_commander_is_not_confused_by_another_commanded_address_of_its_node)
{
    // The application sends its own Commanded Address (BAM) first; the commander's comes after
    // it. The first end is the application's: the commander waits from its own (TransferEnd::id).
    Bench bench;
    Node& us = bench.add_node(kUs);
    AddressCommander commander(us);
    std::vector<CommandResult> results;
    std::vector<vehicle_sil::j1939::TransferEnd> ends;
    vehicle_sil::j1939::Subscription sub{us, us.on_transfer_end([&](const vehicle_sil::j1939::TransferEnd& e) { ends.push_back(e); })};
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            Bytes own(9, 0x00);
            own[8] = 0x93;
            CHECK(us.send(vehicle_sil::j1939::kPgnCommandedAddress, own) == vehicle_sil::j1939::SendStatus::Ok);
            CHECK(commander.command(kEngine, 0x91, [&](const CommandResult& r) { results.push_back(r); }));
        }
    };
    for (int i = 0; i < 200; ++i)
    {
        bench.step();
    }
    CHECK_EQ(ends.size(), 2u);
    CHECK(results.size() == 1 && ends.size() == 2 && results[0].outcome == CommandOutcome::Timeout &&
          results[0].time == ends[1].time + 1250ms);
}

TEST_CASE(address_commander_sends_again_once_then_reports_the_failed_transfer)
{
    // To one address that never answers the RTS: the transfer times out, is sent again once,
    // times out again: NotSent with the transfer's result.
    Bench bench;
    Node& us = bench.add_node(kUs);
    AddressCommander commander(us);
    std::vector<CommandResult> results;
    CHECK(commander.command(kEngine, 0x40, [&](const CommandResult& r) { results.push_back(r); }, 0x30));
    std::size_t rts = 0;
    for (int i = 0; i < 600; ++i)
    {
        bench.step();
        for (const auto& f : bench.take_sent())
        {
            rts += f.pgn == kPgnTpCm && f.data[0] == 0x10 && f.da == 0x30 ? 1 : 0;
        }
    }
    CHECK_EQ(rts, 2u);
    CHECK(results.size() == 1 && results[0].outcome == CommandOutcome::NotSent &&
          results[0].transfer == vehicle_sil::j1939::TransferResult::Aborted &&
          results[0].reason == vehicle_sil::j1939::AbortReason::Timeout);
}

TEST_CASE(address_commander_sends_again_after_its_node_moved)
{
    // The node moves while its command's BAM is under way: the transfer is cancelled and the
    // command goes again from the new address.
    Bench bench;
    Node& us = bench.network.add_node({0x90, Name{}.set_identity_number(9).set_manufacturer_code(7).set_arbitrary_address_capable(true)});
    AddressCommander commander(us);
    std::vector<CommandResult> results;
    bench.steps(30); // claimed at 250 ms
    CHECK(commander.command(kEngine, 0x40, [&](const CommandResult& r) { results.push_back(r); }));
    bench.steps(2); // the announcement at 300 ms
    bench.inject(claim_of(0x90, Name{}.set_identity_number(1).set_manufacturer_code(1))); // moves at 320
    bench.steps(40);
    std::size_t announcements = 0;
    for (const auto& f : bench.take_sent())
    {
        announcements += f.pgn == kPgnTpCm && f.data[0] == 0x20 ? 1 : 0;
    }
    CHECK_EQ(announcements, 2u);
    CHECK(us.address() == 128);
    bench.inject(claim_of(0x40, kEngine));
    bench.steps(2);
    CHECK(results.size() == 1 && results[0].outcome == CommandOutcome::Moved);
}
