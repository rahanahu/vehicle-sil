// SPDX-License-Identifier: MIT
#include "vehicle_sil/peer/peer.hpp"

#include "peer_monitor.hpp"

#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/core/periodic.hpp"
#include "vehicle_sil/eth_silkit/silkit_eth_port.hpp"
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <utility>

namespace vehicle_sil {

namespace {

/// Start of the error that a real-time peer reports to the lifecycle when a step fails.
const std::string kRealTimeFailure = "real-time step failed: ";

/// The names of `items` (each with a `name`), quoted, for messages; "none" if there is none.
template <typename T>
std::string name_list(const std::vector<std::unique_ptr<T>>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += (out.empty() ? "\"" : ", \"") + item->name + "\"";
    }
    return out.empty() ? "none" : out;
}

template <typename T>
bool has_name(const std::vector<std::unique_ptr<T>>& items, std::string_view name)
{
    for (const auto& item : items)
    {
        if (item->name == name)
        {
            return true;
        }
    }
    return false;
}

/// The refusal of `call` because `network` is already a network of the kind `other` of this peer.
[[noreturn]] void refuse_other_kind(const std::string& call, const std::string& network, NetworkKind other,
                                    NetworkKind wanted)
{
    throw std::invalid_argument("vehicle_sil::Peer: " + call + ": \"" + network + "\" is " +
                                std::string(detail::network_kind_with_article(other)) + " network of this peer; " +
                                std::string(detail::network_kind_with_article(wanted)) + " network needs a name of its own");
}

} // namespace

struct Peer::Channel
{
    // Destroyed bottom-up: the network, then the tap, then the gate, then the port.
    std::string name;
    std::size_t order = 0; ///< among the networks, raw channels and LIN nodes (describe())
    std::string controller; ///< of the SIL Kit port; empty for an external port
    std::unique_ptr<can::SilKitCanPort> silkit_port;
    std::unique_ptr<can::Port> external_port;
    /// Keeps the ECU off the bus while ecu.connected is false; the tap sees what the network sees.
    std::unique_ptr<detail::GatedCanPort> gate;
    std::unique_ptr<can::TapPort> tap;
    std::unique_ptr<j1939::Network> network;
};

struct Peer::RawCan
{
    // Destroyed bottom-up: the channel, then the gate, then the port.
    std::string name;
    std::size_t order = 0; ///< as Channel::order
    std::unique_ptr<can::SilKitCanPort> port;
    std::unique_ptr<detail::GatedCanPort> gate;
    std::unique_ptr<can::RawChannel> channel;
};

struct Peer::LinNode
{
    // Destroyed bottom-up: the node, then the gate, then the port.
    std::string name;
    std::size_t order = 0; ///< as Channel::order
    std::unique_ptr<lin::SilKitLinPort> port;
    std::unique_ptr<detail::GatedLinPort> gate;
    std::unique_ptr<lin::LinMaster> master;
    std::unique_ptr<lin::LinSlave> slave;
};

struct Peer::Ethernet
{
    // Destroyed bottom-up: the channel, then the gate, then the port.
    std::string name;
    std::size_t order = 0; ///< as Channel::order
    std::optional<eth::MacAddress> mac; ///< eth::ChannelOptions::mac (describe())
    std::unique_ptr<eth::SilKitEthPort> port;
    std::unique_ptr<detail::GatedEthPort> gate;
    std::unique_ptr<eth::Channel> channel;
};

Peer::Peer(PeerOptions options) :
  agent_("vehicle_sil::Peer \"" + options.participant + "\": ", [this](control::Agent& agent) { bind_agent(agent); }),
  options_(std::move(options))
{
    const std::string who = "vehicle_sil::Peer \"" + options_.participant + "\": ";
    for (const auto& [simulation, code] : options_.network_names)
    {
        if (simulation.empty() || code.empty())
        {
            throw std::invalid_argument(who + "network_names has an entry with an empty name (\"" + simulation +
                                        "\" -> \"" + code + "\"): give the network's name in the simulation and "
                                        "the name this ECU's code gives it");
        }
        for (const auto& [otherSimulation, otherCode] : options_.network_names)
        {
            if (otherCode == code && otherSimulation < simulation)
            {
                throw std::invalid_argument(who + "network_names gives the network \"" + code +
                                            "\" of this ECU's code two names in the simulation, \"" + otherSimulation +
                                            "\" and \"" + simulation +
                                            "\", but a network of an ECU is on one bus: keep one of the two entries");
            }
        }
        if (simulation != code)
        {
            simulation_names_.emplace(code, simulation);
        }
    }
    if (!simulation_names_.empty())
    {
        if (const auto configured = configured_controller_networks(options_.participant_configuration); !configured.empty())
        {
            throw std::invalid_argument(who + "network_names renames networks, and the SIL Kit participant configuration "
                                        "puts a controller on a network itself (" + configured.front() +
                                        "): SIL Kit would use the configuration's network, whatever network_names says. "
                                        "Remove Network from the controllers of the configuration, or leave "
                                        "network_names out and name the networks there");
        }
    }
    ParticipantRuntimeOptions runtimeOptions;
    runtimeOptions.participant_name = options_.participant;
    runtimeOptions.registry_uri = options_.registry_uri;
    runtimeOptions.participant_configuration = options_.participant_configuration;
    runtimeOptions.shutdown_timeout = options_.shutdown_timeout;
    runtimeOptions.name_in_use_wait = options_.name_in_use_wait;
    runtimeOptions.lifecycle_mode = options_.lifecycle;
    runtimeOptions.virtual_time = options_.virtual_time;
    if (options_.virtual_time)
    {
        if (options_.virtual_time->end && *options_.virtual_time->end <= SimTime{})
        {
            throw std::invalid_argument("vehicle_sil::Peer: virtual_time.end must be after 0");
        }
        end_ = options_.virtual_time->end;
        host_.require_time_domain(can::TimeDomain::Virtual);
        host_.set_step_size(options_.virtual_time->step); // std::invalid_argument unless > 0
    }
    else
    {
        if (options_.max_idle <= SimDuration::zero())
        {
            throw std::invalid_argument("vehicle_sil::Peer: max_idle must be > 0");
        }
        host_.require_time_domain(can::TimeDomain::RealTime);
        if (options_.real_time_clock != nullptr)
        {
            clock_ = options_.real_time_clock;
        }
        else
        {
            own_clock_ = std::make_unique<can::SteadyRealTimeClock>();
            clock_ = own_clock_.get();
        }
    }
    runtime_ = std::make_unique<ParticipantRuntime>(std::move(runtimeOptions));
    host_.set_application([this](SimTime now) { run_application(now); });
    agent_.connection().set_on_reconnect([this] {
        for (const auto& c : channels_)
        {
            detail::announce_again(*c->network);
        }
    });
    if (options_.virtual_time)
    {
        runtime_->set_step_handler([this](SimTime now, SimDuration step) {
            // end_ is fixed before start(). The runtime itself skips the steps at or after
            // VirtualTimeOptions::end; detail::set_virtual_time_end() may make it earlier.
            if (end_ && now >= *end_)
            {
                return;
            }
            host_.step(now);
            // Autonomous: nobody else ends the run. No step at or after the end does anything,
            // so this is the last one.
            if (options_.lifecycle == LifecycleMode::Autonomous && end_ && now + step >= *end_)
            {
                runtime_->request_stop("vehicle_sil::Peer: end of virtual time");
            }
        });
    }
    // Where SIL Kit wants the controllers configured and started: on its thread, before the first
    // step, and (Coordinated) once every participant's services are known. Added, not set: a
    // handler that the application sets through runtime() does not replace this one.
    runtime_->add_communication_ready_handler([this] {
        const auto start_can = [this](can::SilKitCanPort& port, const std::string& network) {
            try
            {
                port.start();
            }
            catch (const std::exception& e)
            {
                // Not out of a SIL Kit handler: the participant goes to Error instead.
                runtime_->report_failure("vehicle_sil::Peer: starting the CAN controller of the network " +
                                         network_text(network) + " failed: " + e.what());
            }
        };
        for (auto& c : channels_)
        {
            if (c->silkit_port)
            {
                start_can(*c->silkit_port, c->name);
            }
        }
        for (auto& r : raw_channels_)
        {
            start_can(*r->port, r->name);
        }
        for (auto& e : ethernet_)
        {
            try
            {
                e->port->start();
            }
            catch (const std::exception& error)
            {
                // As for CAN: the participant goes to Error.
                runtime_->report_failure("vehicle_sil::Peer: starting the Ethernet controller of the network " +
                                         network_text(e->name) + " failed: " + error.what());
            }
        }
        for (auto& node : lin_)
        {
            try
            {
                if (node->master)
                {
                    node->master->init();
                }
                else
                {
                    node->slave->init();
                }
            }
            catch (const std::exception& e)
            {
                // As for CAN: the participant goes to Error, and the other nodes are still initialised.
                runtime_->report_failure("vehicle_sil::Peer: initialising the LIN controller of the network " +
                                         network_text(node->name) + " failed: " + e.what());
            }
        }
    });
}

Peer::~Peer()
{
    stop("vehicle_sil::Peer destroyed");
    // No more steps: the LIN nodes (each before its port; they use their controllers), then the
    // networks, taps and ports, then the raw channels and their ports, then the Ethernet channels
    // and their ports. The host, the clock and last the participant go with the members.
    while (!lin_.empty())
    {
        lin_.pop_back();
    }
    channels_.clear();
    raw_channels_.clear();
    ethernet_.clear();
}

void Peer::require_not_started(const char* what) const
{
    if (started_)
    {
        throw std::logic_error("vehicle_sil::Peer \"" + options_.participant + "\": " + what
                               + " is only allowed before start() (set the ECU up before the run)");
    }
}

void Peer::require_virtual_time(const char* what) const
{
    if (!options_.virtual_time)
    {
        throw std::logic_error(std::string("vehicle_sil::Peer: ") + what + " needs virtual time (LIN runs on virtual time only)");
    }
}

const std::string& Peer::simulation_name(const std::string& network) const
{
    const auto renamed = simulation_names_.find(network);
    return renamed == simulation_names_.end() ? network : renamed->second;
}

std::string Peer::network_text(const std::string& network) const
{
    const std::string& simulation = simulation_name(network);
    return "\"" + network + "\"" + (simulation == network ? std::string() : " (\"" + simulation + "\" in the simulation)");
}

std::string Peer::call_text(const std::string& call, const std::string& network) const
{
    const std::string& simulation = simulation_name(network);
    return call + (simulation == network ? std::string() : " (the network \"" + simulation + "\" of the simulation)");
}

void Peer::require_own_simulation_name(const std::string& network, const std::string& call) const
{
    if (simulation_names_.empty())
    {
        return; // without other names, two names of the code are two names of the simulation
    }
    const std::string& simulation = simulation_name(network);
    const auto check = [&](const auto& items) {
        for (const auto& item : items)
        {
            if (item->name != network && simulation_name(item->name) == simulation)
            {
                // One of the two has an entry, and the other is named like the entry's simulation name.
                const std::string& renamed = simulation == network ? item->name : network;
                throw std::invalid_argument(
                    "vehicle_sil::Peer \"" + options_.participant + "\": " + call + ": network_names makes the network \"" +
                    renamed + "\" of this ECU's code the network \"" + simulation +
                    "\" of the simulation, and the code has a network it names \"" + simulation +
                    "\" itself, so two networks of the ECU would be one bus. Give the code's \"" + simulation +
                    "\" another name in the simulation as well (one more entry of network_names; for an ECU "
                    "executable --map-network <simulation's name>=" + simulation + ")");
            }
        }
    };
    check(channels_);
    check(raw_channels_);
    check(lin_);
    check(ethernet_);
}

Peer::Channel& Peer::add_channel(std::string name)
{
    if (name.empty())
    {
        throw std::invalid_argument("vehicle_sil::Peer: empty network name");
    }
    for (const auto& c : channels_)
    {
        if (c->name == name)
        {
            throw std::invalid_argument("vehicle_sil::Peer: the network " + name + " was added before");
        }
    }
    auto c = std::make_unique<Channel>();
    c->name = std::move(name);
    c->order = added_++;
    channels_.push_back(std::move(c));
    return *channels_.back();
}

bool Peer::controller_is_used(const std::string& controller) const
{
    for (const auto& c : channels_)
    {
        if (c->silkit_port && c->controller == controller)
        {
            return true;
        }
    }
    for (const auto& r : raw_channels_)
    {
        if (r->name + ".raw" == controller)
        {
            return true;
        }
    }
    return false;
}

void Peer::require_can_name(const std::string& network, const char* what) const
{
    if (const auto other = detail::network_kind(*this, network); other && *other != NetworkKind::Can)
    {
        refuse_other_kind(std::string(what) + "(\"" + network + "\")", network, *other, NetworkKind::Can);
    }
    const std::string call = std::string(what) + "(\"" + network + "\")";
    require_own_simulation_name(network, call);
    if (network_check_ && !network.empty())
    {
        network_check_(simulation_name(network), NetworkKind::Can, call_text(call, network));
    }
}

void Peer::require_unmonitored(const std::string& network) const
{
    if (monitor_)
    {
        throw std::logic_error("vehicle_sil::Peer \"" + options_.participant + "\": j1939(\"" + network
                               + "\") after the control agent's schema was completed with the monitoring outputs: the "
                                 "network would not be monitored. Add the J1939 networks before a transport is made "
                                 "for the agent (control_silkit::AgentTransport) and before its first submit(), or set PeerOptions::monitor = false");
    }
}

j1939::Network& Peer::j1939(std::string network_name, J1939ChannelOptions options)
{
    require_not_started("j1939()");
    require_unmonitored(network_name);
    require_can_name(network_name, "j1939");
    std::string controller = options.controller_name.empty() ? network_name : options.controller_name;
    if (!network_name.empty() && controller_is_used(controller))
    {
        throw std::invalid_argument("vehicle_sil::Peer: a CAN controller of this peer is named \"" + controller +
                                    "\"; give this one another name (J1939ChannelOptions::controller_name)");
    }
    if (has_name(lin_, controller))
    {
        throw std::invalid_argument("vehicle_sil::Peer: the LIN controller of the LIN network \"" + controller +
                                    "\" has the name \"" + controller +
                                    "\"; give the CAN controller another name (J1939ChannelOptions::controller_name)");
    }
    if (has_name(ethernet_, controller))
    {
        throw std::invalid_argument("vehicle_sil::Peer: the Ethernet controller of the Ethernet network \"" + controller +
                                    "\" has the name \"" + controller +
                                    "\"; give the CAN controller another name (J1939ChannelOptions::controller_name)");
    }
    Channel& c = add_channel(network_name);
    try
    {
        auto* canController = runtime_->participant().CreateCanController(controller, simulation_name(network_name));
        options.port.real_time_clock = options_.virtual_time ? nullptr : clock_;
        c.silkit_port = std::make_unique<can::SilKitCanPort>(*canController, options.port);
        c.controller = std::move(controller);
        c.gate = std::make_unique<detail::GatedCanPort>(*c.silkit_port, agent_.connection());
        c.tap = std::make_unique<can::TapPort>(*c.gate);
        c.network = std::make_unique<j1939::Network>(std::move(options.network));
        host_.add(*c.network, *c.tap);
    }
    catch (...)
    {
        if (c.silkit_port)
        {
            c.silkit_port->close();
        }
        channels_.pop_back();
        throw;
    }
    return *c.network;
}

j1939::Network& Peer::j1939(std::string name, std::unique_ptr<can::Port> port, j1939::NetworkOptions options)
{
    require_not_started("j1939()");
    require_unmonitored(name);
    if (!port)
    {
        throw std::invalid_argument("vehicle_sil::Peer: no port");
    }
    if (simulation_names_.contains(name))
    {
        throw std::invalid_argument("vehicle_sil::Peer \"" + options_.participant + "\": j1939(\"" + name +
                                    "\") on a port of the caller's: network_names says that \"" + name + "\" is \"" +
                                    simulation_name(name) +
                                    "\" in the simulation, but a network on a port that the caller made is on no SIL "
                                    "Kit network that could have another name. Remove the entry \"" +
                                    simulation_name(name) + "\" -> \"" + name + "\" from network_names (--map-network " +
                                    simulation_name(name) + "=" + name + ")");
    }
    require_can_name(name, "j1939");
    Channel& c = add_channel(std::move(name));
    try
    {
        c.external_port = std::move(port);
        c.gate = std::make_unique<detail::GatedCanPort>(*c.external_port, agent_.connection());
        c.tap = std::make_unique<can::TapPort>(*c.gate);
        c.network = std::make_unique<j1939::Network>(std::move(options));
        host_.add(*c.network, *c.tap); // std::logic_error for a port of the other time domain
    }
    catch (...)
    {
        channels_.pop_back();
        throw;
    }
    return *c.network;
}

j1939::Network& Peer::add_j1939(const std::string& network, j1939::NetworkOptions options)
{
    J1939ChannelOptions channel;
    channel.network = std::move(options);
    return j1939(network, std::move(channel));
}

can::RawChannel& Peer::add_can(const std::string& network, can::RawChannelOptions options)
{
    require_not_started("can()");
    if (network.empty())
    {
        throw std::invalid_argument("vehicle_sil::Peer: empty network name");
    }
    for (const auto& r : raw_channels_)
    {
        if (r->name == network)
        {
            throw std::invalid_argument("vehicle_sil::Peer: the raw CAN channel " + network + " was added before");
        }
    }
    require_can_name(network, "can");
    // A controller of its own: a J1939 network on the same bus has the one named like the
    // network (or J1939ChannelOptions::controller_name).
    const std::string controller = network + ".raw";
    if (controller_is_used(controller))
    {
        throw std::invalid_argument("vehicle_sil::Peer: a CAN controller of this peer is named \"" + controller +
                                    "\"; rename that network or controller");
    }
    if (has_name(lin_, controller))
    {
        throw std::invalid_argument("vehicle_sil::Peer: can(\"" + network + "\"): the LIN network \"" + controller +
                                    "\" has a controller named \"" + controller +
                                    "\", the name of this raw channel's controller; rename one of the networks");
    }
    if (has_name(ethernet_, controller))
    {
        throw std::invalid_argument("vehicle_sil::Peer: can(\"" + network + "\"): the Ethernet network \"" + controller +
                                    "\" has a controller named \"" + controller +
                                    "\", the name of this raw channel's controller; rename one of the networks");
    }
    auto made = std::make_unique<RawCan>();
    made->name = network;
    made->order = added_++;
    made->channel = std::make_unique<can::RawChannel>(options); // std::invalid_argument
    raw_channels_.push_back(std::move(made));
    RawCan& r = *raw_channels_.back();
    try
    {
        auto* canController = runtime_->participant().CreateCanController(controller, simulation_name(network));
        can::SilKitCanPortOptions portOptions;
        portOptions.real_time_clock = options_.virtual_time ? nullptr : clock_;
        r.port = std::make_unique<can::SilKitCanPort>(*canController, portOptions);
        r.gate = std::make_unique<detail::GatedCanPort>(*r.port, agent_.connection());
        host_.add(*r.channel, *r.gate);
    }
    catch (...)
    {
        if (r.port)
        {
            r.port->close();
        }
        raw_channels_.pop_back();
        throw;
    }
    return *r.channel;
}

void Peer::keep(std::shared_ptr<void> object)
{
    require_not_started("emplace()");
    owned_.add(std::move(object));
}

void Peer::set_end(SimTime end)
{
    require_not_started("set_virtual_time_end()");
    if (!options_.virtual_time)
    {
        throw std::logic_error("vehicle_sil::Peer: set_virtual_time_end() needs virtual time");
    }
    if (end <= SimTime{})
    {
        throw std::invalid_argument("vehicle_sil::Peer: the end must be after 0");
    }
    if (!end_ || end < *end_)
    {
        end_ = end;
    }
}

namespace detail {

void set_virtual_time_end(Peer& peer, SimTime end) { peer.set_end(end); }

void set_network_check(Peer& peer,
                       std::function<void(const std::string& network, NetworkKind kind, const std::string& call)> check)
{
    peer.require_not_started("set_network_check()");
    peer.network_check_ = std::move(check);
}

std::optional<NetworkKind> network_kind(const Peer& peer, std::string_view network)
{
    if (has_name(peer.channels_, network) || has_name(peer.raw_channels_, network))
    {
        return NetworkKind::Can;
    }
    if (has_name(peer.lin_, network))
    {
        return NetworkKind::Lin;
    }
    if (has_name(peer.ethernet_, network))
    {
        return NetworkKind::Ethernet;
    }
    return std::nullopt;
}

std::optional<NetworkKind> simulation_network_kind(const Peer& peer, std::string_view network)
{
    const auto has = [&](const auto& items) {
        for (const auto& item : items)
        {
            if (peer.simulation_name(item->name) == network)
            {
                return true;
            }
        }
        return false;
    };
    if (has(peer.channels_) || has(peer.raw_channels_))
    {
        return NetworkKind::Can;
    }
    if (has(peer.lin_))
    {
        return NetworkKind::Lin;
    }
    if (has(peer.ethernet_))
    {
        return NetworkKind::Ethernet;
    }
    return std::nullopt;
}

const std::string& simulation_network_name(const Peer& peer, const std::string& network)
{
    return peer.simulation_name(network);
}

} // namespace detail

void Peer::require_lin_name(const std::string& network, const char* what) const
{
    const std::string call = std::string(what).substr(0, std::string(what).size() - 2) + "(\"" + network + "\")";
    if (has_name(lin_, network))
    {
        throw std::invalid_argument("vehicle_sil::Peer: " + call + ": the peer has a LIN node on \"" + network +
                                    "\" already; an ECU has one LIN node per LIN network");
    }
    if (const auto other = detail::network_kind(*this, network); other && *other != NetworkKind::Lin)
    {
        refuse_other_kind(call, network, *other, NetworkKind::Lin);
    }
    if (controller_is_used(network))
    {
        throw std::invalid_argument("vehicle_sil::Peer: " + call + ": a CAN controller of this peer is named \"" + network +
                                    "\" (the LIN controller is named like its network); rename that controller "
                                    "(J1939ChannelOptions::controller_name) or the LIN network");
    }
    require_own_simulation_name(network, call);
    if (network_check_)
    {
        network_check_(simulation_name(network), NetworkKind::Lin, call_text(call, network));
    }
}

Peer::LinNode& Peer::add_lin_node(const std::string& network, lin::LinRole role)
{
    auto node = std::make_unique<LinNode>();
    node->name = network;
    node->order = added_++;
    auto* controller = runtime_->participant().CreateLinController(network, simulation_name(network));
    node->port = std::make_unique<lin::SilKitLinPort>(*controller, role);
    node->gate = std::make_unique<detail::GatedLinPort>(*node->port, agent_.connection(), role);
    lin_.push_back(std::move(node));
    return *lin_.back();
}

lin::LinMaster& Peer::add_lin_master(const std::string& network, lin::LinScheduleTable schedule, lin::LinMasterOptions options)
{
    require_not_started("lin_master()");
    require_virtual_time("lin_master()");
    require_lin_name(network, "lin_master()");
    // Before the controller: SIL Kit cannot remove it, and a second one with its name is refused.
    lin::LinMaster::validate(schedule, options_.virtual_time->step, options);
    LinNode& node = add_lin_node(network, lin::LinRole::Master);
    try
    {
        node.master = std::make_unique<lin::LinMaster>(*node.gate, std::move(schedule), options_.virtual_time->step, options);
    }
    catch (...)
    {
        lin_.pop_back();
        throw;
    }
    return *node.master;
}

lin::LinSlave& Peer::add_lin_slave(const std::string& network, lin::LinSlaveOptions options)
{
    require_not_started("lin_slave()");
    require_virtual_time("lin_slave()");
    require_lin_name(network, "lin_slave()");
    // Before the controller, as in add_lin_master().
    lin::LinSlave::validate(options);
    LinNode& node = add_lin_node(network, lin::LinRole::Slave);
    try
    {
        node.slave = std::make_unique<lin::LinSlave>(*node.gate, std::move(options));
    }
    catch (...)
    {
        lin_.pop_back();
        throw;
    }
    return *node.slave;
}

void Peer::require_ethernet_name(const std::string& network) const
{
    const std::string call = "ethernet(\"" + network + "\")";
    if (has_name(ethernet_, network))
    {
        throw std::invalid_argument("vehicle_sil::Peer: " + call + ": the peer has an Ethernet channel on \"" + network +
                                    "\" already; an ECU has one channel per Ethernet network (keep the reference the "
                                    "first call returned)");
    }
    if (const auto other = detail::network_kind(*this, network); other && *other != NetworkKind::Ethernet)
    {
        refuse_other_kind(call, network, *other, NetworkKind::Ethernet);
    }
    if (controller_is_used(network))
    {
        throw std::invalid_argument("vehicle_sil::Peer: " + call + ": a CAN controller of this peer is named \"" + network +
                                    "\" (the Ethernet controller is named like its network); rename that controller "
                                    "(J1939ChannelOptions::controller_name) or the Ethernet network");
    }
    require_own_simulation_name(network, call);
    if (network_check_)
    {
        network_check_(simulation_name(network), NetworkKind::Ethernet, call_text(call, network));
    }
}

eth::Channel& Peer::add_ethernet(const std::string& network, eth::ChannelOptions options)
{
    require_not_started("ethernet()");
    if (!options_.virtual_time)
    {
        throw std::logic_error("vehicle_sil::Peer: ethernet(\"" + network +
                               "\") needs virtual time: a peer in real time has no Ethernet. Give the peer a step "
                               "(PeerOptions::virtual_time)");
    }
    require_ethernet_name(network);
    // Before the controller: SIL Kit cannot remove it, and a second one with its name is refused.
    eth::Channel::validate(options);
    auto made = std::make_unique<Ethernet>();
    made->name = network;
    made->mac = options.mac;
    ethernet_.push_back(std::move(made));
    Ethernet& e = *ethernet_.back();
    try
    {
        auto* controller = runtime_->participant().CreateEthernetController(network, simulation_name(network));
        eth::SilKitEthPortOptions portOptions;
        // Room for receive_capacity frames in every step whenever they arrive, and an
        // acknowledgement for each frame of a step.
        portOptions.inbox_capacity = eth::Channel::port_inbox_capacity(options);
        portOptions.event_capacity = std::max(portOptions.event_capacity, options.send_capacity);
        e.port = std::make_unique<eth::SilKitEthPort>(*controller, portOptions);
        e.gate = std::make_unique<detail::GatedEthPort>(*e.port, agent_.connection());
        e.channel = std::make_unique<eth::Channel>(*e.gate, std::move(options));
        host_.add(*e.channel);
        e.order = added_++;
    }
    catch (...)
    {
        if (e.port)
        {
            e.port->close();
        }
        ethernet_.pop_back();
        throw;
    }
    return *e.channel;
}

void Peer::on_step(std::function<void(SimTime now)> fn)
{
    require_not_started("on_step()");
    if (!fn)
    {
        throw std::invalid_argument("vehicle_sil::Peer: empty function");
    }
    step_functions_.push_back(std::move(fn));
}

void Peer::every(SimDuration period, std::function<void(SimTime now)> fn)
{
    require_not_started("every()");
    step_functions_.push_back(periodic(period, std::move(fn))); // std::invalid_argument
}

void Peer::at(SimTime time, std::function<void(SimTime now)> fn)
{
    require_not_started("at()");
    step_functions_.push_back(once_at(time, std::move(fn))); // std::invalid_argument
}

void Peer::bind_agent(control::Agent& agent)
{
    std::function<void()> wakeup;
    if (!uses_virtual_time())
    {
        // A queued write wakes the driver, with at most one wake-up pending; stop() clears it
        // before the host goes.
        auto pending = std::make_shared<std::atomic<bool>>(false);
        wakeup = [pending, &host = host_] {
            if (!pending->exchange(true, std::memory_order_acq_rel))
            {
                host.post([pending] { pending->store(false, std::memory_order_release); });
            }
        };
    }
    agent.bind_driver(std::move(wakeup), [this, &agent] {
        agent_.end_declarations(); // describe() leaves the monitoring outputs out
        if (options_.monitor)
        {
            try
            {
                monitor_ = detail::add_peer_monitoring(*this, agent, "vehicle_sil::Peer \"" + options_.participant + "\": ");
            }
            catch (...)
            {
                // A clash, found before anything was registered: the schema stays open, and
                // what the ECU declares now is its own again.
                agent_.reopen_declarations();
                throw;
            }
        }
    });
}

void Peer::run_application(SimTime now)
{
    agent_.begin(now);
    if (monitor_)
    {
        monitor_->before_step();
    }
    for (auto& fn : step_functions_)
    {
        fn(now);
    }
    for (auto& node : lin_)
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
    if (monitor_)
    {
        monitor_->after_step(now, *agent_.find());
    }
    agent_.end(now);
}

Peer::Channel& Peer::channel(std::string_view name) const
{
    for (const auto& c : channels_)
    {
        if (c->name == name)
        {
            return *c;
        }
    }
    const std::string quoted = "\"" + std::string(name) + "\"";
    std::string message = "vehicle_sil::Peer: the participant \"" + options_.participant + "\" has no network named " +
                          quoted + ". Its networks: " + name_list(channels_);
    if (has_name(raw_channels_, name))
    {
        message += " (" + quoted + " is a raw CAN channel of it: raw_channel(name))";
    }
    throw std::out_of_range(message);
}

j1939::Network& Peer::network(std::string_view name) const { return *channel(name).network; }

EcuDescription Peer::describe() const
{
    detail::NetworkList networks;
    for (const auto& c : channels_)
    {
        networks.add(c->order, detail::describe_j1939(c->name, *c->network));
    }
    for (const auto& r : raw_channels_)
    {
        networks.add(r->order, {r->name, NetworkUse::Raw, {}});
    }
    for (const auto& node : lin_)
    {
        networks.add(node->order, {node->name, node->master ? NetworkUse::LinMaster : NetworkUse::LinSlave, {}});
    }
    for (const auto& e : ethernet_)
    {
        networks.add(e->order, {e->name, NetworkUse::Ethernet, {}, e->mac});
    }
    EcuDescription out;
    out.name = options_.participant;
    if (options_.virtual_time)
    {
        out.step = options_.virtual_time->step;
    }
    out.networks = std::move(networks).networks();
    out.variables = agent_.variables();
    return out;
}

std::vector<std::string> Peer::network_names() const
{
    std::vector<std::string> names;
    names.reserve(channels_.size());
    for (const auto& c : channels_)
    {
        names.push_back(c->name);
    }
    return names;
}
can::TapPort& Peer::tap(std::string_view name) const { return *channel(name).tap; }
can::SilKitCanPort* Peer::silkit_port(std::string_view name) const { return channel(name).silkit_port.get(); }

can::RawChannel& Peer::raw_channel(std::string_view name) const
{
    for (const auto& r : raw_channels_)
    {
        if (r->name == name)
        {
            return *r->channel;
        }
    }
    const std::string quoted = "\"" + std::string(name) + "\"";
    std::string message = "vehicle_sil::Peer: the participant \"" + options_.participant +
                          "\" has no raw CAN channel named " + quoted + ". Its raw CAN channels: " +
                          name_list(raw_channels_);
    if (has_name(channels_, name))
    {
        message += " (" + quoted + " is a J1939 network of it: network(name))";
    }
    throw std::out_of_range(message);
}

void Peer::start(std::chrono::milliseconds timeout)
{
    require_not_started("start()");
    // Every ECU has an agent at the start: its input ecu.connected.
    agent_.get().complete_schema(); // the monitoring outputs; the peer stays unstarted if it throws
    started_ = true;
    runtime_->start(timeout);
    if (!options_.virtual_time)
    {
        const std::lock_guard lock(mutex_);
        if (stopping_)
        {
            return; // stopped from another thread meanwhile: no driver
        }
        can::RealTimeDriverOptions driverOptions{options_.max_idle};
        // As with virtual time, a failed step takes the lifecycle to Error: the controller sees
        // the failure, and a stop waits for the Error instead of stopping the simulation.
        driverOptions.on_failure = [runtime = runtime_.get()](const std::string& error) {
            runtime->report_failure(kRealTimeFailure + error);
        };
        driver_ = std::make_unique<can::RealTimeDriver>(host_, *clock_, std::move(driverOptions));
        driver_->start();
        running_driver_.store(driver_.get(), std::memory_order_release);
    }
}

std::optional<SilKit::Services::Orchestration::ParticipantState> Peer::wait(std::chrono::milliseconds timeout)
{
    const auto state = runtime_->wait_until_finished(timeout);
    if (auto* driver = running_driver_.load(std::memory_order_acquire); state && driver != nullptr)
    {
        driver->stop(); // thread safe and idempotent
    }
    return state;
}

std::optional<SilKit::Services::Orchestration::ParticipantState> Peer::run(std::chrono::milliseconds timeout)
{
    start(timeout);
    return wait(timeout);
}

void Peer::stop(std::string_view reason) noexcept
{
    // One stop at a time: a second caller (the destructor too) waits until the first is done.
    const std::lock_guard lock(mutex_);
    if (auto* agent = agent_.find())
    {
        agent->clear_wakeup(); // the agent stops waking the driver of this peer, whose host goes away
    }
    stopping_ = true;
    // Real time: the driver first, so that no step runs after the lifecycle stopped. A step
    // may still call failed() or error() meanwhile: they do not take the lock.
    if (driver_)
    {
        driver_->stop();
    }
    runtime_->stop(reason);
    for (auto& c : channels_)
    {
        if (c->silkit_port)
        {
            c->silkit_port->close();
        }
    }
    for (auto& r : raw_channels_)
    {
        r->port->close();
    }
    for (auto& e : ethernet_)
    {
        e->port->close();
    }
    // No step runs any more: the writes still waiting are cancelled.
    agent_.finish();
}

void Peer::request_stop(std::string_view reason) noexcept
{
    // Real time: failed() is true from the moment the host fails, a little before the driver's
    // thread reports the failure to the lifecycle (on_failure, above). A caller that saw the
    // failure and asks to stop must not get ahead of that report, or the stop would be an
    // ordinary one; so the failure is reported here first (the runtime takes the first report
    // only). With virtual time the runtime keeps a request made while the step handler runs.
    // Only the host is looked at: this may run on the driver's own thread.
    if (!options_.virtual_time && host_.failed())
    {
        try
        {
            runtime_->report_failure(kRealTimeFailure + host_.error());
        }
        catch (const std::exception&)
        {
            // Out of memory for the text; the driver's own report follows.
        }
    }
    runtime_->request_stop(reason);
}

bool Peer::failed() const
{
    if (host_.failed() || !runtime_->step_error().empty())
    {
        return true;
    }
    const auto* driver = running_driver_.load(std::memory_order_acquire);
    return driver != nullptr && driver->failed();
}

std::string Peer::error() const
{
    // Host::error() is complete and fixed once Host::failed() is true.
    if (host_.failed())
    {
        return host_.error();
    }
    if (std::string e = runtime_->step_error(); !e.empty())
    {
        return e;
    }
    const auto* driver = running_driver_.load(std::memory_order_acquire);
    return driver != nullptr ? driver->error() : std::string{};
}

namespace {

/// Starts each peer from its own thread and, if there is one, the controller from this one;
/// joins every thread, then rethrows the controller's exception, else the first of a peer.
void start_peers(SimulationController* controller, const std::vector<Peer*>& peers, std::chrono::milliseconds timeout)
{
    std::vector<std::exception_ptr> errors(peers.size());
    std::vector<std::thread> starters;
    struct Joiner
    {
        std::vector<std::thread>& threads;
        ~Joiner()
        {
            for (auto& t : threads)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }
        }
    } joiner{starters};
    starters.reserve(peers.size());
    for (std::size_t i = 0; i < peers.size(); ++i)
    {
        starters.emplace_back([&, i] {
            try
            {
                peers[i]->start(timeout);
            }
            catch (...)
            {
                errors[i] = std::current_exception();
            }
        });
    }
    std::exception_ptr controllerError;
    if (controller != nullptr)
    {
        try
        {
            controller->start(timeout);
        }
        catch (...)
        {
            controllerError = std::current_exception();
        }
    }
    for (auto& starter : starters)
    {
        starter.join();
    }
    // The controller's error first: without it the peers only time out.
    if (controllerError)
    {
        std::rethrow_exception(controllerError);
    }
    for (const auto& e : errors)
    {
        if (e)
        {
            std::rethrow_exception(e);
        }
    }
}

} // namespace

void start_together(SimulationController& controller, const std::vector<Peer*>& peers, std::chrono::milliseconds timeout)
{
    start_peers(&controller, peers, timeout);
}

void start_together(const std::vector<Peer*>& peers, std::chrono::milliseconds timeout)
{
    start_peers(nullptr, peers, timeout);
}

} // namespace vehicle_sil
