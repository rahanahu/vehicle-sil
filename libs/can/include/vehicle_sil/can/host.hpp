// SPDX-License-Identifier: MIT
//
// Runs the protocol stacks of one participant step by step (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/step/host.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace vehicle_sil::can {

/// The protocol processing of one CAN channel that a Host steps (e.g. one J1939 network).
///
/// A Host calls, for each step at time `now`:
///   begin(now), then on_port_event() for every event and receive() for every frame the port
///   releases before `now`, then process(), then (after the application ran) flush(port).
/// The implementation has no threads and reads no clock: `now` is its only notion of time. It
/// is called from one thread at a time (the driver's). The contract is described in
/// docs/internals/can.md ("Steppable の契約").
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

/// Steps a fixed set of (Steppable, Port) pairs of one participant (or one test loop) together:
/// a vehicle_sil::step::Host whose members are those pairs.
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
/// Time domain, failure, post(), set_wakeup() and threading are those of step::Host: all ports
/// of a host belong to one TimeDomain, the one the driver requires (std::logic_error otherwise).
///
/// The host does not own the Steppables and ports. Destroy them after the host is no longer
/// stepped (stop the driver first).
class Host : public step::Host
{
public:
    Host();
    ~Host() override;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// Registers a Steppable with the port it sends and receives on. Only before the first step;
    /// a port can belong to one Steppable only; the port's time domain must be the one of the
    /// ports added before and the one the driver requires. Throws std::logic_error otherwise. A
    /// port or Steppable must not be registered with another host as well (not detected). If the
    /// port's set_wakeup() throws (a wakeup is set), the pair is not added and the exception
    /// propagates; the same pair can be added again.
    void add(Steppable& steppable, Port& port);
    /// step::Host::add(Member&): a member that is not a CAN pair (an eth::Channel), stepped with
    /// the pairs in the order all of them were added.
    using step::Host::add;

private:
    class PairMember;

    std::vector<std::unique_ptr<PairMember>> pairs_;
};

} // namespace vehicle_sil::can
