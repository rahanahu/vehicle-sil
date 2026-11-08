// SPDX-License-Identifier: MIT
//
// The Exchange part of the J1939 stack (commands of a protocol of one's own and their answers):
// answers and interim answers, timeouts and retries, the counterpart's address and NAME
// (PeerLost), the node's address, long commands (transfer ends), cancel(), callbacks that start,
// cancel and destroy, the network's destruction and the step size.

#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/exchange.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AbortReason;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Exchange;
using vehicle_sil::j1939::ExchangeCallback;
using vehicle_sil::j1939::ExchangeId;
using vehicle_sil::j1939::ExchangeOptions;
using vehicle_sil::j1939::ExchangeOutcome;
using vehicle_sil::j1939::ExchangeResult;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::ReplyMatch;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::Subscription;
using vehicle_sil::j1939::Timer;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::j1939::TransferResult;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kUs = 0x20;
constexpr Address kServer = 0x30;
constexpr std::uint64_t kServerName = 0x0000'3000'0000'0030u;
constexpr std::uint8_t kRead = 0x01;
constexpr std::uint8_t kReply = 0x81;
constexpr std::uint8_t kBusy = 0x7F;

const Name kServerNameValue{kServerName};

Bytes name_bytes(std::uint64_t name)
{
    Bytes out(8);
    for (int i = 0; i < 8; ++i)
    {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(name >> (8 * i));
    }
    return out;
}
J1939Frame claim(Address sa, std::uint64_t name) { return frame(6, kPgnAddressClaim, sa, kGlobal, name_bytes(name)); }

Bytes command(std::uint8_t item) { return {kRead, item, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; }
Bytes reply(std::uint8_t item, std::uint8_t value) { return {kReply, item, value, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; }

/// Takes [kReply, item, ...] as the answer to `item`, [kBusy, item] as an interim one.
vehicle_sil::j1939::ReplyMatcher answer_to(std::uint8_t item)
{
    return [item](const Message& m) {
        if (m.data.size() < 2 || m.data[1] != item)
        {
            return ReplyMatch::Ignore;
        }
        return m.data[0] == kReply ? ReplyMatch::Final : m.data[0] == kBusy ? ReplyMatch::Pending : ReplyMatch::Ignore;
    };
}

/// A copy of an ExchangeResult (its reply data is valid during the callback only).
struct Got
{
    ExchangeOutcome outcome;
    Address peer;
    std::optional<Name> peer_name;
    Bytes data;
    SendStatus send_status;
    TransferResult transfer;
    AbortReason reason;
    std::uint32_t attempts;
    bool pending;
    SimTime time;
};

struct Results
{
    std::vector<Got> got;
    [[nodiscard]] ExchangeCallback callback()
    {
        return [this](const ExchangeResult& r) {
            got.push_back(Got{r.outcome, r.peer, r.peer_name, Bytes(r.reply.data.begin(), r.reply.data.end()), r.send_status,
                              r.transfer, r.reason, r.attempts, r.pending, r.time});
        };
    }
};

/// The frames of `pgn` the network sent so far, with their times.
std::vector<std::pair<J1939Frame, SimTime>> sent_of(Bench& bench, std::uint32_t pgn)
{
    const std::size_t first = bench.sent_times.size();
    const auto frames = bench.take_sent();
    std::vector<std::pair<J1939Frame, SimTime>> out;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i].pgn == pgn)
        {
            out.emplace_back(frames[i], bench.sent_times[first + i]);
        }
    }
    return out;
}

/// A server on a local node: answers [kRead, item] after `delay` with [kReply, item, item * 2]
/// (and, with `busy`, an interim [kBusy, item] at once). Silent while `silent`.
class Server
{
public:
    Server(Node& node, SimDuration delay, bool busy = false)
      : node_(node), delay_(delay), busy_(busy),
        commands_(node, node.on_message(vehicle_sil::j1939::Pgn{kPgnPropA}, [this](const Message& m) { on_command(m); })),
        later_(node, TimerKind::Scheduled, [this] { answer(); })
    {
    }
    bool silent = false;
    int received = 0;
    std::size_t last_length = 0;

private:
    void on_command(const Message& m)
    {
        ++received;
        last_length = m.data.size();
        if (silent || m.data.empty() || m.data[0] != kRead)
        {
            return;
        }
        client_ = m.source;
        item_ = m.data[1];
        if (busy_)
        {
            const Bytes interim{kBusy, item_, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            static_cast<void>(node_.send(vehicle_sil::j1939::Pgn{kPgnPropA}, interim, client_));
        }
        later_.start_after(delay_);
    }
    void answer()
    {
        static_cast<void>(node_.send(vehicle_sil::j1939::Pgn{kPgnPropA}, reply(item_, static_cast<std::uint8_t>(item_ * 2)), client_));
    }
    Node& node_;
    SimDuration delay_;
    bool busy_;
    Address client_ = 0;
    std::uint8_t item_ = 0;
    Subscription commands_;
    Timer later_;
};

constexpr vehicle_sil::j1939::Pgn kPropA{kPgnPropA};
constexpr vehicle_sil::j1939::Pgn kPropB{kPgnPropB};

} // namespace

// --- answers

TEST_CASE(exchange_sends_the_command_and_reports_the_matching_answer)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    const ExchangeId id = exchange.start(kPropA, command(3), kServer, answer_to(3), results.callback());
    CHECK(id && exchange.running(id));
    CHECK_EQ(exchange.pending(), 1u);
    bench.step(); // 0: the command leaves
    const auto sent = sent_of(bench, kPgnPropA);
    CHECK(sent.size() == 1 && sent[0].first == frame(6, kPgnPropA, kUs, kServer, command(3)) && sent[0].second == at(0ms));
    bench.inject(frame(6, kPgnPropA, 0x31, kUs, reply(3, 6))); // another sender
    bench.inject(frame(6, kPgnPropA, kServer, kUs, reply(4, 8))); // the counterpart, not the answer
    bench.step();
    CHECK(results.got.empty());
    bench.inject(frame(6, kPgnPropA, kServer, kUs, reply(3, 6)));
    bench.steps(2);
    CHECK_EQ(results.got.size(), 1u);
    const Got& g = results.got.at(0);
    CHECK(g.outcome == ExchangeOutcome::Reply);
    CHECK(g.data == reply(3, 6));
    CHECK_EQ(g.peer, kServer);
    CHECK(!g.peer_name); // nobody claimed 0x30
    CHECK_EQ(g.attempts, 1u);
    CHECK(!g.pending);
    CHECK(g.time == at(10ms));
    CHECK(!exchange.running(id));
    CHECK_EQ(exchange.pending(), 0u);
    CHECK_EQ(exchange.statistics().sent, 1u);
    CHECK_EQ(exchange.statistics().replies, 1u);
}

TEST_CASE(exchange_sends_a_pdu2_command_to_all_nodes_and_waits_for_the_counterpart)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    exchange.start(kPropB, command(1), kServer, answer_to(1), results.callback());
    bench.step();
    const auto sent = sent_of(bench, kPgnPropB);
    CHECK(sent.size() == 1 && sent[0].first == frame(6, kPgnPropB, kUs, kGlobal, command(1)));
    bench.inject(frame(6, kPgnPropB, 0x31, kGlobal, reply(1, 2)));
    bench.inject(frame(6, kPgnPropB, kServer, kGlobal, reply(1, 2)));
    bench.steps(2);
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Reply && results.got[0].peer == kServer);
}

TEST_CASE(exchange_waits_again_after_an_interim_answer_and_does_not_send_again)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms, .retries = 3});
    Results results;
    exchange.start(kPropA, command(5), kServer, answer_to(5), results.callback());
    exchange.start(kPropA, command(6), kServer, answer_to(6), results.callback());
    bench.step(); // 0
    bench.inject(at(80ms), frame(6, kPgnPropA, kServer, kUs, {kBusy, 5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    bench.inject(at(80ms), frame(6, kPgnPropA, kServer, kUs, {kBusy, 6, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    bench.steps(16); // .. 150: the wait now ends at 180
    CHECK(results.got.empty());
    bench.inject(at(170ms), frame(6, kPgnPropA, kServer, kUs, reply(5, 1)));
    bench.steps(10); // .. 250
    CHECK_EQ(results.got.size(), 2u);
    CHECK(results.got.size() == 2 && results.got[0].outcome == ExchangeOutcome::Reply && results.got[0].pending &&
          results.got[0].time == at(170ms));
    // Item 6: no answer after its interim one; not sent again despite the retries.
    CHECK(results.got.size() == 2 && results.got[1].outcome == ExchangeOutcome::Timeout && results.got[1].pending &&
          results.got[1].attempts == 1u && results.got[1].time == at(180ms));
    CHECK_EQ(sent_of(bench, kPgnPropA).size(), 2u);
}

TEST_CASE(exchange_sends_again_after_the_timeout_then_gives_up)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms, .retries = 2});
    Results results;
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());
    bench.steps(40);
    const auto sent = sent_of(bench, kPgnPropA);
    // A Timeout timer fires in the first step after its deadline: 0, 110, 220; Timeout after 320.
    CHECK_EQ(sent.size(), 3u);
    CHECK(sent.size() == 3 && sent[0].second == at(0ms) && sent[1].second == at(110ms) && sent[2].second == at(220ms));
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Timeout &&
          results.got[0].attempts == 3u && results.got[0].time == at(320ms) && !results.got[0].pending);
    CHECK_EQ(exchange.statistics().sent, 3u);
    CHECK_EQ(exchange.statistics().timeouts, 1u);
}

TEST_CASE(exchange_gives_each_of_two_equal_commands_one_answer_in_order)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms});
    std::vector<int> order;
    exchange.start(kPropA, command(2), kServer, answer_to(2), [&](const ExchangeResult& r) {
        order.push_back(r.outcome == ExchangeOutcome::Reply ? 1 : -1);
    });
    exchange.start(kPropA, command(2), kServer, answer_to(2), [&](const ExchangeResult& r) {
        order.push_back(r.outcome == ExchangeOutcome::Reply ? 2 : -2);
    });
    bench.step();
    bench.inject(frame(6, kPgnPropA, kServer, kUs, reply(2, 4)));
    bench.step();
    CHECK(order == std::vector<int>{1});
    bench.inject(frame(6, kPgnPropA, kServer, kUs, reply(2, 4)));
    bench.step();
    CHECK(order == (std::vector<int>{1, 2}));
}

// --- the counterpart's address

TEST_CASE(exchange_by_name_waits_for_the_name_to_be_claimed_then_sends_to_its_address)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    // Nobody claims the NAME: PeerLost when the timeout after the command was due has passed.
    exchange.start(kPropA, command(1), kServerNameValue, answer_to(1), results.callback());
    bench.steps(12);
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::PeerLost &&
          results.got[0].attempts == 0u && results.got[0].peer == kNull && results.got[0].time == at(100ms));
    CHECK(sent_of(bench, kPgnPropA).empty());
    // Its Address Claimed comes while the next exchange waits (start-up): the command leaves then.
    exchange.start(kPropA, command(1), kServerNameValue, answer_to(1), results.callback());
    bench.steps(3);
    bench.inject(at(145ms), claim(0x44, kServerName));
    bench.steps(2);
    const auto sent = sent_of(bench, kPgnPropA);
    CHECK(sent.size() == 1 && sent[0].first.da == 0x44 && sent[0].second == at(150ms));
    bench.inject(frame(6, kPgnPropA, 0x44, kUs, reply(1, 2)));
    bench.step();
    CHECK(results.got.size() == 2 && results.got[1].outcome == ExchangeOutcome::Reply && results.got[1].peer == 0x44 &&
          results.got[1].peer_name == std::optional<Name>{kServerNameValue});
}

TEST_CASE(exchange_reports_peer_lost_when_the_counterpart_leaves_its_address)
{
    // Three ways: it moves, it cannot claim, a lower NAME takes its address. Given by address
    // (its NAME known from its claim) and by NAME.
    for (int way = 0; way < 3; ++way)
    {
        for (const bool byName : {false, true})
        {
            Bench bench;
            Node& us = bench.add_node(kUs);
            Exchange exchange(us, {.timeout = 500ms, .retries = 2});
            Results results;
            bench.inject(claim(kServer, kServerName));
            bench.step();
            if (byName)
            {
                exchange.start(kPropA, command(1), kServerNameValue, answer_to(1), results.callback());
            }
            else
            {
                exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());
            }
            bench.steps(3);
            CHECK(results.got.empty());
            const J1939Frame leave = way == 0   ? claim(0x31, kServerName)
                                     : way == 1 ? claim(kNull, kServerName)
                                                : claim(kServer, kServerName - 1);
            bench.inject(at(35ms), leave);
            bench.steps(2);
            CHECK_EQ(results.got.size(), 1u);
            CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::PeerLost &&
                  results.got[0].time == at(35ms) && results.got[0].peer == kServer &&
                  results.got[0].peer_name == std::optional<Name>{kServerNameValue});
            CHECK_EQ(exchange.statistics().peer_lost, 1u);
        }
    }
}

TEST_CASE(exchange_to_an_address_without_a_known_name_ends_only_by_timeout)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());
    bench.step();
    bench.inject(claim(kServer, kServerName)); // no NAME was known there when the command left
    bench.steps(15);
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Timeout);
}

TEST_CASE(exchange_with_a_local_counterpart_sees_its_address_changes)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Node& server = bench.network.add_node(NodeOptions{kServer, kServerNameValue});
    Server s(server, 20ms);
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    bench.step();
    exchange.start(kPropA, command(4), kServerNameValue, answer_to(4), results.callback());
    bench.steps(8);
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Reply &&
          results.got[0].data == reply(4, 8) && results.got[0].peer == kServer);
    s.silent = true;
    exchange.start(kPropA, command(4), kServerNameValue, answer_to(4), results.callback());
    bench.step();
    bench.inject(claim(kServer, kServerName - 1)); // the local server loses its address
    // Seen when the local node's Cannot Claim comes (after its delay of up to 153 ms).
    bench.steps(20);
    CHECK(server.address_state() == AddressState::CannotClaim);
    CHECK(results.got.size() == 2 && results.got[1].outcome == ExchangeOutcome::PeerLost);
}

// --- the node's address

TEST_CASE(exchange_waits_for_the_address_claim_and_ends_when_the_node_cannot_claim)
{
    {
        Bench bench;
        Node& us = bench.network.add_node(NodeOptions{0x90, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0)});
        Exchange exchange(us, {.timeout = 100ms});
        Results results;
        exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback()); // before the first step
        bench.steps(30);
        CHECK(us.address_state() == AddressState::Claimed);
        const auto sent = sent_of(bench, kPgnPropA);
        CHECK(sent.size() == 1 && sent[0].second == at(250ms) && sent[0].first.sa == 0x90);
    }
    {
        Bench bench;
        Node& us = bench.network.add_node(NodeOptions{0x90, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0)});
        Exchange exchange(us, {.timeout = 100ms});
        Results results;
        exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());
        bench.step();
        const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
        bench.inject(frame(6, kPgnAddressClaim, 0x90, kGlobal, Bytes(rival.begin(), rival.end())));
        bench.steps(3);
        CHECK(us.address_state() == AddressState::CannotClaim);
        CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::NotSent &&
              results.got[0].send_status == SendStatus::NoAddress && results.got[0].attempts == 0u);
    }
}

TEST_CASE(exchange_sends_again_after_a_move_when_asked_without_using_a_retry)
{
    Bench bench;
    Node& us = bench.network.add_node(
        NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    bench.steps(30); // claimed at 250 ms
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback(),
                   {.on_address_change = vehicle_sil::j1939::AddressChangePolicy::SendAgain});
    bench.step(); // 300: sent from 0x20
    const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
    bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
    bench.steps(30); // moves to 128 at 310 ms, claimed at 560 ms
    const auto sent = sent_of(bench, kPgnPropA);
    CHECK(sent.size() == 2 && sent[0].first.sa == kUs && sent[1].first.sa == 128 && sent[1].second == at(560ms));
    CHECK(results.got.empty());
    bench.inject(frame(6, kPgnPropA, kServer, 128, reply(1, 2)));
    bench.step();
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Reply && results.got[0].attempts == 2u);
}

TEST_CASE(exchange_ends_interrupted_when_the_node_moves_after_sending_by_default)
{
    // The default policy (Stop): a sent command whose answer cannot come is not sent again and
    // not reported as not sent. One not sent yet waits and leaves from the new address.
    Bench bench;
    Node& us = bench.network.add_node(
        NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
    Exchange exchange(us, {.timeout = 100ms, .retries = 3});
    Results results;
    bench.steps(30); // claimed at 250 ms
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());
    bench.step(); // 300: sent from 0x20
    const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
    bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
    bench.step(); // 310: moves to 128 and waits
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Interrupted &&
          results.got[0].attempts == 1u && results.got[0].send_status == SendStatus::Ok && results.got[0].time == at(300ms));
    exchange.start(kPropA, command(2), kServer, answer_to(2), results.callback()); // during the wait
    bench.steps(30);
    const auto sent = sent_of(bench, kPgnPropA);
    CHECK(sent.size() == 2 && sent[0].first.sa == kUs && sent[1].first.sa == 128 && sent[1].first.data[1] == 2);
    CHECK_EQ(exchange.statistics().interrupted, 1u);
}

TEST_CASE(exchange_ends_interrupted_when_the_node_cannot_claim_after_sending)
{
    for (const auto policy : {vehicle_sil::j1939::AddressChangePolicy::Stop, vehicle_sil::j1939::AddressChangePolicy::SendAgain})
    {
        Bench bench;
        Node& us = bench.network.add_node(NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0)});
        Exchange exchange(us, {.timeout = 100ms});
        Results results;
        bench.steps(2);
        exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback(), {.on_address_change = policy});
        bench.step(); // sent
        const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
        bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
        bench.steps(3);
        CHECK(us.address_state() == AddressState::CannotClaim);
        CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Interrupted &&
              results.got[0].send_status == SendStatus::NoAddress && results.got[0].attempts == 1u);
    }
}

// --- long commands

TEST_CASE(exchange_counts_the_wait_of_a_long_command_from_the_end_of_its_transfer)
{
    for (const bool busy : {false, true})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Node& serverNode = bench.add_node(kServer);
        // The answer comes 25 ms after the command arrived; the transfer takes longer than the
        // timeout, so a wait counted from the send would end first.
        Server server(serverNode, 25ms, busy);
        Exchange exchange(us, {.timeout = 30ms, .max_length = 100});
        Results results;
        Bytes longCommand = payload(100, 1);
        longCommand[0] = kRead;
        longCommand[1] = 9;
        exchange.start(kPropA, longCommand, kServer, answer_to(9), results.callback());
        bench.steps(20);
        CHECK_EQ(server.last_length, 100u);
        CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Reply &&
              results.got[0].data == reply(9, 18) && results.got[0].pending == busy);
        CHECK(results.got.size() == 1 && results.got[0].time > at(30ms));
    }
}

TEST_CASE(exchange_sends_a_long_pdu2_command_as_a_broadcast)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 50ms, .max_length = 20});
    Results results;
    exchange.start(kPropB, payload(20, 3), kServer, [](const Message&) { return ReplyMatch::Final; }, results.callback());
    bench.steps(20); // announcement at 0, packets at 50, 100, 150: complete at 150
    CHECK(results.got.empty() || results.got[0].time > at(150ms));
    bench.steps(10);
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Timeout && results.got[0].time >= at(200ms));
}

TEST_CASE(exchange_sends_again_after_an_aborted_transfer_then_reports_not_sent)
{
    for (const std::uint32_t retries : {0u, 1u})
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Exchange exchange(us, {.timeout = 100ms, .retries = retries, .max_length = 20});
        Results results;
        exchange.start(kPropA, payload(20, 3), kServer, answer_to(1), results.callback());
        for (std::uint32_t attempt = 0; attempt <= retries; ++attempt)
        {
            bench.step();
            const auto rtsSent = sent_of(bench, kPgnTpCm);
            CHECK(rtsSent.size() == 1 && rtsSent[0].first == rts(kUs, kServer, 20, kPgnPropA));
            bench.inject(conn_abort(kServer, kUs, 1, kPgnPropA));
            bench.step();
        }
        bench.steps(2);
        CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::NotSent &&
              results.got[0].transfer == TransferResult::AbortedByPeer && results.got[0].reason == AbortReason::Busy &&
              results.got[0].attempts == retries + 1);
    }
}

TEST_CASE(exchange_with_a_long_command_whose_transfer_is_cancelled_follows_the_cause)
{
    // The counterpart's address goes to another NAME during the transfer: the transfer is
    // cancelled, then the Address Claimed shows the loss (PeerLost, not NotSent).
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        Exchange exchange(us, {.timeout = 100ms, .max_length = 20});
        Results results;
        bench.inject(claim(kServer, kServerName));
        bench.step();
        exchange.start(kPropA, payload(20, 3), kServer, answer_to(1), results.callback());
        bench.steps(2); // the RTS left; no clear-to-send comes
        bench.inject(claim(kServer, kServerName - 1));
        bench.steps(2);
        CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::PeerLost);
    }
    // Our node moves during the transfer: sent again from the new address, then answered.
    {
        Bench bench;
        Node& us = bench.network.add_node(
            NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
        Node& serverNode = bench.add_node(kServer);
        Server server(serverNode, 0ms);
        Exchange exchange(us, {.timeout = 100ms, .max_length = 20});
        Results results;
        bench.steps(30); // claimed at 250 ms
        Bytes longCommand = payload(20, 3);
        longCommand[0] = kRead;
        longCommand[1] = 5;
        exchange.start(kPropA, longCommand, kServer, answer_to(5), results.callback(),
                   {.on_address_change = vehicle_sil::j1939::AddressChangePolicy::SendAgain});
        bench.step(); // 300: the RTS
        const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
        bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
        bench.steps(40);
        CHECK(us.address() == 128);
        CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Reply &&
              results.got[0].attempts == 2u && results.got[0].data == reply(5, 10));
    }
}

TEST_CASE(exchange_reports_a_long_command_it_gave_up_as_interrupted)
{
    // The counterpart took every packet but no acknowledgement of the end came: it may have
    // the whole command. A refused transfer (AbortedByPeer) is NotSent instead (other test).
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms, .max_length = 20});
    Results results;
    exchange.start(kPropA, payload(20, 3), kServer, answer_to(1), results.callback());
    bench.step(); // 0: RTS
    bench.inject(cts(kServer, kUs, 3, 1, kPgnPropA));
    bench.steps(200); // packets, then no EndOfMsgACK: this node aborts after T3
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Interrupted &&
          results.got[0].transfer == TransferResult::Aborted && results.got[0].reason == AbortReason::Timeout);
    // No clear-to-send at all: no packet left, the counterpart cannot have it.
    Bench quiet;
    Node& node = quiet.add_node(kUs);
    Exchange other(node, {.timeout = 100ms, .max_length = 20});
    Results got;
    other.start(kPropA, payload(20, 3), kServer, answer_to(1), got.callback());
    quiet.steps(200);
    CHECK(got.got.size() == 1 && got.got[0].outcome == ExchangeOutcome::NotSent &&
          got.got[0].transfer == TransferResult::Aborted);
}

TEST_CASE(exchange_tells_a_long_command_cut_short_by_a_move_from_one_that_may_have_arrived)
{
    // Default policy (Stop). The node moves (a) after the RTS, before any packet: the counterpart
    // cannot have the command, so it is sent again from the new address; (b) after the last
    // packet, before the acknowledgement: it may have it, so Interrupted.
    for (const bool allPackets : {false, true})
    {
        Bench bench;
        Node& us = bench.network.add_node(
            NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
        Exchange exchange(us, {.timeout = 100ms, .max_length = 20});
        Results results;
        bench.steps(30); // claimed at 250 ms
        exchange.start(kPropA, payload(20, 3), kServer, answer_to(1), results.callback());
        bench.step(); // 300: the RTS
        if (allPackets)
        {
            bench.inject(cts(kServer, kUs, 3, 1, kPgnPropA));
            bench.step(); // 310: the three packets leave; no EndOfMsgACK
        }
        const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
        bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
        bench.steps(40);
        std::size_t rts = 0;
        for (const auto& [f, t] : sent_of(bench, kPgnTpCm))
        {
            rts += f.data[0] == 16 ? 1 : 0;
        }
        if (allPackets)
        {
            CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Interrupted &&
                  results.got[0].transfer == TransferResult::Cancelled);
            CHECK_EQ(rts, 1u);
        }
        else
        {
            CHECK(results.got.empty() || results.got[0].outcome == ExchangeOutcome::Timeout);
            CHECK_EQ(rts, 2u); // sent again from 128
        }
    }
}

TEST_CASE(exchange_reports_a_refused_retry_as_interrupted)
{
    // The retry finds the data queue full: the first attempt may have arrived.
    vehicle_sil::j1939::NetworkOptions options;
    options.data_queue_capacity = 2;
    Bench bench(options);
    Node& us = bench.add_node(kUs);
    // A timer with the retry's deadline, started first, fills the queue just before the retry.
    Timer filler(us, TimerKind::Timeout, [&] {
        static_cast<void>(us.send(kPropB, command(9)));
        static_cast<void>(us.send(kPropB, command(9)));
    });
    filler.start_at(at(50ms));
    Exchange exchange(us, {.timeout = 50ms, .retries = 1});
    Results results;
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());
    bench.steps(10);
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Interrupted &&
          results.got[0].send_status == SendStatus::QueueFull && results.got[0].attempts == 1u);
}

// --- long answers (CommandOptions::long_answer) and the wait of each command

namespace {

/// Starts a command to kServer at 0 ms whose answer is a broadcast of PropB from kServer (an
/// interim answer: [kBusy, ...] of PropA), and injects `frames` (time, frame). Returns the
/// outcome and its time.
std::pair<ExchangeOutcome, SimTime> run_long_answer(std::optional<vehicle_sil::j1939::Pgn> longAnswer,
                                                    const std::vector<std::pair<SimTime, J1939Frame>>& frames,
                                                    ExchangeOptions options = {.timeout = 100ms}, int steps = 120)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, options);
    Results results;
    exchange.start(kPropA, command(1), kServer,
                   [](const Message& m) {
                       if (m.pgn == kPropA && !m.data.empty() && m.data[0] == kBusy)
                       {
                           return ReplyMatch::Pending;
                       }
                       return m.pgn == kPropB ? ReplyMatch::Final : ReplyMatch::Ignore;
                   },
                   results.callback(), {.long_answer = longAnswer});
    bench.step(); // 0: sent
    for (const auto& [time, f] : frames)
    {
        bench.inject(time, f);
    }
    bench.steps(steps);
    if (results.got.size() != 1)
    {
        return {ExchangeOutcome::NotSent, SimTime{}};
    }
    return {results.got[0].outcome, results.got[0].time};
}

std::vector<std::pair<SimTime, J1939Frame>> slow_broadcast(Address from, std::uint32_t pgn, SimTime start,
                                                           std::chrono::milliseconds interval, int packets = 3)
{
    const Bytes data = payload(static_cast<std::size_t>(packets) * 7 - 1, 9);
    std::vector<std::pair<SimTime, J1939Frame>> out{{start, bam(from, data.size(), pgn)}};
    for (int k = 1; k <= packets; ++k)
    {
        out.emplace_back(start + k * interval, dt(from, kGlobal, static_cast<std::uint8_t>(k), data));
    }
    return out;
}

} // namespace

TEST_CASE(exchange_holds_the_wait_while_the_long_answer_is_received)
{
    // The answer: a broadcast announced at 50 ms, packets every 50 ms, complete at 200 ms.
    const auto answer = slow_broadcast(kServer, kPgnPropB, at(50ms), 50ms);
    const auto held = run_long_answer(kPropB, answer);
    CHECK(held.first == ExchangeOutcome::Reply && held.second == at(200ms));
    // Without long_answer the wait counts on: Timeout at 100 ms, before the answer is complete.
    const auto plain = run_long_answer(std::nullopt, answer);
    CHECK(plain.first == ExchangeOutcome::Timeout && plain.second == at(100ms));
}

TEST_CASE(exchange_holds_the_wait_for_a_long_answer_by_connection_and_through_an_interim_answer)
{
    const Bytes data = payload(20, 9);
    // A connection to this node: RTS at 50 ms (the node answers with a CTS), packets at 180 ms.
    const auto rtsAnswer = run_long_answer(kPropB, {{at(50ms), rts(kServer, kUs, 20, kPgnPropB)},
                                                    {at(180ms), dt(kServer, kUs, 1, data)},
                                                    {at(180ms), dt(kServer, kUs, 2, data)},
                                                    {at(180ms), dt(kServer, kUs, 3, data)}});
    CHECK(rtsAnswer.first == ExchangeOutcome::Reply && rtsAnswer.second == at(180ms));
    // An interim answer while the long one comes does not end the hold.
    auto frames = slow_broadcast(kServer, kPgnPropB, at(50ms), 50ms);
    frames.emplace_back(at(60ms), frame(6, kPgnPropA, kServer, kUs, {kBusy, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    std::sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto withPending = run_long_answer(kPropB, frames);
    CHECK(withPending.first == ExchangeOutcome::Reply && withPending.second == at(200ms));
}

TEST_CASE(exchange_does_not_hold_the_wait_for_other_transfers)
{
    // Another sender, another PGN, and a connection to another node.
    const auto sender = run_long_answer(kPropB, slow_broadcast(0x31, kPgnPropB, at(50ms), 50ms));
    CHECK(sender.first == ExchangeOutcome::Timeout && sender.second == at(100ms));
    const auto group = run_long_answer(kPropB, slow_broadcast(kServer, 0xFF20, at(50ms), 50ms));
    CHECK(group.first == ExchangeOutcome::Timeout && group.second == at(100ms));
    const auto other = run_long_answer(kPropB, {{at(50ms), rts(kServer, 0x40, 20, kPgnPropB)}});
    CHECK(other.first == ExchangeOutcome::Timeout && other.second == at(100ms));
}

TEST_CASE(exchange_waits_again_from_the_end_of_a_long_answer_that_failed)
{
    const Bytes data = payload(20, 9);
    // A missing packet at 70 ms (Lost): Timeout 100 ms later.
    const auto lost = run_long_answer(kPropB, {{at(50ms), bam(kServer, 20, kPgnPropB)}, {at(70ms), dt(kServer, kGlobal, 2, data)}});
    CHECK(lost.first == ExchangeOutcome::Timeout && lost.second == at(170ms));
    // No packet after the announcement: the receiver gives up after 750 ms (T1), then 100 ms.
    const auto silent = run_long_answer(kPropB, {{at(50ms), bam(kServer, 20, kPgnPropB)}});
    CHECK(silent.first == ExchangeOutcome::Timeout && silent.second == at(900ms));
}

TEST_CASE(exchange_holds_the_wait_at_most_answer_transfer_limit)
{
    // The counterpart announces a new broadcast every 80 ms for ever (each replaces the last).
    std::vector<std::pair<SimTime, J1939Frame>> forever;
    for (int k = 0; k < 40; ++k)
    {
        forever.emplace_back(at(50ms) + k * 80ms, bam(kServer, 20, kPgnPropB));
    }
    const auto r = run_long_answer(kPropB, forever, {.timeout = 100ms, .answer_transfer_limit = 300ms}, 400);
    // Held until 50 + 300 ms; the last broadcast before that ended the hold at most `timeout` later.
    CHECK(r.first == ExchangeOutcome::Timeout && r.second >= at(350ms) && r.second <= at(450ms));
    // A broadcast that never ends (no packets, the limit shorter than T1).
    const auto silent = run_long_answer(kPropB, {{at(50ms), bam(kServer, 20, kPgnPropB)}},
                                        {.timeout = 100ms, .answer_transfer_limit = 200ms});
    CHECK(silent.first == ExchangeOutcome::Timeout && silent.second == at(250ms));
}

TEST_CASE(exchange_waits_each_command_for_its_own_timeout)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms, .retries = 1});
    Results results;
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback(), {.timeout = 30ms});
    exchange.start(kPropA, command(2), kServer, answer_to(2), results.callback()); // ExchangeOptions::timeout
    bench.steps(40);
    // 1: sent at 0 and 40 (first step after 30), Timeout at 70. 2: sent at 0 and 110, Timeout at 210.
    CHECK(results.got.size() == 2 && results.got[0].time == at(70ms) && results.got[0].attempts == 2u);
    CHECK(results.got.size() == 2 && results.got[1].time == at(210ms) && results.got[1].attempts == 2u);
    CHECK(throws<std::invalid_argument>([&] {
        exchange.start(kPropA, command(3), kServer, answer_to(3), results.callback(), {.timeout = SimDuration::zero()});
    }));
}

TEST_CASE(exchange_sends_each_command_again_as_often_as_it_allows)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 50ms, .retries = 2});
    Results results;
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());                 // 2 (the Exchange's)
    exchange.start(kPropA, command(2), kServer, answer_to(2), results.callback(), {.retries = 0}); // an erasure: once
    exchange.start(kPropA, command(3), kServer, answer_to(3), results.callback(), {.retries = 4});
    bench.steps(60);
    CHECK_EQ(results.got.size(), 3u);
    CHECK(results.got.size() == 3 && results.got[0].attempts == 1u && results.got[0].outcome == ExchangeOutcome::Timeout);
    CHECK(results.got.size() == 3 && results.got[1].attempts == 3u && results.got[2].attempts == 5u);
    std::size_t erasures = 0;
    for (const auto& [f, t] : sent_of(bench, kPgnPropA))
    {
        erasures += f.data[1] == 2 ? 1 : 0;
    }
    CHECK_EQ(erasures, 1u);
}

// --- cancel, capacity, arguments

TEST_CASE(exchange_cancel_forgets_the_exchange_and_frees_its_place)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Exchange exchange(us, {.timeout = 100ms, .capacity = 1});
    Results results;
    const ExchangeId first = exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback());
    CHECK(!exchange.start(kPropA, command(2), kServer, answer_to(2), results.callback()));
    CHECK_EQ(exchange.statistics().refused, 1u);
    bench.step();
    CHECK(exchange.cancel(first));
    CHECK(!exchange.cancel(first));
    CHECK(!exchange.cancel(ExchangeId{}));
    const ExchangeId second = exchange.start(kPropA, command(2), kServer, answer_to(2), results.callback());
    CHECK(second && second != first);
    bench.inject(frame(6, kPgnPropA, kServer, kUs, reply(1, 2))); // the cancelled one's answer
    bench.steps(2);
    CHECK(results.got.empty());
    exchange.cancel_all();
    CHECK_EQ(exchange.pending(), 0u);
    bench.steps(20);
    CHECK(results.got.empty());
}

TEST_CASE(exchange_refuses_invalid_arguments)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    CHECK(throws<std::invalid_argument>([&] { Exchange e(us, {.timeout = SimDuration::zero()}); }));
    CHECK(throws<std::invalid_argument>([&] { Exchange e(us, {.capacity = 0}); }));
    CHECK(throws<std::invalid_argument>([&] { Exchange e(us, {.max_length = 0}); }));
    CHECK(throws<std::invalid_argument>([&] { Exchange e(us, {.max_length = 1786}); }));
    Exchange exchange(us);
    Results results;
    const Bytes nine(9, 0);
    CHECK(throws<std::invalid_argument>([&] { exchange.start(kPropA, command(1), kGlobal, answer_to(1), results.callback()); }));
    CHECK(throws<std::invalid_argument>([&] { exchange.start(kPropA, command(1), kNull, answer_to(1), results.callback()); }));
    CHECK(throws<std::invalid_argument>([&] { exchange.start(kPropA, nine, kServer, answer_to(1), results.callback()); }));
    CHECK(throws<std::invalid_argument>([&] { exchange.start(kPropA, Bytes{}, kServer, answer_to(1), results.callback()); }));
    CHECK(throws<std::invalid_argument>([&] { exchange.start(kPropA, command(1), kServer, nullptr, results.callback()); }));
    CHECK(throws<std::invalid_argument>([&] { exchange.start(kPropA, command(1), kServer, answer_to(1), nullptr); }));
    CHECK(throws<std::invalid_argument>([&] { exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback(), {.priority = 8}); }));
    CHECK(throws<std::invalid_argument>(
        [&] { exchange.start(vehicle_sil::j1939::Pgn{0x40000}, command(1), kServer, answer_to(1), results.callback()); }));
    CHECK_EQ(exchange.pending(), 0u);
}

// --- reentrancy and lifetime

TEST_CASE(exchange_callbacks_may_start_the_next_command)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Node& serverNode = bench.add_node(kServer);
    Server server(serverNode, 0ms);
    Exchange exchange(us, {.timeout = 100ms, .capacity = 1});
    std::vector<int> values;
    std::function<void(std::uint8_t)> read = [&](std::uint8_t item) {
        exchange.start(kPropA, command(item), kServer, answer_to(item), [&, item](const ExchangeResult& r) {
            values.push_back(r.outcome == ExchangeOutcome::Reply ? r.reply.data[2] : -1);
            if (item < 5)
            {
                read(static_cast<std::uint8_t>(item + 1)); // the slot is free again
            }
        });
    };
    read(1);
    bench.steps(20);
    CHECK(values == (std::vector<int>{2, 4, 6, 8, 10}));
}

TEST_CASE(exchange_destroyed_in_a_callback_or_a_matcher_calls_nothing_more)
{
    // Two exchanges end at the same deadline; the first callback destroys the Exchange.
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        auto exchange = std::make_unique<Exchange>(us, ExchangeOptions{.timeout = 100ms});
        int calls = 0;
        const auto destroy = [&](const ExchangeResult&) {
            ++calls;
            exchange.reset();
        };
        exchange->start(kPropA, command(1), kServer, answer_to(1), destroy);
        exchange->start(kPropA, command(2), kServer, answer_to(2), destroy);
        bench.steps(20);
        CHECK_EQ(calls, 1);
        CHECK(exchange == nullptr);
        CHECK(!bench.network.failed());
    }
    // A matcher destroys it; and a matcher cancels its own exchange.
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        auto exchange = std::make_unique<Exchange>(us, ExchangeOptions{.timeout = 100ms});
        Results results;
        ExchangeId self{};
        self = exchange->start(kPropA, command(1), kServer,
                               [&](const Message&) {
                                   exchange->cancel(self);
                                   return ReplyMatch::Final;
                               },
                               results.callback());
        exchange->start(kPropA, command(2), kServer,
                        [&](const Message&) {
                            exchange.reset();
                            return ReplyMatch::Final;
                        },
                        results.callback());
        bench.step();
        bench.inject(frame(6, kPgnPropA, kServer, kUs, reply(7, 7)));
        bench.steps(2);
        CHECK(results.got.empty());
        CHECK(exchange == nullptr);
        CHECK(!bench.network.failed());
    }
}

TEST_CASE(exchange_matchers_may_start_and_callbacks_may_cancel_all)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Node& serverNode = bench.add_node(kServer);
    Server server(serverNode, 0ms);
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    bool started = false;
    exchange.start(kPropA, command(1), kServer,
                   [&](const Message& m) {
                       if (!started)
                       {
                           started = true;
                           exchange.start(kPropA, command(2), kServer, answer_to(2), results.callback());
                       }
                       return answer_to(1)(m);
                   },
                   results.callback());
    bench.steps(10);
    CHECK_EQ(results.got.size(), 2u);
    CHECK(results.got.size() == 2 && results.got[0].data == reply(1, 2) && results.got[1].data == reply(2, 4));
    // Two exchanges end at the same deadline; the first callback cancels all: no second call.
    int calls = 0;
    const auto cancelAll = [&](const ExchangeResult&) {
        ++calls;
        exchange.cancel_all();
    };
    exchange.start(kPropA, command(7), 0x55, answer_to(7), cancelAll);
    exchange.start(kPropA, command(8), 0x55, answer_to(8), cancelAll);
    bench.steps(20);
    CHECK_EQ(calls, 1);
    CHECK_EQ(exchange.pending(), 0u);
}

TEST_CASE(exchange_sends_again_after_a_move_also_after_an_interim_answer)
{
    Bench bench;
    Node& us = bench.network.add_node(
        NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
    Exchange exchange(us, {.timeout = 100ms});
    Results results;
    bench.steps(30); // claimed at 250 ms
    exchange.start(kPropA, command(1), kServer, answer_to(1), results.callback(),
                   {.on_address_change = vehicle_sil::j1939::AddressChangePolicy::SendAgain});
    bench.step(); // 300: sent
    bench.inject(frame(6, kPgnPropA, kServer, kUs, {kBusy, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    bench.step();
    const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
    bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
    bench.steps(30);
    const auto sent = sent_of(bench, kPgnPropA);
    CHECK(sent.size() == 2 && sent[1].first.sa == 128);
    bench.inject(frame(6, kPgnPropA, kServer, 128, reply(1, 2)));
    bench.step();
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::Reply && results.got[0].attempts == 2u &&
          results.got[0].pending);
}

TEST_CASE(exchange_waits_for_a_name_no_longer_when_the_node_moves_meanwhile)
{
    Bench bench;
    Node& us = bench.network.add_node(
        NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
    Exchange exchange(us, {.timeout = 400ms});
    Results results;
    bench.steps(30); // claimed at 250 ms
    exchange.start(kPropA, command(1), kServerNameValue, answer_to(1), results.callback());
    bench.step(); // 300: waits for the NAME until 700
    const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
    bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
    bench.steps(50); // moved; claimed again at 560 ms
    CHECK(results.got.size() == 1 && results.got[0].outcome == ExchangeOutcome::PeerLost &&
          results.got[0].time == at(700ms));
}

TEST_CASE(exchange_outlives_its_network_and_an_attached_one_ends_without_callbacks)
{
    auto bench = std::make_unique<Bench>();
    Node& us = bench->add_node(kUs);
    Exchange owned(us, {.timeout = 100ms});
    int calls = 0;
    owned.start(kPropA, command(1), kServer, answer_to(1), [&](const ExchangeResult&) { ++calls; });
    auto& attached = us.attach<Exchange>(ExchangeOptions{.timeout = 100ms});
    attached.start(kPropA, command(2), kServer, answer_to(2), [&](const ExchangeResult&) { ++calls; });
    bench->step();
    bench.reset();
    CHECK_EQ(calls, 0);
    CHECK_EQ(owned.pending(), 1u);
    owned.cancel_all();
    CHECK_EQ(owned.pending(), 0u);
    CHECK(throws<std::logic_error>([&] { owned.start(kPropA, command(1), kServer, answer_to(1), [](const ExchangeResult&) {}); }));
}

TEST_CASE(exchange_runs_are_repeatable_and_end_alike_whatever_the_step_size)
{
    const auto run = [](SimDuration step) {
        Bench bench({}, step);
        Node& us = bench.add_node(kUs);
        Node& serverNode = bench.add_node(kServer);
        Server server(serverNode, 25ms, true);
        Exchange exchange(us, {.timeout = 40ms, .retries = 1, .max_length = 30});
        Results results;
        bench.app = [&](SimTime now) {
            if (now == at(0ms))
            {
                Bytes longCommand = payload(30, 1);
                longCommand[0] = kRead;
                longCommand[1] = 2;
                exchange.start(kPropA, longCommand, kServer, answer_to(2), results.callback());
                exchange.start(kPropA, command(3), 0x55, answer_to(3), results.callback()); // nobody there
            }
        };
        bench.steps(static_cast<int>(400ms / step));
        std::vector<std::pair<int, long long>> out;
        for (const auto& g : results.got)
        {
            out.emplace_back(static_cast<int>(g.outcome) * 10 + static_cast<int>(g.attempts),
                             static_cast<long long>((g.time - SimTime{}).count()));
        }
        return out;
    };
    for (const SimDuration step : {SimDuration{1ms}, SimDuration{5ms}, SimDuration{10ms}})
    {
        const auto first = run(step);
        CHECK(first == run(step)); // the same input gives the same result, times included
        // The command leaves in the step after the application stage and the timeouts fire in
        // the step after their deadline, so the times follow the step; the outcomes do not.
        CHECK(first.size() == 2 && first[0].first == static_cast<int>(ExchangeOutcome::Reply) * 10 + 1 &&
              first[1].first == static_cast<int>(ExchangeOutcome::Timeout) * 10 + 2);
    }
}
