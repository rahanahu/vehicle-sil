// SPDX-License-Identifier: MIT
//
// Raw CAN frames of an application on one port, sent and received inside the host's steps
// (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/frame.hpp"
#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/port.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace vehicle_sil::can {

struct RawChannelOptions
{
    /// Frames send() takes in one step (> 0). Further sends in that step return
    /// RawSendStatus::QueueFull. Allocated once.
    std::size_t send_capacity = 64;
    /// Received frames kept for the handlers in one step (> 0). Further frames of that step are
    /// dropped and counted (RawChannelStatistics::receive_overflows). Allocated once.
    std::size_t receive_capacity = 1024;
};

/// Outcome of RawChannel::send().
enum class RawSendStatus : std::uint8_t
{
    Ok,           ///< Taken; leaves in this step's flush, with the step's time.
    QueueFull,    ///< send_capacity frames were taken in this step already; nothing was taken.
    InvalidFrame, ///< The frame is not valid (can::is_valid()); nothing was taken.
};

/// Counters of a RawChannel.
struct RawChannelStatistics
{
    std::uint64_t frames_received = 0;        ///< Frames handed to the handlers.
    std::uint64_t receive_overflows = 0;      ///< Frames dropped because receive_capacity was reached.
    std::uint64_t frames_sent = 0;            ///< Frames the port accepted.
    std::uint64_t sends_rejected = 0;         ///< send() calls that took nothing (QueueFull, InvalidFrame).
    std::uint64_t frames_dropped = 0;         ///< Frames send() took that the port did not accept.
    std::uint64_t port_receive_overflows = 0; ///< Port events: frames the port dropped because its
                                              ///< inbox was full (as far as the port knows the number).
};

/// Raw CAN frames (11 and 29 bit) of an application on one port: no protocol, no timers. A Host
/// steps it like any other Steppable; Ecu::can() makes one on its own port.
///
/// One step at `now`:
/// - The handlers (on_frame()) are called in process() for every frame the port released before
///   `now`, in the order the port released them and with the frame's time (see Port: ordered by
///   time; frames with equal times in the order they arrived, which on SIL Kit depends on the
///   other participants' threads when they come from different senders). Each frame goes to the
///   handlers in the order they were added. Frames this channel sent are not received back.
/// - send() is allowed from begin() until the flush: from a handler of this or another Steppable
///   of the same host, or from the application. The frames leave in this step's flush, in the
///   order of the send() calls, with the time `now`. A frame the port does not accept is dropped
///   and counted (frames_dropped): when the port's queue is full, that frame and the ones after
///   it in this step; when the port fails it, that frame. Nothing is retried.
///
/// Set up (on_frame()) before the host starts (Steppable::start()); afterwards std::logic_error.
///
/// Threading: not thread safe. The host's driver calls the Steppable functions; send() and the
/// handlers run on its thread. Read statistics() from that thread or after the steps ended.
class RawChannel final : public Steppable
{
public:
    using FrameHandler = std::function<void(const TimedFrame& frame)>;

    /// std::invalid_argument if a capacity is 0.
    explicit RawChannel(RawChannelOptions options = {});
    RawChannel(const RawChannel&) = delete;
    RawChannel& operator=(const RawChannel&) = delete;

    /// Adds `handler` for every received frame (see the class comment). std::invalid_argument for
    /// an empty handler; std::logic_error once the host has started.
    void on_frame(FrameHandler handler);

    /// Takes `frame` for this step's flush (see the class comment and RawSendStatus; a rejected
    /// frame is counted in sends_rejected). std::logic_error outside a step (before begin() or
    /// from the flush on), e.g. from another thread's code or between runs.
    RawSendStatus send(const Frame& frame);

    [[nodiscard]] const RawChannelStatistics& statistics() const noexcept { return statistics_; }

    // Steppable (called by the host).
    void start(std::optional<SimDuration> step_size) override;
    void begin(SimTime now) override;
    void receive(const TimedFrame& frame) override;
    void on_port_event(const PortEvent& event) override;
    void process() override;
    void flush(Port& port) override;

private:
    enum class Phase : std::uint8_t
    {
        Setup,  ///< before start()
        Idle,   ///< between steps
        InStep, ///< begin() .. flush()
    };

    RawChannelOptions options_;
    std::vector<FrameHandler> handlers_;
    std::vector<TimedFrame> received_; ///< this step's frames (capacity receive_capacity)
    std::vector<Frame> outgoing_;      ///< this step's sends (capacity send_capacity)
    RawChannelStatistics statistics_;
    SimTime now_{};
    Phase phase_ = Phase::Setup;
};

} // namespace vehicle_sil::can
