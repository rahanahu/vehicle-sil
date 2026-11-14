// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_silkit/silkit_eth_port.hpp"

#include <exception>
#include <stdexcept>
#include <string>

namespace vehicle_sil::eth {
namespace {

namespace sk = SilKit::Services::Ethernet;

constexpr const char* kWho = "vehicle_sil::eth::SilKitEthPort: ";

std::size_t checked_capacity(std::size_t capacity, const char* name)
{
    if (capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + name + " must be > 0");
    }
    return capacity;
}

} // namespace

SilKitEthPort::SilKitEthPort(sk::IEthernetController& controller, SilKitEthPortOptions options) :
  controller_(controller),
  inbox_(checked_capacity(options.inbox_capacity, "inbox_capacity")),
  events_(checked_capacity(options.event_capacity, "event_capacity"))
{
    const std::lock_guard lock(lifecycle_mutex_);
    try
    {
        frame_handler_ = controller_.AddFrameHandler(
            [this](sk::IEthernetController*, const sk::EthernetFrameEvent& event) { on_frame(event); },
            static_cast<SilKit::Services::DirectionMask>(SilKit::Services::TransmitDirection::RX));
        transmit_handler_ = controller_.AddFrameTransmitHandler(
            [this](sk::IEthernetController*, const sk::EthernetFrameTransmitEvent& event) { on_transmit(event); },
            SilKit_EthernetTransmitStatus_DefaultMask);
        state_handler_ = controller_.AddStateChangeHandler(
            [this](sk::IEthernetController*, const sk::EthernetStateChangeEvent& event) {
                link_up_.store(event.state == sk::EthernetState::LinkUp, std::memory_order_relaxed);
            });
        controller_.Activate();
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
            if (state_handler_)
            {
                controller_.RemoveStateChangeHandler(*state_handler_);
            }
        }
        catch (...)
        {
        }
        throw;
    }
}

SilKitEthPort::~SilKitEthPort()
{
    close();
}

void SilKitEthPort::close() noexcept
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
        if (state_handler_)
        {
            controller_.RemoveStateChangeHandler(*state_handler_);
        }
    }
    catch (...)
    {
    }
    frame_handler_.reset();
    transmit_handler_.reset();
    state_handler_.reset();
    if (wasOpen)
    {
        try
        {
            controller_.Deactivate();
        }
        catch (...)
        {
        }
    }
    inbox_.clear();
    events_.clear();
    const std::lock_guard overflowLock(overflow_mutex_);
    overflow_.clear();
    event_overflow_.clear();
}

SimTime SilKitEthPort::stamp(std::chrono::nanoseconds timestamp) const noexcept
{
    if (timestamp < std::chrono::nanoseconds::zero())
    {
        return SimTime{} + std::chrono::nanoseconds(step_time_ns_.load(std::memory_order_acquire));
    }
    return from_silkit_time(timestamp);
}

void SilKitEthPort::on_frame(const sk::EthernetFrameEvent& event)
{
    const auto raw = event.frame.raw;
    Frame frame;
    if (!frame.assign(std::span<const std::uint8_t>(raw.data(), raw.size())))
    {
        receive_rejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const SimTime time = stamp(event.timestamp);
    if (inbox_.push(time, frame))
    {
        received_.fetch_add(1, std::memory_order_relaxed);
        if (event.timestamp < std::chrono::nanoseconds::zero())
        {
            received_without_time_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    const std::lock_guard lock(overflow_mutex_);
    overflow_.note(time);
}

void SilKitEthPort::on_transmit(const sk::EthernetFrameTransmitEvent& event) noexcept
{
    PortEvent out;
    out.kind = PortEvent::Kind::TransmitFailed;
    switch (event.status)
    {
    case sk::EthernetTransmitStatus::Transmitted:
        tx_transmitted_.fetch_add(1, std::memory_order_relaxed);
        out.kind = PortEvent::Kind::TransmitCompleted;
        break;
    case sk::EthernetTransmitStatus::ControllerInactive:
        tx_controller_inactive_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::EthernetTransmitStatus::LinkDown:
        tx_link_down_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::EthernetTransmitStatus::Dropped:
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        break;
    case sk::EthernetTransmitStatus::InvalidFrameFormat:
        tx_invalid_frame_.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        return;
    }
    out.time = stamp(event.timestamp);
    if (!events_.push(out.time, out)) // a full queue counts it
    {
        const std::lock_guard lock(overflow_mutex_);
        event_overflow_.note(out.time);
    }
}

std::size_t SilKitEthPort::release_before(SimTime now, FrameSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    // A frame without a time that arrives from here on belongs to this step.
    step_time_ns_.store(to_ns(now), std::memory_order_release);
    return inbox_.drain_before(now, [&sink](SimTime time, Frame& frame) { sink(TimedFrame{time, frame}); });
}

SendResult SilKitEthPort::send(const TimedFrame& frame)
{
    if (!is_open() || frame.frame.size() < kHeaderBytes)
    {
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Failed;
    }
    const auto bytes = frame.frame.bytes();
    try
    {
        controller_.SendFrame(sk::EthernetFrame{SilKit::Util::Span<const std::uint8_t>(bytes.data(), bytes.size())});
    }
    catch (const std::exception&)
    {
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return SendResult::Failed;
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
    return SendResult::Accepted;
}

std::size_t SilKitEthPort::release_events_before(SimTime now, PortEventSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    std::optional<PortEvent> overflow;
    std::optional<PortEvent> eventOverflow;
    {
        const std::lock_guard lock(overflow_mutex_);
        overflow = overflow_.take_before(now);
        eventOverflow = event_overflow_.take_before(now);
    }
    // Both overflow events at their place in time order among the queued events.
    return OverflowCollector::release_merged(overflow, sink, [&](auto&& forward) {
        return OverflowCollector::release_merged(eventOverflow, forward, [&](auto&& inner) {
            return events_.drain_before(now, [&inner](SimTime, PortEvent& event) { inner(event); });
        });
    });
}

SilKitEthPortStatistics SilKitEthPort::statistics() const
{
    SilKitEthPortStatistics s;
    s.received = received_.load(std::memory_order_relaxed);
    s.received_without_time = received_without_time_.load(std::memory_order_relaxed);
    s.receive_rejected = receive_rejected_.load(std::memory_order_relaxed);
    s.receive_overflows = inbox_.overflows();
    s.sent = sent_.load(std::memory_order_relaxed);
    s.send_failed = send_failed_.load(std::memory_order_relaxed);
    s.tx_transmitted = tx_transmitted_.load(std::memory_order_relaxed);
    s.tx_controller_inactive = tx_controller_inactive_.load(std::memory_order_relaxed);
    s.tx_link_down = tx_link_down_.load(std::memory_order_relaxed);
    s.tx_dropped = tx_dropped_.load(std::memory_order_relaxed);
    s.tx_invalid_frame = tx_invalid_frame_.load(std::memory_order_relaxed);
    s.event_overflows = events_.overflows();
    s.link_up = link_up_.load(std::memory_order_relaxed);
    return s;
}

} // namespace vehicle_sil::eth
