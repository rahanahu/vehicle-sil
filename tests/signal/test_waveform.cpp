// SPDX-License-Identifier: MIT
//
// The waveforms of vehicle_sil::signal (signal/waveform.hpp): their values at given times, and
// that each depends on the time alone.
#include "test_harness.hpp"

#include "vehicle_sil/signal/waveform.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil::signal;

/// Equal but for the rounding of a few operations (the sine and the exponential are not exact).
bool near(double actual, double expected, double tolerance = 1e-9)
{
    return std::fabs(actual - expected) <= tolerance;
}

} // namespace

TEST_CASE(signal_sine_triangle_and_sawtooth_have_their_corners_at_the_quarters_of_a_period)
{
    // At 0, a quarter, a half, three quarters, and the same one period later.
    const std::array<Nanos, 5> times = {0ms, 50ms, 100ms, 150ms, 200ms};
    const std::array<double, 5> sines = {0.0, 1.0, 0.0, -1.0, 0.0};
    const std::array<double, 5> saws = {-1.0, -0.5, 0.0, 0.5, -1.0};
    for (std::size_t i = 0; i < times.size(); ++i)
    {
        CHECK(near(sine(times[i], 200ms), sines[i]));
        CHECK(near(sine(times[i] + 200ms, 200ms), sines[i]));
        // The triangle and the sawtooth are exact: no function of the C library is in them.
        CHECK_EQ(triangle(times[i], 200ms), sines[i]);
        CHECK_EQ(triangle(times[i] + 600ms, 200ms), sines[i]);
        CHECK_EQ(sawtooth(times[i], 200ms), saws[i]);
        CHECK_EQ(sawtooth(times[i], 200ms, true), -saws[i]);
    }
    CHECK_EQ(triangle(25ms, 200ms), 0.5);
    CHECK_EQ(triangle(125ms, 200ms), -0.5);
    // The last nanosecond of a period is not the next period's first.
    CHECK(sawtooth(Nanos{200ms} - Nanos{1}, 200ms) > 0.99999);
    CHECK_EQ(sawtooth(200ms, 200ms), -1.0);
}

TEST_CASE(signal_phase_moves_the_start_within_the_period)
{
    // 90 degrees: a quarter of a period in.
    CHECK(near(sine(0ms, 200ms, 90.0), 1.0));
    CHECK_EQ(triangle(0ms, 200ms, 90.0), 1.0);
    CHECK_EQ(triangle(50ms, 200ms, 90.0), 0.0);
    CHECK_EQ(sawtooth(0ms, 200ms, false, 180.0), 0.0);
    // A phase is a place in the period: 360 more or less is the same, and so is a negative one.
    CHECK_EQ(triangle(30ms, 200ms, 450.0), triangle(30ms, 200ms, 90.0));
    CHECK_EQ(triangle(30ms, 200ms, -270.0), triangle(30ms, 200ms, 90.0));
    CHECK_EQ(triangle(0ms, 200ms, -90.0), -1.0);
    CHECK(square(0ms, 100ms, 25.0, 0.0));
    CHECK(!square(0ms, 100ms, 25.0, 90.0)); // begins where the high quarter is over
    CHECK(square(0ms, 100ms, 25.0, -90.0) == square(75ms, 100ms, 25.0));
}

TEST_CASE(signal_square_is_high_for_its_duty_from_the_start_of_each_period)
{
    CHECK(square(0ms, 100ms));
    CHECK(square(49ms, 100ms));
    CHECK(!square(50ms, 100ms));
    CHECK(!square(99ms, 100ms));
    CHECK(square(100ms, 100ms));
    CHECK(square(24ms, 100ms, 25.0));
    CHECK(!square(25ms, 100ms, 25.0));
    // 0 is never high, 100 always; what is outside counts as the nearest of them.
    for (const Nanos t : {Nanos{0ms}, Nanos{1ms}, Nanos{50ms}, Nanos{99ms}, Nanos{100ms}})
    {
        CHECK(!square(t, 100ms, 0.0));
        CHECK(square(t, 100ms, 100.0));
        CHECK(!square(t, 100ms, -5.0));
        CHECK(square(t, 100ms, 130.0));
    }
    CHECK_EQ(square_high_time(30ms, 25.0).count(), 7'500'000);
}

TEST_CASE(signal_random_numbers_are_the_same_on_every_platform)
{
    // The numbers themselves: integer arithmetic and one exact conversion, so every compiler and
    // C library must give these bits.
    CHECK_EQ(unit_random(7, 0), 0.30634753597431719);
    CHECK_EQ(unit_random(7, 1), 0.10319287176746084);
    CHECK_EQ(unit_random(7, 2), 0.90165521735338938);
    CHECK_EQ(unit_random(0, 0), 0.13870941014555427);
    CHECK_EQ(unit_random(7, 0, 1), 0.94610741263166598);
    CHECK_EQ(noise_uniform(20ms, 10ms, 7), 0.80331043470677876);
    CHECK_EQ(noise_gaussian(30ms, 10ms, 7), -1.6354912973943989);
    CHECK_EQ(chatter_edge(1, 10ms, 5).count(), 11'194'388);
}

TEST_CASE(signal_noise_depends_on_the_stretch_of_time_and_the_seed_alone)
{
    // Every time within one stretch of `hold` has the stretch's value, in whatever order asked.
    const double third = noise_uniform(30ms, 10ms, 42);
    CHECK_EQ(noise_uniform(39'999'999ns, 10ms, 42), third);
    CHECK_EQ(noise_uniform(35ms, 10ms, 42), third);
    CHECK(noise_uniform(40ms, 10ms, 42) != third);
    CHECK(noise_uniform(30ms, 10ms, 43) != third);
    CHECK_EQ(noise_uniform(30ms, 10ms, 42), third);
    CHECK_EQ(noise_gaussian(31ms, 10ms, 42), noise_gaussian(38ms, 10ms, 42));

    // Evenly from -1 to 1; and a bell of standard deviation 1 that stays within 6.
    double sum = 0.0, squares = 0.0, bellSum = 0.0, bellSquares = 0.0;
    const int count = 20000;
    for (int i = 0; i < count; ++i)
    {
        const double u = noise_uniform(Nanos{1ms} * i, 1ms, 3);
        const double g = noise_gaussian(Nanos{1ms} * i, 1ms, 3);
        CHECK(u >= -1.0 && u < 1.0);
        CHECK(g >= -6.0 && g <= 6.0);
        sum += u;
        squares += u * u;
        bellSum += g;
        bellSquares += g * g;
    }
    CHECK(near(sum / count, 0.0, 0.02));
    CHECK(near(squares / count, 1.0 / 3.0, 0.02));
    CHECK(near(bellSum / count, 0.0, 0.03));
    CHECK(near(bellSquares / count, 1.0, 0.05));
}

TEST_CASE(signal_chirp_sweeps_its_frequency_without_a_jump_of_the_phase)
{
    // 1 Hz to 3 Hz in one second: 2 periods in all, 0.3125 of them after a quarter of the time.
    CHECK(near(chirp(0ms, 1.0, 3.0, 1s), 0.0));
    CHECK(near(chirp(250ms, 1.0, 3.0, 1s), std::sin(2.0 * 3.14159265358979323846 * 0.3125)));
    CHECK(near(chirp(500ms, 1.0, 3.0, 1s), -1.0));
    CHECK(near(chirp(1s, 1.0, 3.0, 1s), 0.0, 1e-6));
    // The same frequency at both ends is a sine, for either sweep.
    for (const Sweep sweep : {Sweep::Linear, Sweep::Log})
    {
        CHECK(near(chirp(125ms, 2.0, 2.0, 1s, sweep), 1.0));
        CHECK(near(chirp(70ms, 2.0, 2.0, 1s, sweep), sine(70ms, 500ms)));
    }
    // Log, 1 Hz to 4 Hz: 3 / ln 4 periods in all.
    CHECK(near(chirp(1s, 1.0, 4.0, 1s, Sweep::Log), std::sin(2.0 * 3.14159265358979323846 * 3.0 / std::log(4.0)), 1e-6));
    CHECK(near(chirp_frequency(500ms, 1.0, 3.0, 1s), 2.0));
    CHECK(near(chirp_frequency(500ms, 1.0, 4.0, 1s, Sweep::Log), 2.0));
    // No jump: neighbouring times give neighbouring values, also where the frequency is highest.
    for (Nanos t{0}; t < 1s; t += 100us)
    {
        CHECK(std::fabs(chirp(t + 100us, 1.0, 40.0, 1s, Sweep::Log) - chirp(t, 1.0, 40.0, 1s, Sweep::Log)) < 0.03);
    }
}

TEST_CASE(signal_lag_goes_most_of_the_way_in_a_few_time_constants)
{
    CHECK_EQ(lag(0ms, 200ms), 0.0);
    CHECK(near(lag(200ms, 200ms), 1.0 - std::exp(-1.0)));
    CHECK(near(lag(1s, 200ms), 0.9932620530009145));
    CHECK(lag(100ms, 200ms) < lag(101ms, 200ms));
}

TEST_CASE(signal_chatter_is_closed_first_and_changes_at_its_edges)
{
    // Without a seed: every interval.
    CHECK(chatter(0ms, 10ms));
    CHECK(chatter(9ms, 10ms));
    CHECK(!chatter(10ms, 10ms));
    CHECK(!chatter(19ms, 10ms));
    CHECK(chatter(20ms, 10ms));
    // With one: each change up to a quarter of the interval early or late, so in the order of
    // their numbers, and the value changes exactly there.
    for (const std::uint64_t seed : {1u, 5u, 99u})
    {
        Nanos before{0};
        for (std::uint64_t k = 1; k <= 200; ++k)
        {
            const Nanos edge = chatter_edge(k, 10ms, seed);
            const Nanos regular = Nanos{10ms} * static_cast<std::int64_t>(k);
            CHECK(edge >= regular - 2500us && edge < regular + 2500us);
            CHECK(edge - before >= 5ms && edge - before <= 15ms);
            CHECK_EQ(chatter(edge - 1ns, 10ms, seed), k % 2 == 1);
            CHECK_EQ(chatter(edge, 10ms, seed), k % 2 == 0);
            before = edge;
        }
    }
    CHECK(chatter_edge(3, 10ms, 1) != chatter_edge(3, 10ms, 2));
}

TEST_CASE(signal_points_are_joined_by_lines_or_kept)
{
    const std::vector<Point> table = {{100ms, 50.0}, {400ms, 20.0}, {500ms, 20.0}};
    // Before the first point: the line from `before` at 0, or `before` itself.
    CHECK_EQ(points_linear(table, 0ms, 10.0), 10.0);
    CHECK_EQ(points_linear(table, 50ms, 10.0), 30.0);
    CHECK_EQ(points_hold(table, 99ms, 10.0), 10.0);
    CHECK_EQ(points_linear(table, 100ms, 10.0), 50.0);
    CHECK_EQ(points_hold(table, 100ms, 10.0), 50.0);
    CHECK_EQ(points_linear(table, 250ms, 10.0), 35.0);
    CHECK_EQ(points_hold(table, 399ms, 10.0), 50.0);
    CHECK_EQ(points_hold(table, 400ms, 10.0), 20.0);
    // From the last point on: its value.
    CHECK_EQ(points_linear(table, 500ms), 20.0);
    CHECK_EQ(points_linear(table, 9s), 20.0);
    CHECK_EQ(points_hold(table, 9s), 20.0);
    // A table that begins at 0 does not look at `before`.
    const std::vector<Point> fromZero = {{0ms, 1.0}, {10ms, 3.0}};
    CHECK_EQ(points_linear(fromZero, 0ms, 77.0), 1.0);
    CHECK_EQ(points_linear(fromZero, 5ms, 77.0), 2.0);
    CHECK_EQ(points_hold(fromZero, 0ms, 77.0), 1.0);
    CHECK_EQ(points_linear({}, 5ms, 77.0), 77.0);
    CHECK_EQ(points_hold({}, 5ms, 77.0), 77.0);
}
