// SPDX-License-Identifier: MIT
//
// Request (PGN 59904) and Acknowledgement (PGN 59392) of the J1939 stack against remote
// nodes the tester plays frame by frame, in the cases that do not need address claiming; the
// cases that need a claimed requester or our own claim are with the address management tests.
#include "j1939_bench.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/j1939/network.hpp"

#include <array>
#include <string>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AckCode;
using vehicle_sil::j1939::HandlerId;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::RequestContext;
using vehicle_sil::j1939::SendStatus;

namespace {

constexpr Address kUs = 0x80;
constexpr Address kTool = 0x90;
constexpr std::uint32_t kUnsupported = 0xFEF1; ///< PDU2, no responder

J1939Frame req(Address from, Address to, std::uint32_t pgn) { return frame(6, kPgnRequest, from, to, request(pgn)); }
J1939Frame ack(std::uint8_t control, Address requester, std::uint32_t pgn, Address from = kUs)
{
    return frame(6, kPgnAck, from, kGlobal, acknowledgement(control, requester, pgn));
}

struct ResponderLog
{
    struct Call
    {
        std::uint32_t pgn;
        Address requester;
        bool specific;
        SimTime time;
    };
    std::vector<Call> calls;
};

} // namespace

TEST_CASE(request_global_for_an_unsupported_pgn_is_not_answered)
{
    Bench bench;
    bench.add_node(kUs);
    bench.inject(req(kTool, kGlobal, kUnsupported));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(request_to_us_for_an_unsupported_pgn_is_nacked_in_the_step_that_handles_it)
{
    Bench bench;
    bench.add_node(kUs);
    bench.inject(at(3ms), req(kTool, kUs, kUnsupported));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {ack(kNack, kTool, kUnsupported)});
    CHECK(bench.sent_times == (std::vector<SimTime>{at(10ms)}));
    bench.step();
    CHECK_J1939(bench.take_sent(), {});
    // A PDU1 group too.
    bench.inject(req(kTool, kUs, kPgnPropA));
    bench.step();
    CHECK_J1939(bench.take_sent(), {ack(kNack, kTool, kPgnPropA)});
}

TEST_CASE(request_to_another_address_is_ignored)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    us.on_request(Pgn{kPgnPropB}, [&](RequestContext&) { ++calls; });
    bench.inject(req(kTool, 0x81, kPgnPropB));
    bench.inject(req(kTool, 0x81, kUnsupported));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(calls, 0);
}

TEST_CASE(request_from_the_null_address_is_not_answered)
{
    // A node without an address may only ask for address claims (answered once claiming exists).
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    us.on_request(Pgn{kPgnPropB}, [&](RequestContext&) { ++calls; });
    bench.inject(req(kNull, kUs, kPgnPropB));
    bench.inject(req(kNull, kGlobal, kPgnPropB));
    bench.inject(req(kNull, kUs, kUnsupported));
    bench.inject(req(kNull, kUs, kPgnAddressClaim));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(calls, 0);
}

TEST_CASE(request_from_a_node_we_have_not_seen_claim_is_nacked)
{
    // B: a requester whose claim was not seen is NACKed too. The NACK only names the requester's
    // address, so the stack answers like for any other requester.
    Bench bench;
    bench.add_node(kUs);
    bench.inject(req(0x33, kUs, kUnsupported));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {ack(kNack, 0x33, kUnsupported)});
}

TEST_CASE(request_shorter_than_three_bytes_is_ignored)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    us.on_any_request([&](RequestContext&) { ++calls; });
    bench.inject(frame(6, kPgnRequest, kTool, kUs, {}));
    bench.inject(frame(6, kPgnRequest, kTool, kUs, {0xF1}));
    bench.inject(frame(6, kPgnRequest, kTool, kUs, {0xF1, 0xFE}));
    bench.inject(frame(6, kPgnRequest, kTool, kGlobal, {0xF1, 0xFE}));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(calls, 0);
    CHECK_EQ(bench.network.statistics().requests_malformed, 4u);
    // The network keeps working.
    bench.inject(req(kTool, kUs, kUnsupported));
    bench.step();
    CHECK_EQ(calls, 1);
}

TEST_CASE(request_longer_than_three_bytes_uses_the_first_three)
{
    Bench bench;
    bench.add_node(kUs);
    bench.inject(frame(6, kPgnRequest, kTool, kUs, {0xF1, 0xFE, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55}));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {ack(kNack, kTool, kUnsupported)});
}

TEST_CASE(request_for_an_invalid_pgn_value_is_nacked_with_the_bytes_received)
{
    // A PDU1 PGN with PS set, and a value above 18 bits: no responder can match, so a
    // destination-specific request is NACKed with the 3 bytes as received.
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    us.on_request(Pgn{kPgnPropA}, [&](RequestContext&) { ++calls; });
    bench.inject(frame(6, kPgnRequest, kTool, kUs, {0x05, 0xEF, 0x00}));
    bench.inject(frame(6, kPgnRequest, kTool, kUs, {0x00, 0x00, 0xFC}));
    bench.inject(frame(6, kPgnRequest, kTool, kGlobal, {0x05, 0xEF, 0x00}));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {ack(kNack, kTool, 0xEF05), ack(kNack, kTool, 0xFC0000)});
    CHECK_EQ(calls, 0);
}

TEST_CASE(responder_acknowledgement_is_sent_only_for_a_destination_specific_request)
{
    const std::array<std::pair<AckCode, std::uint8_t>, 4> codes{{{AckCode::Positive, kAck},
                                                                 {AckCode::Negative, kNack},
                                                                 {AckCode::AccessDenied, kAccessDenied},
                                                                 {AckCode::CannotRespond, kCannotRespond}}};
    for (const auto& [code, control] : codes)
    {
        Bench bench;
        Node& us = bench.add_node(kUs);
        ResponderLog log;
        std::vector<bool> sent;
        us.on_request(Pgn{kPgnPropB}, [&, code = code](RequestContext& context) {
            log.calls.push_back({context.pgn().value(), context.requester(), context.is_destination_specific(), context.time()});
            sent.push_back(context.acknowledge(code));
        });
        bench.inject(at(2ms), req(kTool, kUs, kPgnPropB));
        bench.steps(2);
        CHECK_J1939(bench.take_sent(), {ack(control, kTool, kPgnPropB)});
        bench.inject(req(kTool, kGlobal, kPgnPropB));
        bench.step();
        CHECK_J1939(bench.take_sent(), {});
        CHECK_EQ(log.calls.size(), 2u);
        CHECK_EQ(log.calls[0].pgn, kPgnPropB);
        CHECK_EQ(log.calls[0].requester, kTool);
        CHECK(log.calls[0].specific);
        CHECK(log.calls[0].time == at(2ms));
        CHECK(!log.calls[1].specific);
        CHECK(sent == (std::vector<bool>{true, false}));
    }
}

TEST_CASE(responder_that_neither_responds_nor_acknowledges_sends_nothing)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    const HandlerId id = us.on_request(Pgn{kPgnPropB}, [&](RequestContext&) { ++calls; });
    bench.inject(req(kTool, kUs, kPgnPropB));
    bench.inject(req(kTool, kGlobal, kPgnPropB));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(calls, 2);
    // Without the responder the request is NACKed again.
    CHECK(us.remove_handler(id));
    bench.inject(req(kTool, kUs, kPgnPropB));
    bench.step();
    CHECK_J1939(bench.take_sent(), {ack(kNack, kTool, kPgnPropB)});
    CHECK_EQ(calls, 2);
}

TEST_CASE(responder_for_any_pgn_sees_requests_without_their_own_responder)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<std::uint32_t> any;
    std::vector<std::uint32_t> exact;
    us.on_any_request([&](RequestContext& c) {
        any.push_back(c.pgn().value());
        c.acknowledge(AckCode::Positive);
    });
    us.on_request(Pgn{kPgnPropB}, [&](RequestContext& c) { exact.push_back(c.pgn().value()); });
    bench.inject(req(kTool, kUs, kUnsupported));
    bench.inject(req(kTool, kUs, kPgnPropA));
    bench.inject(req(kTool, kUs, kPgnPropB));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {ack(kAck, kTool, kUnsupported), ack(kAck, kTool, kPgnPropA)});
    CHECK(any == (std::vector<std::uint32_t>{kUnsupported, kPgnPropA}));
    CHECK(exact == (std::vector<std::uint32_t>{kPgnPropB}));
}

TEST_CASE(first_registered_responder_answers)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    std::vector<int> calls;
    us.on_request(Pgn{kPgnPropB}, [&](RequestContext& c) {
        calls.push_back(1);
        c.acknowledge(AckCode::Positive);
    });
    us.on_request(Pgn{kPgnPropB}, [&](RequestContext&) { calls.push_back(2); });
    bench.inject(req(kTool, kUs, kPgnPropB));
    bench.steps(2);
    CHECK(calls == (std::vector<int>{1}));
}

TEST_CASE(request_for_address_claim_is_never_nacked)
{
    // Answered by address claiming (the address claim tests); never NACKed, even with a responder for
    // any PGN, which does not see it either.
    Bench bench;
    Node& us = bench.add_node(kUs);
    int calls = 0;
    us.on_any_request([&](RequestContext& c) {
        ++calls;
        c.acknowledge(AckCode::Negative);
    });
    bench.inject(req(kTool, kUs, kPgnAddressClaim));
    bench.inject(req(kTool, kGlobal, kPgnAddressClaim));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(calls, 0);
}

TEST_CASE(respond_sends_the_group_to_the_requester_or_to_all)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    const std::array<std::uint8_t, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<SendStatus> results;
    us.on_any_request([&](RequestContext& c) { results.push_back(c.respond(data)); });
    bench.inject(at(1ms), req(kTool, kUs, kPgnPropA));     // PDU1, to us: to the requester
    bench.inject(at(2ms), req(kTool, kGlobal, kPgnPropA)); // PDU1, to all: to all
    bench.inject(at(3ms), req(kTool, kUs, kPgnPropB));     // PDU2: always to all
    bench.inject(at(4ms), req(kTool, kUs, 0xEF05));        // invalid PGN: cannot respond
    bench.steps(2);
    const auto payload = Bytes(data.begin(), data.end());
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnPropA, kUs, kTool, payload), frame(6, kPgnPropA, kUs, kGlobal, payload),
                                    frame(6, kPgnPropB, kUs, kGlobal, payload)});
    CHECK(results == (std::vector<SendStatus>{SendStatus::Ok, SendStatus::Ok, SendStatus::Ok, SendStatus::InvalidArgument}));
    CHECK(bench.sent_times == (std::vector<SimTime>{at(10ms), at(10ms), at(10ms)}));
}

TEST_CASE(respond_with_more_than_1785_bytes_is_too_long)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    SendStatus result = SendStatus::Ok;
    us.on_any_request([&](RequestContext& c) {
        const std::array<std::uint8_t, 1786> tooLong{};
        result = c.respond(tooLong);
    });
    bench.inject(req(kTool, kUs, kPgnPropB));
    bench.steps(2);
    CHECK(result == SendStatus::TooLong);
    CHECK_J1939(bench.take_sent(), {});
}

TEST_CASE(our_requests_are_sent_global_or_to_one_node)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.request(Pgn{kUnsupported}) == SendStatus::Ok);
            CHECK(us.request(Pgn{kUnsupported}, kTool) == SendStatus::Ok);
            CHECK(us.request(Pgn{kPgnPropA}, kTool) == SendStatus::Ok);
        }
    };
    bench.step();
    CHECK_J1939(bench.take_sent(), {req(kUs, kGlobal, kUnsupported), req(kUs, kTool, kUnsupported), req(kUs, kTool, kPgnPropA)});
    // The tester NACKs the last one; we see the acknowledgement as a message.
    std::vector<Seen> seen;
    us.on_message(Pgn{kPgnAck}, [&](const Message& m) { seen.push_back(seen_of(m)); });
    bench.inject(ack(kNack, kUs, kPgnPropA, kTool));
    bench.steps(2);
    CHECK_EQ(seen.size(), 1u);
    CHECK(seen[0].data == acknowledgement(kNack, kUs, kPgnPropA));
}

TEST_CASE(requests_are_also_seen_by_message_handlers)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    int requests = 0;
    us.on_message(Pgn{kPgnRequest}, [&](const Message&) { ++requests; });
    bench.inject(req(kTool, kUs, kUnsupported));
    bench.inject(req(kTool, kGlobal, kUnsupported));
    bench.steps(2);
    CHECK_EQ(requests, 2);
    CHECK_J1939(bench.take_sent(), {ack(kNack, kTool, kUnsupported)});
}

TEST_CASE(request_several_in_one_step_are_each_answered_in_handling_order)
{
    // 100 requesters send two requests each at the same time. Each requester's two requests
    // keep their order, and requesters are merged by identifier (lowest source address first,
    // since priority and PGN are equal): the NACKs follow that order.
    Bench bench;
    bench.add_node(kUs);
    for (int i = 0; i < 100; ++i)
    {
        const auto from = static_cast<Address>(100 - i); // injected in reverse address order
        bench.inject(at(5ms), req(from, kUs, kUnsupported));
        bench.inject(at(5ms), req(from, kUs, kPgnPropA));
    }
    bench.steps(2);
    std::vector<J1939Frame> expected;
    for (int a = 1; a <= 100; ++a)
    {
        expected.push_back(ack(kNack, static_cast<Address>(a), kUnsupported));
        expected.push_back(ack(kNack, static_cast<Address>(a), kPgnPropA));
    }
    CHECK(bench.take_sent() == expected);
}

TEST_CASE(request_duplicated_in_one_step_is_answered_twice)
{
    Bench bench;
    bench.add_node(kUs);
    bench.inject(req(kTool, kUs, kUnsupported));
    bench.inject(req(kTool, kUs, kUnsupported));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {ack(kNack, kTool, kUnsupported), ack(kNack, kTool, kUnsupported)});
}

TEST_CASE(local_nodes_request_and_nack_each_other)
{
    Bench bench;
    Node& a = bench.add_node(kUs);
    Node& b = bench.add_node(0x81);
    std::vector<Seen> seenA;
    a.on_message(Pgn{kPgnAck}, [&](const Message& m) { seenA.push_back(seen_of(m)); });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(a.request(Pgn{kUnsupported}, 0x81) == SendStatus::Ok);
        }
    };
    static_cast<void>(b);
    bench.steps(3); // 0: a requests; 10: b NACKs; 20: a sees the NACK
    CHECK_EQ(seenA.size(), 1u);
    CHECK(seenA[0] == (Seen{kPgnAck, 0x81, kGlobal, 6, at(10ms), acknowledgement(kNack, kUs, kUnsupported)}));
    CHECK_J1939(bench.take_sent(), {req(kUs, 0x81, kUnsupported), ack(kNack, kUs, kUnsupported, 0x81)});
}
