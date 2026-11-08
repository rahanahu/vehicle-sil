// SPDX-License-Identifier: MIT
//
// Protocol timers of a J1939 network (vehicle_sil::j1939). Internal to the stack: address
// claiming and the transport protocol start timers here directly; applications and protocol
// parts use vehicle_sil::j1939::Timer (toolbox.hpp), which reserves its slot.
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vehicle_sil::j1939 {

/// How a timer fires relative to the frames of a step (see Network::process()).
enum class TimerKind : std::uint8_t
{
    /// "Nothing arrived by the deadline": fires only once every frame up to the deadline has
    /// been handled. Before a frame with time > deadline, or at the end of a step with
    /// deadline < now. A frame with time == deadline is handled first.
    Timeout,
    /// "Do something at the deadline" (send the next packet, end a wait): fires in the first
    /// step with deadline <= now, before a frame with time > deadline.
    Scheduled,
};

/// Receives the timers it started.
class TimerTarget
{
public:
    /// The timer started with `tag` fired. May start and cancel timers.
    virtual void on_timer(std::uint32_t tag) = 0;

protected:
    TimerTarget() = default;
    TimerTarget(const TimerTarget&) = default;
    TimerTarget& operator=(const TimerTarget&) = default;
    ~TimerTarget() = default;
};

/// Identifies a started timer; stays unique after the timer fired or was cancelled.
struct TimerId
{
    std::uint32_t slot = 0;
    std::uint64_t serial = 0; ///< 0: no timer

    [[nodiscard]] friend constexpr bool operator==(const TimerId&, const TimerId&) noexcept = default;
};

/// A fixed number of timers, ordered by (deadline, start order). Starting a timer does not
/// allocate memory. Not thread safe.
///
/// `reservable` of the slots are handed out by reserve(): every user that may run timers at the
/// same time reserves its share first (a node with a NAME 2, a Timer 1), so start() never runs
/// out of slots while every user keeps to its share. The other slots belong to the users sized
/// with the set (the transport sessions, one each).
class TimerSet
{
public:
    /// `capacity`: the most timers that run at the same time. `reservable`: how many of them
    /// reserve() hands out (at most `capacity`; all of them by default).
    explicit TimerSet(std::size_t capacity, std::size_t reservable = SIZE_MAX);
    /// Reserves `count` slots. false (and nothing reserved) if fewer are left.
    [[nodiscard]] bool reserve(std::size_t count) noexcept;
    /// Returns `count` reserved slots.
    void release(std::size_t count) noexcept;
    [[nodiscard]] std::size_t reserved() const noexcept { return reserved_; }
    [[nodiscard]] std::size_t reservable() const noexcept { return reservable_; }

    /// Starts a timer. Throws std::length_error if `capacity` timers are already running.
    TimerId start(TimerKind kind, SimTime deadline, TimerTarget& target, std::uint32_t tag);
    /// Stops the timer. false if it is not running (already fired or cancelled).
    bool cancel(TimerId id) noexcept;
    [[nodiscard]] bool is_running(TimerId id) const noexcept;

    /// Earliest deadline of the running timers.
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept;
    [[nodiscard]] std::size_t running() const noexcept { return running_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    /// Starts a firing round: the fire_one_*() calls that follow consider only the timers
    /// started before this call. A timer started while the round runs (e.g. a timer restarting
    /// itself at a deadline that is already due) waits for the next round, so a round always
    /// ends. Before the first call every timer is considered.
    void begin_round() noexcept { round_limit_ = next_serial_; }

    /// Fires the first timer (by deadline, start order) with deadline < `time`, of any kind.
    /// false if there is none.
    bool fire_one_before(SimTime time);
    /// Fires the first timer that is due at the end of the step at `now`: a Timeout with
    /// deadline < now or a Scheduled timer with deadline <= now. false if there is none.
    bool fire_one_due(SimTime now);

private:
    struct Slot
    {
        TimerTarget* target = nullptr; ///< nullptr: free
        SimTime deadline{};
        std::uint64_t serial = 0;
        std::uint32_t tag = 0;
        TimerKind kind = TimerKind::Timeout;
    };

    template <typename Due>
    bool fire_first(Due due);

    std::vector<Slot> slots_;
    std::uint64_t next_serial_ = 1;
    std::size_t running_ = 0;
    std::size_t reservable_;
    std::size_t reserved_ = 0;
    std::uint64_t round_limit_ = UINT64_MAX; ///< Timers with serial >= this wait for the next round.
};

} // namespace vehicle_sil::j1939
