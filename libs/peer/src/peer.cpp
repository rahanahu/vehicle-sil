// SPDX-License-Identifier: MIT
#include "vehicle_sil/peer/peer.hpp"

#include "vehicle_sil/can/real_time_driver.hpp"

#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vehicle_sil {

struct Peer::Channel
{
    // Destroyed bottom-up: the network, then the tap, then the port.
    std::string name;
    std::unique_ptr<can::SilKitCanPort> silkit_port;
    std::unique_ptr<can::Port> external_port;
    std::unique_ptr<can::TapPort> tap;
    std::unique_ptr<j1939::Network> network;
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
            host_.step(now);
            // Autonomous: nobody else ends the run. The runtime runs no step at or after the
            // end, so this is the last one.
            const auto& end = options_.virtual_time->end;
            if (options_.lifecycle == LifecycleMode::Autonomous && end && now + step >= *end)
            {
                runtime_->request_stop("vehicle_sil::Peer: end of virtual time");
            }
        });
    }
    runtime_->set_communication_ready_handler([this] {
        for (auto& node : lin_)
        {
            if (node.master)
            {
                node.master->init();
            }
            else
            {
                node.slave->init();
            }
        }
    });
}

Peer::~Peer()
{
    stop("vehicle_sil::Peer destroyed");
    // No more steps: the LIN nodes (they use their controllers), then the networks, taps and
    // ports. The host, the clock and last the participant go with the members.
    lin_.clear();
    channels_.clear();
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

j1939::Network& Peer::j1939(std::string network_name, J1939ChannelOptions options)
{
    require_not_started("j1939()");
    Channel& c = add_channel(network_name);
    try
    {
        const std::string controller = options.controller_name.empty() ? network_name : options.controller_name;
        auto* canController = runtime_->participant().CreateCanController(controller, network_name);
        options.port.real_time_clock = options_.virtual_time ? nullptr : clock_;
        c.silkit_port = std::make_unique<can::SilKitCanPort>(*canController, options.port);
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

lin::LinMaster& Peer::lin_master(std::string network_name, lin::LinScheduleTable schedule, lin::LinMaster::ResultHandler on_result,
                                 lin::LinMaster::StateHandler on_state, lin::LinMasterOptions options)
{
    require_not_started("lin_master()");
    require_virtual_time("lin_master()");
    options.step = options_.virtual_time->step;
    auto* controller = runtime_->participant().CreateLinController(network_name, network_name);
    LinNode node;
    node.master = std::make_unique<lin::LinMaster>(*controller, std::move(schedule), options, std::move(on_result),
                                                   std::move(on_state));
    lin_.push_back(std::move(node));
    return *lin_.back().master;
}

lin::LinSlave& Peer::lin_slave(std::string network_name, lin::LinSlaveOptions options, lin::LinSlave::FrameHandler on_frame,
                               lin::LinSlave::StateHandler on_state)
{
    require_not_started("lin_slave()");
    require_virtual_time("lin_slave()");
    auto* controller = runtime_->participant().CreateLinController(network_name, network_name);
    LinNode node;
    node.slave = std::make_unique<lin::LinSlave>(*controller, std::move(options), std::move(on_frame), std::move(on_state));
    lin_.push_back(std::move(node));
    return *lin_.back().slave;
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
    if (!fn)
    {
        throw std::invalid_argument("vehicle_sil::Peer: empty function");
    }
    if (period <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::Peer: the period must be > 0");
    }
    step_functions_.push_back([period, due = SimTime{}, fn = std::move(fn)](SimTime now) mutable {
        if (now < due)
        {
            return;
        }
        // Skip the times this step passed: once per step.
        due += period * ((now - due) / period + 1);
        fn(now);
    });
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
        if (node.master)
        {
            node.master->step(now);
        }
        else
        {
            node.slave->step(now);
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
    throw std::out_of_range("vehicle_sil::Peer: no network named " + std::string(name));
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

void start_together(SimulationController& controller, const std::vector<Peer*>& peers, std::chrono::milliseconds timeout)
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
    try
    {
        controller.start(timeout);
    }
    catch (...)
    {
        controllerError = std::current_exception();
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

} // namespace vehicle_sil
