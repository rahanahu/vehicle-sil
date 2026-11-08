// SPDX-License-Identifier: MIT
//
// Address management of the J1939 stack beyond the basic claim: the address table,
// arbitrary address capable nodes that move, address violations, Commanded Address, the
// address change handlers, several nodes on one network and several networks on one host.
// The expected frames follow the former AgIsoStack++ tests where they apply
// (formerly tests/unit/test_j1939_address_claim.cpp and test_j1939_request.cpp) and the stack's
// design otherwise.
#include "j1939_bench.hpp"
#include "network_test_access.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AddressChange;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::unit::throws;
namespace can = vehicle_sil::can;

using vehicle_sil::j1939::detail::NetworkTestAccess;

namespace {

constexpr Address kUs = 0x80;
constexpr Address kTool = 0x90;
constexpr std::uint32_t kPgnCommandedAddress = 0xFED8;
constexpr std::uint32_t kUnsupportedPgn = 0xFEF1;
/// Not arbitrary address capable.
constexpr std::uint64_t kOurName = 0x20008200FE001234u;
/// Arbitrary address capable (bit 63): lower priority than every NAME without the bit.
constexpr std::uint64_t kAacName = 0xA0008200FE001234u;

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
J1939Frame nack(Address from, Address requester, std::uint32_t pgn)
{
    return frame(6, kPgnAck, from, kGlobal, acknowledgement(kNack, requester, pgn));
}
J1939Frame status(Address sa) { return frame(6, kPgnPropB, sa, kGlobal, {}); }

Node& add_claiming(Network& network, Address address, std::uint64_t name)
{
    NodeOptions options;
    options.address = address;
    options.name = Name{name};
    return network.add_node(options);
}

Node& add_claiming(Bench& bench, Address address, std::uint64_t name) { return add_claiming(bench.network, address, name); }

SimDuration cannot_claim_delay(std::uint64_t name) { return vehicle_sil::j1939::cannot_claim_delay(Name{name}); }

/// The 9 bytes of a Commanded Address.
std::array<std::uint8_t, 9> commanded(std::uint64_t name, std::uint8_t address)
{
    std::array<std::uint8_t, 9> out{};
    for (int i = 0; i < 8; ++i)
    {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(name >> (8 * i));
    }
    out[8] = address;
    return out;
}

Message commanded_message(std::span<const std::uint8_t> data, SimTime time, Address destination = kGlobal)
{
    return Message{Pgn{kPgnCommandedAddress}, 6, kTool, destination, time, data};
}

std::string describe(const AddressChange& c)
{
    return std::to_string(static_cast<int>(c.previous_state)) + ":" + std::to_string(c.previous_address) + "->" +
           std::to_string(static_cast<int>(c.state)) + ":" + std::to_string(c.address) + "@" +
           std::to_string(vehicle_sil::to_ns(c.time) / 1000000);
}

std::string change(AddressState from, Address fromAddress, AddressState to, Address toAddress, int ms)
{
    return std::to_string(static_cast<int>(from)) + ":" + std::to_string(fromAddress) + "->" +
           std::to_string(static_cast<int>(to)) + ":" + std::to_string(toAddress) + "@" + std::to_string(ms);
}

} // namespace

// --- Address table

TEST_CASE(address_table_records_the_claims_of_other_control_functions)
{
    Bench bench;
    bench.add_node(0x10);
    bench.inject(at(1ms), claim(0x90, 0x1000));
    bench.inject(at(2ms), claim(0x91, 0x2000));
    bench.step();
    bench.step();
    const auto& table = bench.network.address_table();
    CHECK_EQ(table.size(), 2u);
    CHECK(table.name_at(0x90) == Name{0x1000});
    CHECK(table.name_at(0x91) == Name{0x2000});
    CHECK(!table.name_at(0x92).has_value());
    CHECK(!table.name_at(kNull).has_value());
    CHECK(!table.name_at(kGlobal).has_value());
    CHECK(table.address_of(Name{0x2000}) == Address{0x91});
    CHECK(!table.address_of(Name{0x3000}).has_value());
}

TEST_CASE(address_table_follows_a_control_function_that_moves)
{
    // AgIsoStack++: address_claim_of_a_node_that_moves_updates_the_address_table.
    Bench bench;
    bench.add_node(0x10);
    bench.inject(at(1ms), claim(0x90, 0x1000));
    bench.step();
    bench.inject(at(11ms), claim(0x91, 0x1000));
    bench.steps(2);
    const auto& table = bench.network.address_table();
    CHECK_EQ(table.size(), 1u);
    CHECK(!table.name_at(0x90).has_value());
    CHECK(table.name_at(0x91) == Name{0x1000});
}

TEST_CASE(request_from_a_node_that_moved_is_nacked_at_its_new_address)
{
    // Moved here from the Request tests (it needs claimed addresses); the former AgIsoStack++ test
    // of the same name was in tests/unit/test_j1939_request.cpp.
    Bench bench;
    bench.add_node(0x20);
    bench.inject(at(1ms), claim(0x90, 0x3000));
    bench.step();
    bench.inject(at(11ms), claim(0x91, 0x3000));
    bench.inject(at(12ms), req(0x91, 0x20, kUnsupportedPgn));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {nack(0x20, 0x91, kUnsupportedPgn)});
}

TEST_CASE(request_in_the_same_step_as_the_requesters_claim_is_answered)
{
    // Moved here from the Request tests. The claim does not have to come first: a destination-specific
    // Request is answered whether or not the requester's claim was seen. (AgIsoStack++ does not
    // NACK the Request that comes before the claim of a node it does not know yet.)
    Bench bench;
    bench.add_node(0x20);
    bench.step();
    bench.inject(at(5ms), claim(0x90, 0x3000));
    bench.inject(at(6ms), req(0x90, 0x20, kUnsupportedPgn));
    bench.inject(at(7ms), req(0x91, 0x20, kUnsupportedPgn));
    bench.inject(at(8ms), claim(0x91, 0x3001));
    bench.step();
    CHECK_J1939(bench.take_sent(), {nack(0x20, 0x90, kUnsupportedPgn), nack(0x20, 0x91, kUnsupportedPgn)});
    CHECK_EQ(bench.network.address_table().size(), 2u);
}

TEST_CASE(cannot_claim_removes_the_control_function_from_the_address_table)
{
    // A NAME's old address is no longer its own after it could not claim. The former AgIsoStack++ stack kept it
    // (address_claim_cannot_claim_from_other_nodes_is_handled).
    Bench bench;
    bench.add_node(0x10);
    bench.inject(at(1ms), claim(0x90, 0x1000));
    bench.inject(at(2ms), claim(0x91, 0x2000));
    bench.step();
    bench.inject(at(11ms), cannot_claim(0x1000));
    bench.inject(at(12ms), cannot_claim(0x7777)); // not in the table: nothing happens
    bench.steps(2); // 10, 20
    const auto& table = bench.network.address_table();
    CHECK_EQ(table.size(), 1u);
    CHECK(!table.name_at(0x90).has_value());
    CHECK(table.name_at(0x91) == Name{0x2000});
    // It claims again later: the same NAME, at the new address
    // (AgIsoStack++: address_claim_partner_that_could_not_claim_is_found_when_it_claims_later).
    bench.inject(at(21ms), claim(0x92, 0x1000));
    bench.step();
    CHECK(table.address_of(Name{0x1000}) == Address{0x92});
    CHECK_EQ(table.size(), 2u);
}

TEST_CASE(address_table_keeps_the_lower_name_whatever_the_order)
{
    for (const bool lowerFirst : {true, false})
    {
        Bench bench;
        bench.add_node(0x10);
        if (lowerFirst)
        {
            bench.inject(at(1ms), claim(0x90, 0x1000));
            bench.inject(at(1ms), claim(0x90, 0x2000));
        }
        else
        {
            bench.inject(at(1ms), claim(0x90, 0x2000));
            bench.inject(at(1ms), claim(0x90, 0x1000));
        }
        bench.step();
        bench.step();
        CHECK(bench.network.address_table().name_at(0x90) == Name{0x1000});
        CHECK_EQ(bench.network.address_table().size(), 1u);
    }
}

TEST_CASE(address_table_and_claims_for_the_address_of_a_local_node)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step();
    // A higher NAME claims our address: it loses to us and is not entered.
    bench.inject(at(5ms), claim(kUs, kOurName + 1));
    bench.step();
    CHECK(!bench.network.address_table().name_at(kUs).has_value());
    CHECK(us.address_state() == AddressState::Claiming);
    // A lower NAME wins and is entered.
    bench.inject(at(15ms), claim(kUs, kOurName - 1));
    bench.step();
    CHECK(bench.network.address_table().name_at(kUs) == Name{kOurName - 1});
    CHECK(us.address_state() == AddressState::CannotClaim);
}

TEST_CASE(address_table_drops_a_control_function_that_a_local_node_beats)
{
    Bench bench;
    bench.add_node(0x10);
    bench.inject(at(1ms), claim(kUs, kOurName + 1)); // there before us
    bench.step();
    Node* us = nullptr;
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            us = &add_claiming(bench, kUs, kOurName);
        }
    };
    bench.step(); // 10: in the table, our node added
    CHECK(bench.network.address_table().name_at(kUs) == Name{kOurName + 1});
    bench.step(); // 20: our claim beats it
    CHECK(!bench.network.address_table().name_at(kUs).has_value());
    bench.inject(at(25ms), claim(kUs, kOurName + 1)); // it claims again and loses again
    bench.steps(2);
    CHECK(!bench.network.address_table().name_at(kUs).has_value());
    CHECK(us->address_state() == AddressState::Claiming);
}

TEST_CASE(address_table_ignores_local_nodes_and_short_claims)
{
    Bench bench;
    add_claiming(bench, 0x81, kOurName);
    add_claiming(bench, 0x82, kOurName + 1);
    bench.inject(at(1ms), frame(6, kPgnAddressClaim, 0x90, kGlobal, {1, 2, 3, 4, 5, 6, 7}));
    bench.steps(3);
    CHECK_EQ(bench.network.address_table().size(), 0u);
    CHECK_EQ(bench.network.statistics().address_claims_malformed, 1u);
}

TEST_CASE(address_table_holds_every_address_claimed_in_one_step)
{
    // AgIsoStack++: address_claim_of_many_nodes_in_one_step_fills_the_table.
    Bench bench;
    bench.add_node(0x10); // no NAME: does not contend
    for (unsigned a = 0; a < 254; ++a)
    {
        bench.inject(at(1ms), claim(static_cast<Address>(a), 0x1000 + a));
    }
    bench.steps(2);
    const auto& table = bench.network.address_table();
    CHECK_EQ(table.size(), 254u);
    CHECK(table.name_at(0) == Name{0x1000});
    CHECK(table.name_at(253) == Name{0x1000 + 253});
    CHECK_EQ(bench.network.statistics().receive_overflows, 0u);
}

// --- Arbitrary address capable nodes

TEST_CASE(arbitrary_capable_node_moves_to_the_lowest_free_address_when_it_loses)
{
    // AgIsoStack++: address_claim_lost_by_an_arbitrary_capable_cf_moves_to_a_free_address.
    Bench bench;
    Node& us = add_claiming(bench, kUs, kAacName);
    std::vector<std::string> changes;
    us.on_address_change([&](const AddressChange& c) { changes.push_back(describe(c)); });
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // held, dropped when we move
        }
        if (now == at(10ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // held at the new address
        }
    };
    bench.step(); // 0: claim 0x80
    bench.inject(at(2ms), claim(0x81, 0x1000));
    bench.inject(at(5ms), claim(kUs, kAacName - 1));
    bench.step(); // 10: 0x80 and 0x81 are taken
    CHECK_J1939(bench.take_sent(), {claim(kUs, kAacName), claim(0x82, kAacName)});
    CHECK_EQ(us.address(), 0x82);
    CHECK(us.address_state() == AddressState::Claiming);
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
    bench.steps(25); // .. 260: the wait from 10 ms ends
    CHECK(us.address_state() == AddressState::Claimed);
    CHECK_J1939(bench.take_sent(), {status(0x82)});
    CHECK(bench.sent_times.back() == at(260ms));
    CHECK(changes == (std::vector<std::string>{
                         change(AddressState::NotStarted, kUs, AddressState::Claiming, kUs, 0),
                         change(AddressState::Claiming, kUs, AddressState::Claiming, 0x82, 5),
                         change(AddressState::Claiming, 0x82, AddressState::Claimed, 0x82, 260),
                     }));
}

TEST_CASE(the_claim_at_the_new_address_also_answers_contention_in_the_same_step)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kAacName);
    bench.step();
    bench.take_sent();
    bench.inject(at(5ms), claim(kUs, kAacName - 1));   // lost: moves to 0x81
    bench.inject(at(5ms), claim(0x81, kAacName + 1));  // at the same time, a higher NAME at the new address
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(0x81, kAacName)});
    CHECK_EQ(us.address(), 0x81);
    bench.inject(at(11ms), claim(0x81, kAacName + 1)); // in the next step: answered again
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(0x81, kAacName)});
}

TEST_CASE(arbitrary_capable_node_without_an_address_claims_the_first_free_one)
{
    NetworkOptions options;
    options.arbitrary_address_first = 0xA0;
    options.arbitrary_address_last = 0xA3;
    Bench bench(options);
    Node& other = add_claiming(bench, 0xA0, kOurName); // a local node already uses 0xA0
    bench.inject(at(1ms), claim(0xA1, 0x1000));
    bench.step(); // 0
    Node* us = nullptr;
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            us = &add_claiming(bench, kNull, kAacName);
        }
    };
    bench.steps(2); // 10: added, 20: claims
    CHECK_EQ(us->address(), 0xA2);
    CHECK(bench.take_sent().back() == claim(0xA2, kAacName));
    CHECK_EQ(other.address(), 0xA0);
    // A node that is not arbitrary address capable needs an address.
    CHECK(throws<std::invalid_argument>([&] { bench.network.add_node(NodeOptions{kNull, Name{kOurName + 1}}); }));
    CHECK(throws<std::invalid_argument>([&] { bench.network.add_node(NodeOptions{kNull, std::nullopt}); }));
    CHECK(throws<std::invalid_argument>([&] { bench.network.add_node(NodeOptions{kGlobal, Name{kAacName + 1}}); }));
}

TEST_CASE(arbitrary_capable_node_that_finds_no_free_address_cannot_claim)
{
    NetworkOptions options;
    options.arbitrary_address_first = kUs;
    options.arbitrary_address_last = 0x81;
    Bench bench(options, 1ms);
    Node& us = add_claiming(bench, kUs, kAacName);
    bench.step();
    bench.inject(at(0ms), claim(0x81, 0x1000));
    bench.inject(at(5ms), claim(kUs, kAacName - 1));
    bench.steps(200);
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK_J1939(bench.take_sent(), {claim(kUs, kAacName), cannot_claim(kAacName)});
    CHECK(bench.sent_times[1] >= at(5ms) + cannot_claim_delay(kAacName));
    CHECK(bench.sent_times[1] < at(5ms) + cannot_claim_delay(kAacName) + 1ms);
}

TEST_CASE(arbitrary_capable_node_without_an_address_and_no_free_one_cannot_claim)
{
    NetworkOptions options;
    options.arbitrary_address_first = kUs;
    options.arbitrary_address_last = kUs;
    Bench bench(options, 1ms);
    bench.inject(at(0ms), claim(kUs, 0x1000));
    Node* us = nullptr;
    bench.app = [&](SimTime now) {
        if (now == at(1ms))
        {
            us = &add_claiming(bench, kNull, kAacName);
            CHECK(us->send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // held, then dropped
        }
    };
    bench.steps(200);
    CHECK(us->address_state() == AddressState::CannotClaim);
    CHECK_J1939(bench.take_sent(), {cannot_claim(kAacName)});
    // Counted from the step in which it would have claimed.
    CHECK(bench.sent_times[0] >= at(2ms) + cannot_claim_delay(kAacName));
    CHECK(bench.sent_times[0] < at(2ms) + cannot_claim_delay(kAacName) + 1ms);
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
}

TEST_CASE(local_arbitrary_capable_nodes_that_lose_together_choose_different_addresses)
{
    Bench bench;
    Node& a = add_claiming(bench, kUs, kAacName + 1);
    Node& b = add_claiming(bench, kUs, kAacName + 2);
    // Beats both. Handled in the step at 10 ms with their claims of 0x80 looped back at the same
    // time, and before them (external frames first): both lose to this frame.
    bench.inject(at(0ms), claim(kUs, kOurName));
    bench.steps(40);
    CHECK_EQ(a.address(), 0x81);
    CHECK_EQ(b.address(), 0x82);
    CHECK(a.address_state() == AddressState::Claimed);
    CHECK(b.address_state() == AddressState::Claimed);
}

namespace {

struct Outcome
{
    std::vector<std::string> trace;
    std::vector<std::pair<AddressState, Address>> nodes;
};

/// Three networks on one virtual bus, each with one node claiming 0x80 at time 0, stepped with
/// their own step sizes.
Outcome three_networks(const std::array<SimDuration, 3>& steps, const std::array<std::uint64_t, 3>& names)
{
    can::VirtualCanBus bus;
    can::Port& monitor = bus.attach();
    std::array<std::unique_ptr<Network>, 3> networks;
    std::array<std::unique_ptr<can::Host>, 3> hosts;
    std::array<Node*, 3> nodes{};
    can::StepDriver driver;
    for (std::size_t i = 0; i < 3; ++i)
    {
        networks[i] = std::make_unique<Network>();
        nodes[i] = &add_claiming(*networks[i], kUs, names[i]);
        hosts[i] = std::make_unique<can::Host>();
        hosts[i]->add(*networks[i], bus.attach());
        driver.add(*hosts[i], steps[i]);
    }
    driver.run_until(at(1s));
    Outcome out;
    monitor.release_before(at(1s), [&](const can::TimedFrame& f) {
        out.trace.push_back(std::to_string(vehicle_sil::to_ns(f.time)) + " " + to_string(from_can(f.frame)));
    });
    for (const Node* node : nodes)
    {
        out.nodes.emplace_back(node->address_state(), node->address());
    }
    return out;
}

} // namespace

TEST_CASE(arbitrary_capable_nodes_on_three_networks_settle_the_same_way_for_any_step_sizes)
{
    // A beats B (both arbitrary address capable) and the third NAME beats both. Both move to
    // 0x81 (each sees 0x80 taken), contend there, and B moves on to 0x82.
    const std::array<std::uint64_t, 3> names{kAacName + 1, kAacName + 2, kOurName};
    const std::vector<std::pair<AddressState, Address>> expected{
        {AddressState::Claimed, 0x81}, {AddressState::Claimed, 0x82}, {AddressState::Claimed, kUs}};
    // Only the outcome is compared across step sizes: which frames go out on the way depends on
    // what each node has received when it decides (a node with a shorter step moves before the
    // others see the claims), as on a real bus.
    for (const auto& steps : std::vector<std::array<SimDuration, 3>>{
             {10ms, 10ms, 10ms}, {1ms, 10ms, 5ms}, {10ms, 1ms, 3ms}, {3ms, 7ms, 10ms}, {7ms, 3ms, 1ms}})
    {
        const Outcome o = three_networks(steps, names);
        CHECK(o.nodes == expected);
        CHECK(three_networks(steps, names).trace == o.trace); // repeats identically
        // No Cannot Claim: everyone found an address.
        CHECK(std::none_of(o.trace.begin(), o.trace.end(),
                           [](const std::string& line) { return line.find("FE->FF") != std::string::npos; }));
    }
    // The order of the NAMEs, not of the networks, decides.
    const Outcome swapped = three_networks({10ms, 10ms, 10ms}, {kOurName, kAacName + 2, kAacName + 1});
    CHECK(swapped.nodes == (std::vector<std::pair<AddressState, Address>>{
                               {AddressState::Claimed, kUs}, {AddressState::Claimed, 0x82}, {AddressState::Claimed, 0x81}}));
}

TEST_CASE(name_priority_boundaries_decide_contention)
{
    struct Case
    {
        std::uint64_t ours;
        std::uint64_t theirs;
        bool win;
    };
    const std::vector<Case> cases{
        {0, 1, true},
        {1, 0, false},
        {0x7FFFFFFFFFFFFFFFu, 0x8000000000000000u, true}, // the arbitrary address capable bit
        {0x8000000000000000u, 0x7FFFFFFFFFFFFFFFu, false},
        {0xFFFFFFFFFFFFFFFEu, 0xFFFFFFFFFFFFFFFFu, true},
        {0xFFFFFFFFFFFFFFFFu, 0xFFFFFFFFFFFFFFFEu, false},
        {0x0000000100000000u, 0x00000000FFFFFFFFu, false}, // the byte order: byte 4 outweighs bytes 0..3
        {kOurName, kOurName, false},                       // the same NAME from another sender
    };
    for (const auto& c : cases)
    {
        Bench bench;
        Node& us = add_claiming(bench, 0x20, c.ours); // does not wait (unless arbitrary address capable)
        bench.step();
        bench.inject(at(5ms), claim(0x20, c.theirs));
        bench.steps(30);
        CHECK_EQ(us.address() == 0x20, c.win);
    }
}

// --- Address violations

TEST_CASE(address_violation_makes_us_claim_again)
{
    // AgIsoStack++: address_violation_makes_us_reclaim_our_address.
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.steps(31); // .. 300, claimed
    bench.take_sent();
    bench.inject(at(301ms), status(kUs));
    bench.inject(at(302ms), frame(6, kPgnPropA, kUs, 0x33, {1}));
    // AgIsoStack++ address_claim_shorter_than_eight_bytes_is_not_a_claim: a short claim from
    // our address is no claim, but a violation.
    bench.inject(at(303ms), frame(6, kPgnAddressClaim, kUs, kGlobal, {1, 2, 3}));
    bench.inject(at(303ms), status(kUs)); // the same time: one claim answers both
    bench.step(); // 310: a claim for each time
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName), claim(kUs, kOurName), claim(kUs, kOurName)});
    CHECK_EQ(bench.network.statistics().address_violations, 4u);
    CHECK_EQ(bench.network.statistics().address_claims_malformed, 1u);
    CHECK(us.address_state() == AddressState::Claimed);
    bench.inject(at(315ms), status(kUs));
    bench.step(); // 320: again in the next step
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    // A Request for Address Claimed from our address is answered (once), not a violation too.
    bench.inject(at(325ms), req(kUs, kGlobal, kPgnAddressClaim));
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    CHECK_EQ(bench.network.statistics().address_violations, 5u);
}

TEST_CASE(address_violation_during_the_wait_and_without_an_address)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    bench.step();
    bench.take_sent();
    bench.inject(at(5ms), status(kUs));
    bench.step(); // during the wait: claimed again
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    bench.inject(at(15ms), claim(kUs, kOurName - 1)); // lost
    bench.steps(20);
    bench.take_sent();
    bench.inject(at(215ms), status(kUs)); // not our address any more
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
    CHECK(us.address_state() == AddressState::CannotClaim);
    CHECK_EQ(bench.network.statistics().address_violations, 1u);
}

TEST_CASE(nodes_without_a_name_do_not_detect_violations)
{
    Bench bench;
    bench.add_node(kUs);
    bench.inject(at(1ms), status(kUs));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(bench.network.statistics().address_violations, 0u);
}

TEST_CASE(contention_at_one_time_is_answered_with_one_claim)
{
    // AgIsoStack++ (address_claim_duplicated_in_one_step_is_answered_once) answers once per
    // step; here one claim answers the claims of one time, so the count does not depend on the
    // step size.
    Bench bench;
    add_claiming(bench, kUs, kOurName);
    bench.steps(31); // .. 300
    bench.take_sent();
    bench.inject(at(301ms), claim(kUs, kOurName + 1));
    bench.inject(at(301ms), claim(kUs, kOurName + 1));
    bench.inject(at(301ms), claim(kUs, kOurName + 2));
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName)});
    bench.inject(at(311ms), claim(kUs, kOurName + 1));
    bench.inject(at(312ms), claim(kUs, kOurName + 1));
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName), claim(kUs, kOurName)});
}

TEST_CASE(claims_answering_contention_and_violations_do_not_depend_on_the_step_size)
{
    std::vector<std::vector<J1939Frame>> results;
    for (const SimDuration step : {1ms, 3ms, 10ms, 50ms})
    {
        Bench bench({}, step);
        add_claiming(bench, kUs, kOurName);
        while (bench.next_time() <= at(300ms))
        {
            bench.step();
        }
        bench.take_sent();
        for (int ms = 301; ms <= 340; ++ms)
        {
            bench.inject(at(std::chrono::milliseconds(ms)), ms % 3 == 0 ? claim(kUs, kOurName + 1) : status(kUs));
        }
        bench.inject(at(320ms), claim(kUs, kOurName + 2)); // with a violation at the same time
        while (bench.next_time() <= at(500ms))
        {
            bench.step();
        }
        results.push_back(bench.take_sent());
    }
    CHECK_EQ(results[0].size(), 40u); // one claim per time
    for (const auto& r : results)
    {
        CHECK(r == results[0]);
    }
}

// --- Commanded Address

namespace {

/// A claimed arbitrary address capable node at 0x80 (from 260 ms on, 10 ms steps).
struct Commanded
{
    Commanded() : us(add_claiming(bench, kUs, kAacName))
    {
        us.on_address_change([this](const AddressChange& c) { changes.push_back(describe(c)); });
        bench.app = [this](SimTime now) {
            if (app)
            {
                app(now);
            }
        };
        bench.steps(27); // .. 260
        bench.take_sent();
        changes.clear();
    }

    Bench bench;
    Node& us;
    std::vector<std::string> changes;
    std::function<void(SimTime)> app;
};

} // namespace

TEST_CASE(commanded_address_moves_an_arbitrary_capable_node)
{
    // AgIsoStack++: commanded_address_moves_an_arbitrary_capable_cf.
    Commanded t;
    const auto data = commanded(kAacName, 0x95);
    t.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            CHECK(t.us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // queued from 0x80: dropped
            NetworkTestAccess::deliver_message(t.bench.network, commanded_message(data, at(295ms)));
            CHECK_EQ(t.us.address(), 0x95);
            CHECK(t.us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // held: the wait starts again
        }
    };
    t.bench.steps(5); // .. 300
    CHECK_J1939(t.bench.take_sent(), {claim(0x95, kAacName)});
    CHECK_EQ(t.bench.network.statistics().held_dropped, 1u);
    t.bench.steps(25); // .. 550
    CHECK_J1939(t.bench.take_sent(), {status(0x95)});
    CHECK(t.bench.sent_times.back() == at(550ms));
    CHECK(t.us.address_state() == AddressState::Claimed);
    CHECK(t.changes == (std::vector<std::string>{
                           change(AddressState::Claimed, kUs, AddressState::Claiming, 0x95, 295),
                           change(AddressState::Claiming, 0x95, AddressState::Claimed, 0x95, 550),
                       }));
}

TEST_CASE(commanded_address_during_the_wait_drops_what_the_node_holds)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kAacName);
    const auto data = commanded(kAacName, 0x95);
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // held for 0x80
        }
        if (now == at(50ms))
        {
            NetworkTestAccess::deliver_message(bench.network, commanded_message(data, now));
        }
    };
    bench.steps(31); // .. 300: the wait from 50 ms ends at 300
    CHECK_J1939(bench.take_sent(), {claim(kUs, kAacName), claim(0x95, kAacName)});
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
    CHECK(us.address_state() == AddressState::Claimed);
}

TEST_CASE(frames_queued_before_moving_away_and_back_do_not_leave)
{
    Commanded t;
    const auto away = commanded(kAacName, 0x95);
    const auto back = commanded(kAacName, kUs);
    t.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            CHECK(t.us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // queued from 0x80
            NetworkTestAccess::deliver_message(t.bench.network, commanded_message(away, now));
            NetworkTestAccess::deliver_message(t.bench.network, commanded_message(back, now));
        }
    };
    t.bench.steps(5);
    // The message and the claim of 0x95 belong to earlier addresses of the node.
    CHECK_J1939(t.bench.take_sent(), {claim(kUs, kAacName)});
    CHECK_EQ(t.bench.network.statistics().held_dropped, 2u);
}

TEST_CASE(commanded_address_to_our_own_address_confirms_it)
{
    Commanded t;
    const auto data = commanded(kAacName, kUs);
    t.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            NetworkTestAccess::deliver_message(t.bench.network, commanded_message(data, at(295ms), kUs));
        }
    };
    t.bench.steps(5);
    CHECK_J1939(t.bench.take_sent(), {claim(kUs, kAacName)});
    CHECK(t.us.address_state() == AddressState::Claimed);
    CHECK(t.changes.empty());
}

TEST_CASE(invalid_commanded_addresses_are_ignored)
{
    // AgIsoStack++: commanded_address_is_ignored_by_a_cf_that_is_not_arbitrary_capable.
    Bench bench;
    Node& single = add_claiming(bench, 0x20, kOurName);
    Node& aac = add_claiming(bench, kUs, kAacName);
    bench.steps(27);
    bench.take_sent();
    const auto toSingle = commanded(kOurName, 0x30);
    const auto to254 = commanded(kAacName, kNull);
    const auto to255 = commanded(kAacName, kGlobal);
    const auto other = commanded(kAacName + 7, 0x30);
    const auto shortOne = commanded(kAacName, 0x30);
    const std::array<std::uint8_t, 10> longOne{0x34, 0x12, 0x00, 0xFE, 0x00, 0x82, 0x00, 0xA0, 0x30, 0x00};
    bench.app = [&](SimTime now) {
        if (now == at(270ms))
        {
            auto& network = bench.network;
            NetworkTestAccess::deliver_message(network, commanded_message(toSingle, now));                            // not arbitrary capable
            NetworkTestAccess::deliver_message(network, commanded_message(to254, now));                               // no address
            NetworkTestAccess::deliver_message(network, commanded_message(to255, now));                               // no address
            NetworkTestAccess::deliver_message(network, commanded_message(other, now));                               // another NAME: not counted
            NetworkTestAccess::deliver_message(network, commanded_message(std::span(shortOne).first(8), now));        // 8 bytes
            NetworkTestAccess::deliver_message(network, commanded_message(longOne, now));                             // 10 bytes
            NetworkTestAccess::deliver_message(network, commanded_message(shortOne, now, 0x33));                      // to someone else
        }
    };
    bench.steps(3);
    CHECK_J1939(bench.take_sent(), {});
    CHECK_EQ(single.address(), 0x20);
    CHECK_EQ(aac.address(), kUs);
    CHECK_EQ(bench.network.statistics().commanded_addresses_ignored, 5u);
}

TEST_CASE(commanded_address_gives_a_node_without_an_address_a_new_one)
{
    NetworkOptions options;
    options.arbitrary_address_first = kUs;
    options.arbitrary_address_last = kUs;
    Bench bench(options);
    Node& us = add_claiming(bench, kUs, kAacName);
    CHECK(cannot_claim_delay(kAacName) > 15ms); // still pending when the command comes at 20 ms
    bench.step();
    bench.inject(at(5ms), claim(kUs, kOurName));
    bench.step(); // 10: lost, no free address; Cannot Claim scheduled
    CHECK(us.address_state() == AddressState::CannotClaim);
    const auto data = commanded(kAacName, 0x40);
    bench.app = [&](SimTime now) {
        if (now == at(20ms))
        {
            NetworkTestAccess::deliver_message(bench.network, commanded_message(data, now));
        }
    };
    bench.steps(40);
    // The Cannot Claim that was due is not sent any more.
    CHECK_J1939(bench.take_sent(), {claim(kUs, kAacName), claim(0x40, kAacName)});
    CHECK(us.address_state() == AddressState::Claimed);
    CHECK_EQ(us.address(), 0x40);
}

TEST_CASE(cannot_claim_after_a_move_is_counted_from_the_new_loss)
{
    // A Cannot Claim that was due before the node moved does not leave at its old time when the
    // node loses again.
    NetworkOptions options;
    options.arbitrary_address_first = kUs;
    options.arbitrary_address_last = kUs;
    Bench bench(options, 1ms);
    Node& us = add_claiming(bench, kUs, kAacName);
    CHECK(cannot_claim_delay(kAacName) > 15ms);
    bench.step();
    bench.inject(at(0ms), claim(kUs, kOurName));
    bench.step(); // 1: lost at 0, no free address
    const auto data = commanded(kAacName, 0x40);
    bench.app = [&](SimTime now) {
        if (now == at(2ms))
        {
            NetworkTestAccess::deliver_message(bench.network, commanded_message(data, now));
        }
    };
    bench.step(); // 2: claims 0x40
    bench.inject(at(5ms), claim(0x40, kOurName + 1)); // lost again
    bench.steps(200);
    CHECK_J1939(bench.take_sent(), {claim(kUs, kAacName), claim(0x40, kAacName), cannot_claim(kAacName)});
    CHECK(bench.sent_times[2] >= at(5ms) + cannot_claim_delay(kAacName));
    CHECK(us.address_state() == AddressState::CannotClaim);
}

TEST_CASE(cannot_claim_due_in_the_step_of_a_move_does_not_leave)
{
    // The Cannot Claim timer fires in process(); the command moves the node in the application
    // stage of the same step: the Cannot Claim must not follow the new claim.
    NetworkOptions options;
    options.arbitrary_address_first = kUs;
    options.arbitrary_address_last = kUs;
    Bench bench(options, 1ms);
    Node& us = add_claiming(bench, kUs, kAacName);
    bench.step();
    bench.inject(at(0ms), claim(kUs, kOurName)); // lost at 0, no free address
    const auto data = commanded(kAacName, 0x40);
    bool commanded_ = false;
    bench.app = [&](SimTime now) {
        if (!commanded_ && now >= at(0ms) + cannot_claim_delay(kAacName))
        {
            commanded_ = true;
            NetworkTestAccess::deliver_message(bench.network, commanded_message(data, now));
        }
    };
    bench.steps(200);
    CHECK_J1939(bench.take_sent(), {claim(kUs, kAacName), claim(0x40, kAacName)});
    CHECK_EQ(us.address(), 0x40);
    CHECK_EQ(bench.network.statistics().held_dropped, 1u);
}

TEST_CASE(request_for_address_claimed_from_254_still_leaves_after_a_move)
{
    NetworkOptions options;
    options.arbitrary_address_first = kUs;
    options.arbitrary_address_last = kUs;
    Bench bench(options);
    Node& us = add_claiming(bench, kUs, kAacName);
    bench.step();
    bench.inject(at(5ms), claim(kUs, kOurName));
    bench.step(); // 10: lost, no free address
    bench.take_sent();
    const auto data = commanded(kAacName, 0x40);
    bench.app = [&](SimTime now) {
        if (now == at(20ms))
        {
            CHECK(us.request(Pgn{kPgnAddressClaim}) == SendStatus::Ok); // from 254
            NetworkTestAccess::deliver_message(bench.network, commanded_message(data, now));
        }
    };
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {req(kNull, kGlobal, kPgnAddressClaim), claim(0x40, kAacName)});
}

TEST_CASE(commanded_address_to_a_taken_address_is_settled_by_contention)
{
    Commanded t;
    t.bench.inject(at(261ms), claim(0x95, kOurName)); // a lower NAME already at 0x95
    const auto data = commanded(kAacName, 0x95);
    t.app = [&](SimTime now) {
        if (now == at(300ms))
        {
            NetworkTestAccess::deliver_message(t.bench.network, commanded_message(data, now));
        }
    };
    t.bench.steps(4); // .. 300: claims 0x95
    CHECK_J1939(t.bench.take_sent(), {claim(0x95, kAacName)});
    t.bench.inject(at(305ms), claim(0x95, kOurName)); // the owner defends it
    t.bench.step(); // 310: lost, moves to the lowest free address
    CHECK_J1939(t.bench.take_sent(), {claim(kUs, kAacName)});
    CHECK_EQ(t.us.address(), kUs);
}

TEST_CASE(deliver_message_ignores_what_receive_would_ignore)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    std::vector<std::uint8_t> seen;
    us.on_any_message([&](const Message& m) { seen.push_back(m.source); });
    const auto name = name_bytes(0x1000);
    const std::array<std::uint8_t, 0> empty{};
    bench.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            auto& network = bench.network;
            NetworkTestAccess::deliver_message(network, Message{Pgn{kPgnAddressClaim}, 6, kGlobal, kGlobal, now, name}); // source 255
            NetworkTestAccess::deliver_message(network, Message{Pgn{kPgnPropB}, 6, kNull, kGlobal, now, empty});        // from 254
            NetworkTestAccess::deliver_message(network, Message{Pgn{0xEF01}, 6, kTool, kGlobal, now, empty});           // invalid PGN
            NetworkTestAccess::deliver_message(network, Message{Pgn{kPgnAddressClaim}, 6, kNull, kGlobal, now, name}); // Cannot Claim: taken
        }
    };
    bench.steps(2);
    CHECK_EQ(bench.network.statistics().frames_ignored, 3u);
    CHECK(seen == (std::vector<std::uint8_t>{kNull}));
    CHECK_EQ(bench.network.address_table().size(), 0u);
}

TEST_CASE(deliver_message_is_only_allowed_in_the_application_stage)
{
    Bench bench;
    bench.add_node(0x10);
    const auto data = commanded(kAacName, 0x30);
    CHECK(throws<std::logic_error>([&] { NetworkTestAccess::deliver_message(bench.network, commanded_message(data, at(0ms))); }));
    CHECK(bench.network.failed());

    Bench later;
    later.add_node(0x10);
    later.app = [&](SimTime now) {
        if (now == at(10ms))
        {
            NetworkTestAccess::deliver_message(later.network, commanded_message(data, at(11ms))); // after the step time
        }
    };
    later.step();
    CHECK(throws<std::logic_error>([&] { later.step(); }));
    CHECK(later.network.failed());
}

// --- Address change handlers

TEST_CASE(address_change_handlers_see_every_change_and_may_send)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    std::vector<std::string> changes;
    us.on_address_change([&](const AddressChange& c) {
        changes.push_back(describe(c));
        if (c.state == AddressState::Claimed)
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);
        }
        if (c.state == AddressState::CannotClaim)
        {
            CHECK(us.send(Pgn{kPgnPropB}, {}) == SendStatus::NoAddress);
        }
    });
    const auto removed = us.on_address_change([&](const AddressChange&) { changes.push_back("removed"); });
    CHECK(us.remove_handler(removed));
    bench.steps(26); // .. 250
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName), status(kUs)});
    bench.inject(at(255ms), claim(kUs, kOurName - 1));
    bench.steps(20);
    CHECK(changes == (std::vector<std::string>{
                         change(AddressState::NotStarted, kUs, AddressState::Claiming, kUs, 0),
                         change(AddressState::Claiming, kUs, AddressState::Claimed, kUs, 250),
                         change(AddressState::Claimed, kUs, AddressState::CannotClaim, kNull, 255),
                     }));
    CHECK(throws<std::invalid_argument>([&] { us.on_address_change({}); }));
}

TEST_CASE(address_change_handler_registered_in_a_handler_takes_effect_after_it)
{
    Bench bench;
    Node& us = add_claiming(bench, kUs, kOurName);
    std::vector<std::string> changes;
    us.on_address_change([&](const AddressChange& c) {
        changes.push_back("first " + describe(c));
        if (c.state == AddressState::Claiming)
        {
            us.on_address_change([&](const AddressChange& d) { changes.push_back("second " + describe(d)); });
        }
    });
    bench.steps(26);
    CHECK(changes == (std::vector<std::string>{
                         "first " + change(AddressState::NotStarted, kUs, AddressState::Claiming, kUs, 0),
                         "first " + change(AddressState::Claiming, kUs, AddressState::Claimed, kUs, 250),
                         "second " + change(AddressState::Claiming, kUs, AddressState::Claimed, kUs, 250),
                     }));
}

// --- Several nodes on one network

TEST_CASE(nodes_of_one_network_keep_their_own_addresses_and_handlers)
{
    Bench bench;
    Node& loser = add_claiming(bench, kUs, kOurName + 1);
    Node& claimer = add_claiming(bench, 0x81, kOurName + 2);
    Node& fixed = bench.add_node(0x30);
    // What each node gets of the tester's frames (at 5..7 ms; the looped-back frames of the
    // other local nodes have step times).
    std::vector<std::string> seen;
    const auto record = [&](const char* who) {
        return [&seen, who](const Message& m) {
            if (m.time >= at(5ms) && m.time <= at(7ms))
            {
                seen.push_back(std::string(who) + " " + std::to_string(m.pgn.value()));
            }
        };
    };
    loser.on_any_message(record("loser"));
    claimer.on_any_message(record("claimer"));
    fixed.on_any_message(record("fixed"));
    bench.app = [&](SimTime now) {
        if (now == at(0ms))
        {
            CHECK(fixed.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok);   // at once
            CHECK(claimer.send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // after its wait
        }
    };
    bench.step(); // 0
    CHECK_J1939(bench.take_sent(), {claim(kUs, kOurName + 1), claim(0x81, kOurName + 2), status(0x30)});
    bench.inject(at(5ms), claim(kUs, kOurName));            // only the loser loses
    bench.inject(at(6ms), req(kTool, 0x81, kUnsupportedPgn)); // only the claimer answers (held)
    bench.inject(at(7ms), frame(6, kPgnPropA, kTool, 0x30, {}));
    bench.steps(25); // .. 250
    CHECK(loser.address_state() == AddressState::CannotClaim);
    CHECK(claimer.address_state() == AddressState::Claimed);
    CHECK(fixed.address_state() == AddressState::Fixed);
    CHECK(seen == (std::vector<std::string>{
                      "loser 60928", "claimer 60928", "fixed 60928", // the claim to all
                      "claimer 59904",                               // the request to 0x81
                      "fixed 61184",                                 // PropA to 0x30
                  }));
    const auto sent = bench.take_sent();
    CHECK(std::count(sent.begin(), sent.end(), nack(0x81, kTool, kUnsupportedPgn)) == 1);
    CHECK(std::count(sent.begin(), sent.end(), status(0x81)) == 1);
    CHECK(std::count(sent.begin(), sent.end(), cannot_claim(kOurName + 1)) == 1);
}

// --- Several networks on one host

namespace {

/// Two networks on one host, registered in the given order, and a tester on the same bus.
struct TwoNetworks
{
    explicit TwoNetworks(bool aFirst)
    {
        if (aFirst)
        {
            host.add(a, bus.attach());
            host.add(b, bus.attach());
        }
        else
        {
            host.add(b, bus.attach());
            host.add(a, bus.attach());
        }
        host.set_application([this](SimTime now) {
            if (app)
            {
                app(now);
            }
        });
    }

    void step()
    {
        host.step(next);
        next += 10ms;
    }

    std::vector<std::pair<SimTime, J1939Frame>> take_sent()
    {
        std::vector<std::pair<SimTime, J1939Frame>> out;
        tester.release_before(next, [&](const can::TimedFrame& f) { out.emplace_back(f.time, from_can(f.frame)); });
        return out;
    }

    can::VirtualCanBus bus;
    can::Port& tester = bus.attach();
    Network a;
    Network b;
    can::Host host;
    std::function<void(SimTime)> app;
    SimTime next{};
};

} // namespace

TEST_CASE(a_handler_of_one_network_sends_through_another_network_of_the_host)
{
    for (const bool aFirst : {true, false})
    {
        TwoNetworks t(aFirst);
        Node& inside = t.a.add_node({0x10});
        Node& outside = t.b.add_node({0x20});
        inside.on_message(Pgn{kPgnPropA}, [&](const Message& m) {
            if (m.source == kTool)
            {
                const std::array<std::uint8_t, 2> data{0x47, m.data.empty() ? std::uint8_t{0} : m.data[0]};
                CHECK(outside.send(Pgn{kPgnPropB}, data) == SendStatus::Ok);
            }
        });
        t.step(); // 0
        t.tester.send(can::TimedFrame{at(5ms), to_can(frame(6, kPgnPropA, kTool, 0x10, {9}))});
        t.step(); // 10: forwarded in the same step
        t.step();
        const auto sent = t.take_sent();
        CHECK_EQ(sent.size(), 1u);
        CHECK(sent[0].first == at(10ms));
        CHECK(sent[0].second == frame(6, kPgnPropB, 0x20, kGlobal, {0x47, 9}));
    }
}

TEST_CASE(node_added_from_a_handler_of_another_network_takes_part_from_the_next_step)
{
    // Whether the other network handles its frames before or after (the host's order), the new
    // node claims in the next step.
    for (const bool aFirst : {true, false})
    {
        TwoNetworks t(aFirst);
        Node& inside = t.a.add_node({0x10});
        Node* added = nullptr;
        std::vector<SimTime> seen;
        inside.on_message(Pgn{kPgnPropA}, [&](const Message&) {
            added = &add_claiming(t.b, 0x20, kOurName);
            added->on_any_message([&](const Message& m) { seen.push_back(m.time); });
            CHECK(added->send(Pgn{kPgnPropB}, {}) == SendStatus::Ok); // held until its claim
        });
        t.step(); // 0
        t.tester.send(can::TimedFrame{at(5ms), to_can(frame(6, kPgnPropA, kTool, 0x10, {}))});
        t.tester.send(can::TimedFrame{at(6ms), to_can(status(kTool))}); // not for the new node yet
        t.step(); // 10: added
        CHECK(added != nullptr);
        CHECK(added->address_state() == AddressState::NotStarted);
        t.step(); // 20: claims (0x20 does not wait)
        CHECK(added->address_state() == AddressState::Claiming);
        const auto sent = t.take_sent();
        CHECK_EQ(sent.size(), 2u);
        CHECK(sent[0].first == at(20ms));
        CHECK(sent[0].second == claim(0x20, kOurName));
        CHECK(sent[1].second == status(0x20));
        CHECK(seen.empty());
        t.tester.send(can::TimedFrame{at(25ms), to_can(status(kTool))});
        // From the host's application stage too.
        t.app = [&](SimTime now) {
            if (now == at(30ms))
            {
                add_claiming(t.a, 0x21, kOurName + 1);
            }
        };
        t.step(); // 30
        CHECK(seen == (std::vector<SimTime>{at(25ms)})); // takes part now
        t.step(); // 40
        const auto later = t.take_sent();
        CHECK_EQ(later.size(), 1u);
        CHECK(later[0].first == at(40ms));
    }
}

TEST_CASE(add_node_from_a_handler_of_the_same_network_fails_it)
{
    Bench bench;
    Node& inside = bench.add_node(0x10);
    inside.on_message(Pgn{kPgnPropB}, [&](const Message&) { bench.network.add_node({0x11}); });
    bench.inject(at(0ms), status(kTool));
    bench.step();
    CHECK(throws<std::logic_error>([&] { bench.step(); }));
    CHECK(bench.network.failed());
    CHECK(bench.host.failed());
}

TEST_CASE(a_failing_network_fails_the_whole_host)
{
    for (const bool aFirst : {true, false})
    {
        TwoNetworks t(aFirst);
        Node& inside = t.a.add_node({0x10});
        Node& outside = t.b.add_node({0x20});
        inside.on_message(Pgn{kPgnPropB}, [](const Message&) { throw std::runtime_error("handler failed"); });
        int outsideSeen = 0;
        outside.on_message(Pgn{kPgnPropB}, [&](const Message&) { ++outsideSeen; });
        t.step();
        t.tester.send(can::TimedFrame{at(5ms), to_can(status(kTool))});
        CHECK(throws<std::runtime_error>([&] { t.step(); }));
        CHECK(t.host.failed());
        CHECK(t.a.failed());
        CHECK(!t.b.failed());
        CHECK(t.host.error().find("handler failed") != std::string::npos);
        // Every later step fails; the other network is not stepped any more.
        CHECK(throws<std::logic_error>([&] { t.step(); }));
        CHECK(t.take_sent().empty());
        CHECK(outsideSeen == (aFirst ? 0 : 1));
    }
}
