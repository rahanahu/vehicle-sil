// SPDX-License-Identifier: MIT
#include "test_harness.hpp"

#include "vehicle_sil/eth_link/mac_side_table.hpp"

#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {

using namespace vehicle_sil::eth_link;
using FromSilKit = MacSideTable::FromSilKit;
using FromExternal = MacSideTable::FromExternal;
using Side = MacSideTable::Side;

MacAddress mac(const char* text)
{
    return *parse_mac_address(text);
}

/// A locally administered individual address: 02:00:00:nn:nn:nn.
MacAddress numbered(std::uint32_t n)
{
    MacAddress address;
    address.bytes = {0x02, 0x00, 0x00, static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8),
                     static_cast<std::uint8_t>(n)};
    return address;
}

const MacAddress kEcu = mac("02:00:00:00:00:01");
const MacAddress kOutside = mac("02:00:00:00:00:99");

} // namespace

TEST_CASE(mac_side_table_reports_a_sil_kit_address_coming_from_outside)
{
    MacSideTable table;
    CHECK(table.on_silkit_source(kEcu).outcome == FromSilKit::Forward);
    CHECK(table.side_of(kEcu) == Side::SilKit);
    CHECK(table.on_external_source(kEcu, true).outcome == FromExternal::SilKitSide);
    // Also when the frame would have been filtered.
    CHECK(table.on_external_source(kEcu, false).outcome == FromExternal::SilKitSide);
    CHECK(table.side_of(kEcu) == Side::SilKit); // the caller decides what it is
    // The caller found it is not the bridge's own frame: the address is on both sides.
    CHECK(table.mark_both_sides(kEcu));
    CHECK(!table.mark_both_sides(kEcu)); // once
    CHECK(table.on_external_source(kEcu, true).outcome == FromExternal::Blocked);
    CHECK(table.on_silkit_source(kEcu).outcome == FromSilKit::Blocked);
}

TEST_CASE(mac_side_table_learns_the_outside_from_frames_that_pass)
{
    MacSideTable table;
    CHECK(table.on_external_source(kOutside, false).outcome == FromExternal::Forward);
    CHECK(!table.side_of(kOutside).has_value()); // filtered: not learned
    CHECK(table.on_external_source(kOutside, true).outcome == FromExternal::Forward);
    CHECK(table.side_of(kOutside) == Side::External);
    CHECK(table.on_external_source(kOutside, true).outcome == FromExternal::Forward);
    CHECK_EQ(table.size(), std::size_t{1});
}

TEST_CASE(mac_side_table_an_address_on_both_sides_is_blocked_both_ways)
{
    MacSideTable table;
    CHECK(table.on_external_source(kOutside, true).outcome == FromExternal::Forward);
    // The same address now comes from SIL Kit: reported once, then blocked in both directions.
    CHECK(table.on_silkit_source(kOutside).outcome == FromSilKit::NewConflict);
    CHECK(table.side_of(kOutside) == Side::Both);
    CHECK(table.on_silkit_source(kOutside).outcome == FromSilKit::Blocked);
    CHECK(table.on_external_source(kOutside, true).outcome == FromExternal::Blocked);
    CHECK(table.on_external_source(kOutside, false).outcome == FromExternal::Blocked);
    CHECK(!table.mark_both_sides(kOutside));
    // Other addresses are not affected.
    CHECK(table.on_silkit_source(kEcu).outcome == FromSilKit::Forward);
}

TEST_CASE(mac_side_table_never_learns_group_addresses)
{
    MacSideTable table;
    const auto broadcast = mac("ff:ff:ff:ff:ff:ff");
    CHECK(table.on_silkit_source(broadcast).outcome == FromSilKit::Forward);
    CHECK(table.on_external_source(broadcast, true).outcome == FromExternal::Forward);
    CHECK(table.on_silkit_source(broadcast).outcome == FromSilKit::Forward);
    CHECK(!table.add_silkit_address(broadcast));
    CHECK(!table.side_of(broadcast).has_value());
    CHECK_EQ(table.size(), std::size_t{0});
}

TEST_CASE(mac_side_table_named_addresses_are_on_the_sil_kit_side_from_the_start)
{
    MacSideTable table;
    CHECK(table.add_silkit_address(kEcu));
    CHECK(table.on_external_source(kEcu, true).outcome == FromExternal::SilKitSide);
    CHECK(table.on_external_source(kOutside, true).outcome == FromExternal::Forward);
    CHECK(table.add_silkit_address(kOutside)); // already outside: now on both sides
    CHECK(table.side_of(kOutside) == Side::Both);
}

TEST_CASE(mac_side_table_when_full_the_sil_kit_side_takes_the_oldest_outside_place)
{
    MacSideTable table(4);
    CHECK(table.on_silkit_source(numbered(1)).outcome == FromSilKit::Forward);
    for (std::uint32_t n = 2; n <= 4; ++n) // outside: 2 is the oldest
    {
        const auto r = table.on_external_source(numbered(n), true);
        CHECK(!r.not_learned);
        CHECK(!r.table_full);
    }
    // A new outside address does not fit: reported, the first time also as table_full.
    const auto first = table.on_external_source(numbered(5), true);
    CHECK(first.outcome == FromExternal::Forward);
    CHECK(first.not_learned);
    CHECK(first.table_full);
    const auto again = table.on_external_source(numbered(5), true);
    CHECK(again.not_learned);
    CHECK(!again.table_full); // reported once
    CHECK(!table.on_external_source(numbered(6), false).not_learned); // a filtered frame learns nothing
    // A new SIL Kit address takes the place of the oldest outside one.
    const auto silkit = table.on_silkit_source(numbered(7));
    CHECK(!silkit.not_learned);
    CHECK(!table.side_of(numbered(2)).has_value());
    CHECK(table.side_of(numbered(7)) == Side::SilKit);
    CHECK(table.side_of(numbered(3)) == Side::External);
    CHECK(table.side_of(numbered(4)) == Side::External);
    CHECK_EQ(table.size(), std::size_t{4});
    // Named addresses and marks take places too.
    CHECK(table.add_silkit_address(numbered(8)));
    CHECK(!table.side_of(numbered(3)).has_value());
    CHECK(table.mark_both_sides(numbered(9)));
    CHECK(table.side_of(numbered(9)) == Side::Both);
    // Now only SIL Kit side addresses: a new one does not fit either.
    const auto none = table.on_silkit_source(numbered(10));
    CHECK(none.outcome == FromSilKit::Forward);
    CHECK(none.not_learned);
    CHECK(table.on_external_source(numbered(1), true).outcome == FromExternal::SilKitSide); // kept
}

TEST_CASE(mac_side_table_keeps_many_addresses_apart)
{
    MacSideTable table(MacSideTable::kDefaultCapacity);
    for (std::uint32_t n = 0; n < MacSideTable::kDefaultCapacity; ++n)
    {
        const auto r = (n % 2 == 0) ? table.on_silkit_source(numbered(n)).not_learned
                                    : table.on_external_source(numbered(n), true).not_learned;
        CHECK(!r);
    }
    CHECK_EQ(table.size(), MacSideTable::kDefaultCapacity);
    for (std::uint32_t n = 0; n < MacSideTable::kDefaultCapacity; ++n)
    {
        CHECK(table.side_of(numbered(n)) == ((n % 2 == 0) ? Side::SilKit : Side::External));
    }
    // Replacing every outside address by a SIL Kit one keeps the others findable (moving entries
    // back after a removal).
    for (std::uint32_t n = 0; n < MacSideTable::kDefaultCapacity / 2; ++n)
    {
        CHECK(!table.on_silkit_source(numbered(100'000 + n)).not_learned);
    }
    for (std::uint32_t n = 0; n < MacSideTable::kDefaultCapacity; ++n)
    {
        if (n % 2 == 0)
        {
            CHECK(table.side_of(numbered(n)) == Side::SilKit);
        }
        else
        {
            CHECK(!table.side_of(numbered(n)).has_value());
        }
        if (n < MacSideTable::kDefaultCapacity / 2)
        {
            CHECK(table.side_of(numbered(100'000 + n)) == Side::SilKit);
        }
    }
    // Addresses that differ only in the high bytes.
    MacSideTable high(4);
    CHECK(high.on_silkit_source(mac("02:00:00:00:00:01")).outcome == FromSilKit::Forward);
    CHECK(high.on_external_source(mac("06:00:00:00:00:01"), true).outcome == FromExternal::Forward);
    CHECK(high.side_of(mac("06:00:00:00:00:01")) == Side::External);
}

TEST_CASE(mac_side_table_rejects_capacities_out_of_range)
{
    CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>([] { (void)MacSideTable(0); }, "between 1 and 65536, got 0"));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { (void)MacSideTable(MacSideTable::kMaxCapacity + 1); }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { (void)MacSideTable(SIZE_MAX); }));
    CHECK_EQ(MacSideTable(MacSideTable::kMaxCapacity).capacity(), MacSideTable::kMaxCapacity);
}

TEST_CASE(mac_side_table_both_directions_at_once)
{
    // Like a bridge: the SIL Kit thread and the receive thread use the table at the same time.
    MacSideTable table(4096);
    constexpr std::uint32_t kAddresses = 2000;
    std::thread silkit([&] {
        for (std::uint32_t n = 0; n < kAddresses; ++n)
        {
            (void)table.on_silkit_source(numbered(n));
        }
    });
    std::thread external([&] {
        for (std::uint32_t n = kAddresses; n < 2 * kAddresses; ++n)
        {
            (void)table.on_external_source(numbered(n), true);
        }
    });
    silkit.join();
    external.join();
    CHECK_EQ(table.size(), std::size_t{2 * kAddresses});
    CHECK(table.on_external_source(numbered(7), true).outcome == FromExternal::SilKitSide);
    CHECK(table.on_silkit_source(numbered(kAddresses + 7)).outcome == FromSilKit::NewConflict);
}
