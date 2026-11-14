// SPDX-License-Identifier: MIT
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"

#include "vehicle_sil/can_silkit/send_time_tag.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::can {
namespace {

namespace sk = SilKit::Services::Can;

constexpr sk::CanFrameFlagMask flag(sk::CanFrameFlag f) noexcept
{
    return static_cast<sk::CanFrameFlagMask>(f);
}

constexpr sk::CanFrameFlagMask kUnsupportedFlags = flag(sk::CanFrameFlag::Rtr) | flag(sk::CanFrameFlag::Fdf) |
                                                   flag(sk::CanFrameFlag::Xlf);

} // namespace

std::optional<Frame> from_silkit_frame(const sk::CanFrame& in) noexcept
{
    if ((in.flags & kUnsupportedFlags) != 0)
    {
        return std::nullopt;
    }
    const std::size_t length = in.dataField.size();
    if (length > kMaxPayload)
    {
        return std::nullopt;
    }
    // Classic CAN: DLC 0..8 is the byte count, DLC 9..15 also mean 8 bytes.
    const bool dlcMatches = (in.dlc == length) || ((length == kMaxPayload) && (in.dlc > 8) && (in.dlc <= 15));
    if (!dlcMatches)
    {
        return std::nullopt;
    }
    Frame out;
    out.id = in.canId;
    out.format = (in.flags & flag(sk::CanFrameFlag::Ide)) != 0 ? IdFormat::Extended : IdFormat::Standard;
    out.length = static_cast<std::uint8_t>(length);
    std::copy_n(in.dataField.begin(), length, out.data.begin());
    if (!is_valid(out))
    {
        return std::nullopt;
    }
    return out;
}

bool to_silkit_frame(const Frame& in, sk::CanFrame& out) noexcept
{
    if (!is_valid(in))
    {
        return false;
    }
    out = sk::CanFrame{};
    out.canId = in.id;
    out.flags = in.format == IdFormat::Extended ? flag(sk::CanFrameFlag::Ide) : sk::CanFrameFlagMask{0};
    out.dlc = in.length;
    out.dataField = SilKit::Util::Span<const std::uint8_t>(in.data.data(), in.length);
    return true;
}

SilKitCanPort::SilKitCanPort(sk::ICanController& controller, SilKitCanPortOptions options) :
  controller_(controller),
  // StepInbox rejects 0 (std::invalid_argument), like VirtualCanBus. With real time, the
  // RealTimeReceiver holds the frames and events instead.
  inbox_(options.real_time_clock != nullptr && options.inbox_capacity > 0 ? 1 : options.inbox_capacity),
  events_(options.real_time_clock != nullptr && options.event_capacity > 0 ? 1 : options.event_capacity),
  transmit_capacity_(options.transmit_queue_capacity)
{
    if (options.real_time_clock != nullptr)
    {
        real_time_.emplace(*options.real_time_clock, options.inbox_capacity, options.event_capacity);
    }
    if (transmit_capacity_ > options.event_capacity)
    {
        // An acknowledgement lost to a full event queue would keep its place for good.
        throw std::invalid_argument(
            "vehicle_sil::can::SilKitCanPort: transmit_queue_capacity must not exceed event_capacity");
    }
    const std::lock_guard lock(lifecycle_mutex_);
    try
    {
        if (options.bitrate > 0)
        {
            controller_.SetBaudRate(options.bitrate, 0, 0);
        }
        frame_handler_ = controller_.AddFrameHandler(
            [this](sk::ICanController*, const sk::CanFrameEvent& event) { on_frame(event); },
            static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));
        constexpr auto kAll = static_cast<sk::CanTransmitStatusMask>(sk::CanTransmitStatus::Transmitted) |
                              static_cast<sk::CanTransmitStatusMask>(sk::CanTransmitStatus::Canceled) |
                              static_cast<sk::CanTransmitStatusMask>(sk::CanTransmitStatus::TransmitQueueFull);
        transmit_handler_ = controller_.AddFrameTransmitHandler(
            [this](sk::ICanController*, const sk::CanFrameTransmitEvent& event) { on_transmit(event); }, kAll);
        error_state_handler_ = controller_.AddErrorStateChangeHandler(
            [this](sk::ICanController*, const sk::CanErrorStateChangeEvent& event) { on_error_state(event); });
        controller_.Start();
        open_.store(true, std::memory_order_release);
    }
    catch (...)
    {
        // Leave nothing behind that could call into a half-built object.
        try
        {
            if (frame_handler_)
            {
                controller_.RemoveFrameHandler(*frame_handler_);
            }
            if (transmit_handler_)
            {
                controller_.RemoveFrameTransmitHandler(*transmit_handler_);
            }
            if (error_state_handler_)
            {
                controller_.RemoveErrorStateChangeHandler(*error_state_handler_);
            }
        }
        catch (...)
        {
        }
        throw;
    }
}

SilKitCanPort::~SilKitCanPort()
{
    close();
}

void SilKitCanPort::close() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    const bool wasOpen = open_.exchange(false, std::memory_order_acq_rel);
    // RemoveFrameHandler waits for a running handler call to finish, so afterwards SIL Kit no
    // longer touches `this`.
    try
    {
        if (frame_handler_)
        {
            controller_.RemoveFrameHandler(*frame_handler_);
        }
    }
    catch (...)
    {
    }
    try
    {
        if (transmit_handler_)
        {
            controller_.RemoveFrameTransmitHandler(*transmit_handler_);
        }
    }
    catch (...)
    {
    }
    try
    {
        if (error_state_handler_)
        {
            controller_.RemoveErrorStateChangeHandler(*error_state_handler_);
        }
    }
    catch (...)
    {
    }
    frame_handler_.reset();
    transmit_handler_.reset();
    error_state_handler_.reset();
    if (wasOpen)
    {
        try
        {
            controller_.Stop();
        }
        catch (...)
        {
        }
    }
    inbox_.clear();
    events_.clear();
    {
        const std::lock_guard overflowLock(overflow_mutex_);
        overflow_.clear();
    }
    if (real_time_)
    {
        real_time_->set_wakeup({});
        real_time_->clear();
    }
}

void SilKitCanPort::set_wakeup(Wakeup wakeup)
{
    if (real_time_)
    {
        real_time_->set_wakeup(std::move(wakeup));
    }
}

void SilKitCanPort::push_event(const PortEvent& event, bool acknowledgement) noexcept
{
    // Every outstanding frame will bring an acknowledgement: keep a place for each of them
    // (acknowledgements already queued are counted twice, which only errs on the safe side).
    const std::size_t reserve = acknowledgement ? 0 : outstanding_.load(std::memory_order_relaxed);
    if (real_time_)
    {
        static_cast<void>(real_time_->push_event(event, reserve)); // counts a refusal or a full queue
        return;
    }
    if (!acknowledgement && events_.size() + reserve >= events_.capacity())
    {
        event_refusals_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static_cast<void>(events_.push(event.time, event)); // a full queue counts it
}

void SilKitCanPort::on_error_state(const sk::CanErrorStateChangeEvent& event) noexcept
{
    PortEvent out;
    out.kind = PortEvent::Kind::BusStateChanged;
    switch (event.errorState)
    {
    case sk::CanErrorState::ErrorActive:
        out.bus_state = BusState::ErrorActive;
        break;
    case sk::CanErrorState::ErrorPassive:
        out.bus_state = BusState::ErrorPassive;
        break;
    case sk::CanErrorState::BusOff:
        out.bus_state = BusState::BusOff;
        break;
    default:
        return; // NotAvailable: the controller is not initialised; nothing to report
    }
    error_state_changes_.fetch_add(1, std::memory_order_relaxed);
    out.time = from_silkit_time(event.timestamp);
    push_event(out, false);
}

void SilKitCanPort::on_frame(const sk::CanFrameEvent& event)
{
    const auto frame = from_silkit_frame(event.frame);
    if (!frame)
    {
        receive_rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (real_time_)
    {
        if (real_time_->push_frame(*frame))
        {
            received_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const SimTime time = from_silkit_time(event.timestamp);
    if (inbox_.push(time, *frame))
    {
        received_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::lock_guard lock(overflow_mutex_);
    overflow_.note(time, 1);
}

void SilKitCanPort::on_transmit(const sk::CanFrameTransmitEvent& event) noexcept
{
    PortEvent out;
    out.kind = PortEvent::Kind::TransmitFailed;
    switch (event.status)
    {
    case sk::CanTransmitStatus::Transmitted:
        tx_transmitted_.fetch_add(1, std::memory_order_relaxed);
        out.kind = PortEvent::Kind::TransmitCompleted;
        break;
    case sk::CanTransmitStatus::Canceled:
        tx_canceled_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::CanTransmitStatus::TransmitQueueFull:
        tx_queue_full_.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        return;
    }
    out.time = from_silkit_time(event.timestamp);
    out.id = event.canId;
    out.send_time = decode_send_time(event.userContext);
    push_event(out, true);
}

std::size_t SilKitCanPort::release_before(SimTime now, FrameSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    if (real_time_)
    {
        return real_time_->release_before(now, sink);
    }
    return inbox_.drain_before(now, [&sink](SimTime time, Frame& frame) { sink(TimedFrame{time, frame}); });
}

SendResult SilKitCanPort::send(const TimedFrame& frame)
{
    sk::CanFrame out;
    if (!is_open() || !to_silkit_frame(frame.frame, out))
    {
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Failed;
    }
    if (transmit_capacity_ > 0 && outstanding_.load(std::memory_order_relaxed) >= transmit_capacity_)
    {
        send_queue_full_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::QueueFull;
    }
    try
    {
        controller_.SendFrame(out, real_time_ ? nullptr : encode_send_time(frame.time));
    }
    catch (const std::exception&)
    {
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Failed;
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    return SendResult::Accepted;
}

std::size_t SilKitCanPort::release_events_before(SimTime now, PortEventSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    const auto release = [this, &sink](const PortEvent& event) {
        const bool transmit =
            event.kind == PortEvent::Kind::TransmitCompleted || event.kind == PortEvent::Kind::TransmitFailed;
        if (transmit && outstanding_.load(std::memory_order_relaxed) > 0)
        {
            outstanding_.fetch_sub(1, std::memory_order_relaxed); // ends one frame this port sent
        }
        sink(event);
    };
    if (real_time_)
    {
        return real_time_->release_events_before(now, release);
    }
    std::optional<PortEvent> overflow;
    {
        const std::lock_guard lock(overflow_mutex_);
        overflow = overflow_.take_before(now);
    }
    return OverflowCollector::release_merged(overflow, release, [&](auto&& forward) {
        return events_.drain_before(now, [&forward](SimTime, PortEvent& event) { forward(event); });
    });
}

SilKitCanPortStatistics SilKitCanPort::statistics() const
{
    SilKitCanPortStatistics s;
    s.received = received_.load(std::memory_order_relaxed);
    s.receive_rejected = receive_rejected_.load(std::memory_order_relaxed);
    s.receive_overflows = real_time_ ? real_time_->overflows() : inbox_.overflows();
    s.error_state_changes = error_state_changes_.load(std::memory_order_relaxed);
    s.sent = sent_.load(std::memory_order_relaxed);
    s.send_failed = send_failed_.load(std::memory_order_relaxed);
    s.send_queue_full = send_queue_full_.load(std::memory_order_relaxed);
    s.outstanding = outstanding_.load(std::memory_order_relaxed);
    s.tx_transmitted = tx_transmitted_.load(std::memory_order_relaxed);
    s.tx_canceled = tx_canceled_.load(std::memory_order_relaxed);
    s.tx_queue_full = tx_queue_full_.load(std::memory_order_relaxed);
    s.event_overflows =
        real_time_ ? real_time_->event_overflows() : events_.overflows() + event_refusals_.load(std::memory_order_relaxed);
    return s;
}

} // namespace vehicle_sil::can
