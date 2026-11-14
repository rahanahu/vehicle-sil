// SPDX-License-Identifier: MIT
//
// The shapes a scenario step writes over time (Waveform, scenario.hpp): the line of a ramp and
// the shapes of a wave. Each is a function of the virtual time since its step began; the numbers
// come from vehicle_sil::signal (signal/waveform.hpp), which a participant can use as well.
//
// "The base" below is Waveform's: the value the variable has when the step begins, or the one the
// step names (a ramp's from, a wave's around).
#pragma once

#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/signal/waveform.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace vehicle_sil::launch::detail {

/// What repeats: its period and where in it the shape begins.
struct Periodic
{
    SimDuration period{}; ///< > 0
    double phase_deg = 0.0;
};

/// The line from the base to `to`, reached at `length`; `to` at the end.
[[nodiscard]] std::shared_ptr<const Waveform> ramp_shape(double to, SimDuration length);

// Around the base, and the base at the end.
[[nodiscard]] std::shared_ptr<const Waveform> sine_shape(double amplitude, Periodic periodic);
[[nodiscard]] std::shared_ptr<const Waveform> triangle_shape(double amplitude, Periodic periodic);
[[nodiscard]] std::shared_ptr<const Waveform> sawtooth_shape(double amplitude, Periodic periodic, bool falling);
/// base + amplitude while high, base - amplitude while low.
[[nodiscard]] std::shared_ptr<const Waveform> square_shape(double amplitude, Periodic periodic, double duty_percent);
/// `amplitude` is the largest distance from the base, or the standard deviation if `gaussian`.
[[nodiscard]] std::shared_ptr<const Waveform> noise_shape(double amplitude, SimDuration hold, std::uint64_t seed, bool gaussian);
[[nodiscard]] std::shared_ptr<const Waveform> chirp_shape(double amplitude, double from_hz, double to_hz, SimDuration length,
                                                          signal::Sweep sweep);
/// `value` all the time, the base at the end.
[[nodiscard]] std::shared_ptr<const Waveform> pulse_shape(double value);

/// `high` and `low` as they are, and `low` at the end. Does not use the base.
[[nodiscard]] std::shared_ptr<const Waveform> square_levels_shape(double low, double high, Periodic periodic, double duty_percent);

/// From the base towards `to` as a first-order lag does. At the end (`length`): where it has
/// come to, or `to` if `settle`.
[[nodiscard]] std::shared_ptr<const Waveform> lag_shape(double to, SimDuration tau, SimDuration length, bool settle);
/// `to` and the base in turn, beginning with `to` (signal::chatter()); `to` at the end.
[[nodiscard]] std::shared_ptr<const Waveform> chatter_shape(double to, SimDuration interval, std::optional<std::uint64_t> seed);
/// A table of values at times since the step began, in the order of the times: on lines between
/// them, or each kept until the next (`hold`). Before the first point: on the line from the base
/// at 0, or the base. The last point's value at the end. Uses the base only if the first point is
/// after 0.
[[nodiscard]] std::shared_ptr<const Waveform> points_shape(std::vector<signal::Point> points, bool hold);

} // namespace vehicle_sil::launch::detail
