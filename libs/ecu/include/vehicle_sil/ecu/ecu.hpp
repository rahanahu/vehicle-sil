// SPDX-License-Identifier: MIT
//
// Where one ECU's application is assembled, whatever runs it (vehicle_sil::ecu, no SIL Kit).
#pragma once

#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil {

/// Where one ECU's application is assembled: the J1939 networks it is on (and, for other
/// protocols, raw CAN channels), its work per step, and the objects it owns. A runtime
/// implements it (Peer: a SIL Kit participant; testkit::LocalCan::add_ecu(): a host on the
/// local virtual buses), so an application written against Ecu& runs on each of them unchanged.
///
///     class Engine {
///     public:
///         explicit Engine(vehicle_sil::Ecu& ecu) : node_(ecu.j1939("J1939_CAN").add_node({...})) {
///             ecu.every(100ms, [this](vehicle_sil::SimTime) { static_cast<void>(node_.send(pgn, data())); });
///         }
///     };
///
/// One step of an ECU at `now`: its networks and raw CAN channels receive and handle what
/// arrived (all of them in the order they were added by j1939() and can()), then the functions
/// of on_step() and every() run (in the order added), then the networks and raw channels send
/// (in the same order). A handler of one of them may send on another: the frame leaves in this
/// step.
///
/// Set up (j1939(), can(), on_step(), every(), emplace(), and the networks' nodes, handlers and
/// parts, the raw channels' handlers) before the run starts; afterwards these functions throw
/// std::logic_error (nodes may still be added from a step, as j1939::Network::add_node()
/// allows).
///
/// Ownership: the ECU owns its networks, its raw channels and the objects made by emplace();
/// references to them are valid as long as the ECU. It destroys its networks (their parts
/// first), then its raw channels, before the emplaced objects, so handlers and parts may refer
/// to those.
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
    /// network share its bus). std::invalid_argument for an empty name and for a name this ECU
    /// used before; std::logic_error once the run has started.
    j1939::Network& j1939(const std::string& network, j1939::NetworkOptions options = {})
    {
        require_network_name(network);
        return add_j1939(network, std::move(options));
    }
    /// A raw CAN channel of this ECU on the CAN network `network`, for a protocol that is not
    /// J1939: can::RawChannel (send() and on_frame(); no timers, no segmentation; use every()
    /// and on_step() for periodic work). It has a CAN controller of its own on that bus (Peer: a
    /// SIL Kit CAN controller named "<network>.raw"; testkit::LocalCan: an endpoint of the bus
    /// `network`). The same ECU may also have j1939(network) on that bus: they are two
    /// controllers, each receives the other's frames (the J1939 network ignores 11-bit frames),
    /// in the step after the one that sent them, like the frames of other ECUs. Not so for a
    /// Peer's J1939 network on a port the caller made (Peer::j1939(name, port), e.g. Vector XL):
    /// the raw channel is always on SIL Kit, so the two are on different buses.
    /// std::invalid_argument for an empty name, for a name this ECU used for a raw channel
    /// before and for a capacity of 0; std::logic_error once the run has started.
    can::RawChannel& can(const std::string& network, can::RawChannelOptions options = {})
    {
        require_network_name(network);
        return add_can(network, options);
    }
    /// Runs `fn` in every step of this ECU, after its networks handled what arrived and before
    /// they send (in the order added, together with every()). std::invalid_argument for an
    /// empty function; std::logic_error once the run has started.
    virtual void on_step(std::function<void(SimTime now)> fn) = 0;
    /// Runs `fn` in the first step at or after 0, period, 2 * period, ... (once per step at
    /// most; vehicle_sil::periodic() in core/periodic.hpp). std::invalid_argument for period <= 0
    /// or an empty function; std::logic_error once the run has started.
    virtual void every(SimDuration period, std::function<void(SimTime now)> fn) = 0;
    /// Constructs a T that the ECU owns: it is destroyed after the ECU's networks and raw
    /// channels (their handlers and parts may refer to it), the object emplaced last first. Use
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
    /// j1939() of the implementation, with a name that is not empty (a separate name: Peer has
    /// a j1939() of its own).
    virtual j1939::Network& add_j1939(const std::string& network, j1939::NetworkOptions options) = 0;
    /// can() of the implementation, with a name that is not empty.
    virtual can::RawChannel& add_can(const std::string& network, can::RawChannelOptions options) = 0;
    /// Takes ownership of an emplace()d object (std::logic_error once the run has started).
    virtual void keep(std::shared_ptr<void> object) = 0;
};

namespace detail {

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

} // namespace vehicle_sil
