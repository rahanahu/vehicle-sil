// SPDX-License-Identifier: MIT
#include "check_text.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/eth_link/mac_address.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace vehicle_sil::eth_link;

} // namespace

TEST_CASE(mac_address_parses_colons_and_dashes_in_either_case)
{
    const auto colons = parse_mac_address("02:00:00:00:0a:FF");
    const auto dashes = parse_mac_address("02-00-00-00-0A-ff");
    CHECK(colons.has_value());
    CHECK(dashes.has_value());
    CHECK(*colons == *dashes);
    CHECK_EQ(colons->bytes[4], 0x0Au);
    CHECK_EQ(colons->bytes[5], 0xFFu);
    CHECK_TEXT(to_string(*colons), "02:00:00:00:0a:ff");
    CHECK_EQ(colons->value(), std::uint64_t{0x0200'0000'0AFFu});
}

TEST_CASE(mac_address_rejects_what_is_not_six_hex_bytes)
{
    for (const char* text : {"", "02:00:00:00:00", "02:00:00:00:00:01:02", "02:00:00-00:00:01", "02.00.00.00.00.01",
                             "0g:00:00:00:00:01", "2:00:00:00:00:001", "02:00:00:00:00:1 "})
    {
        CHECK(!parse_mac_address(text).has_value());
    }
}

TEST_CASE(mac_address_group_bit_marks_broadcast_and_multicast)
{
    CHECK(parse_mac_address("ff:ff:ff:ff:ff:ff")->is_group());
    CHECK(parse_mac_address("01:00:5e:00:00:01")->is_group());
    CHECK(!parse_mac_address("02:00:00:00:00:01")->is_group());
}

TEST_CASE(mac_address_destination_and_source_come_from_the_header)
{
    std::vector<std::uint8_t> frame = {0x02, 0, 0, 0, 0, 0x01, 0x02, 0, 0, 0, 0, 0x02, 0x88, 0xB5};
    CHECK_TEXT(to_string(*destination_of(frame)), "02:00:00:00:00:01");
    CHECK_TEXT(to_string(*source_of(frame)), "02:00:00:00:00:02");
    frame.pop_back(); // 13 bytes: no complete header
    CHECK(!destination_of(frame).has_value());
    CHECK(!source_of(frame).has_value());
}
