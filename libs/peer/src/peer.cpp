// SPDX-License-Identifier: MIT
#include "vehicle_sil/peer/peer.hpp"

#include "vehicle_sil/can/real_time_driver.hpp"
#include "vehicle_sil/core/periodic.hpp"
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

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

} // namespace

struct Peer::Channel
{
    // Destroyed bottom-up: the network, then the tap, then the port.
    std::string name;
    std::string controller; ///< of the SIL Kit port; empty for an external port
    std::unique_ptr<can::SilKitCanPort> silkit_port;
    std::unique_ptr<can::Port> external_port;
    std::unique_ptr<can::TapPort> tap;
    std::unique_ptr<j1939::Network> network;
};

struct Peer::RawCan
{
    // Destroyed bottom-up: the channel, then the port.
    std::string name;
    std::unique_ptr<can::SilKitCanPort> port;
    std::unique_ptr<can::RawChannel> channel;
};

struct Peer::LinNode
{
    // Destroyed bottom-up: the node, then the port.
    std::string name;
    std::unique_ptr<lin::SilKitLinPort> port;
    std::unique_ptr<lin::LinMaster> master;
    std::unique_ptr<lin::LinSlave> slave;
};

Peer::Peer(PeerOptions options) : options_(std::move(options))
{
    ParticipantRuntimeOptions runtimeOptions;
    runtimeOptions.participant_name = options_.participant;
    runtimeOptions.registry_uri = options_.registry_uri;
    runtimeOptions.participant_configuration = options_.participant_configuration;
    runtimeOptions.shutdown_timeout = options_.shutdown_timeout;
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
    runtime_->set_communication_ready_handler([this] {
        for (auto& node : lin_)
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
    });
}

Peer::~Peer()
{
    stop("vehicle_sil::Peer destroyed");
    // No more steps: the LIN nodes (each before its port; they use their controllers), then the
    // networks, taps and ports, then the raw channels and their ports. The host, the clock and
    // last the participant go with the members.
    while (!lin_.empty())
    {
        lin_.pop_back();
    }
    channels_.clear();
    raw_channels_.clear();
}

void Peer::require_not_started(const char* what) const
{
    if (started_)
    {
        throw std::logic_error(std::string("vehicle_sil::Peer: ") + what + " is only allowed before start()");
    }
}

void Peer::require_virtual_time(const char* what) const
{
    if (!options_.virtual_time)
    {
        throw std::logic_error(std::string("vehicle_sil::Peer: ") + what + " needs virtual time (LIN runs on virtual time only)");
    }
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
    if (has_name(lin_, network))
    {
        throw std::invalid_argument("vehicle_sil::Peer: " + std::string(what) + "(\"" + network + "\"): \"" + network +
                                    "\" is a LIN network of this peer; a CAN network needs a name of its own");
    }
    if (network_check_ && !network.empty())
    {
        network_check_(network, detail::NetworkKind::Can, std::string(what) + "(\"" + network + "\")");
    }
}

j1939::Network& Peer::j1939(std::string network_name, J1939ChannelOptions options)
{
    require_not_started("j1939()");
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
    Channel& c = add_channel(network_name);
    try
    {
        auto* canController = runtime_->participant().CreateCanController(controller, network_name);
        options.port.real_time_clock = options_.virtual_time ? nullptr : clock_;
        c.silkit_port = std::make_unique<can::SilKitCanPort>(*canController, options.port);
        c.controller = std::move(controller);
        c.tap = std::make_unique<can::TapPort>(*c.silkit_port);
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
    if (!port)
    {
        throw std::invalid_argument("vehicle_sil::Peer: no port");
    }
    require_can_name(name, "j1939");
    Channel& c = add_channel(std::move(name));
    try
    {
        c.external_port = std::move(port);
        c.tap = std::make_unique<can::TapPort>(*c.external_port);
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
    auto made = std::make_unique<RawCan>();
    made->name = network;
    made->channel = std::make_unique<can::RawChannel>(options); // std::invalid_argument
    raw_channels_.push_back(std::move(made));
    RawCan& r = *raw_channels_.back();
    try
    {
        auto* canController = runtime_->participant().CreateCanController(controller, network);
        can::SilKitCanPortOptions portOptions;
        portOptions.real_time_clock = options_.virtual_time ? nullptr : clock_;
        r.port = std::make_unique<can::SilKitCanPort>(*canController, portOptions);
        host_.add(*r.channel, *r.port);
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
    return std::nullopt;
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
    if (has_name(channels_, network) || has_name(raw_channels_, network))
    {
        throw std::invalid_argument("vehicle_sil::Peer: " + call + ": \"" + network +
                                    "\" is a CAN network of this peer; a LIN network needs a name of its own");
    }
    if (controller_is_used(network))
    {
        throw std::invalid_argument("vehicle_sil::Peer: " + call + ": a CAN controller of this peer is named \"" + network +
                                    "\" (the LIN controller is named like its network); rename that controller "
                                    "(J1939ChannelOptions::controller_name) or the LIN network");
    }
    if (network_check_)
    {
        network_check_(network, detail::NetworkKind::Lin, call);
    }
}

Peer::LinNode& Peer::add_lin_node(const std::string& network, lin::LinRole role)
{
    auto node = std::make_unique<LinNode>();
    node->name = network;
    auto* controller = runtime_->participant().CreateLinController(network, network);
    node->port = std::make_unique<lin::SilKitLinPort>(*controller, role);
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
        node.master = std::make_unique<lin::LinMaster>(*node.port, std::move(schedule), options_.virtual_time->step, options);
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
        node.slave = std::make_unique<lin::LinSlave>(*node.port, std::move(options));
    }
    catch (...)
    {
        lin_.pop_back();
        throw;
    }
    return *node.slave;
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

void Peer::set_control(std::function<void(SimTime)> before, std::function<void(SimTime)> after,
                       std::function<void()> stopping)
{
    require_not_started("set_control()");
    if (std::exchange(control_set_, true))
    {
        throw std::logic_error("vehicle_sil::Peer: set_control() called twice");
    }
    control_before_ = std::move(before);
    control_after_ = std::move(after);
    control_stopping_ = std::move(stopping);
}

void Peer::run_application(SimTime now)
{
    if (control_before_)
    {
        control_before_(now);
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
    if (control_after_)
    {
        control_after_(now);
    }
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
    started_ = true;
    runtime_->start(timeout);
    if (!options_.virtual_time)
    {
        const std::lock_guard lock(mutex_);
        if (stopping_)
        {
            return; // stopped from another thread meanwhile: no driver
        }
        driver_ = std::make_unique<can::RealTimeDriver>(host_, *clock_, can::RealTimeDriverOptions{options_.max_idle});
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
    if (control_stopping_)
    {
        // Once: the control agent stops waking the driver of this peer, whose host goes away.
        std::exchange(control_stopping_, nullptr)();
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
}

void Peer::request_stop(std::string_view reason) noexcept { runtime_->request_stop(reason); }

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
