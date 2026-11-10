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
    /// "" until the bus is named: a bus made by a form without a bus name.
    std::string name;
    std::unique_ptr<can::VirtualCanBus> bus;
    /// The endpoint inject() sends from; no network uses it.
    can::VirtualCanBus::Endpoint* injector = nullptr;
};

struct LocalBench::Channel
{
    // Destroyed bottom-up: the network, then the tap.
    NamedBus* bus = nullptr; ///< its name is the network's
    can::VirtualCanBus::Endpoint* endpoint = nullptr;
    std::unique_ptr<can::TapPort> tap;
    std::unique_ptr<j1939::Network> network;
};

/// A raw CAN channel of an ECU (Ecu::can()) on an endpoint of its own.
struct LocalBench::RawCan
{
    NamedBus* bus = nullptr; ///< its name is the channel's
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
            if (c->bus->name == network)
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

    /// A J1939 network on the bus `network`, or on the only bus without a bus name (an empty
    /// `network`, for LocalBench::add_j1939()). Nothing is attached, made or named if it throws.
    j1939::Network& attach_j1939(const std::string& network, j1939::NetworkOptions options)
    {
        require_not_started("j1939()");
        const bool named = !network.empty();
        const std::string what = named ? "j1939(\"" + network + "\")" : "add_j1939()";
        NamedBus* existing = named ? owner_.bus_to_name(network) : owner_.only_bus_if_any(what);
        if (named)
        {
            for (const auto& c : channels_)
            {
                if (c->bus == existing)
                {
                    throw std::invalid_argument(std::string(kWho) + "the ECU " + name_ + " has a network named " + network);
                }
            }
            if (existing == nullptr)
            {
                owner_.require_new_bus(network, what);
            }
        }
        auto c = std::make_unique<Channel>();
        c->network = std::make_unique<j1939::Network>(std::move(options)); // std::invalid_argument
        // The host must not keep a network that is not kept here: push_back() cannot throw after
        // the reserve().
        channels_.reserve(channels_.size() + 1);
        c->bus = &owner_.use_bus(existing, network);
        c->endpoint = &c->bus->bus->attach();
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
        NamedBus* existing = owner_.bus_to_name(network);
        for (const auto& r : raw_channels_)
        {
            if (r->bus == existing)
            {
                throw std::invalid_argument(std::string(kWho) + "the ECU " + name_ + " has a raw CAN channel named " +
                                            network);
            }
        }
        if (existing == nullptr)
        {
            owner_.require_new_bus(network, "can(\"" + network + "\")");
        }
        auto r = std::make_unique<RawCan>();
        r->channel = std::make_unique<can::RawChannel>(options); // std::invalid_argument
        // The host must not keep a channel that is not kept here: push_back() cannot throw
        // after the reserve(). (An endpoint stays on its bus once attached.)
        raw_channels_.reserve(raw_channels_.size() + 1);
        r->bus = &owner_.use_bus(existing, network);
        r->endpoint = &r->bus->bus->attach();
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

std::string LocalBench::bus_list() const
{
    std::string out;
    for (const auto& b : buses_)
    {
        out += (out.empty() ? "\"" : ", \"") + b->name + "\"";
    }
    return out;
}

LocalBench::NamedBus* LocalBench::bus_to_name(std::string_view network) const
{
    for (const auto& b : buses_)
    {
        if (b->name == network)
        {
            return b.get();
        }
    }
    // A bus without a name is always the only one: a second bus names it instead.
    if (buses_.size() == 1 && buses_.front()->name.empty())
    {
        return buses_.front().get();
    }
    return nullptr;
}

LocalBench::NamedBus* LocalBench::only_bus_if_any(std::string_view what) const
{
    if (buses_.size() > 1)
    {
        throw std::logic_error(std::string(kWho) + std::string(what) + ": there are " + std::to_string(buses_.size()) +
                               " buses (" + bus_list() +
                               "); name the bus: inject(network, time, frame), injector(network), bus(network), "
                               "add_ecu(name, step).j1939(network)");
    }
    return buses_.empty() ? nullptr : buses_.front().get();
}

void LocalBench::require_new_bus(std::string_view network, std::string_view what) const
{
    if (buses_.empty() || unnamed_ecus_.empty())
    {
        return;
    }
    std::string ecus;
    for (const auto& e : unnamed_ecus_)
    {
        ecus += (ecus.empty() ? "" : ", ") + e;
    }
    throw std::logic_error(std::string(kWho) + std::string(what) + ": a second bus \"" + std::string(network) +
                           "\" beside " + bus_list() +
                           " would leave unclear which bus the ECUs added without a bus name are on (" + ecus +
                           "); add them with add_ecu(name, step).j1939(network), naming the network");
}

LocalBench::NamedBus& LocalBench::use_bus(NamedBus* existing, const std::string& network)
{
    if (existing == nullptr)
    {
        auto b = std::make_unique<NamedBus>();
        b->bus = std::make_unique<can::VirtualCanBus>(options_);
        b->injector = &b->bus->attach();
        buses_.push_back(std::move(b));
        existing = buses_.back().get();
    }
    if (!network.empty())
    {
        existing->name = network;
    }
    return *existing;
}

LocalBench::NamedBus& LocalBench::only_bus(std::string_view what)
{
    return use_bus(only_bus_if_any(what), "");
}

LocalBench::NamedBus& LocalBench::named_bus(std::string_view network)
{
    NamedBus* b = bus_to_name(network);
    if (b == nullptr)
    {
        throw std::out_of_range(std::string(kWho) + "no bus named " + std::string(network));
    }
    return use_bus(b, std::string(network));
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
    static_cast<void>(only_bus_if_any("add_j1939()"));
    add_ecu(std::move(name), step);
    try
    {
        // The name is kept first: nothing can throw after the network was attached.
        unnamed_ecus_.reserve(unnamed_ecus_.size() + 1);
        auto& network = sites_.back()->attach_j1939("", std::move(options));
        unnamed_ecus_.push_back(sites_.back()->name());
        return network;
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

can::VirtualCanBus::Endpoint& LocalBench::injector() { return *only_bus("injector()").injector; }
can::VirtualCanBus::Endpoint& LocalBench::injector(std::string_view network) { return *named_bus(network).injector; }
can::VirtualCanBus& LocalBench::bus() { return *only_bus("bus()").bus; }
can::VirtualCanBus& LocalBench::bus(std::string_view network) { return *named_bus(network).bus; }

can::SendResult LocalBench::inject(SimTime time, const can::Frame& frame)
{
    return only_bus("inject(time, frame)").injector->send(can::TimedFrame{time, frame});
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
