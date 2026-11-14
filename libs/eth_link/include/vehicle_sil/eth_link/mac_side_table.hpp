// SPDX-License-Identifier: MIT
//
// Which side of an Ethernet bridge each source MAC address was seen on: lets the bridge drop its
// own frames when they come back and notice an address that is on both sides (a second bridge
// between the same networks, or one address used on both), without relying on OS settings
// (no SIL Kit).
#pragma once

#include "vehicle_sil/eth_link/mac_address.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace vehicle_sil::eth_link {

/// What the table records, applied to the source MAC of every frame the bridge forwards:
///  - a frame from SIL Kit teaches that its source is on the SIL Kit side;
///  - a frame from the outside teaches that its source is on the outside;
///  - a frame from SIL Kit whose source was seen on the outside means the address is on both
///    sides: it is marked so (NewConflict, once), and from then on frames from it are dropped in
///    both directions (Blocked);
///  - a frame from the outside whose source is on the SIL Kit side is reported as SilKitSide. The
///    table cannot tell whether that is the bridge's own frame coming back (some interfaces
///    deliver what was sent on them) or the address on both sides; the caller decides, and marks
///    the address with mark_both_sides() in the second case.
/// Group (broadcast, multicast) source addresses are never learned.
///
/// Fixed capacity, allocated in the constructor. When it is full, an address on the SIL Kit side
/// (or on both) takes the place of the oldest address on the outside; an address on the outside
/// is not learned. A frame whose address could not be learned reports not_learned (it is not
/// checked against loops), and the first time that happens also table_full. Addresses are never
/// forgotten otherwise. Thread safe (one mutex).
class MacSideTable
{
public:
    static constexpr std::size_t kDefaultCapacity = 1024;
    static constexpr std::size_t kMaxCapacity = 65536;

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
        SilKitSide, ///< the address is on the SIL Kit side: the bridge's own frame, or a conflict
        Blocked,    ///< the address is on both sides: drop
    };

    template <typename Outcome>
    struct Result
    {
        Outcome outcome;
        /// The address had to be learned but did not fit.
        bool not_learned = false;
        /// true the first time an address did not fit.
        bool table_full = false;
    };

    /// Throws std::invalid_argument if `capacity` is 0 or above kMaxCapacity.
    explicit MacSideTable(std::size_t capacity = kDefaultCapacity);

    /// A frame from SIL Kit with this source passed the filter and is about to go out.
    [[nodiscard]] Result<FromSilKit> on_silkit_source(const MacAddress& source);
    /// A frame from the outside with this source. `learn`: the frame passed the filter, so its
    /// source is learned as on the outside (a filtered frame only checks the side).
    [[nodiscard]] Result<FromExternal> on_external_source(const MacAddress& source, bool learn);
    /// Learns `address` as on the SIL Kit side before any frame (an address the user named).
    /// false if it does not fit. Group addresses are not learned.
    bool add_silkit_address(const MacAddress& address);
    /// Marks `address` as on both sides. true if it was not on both sides before.
    bool mark_both_sides(const MacAddress& address);

    [[nodiscard]] std::optional<Side> side_of(const MacAddress& address) const;
    /// How many addresses are learned.
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Slot
    {
        std::uint64_t key = 0;
        std::uint64_t age = 0; ///< when it was learned (larger is newer)
        bool used = false;
        Side side = Side::SilKit;
    };

    [[nodiscard]] std::size_t home_of(std::uint64_t key) const noexcept;
    /// The index of the slot holding `key`, or of the empty slot where it would go.
    [[nodiscard]] std::size_t find_index(std::uint64_t key) const noexcept;
    /// Learns `key` on `side`; when full, an address on the SIL Kit side or on both takes the
    /// place of the oldest one on the outside. false (and table_full_now, once) if it does not fit.
    bool insert(std::uint64_t key, Side side, bool& table_full_now) noexcept;
    /// Removes the entry in slot `index` (linear probing: moves the entries after it back).
    void erase(std::size_t index) noexcept;

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_; // a power of two, at least twice the capacity, so never all used
    std::size_t size_ = 0;
    std::uint64_t next_age_ = 0;
    bool reported_full_ = false;
};

} // namespace vehicle_sil::eth_link
