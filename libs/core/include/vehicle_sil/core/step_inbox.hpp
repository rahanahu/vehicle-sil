// SPDX-License-Identifier: MIT
//
// Buffers timestamped messages until a simulation step boundary (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vehicle_sil {

/// Tie-break that keeps messages with equal timestamps in arrival order.
struct ArrivalOrder
{
    template <typename T>
    constexpr bool operator()(const T&, const T&) const noexcept
    {
        return false;
    }
};

/// Collects messages that SIL Kit delivers on its own threads and hands them to the simulation
/// at step boundaries, in a deterministic order.
///
/// SIL Kit guarantees that at the step for time T every message stamped earlier than T has been
/// received, but it does not fix whether a message stamped exactly T arrives before or after
/// that step. drain_before(T) therefore releases only messages stamped strictly before T. A
/// message stamped T is released at the next step.
///
/// Released messages are ordered by timestamp, then by `TieBreak` (a strict weak ordering on
/// T, e.g. the sender's address), then by arrival. SIL Kit keeps the order of messages from one
/// sender, so messages of one sender with equal keys keep their send order. The order of equal
/// keys from different senders is not deterministic; choose a TieBreak that tells senders apart
/// when that matters.
///
/// The capacity is fixed: storage is allocated once in the constructor. When the inbox is full,
/// push() drops the message and counts it in overflows().
/// Overflow is not deterministic near the limit: whether a message stamped exactly T is still
/// queued when the next push arrives depends on timing. Size the capacity for the worst case and
/// treat any overflow as an error.
///
/// Thread safety: push() may be called from any thread concurrently with drain_before(). The
/// callback of drain_before() runs without the inbox lock held, so it may call push().
/// drain_before() itself has a single consumer: call it from one thread at a time (normally the
/// step handler).
template <typename T, typename TieBreak = ArrivalOrder>
class StepInbox
{
public:
    explicit StepInbox(std::size_t capacity, TieBreak tie_break = {}) : capacity_(capacity), tie_break_(std::move(tie_break))
    {
        if (capacity_ == 0)
        {
            throw std::invalid_argument("StepInbox capacity must be > 0");
        }
        pending_.reserve(capacity_);
        released_.reserve(capacity_);
    }

    /// Queues `value` stamped with `timestamp`. Returns false (and counts an overflow) if full.
    bool push(SimTime timestamp, T value)
    {
        const std::lock_guard lock(mutex_);
        if (pending_.size() >= capacity_)
        {
            ++overflows_;
            return false;
        }
        pending_.push_back(Entry{timestamp, next_sequence_++, std::move(value)});
        return true;
    }

    /// Calls `sink(SimTime timestamp, T& value)` for every queued message stamped before `now`,
    /// in deterministic order, and removes them. Returns the number of messages released.
    template <typename Sink>
    std::size_t drain_before(SimTime now, Sink&& sink)
    {
        {
            const std::lock_guard lock(mutex_);
            released_.clear();
            auto keep = pending_.begin();
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
            {
                if (it->timestamp < now)
                {
                    released_.push_back(std::move(*it));
                }
                else
                {
                    if (keep != it)
                    {
                        *keep = std::move(*it);
                    }
                    ++keep;
                }
            }
            pending_.erase(keep, pending_.end());
        }
        std::sort(released_.begin(), released_.end(), [this](const Entry& a, const Entry& b) {
            if (a.timestamp != b.timestamp)
            {
                return a.timestamp < b.timestamp;
            }
            if (tie_break_(a.value, b.value))
            {
                return true;
            }
            if (tie_break_(b.value, a.value))
            {
                return false;
            }
            return a.sequence < b.sequence;
        });
        for (auto& entry : released_)
        {
            sink(entry.timestamp, entry.value);
        }
        const std::size_t count = released_.size();
        released_.clear();
        return count;
    }

    /// Discards everything queued.
    void clear()
    {
        const std::lock_guard lock(mutex_);
        pending_.clear();
    }

    [[nodiscard]] std::size_t size() const
    {
        const std::lock_guard lock(mutex_);
        return pending_.size();
    }

    [[nodiscard]] std::uint64_t overflows() const
    {
        const std::lock_guard lock(mutex_);
        return overflows_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Entry
    {
        SimTime timestamp;
        std::uint64_t sequence;
        T value;
    };

    const std::size_t capacity_;
    TieBreak tie_break_;
    mutable std::mutex mutex_;
    std::vector<Entry> pending_;
    std::vector<Entry> released_; // only touched by drain_before(), which must not run concurrently with itself
    std::uint64_t next_sequence_ = 0;
    std::uint64_t overflows_ = 0;
};

} // namespace vehicle_sil
