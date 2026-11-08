// SPDX-License-Identifier: MIT
#include "vehicle_sil/testkit/local_can.hpp"

#include <stdexcept>
#include <utility>

namespace vehicle_sil::testkit {

struct LocalCan::Channel
{
    // Destroyed bottom-up: the host first, then the network, then the tap.
    std::string name;
    can::VirtualCanBus::Endpoint* endpoint = nullptr;
    std::unique_ptr<can::TapPort> tap;
    std::unique_ptr<j1939::Network> network;
    std::vector<std::function<void(SimTime)>> applications;
    std::unique_ptr<can::Host> host;
};

LocalCan::LocalCan(can::VirtualCanBusOptions options) : bus_(std::move(options)), injector_(bus_.attach()) {}

// The channels first, the one added last first (each host before its network and tap; the
// driver only keeps pointers to the hosts), then the other members: the driver, the bus with the
// endpoints.
LocalCan::~LocalCan()
{
    // std::vector destroys its elements in no stated order.
    while (!channels_.empty())
    {
        channels_.pop_back();
    }
}

j1939::Network& LocalCan::add_j1939(std::string name, SimDuration step, j1939::NetworkOptions options)
{
    if (started_)
    {
        throw std::logic_error("vehicle_sil::testkit::LocalCan: add_j1939() after the first run");
    }
    if (name.empty())
    {
        throw std::invalid_argument("vehicle_sil::testkit::LocalCan: empty name");
    }
    for (const auto& c : channels_)
    {
        if (c->name == name)
        {
            throw std::invalid_argument("vehicle_sil::testkit::LocalCan: the name " + name + " is used");
        }
    }
    if (step <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::testkit::LocalCan: the step must be > 0");
    }
    auto c = std::make_unique<Channel>();
    c->name = std::move(name);
    c->endpoint = &bus_.attach();
    c->tap = std::make_unique<can::TapPort>(*c->endpoint);
    c->network = std::make_unique<j1939::Network>(std::move(options));
    c->host = std::make_unique<can::Host>();
    c->host->add(*c->network, *c->tap);
    Channel* raw = c.get();
    c->host->set_application([raw](SimTime now) {
        for (std::size_t i = 0; i < raw->applications.size(); ++i)
        {
            raw->applications[i](now);
        }
    });
    driver_.add(*c->host, step);
    channels_.push_back(std::move(c));
    return *raw->network;
}

void LocalCan::on_step(std::string_view name, std::function<void(SimTime now)> fn)
{
    if (started_)
    {
        throw std::logic_error("vehicle_sil::testkit::LocalCan: on_step() after the first run");
    }
    if (!fn)
    {
        throw std::invalid_argument("vehicle_sil::testkit::LocalCan: empty function");
    }
    channel(name).applications.push_back(std::move(fn));
}

LocalCan::Channel& LocalCan::channel(std::string_view name) const
{
    for (const auto& c : channels_)
    {
        if (c->name == name)
        {
            return *c;
        }
    }
    throw std::out_of_range("vehicle_sil::testkit::LocalCan: no network named " + std::string(name));
}

j1939::Network& LocalCan::network(std::string_view name) const { return *channel(name).network; }
can::VirtualCanBus::Endpoint& LocalCan::endpoint(std::string_view name) const { return *channel(name).endpoint; }
can::TapPort& LocalCan::tap(std::string_view name) const { return *channel(name).tap; }
can::Host& LocalCan::host(std::string_view name) const { return *channel(name).host; }

can::SendResult LocalCan::inject(SimTime time, const can::Frame& frame)
{
    return injector_.send(can::TimedFrame{time, frame});
}

void LocalCan::run_until(SimTime end)
{
    started_ = true;
    driver_.run_until(end);
    if (end > now_)
    {
        now_ = end;
        // Nobody reads the injection endpoint: keep its inbox from filling up.
        injector_.release_before(end, [](const can::TimedFrame&) {});
        injector_.release_events_before(end, [](const can::PortEvent&) {});
    }
}

void LocalCan::run_for(SimDuration duration) { run_until(now_ + duration); }

} // namespace vehicle_sil::testkit
