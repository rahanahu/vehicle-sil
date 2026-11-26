// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/raw_channel.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::can {

namespace {

constexpr const char* kWho = "vehicle_sil::can::RawChannel: ";
constexpr std::size_t kMinEventCapacity = 1024;

std::size_t saturating_add(std::size_t a, std::size_t b) noexcept
{
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    return a > max - b ? max : a + b;
}

std::size_t saturating_mul2(std::size_t a) noexcept
{
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    return a > max / 2 ? max : a * 2;
}

std::size_t effective_transmit_event_capacity(const RawChannelOptions& options) noexcept
{
    if (options.transmit_event_capacity > 0)
    {
        return options.transmit_event_capacity;
    }
    return std::max({kMinEventCapacity, saturating_mul2(options.send_capacity),
                     saturating_add(options.port_transmit_queue_capacity, options.send_capacity)});
}

} // namespace

void RawChannel::validate(const RawChannelOptions& options)
{
    if (options.send_capacity == 0 || options.receive_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "the capacities must be > 0");
    }
    if (options.port_event_capacity > 0 && options.port_transmit_queue_capacity > options.port_event_capacity)
    {
        throw std::invalid_argument(std::string(kWho) +
                                    "port_transmit_queue_capacity must not exceed port_event_capacity");
    }
}

std::size_t RawChannel::port_inbox_capacity(const RawChannelOptions& options) noexcept
{
    return options.port_inbox_capacity > 0 ? options.port_inbox_capacity : saturating_mul2(options.receive_capacity);
}

std::size_t RawChannel::port_event_capacity(const RawChannelOptions& options) noexcept
{
    if (options.port_event_capacity > 0)
    {
        return options.port_event_capacity;
    }
    return std::max({kMinEventCapacity, saturating_mul2(options.send_capacity),
                     effective_transmit_event_capacity(options), options.port_transmit_queue_capacity});
}

RawChannel::RawChannel(RawChannelOptions options) : options_(options)
{
    validate(options_);
    transmit_event_capacity_ = effective_transmit_event_capacity(options_);
    received_.reserve(options_.receive_capacity);
    outgoing_.reserve(options_.send_capacity);
    transmit_events_.reserve(transmit_event_capacity_);
    flush_failed_.reserve(options_.send_capacity);
}

void RawChannel::on_frame(FrameHandler handler)
{
    if (phase_ != Phase::Setup)
    {
        throw std::logic_error(std::string(kWho) + "on_frame() is only allowed before the host starts");
    }
    if (!handler)
    {
        throw std::invalid_argument(std::string(kWho) + "empty handler");
    }
    handlers_.push_back(std::move(handler));
}

void RawChannel::on_transmit(TransmitHandler handler)
{
    if (phase_ != Phase::Setup)
    {
        throw std::logic_error(std::string(kWho) + "on_transmit() is only allowed before the host starts");
    }
    if (!handler)
    {
        throw std::invalid_argument(std::string(kWho) + "empty handler");
    }
    transmit_handlers_.push_back(std::move(handler));
}

RawSendStatus RawChannel::send(const Frame& frame)
{
    return send(frame, 0);
}

RawSendStatus RawChannel::send(const Frame& frame, std::uint64_t token)
{
    if (phase_ != Phase::InStep)
    {
        throw std::logic_error(std::string(kWho) +
                               "send() is only allowed inside a step (a handler or the application stage)");
    }
    if (!is_valid(frame))
    {
        ++statistics_.sends_rejected;
        return RawSendStatus::InvalidFrame;
    }
    if (outgoing_.size() >= options_.send_capacity)
    {
        ++statistics_.sends_rejected;
        return RawSendStatus::QueueFull;
    }
    const bool correlated = port_ != nullptr && port_->transmit_capabilities() == TransmitCapabilities::Correlated;
    if (correlated)
    {
        ++statistics_.outstanding;
    }
    outgoing_.push_back(Outgoing{frame, token, correlated});
    return RawSendStatus::Ok;
}

TransmitCapabilities RawChannel::transmit_capabilities() const noexcept
{
    return port_ != nullptr ? port_->transmit_capabilities() : TransmitCapabilities::None;
}

void RawChannel::bind_port(const Port& port) noexcept
{
    port_ = &port;
}

void RawChannel::start(std::optional<SimDuration> step_size)
{
    static_cast<void>(step_size);
    if (phase_ == Phase::Setup)
    {
        phase_ = Phase::Idle;
    }
}

void RawChannel::begin(SimTime now)
{
    if (phase_ == Phase::InStep)
    {
        throw std::logic_error(std::string(kWho) + "begin() before the previous step was flushed");
    }
    phase_ = Phase::InStep;
    now_ = now;
    received_.clear();
    outgoing_.clear();
    transmit_events_.clear();
}

void RawChannel::receive(const TimedFrame& frame)
{
    if (received_.size() >= options_.receive_capacity)
    {
        ++statistics_.receive_overflows;
        return;
    }
    received_.push_back(frame);
}

void RawChannel::on_port_event(const PortEvent& event)
{
    switch (event.kind)
    {
    case PortEvent::Kind::ReceiveOverflow:
        statistics_.port_receive_overflows += event.count;
        return;
    case PortEvent::Kind::EventOverflow: {
        statistics_.port_event_overflows += event.count;
        if (transmit_capabilities() == TransmitCapabilities::Correlated)
        {
            const std::size_t dropped =
                static_cast<std::size_t>(std::min<std::uint64_t>(statistics_.outstanding, event.count));
            statistics_.outstanding -= dropped;
        }
        return;
    }
    case PortEvent::Kind::TransmitCompleted:
    case PortEvent::Kind::TransmitFailed: {
        if (!event.correlated)
        {
            return;
        }
        if (transmit_events_.size() >= transmit_event_capacity_)
        {
            ++statistics_.transmit_event_overflows;
            if (statistics_.outstanding > 0)
            {
                --statistics_.outstanding;
            }
            return;
        }
        TransmitEvent te;
        te.kind = event.kind == PortEvent::Kind::TransmitCompleted ? TransmitEvent::Kind::Completed
                                                                   : TransmitEvent::Kind::Failed;
        te.token = event.token;
        te.id = event.id;
        te.send_time = event.send_time.value_or(event.time);
        te.time = event.time;
        transmit_events_.push_back(te);
        return;
    }
    case PortEvent::Kind::BusStateChanged:
    case PortEvent::Kind::ErrorFrame:
        return;
    }
}

void RawChannel::process()
{
    const auto emit_transmit = [this](const TransmitEvent& event) {
        if (event.kind == TransmitEvent::Kind::Completed)
        {
            ++statistics_.transmit_completed;
        }
        else
        {
            ++statistics_.transmit_failed;
        }
        if (statistics_.outstanding > 0)
        {
            --statistics_.outstanding;
        }
        for (const TransmitHandler& handler : transmit_handlers_)
        {
            handler(event);
        }
    };

    // Merge previous step's due flush() rejections (fail.time < now_) before the first port event
    // whose time is strictly greater, preserving send order at equal timestamps and monotonic time
    // order on timed buses.
    std::size_t pi = 0;
    std::size_t fi = 0;
    while (pi < transmit_events_.size() || (fi < flush_failed_.size() && flush_failed_[fi].time < now_))
    {
        const bool haveDueFailure = fi < flush_failed_.size() && flush_failed_[fi].time < now_;
        if (pi < transmit_events_.size() && (!haveDueFailure || transmit_events_[pi].time <= flush_failed_[fi].time))
        {
            emit_transmit(transmit_events_[pi++]);
        }
        else
        {
            emit_transmit(flush_failed_[fi++]);
        }
    }
    transmit_events_.clear();
    if (fi >= flush_failed_.size())
    {
        flush_failed_.clear();
    }
    else if (fi > 0)
    {
        flush_failed_.erase(flush_failed_.begin(), flush_failed_.begin() + static_cast<std::ptrdiff_t>(fi));
    }

    for (const TimedFrame& frame : received_)
    {
        ++statistics_.frames_received;
        for (const FrameHandler& handler : handlers_)
        {
            handler(frame);
        }
    }
    received_.clear();
}

void RawChannel::flush(Port& port)
{
    // send() is over for this step: what a port call might trigger cannot add frames.
    phase_ = Phase::Idle;
    if (port_ == nullptr)
    {
        port_ = &port;
    }
    const bool correlated = port_->transmit_capabilities() == TransmitCapabilities::Correlated;
    const auto record_flush_failure = [this](const Outgoing& out) {
        ++statistics_.frames_dropped;
        if (!out.counted_in_outstanding)
        {
            ++statistics_.outstanding;
        }
        if (flush_failed_.size() < options_.send_capacity)
        {
            flush_failed_.push_back(TransmitEvent{TransmitEvent::Kind::Failed, out.token, out.frame.id, now_, now_});
        }
        else
        {
            ++statistics_.transmit_event_overflows;
            if (statistics_.outstanding > 0)
            {
                --statistics_.outstanding;
            }
        }
    };

    std::size_t i = 0;
    for (; i < outgoing_.size(); ++i)
    {
        const Outgoing& out = outgoing_[i];
        const SendResult result = port.send(TimedFrame{now_, out.frame}, out.token);
        if (result == SendResult::Accepted)
        {
            ++statistics_.frames_sent;
            if (correlated && !out.counted_in_outstanding)
            {
                ++statistics_.outstanding;
            }
            else if (!correlated && out.counted_in_outstanding && statistics_.outstanding > 0)
            {
                --statistics_.outstanding;
            }
        }
        else if (result == SendResult::QueueFull)
        {
            break; // this frame and the rest are dropped (no retry)
        }
        else
        {
            record_flush_failure(out);
        }
    }
    for (; i < outgoing_.size(); ++i)
    {
        record_flush_failure(outgoing_[i]);
    }
    outgoing_.clear();
}

} // namespace vehicle_sil::can
