// SPDX-License-Identifier: MIT
//
// The contract between an Ethernet channel and its backend (vehicle_sil::eth).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/eth/frame.hpp"
#include "vehicle_sil/step/sink_ref.hpp"
#include "vehicle_sil/step/time_domain.hpp"

#include <cstddef>
#include <cstdint>

namespace vehicle_sil::eth {

using TimeDomain = step::TimeDomain;
using Wakeup = step::Wakeup;

/// Synchronous outcome of Port::send().
enum class SendResult : std::uint8_t
{
    /// The backend took the frame. What happens to it afterwards comes as a PortEvent, if the
    /// backend knows it.
    Accepted,
    /// The backend's transmit queue is full; nothing was taken.
    QueueFull,
    /// The backend cannot send this frame (shorter than kHeaderBytes, the port is closed, a
    /// backend error). Retrying will not help.
    Failed,
};

/// Something that happened at the port, reported after the fact.
struct PortEvent
{
    enum class Kind : std::uint8_t
    {
        /// A frame this port sent was transmitted. `time`: when the backend acknowledged it.
        TransmitCompleted,
        /// send() accepted a frame that the backend then did not transmit (e.g. the link was
        /// down). `time`: when the backend reported it.
        TransmitFailed,
        /// The receive inbox was full and received frames were dropped. `time` is the earliest
        /// time of the dropped frames, `count` how many were dropped until the event was
        /// released.
        ReceiveOverflow,
    };

    Kind kind = Kind::TransmitCompleted;
    SimTime time{};
    std::uint64_t count = 0; ///< ReceiveOverflow: frames dropped.

    [[nodiscard]] friend bool operator==(const PortEvent&, const PortEvent&) noexcept = default;
};

/// Receives the frames a port releases.
using FrameSink = step::SinkRef<TimedFrame>;
/// Receives the events a port releases.
using PortEventSink = step::SinkRef<PortEvent>;

/// One Ethernet controller as a Channel sees it.
///
/// Rules every port follows (the same as vehicle_sil::can::Port):
/// - release_before(now, sink) hands over every received frame with time < now, ordered by time
///   and, for equal times, by arrival. It does not reorder frames with equal times (the Channel
///   does). A frame with time == now is released at a later call.
/// - Frames this port sent itself are not received back.
/// - send() returns the synchronous outcome. `frame.time` is the time of the sending step.
/// - release_events_before(now, sink) hands over the events with time < now in the same order;
///   a host calls it before release_before() in the same step.
///
/// Threading: release_before(), release_events_before() and send() are called from one thread
/// at a time, the driver's. A backend that receives on another thread queues the frames itself.
/// set_wakeup() may be called from any thread.
class Port
{
public:
    virtual ~Port() = default;

    [[nodiscard]] virtual TimeDomain time_domain() const noexcept = 0;

    /// Calls `sink` for every received frame with time < `now` (see above) and removes it.
    /// Returns the number of frames released.
    virtual std::size_t release_before(SimTime now, FrameSink sink) = 0;

    /// Hands `frame` to the backend.
    virtual SendResult send(const TimedFrame& frame) = 0;

    /// Calls `sink` for every event with time < `now` and removes it. Returns the number of
    /// events released. The default reports none.
    virtual std::size_t release_events_before(SimTime now, PortEventSink sink)
    {
        static_cast<void>(now);
        static_cast<void>(sink);
        return 0;
    }

    /// Sets (or with an empty function removes) the function a real-time port calls when
    /// something arrives. A port of TimeDomain::Virtual ignores it (the default).
    virtual void set_wakeup(Wakeup wakeup) { static_cast<void>(wakeup); }

protected:
    Port() = default;
    Port(const Port&) = default;
    Port& operator=(const Port&) = default;
};

} // namespace vehicle_sil::eth
