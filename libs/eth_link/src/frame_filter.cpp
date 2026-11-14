// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/frame_filter.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::eth_link {
namespace {

constexpr std::size_t kEtherTypeOffset = 12; // after the destination and the source MAC
constexpr std::size_t kVlanTagBytes = 4;     // TPID and tag control information

std::uint16_t read_be16(std::span<const std::uint8_t> frame, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>((frame[offset] << 8) | frame[offset + 1]);
}

} // namespace

std::optional<std::uint16_t> ether_type_of(std::span<const std::uint8_t> frame,
                                           std::span<const std::uint16_t> vlan_tpids) noexcept
{
    std::size_t offset = kEtherTypeOffset;
    if (frame.size() < offset + 2)
    {
        return std::nullopt;
    }
    std::uint16_t type = read_be16(frame, offset);
    for (int tags = 0; tags < kMaxVlanTags && std::find(vlan_tpids.begin(), vlan_tpids.end(), type) != vlan_tpids.end(); ++tags)
    {
        offset += kVlanTagBytes;
        if (frame.size() < offset + 2)
        {
            return std::nullopt;
        }
        type = read_be16(frame, offset);
    }
    return type;
}

std::string format_ether_type(std::uint16_t ether_type)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = 12; shift >= 0; shift -= 4)
    {
        text.push_back(kDigits[(ether_type >> shift) & 0x0F]);
    }
    return text;
}

FrameFilter::FrameFilter(FrameFilterOptions options) :
  options_(std::move(options))
{
    if (options_.max_frame_bytes < kEthernetHeaderBytes)
    {
        throw std::invalid_argument("FrameFilter: max_frame_bytes must be at least the Ethernet header (14 bytes)");
    }
    for (const auto& mac : options_.silkit_macs)
    {
        if (mac.is_group())
        {
            throw std::invalid_argument("FrameFilter: " + to_string(mac) +
                                        " is a group address; a SIL Kit side MAC must be an individual address");
        }
    }
}

FrameVerdict FrameFilter::check_length(std::span<const std::uint8_t> frame) const noexcept
{
    if (frame.size() < kEthernetHeaderBytes)
    {
        return FrameVerdict::Malformed;
    }
    if (frame.size() > options_.max_frame_bytes)
    {
        return FrameVerdict::Oversize;
    }
    return FrameVerdict::Pass;
}

bool FrameFilter::ether_type_passes(std::span<const std::uint8_t> frame) const noexcept
{
    if (options_.ether_types.empty())
    {
        return true;
    }
    const auto type = ether_type_of(frame, options_.vlan_tpids);
    return type && std::find(options_.ether_types.begin(), options_.ether_types.end(), *type) != options_.ether_types.end();
}

bool FrameFilter::is_silkit_mac(const MacAddress& address) const noexcept
{
    return std::find(options_.silkit_macs.begin(), options_.silkit_macs.end(), address) != options_.silkit_macs.end();
}

FrameVerdict FrameFilter::to_external(std::span<const std::uint8_t> frame) const noexcept
{
    if (const auto length = check_length(frame); length != FrameVerdict::Pass)
    {
        return length;
    }
    if (!options_.silkit_macs.empty() && !is_silkit_mac(*source_of(frame)))
    {
        return FrameVerdict::Filtered;
    }
    return ether_type_passes(frame) ? FrameVerdict::Pass : FrameVerdict::Filtered;
}

FrameVerdict FrameFilter::from_external(std::span<const std::uint8_t> frame) const noexcept
{
    if (const auto length = check_length(frame); length != FrameVerdict::Pass)
    {
        return length;
    }
    if (!options_.silkit_macs.empty())
    {
        const auto destination = *destination_of(frame);
        if (!destination.is_group() && !is_silkit_mac(destination))
        {
            return FrameVerdict::Filtered;
        }
    }
    return ether_type_passes(frame) ? FrameVerdict::Pass : FrameVerdict::Filtered;
}

bool FrameFilter::passes_every_frame() const noexcept
{
    return options_.ether_types.empty() && options_.silkit_macs.empty();
}

std::string FrameFilter::describe() const
{
    if (passes_every_frame())
    {
        return "every frame, both ways";
    }
    std::string text;
    if (!options_.ether_types.empty())
    {
        text = "EtherType ";
        for (std::size_t i = 0; i < options_.ether_types.size(); ++i)
        {
            text.append(i == 0 ? "" : ",").append(format_ether_type(options_.ether_types[i]));
        }
    }
    if (!options_.silkit_macs.empty())
    {
        text.append(text.empty() ? "SIL Kit MAC " : " and SIL Kit MAC ");
        for (std::size_t i = 0; i < options_.silkit_macs.size(); ++i)
        {
            text.append(i == 0 ? "" : ",").append(to_string(options_.silkit_macs[i]));
        }
    }
    return text + " only, both ways";
}

} // namespace vehicle_sil::eth_link
