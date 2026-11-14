// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/mac_side_table.hpp"

#include <stdexcept>

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
    if (capacity == 0)
    {
        throw std::invalid_argument("MacSideTable: capacity must be at least 1");
    }
    slots_.resize(slot_count_for(capacity));
}

std::size_t MacSideTable::find_index(std::uint64_t key) const noexcept
{
    const std::size_t mask = slots_.size() - 1;
    std::size_t index = static_cast<std::size_t>(mix(key)) & mask;
    // Linear probing ends: at most capacity_ slots are used, fewer than half of them.
    while (slots_[index].used && slots_[index].key != key)
    {
        index = (index + 1) & mask;
    }
    return index;
}

bool MacSideTable::insert(std::size_t index, std::uint64_t key, Side side) noexcept
{
    if (size_ >= capacity_)
    {
        return false;
    }
    slots_[index] = Slot{key, true, side};
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
    const auto index = find_index(key);
    auto& slot = slots_[index];
    if (!slot.used)
    {
        if (insert(index, key, Side::SilKit))
        {
            return {FromSilKit::Forward};
        }
        const bool first = !reported_full_;
        reported_full_ = true;
        return {FromSilKit::Forward, first};
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
    const auto index = find_index(key);
    const auto& slot = slots_[index];
    if (!slot.used)
    {
        if (!learn || insert(index, key, Side::External))
        {
            return {FromExternal::Forward};
        }
        const bool first = !reported_full_;
        reported_full_ = true;
        return {FromExternal::Forward, first};
    }
    switch (slot.side)
    {
    case Side::SilKit: return {FromExternal::OwnFrame};
    case Side::External: return {FromExternal::Forward};
    case Side::Both: break;
    }
    return {FromExternal::Blocked};
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
