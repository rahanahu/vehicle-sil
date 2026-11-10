// SPDX-License-Identifier: MIT
//
// The rule of periodic work in fixed steps: every(period) of Peer and of the test kit's ECUs
// (vehicle_sil::sim_base, no SIL Kit).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <functional>
#include <stdexcept>
#include <utility>

namespace vehicle_sil {

/// Wraps `fn` so that it runs in the first step at or after 0, period, 2 * period, ... (once
/// per step at most; a step that passes several of them runs it once). Call the result with
/// the start time of every step, in increasing order. std::invalid_argument for period <= 0 or
/// an empty function.
///
/// The period need not be a multiple of the step: with steps of 10 and a period of 25, `fn` runs
/// at 0, 30, 50, 80, 100, ...
[[nodiscard]] inline std::function<void(SimTime)> periodic(SimDuration period, std::function<void(SimTime)> fn)
{
    if (period <= SimDuration::zero())
    {
        throw std::invalid_argument("vehicle_sil::periodic: the period must be > 0");
    }
    if (!fn)
    {
        throw std::invalid_argument("vehicle_sil::periodic: empty function");
    }
    return [period, due = SimTime{}, fn = std::move(fn)](SimTime now) mutable {
        if (now < due)
        {
            return;
        }
        // Skip the times this step passed: once per step.
        due += period * ((now - due) / period + 1);
        fn(now);
    };
}

} // namespace vehicle_sil
