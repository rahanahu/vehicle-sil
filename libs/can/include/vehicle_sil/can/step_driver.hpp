// SPDX-License-Identifier: MIT
//
// Drives hosts on virtual time in a test loop, without SIL Kit (vehicle_sil::can).
#pragma once

#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <cstddef>
#include <vector>

namespace vehicle_sil::can {

/// Steps several hosts, each with its own fixed step size, on one virtual timeline.
///
/// The steps run in the order (start time, registration order). That is one of the orders SIL
/// Kit's time synchronisation allows: the step at time T of a host runs only after every step
/// before T of every host. With a virtual bus, a frame sent in the step at t reaches a receiver
/// in its first step after t, so the order between steps that start at the same time does not
/// change the result.
///
/// Every host starts at T = 0. With SIL Kit, ParticipantRuntime drives the host instead (see vehicle_sil::can::bind_host in vehicle_sil::can_silkit).
///
/// Not thread safe; the hosts are stepped on the calling thread.
class StepDriver
{
public:
    /// Registers `host` with step size `step` (> 0), requires TimeDomain::Virtual of its ports
    /// (Host::require_time_domain(); std::logic_error for real-time ports) and passes the step
    /// size to Host::set_step_size(). Only before the first run_until().
    void add(Host& host, SimDuration step);

    /// Runs every step that starts before `end` and has not run yet. Later calls continue from
    /// there. Exceptions from a host propagate (that host is failed); a later call throws again.
    void run_until(SimTime end);

    /// Start time of the next step that would run (the earliest over all hosts).
    [[nodiscard]] SimTime next_step_time() const;

private:
    struct Entry
    {
        Host* host;
        SimDuration step;
        SimTime next;
    };

    std::vector<Entry> entries_;
    bool started_ = false;
};

} // namespace vehicle_sil::can
