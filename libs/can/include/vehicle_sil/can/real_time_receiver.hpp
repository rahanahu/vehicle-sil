// SPDX-License-Identifier: MIT
//
// Receive side of a real-time port: arrival stamps, inboxes and wakeups (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/overflow_collector.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/core/step_inbox.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vehicle_sil::can {

/// Queues what a real-time backend receives on its own thread, for a Port of
/// TimeDomain::RealTime to release on the driver's thread.
///
/// Arrival stamps: each frame and event is stamped, under a lock, with the clock's time, but
/// at least the `now` of the last release (so a frame never lands in a step that was already
/// handled) and at least 1 ns after the previous stamp (so the order of arrival is kept; the
/// stack does not reorder frames with different times). Frames and events share these stamps.
///
/// Each arrival calls the wakeup (set_wakeup()) with its stamp, under the lock, so that after
/// set_wakeup() returns the old function is not called any more.
///
/// A full frame inbox drops the frame, counts it and reports a PortEvent::ReceiveOverflow
/// (OverflowCollector: the drops until the event is released become one event, which takes no
/// place in the event inbox). A full event inbox drops the event and counts it.
///
/// Thread safety: push_frame(), push_event(), report_overflow(), set_wakeup(), clear() and the
/// counters from any thread; release_before() and release_events_before() from one thread at
/// a time (the driver's).
class RealTimeReceiver
{
public:
    /// `frame_capacity` and `event_capacity` (> 0) are allocated once.
    RealTimeReceiver(const RealTimeClock& clock, std::size_t frame_capacity, std::size_t event_capacity);

    RealTimeReceiver(const RealTimeReceiver&) = delete;
    RealTimeReceiver& operator=(const RealTimeReceiver&) = delete;

    /// Stamps and queues a received frame. Returns false if the inbox was full (see above).
    bool push_frame(const Frame& frame);
    /// Stamps `event` (its time is replaced by the arrival stamp) and queues it, keeping
    /// `reserve` places of the event inbox free (for transmit acknowledgements the port still
    /// expects). Returns false (and counts it) if there was no place.
    bool push_event(PortEvent event, std::size_t reserve = 0);
    /// The backend lost `count` received frames (0: an unknown number) before they reached this
    /// receiver: reported like an overflow of the inbox.
    void report_overflow(std::uint64_t count);

    std::size_t release_before(SimTime now, FrameSink sink);
    std::size_t release_events_before(SimTime now, PortEventSink sink);

    void set_wakeup(Wakeup wakeup);
    /// Discards the queued frames and events (and a pending overflow).
    void clear();

    /// Frames dropped because the inbox was full.
    [[nodiscard]] std::uint64_t overflows() const { return frames_.overflows(); }
    /// Events dropped because the event inbox was full.
    [[nodiscard]] std::uint64_t event_overflows() const;
    /// Events waiting in the event inbox.
    [[nodiscard]] std::size_t pending_events() const { return events_.size(); }
    /// Frames waiting in the inbox.
    [[nodiscard]] std::size_t pending() const { return frames_.size(); }

private:
    SimTime stamp_locked();
    void wake_locked(SimTime time) const;

    const RealTimeClock& clock_;
    mutable std::mutex mutex_;
    std::optional<SimTime> released_;   ///< greatest `now` released
    std::optional<SimTime> last_stamp_; ///< previous arrival stamp
    OverflowCollector overflow_;
    std::uint64_t reserve_refusals_ = 0; ///< events refused to keep the reserve free
    Wakeup wakeup_;
    StepInbox<Frame> frames_;
    StepInbox<PortEvent> events_;
};

} // namespace vehicle_sil::can
