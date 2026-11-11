// SPDX-License-Identifier: MIT
//
// Drives a host on the wall clock with one thread (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <functional>
#include <string>
#include <thread>

namespace vehicle_sil::can {

struct RealTimeDriverOptions
{
    /// Longest time between two steps (> 0) when nothing arrives and no timer is due. Bounds
    /// how late work without a timer runs (frames kept back after QueueFull, frames carried
    /// over to the next step). Also passed to Host::set_step_size(), so that a protocol can
    /// check it like a step size of virtual time.
    SimDuration max_idle = std::chrono::milliseconds(10);

    /// Called once on the driver's thread when a step throws, with error(), after failed() has
    /// become true and before the thread ends (stop() from another thread waits for it). It must
    /// not call stop() of this driver. Exceptions from it are ignored. Empty: nothing is called.
    std::function<void(const std::string& error)> on_failure{};
};

/// Steps a host of real-time ports (TimeDomain::RealTime) on its own thread, one per
/// participant, with the time of a RealTimeClock.
///
/// The thread steps when
/// - a port reports an arrival (Port::set_wakeup()) or something is posted (Host::post()),
/// - the earliest timer of the host is due (Host::next_deadline(); the step runs 1 ns after the
///   deadline, so a timeout that needs a time after its deadline fires), or
/// - max_idle has passed since the previous step.
/// It steps right after start(). The time of a step is the clock's time, but at least 1 ns after
/// the previous step and 1 ns after the latest arrival that woke it (so the step releases that
/// frame even if the clock has not moved, as with ManualRealTimeClock).
///
/// The application talks to the stacks only through Host::post() and the host's application
/// function, both run on the driver's thread; the stacks have no locks.
///
/// Failure: if a step throws, the thread stops stepping and ends; failed() and error() tell why
/// (the host is failed as well). Only a new host and driver recover.
///
/// Registration: add the Steppables and ports to the host and set its application before
/// start(); the first step runs on the driver's thread right after it.
///
/// Shutdown: stop() (or the destructor) removes the wakeup from the host and its ports, stops
/// listening to the clock and joins the thread. Then close the ports and destroy the stacks
/// and the host. Called from the driver's thread (from a step), stop() removes the wakeup and
/// asks the thread to end after the step, but does not join it; stop() from another thread (or
/// the destructor) joins it later. start() and stop() may be called from different threads.
class RealTimeDriver final : private RealTimeClock::Listener
{
public:
    /// Requires TimeDomain::RealTime of the host's ports (Host::require_time_domain(),
    /// std::logic_error) and passes max_idle to Host::set_step_size(). The host must not have
    /// started. `host` and `clock` must outlive the driver.
    RealTimeDriver(Host& host, RealTimeClock& clock, RealTimeDriverOptions options = {});
    ~RealTimeDriver();

    RealTimeDriver(const RealTimeDriver&) = delete;
    RealTimeDriver& operator=(const RealTimeDriver&) = delete;

    /// Sets the wakeup of the host and starts the thread. Only once (std::logic_error).
    void start();
    /// Ends the thread (see the class comment). Idempotent.
    void stop() noexcept;

    /// Waits up to `timeout` until the thread sleeps with nothing to do: no arrival or post that
    /// it has not stepped for, and no change of the clock it has not looked at. Also true once the thread has ended. For tests with ManualRealTimeClock: after
    /// advance_to() and wait_idle(), every step due by then has run.
    [[nodiscard]] bool wait_idle(std::chrono::nanoseconds timeout);

    [[nodiscard]] bool running() const;
    [[nodiscard]] bool failed() const;
    /// Why the driver failed (empty while it has not).
    [[nodiscard]] std::string error() const;
    /// Steps run so far.
    [[nodiscard]] std::uint64_t steps() const;
    /// Time of the last step, if any.
    [[nodiscard]] std::optional<SimTime> last_step() const;

private:
    void on_clock_changed() noexcept override;
    void on_arrival(SimTime arrival) noexcept;
    void run();

    Host& host_;
    RealTimeClock& clock_;
    RealTimeDriverOptions options_;

    std::mutex control_mutex_; ///< serialises start() and the joining part of stop()
    mutable std::mutex mutex_;
    std::condition_variable cv_;      ///< wakes the thread
    std::condition_variable idle_cv_; ///< wakes wait_idle()
    std::thread thread_;           ///< touched under control_mutex_ only
    std::thread::id thread_id_;    ///< the driver thread's id, set by run()
    bool started_ = false;
    bool stop_ = false;
    bool ended_ = false;
    bool sleeping_ = false;
    bool kicked_ = false;        ///< a step is wanted now
    bool clock_changed_ = false; ///< the clock jumped since the thread last looked
    bool listening_ = false;
    std::optional<SimTime> earliest_next_; ///< after the latest arrival
    std::optional<SimTime> last_step_;
    std::uint64_t steps_ = 0;
    bool failed_ = false;
    std::string error_;
};

} // namespace vehicle_sil::can
