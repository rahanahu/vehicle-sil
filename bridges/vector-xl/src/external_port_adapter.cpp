// SPDX-License-Identifier: MIT
#include "vehicle_sil/vector_xl/external_port_adapter.hpp"

#include "vehicle_sil/can_silkit/silkit_can_port.hpp"

#include <utility>

namespace vehicle_sil::vector_xl {

ExternalPortAdapter::ExternalPortAdapter(ExternalCanPort& external, const can::RealTimeClock& clock,
                                         ExternalPortAdapterOptions options)
  : external_(external), receiver_(clock, options.inbox_capacity, options.event_capacity)
{
    const std::lock_guard lock(lifecycle_mutex_);
    external_.start([this](const ExternalCanEvent& event) { on_event(event); });
    open_.store(true, std::memory_order_release);
}

ExternalPortAdapter::~ExternalPortAdapter()
{
    close();
}

void ExternalPortAdapter::close() noexcept
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (!open_.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    external_.stop(); // the handler is no longer running or called afterwards
    receiver_.set_wakeup({});
    receiver_.clear();
}

void ExternalPortAdapter::on_event(const ExternalCanEvent& event)
{
    switch (event.kind)
    {
    case ExternalEventKind::Frame:
    {
        const auto frame = can::from_silkit_frame(event.frame);
        if (!frame)
        {
            receive_rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (receiver_.push_frame(*frame))
        {
            received_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    case ExternalEventKind::OwnTransmission:
        own_transmissions_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    case ExternalEventKind::ErrorFrame:
    {
        error_frames_.fetch_add(1, std::memory_order_relaxed);
        can::PortEvent out;
        out.kind = can::PortEvent::Kind::ErrorFrame;
        static_cast<void>(receiver_.push_event(out)); // a full queue counts it
        return;
    }
    case ExternalEventKind::Overrun:
        external_overruns_.fetch_add(1, std::memory_order_relaxed);
        receiver_.report_overflow(0);
        return;
    case ExternalEventKind::BusState:
    case ExternalEventKind::Fault:
    case ExternalEventKind::Warning:
        // Not passed on: the adapter's users read the bus through frames and error frames only.
        return;
    }
}

std::size_t ExternalPortAdapter::release_before(SimTime now, can::FrameSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    return receiver_.release_before(now, sink);
}

std::size_t ExternalPortAdapter::release_events_before(SimTime now, can::PortEventSink sink)
{
    if (!is_open())
    {
        return 0;
    }
    return receiver_.release_events_before(now, sink);
}

can::SendResult ExternalPortAdapter::send(const can::TimedFrame& frame)
{
    SilKit::Services::Can::CanFrame out;
    // Held across the check and the transmit: once close() (another thread) returns, no frame
    // reaches the external port any more.
    const std::lock_guard lock(lifecycle_mutex_);
    if (!is_open() || !can::to_silkit_frame(frame.frame, out))
    {
        send_failed_.fetch_add(1, std::memory_order_relaxed);
        return can::SendResult::Failed;
    }
    switch (external_.transmit(out))
    {
    case TransmitResult::Ok:
        sent_.fetch_add(1, std::memory_order_relaxed);
        return can::SendResult::Accepted;
    case TransmitResult::QueueFull:
        send_queue_full_.fetch_add(1, std::memory_order_relaxed);
        return can::SendResult::QueueFull;
    case TransmitResult::Failed:
        break;
    }
    send_failed_.fetch_add(1, std::memory_order_relaxed);
    return can::SendResult::Failed;
}

void ExternalPortAdapter::set_wakeup(can::Wakeup wakeup)
{
    receiver_.set_wakeup(std::move(wakeup));
}

ExternalPortAdapterStatistics ExternalPortAdapter::statistics() const
{
    ExternalPortAdapterStatistics s;
    s.received = received_.load(std::memory_order_relaxed);
    s.receive_rejected = receive_rejected_.load(std::memory_order_relaxed);
    s.receive_overflows = receiver_.overflows();
    s.own_transmissions_dropped = own_transmissions_dropped_.load(std::memory_order_relaxed);
    s.error_frames = error_frames_.load(std::memory_order_relaxed);
    s.external_overruns = external_overruns_.load(std::memory_order_relaxed);
    s.sent = sent_.load(std::memory_order_relaxed);
    s.send_queue_full = send_queue_full_.load(std::memory_order_relaxed);
    s.send_failed = send_failed_.load(std::memory_order_relaxed);
    s.event_overflows = receiver_.event_overflows();
    return s;
}

} // namespace vehicle_sil::vector_xl
