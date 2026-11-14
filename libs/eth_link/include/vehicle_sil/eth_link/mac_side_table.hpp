// SPDX-License-Identifier: MIT
//
// Which side of an Ethernet bridge each source MAC address was seen on: keeps the bridge from
// forwarding frames in a loop without relying on how the OS reports the frames the bridge sent
// itself (no SIL Kit).
#pragma once

#include "vehicle_sil/eth_link/mac_address.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace vehicle_sil::eth_link {

/// The rule, applied to the source MAC of every frame the bridge forwards:
///  - a frame from SIL Kit teaches that its source is on the SIL Kit side;
///  - a frame from the outside whose source is on the SIL Kit side is the bridge's own frame
///    coming back (some interfaces deliver what was sent on them): dropped (OwnFrame);
///  - a frame from the outside teaches that its source is on the outside;
///  - a frame from SIL Kit whose source was seen on the outside means the address is on both
///    sides: two bridges between the same networks, or one address used on both. The address
///    is marked as on both sides (NewConflict, once) and from then on frames from it are dropped
///    in both directions (Blocked). That stops a loop through two bridges, whatever order the
///    copies arrive in.
/// Group (broadcast, multicast) source addresses are never learned.
///
/// Fixed capacity, allocated in the constructor; when it is full, new addresses are not learned
/// (their frames are forwarded unprotected) and the first such call reports table_full.
/// Addresses are never forgotten. Thread safe (one mutex).
class MacSideTable
{
public:
    static constexpr std::size_t kDefaultCapacity = 1024;

    enum class Side : std::uint8_t
    {
        SilKit,
        External,
        Both,
    };

    enum class FromSilKit
    {
        Forward,
        NewConflict, ///< the address was on the outside and is now on both sides: drop, warn
        Blocked,     ///< the address is on both sides: drop
    };

    enum class FromExternal
    {
        Forward,
        OwnFrame, ///< the address is on the SIL Kit side: the bridge's own frame came back; drop
        Blocked,  ///< the address is on both sides: drop
    };

    template <typename Outcome>
    struct Result
    {
        Outcome outcome;
        /// true the first time an address could not be learned because the table is full.
        bool table_full = false;
    };

    /// Throws std::invalid_argument if `capacity` is 0.
    explicit MacSideTable(std::size_t capacity = kDefaultCapacity);

    /// A frame from SIL Kit with this source passed the filter and is about to go out.
    [[nodiscard]] Result<FromSilKit> on_silkit_source(const MacAddress& source);
    /// A frame from the outside with this source. `learn`: the frame passed the filter, so its
    /// source is learned as on the outside (a filtered frame only checks for OwnFrame and Blocked).
    [[nodiscard]] Result<FromExternal> on_external_source(const MacAddress& source, bool learn);

    [[nodiscard]] std::optional<Side> side_of(const MacAddress& address) const;
    /// How many addresses are learned.
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Slot
    {
        std::uint64_t key = 0;
        bool used = false;
        Side side = Side::SilKit;
    };

    /// The index of the slot holding `key`, or of the empty slot where it would go.
    [[nodiscard]] std::size_t find_index(std::uint64_t key) const noexcept;
    /// Learns `key` on `side` in the empty slot `index`; false if the table is full.
    bool insert(std::size_t index, std::uint64_t key, Side side) noexcept;

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_; // a power of two, at least twice the capacity, so never all used
    std::size_t size_ = 0;
    bool reported_full_ = false;
};

} // namespace vehicle_sil::eth_link
