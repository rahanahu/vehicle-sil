// SPDX-License-Identifier: MIT
//
// Raw Ethernet frames of an application on one port, sent and received inside the host's steps
// (vehicle_sil::eth).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/eth/frame.hpp"
#include "vehicle_sil/eth/port.hpp"
#include "vehicle_sil/step/host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace vehicle_sil::eth {

/// Which received frames a Channel hands over.
enum class Accept : std::uint8_t
{
    /// Every frame on the network (what a capture in promiscuous mode sees).
    All,
    /// Frames to ChannelOptions::mac, to the broadcast address and to the addresses in
    /// ChannelOptions::multicast.
    Addressed,
};

struct ChannelOptions
{
    /// Frames send() takes in one step (> 0). Further sends in that step return
    /// SendStatus::QueueFull. Allocated once.
    std::size_t send_capacity = 256;
    /// Received frames kept for one step (> 0). Further frames of that step are dropped and
    /// counted (ChannelStatistics::receive_overflows). Allocated once.
    std::size_t receive_capacity = 1024;
    /// Longest frame, in bytes without the frame check sequence, that send() takes and that is
    /// handed over on receiving (longer received frames are dropped and counted). From
    /// kHeaderBytes to Frame::kCapacity.
    std::size_t max_frame_bytes = kDefaultMaxFrameBytes;
    /// The channel's own address. Only Accept::Addressed reads it; send() does not check or
    /// fill in the source address.
    std::optional<MacAddress> mac;
    /// Which received frames are handed over. Accept::Addressed needs `mac`.
    Accept accept = Accept::All;
    /// With Accept::Addressed: further destination addresses that are accepted (multicast
    /// groups).
    std::vector<MacAddress> multicast;
};

/// Outcome of Channel::send().
enum class SendStatus : std::uint8_t
{
    Ok,           ///< Taken; leaves in this step's flush, with the step's time.
    QueueFull,    ///< send_capacity frames were taken in this step already; nothing was taken.
    InvalidFrame, ///< Shorter than kHeaderBytes or longer than max_frame_bytes; nothing was taken.
};

/// Counters of a Channel. They only grow.
struct ChannelStatistics
{
    std::uint64_t frames_received = 0;   ///< Frames handed over (handlers and receive()).
    std::uint64_t frames_taken = 0;      ///< Frames taken with receive().
    std::uint64_t frames_not_taken = 0;  ///< Once receive() was called: frames of a step that
                                         ///< receive() did not return by the end of the step.
    std::uint64_t receive_overflows = 0; ///< Frames dropped because receive_capacity was reached.
    std::uint64_t frames_filtered = 0;   ///< Frames not addressed to the channel (Accept::Addressed).
    std::uint64_t frames_invalid = 0;    ///< Received frames shorter than kHeaderBytes or longer
                                         ///< than max_frame_bytes.
    std::uint64_t frames_sent = 0;       ///< Frames the port accepted.
    std::uint64_t sends_rejected = 0;    ///< send() calls that took nothing (QueueFull, InvalidFrame).
    std::uint64_t frames_dropped = 0;    ///< Frames send() took that the port did not accept.
    std::uint64_t transmit_completed = 0; ///< Port events: frames the backend transmitted.
    std::uint64_t transmit_failed = 0;    ///< Port events: frames the backend did not transmit.
    std::uint64_t port_receive_overflows = 0; ///< Port events: frames the port dropped because
                                              ///< its inbox was full.
    std::uint64_t port_event_overflows = 0; ///< Port events: events the port dropped because its
                                            ///< event queue was full (transmit_completed and
                                            ///< transmit_failed miss them).
};

/// Raw Ethernet frames of an application on one port: no protocol stack, no timers. It is a
/// step::Member: add it to a step::Host (the host steps it, with the other members, in the order
/// they were added).
///
/// One step at `now`:
/// - Receive stage: the frames the port releases before `now` (each with the time the sender
///   sent it), after the filter (Accept) and the length check, in the order of time, then source
///   address (bytes 6..11), then arrival. Ordering equal times by the source address makes the
///   order independent of the backend's threads (SIL Kit), so a run on the virtual bus and on
///   SIL Kit hands over the same frames in the same order.
/// - The handlers (on_frame()) are called in the process stage, for each frame in that order,
///   in the order they were added.
/// - Nothing is allocated in the steps: the storage is allocated in the constructor.
/// - receive() takes the same frames, one by one in the same order, from the process stage
///   until the end of the step (e.g. from the application). What it does not take is gone at the
///   next step; once receive() has been called, those frames are counted (frames_not_taken).
///   Handlers and receive() see the frames independently of each other.
/// - send() is allowed from the start of the step until the flush. The frames leave in this
///   step's flush, in the order of the send() calls, with the time `now`. A frame the port does
///   not accept is dropped and counted (frames_dropped): when the port's queue is full, that
///   frame and the ones after it in this step; when the port fails it, that frame. Nothing is
///   retried.
/// - Frames this channel sent are not received back.
///
/// Set up (on_frame()) before the host starts; afterwards std::logic_error. The step::Member
/// functions are for the host only (private here; call them through step::Member).
///
/// Threading: not thread safe. The host's driver calls the member functions; send(), receive()
/// and the handlers run on its thread. Read statistics() from that thread or after the steps.
class Channel final : public step::Member
{
public:
    using FrameHandler = std::function<void(const TimedFrame& frame)>;

    /// `port` must outlive the channel. std::invalid_argument for a capacity of 0, a
    /// max_frame_bytes outside [kHeaderBytes, Frame::kCapacity], or Accept::Addressed without
    /// a mac.
    explicit Channel(Port& port, ChannelOptions options = {});
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /// Adds `handler` for every received frame (see the class comment). std::invalid_argument for
    /// an empty handler; std::logic_error once the host has started.
    void on_frame(FrameHandler handler);

    /// Takes the frame for this step's flush (see the class comment and SendStatus; a rejected
    /// frame is counted in sends_rejected). std::logic_error outside a step.
    SendStatus send(std::span<const std::uint8_t> bytes);
    SendStatus send(const Frame& frame);

    /// The next frame of this step that receive() has not returned yet; std::nullopt when there
    /// is none (or before the receive stage). std::logic_error outside a step.
    [[nodiscard]] std::optional<TimedFrame> receive();

    [[nodiscard]] const ChannelStatistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] const ChannelOptions& options() const noexcept { return options_; }

    /// The time domain of the port.
    [[nodiscard]] TimeDomain time_domain() const noexcept override { return port_->time_domain(); }

private:
    // step::Member (called by the host).
    void start(std::optional<SimDuration> step_size) override;
    void set_wakeup(Wakeup wakeup) override { port_->set_wakeup(std::move(wakeup)); }
    void begin(SimTime now) override;
    void receive(SimTime now) override;
    void process() override;
    void flush() override;

    enum class Phase : std::uint8_t
    {
        Setup,  ///< before start()
        Idle,   ///< between steps
        InStep, ///< begin() .. flush()
    };

    void take_event(const PortEvent& event) noexcept;
    void take_frame(const TimedFrame& frame);
    [[nodiscard]] bool accepted(const Frame& frame) const noexcept;
    SendStatus queue(std::span<const std::uint8_t> bytes);

    Port* port_;
    ChannelOptions options_;
    std::vector<FrameHandler> handlers_;
    std::vector<TimedFrame> received_;   ///< this step's frames, as they arrived (capacity receive_capacity)
    std::vector<std::uint32_t> order_;   ///< indices into received_, in the order they are handed over
    std::size_t next_taken_ = 0;         ///< order_[next_taken_] is the next for receive()
    bool pulls_ = false;                 ///< receive() has been called
    std::vector<Frame> outgoing_;      ///< this step's sends (capacity send_capacity)
    ChannelStatistics statistics_;
    SimTime now_{};
    Phase phase_ = Phase::Setup;
};

} // namespace vehicle_sil::eth
