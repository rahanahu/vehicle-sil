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

TEST_CASE(mac_side_table_drops_the_bridges_own_frame_coming_back)
{
    MacSideTable table;
    CHECK(table.on_silkit_source(kEcu).outcome == FromSilKit::Forward);
    CHECK(table.side_of(kEcu) == Side::SilKit);
    CHECK(table.on_external_source(kEcu, true).outcome == FromExternal::OwnFrame);
    // Also when the returning frame would have been filtered.
    CHECK(table.on_external_source(kEcu, false).outcome == FromExternal::OwnFrame);
    CHECK(table.side_of(kEcu) == Side::SilKit); // a returning frame does not move the address
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
    CHECK(!table.side_of(broadcast).has_value());
    CHECK_EQ(table.size(), std::size_t{0});
}

TEST_CASE(mac_side_table_when_full_forwards_new_addresses_and_reports_once)
{
    MacSideTable table(3);
    for (std::uint32_t n = 1; n <= 3; ++n)
    {
        const auto r = table.on_silkit_source(numbered(n));
        CHECK(r.outcome == FromSilKit::Forward);
        CHECK(!r.table_full);
    }
    const auto first = table.on_external_source(numbered(4), true);
    CHECK(first.outcome == FromExternal::Forward);
    CHECK(first.table_full);
    const auto second = table.on_silkit_source(numbered(5));
    CHECK(second.outcome == FromSilKit::Forward);
    CHECK(!second.table_full); // reported once
    CHECK(!table.side_of(numbered(4)).has_value());
    CHECK_EQ(table.size(), std::size_t{3});
    // The learned addresses keep working.
    CHECK(table.on_external_source(numbered(2), true).outcome == FromExternal::OwnFrame);
}

TEST_CASE(mac_side_table_keeps_many_addresses_apart)
{
    MacSideTable table(MacSideTable::kDefaultCapacity);
    for (std::uint32_t n = 0; n < MacSideTable::kDefaultCapacity; ++n)
    {
        const auto r = (n % 2 == 0) ? table.on_silkit_source(numbered(n)).table_full
                                    : table.on_external_source(numbered(n), true).table_full;
        CHECK(!r);
    }
    CHECK_EQ(table.size(), MacSideTable::kDefaultCapacity);
    for (std::uint32_t n = 0; n < MacSideTable::kDefaultCapacity; ++n)
    {
        CHECK(table.side_of(numbered(n)) == ((n % 2 == 0) ? Side::SilKit : Side::External));
    }
    // Addresses that differ only in the high bytes.
    MacSideTable high(4);
    CHECK(high.on_silkit_source(mac("02:00:00:00:00:01")).outcome == FromSilKit::Forward);
    CHECK(high.on_external_source(mac("06:00:00:00:00:01"), true).outcome == FromExternal::Forward);
    CHECK(high.side_of(mac("06:00:00:00:00:01")) == Side::External);
}

TEST_CASE(mac_side_table_rejects_capacity_zero)
{
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { (void)MacSideTable(0); }));
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
    CHECK(table.on_external_source(numbered(7), true).outcome == FromExternal::OwnFrame);
    CHECK(table.on_silkit_source(numbered(kAddresses + 7)).outcome == FromSilKit::NewConflict);
}
