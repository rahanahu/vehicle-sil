// SPDX-License-Identifier: MIT
//
// Other values for a node than its code gives (vehicle_sil/j1939/node_override.hpp,
// NetworkOptions::node_overrides): the functions that apply an entry and say what a list of
// entries does, the entries as text, and a Network that gives its nodes the values: what the node
// claims with, what add_node() refuses, and that nothing later falls back to the code's values.
#include "j1939_bench.hpp"
#include "network_test_access.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/name.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/node_override.hpp"

#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AddressState;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Name;
using vehicle_sil::j1939::Network;
using vehicle_sil::j1939::NetworkOptions;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::NodeOptions;
using vehicle_sil::j1939::NodeOverride;
using vehicle_sil::j1939::NodeOverrideClash;
using vehicle_sil::j1939::NodeOverrideError;
using vehicle_sil::j1939::NodeOverrideMatch;
using vehicle_sil::j1939::NodeOverrideTextError;
using vehicle_sil::j1939::NodeValues;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::detail::NetworkTestAccess;
using vehicle_sil::unit::throws;
namespace can = vehicle_sil::can;
namespace j = vehicle_sil::j1939;

namespace {

constexpr std::uint32_t kPgnCommandedAddress = 0xFED8;
/// Not arbitrary address capable; manufacturer code 0x7F0, identity number 0x1234.
constexpr std::uint64_t kOurName = 0x20008200FE001234u;
/// kOurName with identity number 2.
constexpr std::uint64_t kOurNameSecond = 0x20008200FE000002u;
/// Arbitrary address capable (bit 63).
constexpr std::uint64_t kAacName = 0xA0008200FE001234u;
constexpr std::uint64_t kAacNameSecond = 0xA0008200FE000002u;

const std::string kWho = "vehicle_sil::j1939::Network: ";

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
J1939Frame req(Address from, Address to, std::uint32_t pgn) { return frame(6, kPgnRequest, from, to, request(pgn)); }

/// An entry: `from`, `address`, and the fields of the NAME.
NodeOverride entry(std::optional<Address> from, std::optional<Address> address,
                   std::optional<std::uint32_t> identity_number = std::nullopt,
                   std::optional<std::uint8_t> function_instance = std::nullopt,
                   std::optional<std::uint8_t> ecu_instance = std::nullopt)
{
    NodeOverride out;
    out.from = from;
    out.address = address;
    out.identity_number = identity_number;
    out.function_instance = function_instance;
    out.ecu_instance = ecu_instance;
    return out;
}

NetworkOptions with(std::vector<NodeOverride> entries)
{
    NetworkOptions options;
    options.node_overrides = std::move(entries);
    return options;
}

NodeValues values(Address address, std::optional<std::uint64_t> name = std::nullopt)
{
    return NodeValues{address, name ? std::optional<Name>(Name{*name}) : std::nullopt};
}

/// what() of the std::invalid_argument that `f` throws; "(nothing thrown)" without one.
template <typename F>
std::string refusal(F&& f)
{
    try
    {
        f();
    }
    catch (const std::invalid_argument& e)
    {
        return e.what();
    }
    return "(nothing thrown)";
}

void check_text(const std::string& actual, const std::string& expected, const char* file, int line)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure(std::string(file) + ":" + std::to_string(line) + ": texts differ\n  actual:   " +
                                         actual + "\n  expected: " + expected);
    }
}

#define CHECK_TEXT(actual, expected) check_text((actual), (expected), __FILE__, __LINE__)

} // namespace

// --- One entry and one node

TEST_CASE(node_override_sets_each_value_and_keeps_the_rest_of_the_name)
{
    const NodeValues coded = values(0x00, kOurName);
    {
        const auto out = j::apply_node_override(coded, entry(std::nullopt, 0x01));
        CHECK(!out.error);
        CHECK(out.values == values(0x01, kOurName));
    }
    {
        const auto out = j::apply_node_override(coded, entry(0x00, std::nullopt, 2));
        CHECK(!out.error);
        CHECK(out.values == values(0x00, kOurNameSecond));
    }
    {
        // Function instance: bits 35..39; ECU instance: bits 32..34.
        const auto out = j::apply_node_override(coded, entry(std::nullopt, std::nullopt, std::nullopt, 3, 5));
        CHECK(!out.error);
        CHECK(out.values == values(0x00, kOurName | (std::uint64_t{3} << 35) | (std::uint64_t{5} << 32)));
    }
    {
        // All of them, on a NAME whose fields are all ones: only the three fields change.
        const std::uint64_t ones = 0xFFFFFFFFFFFFFFFFu;
        const auto out = j::apply_node_override(values(0x10, ones), entry(0x10, 0xFD, 0, 0, 0));
        CHECK(!out.error);
        CHECK(out.values == values(0xFD, 0xFFFFFF00FFE00000u));
    }
    // `from` is not looked at here.
    CHECK(j::apply_node_override(coded, entry(0x55, 0x01)).values == values(0x01, kOurName));
    // An entry that changes nothing gives the code's values.
    CHECK(j::apply_node_override(coded, entry(0x00, std::nullopt)).values == coded);
    CHECK(!entry(0x00, std::nullopt).changes());
    CHECK(entry(std::nullopt, 0x01).changes());
    CHECK(!entry(std::nullopt, 0x01).changes_name());
    CHECK(entry(std::nullopt, std::nullopt, std::nullopt, std::nullopt, 1).changes_name());
}

TEST_CASE(node_override_refuses_a_value_above_its_range_instead_of_cutting_it)
{
    const NodeValues coded = values(0x00, kOurName);
    // The largest values pass ...
    {
        const auto out = j::apply_node_override(coded, entry(std::nullopt, 253, 2097151, 31, 7));
        CHECK(!out.error);
        CHECK_EQ(out.values.address, 253);
        CHECK_EQ(out.values.name->identity_number(), 2097151u);
        CHECK_EQ(out.values.name->function_instance(), 31u);
        CHECK_EQ(out.values.name->ecu_instance(), 7u);
        CHECK_EQ(out.values.name->manufacturer_code(), 0x7F0u);
    }
    // ... and one more does not, and leaves the code's values (2097152 would be 0 after a cut).
    for (const NodeOverride& above : {entry(std::nullopt, std::nullopt, 2097152), entry(std::nullopt, std::nullopt, std::nullopt, 32),
                                      entry(std::nullopt, std::nullopt, std::nullopt, std::nullopt, 8)})
    {
        const auto out = j::apply_node_override(coded, above);
        CHECK(out.error == NodeOverrideError::FieldOutOfRange);
        CHECK(out.values == coded);
    }
    {
        const auto out = j::apply_node_override(coded, entry(std::nullopt, 255));
        CHECK(out.error == NodeOverrideError::AddressOutOfRange);
        CHECK(out.values == coded);
    }
    CHECK_TEXT(j::node_override_range_problem(entry(std::nullopt, std::nullopt, 2097152)),
             std::string("identity_number 2097152, which is out of its range 0..2097151"));
    CHECK_TEXT(j::node_override_range_problem(entry(std::nullopt, std::nullopt, std::nullopt, 32)),
             std::string("function_instance 32, which is out of its range 0..31"));
    CHECK_TEXT(j::node_override_range_problem(entry(std::nullopt, std::nullopt, std::nullopt, std::nullopt, 8)),
             std::string("ecu_instance 8, which is out of its range 0..7"));
    CHECK_TEXT(j::node_override_range_problem(entry(std::nullopt, 255)), std::string("address 255, which is out of its range 0..254"));
    CHECK_TEXT(j::node_override_range_problem(entry(255, 1)), std::string("from 255, which is out of its range 0..254"));
    CHECK_TEXT(j::node_override_range_problem(entry(254, 254, 2097151, 31, 7)), std::string());
}

TEST_CASE(node_override_needs_a_name_for_a_name_field_and_a_capable_name_for_any_address)
{
    const NodeValues unnamed = values(0x00);
    CHECK(j::apply_node_override(unnamed, entry(std::nullopt, 0x01)).values == values(0x01));
    for (const NodeOverride& field : {entry(std::nullopt, std::nullopt, 2), entry(std::nullopt, 0x01, std::nullopt, 1),
                                      entry(std::nullopt, std::nullopt, std::nullopt, std::nullopt, 1)})
    {
        const auto out = j::apply_node_override(unnamed, field);
        CHECK(out.error == NodeOverrideError::NameFieldWithoutName);
        CHECK(out.values == unnamed);
    }
    // 254: any free address.
    CHECK(j::apply_node_override(values(0x00, kAacName), entry(std::nullopt, 254)).values == values(254, kAacName));
    CHECK(j::apply_node_override(values(0x00, kOurName), entry(std::nullopt, 254)).error == NodeOverrideError::AnyAddressNotCapable);
    CHECK(j::apply_node_override(unnamed, entry(std::nullopt, 254)).error == NodeOverrideError::AnyAddressWithoutName);
}

TEST_CASE(node_override_is_for_the_node_at_its_from_or_for_any_node_without_one)
{
    CHECK(j::node_override_matches(entry(0x00, 0x01), 0x00));
    CHECK(!j::node_override_matches(entry(0x00, 0x01), 0x01));
    CHECK(j::node_override_matches(entry(254, 0x01), 254));
    CHECK(j::node_override_matches(entry(std::nullopt, 0x01), 0x00));
    CHECK(j::node_override_matches(entry(std::nullopt, 0x01), 0x21));
}

TEST_CASE(node_override_lists_are_checked_for_two_entries_for_one_node)
{
    const auto clash = [](std::vector<NodeOverride> entries) { return j::find_node_override_clash(entries); };
    CHECK(!clash({}));
    CHECK(!clash({entry(std::nullopt, 0x01)}));
    CHECK(!clash({entry(0x00, 0x01), entry(0x01, 0x00), entry(254, 0x02)}));
    CHECK(clash({entry(0x00, 0x01), entry(0x21, 0x22), entry(0x00, std::nullopt, 2)}) == (NodeOverrideClash{0, 2, false}));
    CHECK(clash({entry(0x21, 0x22), entry(std::nullopt, 0x01)}) == (NodeOverrideClash{0, 1, true}));
    CHECK(clash({entry(std::nullopt, 0x01), entry(0x21, 0x22)}) == (NodeOverrideClash{0, 1, true}));
    CHECK(clash({entry(std::nullopt, 0x01), entry(std::nullopt, 0x02)}) == (NodeOverrideClash{0, 1, true}));
    // An entry that changes nothing counts as well.
    CHECK(clash({entry(0x00, 0x01), entry(0x00, std::nullopt)}) == (NodeOverrideClash{0, 1, false}));
}

// --- A list of entries and the nodes of a network

TEST_CASE(node_overrides_say_what_each_entry_does_to_the_nodes)
{
    const std::vector<NodeValues> none;
    const std::vector<NodeValues> one{values(0x00, kOurName)};
    const std::vector<NodeValues> two{values(0x00, kOurName), values(0x21)};
    const std::vector<NodeValues> same{values(254, kAacName), values(254, kAacName)};
    const std::vector<NodeValues> mixed{values(0x00), values(0x21), values(0x21)};
    const auto run = [](const std::vector<NodeValues>& coded, std::vector<NodeOverride> entries) {
        return j::apply_node_overrides(coded, entries);
    };

    // Applied: the node and its values.
    {
        const auto out = run(two, {entry(0x21, 0x22), entry(0x00, 0x01, 2)});
        CHECK_EQ(out.size(), 2u);
        CHECK(out[0].match == NodeOverrideMatch::Applied);
        CHECK_EQ(out[0].node, 1u);
        CHECK(out[0].values == values(0x22));
        CHECK(out[1].match == NodeOverrideMatch::Applied);
        CHECK_EQ(out[1].node, 0u);
        CHECK(out[1].values == values(0x01, kOurNameSecond));
    }
    // Without from: the only node.
    {
        const auto out = run(one, {entry(std::nullopt, 0x01)});
        CHECK(out[0].match == NodeOverrideMatch::Applied);
        CHECK(out[0].values == values(0x01, kOurName));
    }
    // No node at all, with and without from.
    CHECK(run(none, {entry(std::nullopt, 0x01)})[0].match == NodeOverrideMatch::NoNodes);
    CHECK(run(none, {entry(0x00, 0x01)})[0].match == NodeOverrideMatch::NoNodes);
    // No node at from.
    CHECK(run(two, {entry(0x10, 0x01)})[0].match == NodeOverrideMatch::NoNodeAtFrom);
    // Without from and with nodes at different addresses: which one?
    {
        const auto out = run(two, {entry(std::nullopt, 0x01)});
        CHECK(out[0].match == NodeOverrideMatch::Ambiguous);
        CHECK_EQ(out[0].node, 0u);
    }
    CHECK(run(mixed, {entry(std::nullopt, 0x01)})[0].match == NodeOverrideMatch::Ambiguous);
    // Two nodes at the address the entry picks by: with from, and without when all are there.
    {
        const auto out = run(mixed, {entry(0x21, 0x22), entry(0x00, 0x01)});
        CHECK(out[0].match == NodeOverrideMatch::Indistinct);
        CHECK_EQ(out[0].node, 1u);
        CHECK(out[1].match == NodeOverrideMatch::Applied); // the other node is not touched by it
    }
    CHECK(run(same, {entry(254, 0x80)})[0].match == NodeOverrideMatch::Indistinct);
    CHECK(run(same, {entry(std::nullopt, 0x80)})[0].match == NodeOverrideMatch::Indistinct);
    // An entry that changes nothing is skipped, whatever it points at.
    CHECK(run(two, {entry(0x10, std::nullopt)})[0].match == NodeOverrideMatch::Skipped);
    CHECK(run(none, {entry(std::nullopt, std::nullopt)})[0].match == NodeOverrideMatch::Skipped);
    // Cannot be applied: why, and for which node.
    {
        const auto out = run(two, {entry(0x21, std::nullopt, 2), entry(0x00, 254)});
        CHECK(out[0].match == NodeOverrideMatch::CannotApply);
        CHECK(out[0].error == NodeOverrideError::NameFieldWithoutName);
        CHECK_EQ(out[0].node, 1u);
        CHECK(out[1].match == NodeOverrideMatch::CannotApply);
        CHECK(out[1].error == NodeOverrideError::AnyAddressNotCapable);
    }
}

TEST_CASE(node_overrides_are_matched_against_the_code_so_two_entries_can_swap_addresses)
{
    const std::vector<NodeValues> coded{values(0x00, kOurName), values(0x01, kAacName)};
    const std::vector<NodeOverride> swap{entry(0x00, 0x01), entry(0x01, 0x00)};
    const auto out = j::apply_node_overrides(coded, swap);
    CHECK(out[0].match == NodeOverrideMatch::Applied);
    CHECK_EQ(out[0].node, 0u);
    CHECK(out[0].values == values(0x01, kOurName));
    CHECK(out[1].match == NodeOverrideMatch::Applied);
    CHECK_EQ(out[1].node, 1u);
    CHECK(out[1].values == values(0x00, kAacName));
}

// --- As text

TEST_CASE(node_override_text_is_written_in_one_form_and_read_back)
{
    CHECK_TEXT(j::format_node_override(entry(0x00, 0x01, 2)), std::string("from:0x00,address:0x01,identity_number:2"));
    CHECK_TEXT(j::format_node_override(entry(0xAB, 0xFD, 2097151, 31, 7)),
             std::string("from:0xAB,address:0xFD,identity_number:2097151,function_instance:31,ecu_instance:7"));
    CHECK_TEXT(j::format_node_override(entry(std::nullopt, std::nullopt, std::nullopt, std::nullopt, 0)), std::string("ecu_instance:0"));
    // 254 is not an address of a node: written as the number that the messages call "any".
    CHECK_TEXT(j::format_node_override(entry(254, 254)), std::string("from:254,address:254"));
    CHECK_TEXT(j::format_node_override(entry(std::nullopt, std::nullopt)), std::string());
    CHECK_TEXT(j::coded_address_text(0x0A), std::string("0x0A"));
    CHECK_TEXT(j::coded_address_text(254), std::string("any (254)"));

    for (const NodeOverride& e :
         {entry(0x00, 0x01, 2), entry(0xAB, 0xFD, 2097151, 31, 7), entry(254, 254), entry(std::nullopt, 0x00),
          entry(0x7F, std::nullopt), entry(std::nullopt, std::nullopt, 0), entry(std::nullopt, std::nullopt, std::nullopt, 0, 0)})
    {
        const auto read = j::parse_node_override(j::format_node_override(e));
        CHECK(read.entry.has_value());
        CHECK(*read.entry == e);
    }
    // Any order, decimal or hexadecimal in either case.
    CHECK(*j::parse_node_override("identity_number:0x1fFFff,address:1,from:0XfE").entry == entry(254, 1, 2097151));
    CHECK(*j::parse_node_override("from:16").entry == entry(16, std::nullopt));
    CHECK(*j::parse_node_override("address:007").entry == entry(std::nullopt, 7));
}

TEST_CASE(node_override_text_that_is_wrong_says_which_setting)
{
    const auto read = [](std::string_view text) { return j::parse_node_override(text); };
    const auto is = [](const j::NodeOverrideText& out, NodeOverrideTextError error, std::string_view setting,
                       std::string_view value = {}, std::uint32_t max = 0) {
        return !out.entry && out.error == error && out.setting == setting && out.value == value && out.max == max;
    };
    CHECK(is(read(""), NodeOverrideTextError::EmptySetting, ""));
    CHECK(is(read("address:1,"), NodeOverrideTextError::EmptySetting, ""));
    CHECK(is(read("address:1,,from:0"), NodeOverrideTextError::EmptySetting, ""));
    CHECK(is(read("address"), NodeOverrideTextError::NoValue, "address"));
    CHECK(is(read("adress:0x01"), NodeOverrideTextError::UnknownSetting, "adress"));
    CHECK(is(read("adress"), NodeOverrideTextError::UnknownSetting, "adress"));
    CHECK(is(read("Address:1"), NodeOverrideTextError::UnknownSetting, "Address"));
    CHECK(is(read(" address:1"), NodeOverrideTextError::UnknownSetting, " address"));
    CHECK(is(read("address:0x01,address:0x02"), NodeOverrideTextError::Twice, "address"));
    for (const char* text : {"x1", "", "0x", "-1", "+1", "1 ", " 1", "0x1G", "1.0", "12a"})
    {
        CHECK(is(read(std::string("address:") + text), NodeOverrideTextError::NotANumber, "address", text));
    }
    CHECK(is(read("from:255"), NodeOverrideTextError::OutOfRange, "from", "255", 254));
    CHECK(is(read("address:0xFF"), NodeOverrideTextError::OutOfRange, "address", "0xFF", 254));
    CHECK(is(read("identity_number:2097152"), NodeOverrideTextError::OutOfRange, "identity_number", "2097152", 2097151));
    CHECK(is(read("function_instance:32"), NodeOverrideTextError::OutOfRange, "function_instance", "32", 31));
    CHECK(is(read("ecu_instance:8"), NodeOverrideTextError::OutOfRange, "ecu_instance", "8", 7));
    // Far above 32 and 64 bits: out of range, not some small number after a wrap.
    CHECK(is(read("ecu_instance:4294967296"), NodeOverrideTextError::OutOfRange, "ecu_instance", "4294967296", 7));
    CHECK(is(read("address:0x10000000000000001"), NodeOverrideTextError::OutOfRange, "address", "0x10000000000000001", 254));
    CHECK(is(read("address:99999999999999999999999999"), NodeOverrideTextError::OutOfRange, "address",
             "99999999999999999999999999", 254));
}

// --- A Network with overrides

TEST_CASE(node_with_an_override_claims_the_new_address_with_the_new_name)
{
    Bench bench(with({entry(0x00, 0x01, 2)}));
    Node& us = bench.network.add_node(NodeOptions{0x00, Name{kOurName}});
    CHECK_EQ(us.address(), 0x01);
    CHECK_EQ(us.requested_address(), 0x01);
    CHECK_EQ(us.coded_address(), 0x00);
    CHECK(us.name() == std::optional<Name>(Name{kOurNameSecond}));
    CHECK(bench.network.unused_node_overrides().empty());
    bench.step(); // 0
    CHECK_J1939(bench.take_sent(), {claim(0x01, kOurNameSecond)});
    bench.steps(30);
    CHECK_J1939(bench.take_sent(), {});
    CHECK(us.address_state() == AddressState::Claimed);
    CHECK_EQ(us.address(), 0x01);
}

TEST_CASE(node_override_without_from_is_for_the_only_node_and_only_a_name_field_may_change)
{
    Bench bench(with({entry(std::nullopt, std::nullopt, std::nullopt, 1, 2)}));
    Node& us = bench.network.add_node(NodeOptions{0x30, Name{kOurName}});
    const std::uint64_t name = kOurName | (std::uint64_t{1} << 35) | (std::uint64_t{2} << 32);
    CHECK_EQ(us.requested_address(), 0x30);
    CHECK_EQ(us.coded_address(), 0x30);
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(0x30, name)});
}

TEST_CASE(node_overrides_go_to_their_nodes_and_leave_the_others_as_the_code_says)
{
    // Three nodes; entries for the first and the third, in another order than the nodes.
    Bench bench(with({entry(0x22, 0x32), entry(0x20, 0x30, 7)}));
    Node& a = bench.network.add_node(NodeOptions{0x20, Name{kOurName}});
    Node& b = bench.network.add_node(NodeOptions{0x21, Name{kOurName + 1}});
    Node& c = bench.network.add_node(NodeOptions{0x22, std::nullopt});
    CHECK_EQ(a.address(), 0x30);
    CHECK_EQ(a.name()->identity_number(), 7u);
    CHECK_EQ(b.address(), 0x21);
    CHECK_EQ(b.requested_address(), 0x21);
    CHECK_EQ(b.coded_address(), 0x21);
    CHECK(b.name() == std::optional<Name>(Name{kOurName + 1}));
    CHECK_EQ(c.address(), 0x32);
    CHECK_EQ(c.coded_address(), 0x22);
    CHECK(!c.name());
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(0x30, (kOurName & ~std::uint64_t{0x1FFFFF}) | 7), claim(0x21, kOurName + 1)});
}

TEST_CASE(two_node_overrides_swap_the_addresses_of_two_nodes)
{
    Bench bench(with({entry(0x00, 0x01), entry(0x01, 0x00)}));
    Node& a = bench.network.add_node(NodeOptions{0x00, Name{kOurName}});
    Node& b = bench.network.add_node(NodeOptions{0x01, Name{kOurName + 1}});
    CHECK_EQ(a.address(), 0x01);
    CHECK_EQ(a.coded_address(), 0x00);
    CHECK_EQ(b.address(), 0x00);
    CHECK_EQ(b.coded_address(), 0x01);
    CHECK(bench.network.unused_node_overrides().empty());
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(0x01, kOurName), claim(0x00, kOurName + 1)});
}

TEST_CASE(node_override_to_any_address_makes_a_capable_node_claim_a_free_one)
{
    Bench bench(with({entry(0x00, 254)}));
    Node& us = bench.network.add_node(NodeOptions{0x00, Name{kAacName}});
    CHECK_EQ(us.requested_address(), 254);
    bench.step();
    CHECK_J1939(bench.take_sent(), {claim(128, kAacName)}); // the first of the arbitrary address range
    // ... and from: 254 picks the node whose code asks for any address.
    Bench fixed(with({entry(254, 0x05)}));
    Node& other = fixed.network.add_node(NodeOptions{kNull, Name{kAacName}});
    CHECK_EQ(other.coded_address(), 254);
    fixed.step();
    CHECK_J1939(fixed.take_sent(), {claim(0x05, kAacName)});
}

TEST_CASE(node_override_lists_that_cannot_stand_are_refused_by_the_constructor)
{
    CHECK_TEXT(refusal([] { Network network(with({entry(0x00, 0x01), entry(0x00, std::nullopt, 2)})); }),
             kWho + "node_overrides has two entries for the node at 0x00 in the code (from:0x00,address:0x01 and "
                    "from:0x00,identity_number:2): write its settings in one entry");
    const std::string withoutFrom = kWho + "node_overrides has an entry without from (address:0x01), which stands for the "
                                           "only node, and another entry (from:0x21,address:0x22): give every entry a from";
    CHECK_TEXT(refusal([] { Network network(with({entry(std::nullopt, 0x01), entry(0x21, 0x22)})); }), withoutFrom);
    CHECK_TEXT(refusal([] { Network network(with({entry(0x21, 0x22), entry(std::nullopt, 0x01)})); }), withoutFrom);
    // A value out of its range can only come from a program that fills the struct itself.
    CHECK_TEXT(refusal([] { Network network(with({entry(0x00, std::nullopt, 2097152)})); }),
             kWho + "the node override sets identity_number 2097152, which is out of its range 0..2097151");
    CHECK_TEXT(refusal([] { Network network(with({entry(0x00, 255)})); }),
             kWho + "the node override sets address 255, which is out of its range 0..254");
}

TEST_CASE(second_node_for_one_node_override_is_refused_and_nothing_is_added)
{
    // Two nodes at one address in the code: no from can tell them apart.
    {
        Network network(with({entry(0x00, 0x01)}));
        network.add_node(NodeOptions{0x00, Name{kOurName}});
        const std::size_t timers = network.timers_free();
        CHECK_TEXT(refusal([&] { network.add_node(NodeOptions{0x00, Name{kOurName + 1}}); }),
                 kWho + "the node override from:0x00,address:0x01 cannot pick a node: the code adds two nodes at 0x00, and "
                        "from tells nodes apart by that address; remove the override, and give the nodes addresses of "
                        "their own in the code");
        CHECK_EQ(network.node_count(), 1u);
        CHECK_EQ(network.timers_free(), timers);
        CHECK(network.unused_node_overrides().empty());
        // Another node is still added as before.
        CHECK_EQ(network.add_node(NodeOptions{0x02, Name{kOurName + 2}}).address(), 0x02);
    }
    // The same for two nodes whose code asks for any address.
    {
        Network network(with({entry(254, 0x90)}));
        network.add_node(NodeOptions{kNull, Name{kAacName}});
        CHECK_TEXT(refusal([&] { network.add_node(NodeOptions{kNull, Name{kAacName + 1}}); }),
                 kWho + "the node override from:254,address:0x90 cannot pick a node: the code adds two nodes at any (254), "
                        "and from tells nodes apart by that address; remove the override, and give the nodes addresses of "
                        "their own in the code");
    }
    // An entry without from and a second node at another address: a from would say which.
    {
        Network network(with({entry(std::nullopt, 0x01)}));
        network.add_node(NodeOptions{0x00, Name{kOurName}});
        const std::size_t timers = network.timers_free();
        CHECK_TEXT(refusal([&] { network.add_node(NodeOptions{0x21, Name{kOurName + 1}}); }),
                 kWho + "the node override address:0x01 has no from, so it is for the only node, and a second node was "
                        "added (at 0x21 in the code; the first is at 0x00): give it a from (with ecu_main: --j1939-node "
                        "<network>=from:<address in the code>,...)");
        CHECK_EQ(network.node_count(), 1u);
        CHECK_EQ(network.timers_free(), timers);
    }
    // ... and at the same address: nothing can.
    {
        Network network(with({entry(std::nullopt, 0x01)}));
        network.add_node(NodeOptions{0x00, Name{kOurName}});
        CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>(
            [&] { network.add_node(NodeOptions{0x00, std::nullopt}); }, "address:0x01 cannot pick a node: the code adds two nodes at 0x00"));
    }
}

TEST_CASE(node_override_that_cannot_be_applied_is_refused_and_stays_unused)
{
    {
        Network network(with({entry(0x00, std::nullopt, 2)}));
        const std::size_t timers = network.timers_free();
        CHECK_TEXT(refusal([&] { network.add_node(NodeOptions{0x00, std::nullopt}); }),
                 kWho + "the node override from:0x00,identity_number:2 sets a NAME field, but the node at 0x00 in the code "
                        "has no NAME (it does not claim its address): remove identity_number; for this node only address "
                        "can be changed");
        CHECK_EQ(network.node_count(), 0u);
        CHECK_EQ(network.timers_free(), timers);
        CHECK(network.unused_node_overrides() == (std::vector<std::size_t>{0}));
        // The entry is still there for a node it fits.
        CHECK_EQ(network.add_node(NodeOptions{0x00, Name{kOurName}}).name()->identity_number(), 2u);
        CHECK(network.unused_node_overrides().empty());
    }
    {
        Network network(with({entry(0x00, 0x01, std::nullopt, 1, 2)}));
        CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>([&] { network.add_node(NodeOptions{0x00, std::nullopt}); },
                                                                     ": remove function_instance, ecu_instance; for this node"));
    }
    {
        Network network(with({entry(0x00, 254)}));
        CHECK_TEXT(refusal([&] { network.add_node(NodeOptions{0x00, Name{kOurName}}); }),
                 kWho + "the node override from:0x00,address:254 asks for any free address (254), but the NAME of the node "
                        "at 0x00 in the code is not arbitrary address capable: give an address of 0..253");
        CHECK_TEXT(refusal([&] { network.add_node(NodeOptions{0x00, std::nullopt}); }),
                 kWho + "the node override from:0x00,address:254 asks for any free address (254), but the node at 0x00 in "
                        "the code has no NAME, so it cannot claim a free address: give an address of 0..253");
        CHECK_EQ(network.node_count(), 0u);
    }
    // What add_node() refuses for the code's values, it refuses for the override's: the timers of
    // a node that gets no NAME field are not reserved twice, and a full network takes none.
    {
        NetworkOptions options = with({entry(0x00, 0x01)});
        options.timer_capacity = 1;
        Network network(options);
        CHECK(throws<std::invalid_argument>([&] { network.add_node(NodeOptions{0x00, Name{kOurName}}); }));
        CHECK(network.unused_node_overrides() == (std::vector<std::size_t>{0}));
        CHECK_EQ(network.add_node(NodeOptions{0x00, std::nullopt}).address(), 0x01);
    }
}

TEST_CASE(node_overrides_are_closed_at_the_start_and_later_nodes_keep_their_code)
{
    // close_node_overrides(): the entries no node took, once.
    {
        Network network(with({entry(0x00, 0x01), entry(0x10, 0x11), entry(0x20, std::nullopt)}));
        network.add_node(NodeOptions{0x00, Name{kOurName}});
        CHECK(network.unused_node_overrides() == (std::vector<std::size_t>{1})); // not the one that changes nothing
        CHECK(network.close_node_overrides() == (std::vector<std::size_t>{1}));
        CHECK(network.close_node_overrides().empty());
        CHECK(network.unused_node_overrides() == (std::vector<std::size_t>{1}));
        // Closed: a node at the from of an unused entry, and a second one at the from of a used
        // one, are added with the code's values and without a refusal.
        Node& late = network.add_node(NodeOptions{0x10, Name{kOurName + 1}});
        CHECK_EQ(late.address(), 0x10);
        CHECK_EQ(late.requested_address(), 0x10);
        Node& again = network.add_node(NodeOptions{0x00, Name{kOurName + 2}});
        CHECK_EQ(again.address(), 0x00);
        CHECK(again.name() == std::optional<Name>(Name{kOurName + 2}));
        CHECK(network.unused_node_overrides() == (std::vector<std::size_t>{1}));
    }
    // An entry without from that a node took does not refuse a second node after the close.
    {
        Network network(with({entry(std::nullopt, 0x01)}));
        network.add_node(NodeOptions{0x00, Name{kOurName}});
        CHECK(network.close_node_overrides().empty());
        CHECK_EQ(network.add_node(NodeOptions{0x21, Name{kOurName + 1}}).address(), 0x21);
        CHECK_EQ(network.node(0).address(), 0x01);
    }
    // The first begin() closes them too: a node added in a step or after it keeps its code.
    {
        Bench bench(with({entry(std::nullopt, 0x01), }));
        bench.network.add_node(NodeOptions{0x00, Name{kOurName}});
        Node* inStep = nullptr;
        bench.app = [&](SimTime now) {
            if (now == at(0ms))
            {
                inStep = &bench.network.add_node(NodeOptions{0x21, Name{kOurName + 1}});
            }
        };
        bench.step(); // 0
        CHECK(inStep != nullptr);
        CHECK_EQ(inStep->address(), 0x21);
        Node& after = bench.network.add_node(NodeOptions{0x00, Name{kOurName + 2}});
        CHECK_EQ(after.address(), 0x00);
        CHECK(bench.network.close_node_overrides().empty()); // closed before
        bench.step(); // 10
        CHECK_J1939(bench.take_sent(), {claim(0x01, kOurName), claim(0x21, kOurName + 1), claim(0x00, kOurName + 2)});
    }
    // An unused entry does not catch a node added after the first step either.
    {
        Bench bench(with({entry(0x10, 0x11)}));
        bench.step();
        CHECK_EQ(bench.network.add_node(NodeOptions{0x10, std::nullopt}).address(), 0x10);
        CHECK(bench.network.unused_node_overrides() == (std::vector<std::size_t>{0}));
    }
}

// --- The same node code twice on one bus

namespace {

struct Twice
{
    AddressState first{};
    AddressState second{};
    Address first_address = kNull;
    Address second_address = kNull;
};

/// Two networks on one bus, each with the node that the same code adds (0x00, kOurName); the
/// second network with `entries`.
Twice same_code_twice(std::vector<NodeOverride> entries)
{
    can::VirtualCanBus bus;
    Network netA;
    Network netB(with(std::move(entries)));
    Node& a = netA.add_node(NodeOptions{0x00, Name{kOurName}});
    Node& b = netB.add_node(NodeOptions{0x00, Name{kOurName}});
    can::Host hostA;
    can::Host hostB;
    hostA.add(netA, bus.attach());
    hostB.add(netB, bus.attach());
    can::StepDriver driver;
    driver.add(hostA, 10ms);
    driver.add(hostB, 10ms);
    driver.run_until(at(1s));
    return Twice{a.address_state(), b.address_state(), a.address(), b.address()};
}

} // namespace

TEST_CASE(same_node_code_twice_on_one_bus_needs_an_override_to_hold_two_addresses)
{
    // As the code is: one address and one NAME for both, and neither can hold it.
    const Twice clash = same_code_twice({});
    CHECK(clash.first == AddressState::CannotClaim);
    CHECK(clash.second == AddressState::CannotClaim);
    // The second with another address and identity number: both hold theirs.
    const Twice apart = same_code_twice({entry(std::nullopt, 0x01, 2)});
    CHECK(apart.first == AddressState::Claimed);
    CHECK(apart.second == AddressState::Claimed);
    CHECK_EQ(apart.first_address, 0x00);
    CHECK_EQ(apart.second_address, 0x01);
}

// --- The values of the override stay

TEST_CASE(node_with_an_override_that_loses_its_address_moves_on_from_the_new_values)
{
    // The code says 0x00; the override 0x85 and identity number 2. Losing 0x85, the node takes the
    // lowest free address with the new NAME: neither 0x00 nor the old NAME comes back.
    Bench bench(with({entry(0x00, 0x85, 2)}));
    Node& us = bench.network.add_node(NodeOptions{0x00, Name{kAacName}});
    bench.step(); // 0: claim 0x85
    bench.inject(at(5ms), claim(0x85, kAacNameSecond - 1));
    bench.step(); // 10: lost
    CHECK_J1939(bench.take_sent(), {claim(0x85, kAacNameSecond), claim(128, kAacNameSecond)});
    CHECK_EQ(us.address(), 128);
    CHECK_EQ(us.requested_address(), 0x85);
    CHECK_EQ(us.coded_address(), 0x00);
    // A node that cannot move says Cannot Claim with the new NAME.
    Bench single(with({entry(0x00, 0x05, 2)}));
    Node& fixed = single.network.add_node(NodeOptions{0x00, Name{kOurName}});
    single.step();
    single.inject(at(5ms), claim(0x05, kOurNameSecond - 1));
    single.steps(30); // the Cannot Claim delay is at most 153 ms
    CHECK_J1939(single.take_sent(), {claim(0x05, kOurNameSecond), claim(kNull, kOurNameSecond)});
    CHECK(fixed.address_state() == AddressState::CannotClaim);
    CHECK_EQ(fixed.requested_address(), 0x05);
}

TEST_CASE(node_with_an_override_wins_and_answers_requests_with_the_new_values)
{
    Bench bench(with({entry(0x00, 0x01, 2)}));
    Node& us = bench.network.add_node(NodeOptions{0x00, Name{kOurName}});
    bench.step(); // 0
    // A claim for the address in the code is none of ours; one for the new address with a higher
    // NAME is answered with our claim.
    bench.inject(at(2ms), claim(0x00, kOurNameSecond - 1));
    bench.inject(at(5ms), claim(0x01, kOurNameSecond + 1));
    bench.step(); // 10
    CHECK_J1939(bench.take_sent(), {claim(0x01, kOurNameSecond), claim(0x01, kOurNameSecond)});
    // What an ECU does when it comes back to its bus: it asks everybody for their claims and
    // answers itself, from the address it holds.
    bench.app = [&](SimTime now) {
        if (now == at(20ms))
        {
            CHECK(us.request(Pgn{kPgnAddressClaim}) == SendStatus::Ok);
        }
    };
    bench.steps(2); // 20, 30
    CHECK_J1939(bench.take_sent(), {req(0x01, kGlobal, kPgnAddressClaim), claim(0x01, kOurNameSecond)});
    // A request from outside to the new address is answered, one to the address in the code is not.
    bench.app = nullptr;
    bench.inject(req(0x90, 0x00, kPgnAddressClaim));
    bench.inject(req(0x90, 0x01, kPgnAddressClaim));
    bench.steps(2);
    CHECK_J1939(bench.take_sent(), {claim(0x01, kOurNameSecond)});
}

TEST_CASE(commanded_address_reaches_a_node_by_the_name_of_its_override)
{
    Bench bench(with({entry(0x00, 0x85, 2)}));
    Node& us = bench.network.add_node(NodeOptions{0x00, Name{kAacName}});
    bench.steps(27); // .. 260: claimed
    CHECK(us.address_state() == AddressState::Claimed);
    bench.take_sent();
    const auto command = [](std::uint64_t name, std::uint8_t address) {
        std::array<std::uint8_t, 9> out{};
        for (std::size_t i = 0; i < 8; ++i)
        {
            out[i] = static_cast<std::uint8_t>(name >> (8 * i));
        }
        out[8] = address;
        return out;
    };
    const auto forCode = command(kAacName, 0x96);
    const auto forOverride = command(kAacNameSecond, 0x95);
    bench.app = [&](SimTime now) {
        if (now == at(270ms))
        {
            // For the NAME in the code: not this node.
            NetworkTestAccess::deliver_message(bench.network, Message{Pgn{kPgnCommandedAddress}, 6, 0x90, kGlobal, at(265ms), forCode});
            CHECK_EQ(us.address(), 0x85);
            NetworkTestAccess::deliver_message(bench.network,
                                               Message{Pgn{kPgnCommandedAddress}, 6, 0x90, kGlobal, at(266ms), forOverride});
            CHECK_EQ(us.address(), 0x95);
        }
    };
    bench.step(); // 270
    CHECK_J1939(bench.take_sent(), {claim(0x95, kAacNameSecond)});
    CHECK_EQ(us.requested_address(), 0x85);
}
