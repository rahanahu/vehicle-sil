// SPDX-License-Identifier: MIT
//
// Simulation (virtual) time, kept apart from wall-clock time by type (vehicle_sil::core).
#pragma once

#include <chrono>
#include <cstdint>
#include <ratio>

namespace vehicle_sil {

/// Clock of the SIL Kit virtual time. It is not a wall clock: there is no now(). The current
/// simulation time comes from the time synchronisation (ParticipantRuntime::now() or the step
/// handler's argument). Using a distinct clock type makes SimTime incompatible with
/// std::chrono::steady_clock / system_clock time points at compile time.
struct SimClock
{
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<SimClock, duration>;
    static constexpr bool is_steady = true;
};

/// A point in simulation time. SimTime{} is the start of the simulation (T = 0).
using SimTime = SimClock::time_point;

/// A span of simulation time (nanoseconds, the resolution of SIL Kit).
using SimDuration = SimClock::duration;

/// SIL Kit passes simulation time as nanoseconds since the start of the simulation.
[[nodiscard]] constexpr SimTime from_silkit_time(std::chrono::nanoseconds silkit_time) noexcept
{
    return SimTime{silkit_time};
}

[[nodiscard]] constexpr std::chrono::nanoseconds to_silkit_time(SimTime time) noexcept
{
    return time.time_since_epoch();
}

/// true if `d` is a non-negative multiple of `step` (`step` > 0), i.e. a time offset that a
/// participant with this step size can hit exactly.
[[nodiscard]] constexpr bool on_step_grid(SimDuration d, SimDuration step) noexcept
{
    return d >= SimDuration::zero() && (d % step) == SimDuration::zero();
}

/// Nanoseconds since the start of the simulation, for logs and traces.
[[nodiscard]] constexpr std::int64_t to_ns(SimTime time) noexcept
{
    return time.time_since_epoch().count();
}

} // namespace vehicle_sil
