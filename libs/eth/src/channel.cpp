// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth/channel.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::eth {

namespace {
constexpr const char* kWho = "vehicle_sil::eth::Channel: ";

constexpr std::size_t kDefaultTransmitEventCapacity = 1024;

std::size_t saturating_add(std::size_t a, std::size_t b) noexcept
{
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    return a > kMax - b ? kMax : a + b;
}

std::size_t saturating_double(std::size_t value) noexcept
{
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    return value > kMax / 2 ? kMax : value * 2;
}

std::size_t effective_transmit_event_capacity(const ChannelOptions& options) noexcept
{
    if (options.transmit_event_capacity > 0)
    {
        return options.transmit_event_capacity;
    }
    return std::max<std::size_t>({
        kDefaultTransmitEventCapacity,
        saturating_double(options.send_capacity),
        saturating_add(options.port_transmit_queue_capacity, options.send_capacity),
    });
}
} // namespace

void Channel::validate(const ChannelOptions& options)
{
    if (options.send_capacity == 0 || options.receive_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "send_capacity and receive_capacity must be > 0");
    }
    if (options.max_frame_bytes < kHeaderBytes || options.max_frame_bytes > Frame::kCapacity)
    {
        throw std::invalid_argument(std::string(kWho) + "max_frame_bytes must be from " + std::to_string(kHeaderBytes) +
                                    " to " + std::to_string(Frame::kCapacity) + ", not " +
                                    std::to_string(options.max_frame_bytes));
    }
    if (options.accept == Accept::Addressed && !options.mac)
    {
        throw std::invalid_argument(std::string(kWho) + "Accept::Addressed needs the channel's own address (mac)");
    }
    if (options.port_event_capacity > 0 && options.port_transmit_queue_capacity > options.port_event_capacity)
    {
        throw std::invalid_argument(std::string(kWho) + "port_transmit_queue_capacity must be <= port_event_capacity");
    }
}

std::size_t Channel::port_inbox_capacity(const ChannelOptions& options) noexcept
{
    if (options.port_inbox_capacity > 0)
    {
        return options.port_inbox_capacity;
    }
    return saturating_double(options.receive_capacity);
}

std::size_t Channel::port_event_capacity(const ChannelOptions& options) noexcept
{
    if (options.port_event_capacity > 0)
    {
        return options.port_event_capacity;
    }
    return std::max<std::size_t>({
        kDefaultTransmitEventCapacity,
        saturating_double(options.send_capacity),
        effective_transmit_event_capacity(options),
        options.port_transmit_queue_capacity,
    });
}

Channel::Channel(Port& port, ChannelOptions options) :
  port_(&port),
  options_(std::move(options)),
  transmit_event_capacity_(effective_transmit_event_capacity(options_))
{
    validate(options_);
    received_.reserve(options_.receive_capacity);
    order_.reserve(options_.receive_capacity);
    outgoing_.reserve(options_.send_capacity);
    flush_failed_.reserve(options_.send_capacity);
    transmit_events_.reserve(transmit_event_capacity_);
}

void Channel::on_frame(FrameHandler handler)
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

void Channel::on_transmit(TransmitHandler handler)
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

SendStatus Channel::send(const Frame& frame)
{
    return queue(frame.bytes(), 0);
}

SendStatus Channel::send(const Frame& frame, std::uint64_t token)
{
    return queue(frame.bytes(), token);
}

SendStatus Channel::send(std::span<const std::uint8_t> bytes)
{
    return queue(bytes, 0);
}

SendStatus Channel::send(std::span<const std::uint8_t> bytes, std::uint64_t token)
{
    return queue(bytes, token);
}

SendStatus Channel::queue(std::span<const std::uint8_t> bytes, std::uint64_t token)
{
    if (phase_ != Phase::InStep)
    {
        throw std::logic_error(std::string(kWho) +
                               "send() is only allowed inside a step (a handler or the application stage)");
    }
    if (bytes.size() < kHeaderBytes || bytes.size() > options_.max_frame_bytes)
    {
        ++statistics_.sends_rejected;
        return SendStatus::InvalidFrame;
    }
    if (outgoing_.size() >= options_.send_capacity)
    {
        ++statistics_.sends_rejected;
        return SendStatus::QueueFull;
    }
    outgoing_.emplace_back();
    static_cast<void>(outgoing_.back().frame.assign(bytes)); // fits: max_frame_bytes <= Frame::kCapacity
    outgoing_.back().token = token;
    if (port_->transmit_capabilities() == TransmitCapabilities::Correlated)
    {
        ++statistics_.outstanding;
    }
    return SendStatus::Ok;
}

std::optional<TimedFrame> Channel::receive()
{
    if (phase_ != Phase::InStep)
    {
        throw std::logic_error(std::string(kWho) + "receive() is only allowed inside a step (a handler or the application stage)");
    }
    pulls_ = true;
    if (next_taken_ >= order_.size())
    {
        return std::nullopt;
    }
    ++statistics_.frames_taken;
    return received_[order_[next_taken_++]];
}

void Channel::start(std::optional<SimDuration> step_size)
{
    static_cast<void>(step_size);
    if (phase_ == Phase::Setup)
    {
        phase_ = Phase::Idle;
    }
}

void Channel::begin(SimTime now)
{
    if (phase_ == Phase::InStep)
    {
        throw std::logic_error(std::string(kWho) + "begin() before the previous step was flushed");
    }
    phase_ = Phase::InStep;
    now_ = now;
    received_.clear();
    order_.clear();
    next_taken_ = 0;
    outgoing_.clear();
    transmit_events_.clear();
}

bool Channel::accepted(const Frame& frame) const noexcept
{
    if (options_.accept == Accept::All)
    {
        return true;
    }
    const MacAddress destination = frame.destination();
    return destination == *options_.mac || destination.is_broadcast() ||
           std::find(options_.multicast.begin(), options_.multicast.end(), destination) != options_.multicast.end();
}

void Channel::push_transmit_event(const TransmitEvent& event) noexcept
{
    if (transmit_events_.size() >= transmit_event_capacity_)
    {
        ++statistics_.transmit_event_overflows;
        if (statistics_.outstanding > 0)
        {
            --statistics_.outstanding;
        }
        return;
    }
    transmit_events_.push_back(event);
}

void Channel::take_event(const PortEvent& event) noexcept
{
    switch (event.kind)
    {
    case PortEvent::Kind::TransmitCompleted:
        if (!event.correlated)
        {
            if (port_->transmit_capabilities() == TransmitCapabilities::None)
            {
                ++statistics_.transmit_completed;
            }
            break;
        }
        push_transmit_event(TransmitEvent{
            TransmitEvent::Kind::Completed,
            event.token,
            event.send_time.value_or(event.time),
            event.time,
        });
        break;
    case PortEvent::Kind::TransmitFailed:
        if (!event.correlated)
        {
            if (port_->transmit_capabilities() == TransmitCapabilities::None)
            {
                ++statistics_.transmit_failed;
            }
            break;
        }
        push_transmit_event(TransmitEvent{
            TransmitEvent::Kind::Failed,
            event.token,
            event.send_time.value_or(event.time),
            event.time,
        });
        break;
    case PortEvent::Kind::ReceiveOverflow:
        statistics_.port_receive_overflows += event.count;
        break;
    case PortEvent::Kind::EventOverflow:
        statistics_.port_event_overflows += event.count;
        if (port_->transmit_capabilities() == TransmitCapabilities::Correlated)
        {
            const std::uint64_t drop = std::min(statistics_.outstanding, event.count);
            statistics_.outstanding -= drop;
        }
        break;
    }
}

void Channel::take_frame(const TimedFrame& frame)
{
    if (frame.frame.size() < kHeaderBytes || frame.frame.size() > options_.max_frame_bytes)
    {
        ++statistics_.frames_invalid;
        return;
    }
    if (!accepted(frame.frame))
    {
        ++statistics_.frames_filtered;
        return;
    }
    if (received_.size() >= options_.receive_capacity)
    {
        ++statistics_.receive_overflows;
        return;
    }
    received_.push_back(frame);
}

void Channel::receive(SimTime now)
{
    port_->release_events_before(now, [this](const PortEvent& event) { take_event(event); });
    port_->release_before(now, [this](const TimedFrame& frame) { take_frame(frame); });
    // The port releases by time, then arrival. Equal times from different senders arrive in an
    // order that depends on the backend's threads; the source address makes it deterministic
    // (the index is the last key, so one sender's frames keep their order). std::sort on the
    // indices neither allocates nor copies frames (std::stable_sort would allocate).
    for (std::size_t i = 0; i < received_.size(); ++i)
    {
        order_.push_back(static_cast<std::uint32_t>(i));
    }
    std::sort(order_.begin(), order_.end(), [this](std::uint32_t ia, std::uint32_t ib) {
        const TimedFrame& a = received_[ia];
        const TimedFrame& b = received_[ib];
        if (a.time != b.time)
        {
            return a.time < b.time;
        }
        const MacAddress sa = a.frame.source();
        const MacAddress sb = b.frame.source();
        if (sa != sb)
        {
            return sa < sb;
        }
        return ia < ib;
    });
}

void Channel::process()
{
    const auto emitTransmit = [this](const TransmitEvent& event) {
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
    // order on timed backends.
    std::size_t pi = 0;
    std::size_t fi = 0;
    while (pi < transmit_events_.size() || (fi < flush_failed_.size() && flush_failed_[fi].time < now_))
    {
        const bool haveDueFailure = fi < flush_failed_.size() && flush_failed_[fi].time < now_;
        if (pi < transmit_events_.size() && (!haveDueFailure || transmit_events_[pi].time <= flush_failed_[fi].time))
        {
            emitTransmit(transmit_events_[pi++]);
        }
        else
        {
            emitTransmit(flush_failed_[fi++]);
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

    for (const std::uint32_t index : order_)
    {
        const TimedFrame& frame = received_[index];
        ++statistics_.frames_received;
        for (const FrameHandler& handler : handlers_)
        {
            handler(frame);
        }
    }
}

void Channel::flush()
{
    // send() is over for this step; receive() too.
    phase_ = Phase::Idle;
    if (pulls_)
    {
        statistics_.frames_not_taken += order_.size() - next_taken_;
    }
    const bool correlatedPort = port_->transmit_capabilities() == TransmitCapabilities::Correlated;
    auto recordFlushFailed = [this, correlatedPort](std::uint64_t token) {
        ++statistics_.frames_dropped;
        if (!correlatedPort)
        {
            ++statistics_.outstanding;
        }
        if (flush_failed_.size() < options_.send_capacity)
        {
            flush_failed_.push_back(TransmitEvent{TransmitEvent::Kind::Failed, token, now_, now_});
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
        const SendResult result = port_->send(TimedFrame{now_, outgoing_[i].frame}, outgoing_[i].token);
        if (result == SendResult::Accepted)
        {
            ++statistics_.frames_sent;
        }
        else if (result == SendResult::QueueFull)
        {
            for (; i < outgoing_.size(); ++i)
            {
                recordFlushFailed(outgoing_[i].token);
            }
            break; // this frame and the rest are dropped (no retry)
        }
        else
        {
            recordFlushFailed(outgoing_[i].token);
        }
    }
    outgoing_.clear();
}

} // namespace vehicle_sil::eth
