// SPDX-License-Identifier: MIT
//
// Where one ECU's application is assembled, whatever runs it (vehicle_sil::ecu, no SIL Kit).
#pragma once

#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/lin/lin_master.hpp"
#include "vehicle_sil/lin/lin_slave.hpp"

#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace vehicle_sil {

namespace detail {
class EcuAgent;
} // namespace detail

/// Where one ECU's application is assembled: the J1939 networks it is on (and, for other
/// protocols, raw CAN channels), its LIN master and slaves, the variables it shows for control
/// from outside, its work per step, and the objects it owns. A runtime implements it (Peer: a
/// SIL Kit participant; LocalBench::add_ecu(): a host on the local virtual buses), so an
/// application written against Ecu& runs on each of them unchanged.
///
///     class Engine {
///     public:
///         explicit Engine(vehicle_sil::Ecu& ecu) : node_(ecu.j1939("J1939_CAN").add_node({...})) {
///             ecu.input("engine.throttle", &throttle_, {.unit = "%", .min = 0.0, .max = 100.0});
///             ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
///             ecu.every(100ms, [this](vehicle_sil::SimTime) { static_cast<void>(node_.send(pgn, data())); });
///         }
///     };
///
/// One step of an ECU at `now`: its networks and raw CAN channels receive and handle what
/// arrived (all of them in the order they were added by j1939() and can()), then the writes to
/// its variables whose time has come take effect (control::Agent::on_step_begin()), then the
/// functions of on_step(), every() and at() run (in the order added), then the LIN nodes'
/// step(now) (in the order added by lin_master() and lin_slave()), then the variables are sampled
/// (control::Agent::on_step_end(), every AgentOptions::sample_period), then the networks and
/// raw channels send (in the same order as they received). A handler of one of them may send on
/// another: the frame leaves in this step. What the application asks of a LIN node in a step
/// (set_frame_data(), set_response(), request_sleep(), ...) takes effect in that step; what a
/// LIN node receives is reported in its step, so the application sees it in the next one. A
/// write to a variable is seen by the application in the step it takes effect in (the J1939
/// handlers and the parts' timers of that step ran before it); the sample of a step has what
/// the application and the LIN nodes did in it.
///
/// Variables (input(), output() and their forms): the ECU's control::Agent (agent()) holds
/// them, made when first used; an ECU without variables has none and its steps do nothing for
/// it. Writes come through the agent (Agent::submit(), or a transport such as
/// control_grpc::AgentClient on agent()), values go out through Agent::latest(). The ECU drives
/// the agent: it applies the writes and samples in its steps as above. A Peer cancels the
/// writes still waiting when it stops (stop() and its destructor; Agent::finish()), so a
/// transport stopped after it still reports them; on LocalBench nobody receives the results of
/// the writes still waiting (the transports are gone before the ECU). Peer adds monitoring
/// outputs of its own (PeerOptions::monitor).
///
/// Network names: one name is one network of one kind. j1939() and can() may name the same CAN
/// network; a LIN network needs a name of its own, used by one LIN node of the ECU. LocalBench
/// and Simulation check the kind across their ECUs (std::invalid_argument; on SIL Kit, among the
/// ECUs of the Simulation's process only: other processes are not seen).
///
/// Set up (j1939(), can(), lin_master(), lin_slave(), input(), output(), set_agent_options(),
/// on_step(), every(), at(), emplace(), and the networks' nodes, handlers and parts, the raw
/// channels' and LIN nodes' handlers) before the run starts; afterwards these functions throw
/// std::logic_error (nodes may still be added from a step, as j1939::Network::add_node()
/// allows).
///
/// Ownership: the ECU owns its networks, its raw channels, its LIN nodes, its control agent and
/// the objects made by emplace(); references to them are valid as long as the ECU. It destroys
/// its LIN nodes, then its networks (their parts first), then its raw channels, then its agent,
/// before the emplaced objects, so handlers, parts and the variables' pointers and functions may
/// refer to those.
///
/// Threading: set it up from one thread. The steps run on the runtime's thread (see the
/// implementation); so do the functions, handlers and parts.
class Ecu
{
public:
    virtual ~Ecu() = default;
    Ecu(const Ecu&) = delete;
    Ecu& operator=(const Ecu&) = delete;

    /// A J1939 network of this ECU on the CAN network `network` (ECUs that name the same
    /// network share its bus). std::invalid_argument for an empty name, for a name this ECU
    /// used before for a J1939 network and for a LIN network's name; std::logic_error once the
    /// run has started.
    j1939::Network& j1939(const std::string& network, j1939::NetworkOptions options = {})
    {
        require_network_name(network);
        return add_j1939(network, std::move(options));
    }
    /// A raw CAN channel of this ECU on the CAN network `network`, for a protocol that is not
    /// J1939: can::RawChannel (send() and on_frame(); no timers, no segmentation; use every()
    /// and on_step() for periodic work). It has a CAN controller of its own on that bus (Peer: a
    /// SIL Kit CAN controller named "<network>.raw"; LocalBench: an endpoint of the bus
    /// `network`). The same ECU may also have j1939(network) on that bus: they are two
    /// controllers, each receives the other's frames (the J1939 network ignores 11-bit frames),
    /// in the step after the one that sent them, like the frames of other ECUs. Not so for a
    /// Peer's J1939 network on a port the caller made (Peer::j1939(name, port), e.g. Vector XL):
    /// the raw channel is always on SIL Kit, so the two are on different buses.
    /// std::invalid_argument for an empty name, for a name this ECU used for a raw channel
    /// before, for a LIN network's name and for a capacity of 0; std::logic_error once the run
    /// has started.
    can::RawChannel& can(const std::string& network, can::RawChannelOptions options = {})
    {
        require_network_name(network);
        return add_can(network, options);
    }
    /// The LIN master of this ECU on the LIN network `network` (the ECUs that name the same
    /// network share its bus; one master per bus), running `schedule` with the ECU's step
    /// (lin::LinMaster). Set its handlers (on_result(), on_state()) before the run. Peer: a SIL
    /// Kit LIN controller named `network`, virtual time only; LocalBench: an endpoint of the
    /// virtual LIN bus `network`.
    /// std::invalid_argument for an empty name, a name this ECU used before (for any network),
    /// an invalid schedule or options (lin::LinMaster's constructor); std::logic_error once the
    /// run has started, for a Peer in real time, and for a second master on a LocalBench bus.
    lin::LinMaster& lin_master(const std::string& network, lin::LinScheduleTable schedule,
                               lin::LinMasterOptions options = {})
    {
        require_network_name(network);
        return add_lin_master(network, std::move(schedule), options);
    }
    /// A LIN slave of this ECU on the LIN network `network` (lin::LinSlave). Set its handlers
    /// (on_frame(), on_state()) before the run. Peer and LocalBench as for lin_master().
    /// std::invalid_argument for an empty name, a name this ECU used before (for any network)
    /// and invalid options (lin::LinSlave's constructor); std::logic_error once the run has
    /// started and for a Peer in real time.
    lin::LinSlave& lin_slave(const std::string& network, lin::LinSlaveOptions options = {})
    {
        require_network_name(network);
        return add_lin_slave(network, std::move(options));
    }
    /// A variable that can be written from outside while the simulation runs, bound to
    /// `*value` (bool, an integer type up to 64 bits signed or 32 bits unsigned, or a
    /// floating-point type; control::Agent::input()). VariableOptions: unit, range (checked on
    /// every write), step, description, parameter (writable before the first step only).
    /// `value` must stay valid as long as the ECU (a member of an emplace()d object, or of the
    /// application class that Simulation::add() makes). std::invalid_argument for an empty
    /// name, a name this ECU declared before and bad options; std::logic_error once the run has
    /// started or the schema is complete. A clash with a Peer's monitoring outputs shows when
    /// the schema is completed (control::Agent::complete_schema(): Peer::start(), an
    /// AgentClient's constructor or the first submit()), as std::invalid_argument there.
    template <typename T>
    void input(std::string name, T* value, control::VariableOptions options = {})
    {
        require_not_started("input()");
        declare([&](control::Agent& target) { target.input(std::move(name), value, std::move(options)); });
    }
    /// An input read and written through functions (the setter may do more than store); its
    /// type is the getter's return type. A setter that throws rejects the write
    /// (control::CommandStatus::Rejected).
    template <typename Get, typename Set>
        requires std::is_invocable_v<Get>
    void input(std::string name, Get get, Set set, control::VariableOptions options = {})
    {
        require_not_started("input()");
        declare([&](control::Agent& target) { target.input(std::move(name), std::move(get), std::move(set), std::move(options)); });
    }
    /// An integer or enum input that only takes the values of `items` (names and values unique,
    /// each within the type).
    template <typename T>
    void input_enum(std::string name, T* value, std::vector<control::EnumItem> items, control::VariableOptions options = {})
    {
        require_not_started("input_enum()");
        declare([&](control::Agent& target) { target.input_enum(std::move(name), value, std::move(items), std::move(options)); });
    }
    /// A read-only variable bound to `*value` (as input()).
    template <typename T>
    void output(std::string name, const T* value, control::VariableOptions options = {})
    {
        require_not_started("output()");
        declare([&](control::Agent& target) { target.output(std::move(name), value, std::move(options)); });
    }
    /// A read-only variable read through `get` (its type is the return type), called in the
    /// samples, on the step thread.
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output(std::string name, Get get, control::VariableOptions options = {})
    {
        require_not_started("output()");
        declare([&](control::Agent& target) { target.output(std::move(name), std::move(get), std::move(options)); });
    }
    /// A read-only integer variable that takes the values of `items` (a state).
    template <typename Get>
        requires std::is_invocable_v<Get>
    void output_enum(std::string name, Get get, std::vector<control::EnumItem> items, control::VariableOptions options = {})
    {
        require_not_started("output_enum()");
        declare([&](control::Agent& target) { target.output_enum(std::move(name), std::move(get), std::move(items), std::move(options)); });
    }
    /// The options of this ECU's agent (queue sizes, AgentOptions::sample_period). Before the
    /// agent is made: before the first input(), output() or agent() (std::logic_error
    /// afterwards, and once the run has started). std::invalid_argument for bad options
    /// (control::Agent::check_options(), with the ECU named); the options stay as they were.
    void set_agent_options(control::AgentOptions options);
    /// This ECU's control agent, made on first use (input() and output() use it too). Use it to
    /// connect a transport (control_grpc::AgentClient, which completes the schema: declare the
    /// variables first; the networks and nodes of a Peer that exist then are the ones its
    /// monitoring covers), to write (submit()), to read (latest(), schema()) and for events
    /// (log(), from the steps). After the run has started it returns the agent the ECU has;
    /// std::logic_error if it has none. A transport must be destroyed before the ECU (Peer,
    /// Simulation, LocalBench), which destroys its agent: declare it after them, and do not
    /// emplace() it in the ECU (the emplaced objects go after the agent). An ECU destroyed while
    /// a transport still holds a control::TransportLease on its agent writes why to stderr and
    /// calls std::terminate() (instead of a transport using a destroyed agent).
    control::Agent& agent();

    /// Runs `fn` in every step of this ECU, after its networks handled what arrived and before
    /// they send (in the order added, together with every() and at()). std::invalid_argument for an
    /// empty function; std::logic_error once the run has started.
    virtual void on_step(std::function<void(SimTime now)> fn) = 0;
    /// Runs `fn` in the first step at or after 0, period, 2 * period, ... (once per step at
    /// most; vehicle_sil::periodic() in core/periodic.hpp). std::invalid_argument for period <= 0
    /// or an empty function; std::logic_error once the run has started.
    virtual void every(SimDuration period, std::function<void(SimTime now)> fn) = 0;
    /// Runs `fn` once, in the first step that starts at or after `time` (the step need not land on
    /// it; the first step if `time` is before it), at the same place as on_step() and every() (in
    /// the order added, together with them; vehicle_sil::once_at() in core/periodic.hpp). For
    /// what a script does at a moment of the run, e.g. calling Node::send() or a part that sends,
    /// which only works in a step. A run that ends before `time` never calls it (silently).
    /// std::invalid_argument for an empty function; std::logic_error once the run has started
    /// (add every at() before the first run).
    virtual void at(SimTime time, std::function<void(SimTime now)> fn) = 0;
    /// Constructs a T that the ECU owns: it is destroyed after the ECU's LIN nodes, networks and
    /// raw channels (their handlers and parts may refer to it), the object emplaced last first. Use
    /// it for the models and application objects the handlers refer to. Only before the run
    /// starts: afterwards std::logic_error, and no T is made. Exceptions of T's constructor
    /// propagate and the T is not kept, but what the constructor set up on the ECU (networks,
    /// nodes, handlers, functions) stays and may refer to the destroyed T: do not run the ECU
    /// after that (Simulation::add() makes its Simulation unusable).
    template <class T, class... Args>
    T& emplace(Args&&... args)
    {
        require_not_started("emplace()");
        auto object = std::make_shared<T>(std::forward<Args>(args)...);
        T& ref = *object;
        keep(std::move(object));
        return ref;
    }

protected:
    Ecu() = default;

private:
    static void require_network_name(const std::string& network)
    {
        if (network.empty())
        {
            throw std::invalid_argument("vehicle_sil::Ecu: empty network name");
        }
    }

    /// Throws std::logic_error (naming `what`) once the run has started.
    virtual void require_not_started(const char* what) const = 0;
    /// true once the run has started.
    [[nodiscard]] virtual bool started() const noexcept = 0;
    /// The implementation's control agent holder (detail::EcuAgent).
    virtual detail::EcuAgent& ecu_agent() noexcept = 0;
    /// Runs `add` (a registration) on the agent, made if there is none; its exceptions come
    /// back with the ECU named.
    void declare(const std::function<void(control::Agent&)>& add);
    /// j1939() of the implementation, with a name that is not empty (a separate name: Peer has
    /// a j1939() of its own).
    virtual j1939::Network& add_j1939(const std::string& network, j1939::NetworkOptions options) = 0;
    /// can() of the implementation, with a name that is not empty.
    virtual can::RawChannel& add_can(const std::string& network, can::RawChannelOptions options) = 0;
    /// lin_master() of the implementation, with a name that is not empty. Initialises the
    /// master (init()) before its first step.
    virtual lin::LinMaster& add_lin_master(const std::string& network, lin::LinScheduleTable schedule,
                                           lin::LinMasterOptions options) = 0;
    /// lin_slave() of the implementation, with a name that is not empty. Initialises the slave
    /// (init()) before its first step.
    virtual lin::LinSlave& add_lin_slave(const std::string& network, lin::LinSlaveOptions options) = 0;
    /// Takes ownership of an emplace()d object (std::logic_error once the run has started).
    virtual void keep(std::shared_ptr<void> object) = 0;
};

namespace detail {

/// The control agent of an Ecu implementation (Peer, LocalBench's ECUs): made on first use,
/// with the options set before; the steps call begin() and end() around the application and
/// the LIN nodes, and the implementation calls finish() once no step runs any more. Make it the
/// second data member, after OwnedObjects: the agent goes after the networks and before the
/// emplace()d objects that its variables may refer to.
class EcuAgent
{
public:
    /// `who` starts the messages (e.g. "vehicle_sil::Peer \"Engine\": "). `bind` is called once
    /// with the new agent, to bind it to its driver (control::Agent::bind_driver()).
    EcuAgent(std::string who, std::function<void(control::Agent&)> bind) : who_(std::move(who)), bind_(std::move(bind))
    {
    }
    /// Destroys the agent. A transport still attached to it (control::TransportLease) would use
    /// it afterwards: then this writes why to stderr and calls std::terminate().
    ~EcuAgent()
    {
        if (const std::string why = transports_left(); !why.empty())
        {
            std::fputs(why.c_str(), stderr);
            std::fflush(stderr);
            std::terminate();
        }
    }
    EcuAgent(const EcuAgent&) = delete;
    EcuAgent& operator=(const EcuAgent&) = delete;

    /// Empty if no transport holds the agent (or there is none); else the message the destructor
    /// writes before it stops the process (a line, naming the ECU and how to fix it).
    [[nodiscard]] std::string transports_left() const
    {
        const std::size_t n = agent_ ? agent_->transports() : 0;
        if (n == 0)
        {
            return {};
        }
        return who_ + "destroyed while " + std::to_string(n)
               + " transport(s) (e.g. control_grpc::AgentClient) still use its control agent: destroy the AgentClient "
                 "before the ECU, Simulation or LocalBench (declare it after them), and do not emplace() it in the "
                 "ECU\n";
    }

    void set_options(control::AgentOptions options)
    {
        if (agent_)
        {
            throw std::logic_error(who_ + "set_agent_options() after the control agent was made: call it before the "
                                          "first input(), output() or agent()");
        }
        try
        {
            control::Agent::check_options(options);
        }
        catch (const std::invalid_argument& e)
        {
            throw std::invalid_argument(who_ + "set_agent_options(): " + e.what());
        }
        options_ = options;
    }
    /// The agent, made now if there is none (std::invalid_argument for bad options, with `who`
    /// in front; then none is kept).
    control::Agent& get()
    {
        if (!agent_)
        {
            std::unique_ptr<control::Agent> agent;
            try
            {
                agent = std::make_unique<control::Agent>(options_);
            }
            catch (const std::invalid_argument& e)
            {
                throw std::invalid_argument(who_ + e.what());
            }
            if (bind_)
            {
                bind_(*agent);
            }
            agent_ = std::move(agent);
        }
        return *agent_;
    }
    /// Runs `add` on get(); std::invalid_argument and std::logic_error come back of the same
    /// type with `who` in front.
    void declare(const std::function<void(control::Agent&)>& add)
    {
        auto& agent = get(); // names the ECU itself
        try
        {
            add(agent);
        }
        catch (const std::invalid_argument& e)
        {
            throw std::invalid_argument(who_ + e.what());
        }
        catch (const std::logic_error& e)
        {
            throw std::logic_error(who_ + e.what());
        }
    }
    /// The agent, or std::logic_error if there is none (after the run started).
    control::Agent& existing() const
    {
        if (!agent_)
        {
            throw std::logic_error(who_ + "agent() after the run started, but the ECU has no control agent: declare its "
                                          "variables (input(), output()) or call agent() before the run");
        }
        return *agent_;
    }
    [[nodiscard]] control::Agent* find() const noexcept { return agent_.get(); }
    /// Before the application of the step at `now`: applies the writes whose time has come.
    void begin(SimTime now)
    {
        if (agent_)
        {
            agent_->on_step_begin(now);
        }
    }
    /// After the application and the LIN nodes: samples the values.
    void end(SimTime now)
    {
        if (agent_)
        {
            agent_->on_step_end(now);
        }
    }
    /// No step runs any more: cancels the writes still waiting (control::Agent::finish()).
    /// Repeated calls do nothing.
    void finish() noexcept
    {
        if (agent_ && !finished_)
        {
            finished_ = true;
            try
            {
                agent_->finish();
            }
            catch (...)
            {
                // Out of memory for a result's text: the remaining writes stay unanswered.
            }
        }
    }

private:
    std::string who_;
    std::function<void(control::Agent&)> bind_;
    control::AgentOptions options_{};
    std::unique_ptr<control::Agent> agent_;
    bool finished_ = false;
};

/// The objects an Ecu implementation keeps for emplace(). Its destructor (and clear())
/// destroys them in the reverse order of add(); std::vector states no order for its elements.
/// Make it the first data member, so that it goes after everything else of the ECU.
class OwnedObjects
{
public:
    OwnedObjects() = default;
    ~OwnedObjects() { clear(); }
    OwnedObjects(const OwnedObjects&) = delete;
    OwnedObjects& operator=(const OwnedObjects&) = delete;

    /// Takes `object` (the caller's reference goes; std::bad_alloc leaves it destroyed).
    void add(std::shared_ptr<void> object) { objects_.push_back(std::move(object)); }
    /// Destroys the objects, the one added last first.
    void clear() noexcept
    {
        while (!objects_.empty())
        {
            objects_.pop_back();
        }
    }
    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }

private:
    std::vector<std::shared_ptr<void>> objects_;
};

} // namespace detail

inline void Ecu::set_agent_options(control::AgentOptions options)
{
    require_not_started("set_agent_options()");
    ecu_agent().set_options(options);
}

inline void Ecu::declare(const std::function<void(control::Agent&)>& add) { ecu_agent().declare(add); }

inline control::Agent& Ecu::agent()
{
    auto& holder = ecu_agent();
    if (started())
    {
        return holder.existing();
    }
    return holder.get();
}

} // namespace vehicle_sil
