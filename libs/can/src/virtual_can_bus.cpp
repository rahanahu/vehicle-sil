// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::can {

namespace {

// The bus model's context of a frame: its send time and whether a fault drops it.
std::uint64_t pack_context(SimTime send_time, bool lost) noexcept
{
    return (static_cast<std::uint64_t>(to_ns(send_time)) << 1) | (lost ? 1u : 0u);
}

SimTime unpack_send_time(std::uint64_t context) noexcept
{
    return SimTime{} + std::chrono::nanoseconds(static_cast<std::int64_t>(context >> 1));
}

bool unpack_lost(std::uint64_t context) noexcept
{
    return (context & 1u) != 0;
}

} // namespace

VirtualCanBus::VirtualCanBus(VirtualCanBusOptions options) : options_(options)
{
    if (options_.inbox_capacity == 0)
    {
        throw std::invalid_argument("vehicle_sil::can::VirtualCanBus: inbox_capacity must be > 0");
    }
    if (options_.timing)
    {
        const auto& timing = *options_.timing;
        if (timing.simulator_step < SimDuration::zero())
        {
            throw std::invalid_argument("vehicle_sil::can::VirtualCanBus: simulator_step must be >= 0");
        }
        // CanBusModel checks the bitrate and the queue capacity.
        model_.emplace(CanBusModelOptions{timing.bitrate, timing.transmit_queue_capacity});
    }
}

VirtualCanBus::Endpoint& VirtualCanBus::attach()
{
    const std::size_t index = endpoints_.size();
    // Without timing the transmit queue stays empty.
    const std::size_t transmitCapacity = model_ ? model_->queue_capacity() : 1;
    const std::size_t eventCapacity = options_.inbox_capacity;
    // Endpoint's constructor is private; make_unique cannot reach it.
    endpoints_.push_back(std::unique_ptr<Endpoint>(
        new Endpoint(*this, index, options_.inbox_capacity, eventCapacity, transmitCapacity)));
    if (model_)
    {
        model_->add_sender(); // index == the endpoint's index
    }
    return *endpoints_.back();
}

SendResult VirtualCanBus::submit(Endpoint& from, const TimedFrame& frame, const FaultAction& action)
{
    std::size_t copies = 1;
    SimTime ready = frame.time;
    switch (action.kind)
    {
    case FaultAction::Kind::QueueFull:
        return SendResult::QueueFull;
    case FaultAction::Kind::Failed:
        return SendResult::Failed;
    case FaultAction::Kind::Duplicate:
        copies = 2;
        break;
    case FaultAction::Kind::Delay:
        if (action.delay <= SimDuration::zero())
        {
            throw std::invalid_argument("vehicle_sil::can::VirtualCanBus: a fault delay must be > 0");
        }
        ready = frame.time + action.delay;
        break;
    case FaultAction::Kind::TransmitFailed:
        check_causality(from, frame);
        from.transmit_failed(frame);
        return SendResult::Accepted;
    case FaultAction::Kind::Deliver:
    case FaultAction::Kind::Drop:
        break;
    }
    check_causality(from, frame);
    // The queue holds what the bus has decided so far. A Host releases its port's frames
    // (deciding the bus up to the step time) before it sends, so for hosts this is the queue at
    // the send time. (Not deciding here keeps a frame sent ahead of time, as tests inject them,
    // from freezing the bus up to its time.)
    if (from.staged_.size() + model_->queued(from.index_) + copies > model_->queue_capacity())
    {
        return SendResult::QueueFull;
    }
    const bool lost = action.kind == FaultAction::Kind::Drop;
    for (std::size_t i = 0; i < copies; ++i)
    {
        from.staged_.push(ready, Endpoint::Staged{frame.frame, frame.time, lost});
    }
    return SendResult::Accepted;
}

void VirtualCanBus::hand_over_before(SimTime time)
{
    for (auto& endpoint : endpoints_)
    {
        const std::size_t index = endpoint->index_;
        endpoint->staged_.drain_before(time, [this, index](SimTime ready, Endpoint::Staged& staged) {
            // Cannot be full: send() counts the staged frames against the same capacity.
            static_cast<void>(model_->submit(index, staged.frame, ready, pack_context(staged.send_time, staged.lost)));
        });
    }
}

void VirtualCanBus::advance(SimTime now)
{
    if (!model_)
    {
        return;
    }
    const auto transmitted = [this](const CanBusModel::Transmission& tx) {
        Endpoint& from = *endpoints_[tx.sender];
        if (!unpack_lost(tx.context))
        {
            for (auto& endpoint : endpoints_)
            {
                if (endpoint.get() != &from)
                {
                    endpoint->receive_frame(tx.end, tx.frame);
                }
            }
        }
        PortEvent event;
        event.kind = PortEvent::Kind::TransmitCompleted;
        event.time = tx.end;
        event.id = tx.frame.id;
        event.send_time = unpack_send_time(tx.context);
        from.events_.push(tx.end, event);
    };
    const SimDuration step = options_.timing->simulator_step;
    if (step == SimDuration::zero())
    {
        if (now <= decided_until_)
        {
            return;
        }
        hand_over_before(now);
        model_->run(SimTime{}, now, transmitted);
        decided_until_ = now;
        return;
    }
    while (next_window_ < now)
    {
        const bool idle = model_->queued() == 0 &&
                          std::all_of(endpoints_.begin(), endpoints_.end(), [](const auto& e) { return e->staged_.size() == 0; });
        if (idle)
        {
            // Nothing to decide before `now`: go to the last boundary before it.
            const auto steps = (to_ns(now) - 1) / step.count();
            const SimTime last = SimTime{} + SimDuration{steps * step.count()};
            if (last > next_window_)
            {
                next_window_ = last;
            }
        }
        const SimTime window = next_window_;
        hand_over_before(window);
        model_->run(window, window + step, transmitted);
        decided_until_ = window;
        next_window_ = window + step;
    }
}

void VirtualCanBus::check_causality(const Endpoint& from, const TimedFrame& frame) const
{
    if (model_)
    {
        if (frame.time < decided_until_)
        {
            throw std::logic_error("vehicle_sil::can::VirtualCanBus: frame at t=" + std::to_string(to_ns(frame.time)) +
                                   " ns is older than the decided bus (t=" + std::to_string(to_ns(decided_until_)) +
                                   " ns); step the hosts in time order");
        }
        return;
    }
    for (const auto& endpoint : endpoints_)
    {
        if (endpoint.get() != &from && endpoint->released_before_ && frame.time < *endpoint->released_before_)
        {
            throw std::logic_error("vehicle_sil::can::VirtualCanBus: frame at t=" + std::to_string(to_ns(frame.time)) +
                                   " ns is older than a receiver's last step (t=" +
                                   std::to_string(to_ns(*endpoint->released_before_)) +
                                   " ns); step the hosts in time order");
        }
    }
}

void VirtualCanBus::deliver(const Endpoint& from, const TimedFrame& frame)
{
    check_causality(from, frame);
    for (auto& endpoint : endpoints_)
    {
        if (endpoint.get() != &from)
        {
            endpoint->receive_frame(frame.time, frame.frame);
        }
    }
}

bool VirtualCanBus::Endpoint::cut_off(SimTime time, OutageDirection direction) const noexcept
{
    return std::any_of(outages_.begin(), outages_.end(), [&](const Outage& o) {
        return (o.direction == direction || o.direction == OutageDirection::Both) && o.from <= time && time < o.until;
    });
}

void VirtualCanBus::Endpoint::add_outage(SimTime from, SimTime until, OutageDirection direction)
{
    if (until <= from)
    {
        throw std::invalid_argument("vehicle_sil::can::VirtualCanBus: an outage must end after it starts");
    }
    outages_.push_back(Outage{from, until, direction});
}

void VirtualCanBus::Endpoint::receive_frame(SimTime time, const Frame& frame)
{
    if (cut_off(time, OutageDirection::Receive))
    {
        ++outage_dropped_;
        return;
    }
    if (!inbox_.push(time, frame))
    {
        overflow_.note(time, 1);
    }
}

void VirtualCanBus::Endpoint::transmit_failed(const TimedFrame& frame)
{
    PortEvent event;
    event.kind = PortEvent::Kind::TransmitFailed;
    event.time = frame.time;
    event.id = frame.frame.id;
    event.send_time = frame.time;
    static_cast<void>(events_.push(event.time, event)); // a full queue counts it
}

std::size_t VirtualCanBus::Endpoint::release_before(SimTime now, FrameSink sink)
{
    bus_.advance(now);
    if (!released_before_ || now > *released_before_)
    {
        released_before_ = now;
    }
    return inbox_.drain_before(now, [&sink](SimTime time, Frame& frame) { sink(TimedFrame{time, frame}); });
}

SendResult VirtualCanBus::Endpoint::send(const TimedFrame& frame)
{
    if (!is_valid(frame.frame))
    {
        return SendResult::Failed;
    }
    if (cut_off(frame.time, OutageDirection::Send))
    {
        bus_.check_causality(*this, frame);
        ++outage_lost_;
        transmit_failed(frame);
        return SendResult::Accepted;
    }
    const FaultAction action = injector_ ? injector_(frame) : FaultAction::deliver();
    if (bus_.model_)
    {
        return bus_.submit(*this, frame, action);
    }
    switch (action.kind)
    {
    case FaultAction::Kind::Deliver:
        bus_.deliver(*this, frame);
        break;
    case FaultAction::Kind::Drop:
        break;
    case FaultAction::Kind::Duplicate:
        bus_.deliver(*this, frame);
        bus_.deliver(*this, frame);
        break;
    case FaultAction::Kind::Delay:
        if (action.delay <= SimDuration::zero())
        {
            throw std::invalid_argument("vehicle_sil::can::VirtualCanBus: a fault delay must be > 0");
        }
        bus_.deliver(*this, TimedFrame{frame.time + action.delay, frame.frame});
        break;
    case FaultAction::Kind::QueueFull:
        return SendResult::QueueFull;
    case FaultAction::Kind::Failed:
        return SendResult::Failed;
    case FaultAction::Kind::TransmitFailed:
        bus_.check_causality(*this, frame);
        transmit_failed(frame);
        break;
    }
    return SendResult::Accepted;
}

std::size_t VirtualCanBus::Endpoint::release_events_before(SimTime now, PortEventSink sink)
{
    bus_.advance(now);
    return OverflowCollector::release_merged(overflow_.take_before(now), sink, [&](auto&& forward) {
        return events_.drain_before(now, [&forward](SimTime, PortEvent& event) { forward(event); });
    });
}

std::size_t VirtualCanBus::Endpoint::transmit_pending() const
{
    return staged_.size() + (bus_.model_ ? bus_.model_->queued(index_) : 0);
}

} // namespace vehicle_sil::can
