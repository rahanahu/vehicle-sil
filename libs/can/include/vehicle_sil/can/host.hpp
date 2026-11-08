// SPDX-License-Identifier: MIT
//
// Runs the protocol stacks of one participant step by step (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::can {

/// The protocol processing of one CAN channel that a Host steps (e.g. one J1939 network).
///
/// A Host calls, for each step at time `now`:
///   begin(now), then on_port_event() for every event and receive() for every frame the port
///   releases before `now`, then process(), then (after the application ran) flush(port).
/// The implementation has no threads and reads no clock: `now` is its only notion of time. It
/// is called from one thread at a time (the driver's). The contract is described in
/// docs/architecture.md ("Steppable の契約").
class Steppable
{
public:
    virtual ~Steppable() = default;

    /// Starts the step at `now`. `now` is greater than the previous step's.
    virtual void begin(SimTime now) = 0;
    /// A frame received before `now`. Only collect it here; handle it in process().
    virtual void receive(const TimedFrame& frame) = 0;
    /// An event of the port about a frame this Steppable sent earlier (PortEvent), released
    /// before `now`. Called after begin() and before the frames of the step are received. Only
    /// record it here; it must not send. The default ignores events.
    virtual void on_port_event(const PortEvent& event) { static_cast<void>(event); }
    /// Handles the collected frames and the timers that are due.
    virtual void process() = 0;
    /// Sends what the step produced, through `port` (the port registered with this Steppable).
    virtual void flush(Port& port) = 0;

    /// Called once by the host before the first step's begin(), with the host's step size if it
    /// is known (Host::set_step_size()). A Steppable checks here whether its protocol can run
    /// with that step size and throws (e.g. std::invalid_argument) if not; the host then fails.
    /// The default accepts any step size.
    virtual void start(std::optional<SimDuration> step_size) { static_cast<void>(step_size); }

    /// The earliest deadline of the Steppable's timers: it needs a step after this time even if
    /// nothing arrives. Empty: no timer is running. A real-time driver sleeps until then (or
    /// until something arrives, or at most its idle limit). Work for the next flush that has no
    /// timer (frames kept back after QueueFull, frames carried over) is not included. The
    /// default has no timers.
    [[nodiscard]] virtual std::optional<SimTime> next_deadline() const noexcept { return std::nullopt; }

protected:
    Steppable() = default;
    Steppable(const Steppable&) = default;
    Steppable& operator=(const Steppable&) = default;
};

/// Steps a fixed set of (Steppable, Port) pairs of one participant (or one test loop) together.
///
/// Before the first step, the host calls start() of every Steppable in registration order.
/// One step at `now` runs five stages, each over all pairs in registration order:
///   1. begin(now)   2. on_port_event() of the events, then receive() of the frames, that the
///      port releases before `now`
///   3. process()    4. the functions passed to post() (in the order they were posted), then
///      the application, once   5. flush(port)
/// So a receive handler of one Steppable may send on another Steppable of the same host: both
/// are inside the same step and the frame leaves in stage 5.
///
/// Time domain: all ports of a host belong to one TimeDomain, and the driver requires its
/// domain (require_time_domain()): StepDriver and bind_host() Virtual, RealTimeDriver RealTime.
/// Mixing them is a configuration error (std::logic_error), e.g. a Vector XL port in a host that
/// SIL Kit's time synchronisation steps.
///
/// Failure: if step() throws (from a Steppable, the application or a port, or because it was
/// misused), the host becomes failed and rethrows. Every later step() throws std::logic_error.
/// Only a new host recovers.
///
/// The host does not own the Steppables and ports. Destroy them after the host is no longer
/// stepped (stop the driver first).
///
/// Not thread safe, except post() and set_wakeup(): call everything else from the driver's
/// thread.
class Host
{
public:
    using Application = std::function<void(SimTime now)>;

    Host() = default;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// Registers a Steppable with the port it sends and receives on. Only before the first step;
    /// a port can belong to one Steppable only; the port's time domain must be the one of the
    /// ports added before and the one the driver requires. Throws std::logic_error otherwise. A
    /// port or Steppable must not be registered with another host as well (not detected).
    void add(Steppable& steppable, Port& port);

    /// Called by a driver: the ports must belong to `domain`. Throws std::logic_error if a port
    /// added before does not, or if another domain was required before; add() checks the later
    /// ports. Only before the first step.
    void require_time_domain(TimeDomain domain);

    /// The time domain of the ports (or the required one), empty if neither is known.
    [[nodiscard]] std::optional<TimeDomain> time_domain() const noexcept;

    /// Queues `fn` to run on the driver's thread at the start of stage 4 of the next step (in
    /// the order of the post() calls), before the application. Use it to hand work to the
    /// protocol stacks from another thread or from outside the step (e.g. a SIL Kit handler);
    /// the stacks have no locks. Thread safe. A function posted during a step runs in the next
    /// step. If `fn` throws, the step fails like it does for the application. A failed host
    /// discards the functions posted to it. With virtual time, the step a function runs in depends on when
    /// it was posted; data that must be handled deterministically belongs in a StepInbox.
    void post(std::function<void()> fn);

    /// Sets (or with an empty function removes) the wakeup of a real-time driver: the host
    /// passes it to every port (Port::set_wakeup()) and calls it after post() (with the time
    /// zero). Thread safe.
    void set_wakeup(Wakeup wakeup);

    /// The earliest Steppable::next_deadline() of the Steppables (empty: none).
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept;

    /// The application's work for each step (stage 4). Optional. Only before the first step.
    void set_application(Application application);

    /// The size of the steps the driver runs (StepDriver::add() and bind_host() set it). Only
    /// before the first step; must be > 0 (std::invalid_argument). Passed to every
    /// Steppable::start() before the first step.
    void set_step_size(SimDuration step_size);

    /// Runs the step at `now`. `now` must be greater than the previous step's.
    void step(SimTime now);

    /// Thread safe.
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    /// What made the host fail (empty while it has not failed). Once failed() returned true,
    /// it is set and no longer changes, so another thread may read it then.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    /// Time of the last step that started, if any.
    [[nodiscard]] std::optional<SimTime> last_step() const noexcept { return last_step_; }

private:
    struct Channel
    {
        Steppable* steppable;
        Port* port;
    };

    void run_step(SimTime now);
    void require_not_started(const char* what) const;

    void run_posted();
    void discard_posted() noexcept;

    std::vector<Channel> channels_;
    Application application_;
    std::optional<TimeDomain> domain_;
    std::optional<TimeDomain> required_domain_;

    mutable std::mutex post_mutex_; ///< guards posted_ and wakeup_
    std::vector<std::function<void()>> posted_;
    std::vector<std::function<void()>> running_; ///< driver's thread only
    Wakeup wakeup_;
    std::optional<SimDuration> step_size_;
    std::optional<SimTime> last_step_;
    bool in_step_ = false;
    std::atomic<bool> failed_{false};
    std::string error_;
};

} // namespace vehicle_sil::can
