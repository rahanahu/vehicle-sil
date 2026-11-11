// SPDX-License-Identifier: MIT
#include "vehicle_sil/local/local_bench.hpp"

#include "vehicle_sil/core/periodic.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil {

namespace {
constexpr const char* kWho = "vehicle_sil::LocalBench: ";

std::string quoted(std::string_view name) { return "\"" + std::string(name) + "\""; }
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
        std::string message = std::string(kWho) + "the ECU " + quoted(name_) + " has no network named " +
                              quoted(network) + ". Its networks: " + network_list();
        if (has_raw_channel(network))
        {
            message += " (" + quoted(network) + " is a raw CAN channel of it: raw_channel(ecu, network))";
        }
        throw std::out_of_range(message);
    }

    [[nodiscard]] can::RawChannel& raw_channel(std::string_view network) const
    {
        for (const auto& r : raw_channels_)
        {
            if (r->bus->name == network)
            {
                return *r->channel;
            }
        }
        std::string message = std::string(kWho) + "the ECU " + quoted(name_) + " has no raw CAN channel named " +
                              quoted(network) + ". Its raw CAN channels: " + raw_channel_list();
        if (has_network(network))
        {
            message += " (" + quoted(network) + " is a J1939 network of it: network(ecu, network))";
        }
        throw std::out_of_range(message);
    }

    /// The only J1939 network, for the lookup `call`("<ecu>").
    [[nodiscard]] Channel& only_channel(std::string_view call) const
    {
        if (channels_.size() == 1)
        {
            return *channels_.front();
        }
        std::string message = std::string(kWho) + std::string(call) + "(" + quoted(name_) + "): the ECU " + quoted(name_);
        if (channels_.empty())
        {
            message += " has no J1939 network";
            if (!raw_channels_.empty())
            {
                message += " (raw CAN channels are not J1939 networks: look its raw CAN channels " + raw_channel_list() +
                           " up with raw_channel(" + quoted(name_) + ", network))";
            }
            else
            {
                message += " (add one with its j1939(network))";
            }
        }
        else
        {
            message += " has " + std::to_string(channels_.size()) + " J1939 networks (" + network_list() +
                       "): name the network, e.g. " + std::string(call) + "(" + quoted(name_) + ", " +
                       quoted(channels_.front()->bus->name) + ")";
        }
        throw std::logic_error(message);
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
                if (c->bus == existing && existing->name.empty())
                {
                    throw std::invalid_argument(
                        std::string(kWho) + what + ": the ECU " + quoted(name_) +
                        " already has a network on the only bus, which has no name yet (it was added with "
                        "add_j1939()), and " +
                        what +
                        " would name that bus and give the ECU a second network on it. To name the bus, add the ECU "
                        "with add_ecu(" +
                        quoted(name_) + ", step).j1939(" + quoted(network) + ") instead of add_j1939(), or name the "
                        "bus with another ECU's j1939(" + quoted(network) + ")");
                }
                if (c->bus == existing)
                {
                    throw std::invalid_argument(std::string(kWho) + what + ": the ECU " + quoted(name_) +
                                                " already has a network named " + quoted(network));
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
    /// The names of the J1939 networks, quoted, for messages ("none" if there is none).
    [[nodiscard]] std::string network_list() const
    {
        std::string out;
        for (const auto& c : channels_)
        {
            out += (out.empty() ? "" : ", ") + quoted(c->bus->name);
        }
        return out.empty() ? "none" : out;
    }

    /// The names of the raw CAN channels, quoted, for messages ("none" if there is none).
    [[nodiscard]] std::string raw_channel_list() const
    {
        std::string out;
        for (const auto& r : raw_channels_)
        {
            out += (out.empty() ? "" : ", ") + quoted(r->bus->name);
        }
        return out.empty() ? "none" : out;
    }

    [[nodiscard]] bool has_network(std::string_view network) const
    {
        return std::any_of(channels_.begin(), channels_.end(), [&](const auto& c) { return c->bus->name == network; });
    }

    [[nodiscard]] bool has_raw_channel(std::string_view network) const
    {
        return std::any_of(raw_channels_.begin(), raw_channels_.end(),
                           [&](const auto& r) { return r->bus->name == network; });
    }

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
                throw std::invalid_argument(std::string(kWho) + "can(" + quoted(network) + "): the ECU " + quoted(name_) +
                                            " already has a raw CAN channel named " + quoted(network));
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
        ecus += (ecus.empty() ? "\"" : ", \"") + e + "\"";
    }
    // add_j1939() means the only bus; refusing the second bus in either order keeps the result
    // independent of the order of the calls.
    throw std::logic_error(std::string(kWho) + std::string(what) + ": no second bus \"" + std::string(network) +
                           "\" can be made: the ECUs " + ecus +
                           " were added without a bus name (add_j1939()), and with a second bus it would be unclear "
                           "which bus they are meant to be on. Add those ECUs with add_ecu(name, step).j1939(\"<bus>\") "
                           "instead. Buses: " +
                           bus_list());
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

LocalBench::NamedBus& LocalBench::named_bus(std::string_view network, std::string_view call, std::string_view rest) const
{
    for (const auto& b : buses_)
    {
        if (b->name == network)
        {
            return *b;
        }
    }
    const std::string quoted = "\"" + std::string(network) + "\"";
    std::string message = std::string(kWho) + std::string(call) + quoted + std::string(rest) + ": no bus named " + quoted;
    if (buses_.empty())
    {
        message += ": there is no bus yet. Use bus() / injector() / inject(time, frame) without a bus name (they make "
                   "the only bus)";
        if (!network.empty())
        {
            message += ", or add an ECU on the bus first with add_ecu(name, step).j1939(" + quoted + ") or can(" +
                       quoted + ")";
        }
    }
    else if (buses_.front()->name.empty()) // then it is the only bus
    {
        message += ": the only bus has no name yet. Use bus() / injector() / inject(time, frame) without a bus name, "
                   "or name the bus first with an ECU's j1939(" +
                   quoted + ") or can(" + quoted + ")";
    }
    else
    {
        message += ". Buses: " + bus_list();
    }
    throw std::out_of_range(message);
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
            throw std::invalid_argument(std::string(kWho) + "the name " + quoted(name) + " is used");
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
    std::string names;
    for (const auto& s : sites_)
    {
        names += (names.empty() ? "" : ", ") + quoted(s->name());
    }
    throw std::out_of_range(std::string(kWho) + "no ECU named " + quoted(name) + ". ECUs: " +
                            (names.empty() ? "none (add them with add_ecu() or add_j1939())" : names));
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

j1939::Network& LocalBench::network(std::string_view ecu) const { return *site(ecu).only_channel("network").network; }
can::VirtualCanBus::Endpoint& LocalBench::endpoint(std::string_view ecu) const
{
    return *site(ecu).only_channel("endpoint").endpoint;
}
can::TapPort& LocalBench::tap(std::string_view ecu) const { return *site(ecu).only_channel("tap").tap; }
can::Host& LocalBench::host(std::string_view ecu) const { return site(ecu).host(); }
can::RawChannel& LocalBench::raw_channel(std::string_view ecu, std::string_view network) const
{
    return site(ecu).raw_channel(network);
}

can::VirtualCanBus::Endpoint& LocalBench::injector() { return *only_bus("injector()").injector; }
can::VirtualCanBus::Endpoint& LocalBench::injector(std::string_view network) const
{
    return *named_bus(network, "injector(", ")").injector;
}
can::VirtualCanBus& LocalBench::bus() { return *only_bus("bus()").bus; }
can::VirtualCanBus& LocalBench::bus(std::string_view network) const { return *named_bus(network, "bus(", ")").bus; }

can::SendResult LocalBench::inject(SimTime time, const can::Frame& frame)
{
    return only_bus("inject(time, frame)").injector->send(can::TimedFrame{time, frame});
}

can::SendResult LocalBench::inject(std::string_view network, SimTime time, const can::Frame& frame)
{
    return named_bus(network, "inject(", ", time, frame)").injector->send(can::TimedFrame{time, frame});
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
