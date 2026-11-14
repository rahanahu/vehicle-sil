// SPDX-License-Identifier: MIT
//
// Which frames an Ethernet bridge lets through, in both directions (no SIL Kit).
#pragma once

#include "vehicle_sil/eth_link/mac_address.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vehicle_sil::eth_link {

/// The tag protocol identifiers that mark a VLAN tag by default: 0x8100 and 0x88A8. Values from
/// general references, not checked against the standard; FrameFilterOptions::vlan_tpids
/// replaces them.
inline constexpr std::uint16_t kDefaultVlanTpids[] = {0x8100, 0x88A8};
/// The longest frame passed by default, in bytes without FCS. A value from general references
/// (1500 bytes of payload after the header and two VLAN tags), not checked against the standard;
/// FrameFilterOptions::max_frame_bytes replaces it.
inline constexpr std::size_t kDefaultMaxFrameBytes = 1522;
/// How many VLAN tags ether_type_of() looks behind.
inline constexpr int kMaxVlanTags = 2;

/// The EtherType of `frame`: the two bytes after the source MAC, or, while those are one of
/// `vlan_tpids`, the two bytes after that tag (4 bytes), at most kMaxVlanTags times. nullopt if
/// the frame ends before. Values below 0x0600 (a length, not a type) are returned as they are.
[[nodiscard]] std::optional<std::uint16_t> ether_type_of(std::span<const std::uint8_t> frame,
                                                         std::span<const std::uint16_t> vlan_tpids) noexcept;

struct FrameFilterOptions
{
    /// Pass only these EtherTypes (behind VLAN tags). Empty: any EtherType.
    std::vector<std::uint16_t> ether_types;
    /// The MAC addresses of the SIL Kit side. Not empty: to the outside, pass only frames whose
    /// source is one of them; from the outside, only frames whose destination is one of them or
    /// a group address. Empty: any address. Individual addresses only.
    std::vector<MacAddress> silkit_macs;
    /// The values that mark a VLAN tag (see kDefaultVlanTpids).
    std::vector<std::uint16_t> vlan_tpids{std::begin(kDefaultVlanTpids), std::end(kDefaultVlanTpids)};
    /// Longer frames are dropped as Oversize. At least kEthernetHeaderBytes.
    std::size_t max_frame_bytes = kDefaultMaxFrameBytes;
};

enum class FrameVerdict
{
    Pass,
    Filtered,  ///< not one of the frames the options let through
    Oversize,  ///< longer than max_frame_bytes
    Malformed, ///< shorter than an Ethernet header (kEthernetHeaderBytes)
};

/// The rules of FrameFilterOptions. Immutable: both directions may be checked concurrently.
/// Checking allocates nothing.
class FrameFilter
{
public:
    /// Throws std::invalid_argument if max_frame_bytes is below kEthernetHeaderBytes or
    /// silkit_macs holds a group address.
    explicit FrameFilter(FrameFilterOptions options = {});

    /// A frame from SIL Kit, to go out.
    [[nodiscard]] FrameVerdict to_external(std::span<const std::uint8_t> frame) const noexcept;
    /// A frame from the outside, to go into SIL Kit.
    [[nodiscard]] FrameVerdict from_external(std::span<const std::uint8_t> frame) const noexcept;

    /// Neither EtherTypes nor MAC addresses restrict the frames (the length still does).
    [[nodiscard]] bool passes_every_frame() const noexcept;
    /// For the bridge's start line: "every frame, both ways", "EtherType 0x88b5,0x0800 only, both
    /// ways", "SIL Kit MAC 02:00:00:00:00:01 only, both ways", or both joined with "and".
    [[nodiscard]] std::string describe() const;
    [[nodiscard]] const FrameFilterOptions& options() const noexcept { return options_; }

private:
    [[nodiscard]] FrameVerdict check_length(std::span<const std::uint8_t> frame) const noexcept;
    [[nodiscard]] bool ether_type_passes(std::span<const std::uint8_t> frame) const noexcept;
    [[nodiscard]] bool is_silkit_mac(const MacAddress& address) const noexcept;

    FrameFilterOptions options_;
};

/// "0x88b5": four lower case hex digits.
[[nodiscard]] std::string format_ether_type(std::uint16_t ether_type);

} // namespace vehicle_sil::eth_link
