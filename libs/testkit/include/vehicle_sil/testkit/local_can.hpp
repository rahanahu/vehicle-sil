// SPDX-License-Identifier: MIT
//
// Virtual CAN buses with ECUs and their J1939 networks on them, stepped on virtual time in the
// calling thread: the test setup without SIL Kit (vehicle_sil::testkit).
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/tap_port.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::testkit {

/// Virtual CAN buses (VirtualCanBus), one per network name, and ECUs on them, each ECU on its
/// own host with its own step size, the way separate SIL Kit participants would be. A
/// StepDriver runs them in the calling thread; nothing reads a clock.
///
/// An ECU (add_ecu()) is a vehicle_sil::Ecu: Ecu::j1939(network) attaches an endpoint to the
/// bus named `network`, wraps it in a TapPort and puts a J1939 network on it; the ECUs that name
/// the same network share its bus. Ecu::can(network) (a raw CAN channel) attaches an endpoint of
/// its own to that bus, so an ECU with j1939(network) and can(network) has two endpoints on it,
/// and each receives the other's frames. A bus is made when it is first named, with the
/// constructor's options.
///
/// The unnamed bus: add_j1939(name, step) is an ECU with one J1939 network on a bus without a
/// name (the bus "" in the lookups by name), and inject(time, frame), injector() and bus() use
/// that bus. A LocalCan uses either the unnamed bus or named buses, never both, so that a frame
/// cannot go to a bus no ECU is on: what is used first decides (add_j1939() and those three: the
/// unnamed bus; Ecu::j1939(network) and Ecu::can(network): named buses), and the other kind then
/// throws std::logic_error. An add_j1939(), j1939() or can() that throws decides nothing. Raw
/// channels are on named buses only (Ecu::can("") is std::invalid_argument, like j1939("")).
///
/// Look them up by the ECU's name and the network's: network() for the stack, endpoint() for
/// fault injection and outages (VirtualCanBus::Endpoint), tap() to listen to its raw frames,
/// host() for the ECU's host. The one-name forms are for an ECU with exactly one network (as
/// add_j1939() makes; also an ECU with one named network). These lookups are for the J1939
/// networks only: a raw channel is not one of them and does not count in the one-name forms.
/// inject() sends a raw frame (11 or 29 bit) from an endpoint of its own on a bus, which no
/// network uses (injector()).
///
/// Set up (add_ecu(), add_j1939(), on_step(), and the ECUs' j1939(), can(), on_step(), every()
/// and emplace()) before the first run; afterwards they throw std::logic_error. Times: every host
/// starts at 0; run_until(t) runs every step that starts before t. A frame injected with time t
/// reaches a network in its first step after t; t must not be earlier than now(), the end of the
/// last run (the bus refuses the past: std::logic_error).
///
/// One step of an ECU at `now`: its networks and raw channels receive and handle what arrived (in
/// the order added), then on_step() and every() (in the order added), then the networks and raw
/// channels send. A frame one network of the ECU receives can be sent on to another network (or
/// raw channel) of it in the same step.
///
/// Ownership: LocalCan owns the buses, the endpoints, the taps, the networks, the raw channels,
/// the hosts and the objects the ECUs emplace(). It destroys the ECUs last added first, each
/// with its host first, then its networks (the one added last first; each network's parts
/// first, then its tap), then its raw channels (the one added last first), then the objects it
/// emplace()d (the one emplaced last first); then the driver, and last the buses with the
/// endpoints. References it returns are valid as long as the LocalCan. Observers and tap
/// listeners may outlive it (they are detached).
///
/// Not thread safe.
class LocalCan
{
public:
    explicit LocalCan(can::VirtualCanBusOptions options = {});
    ~LocalCan();
    LocalCan(const LocalCan&) = delete;
    LocalCan& operator=(const LocalCan&) = delete;

    /// Adds an ECU stepped every `step` (> 0) on a host of its own and returns it.
    /// std::invalid_argument for an empty or a used name, or step <= 0; std::logic_error after
    /// the first run.
    Ecu& add_ecu(std::string name, SimDuration step);
    /// An ECU stepped every `step` with one J1939 network on the unnamed bus (named "" in the
    /// lookups). std::logic_error if this LocalCan uses named buses (use add_ecu(name,
    /// step).j1939(network) then), otherwise as add_ecu() and Ecu::j1939(). Nothing is added if
    /// it throws.
    j1939::Network& add_j1939(std::string name, SimDuration step, j1939::NetworkOptions options = {});
    /// The ECU's on_step(): work for the application stage of its host (in the order added,
    /// together with its every()), after its networks handled what they received. Send from
    /// here. std::out_of_range for an unknown name.
    void on_step(std::string_view name, std::function<void(SimTime now)> fn);

    /// The ECU of that name. std::out_of_range for an unknown name.
    [[nodiscard]] Ecu& ecu(std::string_view name) const;
    /// The names of the ECUs, in the order they were added.
    [[nodiscard]] std::vector<std::string> ecu_names() const;
    /// The network `network` of the ECU `ecu`. std::out_of_range for an unknown name.
    [[nodiscard]] j1939::Network& network(std::string_view ecu, std::string_view network) const;
    [[nodiscard]] can::VirtualCanBus::Endpoint& endpoint(std::string_view ecu, std::string_view network) const;
    [[nodiscard]] can::TapPort& tap(std::string_view ecu, std::string_view network) const;
    /// The same for the only network of the ECU `ecu`: std::out_of_range for an unknown ECU,
    /// std::logic_error if it has none or more than one.
    [[nodiscard]] j1939::Network& network(std::string_view ecu) const;
    [[nodiscard]] can::VirtualCanBus::Endpoint& endpoint(std::string_view ecu) const;
    [[nodiscard]] can::TapPort& tap(std::string_view ecu) const;
    /// The ECU's host (std::out_of_range for an unknown name).
    [[nodiscard]] can::Host& host(std::string_view ecu) const;

    /// Sends `frame` at `time` from the injection endpoint of the unnamed bus (the result of its
    /// send()). std::logic_error if this LocalCan uses named buses (use inject(network, time,
    /// frame) then).
    can::SendResult inject(SimTime time, const can::Frame& frame);
    /// The same on the bus `network` (std::out_of_range for an unknown bus).
    can::SendResult inject(std::string_view network, SimTime time, const can::Frame& frame);
    /// The endpoint inject() sends from on the unnamed bus: set a fault injector or an outage on
    /// it like on any other. It receives every frame on the bus; run_until() discards them.
    /// std::logic_error if this LocalCan uses named buses (use injector(network) then).
    [[nodiscard]] can::VirtualCanBus::Endpoint& injector();
    /// The same on the bus `network` (std::out_of_range for an unknown bus).
    [[nodiscard]] can::VirtualCanBus::Endpoint& injector(std::string_view network) const;
    /// The unnamed bus. std::logic_error if this LocalCan uses named buses (use bus(network)
    /// then).
    [[nodiscard]] can::VirtualCanBus& bus();
    /// The bus `network` (std::out_of_range for an unknown bus).
    [[nodiscard]] can::VirtualCanBus& bus(std::string_view network);

    /// Runs every step that starts before `end` and has not run yet. Exceptions of a host
    /// propagate (that host is failed: see host()).
    void run_until(SimTime end);
    /// run_until(now() + duration).
    void run_for(SimDuration duration);
    /// Where the runs got to: the `end` of the last run_until() (0 before).
    [[nodiscard]] SimTime now() const noexcept { return now_; }

private:
    class Site;
    struct Channel;
    struct RawCan;
    struct NamedBus;
    [[nodiscard]] Site& site(std::string_view name) const;
    [[nodiscard]] NamedBus& named_bus(std::string_view network) const;
    /// Throws std::logic_error (for `what`) if the bus `network` ("": the unnamed bus) is not of
    /// the kind this LocalCan uses; decides nothing.
    void require_bus_kind(std::string_view network, std::string_view what) const;
    /// The bus `network` ("": the unnamed bus), made now if it does not exist; decides the kind
    /// of buses this LocalCan uses (require_bus_kind() first).
    NamedBus& bus_for(const std::string& network, std::string_view what);

    /// Which buses this LocalCan uses: decided by the first use.
    enum class BusKind
    {
        Undecided,
        Unnamed,
        Named,
    };

    can::VirtualCanBusOptions options_;
    /// The buses, in the order made: the unnamed one only, or named ones.
    std::vector<std::unique_ptr<NamedBus>> buses_;
    BusKind bus_kind_ = BusKind::Undecided;
    std::vector<std::unique_ptr<Site>> sites_;
    can::StepDriver driver_;
    SimTime now_{};
    bool started_ = false;
};

} // namespace vehicle_sil::testkit
