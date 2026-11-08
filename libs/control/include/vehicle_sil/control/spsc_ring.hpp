// SPDX-License-Identifier: MIT
//
// Fixed-capacity single-producer single-consumer ring (vehicle_sil::control).
#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vehicle_sil::control {

/// A queue of fixed capacity between exactly one producer thread and one consumer thread.
/// Neither side ever waits: try_push() fails when the ring is full, try_pop() when it is empty.
/// Storage is allocated once in the constructor.
template <typename T>
class SpscRing
{
public:
    explicit SpscRing(std::size_t capacity) : slots_(checked(capacity) + 1) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /// Producer only. @returns false (and leaves `value` untouched) if the ring is full.
    bool try_push(T& value)
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = advance(tail);
        if (next == head_.load(std::memory_order_acquire))
        {
            return false;
        }
        slots_[tail] = std::move(value);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer only.
    std::optional<T> try_pop()
    {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
        {
            return std::nullopt;
        }
        std::optional<T> value(std::move(slots_[head]));
        slots_[head] = T{};
        head_.store(advance(head), std::memory_order_release);
        return value;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size() - 1; }

private:
    static std::size_t checked(std::size_t capacity)
    {
        if (capacity == 0 || capacity >= std::numeric_limits<std::size_t>::max() / 2)
        {
            throw std::invalid_argument("SpscRing capacity must be > 0 and reasonable");
        }
        return capacity;
    }

    [[nodiscard]] std::size_t advance(std::size_t i) const noexcept { return i + 1 == slots_.size() ? 0 : i + 1; }

    std::vector<T> slots_;
    // On separate cache lines: the producer writes tail_, the consumer head_.
    alignas(64) std::atomic<std::size_t> head_{0}; ///< next slot to pop (consumer)
    alignas(64) std::atomic<std::size_t> tail_{0}; ///< next slot to fill (producer)
};

} // namespace vehicle_sil::control
