// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/raw_channel.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::can {

namespace {
constexpr const char* kWho = "vehicle_sil::can::RawChannel: ";
} // namespace

RawChannel::RawChannel(RawChannelOptions options) : options_(options)
{
    if (options_.send_capacity == 0 || options_.receive_capacity == 0)
    {
        throw std::invalid_argument(std::string(kWho) + "the capacities must be > 0");
    }
    received_.reserve(options_.receive_capacity);
    outgoing_.reserve(options_.send_capacity);
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

RawSendStatus RawChannel::send(const Frame& frame)
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
    outgoing_.push_back(frame);
    return RawSendStatus::Ok;
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
    if (event.kind == PortEvent::Kind::ReceiveOverflow)
    {
        statistics_.port_receive_overflows += event.count;
    }
}

void RawChannel::process()
{
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
    std::size_t i = 0;
    for (; i < outgoing_.size(); ++i)
    {
        const SendResult result = port.send(TimedFrame{now_, outgoing_[i]});
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

} // namespace vehicle_sil::can
