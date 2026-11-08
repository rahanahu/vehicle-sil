// SPDX-License-Identifier: MIT
//
// Read-only tap on a port: reports the raw frames and events that pass through it, with their
// times (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/port.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace vehicle_sil::can {

/// One thing that passed through a TapPort.
struct TapRecord
{
    enum class Direction : std::uint8_t
    {
        Received, ///< The port released this frame to the stack (release_before()).
        Sent,     ///< The stack handed this frame to the port (send()); `result` is what send() returned.
        Event,    ///< The port released this event to the stack (release_events_before()).
    };
    Direction direction = Direction::Received;
    /// Received: the frame's time. Sent: TimedFrame::time (the time of the sending step).
    /// Event: PortEvent::time.
    SimTime time{};
    Frame frame{};                          ///< Received, Sent.
    SendResult result = SendResult::Accepted; ///< Sent.
    PortEvent event{};                      ///< Event.
};

class TapPort;

/// Receives the records of one TapPort. It only reads: calling send() or a release function of
/// the TapPort from on_tap() throws std::logic_error. Destroying either side first is safe: the
/// listener detaches itself, and a destroyed TapPort forgets it.
class TapListener
{
public:
    /// Called on the thread that uses the port (the driver's), in the order the frames and
    /// events pass. The record is valid for the call only.
    virtual void on_tap(const TapRecord& record) = 0;

protected:
    TapListener() = default;
    TapListener(const TapListener&) = delete;
    TapListener& operator=(const TapListener&) = delete;
    virtual ~TapListener();

private:
    friend class TapPort;
    TapPort* tap_ = nullptr;
};

/// A Port that forwards everything to `inner` and reports what passes through it to one
/// TapListener: the frames the stack receives (with their times), the frames it sends (with the
/// result of send()) and the port's events. It injects nothing and changes nothing: with or
/// without a listener that does not throw, the stack sees the same. 11-bit and 29-bit frames
/// alike. An exception from the listener propagates to the caller (the host fails): a received
/// frame or event is reported before the stack gets it, so the stack does not get that one.
///
/// The time domain and the wakeup are the inner port's. Use it like the inner port: from the
/// driver's thread, registered with one host. `inner` must outlive the TapPort.
class TapPort final : public Port
{
public:
    explicit TapPort(Port& inner, TapListener* listener = nullptr) noexcept;
    ~TapPort() override;
    TapPort(const TapPort&) = delete;
    TapPort& operator=(const TapPort&) = delete;

    /// Sets the listener (nullptr: none), replacing the previous one. Not from on_tap()
    /// (std::logic_error).
    void set_listener(TapListener* listener);
    [[nodiscard]] TapListener* listener() const noexcept { return listener_; }
    [[nodiscard]] Port& inner() const noexcept { return inner_; }

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return inner_.time_domain(); }
    std::size_t release_before(SimTime now, FrameSink sink) override;
    SendResult send(const TimedFrame& frame) override;
    std::size_t release_events_before(SimTime now, PortEventSink sink) override;
    void set_wakeup(Wakeup wakeup) override { inner_.set_wakeup(std::move(wakeup)); }

private:
    friend class TapListener;
    void notify(const TapRecord& record);
    void require_not_tapping(const char* what) const;

    Port& inner_;
    TapListener* listener_ = nullptr;
    bool tapping_ = false;
};

} // namespace vehicle_sil::can
