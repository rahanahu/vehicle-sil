// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/timer_set.hpp"

#include <stdexcept>

namespace vehicle_sil::j1939 {

TimerSet::TimerSet(std::size_t capacity, std::size_t reservable)
  : slots_(capacity), reservable_(reservable < capacity ? reservable : capacity)
{
}

bool TimerSet::reserve(std::size_t count) noexcept
{
    if (count > reservable_ - reserved_)
    {
        return false;
    }
    reserved_ += count;
    return true;
}

void TimerSet::release(std::size_t count) noexcept
{
    reserved_ -= count < reserved_ ? count : reserved_;
}

TimerId TimerSet::start(TimerKind kind, SimTime deadline, TimerTarget& target, std::uint32_t tag)
{
    for (std::size_t i = 0; i < slots_.size(); ++i)
    {
        Slot& slot = slots_[i];
        if (slot.target == nullptr)
        {
            slot = Slot{&target, deadline, next_serial_++, tag, kind};
            ++running_;
            return TimerId{static_cast<std::uint32_t>(i), slot.serial};
        }
    }
    throw std::length_error("vehicle_sil::j1939::TimerSet: too many timers running");
}

bool TimerSet::cancel(TimerId id) noexcept
{
    if (!is_running(id))
    {
        return false;
    }
    slots_[id.slot].target = nullptr;
    --running_;
    return true;
}

bool TimerSet::is_running(TimerId id) const noexcept
{
    return id.serial != 0 && id.slot < slots_.size() && slots_[id.slot].target != nullptr && slots_[id.slot].serial == id.serial;
}

std::optional<SimTime> TimerSet::next_deadline() const noexcept
{
    std::optional<SimTime> earliest;
    for (const auto& slot : slots_)
    {
        if (slot.target != nullptr && (!earliest || slot.deadline < *earliest))
        {
            earliest = slot.deadline;
        }
    }
    return earliest;
}

template <typename Due>
bool TimerSet::fire_first(Due due)
{
    Slot* first = nullptr;
    for (auto& slot : slots_)
    {
        if (slot.target == nullptr || slot.serial >= round_limit_ || !due(slot))
        {
            continue;
        }
        // Serials grow with every start(), so they give the start order.
        if (first == nullptr || slot.deadline < first->deadline || (slot.deadline == first->deadline && slot.serial < first->serial))
        {
            first = &slot;
        }
    }
    if (first == nullptr)
    {
        return false;
    }
    TimerTarget* target = first->target;
    const std::uint32_t tag = first->tag;
    first->target = nullptr; // free the slot first: the target may start a new timer
    --running_;
    target->on_timer(tag);
    return true;
}

bool TimerSet::fire_one_before(SimTime time)
{
    return fire_first([time](const Slot& slot) { return slot.deadline < time; });
}

bool TimerSet::fire_one_due(SimTime now)
{
    return fire_first([now](const Slot& slot) {
        return slot.kind == TimerKind::Timeout ? slot.deadline < now : slot.deadline <= now;
    });
}

} // namespace vehicle_sil::j1939
