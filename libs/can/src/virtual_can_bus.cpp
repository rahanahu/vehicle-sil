// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::can {

VirtualCanBus::Endpoint::Endpoint(VirtualCanBus& bus, std::size_t index, std::size_t capacity, std::size_t event_capacity,
                                  std::size_t staged_capacity, std::size_t transmit_capacity, std::size_t slot_capacity)
  : bus_(bus),
    index_(index),
    inbox_(capacity),
    events_(event_capacity),
    staged_(staged_capacity),
    transmit_capacity_(transmit_capacity),
    tx_slots_(slot_capacity)
{
    free_slots_.reserve(slot_capacity);
    for (std::size_t i = slot_capacity; i > 0; --i)
    {
        free_slots_.push_back(i - 1);
    }
}

std::size_t VirtualCanBus::Endpoint::allocate_tx_slot(SimTime send_time, std::uint64_t token, bool lost,
                                                      bool correlated) noexcept
{
    const std::size_t slot = free_slots_.back();
    free_slots_.pop_back();
    tx_slots_[slot] = TxSlot{send_time, token, lost, correlated};
    return slot;
}

VirtualCanBus::Endpoint::TxSlot VirtualCanBus::Endpoint::release_tx_slot(std::size_t slot) noexcept
{
    const TxSlot value = tx_slots_[slot];
    free_slots_.push_back(slot);
    return value;
}

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
    return attach(VirtualCanBusEndpointOptions{});
}

VirtualCanBus::Endpoint& VirtualCanBus::attach(VirtualCanBusEndpointOptions options)
{
    const std::size_t index = endpoints_.size();
    const std::size_t inboxCapacity = options.inbox_capacity > 0 ? options.inbox_capacity : options_.inbox_capacity;
    const std::size_t eventCapacity = options.event_capacity > 0 ? options.event_capacity : options_.inbox_capacity;
    // Without timing the transmit queue stays empty (staged_ still needs capacity >= 1 for StepInbox).
    const std::size_t transmitCapacity =
        model_ ? (options.transmit_queue_capacity > 0 ? options.transmit_queue_capacity : model_->queue_capacity())
               : options.transmit_queue_capacity;
    const std::size_t stagedCapacity = model_ ? transmitCapacity : 1;
    const std::size_t slotCapacity = model_ ? transmitCapacity : 0;
    // Endpoint's constructor is private; make_unique cannot reach it.
    endpoints_.push_back(std::unique_ptr<Endpoint>(
        new Endpoint(*this, index, inboxCapacity, eventCapacity, stagedCapacity, transmitCapacity, slotCapacity)));
    if (model_)
    {
        model_->add_sender(transmitCapacity); // index == the endpoint's index
    }
    return *endpoints_.back();
}

SendResult VirtualCanBus::submit(Endpoint& from, const TimedFrame& frame, const FaultAction& action, std::uint64_t token)
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
        from.transmit_failed(frame, token);
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
    if (from.staged_.size() + model_->queued(from.index_) + copies > from.transmit_capacity_)
    {
        return SendResult::QueueFull;
    }
    const bool lost = action.kind == FaultAction::Kind::Drop;
    for (std::size_t i = 0; i < copies; ++i)
    {
        // Only the first copy correlates back to the caller's token; a duplicated copy stays uncorrelated
        // so one Accepted send never produces two correlated completions for the same token.
        const bool correlated = (i == 0);
        from.staged_.push(ready, Endpoint::Staged{frame.frame, frame.time, correlated ? token : 0, lost, correlated});
    }
    ++from.outstanding_;
    return SendResult::Accepted;
}

void VirtualCanBus::hand_over_before(SimTime time)
{
    for (auto& endpoint : endpoints_)
    {
        Endpoint& ep = *endpoint;
        const std::size_t index = ep.index_;
        ep.staged_.drain_before(time, [this, &ep, index](SimTime ready, Endpoint::Staged& staged) {
            const std::size_t slot = ep.allocate_tx_slot(staged.send_time, staged.token, staged.lost, staged.correlated);
            // Cannot be full: send() counts the staged frames against the same capacity.
            static_cast<void>(model_->submit(index, staged.frame, ready, static_cast<std::uint64_t>(slot)));
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
        const Endpoint::TxSlot slot = from.release_tx_slot(static_cast<std::size_t>(tx.context));
        if (!slot.lost)
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
        event.send_time = slot.send_time;
        event.token = slot.token;
        event.correlated = slot.correlated;
        from.push_transmit_event(event);
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

void VirtualCanBus::Endpoint::push_transmit_event(const PortEvent& event)
{
    if (!events_.push(event.time, event))
    {
        if (event.correlated)
        {
            event_overflow_.note(event.time, 1);
        }
    }
}

void VirtualCanBus::Endpoint::transmit_completed(const TimedFrame& frame, std::uint64_t token)
{
    ++outstanding_;
    PortEvent event;
    event.kind = PortEvent::Kind::TransmitCompleted;
    event.time = frame.time;
    event.id = frame.frame.id;
    event.send_time = frame.time;
    event.token = token;
    event.correlated = true;
    push_transmit_event(event);
}

void VirtualCanBus::Endpoint::transmit_failed(const TimedFrame& frame, std::uint64_t token)
{
    ++outstanding_;
    PortEvent event;
    event.kind = PortEvent::Kind::TransmitFailed;
    event.time = frame.time;
    event.id = frame.frame.id;
    event.send_time = frame.time;
    event.token = token;
    event.correlated = true;
    push_transmit_event(event);
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
    return send(frame, 0);
}

SendResult VirtualCanBus::Endpoint::send(const TimedFrame& frame, std::uint64_t token)
{
    if (!is_valid(frame.frame))
    {
        return SendResult::Failed;
    }
    if (cut_off(frame.time, OutageDirection::Send))
    {
        bus_.check_causality(*this, frame);
        ++outage_lost_;
        transmit_failed(frame, token);
        return SendResult::Accepted;
    }
    const FaultAction action = injector_ ? injector_(frame) : FaultAction::deliver();
    if (bus_.model_)
    {
        return bus_.submit(*this, frame, action, token);
    }
    switch (action.kind)
    {
    case FaultAction::Kind::QueueFull:
        return SendResult::QueueFull;
    case FaultAction::Kind::Failed:
        return SendResult::Failed;
    case FaultAction::Kind::Delay:
        if (action.delay <= SimDuration::zero())
        {
            throw std::invalid_argument("vehicle_sil::can::VirtualCanBus: a fault delay must be > 0");
        }
        bus_.check_causality(*this, TimedFrame{frame.time + action.delay, frame.frame});
        break;
    case FaultAction::Kind::TransmitFailed:
        bus_.check_causality(*this, frame);
        transmit_failed(frame, token);
        return SendResult::Accepted;
    case FaultAction::Kind::Deliver:
    case FaultAction::Kind::Duplicate:
        bus_.check_causality(*this, frame);
        break;
    case FaultAction::Kind::Drop:
        break;
    }
    if (transmit_capacity_ > 0 && outstanding_ >= transmit_capacity_)
    {
        return SendResult::QueueFull;
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
        bus_.deliver(*this, TimedFrame{frame.time + action.delay, frame.frame});
        break;
    case FaultAction::Kind::QueueFull:
    case FaultAction::Kind::Failed:
    case FaultAction::Kind::TransmitFailed:
        break;
    }
    transmit_completed(frame, token);
    return SendResult::Accepted;
}

std::size_t VirtualCanBus::Endpoint::release_events_before(SimTime now, PortEventSink sink)
{
    bus_.advance(now);
    const auto release = [this, &sink](const PortEvent& event) {
        if (event.correlated &&
            (event.kind == PortEvent::Kind::TransmitCompleted || event.kind == PortEvent::Kind::TransmitFailed))
        {
            if (outstanding_ > 0)
            {
                --outstanding_;
            }
        }
        else if (event.kind == PortEvent::Kind::EventOverflow)
        {
            const std::size_t dropped = static_cast<std::size_t>(std::min<std::uint64_t>(outstanding_, event.count));
            outstanding_ -= dropped;
        }
        sink(event);
    };
    return OverflowCollector::release_merged(overflow_.take_before(now), release, [&](auto&& forward) {
        return OverflowCollector::release_merged(event_overflow_.take_before(now), forward, [&](auto&& inner) {
            return events_.drain_before(now, [&inner](SimTime, PortEvent& event) { inner(event); });
        });
    });
}

std::size_t VirtualCanBus::Endpoint::transmit_pending() const
{
    return staged_.size() + (bus_.model_ ? bus_.model_->queued(index_) : 0);
}

} // namespace vehicle_sil::can
