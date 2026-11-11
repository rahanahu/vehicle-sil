// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/toolbox.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::j1939 {

Subscription::Subscription(Node& node, HandlerId id) noexcept
  : node_(&node), id_(id), alive_(node.network().alive_)
{
}

Subscription::Subscription(Subscription&& other) noexcept
  : node_(std::exchange(other.node_, nullptr)), id_(std::exchange(other.id_, HandlerId{})), alive_(std::move(other.alive_))
{
    other.alive_.reset();
}

Subscription& Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other)
    {
        reset();
        node_ = std::exchange(other.node_, nullptr);
        id_ = std::exchange(other.id_, HandlerId{});
        alive_ = std::move(other.alive_);
        other.alive_.reset();
    }
    return *this;
}

void Subscription::reset() noexcept
{
    Node* node = std::exchange(node_, nullptr);
    if (node != nullptr && !alive_.expired())
    {
        static_cast<void>(node->remove_handler(id_));
    }
    id_ = HandlerId{};
    alive_.reset();
}

Timer::Timer(Network& network, TimerKind kind, Callback callback)
  : network_(&network), alive_(network.alive_), kind_(kind), callback_(std::move(callback))
{
    if (!callback_)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Timer: empty callback");
    }
    network.require_not_observing("creating a Timer");
    if (!network.timers_.reserve(1))
    {
        throw std::length_error("vehicle_sil::j1939::Timer: no timer left: all " + std::to_string(network.timers_.reservable()) +
                                " of NetworkOptions::timer_capacity are taken (2 per node with a NAME, 1 per Timer); raise "
                                "timer_capacity, or check Network::timers_free() before creating a part in a handler");
    }
}

Timer::~Timer()
{
    if (destroyed_ != nullptr)
    {
        *destroyed_ = true;
    }
    if (!alive_.expired())
    {
        static_cast<void>(network_->fail_if_observing("destroying a Timer"));
        network_->timers_.cancel(id_);
        network_->timers_.release(1);
    }
}

void Timer::start_at(SimTime deadline)
{
    if (alive_.expired())
    {
        throw std::logic_error("vehicle_sil::j1939::Timer: started after its network was destroyed");
    }
    network_->require_not_observing("Timer::start_at()");
    network_->timers_.cancel(id_);
    // Cannot run out: the slot was reserved when the Timer was created.
    id_ = network_->timers_.start(kind_, deadline, *this, 0);
    deadline_ = deadline;
}

void Timer::start_after(SimDuration delay)
{
    if (alive_.expired())
    {
        throw std::logic_error("vehicle_sil::j1939::Timer: started after its network was destroyed");
    }
    start_at(network_->event_time() + delay);
}

bool Timer::cancel() noexcept
{
    if (alive_.expired() || network_->fail_if_observing("Timer::cancel()"))
    {
        return false;
    }
    return network_->timers_.cancel(id_);
}

bool Timer::running() const noexcept
{
    return !alive_.expired() && network_->timers_.is_running(id_);
}

std::optional<SimTime> Timer::deadline() const noexcept
{
    if (destroyed_ == nullptr && !running())
    {
        return std::nullopt;
    }
    return deadline_;
}

void Timer::on_timer(std::uint32_t)
{
    // The callback runs from a local: it may destroy this Timer (or restart it) safely.
    Network& network = *network_;
    const std::optional<SimTime> outerTime = network.event_time_;
    network.event_time_ = deadline_;
    Callback callback = std::move(callback_);
    bool destroyed = false;
    destroyed_ = &destroyed;
    try
    {
        callback();
    }
    catch (...)
    {
        if (!destroyed)
        {
            destroyed_ = nullptr;
            callback_ = std::move(callback);
        }
        network.event_time_ = outerTime;
        throw;
    }
    if (!destroyed)
    {
        destroyed_ = nullptr;
        callback_ = std::move(callback);
    }
    network.event_time_ = outerTime;
}

} // namespace vehicle_sil::j1939
