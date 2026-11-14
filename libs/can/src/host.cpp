// SPDX-License-Identifier: MIT
#include "vehicle_sil/can/host.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::can {

/// A (Steppable, Port) pair as a member of the step::Host: the CAN stages of one channel.
class Host::PairMember final : public step::Member
{
public:
    PairMember(Steppable& steppable, Port& port) noexcept : steppable_(&steppable), port_(&port) {}

    [[nodiscard]] Steppable* steppable() const noexcept { return steppable_; }
    [[nodiscard]] Port* port() const noexcept { return port_; }

    [[nodiscard]] TimeDomain time_domain() const noexcept override { return port_->time_domain(); }
    void start(std::optional<SimDuration> step_size) override { steppable_->start(step_size); }
    void set_wakeup(Wakeup wakeup) override { port_->set_wakeup(std::move(wakeup)); }
    void begin(SimTime now) override { steppable_->begin(now); }
    void receive(SimTime now) override
    {
        Steppable& steppable = *steppable_;
        port_->release_events_before(now, [&steppable](const PortEvent& event) { steppable.on_port_event(event); });
        port_->release_before(now, [&steppable](const TimedFrame& frame) { steppable.receive(frame); });
    }
    void process() override { steppable_->process(); }
    void flush() override { steppable_->flush(*port_); }
    [[nodiscard]] std::optional<SimTime> next_deadline() const noexcept override { return steppable_->next_deadline(); }

private:
    Steppable* steppable_;
    Port* port_;
};

Host::Host() = default;
Host::~Host() = default;

void Host::add(Steppable& steppable, Port& port)
{
    check_can_add(port.time_domain());
    const bool taken = std::any_of(pairs_.begin(), pairs_.end(), [&](const std::unique_ptr<PairMember>& pair) {
        return pair->port() == &port || pair->steppable() == &steppable;
    });
    if (taken)
    {
        throw std::logic_error("vehicle_sil::can::Host: a Steppable and a port can each be added only once");
    }
    pairs_.push_back(std::make_unique<PairMember>(steppable, port));
    try
    {
        step::Host::add(*pairs_.back());
    }
    catch (...)
    {
        pairs_.pop_back();
        throw;
    }
}

} // namespace vehicle_sil::can
