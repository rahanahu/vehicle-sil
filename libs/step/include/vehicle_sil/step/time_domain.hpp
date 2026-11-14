// SPDX-License-Identifier: MIT
//
// The time base of a port and the wakeup of a real-time driver (vehicle_sil::step).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <functional>

namespace vehicle_sil::step {

/// The time base a port's times belong to.
enum class TimeDomain
{
    /// Simulation time that only advances step by step (SIL Kit time synchronisation, the
    /// virtual bus). Deterministic.
    Virtual,
    /// Wall-clock time of a real-time driver (SIL Kit without time synchronisation, Vector XL).
    RealTime,
};

/// Wakes a real-time driver because something arrived with time `arrival` (the driver's next
/// step is later than `arrival`, so the port can release it). Called from any thread, with the
/// port's lock held: it must return quickly and must not call back into the port or the host.
using Wakeup = std::function<void(SimTime arrival)>;

} // namespace vehicle_sil::step
