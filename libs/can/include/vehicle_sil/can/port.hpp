// SPDX-License-Identifier: MIT
//
// The contract between a protocol stack and a CAN backend (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace vehicle_sil::can {

/// The time base a port's frame times belong to.
enum class TimeDomain
{
    /// Simulation time that only advances step by step (SIL Kit time synchronisation, the
    /// virtual bus). Deterministic.
    Virtual,
    /// Wall-clock time of a real-time driver (SIL Kit without time synchronisation, Vector XL).
    RealTime,
};

/// Synchronous outcome of Port::send().
enum class SendResult
{
    /// The backend took the frame. It may still fail later on the bus; that is not reported
    /// here.
    Accepted,
    /// The backend did not take the frame because its transmit queue is full. The caller may
    /// retry at a later step.
    QueueFull,
    /// The backend cannot send this frame (invalid frame, backend error). Retrying will not help.
    Failed,
};

/// Non-owning reference to a callable object `void(const T&)` (e.g. a lambda), valid for one
/// call.
///
/// Used instead of std::function so that handing values over never allocates. The referenced
/// callable must outlive the SinkRef; pass it directly as an argument.
template <typename T>
class SinkRef
{
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, SinkRef> && std::is_object_v<std::remove_reference_t<F>> &&
                 std::is_invocable_v<F&, const T&>)
    SinkRef(F&& f) noexcept
      : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
        call_([](void* object, const T& value) { (*static_cast<std::remove_reference_t<F>*>(object))(value); })
    {
    }

    void operator()(const T& value) const { call_(object_, value); }

private:
    void* object_;
    void (*call_)(void*, const T&);
};

/// Receives the frames a port releases.
using FrameSink = SinkRef<TimedFrame>;

/// Error state of a CAN controller.
enum class BusState : std::uint8_t
{
    ErrorActive,
    ErrorPassive,
    BusOff,
};

/// Something that happened at the port, reported after the fact: what became of a frame this
/// port sent, and the state of the receive inbox and of the controller.
///
/// Port::send() only gives the synchronous outcome. A backend that learns later whether and when
/// the frame left the bus (a bus model, SIL Kit's transmit acknowledgements) reports it here.
/// A backend that cannot tell a kind reports none of it.
struct PortEvent
{
    enum class Kind : std::uint8_t
    {
        /// The frame was transmitted. `time` is the end of the transmission when the backend
        /// models the bus, otherwise the time the backend acknowledged it.
        TransmitCompleted,
        /// send() accepted the frame, but the backend did not transmit it (its transmit queue
        /// was full, the transmission was cancelled, or the controller is cut off the bus).
        TransmitFailed,
        /// The receive inbox was full and received frames were dropped. `time` is the earliest
        /// time of the dropped frames, `count` how many were dropped until the event was
        /// released (0: the number is not known, at least in part).
        ReceiveOverflow,
        /// The controller's error state changed to `bus_state`.
        BusStateChanged,
        /// An error frame was seen on the bus.
        ErrorFrame,
    };

    Kind kind = Kind::TransmitCompleted;
    SimTime time{};       ///< When it happened, in the port's time domain.
    std::uint32_t id = 0; ///< TransmitCompleted, TransmitFailed: CAN identifier of the frame.
    /// TransmitCompleted, TransmitFailed: TimedFrame::time the frame was sent with, if the
    /// backend knows it (it tells frames with the same identifier apart).
    std::optional<SimTime> send_time;
    std::uint64_t count = 0;                 ///< ReceiveOverflow: frames dropped.
    BusState bus_state = BusState::ErrorActive; ///< BusStateChanged: the new state.

    [[nodiscard]] friend bool operator==(const PortEvent&, const PortEvent&) noexcept = default;
};

/// Receives the events a port releases.
using PortEventSink = SinkRef<PortEvent>;

/// Wakes a real-time driver because a frame or an event arrived with time `arrival` (the
/// driver's next step is later than `arrival`, so the port can release it). Called from any
/// thread, with the port's lock held: it must return quickly and must not call back into the
/// port or the host.
using Wakeup = std::function<void(SimTime arrival)>;

/// One CAN controller as a protocol stack sees it.
///
/// Rules every port follows:
/// - release_before(now, sink) hands over every received frame with time < now, ordered by time
///   and, for equal times, by arrival. It does not reorder frames with equal times: the final
///   order of those is the protocol stack's decision. A frame with time == now is released at a
///   later call.
/// - Frames this port sent itself are not received back (echoes and transmit receipts are
///   dropped in the port). Delivery to other nodes of the same stack is the stack's job.
/// - send() returns the synchronous outcome. What happens to the frame afterwards (transmitted,
///   or dropped by the backend) comes later as a PortEvent, if the backend knows it.
/// - release_events_before(now, sink) hands over the events with time < now, ordered by time and
///   then by arrival, like release_before(). A host calls it before release_before() in the same
///   step.
///
/// - A port of TimeDomain::RealTime stamps what it receives with the arrival time on the
///   driver's clock (RealTimeReceiver) and calls the wakeup set with set_wakeup() for each
///   arrival.
///
/// Threading: release_before(), release_events_before() and send() are called from one thread
/// at a time, the driver's thread. A backend that receives on another thread queues the frames
/// itself. set_wakeup() may be called from any thread.
class Port
{
public:
    virtual ~Port() = default;

    [[nodiscard]] virtual TimeDomain time_domain() const noexcept = 0;

    /// Calls `sink` for every received frame with time < `now` (see above) and removes it.
    /// Returns the number of frames released.
    virtual std::size_t release_before(SimTime now, FrameSink sink) = 0;

    /// Hands `frame` to the backend. `frame.time` is the time of the sending step.
    virtual SendResult send(const TimedFrame& frame) = 0;

    /// Calls `sink` for every event with time < `now` (see above) and removes it. Returns the
    /// number of events released. The default reports no events.
    virtual std::size_t release_events_before(SimTime now, PortEventSink sink)
    {
        static_cast<void>(now);
        static_cast<void>(sink);
        return 0;
    }

    /// Sets (or with an empty function removes) the function a real-time port calls when a
    /// frame or event arrives. After it returns, the previous function is no longer called. A
    /// port of TimeDomain::Virtual ignores it (the default).
    virtual void set_wakeup(Wakeup wakeup) { static_cast<void>(wakeup); }

protected:
    Port() = default;
    Port(const Port&) = default;
    Port& operator=(const Port&) = default;
};

} // namespace vehicle_sil::can
