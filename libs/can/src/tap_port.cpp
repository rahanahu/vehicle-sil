// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/tap_port.hpp"

#include <stdexcept>
#include <string>

namespace vehicle_sil::can {

namespace {

/// Sets a flag for the lifetime of the object.
class Flag
{
public:
    explicit Flag(bool& flag) noexcept : flag_(flag) { flag_ = true; }
    ~Flag() { flag_ = false; }
    Flag(const Flag&) = delete;
    Flag& operator=(const Flag&) = delete;

private:
    bool& flag_;
};

} // namespace

TapListener::~TapListener()
{
    if (tap_ != nullptr)
    {
        tap_->listener_ = nullptr;
    }
}

TapPort::TapPort(Port& inner, TapListener* listener) noexcept : inner_(inner)
{
    if (listener != nullptr)
    {
        if (listener->tap_ != nullptr)
        {
            listener->tap_->listener_ = nullptr;
        }
        listener->tap_ = this;
        listener_ = listener;
    }
}

TapPort::~TapPort()
{
    if (listener_ != nullptr)
    {
        listener_->tap_ = nullptr;
    }
}

void TapPort::set_listener(TapListener* listener)
{
    require_not_tapping("set_listener()");
    if (listener_ == listener)
    {
        return;
    }
    if (listener != nullptr && listener_ != nullptr)
    {
        throw std::logic_error("vehicle_sil::can::TapPort::set_listener(): this TapPort already has a TapListener (a "
                               "TapPort has one); read the frames from that listener (one testkit::FrameLog per "
                               "TapPort), or remove it first with set_listener(nullptr)");
    }
    if (listener_ != nullptr)
    {
        listener_->tap_ = nullptr;
    }
    listener_ = listener;
    if (listener_ != nullptr)
    {
        if (listener_->tap_ != nullptr)
        {
            listener_->tap_->listener_ = nullptr;
        }
        listener_->tap_ = this;
    }
}

void TapPort::require_not_tapping(const char* what) const
{
    if (tapping_)
    {
        throw std::logic_error(std::string("vehicle_sil::can::TapPort::") + what +
                               ": a TapListener must not use the port it listens to");
    }
}

void TapPort::notify(const TapRecord& record)
{
    if (listener_ == nullptr)
    {
        return;
    }
    const Flag flag(tapping_);
    listener_->on_tap(record);
}

std::size_t TapPort::release_before(SimTime now, FrameSink sink)
{
    require_not_tapping("release_before()");
    if (listener_ == nullptr)
    {
        return inner_.release_before(now, sink);
    }
    return inner_.release_before(now, [this, &sink](const TimedFrame& frame) {
        TapRecord record;
        record.direction = TapRecord::Direction::Received;
        record.time = frame.time;
        record.frame = frame.frame;
        notify(record);
        sink(frame);
    });
}

SendResult TapPort::send(const TimedFrame& frame)
{
    return send(frame, 0);
}

SendResult TapPort::send(const TimedFrame& frame, std::uint64_t token)
{
    require_not_tapping("send()");
    const SendResult result = inner_.send(frame, token);
    if (listener_ != nullptr)
    {
        TapRecord record;
        record.direction = TapRecord::Direction::Sent;
        record.time = frame.time;
        record.frame = frame.frame;
        record.result = result;
        record.token = token;
        notify(record);
    }
    return result;
}

std::size_t TapPort::release_events_before(SimTime now, PortEventSink sink)
{
    require_not_tapping("release_events_before()");
    if (listener_ == nullptr)
    {
        return inner_.release_events_before(now, sink);
    }
    return inner_.release_events_before(now, [this, &sink](const PortEvent& event) {
        TapRecord record;
        record.direction = TapRecord::Direction::Event;
        record.time = event.time;
        record.event = event;
        notify(record);
        sink(event);
    });
}

} // namespace vehicle_sil::can
