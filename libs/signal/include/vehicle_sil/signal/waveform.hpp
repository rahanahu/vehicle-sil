// SPDX-License-Identifier: MIT
//
// The usual test signals as functions of time: sine, triangle, sawtooth, square, noise, chirp,
// first-order lag, contact bounce and a table of points. Header only, standard library only.
//
// Every function is a pure function of the time `t` since the signal began (and of its
// parameters): it keeps no state, so it gives the same value however often and in whatever order
// it is asked. A simulation that reads one at the times of its steps therefore gets the same
// values with any step, at the times the steps share.
//
// The periodic ones return a factor (-1 to 1, or a bool): scale it yourself,
//
//     double throttle = 50.0 + 5.0 * signal::sine(t, 200ms);
//
// Across compilers and operating systems: triangle, sawtooth, square, the noise, chatter and the
// points use only integer arithmetic and +, -, *, / of doubles, which give the same bits
// everywhere (as long as the compiler does not contract a*b+c into one instruction, which the
// usual x86-64 builds do not). sine, chirp and lag call std::sin and std::expm1, whose last bit
// may differ between C libraries.
//
// Arguments outside what a function is for never divide by zero and never trap; each function
// says what it returns then (mostly the middle, 0). A period, hold, interval or time constant of
// 0 or less is such an argument, as is a phase that is not a finite number (taken as 0).
//
// vehicle_sil_launch's scenarios use these for their "wave" steps (docs/scenario.md).
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>

namespace vehicle_sil::signal {

using Nanos = std::chrono::nanoseconds;

namespace detail {

/// Where `t` is in its period, in nanoseconds from the period's start: 0 to period - 1. The phase
/// moves the start: 90 degrees begins a quarter of a period in. Any `t`. A `period` of 0 or less: 0.
[[nodiscard]] inline std::int64_t position(Nanos t, Nanos period, double phase_deg) noexcept
{
    const std::int64_t p = period.count();
    if (p <= 0)
    {
        return 0;
    }
    double turns = std::isfinite(phase_deg) ? phase_deg / 360.0 : 0.0;
    turns -= std::floor(turns); // 0 to 1
    const std::int64_t shift = static_cast<std::int64_t>(std::llround(turns * static_cast<double>(p))) % p;
    const std::int64_t pos = (t.count() % p + p) % p;
    return pos >= p - shift ? pos - (p - shift) : pos + shift;
}

/// The same as a share of the period: 0 (inclusive) to 1 (exclusive). A `period` of 0 or less: 0.
[[nodiscard]] inline double cycle(Nanos t, Nanos period, double phase_deg) noexcept
{
    return period.count() <= 0 ? 0.0 : static_cast<double>(position(t, period, phase_deg)) / static_cast<double>(period.count());
}

/// A mixing function of 64 bits (the finalizer of SplitMix64).
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t x) noexcept
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

} // namespace detail

/// The `index`-th number of the sequence of `seed` (and of `stream`: sequences of one seed that
/// do not follow each other): 0 (inclusive) to 1 (exclusive), evenly spread. The same on every
/// platform. Not for cryptography.
[[nodiscard]] constexpr double unit_random(std::uint64_t seed, std::uint64_t index, std::uint64_t stream = 0) noexcept
{
    const std::uint64_t h = detail::mix(detail::mix(seed ^ detail::mix(stream)) ^ (index * 0xD6E8FEB86659FD93ULL + stream));
    return static_cast<double>(h >> 11) * 0x1.0p-53;
}

/// Where `t` is in its period: from 0 (the period's start) to just under `period`. The phase
/// moves the start: with 90 degrees the time 0 is a quarter of a period in. Any `t`, also before
/// 0. A `period` of 0 or less: 0. (What sine(), square() and the others read their value at.)
[[nodiscard]] inline Nanos time_in_period(Nanos t, Nanos period, double phase_deg = 0.0) noexcept
{
    return Nanos{detail::position(t, period, phase_deg)};
}

/// A sine: 0 at the start, 1 after a quarter of the period, -1 after three quarters. A `period`
/// of 0 or less: 0.
[[nodiscard]] inline double sine(Nanos t, Nanos period, double phase_deg = 0.0) noexcept
{
    return std::sin(2.0 * std::numbers::pi * detail::cycle(t, period, phase_deg));
}

/// A triangle with the sine's corners: 0 at the start, 1 after a quarter of the period, -1 after
/// three quarters, straight lines between. A `period` of 0 or less: 0.
[[nodiscard]] inline double triangle(Nanos t, Nanos period, double phase_deg = 0.0) noexcept
{
    const double x = detail::cycle(t, period, phase_deg);
    return x < 0.25 ? 4.0 * x : x < 0.75 ? 2.0 - 4.0 * x : 4.0 * x - 4.0;
}

/// A sawtooth: -1 at the start of a period, rising in a straight line towards 1 at its end, then
/// back to -1 at once. `falling`: from 1 down towards -1. A `period` of 0 or less: 0.
[[nodiscard]] inline double sawtooth(Nanos t, Nanos period, bool falling = false, double phase_deg = 0.0) noexcept
{
    if (period.count() <= 0)
    {
        return 0.0;
    }
    const double x = 2.0 * detail::cycle(t, period, phase_deg) - 1.0;
    return falling ? -x : x;
}

/// How long a square of this period and duty (percent, taken as 0 to 100) is high in each period.
/// A `period` of 0 or less: no time.
[[nodiscard]] inline Nanos square_high_time(Nanos period, double duty_percent) noexcept
{
    if (period.count() <= 0)
    {
        return Nanos{0};
    }
    const double duty = duty_percent > 100.0 ? 100.0 : duty_percent > 0.0 ? duty_percent : 0.0; // (not a number: 0)
    return Nanos{static_cast<std::int64_t>(std::llround(duty / 100.0 * static_cast<double>(period.count())))};
}

/// A square: high (true) from the start of a period for `duty_percent` of it, then low. A duty of
/// 0 is always low, of 100 always high. A `period` of 0 or less: low.
[[nodiscard]] inline bool square(Nanos t, Nanos period, double duty_percent = 50.0, double phase_deg = 0.0) noexcept
{
    return detail::position(t, period, phase_deg) < square_high_time(period, duty_percent).count();
}

/// Noise that keeps each value for `hold`: -1 (inclusive) to 1 (exclusive), evenly spread. The
/// value depends on `seed` and on which stretch of `hold` the time `t` is in, not on how often it
/// is asked. For `t` >= 0. A `hold` of 0 or less: 0.
[[nodiscard]] inline double noise_uniform(Nanos t, Nanos hold, std::uint64_t seed) noexcept
{
    return hold.count() <= 0 ? 0.0 : 2.0 * unit_random(seed, static_cast<std::uint64_t>(t.count() / hold.count())) - 1.0;
}

/// The same with values spread like a bell around 0 with the standard deviation 1: the sum of
/// twelve even numbers, so close to a normal distribution and never beyond -6 and 6. A `hold` of
/// 0 or less: 0.
[[nodiscard]] inline double noise_gaussian(Nanos t, Nanos hold, std::uint64_t seed) noexcept
{
    if (hold.count() <= 0)
    {
        return 0.0;
    }
    const auto index = static_cast<std::uint64_t>(t.count() / hold.count());
    double sum = 0.0;
    for (std::uint64_t stream = 1; stream <= 12; ++stream)
    {
        sum += unit_random(seed, index, stream);
    }
    return sum - 6.0;
}

enum class Sweep
{
    Linear, ///< the frequency changes by the same number of hertz in every second
    Log,    ///< the frequency changes by the same factor in every second
};

/// A sine whose frequency goes from `from_hz` at the start to `to_hz` at `length`, without a jump
/// in its phase: 0 at the start. For frequencies of 0 or more (a linear sweep may begin or end at
/// 0 Hz; a log sweep needs both above 0) and 0 <= `t`. A `length` of 0 or less, a frequency that
/// is negative or not a finite number, a log sweep from or to 0 Hz: 0.
[[nodiscard]] inline double chirp(Nanos t, double from_hz, double to_hz, Nanos length, Sweep sweep = Sweep::Linear) noexcept
{
    if (length.count() <= 0 || !std::isfinite(from_hz) || !std::isfinite(to_hz) || from_hz < 0.0 || to_hz < 0.0 ||
        (sweep == Sweep::Log && (from_hz == 0.0 || to_hz == 0.0)))
    {
        return 0.0;
    }
    const double x = static_cast<double>(t.count()) * 1e-9;
    const double d = static_cast<double>(length.count()) * 1e-9;
    double cycles = 0.0; // how many periods have passed
    if (sweep == Sweep::Linear || to_hz == from_hz)
    {
        cycles = from_hz * x + (to_hz - from_hz) * x * x / (2.0 * d);
    }
    else
    {
        const double rate = std::log(to_hz / from_hz);
        cycles = from_hz * d * std::expm1(rate * x / d) / rate;
    }
    return std::sin(2.0 * std::numbers::pi * (cycles - std::floor(cycles)));
}

/// The frequency of that chirp at `t`. A `length` of 0 or less, a log sweep from 0 Hz or less:
/// `from_hz`.
[[nodiscard]] inline double chirp_frequency(Nanos t, double from_hz, double to_hz, Nanos length, Sweep sweep = Sweep::Linear) noexcept
{
    if (length.count() <= 0 || (sweep == Sweep::Log && !(from_hz > 0.0)))
    {
        return from_hz;
    }
    const double share = static_cast<double>(t.count()) / static_cast<double>(length.count());
    return sweep == Sweep::Linear ? from_hz + (to_hz - from_hz) * share : from_hz * std::pow(to_hz / from_hz, share);
}

/// How far a first-order lag with the time constant `tau` has gone towards its target at `t`
/// after a step: 0 at the start, 0.632 after tau, 0.993 after 5 tau. For 0 <= `t`. A `tau` of 0
/// or less: 1 (no lag: it is there at once).
///
///     double value = from + (to - from) * signal::lag(t, 200ms);
[[nodiscard]] inline double lag(Nanos t, Nanos tau) noexcept
{
    return tau.count() <= 0 ? 1.0 : -std::expm1(-static_cast<double>(t.count()) / static_cast<double>(tau.count()));
}

/// When the `index`-th change of a bouncing contact is (the first, index 1, is its first fall
/// back; the contact closes at 0). Without a seed the changes are `interval` apart; with one,
/// each is up to a quarter of the interval earlier or later, so two changes are a half to one and
/// a half intervals apart.
[[nodiscard]] inline Nanos chatter_edge(std::uint64_t index, Nanos interval, std::optional<std::uint64_t> seed = std::nullopt) noexcept
{
    const std::int64_t regular = static_cast<std::int64_t>(index) * interval.count();
    if (!seed || index == 0)
    {
        return Nanos{regular};
    }
    const std::int64_t half = interval.count() / 2;
    return Nanos{regular + static_cast<std::int64_t>(unit_random(*seed, index) * static_cast<double>(half)) - half / 2};
}

/// A contact that bounces: closed (true) from 0, open after the first change, closed after the
/// second, and so on (chatter_edge()). For 0 <= `t`. An `interval` of 0 or less: closed (it does
/// not bounce). Stop asking when the bouncing is to be over.
[[nodiscard]] inline bool chatter(Nanos t, Nanos interval, std::optional<std::uint64_t> seed = std::nullopt) noexcept
{
    if (interval.count() <= 0)
    {
        return true;
    }
    // The stretch between the regular times that t is in has at most one change before t that a
    // regular count would miss or count too early: look at the two nearest.
    auto changes = static_cast<std::uint64_t>(t.count() / interval.count());
    if (changes > 0 && t < chatter_edge(changes, interval, seed))
    {
        --changes;
    }
    else if (t >= chatter_edge(changes + 1, interval, seed))
    {
        ++changes;
    }
    return changes % 2 == 0;
}

/// A point of a table: the value at a time.
struct Point
{
    Nanos time{};
    double value = 0.0;
};

/// The table's value at `t`, on straight lines between the points. `points` in the order of
/// their times, which all differ. Before the first point: on the line from `before` at 0 to it.
/// From the last point on: its value. Without points: `before`. (Where two points have one time,
/// or the first is at 0 or before and `t` before it: the later point's value.)
[[nodiscard]] inline double points_linear(std::span<const Point> points, Nanos t, double before = 0.0) noexcept
{
    if (points.empty())
    {
        return before;
    }
    if (t >= points.back().time)
    {
        return points.back().value;
    }
    // The first point after t (there is one: t is before the last), and the one before it.
    const auto next = std::upper_bound(points.begin(), points.end(), t, [](Nanos time, const Point& p) { return time < p.time; });
    const Point a = next == points.begin() ? Point{Nanos{0}, before} : *(next - 1);
    const Point b = *next;
    if (b.time <= a.time)
    {
        return b.value;
    }
    return a.value + (b.value - a.value) * (static_cast<double>((t - a.time).count()) / static_cast<double>((b.time - a.time).count()));
}

/// The table's value at `t`, each value kept until the next point. Before the first point:
/// `before`.
[[nodiscard]] inline double points_hold(std::span<const Point> points, Nanos t, double before = 0.0) noexcept
{
    const auto next = std::upper_bound(points.begin(), points.end(), t, [](Nanos time, const Point& p) { return time < p.time; });
    return next == points.begin() ? before : (next - 1)->value;
}

} // namespace vehicle_sil::signal
