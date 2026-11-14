// SPDX-License-Identifier: MIT
//
// Raw Ethernet frames and MAC addresses (vehicle_sil::eth).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vehicle_sil::eth {

// The values below come from general references, not from the standard; docs/internals/ethernet.md
// lists them ("未確認の値"). Where they matter, the user can change them (ChannelOptions,
// VirtualEthernetBusOptions).

/// Bytes before the payload that vehicle-sil reads: destination address (bytes 0..5) and source
/// address (bytes 6..11), followed by two bytes (EtherType or length) that vehicle-sil does not
/// interpret. A frame shorter than this is not sent.
inline constexpr std::size_t kHeaderBytes = 14;

/// Default of ChannelOptions::max_frame_bytes: a frame without the frame check sequence.
inline constexpr std::size_t kDefaultMaxFrameBytes = 1522;

/// Default of VirtualEthernetBusOptions::min_frame_bytes: shorter frames reach the receivers
/// padded with zero bytes to this length, as SIL Kit's Ethernet controller pads them.
inline constexpr std::size_t kDefaultMinFrameBytes = 60;

/// A MAC address, in the order of the bytes in the frame.
struct MacAddress
{
    std::array<std::uint8_t, 6> bytes{};

    /// Reads "02:00:00:00:00:01" (six pairs of hex digits, upper or lower case, separated by ':'
    /// or '-'). std::nullopt for anything else.
    [[nodiscard]] static std::optional<MacAddress> parse(std::string_view text) noexcept;

    /// "02:00:00:00:00:01" (lower case, ':').
    [[nodiscard]] std::string to_string() const;

    /// All bytes 0xFF.
    [[nodiscard]] constexpr bool is_broadcast() const noexcept
    {
        return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0xFF; });
    }
    /// The group bit (bit 0 of the first byte) is set: a multicast address, or the broadcast one.
    [[nodiscard]] constexpr bool is_multicast() const noexcept { return (bytes[0] & 0x01u) != 0; }

    [[nodiscard]] friend constexpr auto operator<=>(const MacAddress&, const MacAddress&) noexcept = default;
};

/// The broadcast address ff:ff:ff:ff:ff:ff.
inline constexpr MacAddress kBroadcast{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

/// One raw Ethernet frame: the bytes from the destination address to the end of the payload,
/// without the frame check sequence. vehicle-sil does not interpret the bytes after the source
/// address (EtherType, VLAN tags, payload); they go through as they are.
///
/// The bytes are held in the object (no allocation), up to kCapacity. Copies copy only size()
/// bytes. Comparison compares size() and the bytes.
class Frame
{
public:
    /// The most bytes a Frame holds: room above kDefaultMaxFrameBytes for frames with more tags.
    /// Larger frames (jumbo frames) are not supported.
    static constexpr std::size_t kCapacity = 2048;

    Frame() noexcept {} // NOLINT: the bytes beyond size() are never read
    Frame(const Frame& other) noexcept : size_(other.size_) { std::copy_n(other.data_.data(), size_, data_.data()); }
    Frame& operator=(const Frame& other) noexcept
    {
        size_ = other.size_;
        std::copy_n(other.data_.data(), size_, data_.data());
        return *this;
    }

    /// A frame with a copy of `bytes`; std::nullopt if they are more than kCapacity.
    [[nodiscard]] static std::optional<Frame> from(std::span<const std::uint8_t> bytes) noexcept
    {
        Frame frame;
        if (!frame.assign(bytes))
        {
            return std::nullopt;
        }
        return frame;
    }

    /// Replaces the bytes with a copy of `bytes`. Returns false (and leaves the frame as it was)
    /// if they are more than kCapacity.
    bool assign(std::span<const std::uint8_t> bytes) noexcept
    {
        if (bytes.size() > kCapacity)
        {
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), data_.begin());
        size_ = bytes.size();
        return true;
    }

    /// Appends zero bytes up to `length` (at most kCapacity). Does nothing to a frame that is
    /// at least that long.
    void pad_to(std::size_t length) noexcept
    {
        length = std::min(length, kCapacity);
        if (size_ < length)
        {
            std::fill(data_.begin() + static_cast<std::ptrdiff_t>(size_), data_.begin() + static_cast<std::ptrdiff_t>(length),
                      std::uint8_t{0});
            size_ = length;
        }
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {data_.data(), size_}; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    /// Bytes 0..5 (all zero if the frame is shorter).
    [[nodiscard]] MacAddress destination() const noexcept { return address_at(0); }
    /// Bytes 6..11 (all zero if the frame is shorter).
    [[nodiscard]] MacAddress source() const noexcept { return address_at(6); }

    [[nodiscard]] friend bool operator==(const Frame& a, const Frame& b) noexcept
    {
        return a.size_ == b.size_ && std::equal(a.data_.begin(), a.data_.begin() + static_cast<std::ptrdiff_t>(a.size_),
                                                b.data_.begin());
    }

private:
    [[nodiscard]] MacAddress address_at(std::size_t offset) const noexcept
    {
        MacAddress address;
        if (size_ >= offset + address.bytes.size())
        {
            std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), address.bytes.size(), address.bytes.begin());
        }
        return address;
    }

    std::size_t size_ = 0;
    std::array<std::uint8_t, kCapacity> data_; // only the first size_ bytes are set
};

/// A frame with a time: on receiving, the time the backend gave it (on virtual time, the step
/// time of a time-synchronised sender; see SilKitEthPort for a sender without time
/// synchronisation); on sending, the time of the sending step.
struct TimedFrame
{
    SimTime time{};
    Frame frame;

    [[nodiscard]] friend bool operator==(const TimedFrame&, const TimedFrame&) noexcept = default;
};

} // namespace vehicle_sil::eth
