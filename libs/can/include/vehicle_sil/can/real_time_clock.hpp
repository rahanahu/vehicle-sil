// SPDX-License-Identifier: MIT
//
// Clocks of the real-time driver and real-time ports (vehicle_sil::can).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace vehicle_sil::can {

/// The clock a RealTimeDriver steps by and real-time ports stamp arrivals with. A driver and
/// the ports of its host must use the same clock.
///
/// Times are SimTime: the elapsed time since the clock's origin.
class RealTimeClock
{
public:
    /// Told when the time of a clock jumps (ManualRealTimeClock), so that a waiting driver
    /// looks at the time again.
    class Listener
    {
    public:
        /// Called from the thread that changed the time. Must not call back into the clock's
        /// add_listener() or remove_listener().
        virtual void on_clock_changed() noexcept = 0;

    protected:
        ~Listener() = default;
    };

    virtual ~RealTimeClock() = default;

    /// The current time. Never decreases. Thread safe.
    [[nodiscard]] virtual SimTime now() const = 0;

    /// Blocks on `cv` (`lock` holds the mutex `cv` is used with) until now() >= `deadline`,
    /// until `cv` is notified, or spuriously. The caller checks its condition again afterwards.
    virtual void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, SimTime deadline) const = 0;

    /// Registers a listener; the default clock never jumps and ignores it. Thread safe.
    virtual void add_listener(Listener& listener) { static_cast<void>(listener); }
    /// Removes a listener. After it returns, the listener is not called any more. Thread safe.
    virtual void remove_listener(Listener& listener) noexcept { static_cast<void>(listener); }

protected:
    RealTimeClock() = default;
    RealTimeClock(const RealTimeClock&) = default;
    RealTimeClock& operator=(const RealTimeClock&) = default;
};

/// The wall clock (std::chrono::steady_clock). Its origin is the time it was constructed.
class SteadyRealTimeClock final : public RealTimeClock
{
public:
    SteadyRealTimeClock() noexcept : origin_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] SimTime now() const override;
    void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, SimTime deadline) const override;

private:
    std::chrono::steady_clock::time_point origin_;
};

/// A clock that only moves when it is told to, for deterministic tests of real-time drivers and
/// ports. advance_to() wakes the drivers that wait on it.
///
/// It must outlive the drivers and ports that use it.
class ManualRealTimeClock final : public RealTimeClock
{
public:
    explicit ManualRealTimeClock(SimTime start = SimTime{}) noexcept : now_(start) {}

    [[nodiscard]] SimTime now() const override;
    /// Waits for a notification of `cv` if `deadline` has not come yet (advance_to() notifies
    /// the drivers through their listeners).
    void wait_until(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, SimTime deadline) const override;
    void add_listener(Listener& listener) override;
    void remove_listener(Listener& listener) noexcept override;

    /// Sets the time to `time` (>= now(); std::invalid_argument otherwise) and tells the
    /// listeners. Thread safe.
    void advance_to(SimTime time);
    /// advance_to(now() + `duration`), `duration` >= 0.
    void advance(SimDuration duration);

private:
    mutable std::mutex time_mutex_;
    SimTime now_;
    std::mutex listener_mutex_; ///< taken before a listener's own locks, never inside them
    std::vector<Listener*> listeners_;
};

} // namespace vehicle_sil::can
