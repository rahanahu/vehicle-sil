// SPDX-License-Identifier: MIT
#include "check_text.hpp"
#include "fake_link.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/eth_link/frame_filter.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

using namespace vehicle_sil::eth_link;
using vehicle_sil::fake_eth::Bytes;
using vehicle_sil::fake_eth::make_frame;
using vehicle_sil::fake_eth::make_tagged_frame;

// Locally administered addresses; 0x88B5 is an EtherType known to be free for local experiments
// (not checked against the standard).
constexpr std::uint16_t kExperimental = 0x88B5;
constexpr std::uint16_t kOther = 0x0800;
const char* const kEcu = "02:00:00:00:00:01";
const char* const kEcu2 = "02:00:00:00:00:02";
const char* const kOutside = "02:00:00:00:00:99";
const char* const kBroadcast = "ff:ff:ff:ff:ff:ff";

MacAddress mac(const char* text)
{
    return *parse_mac_address(text);
}

FrameFilter ether_types(std::vector<std::uint16_t> types)
{
    FrameFilterOptions options;
    options.ether_types = std::move(types);
    return FrameFilter(options);
}

FrameFilter silkit_macs(std::vector<MacAddress> macs)
{
    FrameFilterOptions options;
    options.silkit_macs = std::move(macs);
    return FrameFilter(options);
}

/// `frame` with another VLAN tag inserted before the EtherType field.
Bytes add_tag(Bytes frame, std::uint16_t tpid, std::uint16_t vlan_id)
{
    const std::uint8_t tag[] = {static_cast<std::uint8_t>(tpid >> 8), static_cast<std::uint8_t>(tpid),
                                static_cast<std::uint8_t>(vlan_id >> 8), static_cast<std::uint8_t>(vlan_id)};
    frame.insert(frame.begin() + 12, std::begin(tag), std::end(tag));
    return frame;
}

} // namespace

TEST_CASE(ether_type_of_looks_behind_up_to_two_vlan_tags)
{
    const std::uint16_t defaults[] = {0x8100, 0x88A8};
    const auto plain = make_frame(kOutside, kEcu, kExperimental);
    const auto single = make_tagged_frame(kOutside, kEcu, 0x8100, 5, kExperimental);
    const auto dual = add_tag(make_tagged_frame(kOutside, kEcu, 0x8100, 5, kExperimental), 0x88A8, 7);
    const auto triple = add_tag(dual, 0x8100, 9);
    CHECK_EQ(*ether_type_of(plain, defaults), kExperimental);
    CHECK_EQ(*ether_type_of(single, defaults), kExperimental);
    CHECK_EQ(*ether_type_of(dual, defaults), kExperimental);
    CHECK_EQ(*ether_type_of(triple, defaults), std::uint16_t{0x8100}); // a third tag is not looked behind
}

TEST_CASE(ether_type_of_uses_the_given_tag_identifiers_only)
{
    const auto tagged = make_tagged_frame(kOutside, kEcu, 0x9100, 5, kExperimental);
    const std::uint16_t defaults[] = {0x8100, 0x88A8};
    const std::uint16_t custom[] = {0x9100};
    CHECK_EQ(*ether_type_of(tagged, defaults), std::uint16_t{0x9100});
    CHECK_EQ(*ether_type_of(tagged, custom), kExperimental);
    CHECK_EQ(*ether_type_of(tagged, std::span<const std::uint16_t>{}), std::uint16_t{0x9100});
}

TEST_CASE(ether_type_of_a_frame_that_ends_inside_a_tag_is_unknown)
{
    const std::uint16_t defaults[] = {0x8100};
    auto tagged = make_tagged_frame(kOutside, kEcu, 0x8100, 5, kExperimental, 0);
    tagged.resize(17); // the tag but not the EtherType behind it
    CHECK(!ether_type_of(tagged, defaults).has_value());
    tagged.resize(13);
    CHECK(!ether_type_of(tagged, defaults).has_value());
}

TEST_CASE(frame_filter_passes_only_the_listed_ether_types_both_ways)
{
    const auto filter = ether_types({kExperimental, 0x0806});
    CHECK(filter.to_external(make_frame(kOutside, kEcu, kExperimental)) == FrameVerdict::Pass);
    CHECK(filter.from_external(make_frame(kEcu, kOutside, 0x0806)) == FrameVerdict::Pass);
    CHECK(filter.to_external(make_frame(kOutside, kEcu, kOther)) == FrameVerdict::Filtered);
    CHECK(filter.from_external(make_frame(kEcu, kOutside, kOther)) == FrameVerdict::Filtered);
    // behind VLAN tags
    CHECK(filter.to_external(make_tagged_frame(kOutside, kEcu, 0x8100, 5, kExperimental)) == FrameVerdict::Pass);
    CHECK(filter.from_external(make_tagged_frame(kEcu, kOutside, 0x88A8, 5, kOther)) == FrameVerdict::Filtered);
}

TEST_CASE(frame_filter_vlan_tag_identifiers_can_be_replaced)
{
    FrameFilterOptions options;
    options.ether_types = {kExperimental};
    options.vlan_tpids = {0x9100};
    const FrameFilter filter(options);
    CHECK(filter.from_external(make_tagged_frame(kEcu, kOutside, 0x9100, 5, kExperimental)) == FrameVerdict::Pass);
    // 0x8100 no longer marks a tag: the EtherType is 0x8100.
    CHECK(filter.from_external(make_tagged_frame(kEcu, kOutside, 0x8100, 5, kExperimental)) == FrameVerdict::Filtered);
}

TEST_CASE(frame_filter_silkit_macs_are_the_source_out_and_the_destination_in)
{
    const auto filter = silkit_macs({mac(kEcu), mac(kEcu2)});
    // To the outside: the source must be one of the SIL Kit side's addresses.
    CHECK(filter.to_external(make_frame(kOutside, kEcu, kOther)) == FrameVerdict::Pass);
    CHECK(filter.to_external(make_frame(kOutside, kEcu2, kOther)) == FrameVerdict::Pass);
    CHECK(filter.to_external(make_frame(kEcu, kOutside, kOther)) == FrameVerdict::Filtered);
    // From the outside: the destination must be one of them, or a group address.
    CHECK(filter.from_external(make_frame(kEcu2, kOutside, kOther)) == FrameVerdict::Pass);
    CHECK(filter.from_external(make_frame(kBroadcast, kOutside, kOther)) == FrameVerdict::Pass);
    CHECK(filter.from_external(make_frame("01:00:5e:00:00:01", kOutside, kOther)) == FrameVerdict::Pass);
    CHECK(filter.from_external(make_frame("02:00:00:00:00:03", kOutside, kOther)) == FrameVerdict::Filtered);
    CHECK(filter.from_external(make_frame(kOutside, kEcu, kOther)) == FrameVerdict::Filtered);
}

TEST_CASE(frame_filter_ether_types_and_macs_must_both_pass)
{
    FrameFilterOptions options;
    options.ether_types = {kExperimental};
    options.silkit_macs = {mac(kEcu)};
    const FrameFilter filter(options);
    CHECK(filter.to_external(make_frame(kOutside, kEcu, kExperimental)) == FrameVerdict::Pass);
    CHECK(filter.to_external(make_frame(kOutside, kEcu, kOther)) == FrameVerdict::Filtered);
    CHECK(filter.to_external(make_frame(kOutside, kEcu2, kExperimental)) == FrameVerdict::Filtered);
    CHECK(filter.from_external(make_frame(kEcu, kOutside, kExperimental)) == FrameVerdict::Pass);
    CHECK(filter.from_external(make_frame(kEcu2, kOutside, kExperimental)) == FrameVerdict::Filtered);
}

TEST_CASE(frame_filter_without_lists_passes_every_frame)
{
    const FrameFilter filter;
    CHECK(filter.passes_every_frame());
    CHECK(filter.to_external(make_frame(kOutside, kEcu, kOther)) == FrameVerdict::Pass);
    CHECK(filter.from_external(make_frame("02:00:00:00:00:03", kOutside, 0x1234)) == FrameVerdict::Pass);
    CHECK(!ether_types({kExperimental}).passes_every_frame());
    CHECK(!silkit_macs({mac(kEcu)}).passes_every_frame());
}

TEST_CASE(frame_filter_drops_frames_longer_than_the_maximum_as_oversize)
{
    const FrameFilter defaults;
    // 14 bytes of header: 1508 bytes of payload make the default maximum, 1522.
    CHECK(defaults.to_external(make_frame(kOutside, kEcu, kOther, 1508)) == FrameVerdict::Pass);
    CHECK(defaults.to_external(make_frame(kOutside, kEcu, kOther, 1509)) == FrameVerdict::Oversize);
    CHECK(defaults.from_external(make_frame(kEcu, kOutside, kOther, 1509)) == FrameVerdict::Oversize);

    FrameFilterOptions options;
    options.max_frame_bytes = 100;
    options.ether_types = {kExperimental};
    const FrameFilter small(options);
    CHECK(small.from_external(make_frame(kEcu, kOutside, kExperimental, 86)) == FrameVerdict::Pass);
    // Oversize before Filtered: the length is checked first.
    CHECK(small.from_external(make_frame(kEcu, kOutside, kOther, 87)) == FrameVerdict::Oversize);
}

TEST_CASE(frame_filter_drops_frames_shorter_than_a_header_as_malformed)
{
    const FrameFilter filter;
    auto frame = make_frame(kOutside, kEcu, kOther, 0);
    CHECK(filter.to_external(frame) == FrameVerdict::Pass);
    frame.pop_back();
    CHECK(filter.to_external(frame) == FrameVerdict::Malformed);
    CHECK(filter.from_external(frame) == FrameVerdict::Malformed);
    CHECK(silkit_macs({mac(kEcu)}).from_external(Bytes{}) == FrameVerdict::Malformed);
}

TEST_CASE(frame_filter_describes_what_passes)
{
    CHECK_TEXT(FrameFilter().describe(), "every frame, both ways");
    CHECK_TEXT(ether_types({kExperimental, 0x0806}).describe(), "EtherType 0x88b5,0x0806 only, both ways");
    CHECK_TEXT(silkit_macs({mac(kEcu), mac(kEcu2)}).describe(),
               "SIL Kit MAC 02:00:00:00:00:01,02:00:00:00:00:02 only, both ways");
    FrameFilterOptions options;
    options.ether_types = {kExperimental};
    options.silkit_macs = {mac(kEcu)};
    CHECK_TEXT(FrameFilter(options).describe(), "EtherType 0x88b5 and SIL Kit MAC 02:00:00:00:00:01 only, both ways");
    CHECK_TEXT(format_ether_type(0x0800), "0x0800");
}

TEST_CASE(frame_filter_rejects_options_it_cannot_apply)
{
    FrameFilterOptions shortMax;
    shortMax.max_frame_bytes = 13;
    CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>([&] { (void)FrameFilter{shortMax}; }, "at least the Ethernet header"));
    FrameFilterOptions group;
    group.silkit_macs = {mac(kBroadcast)};
    CHECK(vehicle_sil::unit::throws_with<std::invalid_argument>([&] { (void)FrameFilter{group}; },
                                                               "ff:ff:ff:ff:ff:ff is a group address"));
}
