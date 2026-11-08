// SPDX-License-Identifier: MIT
//
// NAME and the basics of address claiming of the J1939 stack: the first claim, the
// wait after the claim, contention between two nodes, Request for Address Claimed and Cannot
// Claim. The expected frames follow the former AgIsoStack++ tests where they apply
// (formerly tests/unit/test_j1939_address_claim.cpp) and the stack's design otherwise.
#include "j1939_bench.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::unit::throws;
namespace can = vehicle_sil::can;

namespace {

constexpr Address kUs = 0x80;  ///< in 128..247: waits 250 ms after the claim
constexpr Address kTool = 0x90;
/// The NAME of our node: not arbitrary address capable.
constexpr std::uint64_t kOurName = 0x20008200FE001234u;

Bytes name_bytes(std::uint64_t name)
{
    Bytes out;
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::uint8_t>(name >> (8 * i)));
    }
    return out;
}

J1939Frame claim(Address sa, std::uint64_t name) { return frame(6, kPgnAddressClaim, sa, kGlobal, name_bytes(name)); }
J1939Frame cannot_claim(std::uint64_t name) { return claim(kNull, name); }
J1939Frame req(Address from, Address to, std::uint32_t pgn) { return frame(6, kPgnRequest, from, to, request(pgn)); }

Node& add_claiming(Bench& bench, Address address, std::uint64_t name)
{
    NodeOptions options;
    options.address = address;
    options.name = Name{name};
    return bench.network.add_node(options);
}

/// The Cannot Claim delay of `name` as the stack documents it: 0..255 steps of 0.6 ms, from
/// the NAME. Only used to find the step in which Cannot Claim must leave.
SimDuration cannot_claim_delay(std::uint64_t name) { return vehicle_sil::j1939::cannot_claim_delay(Name{name}); }

} // namespace

// --- NAME

TEST_CASE(name_fields_have_their_bit_positions)
{
    const Name name{kOurName};
    CHECK_EQ(name.identity_number(), 0x01234u);
    CHECK_EQ(name.manufacturer_code(), 0x7F0u);
    CHECK_EQ(name.ecu_instance(), 0u);
    CHECK_EQ(name.function_instance(), 0u);
    CHECK_EQ(name.function(), 0x82u);
    CHECK(!name.reserved());
    CHECK_EQ(name.vehicle_system(), 0u);
    CHECK_EQ(name.vehicle_system_instance(), 0u);
    CHECK_EQ(name.industry_group(), 2u);
    CHECK(!name.arbitrary_address_capable());
}

TEST_CASE(name_setters_keep_the_other_fields_and_drop_excess_bits)
{
    Name name;
    name.set_identity_number(0x1FFFFF)
        .set_manufacturer_code(0x7FF)
        .set_ecu_instance(7)
        .set_function_instance(31)
        .set_function(0xFF)
        .set_reserved(true)
        .set_vehicle_system(0x7F)
        .set_vehicle_system_instance(15)
        .set_industry_group(7)
        .set_arbitrary_address_capable(true);
    CHECK_EQ(name.value(), 0xFFFFFFFFFFFFFFFFu);
    name.set_function(0x12);
    CHECK_EQ(name.value(), 0xFFFF12FFFFFFFFFFu);
    name.set_identity_number(0xFFFFFFFF); // only 21 bits are kept
    CHECK_EQ(name.identity_number(), 0x1FFFFFu);
    CHECK_EQ(name.manufacturer_code(), 0x7FFu);
    Name zero;
    zero.set_arbitrary_address_capable(true);
    CHECK_EQ(zero.value(), 0x8000000000000000u);
    zero.set_industry_group(1);
    CHECK_EQ(zero.value(), 0x9000000000000000u);
}

TEST_CASE(name_bytes_are_least_significant_first_and_lower_wins)
{
    const Name name{0x0102030405060708u};
    const auto bytes = name.to_bytes();
    CHECK((Bytes(bytes.begin(), bytes.end()) == Bytes{8, 7, 6, 5, 4, 3, 2, 1}));
    CHECK(Name::from_bytes(bytes) == name);
    CHECK(Name{1} < Name{2});
    CHECK(Name{0x7FFFFFFFFFFFFFFFu} < Name{0x8000000000000000u});
}

TEST_CASE(cannot_claim_delay_is_a_function_of_the_name_up_to_153_ms)
{
    std::vector<SimDuration> delays;
    for (std::uint64_t n = 0; n < 2000; ++n)
    {
        const auto d = cannot_claim_delay(kOurName + n * 7919);
        CHECK(d >= SimDuration::zero());
        CHECK(d <= std::chrono::microseconds(153000));
        CHECK_EQ(d.count() % 600000, 0); // 0.6 ms steps
        delays.push_back(d);
    }
    CHECK(cannot_claim_delay(kOurName) == cannot_claim_delay(kOurName));
    // Spread over the range, not one value.
    std::sort(delays.begin(), delays.end());
    CHECK(delays.front() < std::chrono::milliseconds(10));
    CHECK(delays.back() > std::chrono::milliseconds(140));
}

// --- the first claim and the wait after it

TEST_CASE(node_with_a_name_claims_its_address_in_the_first_step)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    CHECK(us.address_state() == AddressState::NotStarted);
    bench.step(); // 0
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    CHECK(bench.sent_times == (std::vector<SimTime>{at(0ms)}));
    CHECK(us.address_state() == AddressState::Claiming);
    CHECK_EQ(us.address(), kUs);
    bench.steps(30);
    CHECK_J1939(bench.take_sent(), {});
    CHECK(us.address_state() == AddressState::Claimed);
}

TEST_CASE(messages_sent_during_the_wait_leave_when_it_ends)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms) || now == at(100ms) || now == at(250ms) || now == at(260ms))
        {
            const std::array<std::uint8_t, 1> one{static_cast<std::uint8_t>(vehicle_sil::to_ns(now) / 10000000)};
            CHECK(us.send(Pgn{kPgnPropB}, one) == SendStatus::Ok);
        }
    };
    // A request to us during the wait: the NACK is held as well.
    bench.inject(at(50ms), req(kTool, kUs, 0xFEF1));
    bench.steps(25); // 0 .. 240
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    bench.step(); // 250: the wait ends before the application runs
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnAck, kUs, kGlobal, acknowledgement(kNack, kTool, 0xFEF1)),
                                    frame(6, kPgnPropB, kUs, kGlobal, {0}), frame(6, kPgnPropB, kUs, kGlobal, {10}),
                                    frame(6, kPgnPropB, kUs, kGlobal, {25})});
    bench.step(); // 260: sent at once
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnPropB, kUs, kGlobal, {26})});
}

TEST_CASE(held_messages_beyond_the_hold_capacity_are_refused)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    std::vector<SendStatus> results;
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            for (int i = 0; i < 17; ++i)
            {
                results.push_back(us.send(Pgn{kPgnPropB}, {}));
            }
        }
    };
    bench.steps(26);
    CHECK(results.back() == SendStatus::QueueFull);
    CHECK_EQ(std::count(results.begin(), results.end(), SendStatus::Ok), 16);
    const auto sent = bench.take_sent();
    CHECK_EQ(sent.size(), 17u); // the claim and the 16 held messages
}

TEST_CASE(single_address_node_below_128_does_not_wait)
{
    Bench bench;
    Node& us = add_claiming(bench, 0x20, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
        }
    };
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(0x20, kOurName), frame(6, kPgnPropB, 0x20, kGlobal, {})});
}

TEST_CASE(arbitrary_address_capable_node_waits_at_any_address)
{
    Bench bench;
    Node& us = add_claiming(bench, 0x20, kOurName | 0x8000000000000000u);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
        }
    };
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(0x20, kOurName | 0x8000000000000000u)});
}

// --- contention between two nodes

TEST_CASE(we_win_against_a_higher_name_and_claim_again)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step(); // 0: our claim
    bench.take_sent();
    bench.inject(at(5ms), claim(kUs, kOurName + 1));
    bench.step(); // 10
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    CHECK(us.address_state() == AddressState::Claiming);
    bench.steps(25);
    CHECK(us.address_state() == AddressState::Claimed);
    CHECK_EQ(us.address(), kUs);
}

TEST_CASE(we_lose_against_a_lower_name_and_send_cannot_claim_after_the_delay)
{
    Bench bench({}, 1ms);
    Node& us = add_claiming(bench, kUs, kOurName);
    std::vector<SendStatus> results;
    bench.app = [&](SimTime now) {
        if (now == at(1ms))
        {
            results.push_back(us.send(Pgn{kPgnPropB}, {})); // held
        }
        if (now == at(20ms))
        {
            results.push_back(us.send(Pgn{kPgnPropB}, {}));
            results.push_back(us.request(Pgn{kPgnPropB}));
        }
    };
    bench.step(); // 0
    bench.take_sent();
    bench.inject(at(5ms), claim(kUs, kOurName - 1));
    const SimTime expected = at(5ms) + cannot_claim_delay(kOurName);
    std::vector<SimTime> times;
    for (int i = 0; i < 200; ++i)
    {
        bench.step();
    }
    CHECK_J1939(bench.take_sent(), {cannot_claim(kOurName)});
    // Sent in the first 1 ms step at or after the delay (counted from the frame that beat us).
    CHECK_EQ(bench.sent_times.size(), 2u);
    CHECK(bench.sent_times[1] >= expected);
    CHECK(bench.sent_times[1] < expected + 1ms);
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK_EQ(us.address(), kNull);
    CHECK(results == (std::vector<SendStatus>{SendStatus::Ok, SendStatus::NoAddress, SendStatus::NoAddress}));
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
}

TEST_CASE(equal_name_from_another_sender_counts_as_a_loss)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step();
    bench.inject(at(5ms), claim(kUs, kOurName));
    bench.steps(20);
    CHECK(us.address_state() == AddressState::CannotClaim);
}

TEST_CASE(claims_for_other_addresses_and_cannot_claims_do_not_contend)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step();
    bench.take_sent();
    bench.inject(at(5ms), claim(0x81, kOurName - 1));
    bench.inject(at(6ms), cannot_claim(kOurName - 2));
    bench.inject(at(7ms), frame(6, kPgnAddressClaim, 0x81, kGlobal, {1, 2, 3})); // too short
    bench.steps(30);
    CHECK_J1939(bench.take_sent(), {});
    CHECK(us.address_state() == AddressState::Claimed);
}

namespace {

struct Pair
{
    std::vector<std::string> trace;
    AddressState a{};
    AddressState b{};
};

/// Two networks, each with one node claiming 0x80, on one virtual bus with their own step sizes.
Pair two_networks(SimDuration stepA, SimDuration stepB, std::uint64_t nameA, std::uint64_t nameB)
{
    can::VirtualCanBus bus;
    can::Port& monitor = bus.attach();
    vehicle_sil::j1939::Network netA;
    vehicle_sil::j1939::Network netB;
    Node& a = netA.add_node(NodeOptions{kUs, Name{nameA}});
    Node& b = netB.add_node(NodeOptions{kUs, Name{nameB}});
    can::Host hostA;
    can::Host hostB;
    hostA.add(netA, bus.attach());
    hostB.add(netB, bus.attach());
    can::StepDriver driver;
    driver.add(hostA, stepA);
    driver.add(hostB, stepB);
    driver.run_until(at(1s));
    Pair out;
    monitor.release_before(at(1s), [&](const can::TimedFrame& f) {
        out.trace.push_back(std::to_string(vehicle_sil::to_ns(f.time)) + " " + to_string(from_can(f.frame)));
    });
    out.a = a.address_state();
    out.b = b.address_state();
    return out;
}

} // namespace

TEST_CASE(two_nodes_on_two_networks_resolve_the_same_address)
{
    const std::uint64_t low = kOurName;
    const std::uint64_t high = kOurName + 0x100;
    for (const auto& [stepA, stepB] : std::vector<std::pair<SimDuration, SimDuration>>{{10ms, 10ms}, {1ms, 10ms}, {10ms, 1ms}, {3ms, 7ms}})
    {
        const Pair p = two_networks(stepA, stepB, low, high);
        CHECK(p.a == AddressState::Claimed);
        CHECK(p.b == AddressState::CannotClaim);
        // Both claim at 0 and each sees the other's claim (no claim answers a claim of the same
        // time); B sends Cannot Claim after its delay.
        CHECK_EQ(p.trace.size(), 3u);
        CHECK(std::count_if(p.trace.begin(), p.trace.end(), [&](const std::string& line) {
                  return line.find(to_string(cannot_claim(high))) != std::string::npos;
              }) == 1);
        CHECK(std::count_if(p.trace.begin(), p.trace.end(), [&](const std::string& line) {
                  return line.find(to_string(claim(kUs, low))) != std::string::npos;
              }) == 1);
        // Repeats identically.
        CHECK(two_networks(stepA, stepB, low, high).trace == p.trace);
        // Swapping the names swaps the outcome.
        const Pair q = two_networks(stepA, stepB, high, low);
        CHECK(q.a == AddressState::CannotClaim);
        CHECK(q.b == AddressState::Claimed);
    }
}

TEST_CASE(two_local_nodes_with_the_same_address_resolve_through_loopback)
{
    Bench bench;
    Node& a = add_claiming(bench, kUs, kOurName + 5);
    Node& b = add_claiming(bench, kUs, kOurName);
    bench.steps(40);
    CHECK(a.address_state() == AddressState::CannotClaim);
    CHECK(b.address_state() == AddressState::Claimed);
    const auto sent = bench.take_sent();
    // b does not answer a's claim of the same time: a lost on b's claim already.
    CHECK_EQ(sent.size(), 3u);
    CHECK(sent[0] == claim(kUs, kOurName + 5));
    CHECK(sent[1] == claim(kUs, kOurName));
    CHECK(sent[2] == cannot_claim(kOurName + 5));
}

// --- Request for Address Claimed

TEST_CASE(request_for_address_claimed_is_answered_with_our_claim)
{
    Bench bench;
    add_claiming(bench, kUs, kOurName);
    bench.step();
    bench.take_sent();
    bench.inject(at(5ms), req(kTool, kGlobal, kPgnAddressClaim));
    bench.step(); // 10: during the wait, still answered
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    bench.inject(at(15ms), req(kNull, kUs, kPgnAddressClaim)); // from a node without address
    bench.inject(at(16ms), req(kTool, 0x81, kPgnAddressClaim)); // to someone else
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
}

TEST_CASE(request_for_address_claimed_after_losing_is_answered_with_cannot_claim)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step();
    bench.inject(at(5ms), claim(kUs, kOurName - 1));
    bench.steps(20);
    CHECK(us.address_state() == AddressState::CannotClaim);
    bench.take_sent();
    bench.sent_times.clear();
    std::vector<std::uint8_t> seen;
    us.on_any_message([&](const Message& m) { seen.push_back(m.destination); });
    bench.inject(at(205ms), req(kTool, kGlobal, kPgnAddressClaim));
    bench.inject(at(205ms), req(kTool, kGlobal, kPgnAddressClaim)); // answered by the same Cannot Claim
    bench.inject(at(205ms), req(kTool, kUs, kPgnAddressClaim));     // our old address: not us any more
    bench.inject(at(206ms), frame(6, kPgnPropA, kTool, kNull, {}));  // to 254: not for a node without address
    bench.steps(20);
    CHECK_J1939(bench.take_sent(), {cannot_claim(kOurName)});
    // After the delay, counted from the request.
    CHECK_EQ(bench.sent_times.size(), 1u);
    CHECK(bench.sent_times[0] >= at(205ms) + cannot_claim_delay(kOurName));
    CHECK(seen == (std::vector<std::uint8_t>{kGlobal, kGlobal}));
    CHECK(bench.sent_times[0] < at(205ms) + cannot_claim_delay(kOurName) + 10ms);
}

TEST_CASE(our_request_for_address_claimed_is_answered_by_ourselves_too)
{
    Bench bench;
    Node& us = add_claiming(bench, 0x20, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            CHECK(us.request(Pgn{kPgnAddressClaim}) == SendStatus::Ok);
        }
    };
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {claim(0x20, kOurName), req(0x20, kGlobal, kPgnAddressClaim), claim(0x20, kOurName)});
}

TEST_CASE(node_without_an_address_may_request_address_claimed_only)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step();
    bench.inject(at(5ms), claim(kUs, kOurName - 1));
    bench.steps(20);
    bench.take_sent();
    bench.app = [&](SimTime now) {
        if (now == at(210ms))
        {
            CHECK(us.request(Pgn{kPgnPropB}) == SendStatus::NoAddress);
            CHECK(us.request(Pgn{kPgnAddressClaim}) == SendStatus::Ok);
        }
    };
    bench.steps(20);
    const auto sent = bench.take_sent();
    CHECK(!sent.empty());
    CHECK(sent[0] == req(kNull, kGlobal, kPgnAddressClaim));
    CHECK(sent.back() == cannot_claim(kOurName)); // and answers itself with Cannot Claim
}

TEST_CASE(node_without_a_name_keeps_its_fixed_address)
{
    Bench bench;
    Node& us = bench.add_node(kUs);
    bench.inject(claim(kUs, 1));
    bench.inject(req(kTool, kGlobal, kPgnAddressClaim));
    bench.steps(3);
    CHECK_J1939(bench.take_sent(), {});
    CHECK(us.address_state() == AddressState::Fixed);
    CHECK_EQ(us.address(), kUs);
}

TEST_CASE(cannot_claim_delay_values_are_fixed)
{
    // The values are part of the deterministic traces; changing the function changes them.
    CHECK(cannot_claim_delay(kOurName) == std::chrono::microseconds(93600));
    CHECK(cannot_claim_delay(0x20008200FE001239u) == std::chrono::microseconds(50400));
}

TEST_CASE(frames_queued_before_losing_do_not_leave_from_the_lost_address)
{
    // A response queued in the same step, just before the claim that beats us.
    Bench bench;
    Node& us = add_claiming(bench, 0x20, kOurName); // does not wait
    us.on_request(Pgn{kPgnPropB}, [](vehicle_sil::j1939::RequestContext& c) {
        const std::array<std::uint8_t, 1> one{1};
        CHECK(c.respond(one) == SendStatus::Ok);
    });
    bench.step();
    bench.take_sent();
    // Same time: the request (lower identifier) is handled before the claim.
    bench.inject(at(5ms), claim(0x20, kOurName - 1));
    bench.inject(at(5ms), req(kTool, 0x20, kPgnPropB));
    bench.steps(12); // past cannot_claim_delay(kOurName)
    CHECK_J1939(bench.take_sent(), {cannot_claim(kOurName)});
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
}

TEST_CASE(messages_released_by_the_wait_are_dropped_if_we_lose_before_the_flush)
{
    // 40 ms steps: the wait ends in the step at 280 ms, before a contending claim sent at 270 ms.
    Bench bench({}, 40ms);
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
        }
    };
    bench.steps(7); // 0 .. 240
    bench.take_sent();
    bench.inject(at(270ms), claim(kUs, kOurName - 1));
    bench.steps(4); // 280 (release, then lose) .. 400, past cannot_claim_delay(kOurName)
    CHECK_J1939(bench.take_sent(), {cannot_claim(kOurName)});
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
}

TEST_CASE(wait_ends_in_the_first_step_at_or_after_250_ms)
{
    Bench bench({}, 3ms);
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
        }
    };
    bench.steps(85); // 0 .. 252
    const auto sent = bench.take_sent();
    CHECK_EQ(sent.size(), 2u);
    CHECK(bench.sent_times[1] == at(252ms));
    CHECK(us.address_state() == AddressState::Claimed);
}

TEST_CASE(node_added_in_the_application_stage_claims_in_the_next_step)
{
    Bench bench;
    Node* added = nullptr;
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            added = &bench.network.add_node(NodeOptions{kUs, Name{kOurName}});
            // Before its claim: held, and a Request for Address Claimed goes from 254.
            CHECK(added->send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
            CHECK(added->request(Pgn{kPgnAddressClaim}) == SendStatus::Ok);
            CHECK(added->address_state() == AddressState::NotStarted);
        }
    };
    bench.steps(3); // 0, 10, 20
    CHECK_J1939(bench.take_sent(), {req(kNull, kGlobal, kPgnAddressClaim), claim(kUs, kOurName)});
    CHECK(bench.sent_times == (std::vector<SimTime>{at(10ms), at(20ms)}));
    bench.steps(25); // .. 270: the wait from 20 ms ends at 270
    CHECK_J1939(bench.take_sent(), {frame(6, kPgnPropB, kUs, kGlobal, {})});
    CHECK(bench.sent_times.back() == at(270ms));
}

TEST_CASE(a_claimed_address_can_still_be_lost)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.steps(30);
    CHECK(us.address_state() == AddressState::Claimed);
    bench.take_sent();
    bench.inject(at(295ms), claim(kUs, kOurName - 1));
    bench.steps(10);
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK_J1939(bench.take_sent(), {cannot_claim(kOurName)});
}

TEST_CASE(wait_exception_boundaries)
{
    // Not arbitrary address capable: 0..127 and 248..253 send at once, 128..247 wait.
    for (const auto& [address, waits] : std::vector<std::pair<Address, bool>>{{0, false}, {127, false}, {128, true},
                                                                              {247, true}, {248, false}, {253, false}})
    {
        Bench bench;
        Node& us = add_claiming(bench, address, kOurName);
        bench.app = [&](SimTime now) {
            if (now == at(0ms))
            {
                CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
            }
        };
        bench.step();
        CHECK_EQ(bench.take_sent().size(), waits ? 1u : 2u);
    }
}

TEST_CASE(held_acknowledgements_beyond_the_hold_capacity_are_dropped)
{
    NetworkOptions options;
    options.hold_capacity = 2;
    Bench bench(options);
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
        }
    };
    bench.inject(at(5ms), req(kTool, kUs, 0xFEF1)); // its NACK does not fit any more
    bench.steps(27);
    const auto sent = bench.take_sent();
    CHECK_EQ(sent.size(), 3u); // the claim and the 2 held messages
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
}

TEST_CASE(held_messages_that_do_not_fit_the_data_queue_are_dropped_and_counted)
{
    NetworkOptions options;
    options.data_queue_capacity = 4;
    Bench bench(options);
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            for (int i = 0; i < 6; ++i)
            {
                CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
            }
        }
    };
    bench.steps(27);
    CHECK_EQ(bench.take_sent().size(), 5u); // the claim and 4 of the 6
    CHECK_EQ(bench.network.statistics().held_dropped, 2u);
}

TEST_CASE(timer_capacity_must_cover_the_claiming_nodes)
{
    NetworkOptions options;
    options.timer_capacity = 3;
    vehicle_sil::j1939::Network network(options);
    network.add_node(NodeOptions{0x10, Name{1}});
    network.add_node(NodeOptions{0x11, std::nullopt}); // no NAME: no timers
    CHECK(throws<std::invalid_argument>([&] { network.add_node(NodeOptions{0x12, Name{2}}); }));
    // Exactly enough: 2 claiming nodes with 4 timers.
    options.timer_capacity = 4;
    vehicle_sil::j1939::Network enough(options);
    enough.add_node(NodeOptions{0x10, Name{1}});
    enough.add_node(NodeOptions{0x12, Name{2}});
    CHECK_EQ(enough.node_count(), 2u);
}

TEST_CASE(claim_sent_again_after_a_win_is_dropped_if_we_lose_in_the_same_step)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step();
    bench.take_sent();
    // Two other nodes claim our address at the same time: we beat the first (and queue our claim
    // again) but lose to the second, so the queued claim must not leave from our old address.
    bench.inject(at(5ms), claim(kUs, kOurName + 1));
    bench.inject(at(5ms), claim(kUs, kOurName - 1));
    bench.steps(12); // past cannot_claim_delay(kOurName)
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK_J1939(bench.take_sent(), {cannot_claim(kOurName)});
}
