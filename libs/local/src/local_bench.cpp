// SPDX-License-Identifier: MIT
#include "vehicle_sil/local/local_bench.hpp"

#include "vehicle_sil/core/periodic.hpp"

#include <stdexcept>
#include <utility>

namespace vehicle_sil {

namespace {
constexpr const char* kWho = "vehicle_sil::LocalBench: ";
} // namespace

struct LocalBench::NamedBus
{
    std::string name;
    std::unique_ptr<can::VirtualCanBus> bus;
    /// The endpoint inject() sends from; no network uses it.
    can::VirtualCanBus::Endpoint* injector = nullptr;
};

struct LocalBench::Channel
{
    // Destroyed bottom-up: the network, then the tap.
    std::string name;
    can::VirtualCanBus::Endpoint* endpoint = nullptr;
    std::unique_ptr<can::TapPort> tap;
    std::unique_ptr<j1939::Network> network;
};

/// A raw CAN channel of an ECU (Ecu::can()) on an endpoint of its own.
struct LocalBench::RawCan
{
    std::string name;
    can::VirtualCanBus::Endpoint* endpoint = nullptr;
    std::unique_ptr<can::RawChannel> channel;
};

/// One ECU: a host with its networks, its raw channels, its application stage and the objects
/// it owns.
class LocalBench::Site final : public Ecu
{
public:
    Site(LocalBench& owner, std::string ecuName, SimDuration stepSize) :
      owner_(owner), name_(std::move(ecuName)), step_(stepSize), host_(std::make_unique<can::Host>())
    {
        host_->set_application([this](SimTime now) {
            for (std::size_t i = 0; i < applications_.size(); ++i)
            {
                applications_[i](now);
            }
        });
    }

    // The host first, then the networks (the one added last first, each before its tap), then
    // the raw channels (the one added last first); the owned objects go last with the members.
    ~Site() override
    {
        host_.reset();
        while (!channels_.empty())
        {
            channels_.pop_back();
        }
        while (!raw_channels_.empty())
        {
            raw_channels_.pop_back();
        }
    }

    Site(const Site&) = delete;
    Site& operator=(const Site&) = delete;

    void on_step(std::function<void(SimTime now)> fn) override
    {
        require_not_started("on_step()");
        if (!fn)
        {
            throw std::invalid_argument(std::string(kWho) + "empty function");
        }
        applications_.push_back(std::move(fn));
    }

    void every(SimDuration period, std::function<void(SimTime now)> fn) override
    {
        require_not_started("every()");
        applications_.push_back(periodic(period, std::move(fn))); // std::invalid_argument
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] SimDuration step() const noexcept { return step_; }
    [[nodiscard]] can::Host& host() const noexcept { return *host_; }

    [[nodiscard]] Channel& channel(std::string_view network) const
    {
        for (const auto& c : channels_)
        {
            if (c->name == network)
            {
                return *c;
            }
        }
        throw std::out_of_range(std::string(kWho) + "the ECU " + name_ + " has no network named " + std::string(network));
    }

    [[nodiscard]] Channel& only_channel() const
    {
        if (channels_.size() != 1)
        {
            throw std::logic_error(std::string(kWho) + "the ECU " + name_ + " has " + std::to_string(channels_.size()) +
                                   " networks: name the network");
        }
        return *channels_.front();
    }

    /// A J1939 network on the bus `network` ("": the unnamed bus, for LocalBench::add_j1939()).
    /// Nothing is attached if it throws.
    j1939::Network& attach_j1939(const std::string& network, j1939::NetworkOptions options)
    {
        require_not_started("j1939()");
        for (const auto& c : channels_)
        {
            if (c->name == network)
            {
                throw std::invalid_argument(std::string(kWho) + "the ECU " + name_ + " has a network named " + network);
            }
        }
        const std::string what = "j1939(\"" + network + "\")";
        owner_.require_bus_kind(network, what);
        auto c = std::make_unique<Channel>();
        c->name = network;
        c->network = std::make_unique<j1939::Network>(std::move(options)); // std::invalid_argument
        // The host must not keep a network that is not kept here: push_back() cannot throw after
        // the reserve().
        channels_.reserve(channels_.size() + 1);
        c->endpoint = &owner_.bus_for(network, what).bus->attach();
        c->tap = std::make_unique<can::TapPort>(*c->endpoint);
        host_->add(*c->network, *c->tap);
        channels_.push_back(std::move(c));
        return *channels_.back()->network;
    }

private:
    j1939::Network& add_j1939(const std::string& network, j1939::NetworkOptions options) override
    {
        return attach_j1939(network, std::move(options));
    }

    can::RawChannel& add_can(const std::string& network, can::RawChannelOptions options) override
    {
        require_not_started("can()");
        for (const auto& r : raw_channels_)
        {
            if (r->name == network)
            {
                throw std::invalid_argument(std::string(kWho) + "the ECU " + name_ + " has a raw CAN channel named " +
                                            network);
            }
        }
        const std::string what = "can(\"" + network + "\")";
        owner_.require_bus_kind(network, what);
        auto r = std::make_unique<RawCan>();
        r->name = network;
        r->channel = std::make_unique<can::RawChannel>(options); // std::invalid_argument
        // The host must not keep a channel that is not kept here: push_back() cannot throw
        // after the reserve(). (An endpoint stays on its bus once attached.)
        raw_channels_.reserve(raw_channels_.size() + 1);
        r->endpoint = &owner_.bus_for(network, what).bus->attach();
        host_->add(*r->channel, *r->endpoint);
        raw_channels_.push_back(std::move(r));
        return *raw_channels_.back()->channel;
    }

    void keep(std::shared_ptr<void> object) override
    {
        require_not_started("emplace()");
        owned_.add(std::move(object));
    }

    void require_not_started(const char* what) const override
    {
        if (owner_.started_)
        {
            throw std::logic_error(std::string(kWho) + what + " after the first run");
        }
    }

    detail::OwnedObjects owned_; ///< first: destroyed after everything else of the ECU
    LocalBench& owner_;
    std::string name_;
    SimDuration step_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::vector<std::unique_ptr<RawCan>> raw_channels_; ///< not in channels_: the lookups are J1939 only
    std::vector<std::function<void(SimTime)>> applications_;
    std::unique_ptr<can::Host> host_;
};

LocalBench::LocalBench(can::VirtualCanBusOptions options) : options_(std::move(options)) {}

// The ECUs first, the one added last first (each with its host first; the driver only keeps
// pointers to the hosts), then the other members: the driver, the buses with the endpoints.
LocalBench::~LocalBench()
{
    // std::vector destroys its elements in no stated order.
    while (!sites_.empty())
    {
        sites_.pop_back();
    }
}

void LocalBench::require_bus_kind(std::string_view network, std::string_view what) const
{
    if (network.empty() && bus_kind_ == BusKind::Named)
    {
        throw std::logic_error(std::string(kWho) + std::string(what) +
                               ": the ECUs of this LocalBench are on named buses, there is no unnamed bus; name the bus "
                               "(inject(network, time, frame), injector(network), bus(network)) and add ECUs with "
                               "add_ecu(name, step).j1939(network) instead of add_j1939()");
    }
    if (!network.empty() && bus_kind_ == BusKind::Unnamed)
    {
        throw std::logic_error(std::string(kWho) + std::string(what) +
                               ": this LocalBench uses the unnamed bus (add_j1939(), inject(time, frame), injector(), "
                               "bus()), which a named bus would never reach; add this ECU with add_j1939() too, or "
                               "name the bus everywhere: add_ecu(name, step).j1939(network) and inject(network, ...)");
    }
}

LocalBench::NamedBus& LocalBench::bus_for(const std::string& network, std::string_view what)
{
    require_bus_kind(network, what);
    bus_kind_ = network.empty() ? BusKind::Unnamed : BusKind::Named;
    for (const auto& b : buses_)
    {
        if (b->name == network)
        {
            return *b;
        }
    }
    auto b = std::make_unique<NamedBus>();
    b->name = network;
    b->bus = std::make_unique<can::VirtualCanBus>(options_);
    b->injector = &b->bus->attach();
    buses_.push_back(std::move(b));
    return *buses_.back();
}

LocalBench::NamedBus& LocalBench::named_bus(std::string_view network) const
{
    for (const auto& b : buses_)
    {
        if (b->name == network)
        {
            return *b;
        }
    }
    throw std::out_of_range(std::string(kWho) + "no bus named " + std::string(network));
}

Ecu& LocalBench::add_ecu(std::string name, SimDuration step)
{
    if (started_)
    {
        throw std::logic_error(std::string(kWho) + "add_ecu() after the first run");
    }
    if (name.empty())
    {
        throw std::invalid_argument(std::string(kWho) + "empty name");
    }
    for (const auto& s : sites_)
    {
        if (s->name() == name)
        {
            throw std::invalid_argument(std::string(kWho) + "the name " + name + " is used");
        }
    }
    if (step <= SimDuration::zero())
    {
        throw std::invalid_argument(std::string(kWho) + "the step must be > 0");
    }
    sites_.push_back(std::make_unique<Site>(*this, std::move(name), step));
    return *sites_.back();
}

j1939::Network& LocalBench::add_j1939(std::string name, SimDuration step, j1939::NetworkOptions options)
{
    if (started_)
    {
        throw std::logic_error(std::string(kWho) + "add_j1939() after the first run");
    }
    require_bus_kind("", "add_j1939()");
    add_ecu(std::move(name), step);
    try
    {
        return sites_.back()->attach_j1939("", std::move(options));
    }
    catch (...)
    {
        sites_.pop_back();
        throw;
    }
}

void LocalBench::on_step(std::string_view name, std::function<void(SimTime now)> fn)
{
    if (started_)
    {
        throw std::logic_error(std::string(kWho) + "on_step() after the first run");
    }
    if (!fn)
    {
        throw std::invalid_argument(std::string(kWho) + "empty function");
    }
    site(name).on_step(std::move(fn));
}

LocalBench::Site& LocalBench::site(std::string_view name) const
{
    for (const auto& s : sites_)
    {
        if (s->name() == name)
        {
            return *s;
        }
    }
    throw std::out_of_range(std::string(kWho) + "no ECU named " + std::string(name));
}

Ecu& LocalBench::ecu(std::string_view name) const { return site(name); }

std::vector<std::string> LocalBench::ecu_names() const
{
    std::vector<std::string> out;
    out.reserve(sites_.size());
    for (const auto& s : sites_)
    {
        out.push_back(s->name());
    }
    return out;
}

j1939::Network& LocalBench::network(std::string_view ecu, std::string_view network) const
{
    return *site(ecu).channel(network).network;
}
can::VirtualCanBus::Endpoint& LocalBench::endpoint(std::string_view ecu, std::string_view network) const
{
    return *site(ecu).channel(network).endpoint;
}
can::TapPort& LocalBench::tap(std::string_view ecu, std::string_view network) const { return *site(ecu).channel(network).tap; }

j1939::Network& LocalBench::network(std::string_view ecu) const { return *site(ecu).only_channel().network; }
can::VirtualCanBus::Endpoint& LocalBench::endpoint(std::string_view ecu) const { return *site(ecu).only_channel().endpoint; }
can::TapPort& LocalBench::tap(std::string_view ecu) const { return *site(ecu).only_channel().tap; }
can::Host& LocalBench::host(std::string_view ecu) const { return site(ecu).host(); }

can::VirtualCanBus::Endpoint& LocalBench::injector() { return *bus_for("", "injector()").injector; }
can::VirtualCanBus::Endpoint& LocalBench::injector(std::string_view network) const { return *named_bus(network).injector; }
can::VirtualCanBus& LocalBench::bus() { return *bus_for("", "bus()").bus; }
can::VirtualCanBus& LocalBench::bus(std::string_view network) { return *named_bus(network).bus; }

can::SendResult LocalBench::inject(SimTime time, const can::Frame& frame)
{
    return bus_for("", "inject(time, frame)").injector->send(can::TimedFrame{time, frame});
}

can::SendResult LocalBench::inject(std::string_view network, SimTime time, const can::Frame& frame)
{
    return injector(network).send(can::TimedFrame{time, frame});
}

void LocalBench::run_until(SimTime end)
{
    if (!started_)
    {
        started_ = true;
        // The hosts join the driver now, in the order the ECUs were added: until here an ECU
        // could still be taken back (add_j1939()).
        for (const auto& s : sites_)
        {
            driver_.add(s->host(), s->step());
        }
    }
    driver_.run_until(end);
    if (end > now_)
    {
        now_ = end;
        // Nobody reads the injection endpoints: keep their inboxes from filling up.
        for (const auto& b : buses_)
        {
            b->injector->release_before(end, [](const can::TimedFrame&) {});
            b->injector->release_events_before(end, [](const can::PortEvent&) {});
        }
    }
}

void LocalBench::run_for(SimDuration duration) { run_until(now_ + duration); }

} // namespace vehicle_sil
