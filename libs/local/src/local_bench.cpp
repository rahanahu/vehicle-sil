// SPDX-License-Identifier: MIT
#include "vehicle_sil/local/local_bench.hpp"

#include "vehicle_sil/core/periodic.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil {

namespace {
constexpr const char* kWho = "vehicle_sil::LocalBench: ";

std::string in_quotes(std::string_view name) { return "\"" + std::string(name) + "\""; }

/// The port a LIN node of an ECU is made on. It reaches its bus endpoint only once the node
/// exists (bind()), so a node whose constructor throws leaves nothing on the bus.
class LinNodePort final : public lin::LinPort
{
public:
    void bind(lin::VirtualLinBus::Endpoint& endpoint)
    {
        endpoint_ = &endpoint;
        endpoint_->set_listener(listener_);
    }

    void set_listener(lin::LinPortListener* listener) override
    {
        listener_ = listener;
        if (endpoint_ != nullptr)
        {
            endpoint_->set_listener(listener);
        }
    }
    void open() override { endpoint().open(); }
    void send_header(SimTime now, lin::LinId id, std::uint8_t data_length, lin::LinChecksumModel checksum_model) override
    {
        endpoint().send_header(now, id, data_length, checksum_model);
    }
    void send_frame(SimTime now, const lin::LinFrame& frame) override { endpoint().send_frame(now, frame); }
    void send_go_to_sleep(SimTime now) override { endpoint().send_go_to_sleep(now); }
    void respond(SimTime header_time, const lin::LinFrame& frame) override { endpoint().respond(header_time, frame); }
    void send_wakeup(SimTime now) override { endpoint().send_wakeup(now); }
    void resume() override { endpoint().resume(); }

private:
    [[nodiscard]] lin::VirtualLinBus::Endpoint& endpoint() const
    {
        if (endpoint_ == nullptr)
        {
            throw std::logic_error(std::string(kWho) + "a LIN node used its port before it was on its bus");
        }
        return *endpoint_;
    }

    lin::VirtualLinBus::Endpoint* endpoint_ = nullptr;
    lin::LinPortListener* listener_ = nullptr;
};

/// A LIN master or slave of an ECU.
struct LinNode
{
    // Destroyed bottom-up: the node, then the gate, then its port.
    std::string network;
    std::size_t order = 0; ///< among the ECU's networks, raw channels and LIN nodes (describe())
    LinNodePort port;
    /// Keeps the ECU off the bus while ecu.connected is false.
    std::unique_ptr<detail::GatedLinPort> gate;
    std::unique_ptr<lin::LinMaster> master;
    std::unique_ptr<lin::LinSlave> slave;
};
} // namespace

struct LocalBench::NamedBus
{
    /// "" until the bus is named: a bus made by a form without a bus name.
    std::string name;
    std::unique_ptr<can::VirtualCanBus> bus;
    /// The endpoint inject() sends from; no network uses it.
    can::VirtualCanBus::Endpoint* injector = nullptr;
};

struct LocalBench::NamedLinBus
{
    std::string name;
    std::unique_ptr<lin::VirtualLinBus> bus;
};

struct LocalBench::Channel
{
    // Destroyed bottom-up: the network, then the tap, then the gate.
    NamedBus* bus = nullptr; ///< its name is the network's
    std::size_t order = 0;   ///< as LinNode::order
    can::VirtualCanBus::Endpoint* endpoint = nullptr;
    /// Keeps the ECU off the bus while ecu.connected is false; the tap sees what the network sees.
    std::unique_ptr<detail::GatedCanPort> gate;
    std::unique_ptr<can::TapPort> tap;
    std::unique_ptr<j1939::Network> network;
};

/// A raw CAN channel of an ECU (Ecu::can()) on an endpoint of its own.
struct LocalBench::RawCan
{
    NamedBus* bus = nullptr; ///< its name is the channel's
    std::size_t order = 0;   ///< as LinNode::order
    can::VirtualCanBus::Endpoint* endpoint = nullptr;
    std::unique_ptr<detail::GatedCanPort> gate;
    std::unique_ptr<can::RawChannel> channel;
};

/// One ECU: a host with its networks, its raw channels, its application stage and the objects
/// it owns.
class LocalBench::Site final : public Ecu
{
public:
    Site(LocalBench& owner, std::string ecuName, SimDuration stepSize) :
      // No wakeup: the calling thread runs the steps, in virtual time.
      agent_(std::string(kWho) + "ECU " + in_quotes(ecuName) + ": ", [](control::Agent& agent) { agent.bind_driver(); }),
      owner_(owner),
      name_(std::move(ecuName)),
      step_(stepSize),
      host_(std::make_unique<can::Host>())
    {
        host_->set_application([this](SimTime now) {
            agent_.begin(now);
            for (std::size_t i = 0; i < applications_.size(); ++i)
            {
                applications_[i](now);
            }
            for (const auto& node : lin_nodes_)
            {
                if (node->master)
                {
                    node->master->step(now);
                }
                else
                {
                    node->slave->step(now);
                }
            }
            agent_.end(now);
        });
        agent_.connection().set_on_reconnect([this] {
            for (const auto& c : channels_)
            {
                detail::announce_again(*c->network);
            }
        });
    }

    // The writes still waiting on the control agent are cancelled (no step runs any more; the
    // transports are gone already, so the agent's state is only consistent, not reported). Then
    // the host, then the LIN nodes (the one added last first), then the networks (the one added
    // last first, each before its tap), then the raw channels (the one added last first); the
    // control agent and last the owned objects go with the members.
    ~Site() override
    {
        agent_.finish();
        host_.reset();
        while (!lin_nodes_.empty())
        {
            lin_nodes_.pop_back();
        }
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

    void at(SimTime time, std::function<void(SimTime now)> fn) override
    {
        require_not_started("at()");
        applications_.push_back(once_at(time, std::move(fn))); // std::invalid_argument
    }

    [[nodiscard]] EcuDescription describe() const override
    {
        detail::NetworkList networks;
        for (const auto& c : channels_)
        {
            networks.add(c->order, detail::describe_j1939(c->bus->name, *c->network));
        }
        for (const auto& r : raw_channels_)
        {
            networks.add(r->order, {r->bus->name, NetworkUse::Raw, {}});
        }
        for (const auto& node : lin_nodes_)
        {
            networks.add(node->order, {node->network, node->master ? NetworkUse::LinMaster : NetworkUse::LinSlave, {}});
        }
        EcuDescription out;
        out.name = name_;
        out.step = step_;
        out.networks = std::move(networks).networks();
        out.variables = agent_.variables();
        return out;
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] SimDuration step() const noexcept { return step_; }
    /// Makes the control agent if the ECU has none yet (before the first run).
    void make_agent() { static_cast<void>(agent_.get()); }
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
        std::string message = std::string(kWho) + "the ECU " + in_quotes(name_) + " has no network named " +
                              in_quotes(network) + ". Its networks: " + network_list();
        if (has_raw_channel(network))
        {
            message += " (" + in_quotes(network) + " is a raw CAN channel of it: raw_channel(ecu, network))";
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
        std::string message = std::string(kWho) + "the ECU " + in_quotes(name_) + " has no raw CAN channel named " +
                              in_quotes(network) + ". Its raw CAN channels: " + raw_channel_list();
        if (has_network(network))
        {
            message += " (" + in_quotes(network) + " is a J1939 network of it: network(ecu, network))";
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
        std::string message = std::string(kWho) + std::string(call) + "(" + in_quotes(name_) + "): the ECU " + in_quotes(name_);
        if (channels_.empty())
        {
            message += " has no J1939 network";
            if (!raw_channels_.empty())
            {
                message += " (raw CAN channels are not J1939 networks: look its raw CAN channels " + raw_channel_list() +
                           " up with raw_channel(" + in_quotes(name_) + ", network))";
            }
            else
            {
                message += " (add one with its j1939(network))";
            }
        }
        else
        {
            message += " has " + std::to_string(channels_.size()) + " J1939 networks (" + network_list() +
                       "): name the network, e.g. " + std::string(call) + "(" + in_quotes(name_) + ", " +
                       in_quotes(channels_.front()->bus->name) + ")";
        }
        throw std::logic_error(message);
    }

    /// The LIN node on `network` of the role `role` (std::out_of_range otherwise).
    [[nodiscard]] LinNode& lin_node(std::string_view network, lin::LinRole role) const
    {
        for (const auto& node : lin_nodes_)
        {
            if (node->network == network && (node->master != nullptr) == (role == lin::LinRole::Master))
            {
                return *node;
            }
        }
        std::string nodes;
        for (const auto& node : lin_nodes_)
        {
            nodes += (nodes.empty() ? "" : ", ") + in_quotes(node->network) + (node->master ? " (master)" : " (slave)");
        }
        throw std::out_of_range(std::string(kWho) + "the ECU " + in_quotes(name_) + " has no LIN " +
                                (role == lin::LinRole::Master ? "master" : "slave") + " on " + in_quotes(network) +
                                ". Its LIN nodes: " + (nodes.empty() ? "none" : nodes));
    }

    /// A J1939 network on the bus `network`, or on the only bus without a bus name (an empty
    /// `network`, for LocalBench::add_j1939()). Nothing is attached, made or named if it throws.
    j1939::Network& attach_j1939(const std::string& network, j1939::NetworkOptions options)
    {
        require_not_started("j1939()");
        const bool named = !network.empty();
        const std::string what = named ? "j1939(\"" + network + "\")" : "add_j1939()";
        if (named)
        {
            owner_.require_can_name(network, name_, what);
        }
        NamedBus* existing = named ? owner_.bus_to_name(network) : owner_.only_bus_if_any(what);
        if (named)
        {
            for (const auto& c : channels_)
            {
                if (c->bus == existing && existing->name.empty())
                {
                    throw std::invalid_argument(
                        std::string(kWho) + what + ": the ECU " + in_quotes(name_) +
                        " already has a network on the only bus, which has no name yet (it was added with "
                        "add_j1939()), and " +
                        what +
                        " would name that bus and give the ECU a second network on it. To name the bus, add the ECU "
                        "with add_ecu(" +
                        in_quotes(name_) + ", step).j1939(" + in_quotes(network) + ") instead of add_j1939(), or name the "
                        "bus with another ECU's j1939(" + in_quotes(network) + ")");
                }
                if (c->bus == existing)
                {
                    throw std::invalid_argument(std::string(kWho) + what + ": the ECU " + in_quotes(name_) +
                                                " already has a network named " + in_quotes(network));
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
        c->order = added_++;
        c->endpoint = &c->bus->bus->attach();
        c->gate = std::make_unique<detail::GatedCanPort>(*c->endpoint, agent_.connection());
        c->tap = std::make_unique<can::TapPort>(*c->gate);
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
            out += (out.empty() ? "" : ", ") + in_quotes(c->bus->name);
        }
        return out.empty() ? "none" : out;
    }

    /// The names of the raw CAN channels, quoted, for messages ("none" if there is none).
    [[nodiscard]] std::string raw_channel_list() const
    {
        std::string out;
        for (const auto& r : raw_channels_)
        {
            out += (out.empty() ? "" : ", ") + in_quotes(r->bus->name);
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
        owner_.require_can_name(network, name_, "can(\"" + network + "\")");
        NamedBus* existing = owner_.bus_to_name(network);
        for (const auto& r : raw_channels_)
        {
            if (r->bus == existing)
            {
                throw std::invalid_argument(std::string(kWho) + "can(" + in_quotes(network) + "): the ECU " + in_quotes(name_) +
                                            " already has a raw CAN channel named " + in_quotes(network));
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
        r->order = added_++;
        r->endpoint = &r->bus->bus->attach();
        r->gate = std::make_unique<detail::GatedCanPort>(*r->endpoint, agent_.connection());
        host_->add(*r->channel, *r->gate);
        raw_channels_.push_back(std::move(r));
        return *raw_channels_.back()->channel;
    }

    lin::LinMaster& add_lin_master(const std::string& network, lin::LinScheduleTable schedule,
                                   lin::LinMasterOptions options) override
    {
        auto node = new_lin_node(network, "lin_master");
        node->gate = std::make_unique<detail::GatedLinPort>(node->port, agent_.connection(), lin::LinRole::Master);
        node->master = std::make_unique<lin::LinMaster>(*node->gate, std::move(schedule), step_, options);
        attach_lin_node(std::move(node), lin::LinRole::Master);
        return *lin_nodes_.back()->master;
    }

    lin::LinSlave& add_lin_slave(const std::string& network, lin::LinSlaveOptions options) override
    {
        auto node = new_lin_node(network, "lin_slave");
        node->gate = std::make_unique<detail::GatedLinPort>(node->port, agent_.connection(), lin::LinRole::Slave);
        node->slave = std::make_unique<lin::LinSlave>(*node->gate, std::move(options));
        attach_lin_node(std::move(node), lin::LinRole::Slave);
        return *lin_nodes_.back()->slave;
    }

    /// A LIN node on `network`, not yet on its bus; checks the name (`call`: lin_master or
    /// lin_slave).
    std::unique_ptr<LinNode> new_lin_node(const std::string& network, const std::string& call)
    {
        require_not_started((call + "()").c_str());
        const std::string what = call + "(" + in_quotes(network) + ")";
        for (const auto& node : lin_nodes_)
        {
            if (node->network == network)
            {
                throw std::invalid_argument(std::string(kWho) + what + ": the ECU " + in_quotes(name_) +
                                            " has a LIN node on " + in_quotes(network) +
                                            " already; an ECU has one LIN node per LIN bus");
            }
        }
        owner_.require_lin_name(network, name_, what);
        auto node = std::make_unique<LinNode>();
        node->network = network;
        return node;
    }

    /// Puts the node made on `node->port` on its bus (made now if it is new) and initialises
    /// it. Nothing is attached or made if it throws (a second master).
    void attach_lin_node(std::unique_ptr<LinNode> node, lin::LinRole role)
    {
        lin_nodes_.reserve(lin_nodes_.size() + 1);
        auto* existing = owner_.find_lin_bus(node->network);
        lin::VirtualLinBus& bus = existing != nullptr ? *existing->bus : owner_.use_lin_bus(node->network);
        node->port.bind(bus.attach(role, name_)); // std::logic_error for a second master (not on a new bus)
        node->order = added_++;
        if (node->master)
        {
            node->master->init();
        }
        else
        {
            node->slave->init();
        }
        lin_nodes_.push_back(std::move(node));
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
            throw std::logic_error(std::string(kWho) + "ECU " + in_quotes(name_) + ": " + what
                                   + " after the first run (set the ECU up before the run)");
        }
    }
    [[nodiscard]] bool started() const noexcept override { return owner_.started_; }
    detail::EcuAgent& ecu_agent() noexcept override { return agent_; }

    detail::OwnedObjects owned_; ///< first: destroyed after everything else of the ECU
    detail::EcuAgent agent_;     ///< second: after the networks, before the owned objects
    LocalBench& owner_;
    std::string name_;
    SimDuration step_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::vector<std::unique_ptr<RawCan>> raw_channels_; ///< not in channels_: the lookups are J1939 only
    std::vector<std::unique_ptr<LinNode>> lin_nodes_;   ///< stepped after the application, in this order
    std::size_t added_ = 0; ///< numbers the networks, raw channels and LIN nodes in the order added
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

std::string LocalBench::lin_bus_list() const
{
    std::string out;
    for (const auto& b : lin_buses_)
    {
        out += (out.empty() ? "" : ", ") + in_quotes(b->name);
    }
    return out.empty() ? "none" : out;
}

LocalBench::NamedLinBus* LocalBench::find_lin_bus(std::string_view network) const
{
    for (const auto& b : lin_buses_)
    {
        if (b->name == network)
        {
            return b.get();
        }
    }
    return nullptr;
}

lin::VirtualLinBus& LocalBench::use_lin_bus(const std::string& network)
{
    if (auto* existing = find_lin_bus(network))
    {
        return *existing->bus;
    }
    auto b = std::make_unique<NamedLinBus>();
    b->name = network;
    b->bus = std::make_unique<lin::VirtualLinBus>();
    lin_buses_.push_back(std::move(b));
    return *lin_buses_.back()->bus;
}

void LocalBench::require_can_name(std::string_view network, std::string_view ecu, std::string_view what) const
{
    if (find_lin_bus(network) != nullptr)
    {
        throw std::invalid_argument(std::string(kWho) + std::string(what) + " of the ECU " + in_quotes(ecu) + ": " +
                                    in_quotes(network) +
                                    " is a LIN bus (made by lin_master() or lin_slave()); a CAN bus needs a name of "
                                    "its own");
    }
}

void LocalBench::require_lin_name(std::string_view network, std::string_view ecu, std::string_view what) const
{
    for (const auto& b : buses_)
    {
        if (!b->name.empty() && b->name == network)
        {
            throw std::invalid_argument(std::string(kWho) + std::string(what) + " of the ECU " + in_quotes(ecu) + ": " +
                                        in_quotes(network) +
                                        " is a CAN bus (made by j1939() or can()); a LIN bus needs a name of its own");
        }
    }
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
    if (find_lin_bus(network) != nullptr)
    {
        message += " (" + quoted + " is a LIN bus: lin_bus(network))";
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
            throw std::invalid_argument(std::string(kWho) + "the name " + in_quotes(name) + " is used");
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
        names += (names.empty() ? "" : ", ") + in_quotes(s->name());
    }
    throw std::out_of_range(std::string(kWho) + "no ECU named " + in_quotes(name) + ". ECUs: " +
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
lin::LinMaster& LocalBench::lin_master(std::string_view ecu, std::string_view network) const
{
    return *site(ecu).lin_node(network, lin::LinRole::Master).master;
}
lin::LinSlave& LocalBench::lin_slave(std::string_view ecu, std::string_view network) const
{
    return *site(ecu).lin_node(network, lin::LinRole::Slave).slave;
}
lin::VirtualLinBus& LocalBench::lin_bus(std::string_view network) const
{
    if (auto* found = find_lin_bus(network))
    {
        return *found->bus;
    }
    std::string message = std::string(kWho) + "lin_bus(" + in_quotes(network) + "): no LIN bus named " + in_quotes(network) +
                          ". LIN buses: " + lin_bus_list() + " (an ECU's lin_master(network) or lin_slave(network) "
                          "makes one)";
    for (const auto& b : buses_)
    {
        if (!b->name.empty() && b->name == network)
        {
            message += "; " + in_quotes(network) + " is a CAN bus: bus(network)";
        }
    }
    throw std::out_of_range(message);
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
            s->make_agent(); // every ECU has its input ecu.connected
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
