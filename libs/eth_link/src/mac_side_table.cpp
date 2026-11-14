// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/mac_side_table.hpp"

#include <stdexcept>
#include <string>

namespace vehicle_sil::eth_link {
namespace {

std::size_t slot_count_for(std::size_t capacity)
{
    std::size_t count = 2;
    while (count < 2 * capacity)
    {
        count *= 2;
    }
    return count;
}

/// Spreads the 48 bits of an address over the index bits (the low bytes of MAC addresses of
/// one vendor differ, the high bytes do not).
std::uint64_t mix(std::uint64_t key) noexcept
{
    key ^= key >> 29;
    key *= 0xBF58'476D'1CE4'E5B9u;
    key ^= key >> 32;
    return key;
}

} // namespace

MacSideTable::MacSideTable(std::size_t capacity) :
  capacity_(capacity)
{
    if (capacity == 0 || capacity > kMaxCapacity)
    {
        throw std::invalid_argument("MacSideTable: capacity must be between 1 and " + std::to_string(kMaxCapacity) +
                                    ", got " + std::to_string(capacity));
    }
    slots_.resize(slot_count_for(capacity));
}

std::size_t MacSideTable::home_of(std::uint64_t key) const noexcept
{
    return static_cast<std::size_t>(mix(key)) & (slots_.size() - 1);
}

std::size_t MacSideTable::find_index(std::uint64_t key) const noexcept
{
    const std::size_t mask = slots_.size() - 1;
    std::size_t index = home_of(key);
    // Linear probing ends: at most capacity_ slots are used, fewer than half of them.
    while (slots_[index].used && slots_[index].key != key)
    {
        index = (index + 1) & mask;
    }
    return index;
}

void MacSideTable::erase(std::size_t index) noexcept
{
    const std::size_t mask = slots_.size() - 1;
    std::size_t hole = index;
    for (std::size_t next = (hole + 1) & mask; slots_[next].used; next = (next + 1) & mask)
    {
        // The entry at `next` may move into the hole unless its home lies cyclically in (hole, next].
        const std::size_t home = home_of(slots_[next].key);
        const bool stays = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
        if (!stays)
        {
            slots_[hole] = slots_[next];
            hole = next;
        }
    }
    slots_[hole] = Slot{};
    --size_;
}

bool MacSideTable::insert(std::uint64_t key, Side side, bool& table_full_now) noexcept
{
    if (size_ >= capacity_ && side != Side::External)
    {
        // The SIL Kit side first: give it the place of the oldest address on the outside.
        std::size_t oldest = slots_.size();
        for (std::size_t i = 0; i < slots_.size(); ++i)
        {
            if (slots_[i].used && slots_[i].side == Side::External &&
                (oldest == slots_.size() || slots_[i].age < slots_[oldest].age))
            {
                oldest = i;
            }
        }
        if (oldest != slots_.size())
        {
            erase(oldest);
        }
    }
    if (size_ >= capacity_)
    {
        table_full_now = !reported_full_;
        reported_full_ = true;
        return false;
    }
    slots_[find_index(key)] = Slot{key, next_age_++, true, side};
    ++size_;
    return true;
}

MacSideTable::Result<MacSideTable::FromSilKit> MacSideTable::on_silkit_source(const MacAddress& source)
{
    if (source.is_group())
    {
        return {FromSilKit::Forward};
    }
    const auto key = source.value();
    const std::lock_guard lock(mutex_);
    auto& slot = slots_[find_index(key)];
    if (!slot.used)
    {
        Result<FromSilKit> result{FromSilKit::Forward};
        result.not_learned = !insert(key, Side::SilKit, result.table_full);
        return result;
    }
    switch (slot.side)
    {
    case Side::SilKit: return {FromSilKit::Forward};
    case Side::External: slot.side = Side::Both; return {FromSilKit::NewConflict};
    case Side::Both: break;
    }
    return {FromSilKit::Blocked};
}

MacSideTable::Result<MacSideTable::FromExternal> MacSideTable::on_external_source(const MacAddress& source, bool learn)
{
    if (source.is_group())
    {
        return {FromExternal::Forward};
    }
    const auto key = source.value();
    const std::lock_guard lock(mutex_);
    const auto& slot = slots_[find_index(key)];
    if (!slot.used)
    {
        Result<FromExternal> result{FromExternal::Forward};
        if (learn)
        {
            result.not_learned = !insert(key, Side::External, result.table_full);
        }
        return result;
    }
    switch (slot.side)
    {
    case Side::SilKit: return {FromExternal::SilKitSide};
    case Side::External: return {FromExternal::Forward};
    case Side::Both: break;
    }
    return {FromExternal::Blocked};
}

bool MacSideTable::add_silkit_address(const MacAddress& address)
{
    if (address.is_group())
    {
        return false;
    }
    const auto key = address.value();
    const std::lock_guard lock(mutex_);
    auto& slot = slots_[find_index(key)];
    if (slot.used)
    {
        if (slot.side == Side::External)
        {
            slot.side = Side::Both;
        }
        return true;
    }
    bool full = false;
    return insert(key, Side::SilKit, full);
}

bool MacSideTable::mark_both_sides(const MacAddress& address)
{
    const auto key = address.value();
    const std::lock_guard lock(mutex_);
    auto& slot = slots_[find_index(key)];
    if (slot.used)
    {
        const bool before = slot.side == Side::Both;
        slot.side = Side::Both;
        return !before;
    }
    bool full = false;
    (void)insert(key, Side::Both, full);
    return true;
}

std::optional<MacSideTable::Side> MacSideTable::side_of(const MacAddress& address) const
{
    const std::lock_guard lock(mutex_);
    const auto& slot = slots_[find_index(address.value())];
    if (!slot.used)
    {
        return std::nullopt;
    }
    return slot.side;
}

std::size_t MacSideTable::size() const
{
    const std::lock_guard lock(mutex_);
    return size_;
}

} // namespace vehicle_sil::eth_link
