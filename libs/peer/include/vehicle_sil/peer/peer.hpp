// SPDX-License-Identifier: MIT
//
// One SIL Kit participant with its J1939 networks and LIN nodes, assembled and run
// (vehicle_sil::Peer).
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/raw_channel.hpp"
#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/can/tap_port.hpp"
#include "vehicle_sil/can_silkit/silkit_can_port.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil {

namespace can {
class RealTimeDriver;
} // namespace can

class Peer;

namespace detail {

/// For vehicle_sil::Simulation only, which makes its peers before it knows the end of the run:
/// with virtual time, the peer runs no step that starts at or after `end` (the earlier of this
/// and VirtualTimeOptions::end applies). Unlike VirtualTimeOptions::end, those steps still take
/// part in the time synchronisation, but do nothing, so what the peer did does not depend on
/// when a controller's stop reaches it. Autonomous: the peer stops itself after its last step
/// before `end`. Only before start() and with virtual time (std::logic_error); `end` must be
/// after 0 (std::invalid_argument).
void set_virtual_time_end(Peer& peer, SimTime end);

/// The kind of a network of a peer: CAN (j1939(), can()) or LIN (lin_master(), lin_slave()).
enum class NetworkKind
{
    Can,
    Lin,
};

/// For vehicle_sil::Simulation only, which checks the network names across the peers of its
/// process: before the peer makes a CAN or LIN network, it calls `check` with the network's
/// name, its kind and the call (e.g. `lin_slave("LIN1")`); `check` refuses it by throwing. Only
/// before start() (std::logic_error).
void set_network_check(Peer& peer,
                       std::function<void(const std::string& network, NetworkKind kind, const std::string& call)> check);

/// The kind of the network named `network` of `peer`, if it has one.
[[nodiscard]] std::optional<NetworkKind> network_kind(const Peer& peer, std::string_view network);

} // namespace detail

struct PeerOptions
{
    std::string participant;
    std::string registry_uri = "silkit://localhost:8500";
    std::string participant_configuration = "{}";
    /// Coordinated: a SimulationController (another participant) starts and stops the
    /// simulation. Autonomous: the peer runs on its own until stop() or request_stop(), or with
    /// virtual time until virtual_time.end (it stops itself after its last step before the end).
    LifecycleMode lifecycle = LifecycleMode::Autonomous;
    /// Set: SIL Kit's time synchronisation steps the peer with this step (J1939 and LIN); an
    /// `end` must be after 0 (std::invalid_argument).
    /// Empty: real time; a RealTimeDriver steps the peer on its own thread (J1939 only; SIL Kit
    /// CAN ports in real-time mode, or external ports such as Vector XL).
    std::optional<VirtualTimeOptions> virtual_time = VirtualTimeOptions{};
    std::chrono::milliseconds shutdown_timeout{5000};
    /// Real time: the longest time between two steps (RealTimeDriverOptions::max_idle).
    SimDuration max_idle = std::chrono::milliseconds(10);
    /// Real time: the clock of the driver and the ports. nullptr: the peer uses a
    /// SteadyRealTimeClock of its own. Must outlive the peer.
    can::RealTimeClock* real_time_clock = nullptr;
};

struct J1939ChannelOptions
{
    j1939::NetworkOptions network{};
    /// The SIL Kit CAN port. real_time_clock is set by the peer.
    can::SilKitCanPortOptions port{};
    /// Name of the SIL Kit CAN controller. Empty: the network name.
    std::string controller_name;
};

/// One SIL Kit participant and what runs in it: J1939 networks on CAN, LIN masters and slaves,
/// and the application's work for each step. The peer creates the participant, the
/// controllers and the ports, steps everything (with SIL Kit's time synchronisation, or in real
/// time with a RealTimeDriver), initialises the LIN nodes when communication is ready, and tears
/// it all down in the right order. It is an Ecu: an application written against Ecu& runs on a
/// peer as it does on the local buses (Ecu::j1939(network) is j1939(network) with default port
/// options). Ecu::lin_master() and Ecu::lin_slave() make a SIL Kit LIN controller named like
/// the network, a lin::SilKitLinPort on it and the node (virtual time only).
///
///     vehicle_sil::Peer peer({.participant = "Engine", .registry_uri = uri});
///     auto& ecu = peer.j1939("J1939_CAN1").add_node({.address = 0x00, .name = name});
///     peer.every(100ms, [&](SimTime) { static_cast<void>(ecu.send(pgn, model.encode())); });
///     peer.start();
///     peer.wait();
///
/// One step at `now`, in this order (as Ecu states): the J1939 networks and the raw CAN channels
/// (Ecu::can()) receive and handle what arrived (in the order they were added), then the
/// application (on_step() and every(), in the order added), then the LIN nodes' step(now) (in
/// the order added), then the J1939 networks and raw channels send (in the same order). A
/// control agent's hooks (set_control()) run right before and after the application and LIN
/// stage. LIN requests made by the application (LinMaster::set_frame_data(),
/// LinSlave::set_response()) therefore take effect in the same step; what the LIN nodes receive
/// is reported in their step, and the application sees it in the next one. The LIN nodes are
/// initialised (init()) when communication is ready, before the first step.
///
/// Names: a network name is one kind of network (J1939 and raw CAN may share a CAN network; a
/// LIN network's name is its own), and no two SIL Kit controllers of the peer have one name
/// (std::invalid_argument).
///
/// Set up (j1939(), can(), lin_master(), lin_slave(), on_step(), every(), emplace(), and the
/// networks' nodes, handlers, parts, observers and tap listeners, the LIN nodes' handlers)
/// before start(); the set-up functions throw std::logic_error afterwards. Nodes may still be
/// added from a step, as Network::add_node() allows.
///
/// Ownership: the peer owns the participant, the controllers, the ports, the networks, the
/// LIN nodes and the objects made by emplace(); references it returns are valid as long as the
/// peer. What the handlers refer to (models) must outlive the networks: emplace() it, or
/// declare it before the peer. stop() (and the destructor) stops the steps (real time: the
/// driver, then the lifecycle; virtual time: the lifecycle) and closes the SIL Kit ports; the
/// destructor then destroys the LIN nodes (each before its port), the networks (their parts
/// first), the taps and the
/// ports (an external port closes in its destructor), the raw CAN channels and their ports, the
/// host, the participant, and last the emplace()d objects (the one emplaced last first). So the
/// emplace()d objects go after the participant: their destructors must not use SIL Kit
/// services (the participant, controllers or Pub/Sub made through runtime()); make an object
/// that does so after the peer instead, so that it goes first.
///
/// Threading: set up, start() and wait() from one thread. The steps run on SIL Kit's thread
/// (virtual time) or on the driver's thread (real time); handlers, the application, observers
/// and tap listeners run there, so read what they record after wait() or stop(). stop() from
/// any thread but a step or a SIL Kit handler (use request_stop() there), and from another
/// thread only after start() returned; concurrent stop() calls are serialised. Destroy the peer
/// only after a stop() of another thread has returned. failed() and error() from any thread,
/// also from a step.
class Peer : public Ecu
{
public:
    /// Connects to the registry (SilKit::SilKitError on failure).
    explicit Peer(PeerOptions options);
    ~Peer() override;
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    /// A J1939 network on a SIL Kit CAN controller of network `network_name`. Its port is
    /// wrapped in a TapPort (tap()). std::invalid_argument for an empty name, a name used
    /// before (also a LIN network's), or a controller name (J1939ChannelOptions::controller_name,
    /// else the network name) that a controller of this peer has, also a raw channel's
    /// "<network>.raw" and a LIN controller's.
    j1939::Network& j1939(std::string network_name, J1939ChannelOptions options = {});
    /// A J1939 network on a port the caller made (e.g. a Vector XL ExternalPortAdapter with
    /// real_time_clock()), owned by the peer from now on and destroyed before the participant.
    /// The port's time domain must match the peer's (real time: TimeDomain::RealTime);
    /// std::logic_error otherwise. std::invalid_argument for a name used before (also a LIN
    /// network's).
    j1939::Network& j1939(std::string name, std::unique_ptr<can::Port> port, j1939::NetworkOptions options = {});
    /// Runs `fn` in every step (see the class comment for the order).
    void on_step(std::function<void(SimTime now)> fn) override;
    /// Runs `fn` in the first step at or after 0, period, 2 * period, ... (once per step at
    /// most; a step that passes several of them runs it once; vehicle_sil::periodic()).
    /// std::invalid_argument for period <= 0 or an empty function.
    void every(SimDuration period, std::function<void(SimTime now)> fn) override;

    /// Hooks for a control agent (vehicle_sil::control::attach()): `before` runs in every step
    /// just before the application (before on_step() and every(), whenever they were added),
    /// `after` just after the LIN nodes' step, before the J1939 networks send, and `stopping`
    /// once when stop() (or the destructor) begins. Any may be empty. Once only, before start()
    /// (std::logic_error).
    void set_control(std::function<void(SimTime now)> before, std::function<void(SimTime now)> after,
                     std::function<void()> stopping = {});

    /// std::out_of_range for an unknown name.
    [[nodiscard]] j1939::Network& network(std::string_view name) const;
    /// The names of the J1939 networks, in the order they were added.
    [[nodiscard]] std::vector<std::string> network_names() const;
    [[nodiscard]] can::TapPort& tap(std::string_view name) const;
    /// The SIL Kit port of a network made by j1939(network_name); nullptr for an external port.
    [[nodiscard]] can::SilKitCanPort* silkit_port(std::string_view name) const;
    /// The raw CAN channel on the network `name` (Ecu::can(name)). std::out_of_range for an
    /// unknown name.
    [[nodiscard]] can::RawChannel& raw_channel(std::string_view name) const;

    [[nodiscard]] ParticipantRuntime& runtime() noexcept { return *runtime_; }
    [[nodiscard]] can::Host& host() noexcept { return host_; }
    [[nodiscard]] bool uses_virtual_time() const noexcept { return options_.virtual_time.has_value(); }
    /// Real time: the clock of the driver and the ports (for external ports). nullptr with
    /// virtual time.
    [[nodiscard]] can::RealTimeClock* real_time_clock() const noexcept { return clock_; }

    /// Starts the lifecycle and waits until the participant runs (ParticipantRuntime::start()),
    /// then, in real time, starts the driver. Once only (std::logic_error).
    void start(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    /// Waits until the lifecycle ends (ParticipantRuntime::wait_until_finished()). Real time:
    /// then stops the driver.
    std::optional<SilKit::Services::Orchestration::ParticipantState> wait(std::chrono::milliseconds timeout = std::chrono::minutes(10));
    /// start(), then wait().
    std::optional<SilKit::Services::Orchestration::ParticipantState> run(std::chrono::milliseconds timeout = std::chrono::minutes(10));
    /// Stops the steps (real time: the driver first) and the lifecycle, waits for it and closes
    /// the SIL Kit ports. Idempotent; never throws. The networks stay for reading.
    void stop(std::string_view reason = "stopped") noexcept;
    /// Asks the lifecycle to stop without waiting (also from a step).
    void request_stop(std::string_view reason = "stop requested") noexcept;

    /// A step failed: the host (a network, the application, a LIN node, a port), the step
    /// handler or the real-time driver threw.
    [[nodiscard]] bool failed() const;
    /// Why (empty if not failed).
    [[nodiscard]] std::string error() const;

private:
    friend void detail::set_virtual_time_end(Peer& peer, SimTime end);
    friend void detail::set_network_check(
        Peer& peer, std::function<void(const std::string& network, detail::NetworkKind kind, const std::string& call)> check);
    friend std::optional<detail::NetworkKind> detail::network_kind(const Peer& peer, std::string_view network);

    struct Channel;
    struct RawCan;
    struct LinNode;
    j1939::Network& add_j1939(const std::string& network, j1939::NetworkOptions options) override;
    /// Ecu::can(): a raw channel on a SIL Kit CAN controller of its own, named "<network>.raw"
    /// (std::invalid_argument if a controller of this peer has that name).
    can::RawChannel& add_can(const std::string& network, can::RawChannelOptions options) override;
    /// Ecu::lin_master() and Ecu::lin_slave(): a SIL Kit LIN controller named `network`, its
    /// lin::SilKitLinPort and the node (virtual time only; std::logic_error in real time).
    lin::LinMaster& add_lin_master(const std::string& network, lin::LinScheduleTable schedule,
                                   lin::LinMasterOptions options) override;
    lin::LinSlave& add_lin_slave(const std::string& network, lin::LinSlaveOptions options) override;
    /// Checks the name of a new LIN network `network` for the call `what` (lin_master() or
    /// lin_slave()): std::invalid_argument for a name or a controller name in use, and what the
    /// network check (detail::set_network_check()) refuses.
    void require_lin_name(const std::string& network, const char* what) const;
    /// Adds a LIN node on the LIN network `network` (checked by require_lin_name()), with its SIL
    /// Kit controller and port. Check the node's arguments first: the controller cannot be
    /// removed again. The caller makes the master or slave on the port and removes the node if
    /// that throws.
    LinNode& add_lin_node(const std::string& network, lin::LinRole role);
    /// Checks the name of a new CAN network `network` for the call `what` (j1939 or can):
    /// std::invalid_argument if it is the name of a LIN network of this peer, and what the
    /// network check (detail::set_network_check()) refuses.
    void require_can_name(const std::string& network, const char* what) const;
    void keep(std::shared_ptr<void> object) override;
    void require_not_started(const char* what) const override;
    /// detail::set_virtual_time_end().
    void set_end(SimTime end);
    /// true if a SIL Kit CAN controller of this peer (a J1939 network's or a raw channel's) has
    /// the name `controller`. A LIN controller is named like its LIN network (lin_).
    [[nodiscard]] bool controller_is_used(const std::string& controller) const;
    void require_virtual_time(const char* what) const;
    Channel& add_channel(std::string name);
    [[nodiscard]] Channel& channel(std::string_view name) const;
    void run_application(SimTime now);

    /// First: the emplace()d objects go after everything else.
    detail::OwnedObjects owned_;
    PeerOptions options_;
    /// The end of the handled steps (VirtualTimeOptions::end or set_end()); virtual time only.
    std::optional<SimTime> end_;
    std::unique_ptr<ParticipantRuntime> runtime_;
    std::unique_ptr<can::RealTimeClock> own_clock_;
    can::RealTimeClock* clock_ = nullptr;
    can::Host host_;
    std::vector<std::unique_ptr<Channel>> channels_;
    /// The raw CAN channels (Ecu::can()); not in channels_, so network() and the other lookups
    /// by name are J1939 only (raw_channel() looks here).
    std::vector<std::unique_ptr<RawCan>> raw_channels_;
    /// The LIN nodes, in the order added.
    std::vector<std::unique_ptr<LinNode>> lin_;
    /// detail::set_network_check(); empty: none.
    std::function<void(const std::string& network, detail::NetworkKind kind, const std::string& call)> network_check_;
    std::vector<std::function<void(SimTime)>> step_functions_;
    std::function<void(SimTime)> control_before_;
    std::function<void(SimTime)> control_after_;
    std::function<void()> control_stopping_;
    bool control_set_ = false;
    std::mutex mutex_; ///< serialises stop(); guards driver_ and stopping_ against start()
    std::unique_ptr<can::RealTimeDriver> driver_;
    /// driver_ once it runs (set once in start()), for failed(), error() and wait() without the
    /// lock: a step may call them while stop() holds the lock and waits for the step.
    std::atomic<can::RealTimeDriver*> running_driver_{nullptr};
    bool stopping_ = false;
    bool started_ = false;
};

/// Starts `peers` and the controller together, each peer from its own thread (as separate
/// processes would), and waits until every start has returned. Use it for Coordinated peers in
/// one process. Rethrows the controller's exception, else the first of a peer's start(); joins
/// every thread first.
void start_together(SimulationController& controller, const std::vector<Peer*>& peers,
                    std::chrono::milliseconds timeout = std::chrono::seconds(30));
/// The same without a controller: a controller elsewhere (another process or thread) starts
/// the simulation. Rethrows the first exception of a peer's start(); joins every thread first.
void start_together(const std::vector<Peer*>& peers, std::chrono::milliseconds timeout = std::chrono::seconds(30));

} // namespace vehicle_sil
