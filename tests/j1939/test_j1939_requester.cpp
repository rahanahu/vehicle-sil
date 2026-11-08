// SPDX-License-Identifier: MIT
//
// The Requester part of the J1939 stack: responses, acknowledgements, timeouts and retries,
// requests to all nodes, shared transactions, min_interval, the node's address, cancel().
// Also parse_acknowledgement() of the engine.

#include "j1939_bench.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/requester.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AckCode;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::RequestId;
using vehicle_sil::j1939::RequestOutcome;
using vehicle_sil::j1939::Requester;
using vehicle_sil::j1939::RequesterOptions;
using vehicle_sil::j1939::RequestResult;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kUs = 0x20;
constexpr Address kEcu = 0x30;
constexpr std::uint32_t kPgnInfo = 0xFF40; // PDU2
const Bytes kAnswer{9, 8, 7, 6, 5, 4, 3, 2};

/// A copy of a RequestResult (its data is valid during the callback only).
struct Got
{
    RequestOutcome outcome;
    Address source;
    Bytes data;
    AckCode ack;
    SendStatus send_status;
    std::uint32_t attempts;
    bool last;
    SimTime time;
};

struct Results
{
    std::vector<Got> got;
    [[nodiscard]] vehicle_sil::j1939::RequestCallback callback()
    {
        return [this](const RequestResult& r) {
            got.push_back(Got{r.outcome, r.source, Bytes(r.data.begin(), r.data.end()), r.ack, r.send_status, r.attempts,
                              r.last, r.time});
        };
    }
};

J1939Frame request_from_us(Address to, std::uint32_t pgn) { return frame(6, kPgnRequest, kUs, to, request(pgn)); }

/// The Requests among the frames the network sent, with their times.
std::vector<std::pair<J1939Frame, SimTime>> requests_sent(Bench& bench)
{
    const std::size_t first = bench.sent_times.size();
    const auto frames = bench.take_sent();
    std::vector<std::pair<J1939Frame, SimTime>> out;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i].pgn == kPgnRequest)
        {
            out.emplace_back(frames[i], bench.sent_times[first + i]);
        }
    }
    return out;
}

} // namespace

TEST_CASE(parse_acknowledgement_reads_what_acknowledge_sends)
{
    const Bytes data = acknowledgement(kNack, 0x42, 0x1FECA);
    Message m;
    m.pgn = Pgn{kPgnAck};
    m.data = data;
    const auto ack = vehicle_sil::j1939::parse_acknowledgement(m);
    CHECK(ack.has_value());
    CHECK(ack->code == AckCode::Negative);
    CHECK_EQ(ack->group_function, 0xFF);
    CHECK_EQ(ack->address, 0x42);
    CHECK(ack->pgn == Pgn{0x1FECA});
    m.data = std::span<const std::uint8_t>(data).first(7);
    CHECK(!vehicle_sil::j1939::parse_acknowledgement(m));
    m.pgn = Pgn{kPgnRequest};
    m.data = data;
    CHECK(!vehicle_sil::j1939::parse_acknowledgement(m));
}

TEST_CASE(requester_gets_the_response_from_the_destination)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Results results;
    const RequestId id = Requester::of(us).request(Pgn{kPgnInfo}, kEcu, results.callback());
    CHECK(id);
    CHECK_EQ(Requester::of(us).pending(), 1u);
    bench.step(); // 0: the Request leaves
    const auto sent = requests_sent(bench);
    CHECK(sent.size() == 1 && sent[0].first == request_from_us(kEcu, kPgnInfo) && sent[0].second == at(0ms));
    bench.inject(frame(6, kPgnInfo, 0x31, kGlobal, kAnswer)); // another sender: not the answer
    bench.step();
    CHECK(results.got.empty());
    bench.inject(frame(6, kPgnInfo, kEcu, kGlobal, kAnswer));
    bench.steps(2);
    CHECK_EQ(results.got.size(), 1u);
    const Got& g = results.got.at(0);
    CHECK(g.outcome == RequestOutcome::Response);
    CHECK_EQ(g.source, kEcu);
    CHECK(g.data == kAnswer);
    CHECK_EQ(g.attempts, 1u);
    CHECK(g.last);
    CHECK(g.time == at(10ms));
    CHECK_EQ(Requester::of(us).pending(), 0u);
}

TEST_CASE(requester_reports_acknowledgements_for_this_node_only)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Results results;
    Requester::of(us).request(Pgn{kPgnInfo}, kEcu, results.callback());
    bench.step();
    bench.inject(frame(6, kPgnAck, kEcu, kGlobal, acknowledgement(kNack, 0x21, kPgnInfo))); // for 0x21
    bench.inject(frame(6, kPgnAck, kEcu, kGlobal, acknowledgement(kNack, kUs, kPgnInfo + 1))); // another PGN
    bench.inject(frame(6, kPgnAck, kEcu, kGlobal, acknowledgement(kAccessDenied, kUs, kPgnInfo)));
    bench.steps(2);
    CHECK_EQ(results.got.size(), 1u);
    CHECK(results.got.at(0).outcome == RequestOutcome::Acknowledged);
    CHECK(results.got.at(0).ack == AckCode::AccessDenied);
    CHECK_EQ(results.got.at(0).source, kEcu);
}

TEST_CASE(requester_sends_again_after_the_timeout_then_gives_up)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Results results;
    Requester::of(us).request(Pgn{kPgnInfo}, kEcu, results.callback()); // timeout 1250 ms, 2 retries
    for (int i = 0; i < 400; ++i)
    {
        bench.step();
    }
    const auto sent = requests_sent(bench);
    CHECK_EQ(sent.size(), 3u);
    // Each wait counts from the step that sent the Request; it ends in the first step after it.
    CHECK(sent.size() == 3 && sent[0].second == at(0ms) && sent[1].second == at(1260ms) && sent[2].second == at(2520ms));
    CHECK_EQ(results.got.size(), 1u);
    CHECK(results.got.at(0).outcome == RequestOutcome::Timeout);
    CHECK_EQ(results.got.at(0).attempts, 3u);
    CHECK(results.got.at(0).time == at(3770ms));
    CHECK_EQ(Requester::of(us).statistics().requests_sent, 3u);
}

TEST_CASE(requester_answers_requests_for_the_same_group_with_one_request)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Results a;
    Results b;
    Requester& requester = Requester::of(us);
    requester.request(Pgn{kPgnInfo}, kEcu, a.callback());
    bench.step(); // 0: sent
    requester.request(Pgn{kPgnInfo}, kEcu, b.callback()); // joins the one under way
    bench.inject(frame(6, kPgnInfo, kEcu, kGlobal, kAnswer));
    bench.steps(2);
    CHECK_EQ(requests_sent(bench).size(), 1u);
    CHECK(a.got.size() == 1 && b.got.size() == 1);
    CHECK(b.got.at(0).outcome == RequestOutcome::Response && b.got.at(0).data == kAnswer);
    CHECK_EQ(requester.statistics().joined, 1u);
}

TEST_CASE(requester_reports_every_response_to_a_request_to_all_nodes)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Results results;
    Results late;
    Requester& requester = Requester::of(us);
    auto record = results.callback();
    requester.request(Pgn{kPgnInfo}, kGlobal, [&](const RequestResult& r) {
        record(r);
        if (results.got.size() == 1)
        {
            // Joins from the callback: gets what comes after, not the response being reported.
            requester.request(Pgn{kPgnInfo}, kGlobal, late.callback());
        }
    });
    bench.step();
    bench.inject(frame(6, kPgnInfo, 0x30, kGlobal, kAnswer));
    bench.inject(frame(6, kPgnInfo, 0x31, kGlobal, kAnswer));
    for (int i = 0; i < 200; ++i)
    {
        bench.step();
    }
    const auto sent = requests_sent(bench);
    CHECK(sent.size() == 1 && sent[0].first == request_from_us(kGlobal, kPgnInfo)); // not sent again
    CHECK_EQ(results.got.size(), 3u);
    CHECK(results.got.at(0).outcome == RequestOutcome::Response && !results.got.at(0).last && results.got.at(0).source == 0x30);
    CHECK(results.got.at(1).outcome == RequestOutcome::Response && !results.got.at(1).last && results.got.at(1).source == 0x31);
    CHECK(results.got.at(2).outcome == RequestOutcome::Timeout && results.got.at(2).last);
    CHECK(results.got.at(2).time == at(1250ms));
    CHECK_EQ(late.got.size(), 2u);
    CHECK(late.got.size() == 2 && late.got[0].source == 0x31 && late.got[1].outcome == RequestOutcome::Timeout);
}

TEST_CASE(requester_waits_for_the_address_claim)
{
    Bench bench;
    // 0x90 (128 .. 247): the node waits 250 ms after its claim.
    Node& us = bench.network.add_node(NodeOptions{0x90, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0)});
    Results results;
    Requester::of(us).request(Pgn{kPgnInfo}, kEcu, results.callback()); // before the first step
    bench.steps(30);
    CHECK(us.address_state() == AddressState::Claimed);
    const auto sent = requests_sent(bench);
    CHECK(sent.size() == 1 && sent[0].second == at(250ms)); // when the wait after the claim ended
}

TEST_CASE(requester_sends_again_from_the_new_address_after_a_move)
{
    Bench bench;
    Node& us = bench.network.add_node(
        NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
    Results results;
    bench.steps(30); // claimed at 250 ms
    Requester::of(us).request(Pgn{kPgnInfo}, kEcu, results.callback());
    bench.step(); // 300: sent from 0x20
    const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
    bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end()))); // at 300 ms
    bench.steps(30); // moves to 128 at 310 ms, claimed at 560 ms
    const auto sent = requests_sent(bench);
    CHECK_EQ(sent.size(), 2u);
    CHECK(sent.size() == 2 && sent[0].first.sa == kUs && sent[1].first.sa == 128 && sent[1].second == at(560ms));
    CHECK(results.got.empty());
    // The move did not use up a retry: two more Requests after timeouts, then Timeout.
    for (int i = 0; i < 400; ++i)
    {
        bench.step();
    }
    CHECK_EQ(requests_sent(bench).size(), 2u);
    CHECK(results.got.size() == 1 && results.got[0].outcome == RequestOutcome::Timeout && results.got[0].attempts == 4u);
}

TEST_CASE(requester_spaces_requests_for_the_same_group_by_min_interval)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    RequesterOptions options;
    options.min_interval = 500ms;
    auto& requester = Requester::attach(us, options);
    CHECK(&Requester::of(us) == &requester);
    // One per node: a second one would silently not be the one of() returns.
    CHECK(throws<std::logic_error>([&] { Requester::attach(us, options); }));
    static_assert(!std::is_constructible_v<Requester, Node&, RequesterOptions>);
    Results results;
    requester.request(Pgn{kPgnInfo}, kEcu, results.callback());
    bench.step(); // 0: sent
    bench.inject(frame(6, kPgnInfo, kEcu, kGlobal, kAnswer));
    bench.step(); // 10: answered
    requester.request(Pgn{kPgnInfo}, kEcu, results.callback()); // waits until 500 ms
    requester.request(Pgn{kPgnInfo + 1}, kEcu, results.callback()); // another group: at once
    bench.steps(60);
    const auto sent = requests_sent(bench);
    CHECK_EQ(sent.size(), 3u);
    CHECK(sent.size() == 3 && sent[1].first == request_from_us(kEcu, kPgnInfo + 1) && sent[1].second == at(20ms));
    CHECK(sent.size() == 3 && sent[2].first == request_from_us(kEcu, kPgnInfo) && sent[2].second == at(500ms));
}

TEST_CASE(requester_cancel_forgets_the_request)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Results results;
    Requester& requester = Requester::of(us);
    const RequestId id = requester.request(Pgn{kPgnInfo}, kEcu, results.callback());
    bench.step();
    CHECK(requester.cancel(id));
    CHECK(!requester.cancel(id));
    bench.inject(frame(6, kPgnInfo, kEcu, kGlobal, kAnswer));
    bench.steps(300);
    CHECK(results.got.empty());
    CHECK_EQ(requests_sent(bench).size(), 1u); // no retries either
    CHECK_EQ(requester.pending(), 0u);
}

TEST_CASE(requester_refuses_above_its_capacity_and_invalid_requests)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    RequesterOptions options;
    options.capacity = 1;
    Requester& requester = Requester::attach(us, options);
    Results results;
    CHECK(requester.request(Pgn{kPgnInfo}, kEcu, results.callback()));
    CHECK(!requester.request(Pgn{kPgnInfo + 1}, kEcu, results.callback()));
    CHECK_EQ(requester.statistics().refused, 1u);
    CHECK(throws<std::invalid_argument>([&] { requester.request(Pgn{0xEF01}, kEcu, results.callback()); }));
    CHECK(throws<std::invalid_argument>([&] { requester.request(Pgn{kPgnInfo}, kNull, results.callback()); }));
    CHECK(throws<std::invalid_argument>([&] { requester.request(Pgn{kPgnInfo}, kEcu, {}); }));
    RequesterOptions bad;
    bad.timeout = 0ms;
    Node& other = bench.add_node(0x21);
    CHECK(throws<std::invalid_argument>([&] { Requester::attach(other, bad); }));
}

TEST_CASE(requester_callbacks_may_request_again)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    Requester& requester = Requester::of(us);
    std::vector<RequestOutcome> outcomes;
    std::function<void(const RequestResult&)> again = [&](const RequestResult& r) {
        outcomes.push_back(r.outcome);
        if (outcomes.size() < 3)
        {
            requester.request(r.pgn, r.destination, again); // from the handler: sent in this step
        }
    };
    requester.request(Pgn{kPgnInfo}, kEcu, again);
    for (int i = 0; i < 3; ++i)
    {
        bench.step();
        bench.inject(frame(6, kPgnInfo, kEcu, kGlobal, kAnswer));
    }
    bench.steps(2);
    CHECK_EQ(outcomes.size(), 3u);
    const auto sent = requests_sent(bench);
    CHECK(sent.size() == 3 && sent[1].second == at(10ms) && sent[2].second == at(20ms));
}

TEST_CASE(requester_ends_requests_with_not_sent_when_the_node_cannot_claim)
{
    Bench bench;
    Node& us = bench.network.add_node(NodeOptions{0x90, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0)});
    Results results;
    Requester::of(us).request(Pgn{kPgnInfo}, kEcu, results.callback()); // waits for the claim
    bench.step();
    const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
    bench.inject(frame(6, kPgnAddressClaim, 0x90, kGlobal, Bytes(rival.begin(), rival.end())));
    bench.steps(3);
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK(results.got.size() == 1 && results.got[0].outcome == RequestOutcome::NotSent &&
          results.got[0].send_status == SendStatus::NoAddress && results.got[0].attempts == 0u);
    CHECK(requests_sent(bench).empty());
}

TEST_CASE(requester_keeps_min_interval_when_it_sends_again_after_a_move)
{
    Bench bench;
    Node& us = bench.network.add_node(
        NodeOptions{kUs, Name{}.set_identity_number(7).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true)});
    RequesterOptions options;
    options.min_interval = 1s;
    Results results;
    bench.steps(30); // claimed at 250 ms
    Requester::attach(us, options).request(Pgn{kPgnInfo}, kEcu, results.callback());
    bench.step(); // 300: sent
    const auto rival = Name{}.set_identity_number(1).set_manufacturer_code(1).to_bytes();
    bench.inject(frame(6, kPgnAddressClaim, kUs, kGlobal, Bytes(rival.begin(), rival.end())));
    bench.steps(120); // moves at 310, claimed at 560 ms; min_interval: not before 1300 ms
    const auto sent = requests_sent(bench);
    CHECK(sent.size() == 2 && sent[1].first.sa == 128 && sent[1].second == at(1300ms));
}
