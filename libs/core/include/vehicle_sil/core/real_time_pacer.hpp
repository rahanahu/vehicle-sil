// SPDX-License-Identifier: MIT
//
// Couples simulation time to the wall clock at a fixed ratio (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>

namespace vehicle_sil {

/// Computes how long a participant has to hold a simulation step so that simulation time
/// advances at `factor` times wall-clock speed: 1.0 is real time, 4.0 is four times faster than
/// real time, 0.5 is half real-time speed.
///
/// The first step anchors the two clocks: wall time w0 at simulation time s0. The step that
/// covers [T, T + step) may then finish no earlier than w0 + (T + step - s0) / factor. Pacing
/// only ever waits, so a participant that is slower than the target stays slower; it never
/// skips steps to catch up.
///
/// Because SIL Kit advances virtual time only when every participant has finished its step,
/// pacing one participant paces the whole simulation.
class RealTimePacer
{
public:
    using WallClock = std::chrono::steady_clock;

    static constexpr double kMinFactor = 1e-6;
    static constexpr double kMaxFactor = 1e6;

    /// @param factor Simulation seconds per wall-clock second, within [kMinFactor, kMaxFactor]
    ///        (outside that range the wall-clock deadlines would overflow or be meaningless).
    explicit RealTimePacer(double factor) : factor_(factor)
    {
        if (!(factor_ >= kMinFactor && factor_ <= kMaxFactor))
        {
            throw std::invalid_argument("RealTimePacer factor must be within [1e-6, 1e6]");
        }
    }

    /// Wall-clock time at which the step [step_start, step_start + step) may end.
    /// `wall_now` is the wall-clock time at which the step started; the first call anchors the
    /// clocks.
    [[nodiscard]] WallClock::time_point step_deadline(SimTime step_start, SimDuration step, WallClock::time_point wall_now)
    {
        if (!anchor_)
        {
            anchor_ = Anchor{step_start, wall_now};
        }
        const auto simElapsed = std::chrono::duration<double, std::nano>((step_start + step) - anchor_->sim);
        const auto wallElapsed = std::chrono::duration_cast<WallClock::duration>(simElapsed / factor_);
        return anchor_->wall + wallElapsed;
    }

    [[nodiscard]] double factor() const noexcept { return factor_; }

private:
    struct Anchor
    {
        SimTime sim;
        WallClock::time_point wall;
    };

    double factor_;
    std::optional<Anchor> anchor_;
};

} // namespace vehicle_sil
