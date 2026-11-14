// SPDX-License-Identifier: MIT
//
// Runs the protocol processing of one participant step by step, for any bus (vehicle_sil::step).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/step/time_domain.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::step {

/// One participant of the steps of a Host: the protocol processing of one channel together with
/// the port it receives and sends on (e.g. a CAN Steppable and its Port; vehicle_sil::can::Host
/// makes these from the pairs it is given).
///
/// A Host calls, for each step at time `now`:
///   begin(now), then receive(now), then process(), then (after the application ran) flush().
/// The implementation has no threads and reads no clock: `now` is its only notion of time. It
/// is called from one thread at a time (the driver's), except set_wakeup().
class Member
{
public:
    virtual ~Member() = default;

    /// The time domain of the member's port. Read once, when the member is added to a host.
    [[nodiscard]] virtual TimeDomain time_domain() const noexcept = 0;

    /// Called once by the host before the first step's begin(), with the host's step size if it
    /// is known (Host::set_step_size()). A member checks here whether its protocol can run with
    /// that step size and throws (e.g. std::invalid_argument) if not; the host then fails. The
    /// default accepts any step size.
    virtual void start(std::optional<SimDuration> step_size) { static_cast<void>(step_size); }

    /// Sets (or with an empty function removes) the function a real-time port calls when
    /// something arrives (see Wakeup). After it returns, the previous function is no longer
    /// called. May be called from any thread. The default ignores it (a port of
    /// TimeDomain::Virtual needs none).
    virtual void set_wakeup(Wakeup wakeup) { static_cast<void>(wakeup); }

    /// Starts the step at `now`. `now` is greater than the previous step's.
    virtual void begin(SimTime now) = 0;
    /// Takes from the port what it received before `now` (its events first, then its frames) and
    /// only collects it; process() handles it.
    virtual void receive(SimTime now) = 0;
    /// Handles what was received and the timers that are due.
    virtual void process() = 0;
    /// Sends what the step produced, through the port.
    virtual void flush() = 0;

    /// The earliest deadline of the member's timers: it needs a step after this time even if
    /// nothing arrives. Empty: no timer is running. A real-time driver sleeps until then (or
    /// until something arrives, or at most its idle limit). The default has no timers.
    [[nodiscard]] virtual std::optional<SimTime> next_deadline() const noexcept { return std::nullopt; }

protected:
    Member() = default;
    Member(const Member&) = default;
    Member& operator=(const Member&) = default;
};

/// Steps a fixed set of members of one participant (or one test loop) together.
///
/// Before the first step, the host calls start() of every member in registration order.
/// One step at `now` runs five stages, each over all members in registration order:
///   1. begin(now)   2. receive(now)   3. process()
///   4. the functions passed to post() (in the order they were posted), then the application,
///      once   5. flush()
/// So a receive handler of one member may send on another member of the same host: both are
/// inside the same step and what it sends leaves in stage 5.
///
/// Time domain: all members of a host belong to one TimeDomain, and the driver requires its
/// domain (require_time_domain()): StepDriver and bind_host() Virtual, RealTimeDriver RealTime.
/// Mixing them is a configuration error (std::logic_error), e.g. a Vector XL port in a host that
/// SIL Kit's time synchronisation steps.
///
/// Failure: if step() throws (from a member, the application or a port, or because it was
/// misused), the host becomes failed and rethrows. Every later step() throws std::logic_error.
/// Only a new host recovers.
///
/// The host does not own the members. Destroy them after the host is no longer stepped (stop
/// the driver first).
///
/// Not thread safe, except post() and set_wakeup(): call everything else from the driver's
/// thread.
class Host
{
public:
    using Application = std::function<void(SimTime now)>;

    Host() = default;
    virtual ~Host() = default;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// Registers a member. Only before the first step; a member can be added once; its time
    /// domain must be the one of the members added before and the one the driver requires.
    /// Throws std::logic_error otherwise. A member must not be added to another host as well
    /// (not detected).
    void add(Member& member);

    /// Called by a driver: the members must belong to `domain`. Throws std::logic_error if a
    /// member added before does not, or if another domain was required before; add() checks the
    /// later members. Only before the first step.
    void require_time_domain(TimeDomain domain);

    /// The time domain of the members (or the required one), empty if neither is known.
    [[nodiscard]] std::optional<TimeDomain> time_domain() const noexcept;

    /// Queues `fn` to run on the driver's thread at the start of stage 4 of the next step (in
    /// the order of the post() calls), before the application. Use it to hand work to the
    /// protocol stacks from another thread or from outside the step (e.g. a SIL Kit handler);
    /// the stacks have no locks. Thread safe. A function posted during a step runs in the next
    /// step. If `fn` throws, the step fails like it does for the application. A failed host
    /// discards the functions posted to it. With virtual time, the step a function runs in
    /// depends on when it was posted; data that must be handled deterministically belongs in a
    /// StepInbox.
    void post(std::function<void()> fn);

    /// Sets (or with an empty function removes) the wakeup of a real-time driver: the host
    /// passes it to every member (Member::set_wakeup()) and calls it after post() (with the time
    /// zero). Thread safe.
    void set_wakeup(Wakeup wakeup);

    /// The earliest Member::next_deadline() of the members (empty: none).
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept;

    /// The application's work for each step (stage 4). Optional. Only before the first step.
    void set_application(Application application);

    /// The size of the steps the driver runs (StepDriver::add() and bind_host() set it). Only
    /// before the first step; must be > 0 (std::invalid_argument). Passed to every
    /// Member::start() before the first step.
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

protected:
    /// The checks of add() that do not depend on the member itself: the host has not started
    /// and `domain` fits the members added before and the driver. Throws std::logic_error.
    void check_can_add(TimeDomain domain) const;

private:
    void run_step(SimTime now);
    void require_not_started(const char* what) const;

    void run_posted();
    void discard_posted() noexcept;

    std::vector<Member*> members_;
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

} // namespace vehicle_sil::step
