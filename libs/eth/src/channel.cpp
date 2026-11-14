// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth/channel.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::eth {

namespace {
constexpr const char* kWho = "vehicle_sil::eth::Channel: ";
} // namespace

Channel::Channel(Port& port, ChannelOptions options) : port_(&port), options_(std::move(options))
{
    if (options_.send_capacity == 0 || options_.receive_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "send_capacity and receive_capacity must be > 0");
    }
    if (options_.max_frame_bytes < kHeaderBytes || options_.max_frame_bytes > Frame::kCapacity)
    {
        throw std::invalid_argument(std::string(kWho) + "max_frame_bytes must be from " + std::to_string(kHeaderBytes) +
                                    " to " + std::to_string(Frame::kCapacity) + ", not " +
                                    std::to_string(options_.max_frame_bytes));
    }
    if (options_.accept == Accept::Addressed && !options_.mac)
    {
        throw std::invalid_argument(std::string(kWho) + "Accept::Addressed needs the channel's own address (mac)");
    }
    received_.reserve(options_.receive_capacity);
    outgoing_.reserve(options_.send_capacity);
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

SendStatus Channel::send(const Frame& frame)
{
    return queue(frame.bytes());
}

SendStatus Channel::send(std::span<const std::uint8_t> bytes)
{
    return queue(bytes);
}

SendStatus Channel::queue(std::span<const std::uint8_t> bytes)
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
    static_cast<void>(outgoing_.back().assign(bytes)); // fits: max_frame_bytes <= Frame::kCapacity
    return SendStatus::Ok;
}

std::optional<TimedFrame> Channel::receive()
{
    if (phase_ != Phase::InStep)
    {
        throw std::logic_error(std::string(kWho) + "receive() is only allowed inside a step (a handler or the application stage)");
    }
    if (next_taken_ >= received_.size())
    {
        return std::nullopt;
    }
    ++statistics_.frames_taken;
    return received_[next_taken_++];
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
    next_taken_ = 0;
    outgoing_.clear();
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

void Channel::take_event(const PortEvent& event) noexcept
{
    switch (event.kind)
    {
    case PortEvent::Kind::TransmitCompleted:
        ++statistics_.transmit_completed;
        break;
    case PortEvent::Kind::TransmitFailed:
        ++statistics_.transmit_failed;
        break;
    case PortEvent::Kind::ReceiveOverflow:
        statistics_.port_receive_overflows += event.count;
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
    // (the sort is stable, so one sender's frames keep their order).
    std::stable_sort(received_.begin(), received_.end(), [](const TimedFrame& a, const TimedFrame& b) {
        if (a.time != b.time)
        {
            return a.time < b.time;
        }
        return a.frame.source() < b.frame.source();
    });
}

void Channel::process()
{
    for (const TimedFrame& frame : received_)
    {
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
    std::size_t i = 0;
    for (; i < outgoing_.size(); ++i)
    {
        const SendResult result = port_->send(TimedFrame{now_, outgoing_[i]});
        if (result == SendResult::Accepted)
        {
            ++statistics_.frames_sent;
        }
        else if (result == SendResult::QueueFull)
        {
            break; // this frame and the rest are dropped (no retry)
        }
        else
        {
            ++statistics_.frames_dropped;
        }
    }
    statistics_.frames_dropped += outgoing_.size() - i;
    outgoing_.clear();
}

} // namespace vehicle_sil::eth
