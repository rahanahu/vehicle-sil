// SPDX-License-Identifier: MIT
//
// A virtual CAN bus with J1939 networks on it, stepped on virtual time in the calling thread:
// the test setup without SIL Kit (vehicle_sil::testkit).
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/tap_port.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"
#include "vehicle_sil/j1939/network.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::testkit {

/// One virtual CAN bus (VirtualCanBus) and the J1939 networks on it, each on its own host with
/// its own step size, the way separate SIL Kit participants would be. A StepDriver runs them
/// in the calling thread; nothing reads a clock.
///
/// Each network gets an endpoint of the bus wrapped in a TapPort. Look them up by the name
/// given to add_j1939(): network() for the stack, endpoint() for fault injection and outages
/// (VirtualCanBus::Endpoint), tap() to listen to its raw frames. inject() sends a raw frame (11
/// or 29 bit) from an endpoint of its own, which no network uses.
///
/// Set up (add_j1939(), on_step()) before the first run; afterwards they throw
/// std::logic_error. Times: every host starts at 0; run_until(t) runs every step that starts
/// before t. A frame injected with time t reaches a network in its first step after t; t must
/// not be earlier than now(), the end of the last run (the bus refuses the past:
/// std::logic_error).
///
/// Ownership: LocalCan owns the bus, the endpoints, the taps, the networks and the hosts. It
/// destroys the networks last added first, each with its host first, then the network (its
/// parts first), then its tap; the bus and the endpoints go last. References it returns are
/// valid as long as the LocalCan. Observers and tap listeners may outlive it (they are
/// detached).
///
/// Not thread safe.
class LocalCan
{
public:
    explicit LocalCan(can::VirtualCanBusOptions options = {});
    ~LocalCan();
    LocalCan(const LocalCan&) = delete;
    LocalCan& operator=(const LocalCan&) = delete;

    /// Adds a J1939 network stepped every `step` (> 0) and returns it. std::invalid_argument for
    /// an empty or a used name.
    j1939::Network& add_j1939(std::string name, SimDuration step, j1939::NetworkOptions options = {});
    /// Adds work to the application stage of that network's host (in the order added): it runs
    /// in every step, after the network handled what it received. Send from here.
    void on_step(std::string_view name, std::function<void(SimTime now)> fn);

    /// std::out_of_range for an unknown name.
    [[nodiscard]] j1939::Network& network(std::string_view name) const;
    [[nodiscard]] can::VirtualCanBus::Endpoint& endpoint(std::string_view name) const;
    [[nodiscard]] can::TapPort& tap(std::string_view name) const;
    [[nodiscard]] can::Host& host(std::string_view name) const;

    /// Sends `frame` at `time` from the injection endpoint (the result of its send()).
    can::SendResult inject(SimTime time, const can::Frame& frame);
    /// The endpoint inject() sends from: set a fault injector or an outage on it like on any
    /// other. It receives every frame on the bus; run_until() discards them.
    [[nodiscard]] can::VirtualCanBus::Endpoint& injector() const noexcept { return injector_; }
    [[nodiscard]] can::VirtualCanBus& bus() noexcept { return bus_; }

    /// Runs every step that starts before `end` and has not run yet. Exceptions of a host
    /// propagate (that host is failed: see host()).
    void run_until(SimTime end);
    /// run_until(now() + duration).
    void run_for(SimDuration duration);
    /// Where the runs got to: the `end` of the last run_until() (0 before).
    [[nodiscard]] SimTime now() const noexcept { return now_; }

private:
    struct Channel;
    [[nodiscard]] Channel& channel(std::string_view name) const;

    can::VirtualCanBus bus_;
    can::VirtualCanBus::Endpoint& injector_;
    std::vector<std::unique_ptr<Channel>> channels_;
    can::StepDriver driver_;
    SimTime now_{};
    bool started_ = false;
};

} // namespace vehicle_sil::testkit
