// SPDX-License-Identifier: MIT
//
// The line of one input of a scenario (libs/scenario/include/vehicle_sil/scenario/detail/scenario_line.hpp) by itself: what the
// scenario's engine cannot be made to do from a scenario file (two motions put at one time, a
// motion read while it runs).

#include "vehicle_sil/scenario/detail/scenario_line.hpp"
#include "vehicle_sil/scenario/detail/scenario_shapes.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using launch::detail::BaseSource;
using launch::detail::Line;
using launch::detail::LineMotion;

/// The line from the base to `to`, reached at `length`.
class To : public Waveform
{
public:
    To(double to, SimDuration length) : to_(to), length_(length) {}
    double value(SimDuration t, double base) const override
    {
        return base + (to_ - base) * static_cast<double>(t.count()) / static_cast<double>(length_.count());
    }
    double end(double) const override { return to_; }

private:
    double to_;
    SimDuration length_;
};

/// 2 in the first half of every 20 ms and 1 in the second, 1 at the end: no base.
class Levels : public Waveform
{
public:
    double value(SimDuration t, double) const override { return t % SimDuration{20ms} < SimDuration{10ms} ? 2.0 : 1.0; }
    double end(double) const override { return 1.0; }
    bool uses_base() const override { return false; }
};

std::shared_ptr<const Waveform> ramp_shape(double to, SimDuration length)
{
    return std::make_shared<To>(to, length);
}

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

/// "7", "7.5", "true"; "-" without a value.
std::string text(const std::optional<control::Value>& value)
{
    if (!value)
    {
        return "-";
    }
    if (const auto* b = std::get_if<bool>(&*value))
    {
        return *b ? "true" : "false";
    }
    if (const auto* i = std::get_if<std::int64_t>(&*value))
    {
        return std::to_string(*i);
    }
    std::string out = std::to_string(std::get<double>(*value));
    out.erase(out.find_last_not_of('0') + 1);
    return out.back() == '.' ? out + "0" : out;
}

} // namespace

TEST_CASE(scenario_line_gives_a_motion_its_base_from_the_step_then_the_line_then_a_sample)
{
    const auto ramp = ramp_shape(20.0, 100ms);
    // Nothing on the line: the sample, and without one there is no base.
    Line bare(control::ValueType::Integer);
    CHECK(!bare.value(at(0ms)) && !bare.placed());
    const LineMotion none = bare.move(at(0ms), ramp, 100ms, std::nullopt, std::nullopt);
    CHECK(!none.has_value() && !none.base && none.source == BaseSource::None);
    CHECK(!bare.value(at(50ms)));
    Line sampled(control::ValueType::Integer);
    const LineMotion fromSample = sampled.move(at(0ms), ramp, 100ms, std::nullopt, control::Value{std::int64_t{60}});
    CHECK(fromSample.has_value() && fromSample.base == 60.0 && fromSample.source == BaseSource::Sample);
    // Once the scenario has put a value there, that is the base, whatever the sample says.
    Line set(control::ValueType::Integer);
    set.set(at(0ms), control::Value{std::int64_t{10}});
    CHECK(text(set.value(at(0ms))) == "10" && text(set.value(at(1s))) == "10" && !set.motion());
    const LineMotion fromLine = set.move(at(50ms), ramp, 100ms, std::nullopt, control::Value{std::int64_t{60}});
    CHECK(fromLine.base == 10.0 && fromLine.source == BaseSource::Line);
    CHECK(text(set.base()) == "10" && set.placed() == at(50ms));
    // The base the step names goes before both.
    const LineMotion named = set.move(at(200ms), ramp, 100ms, 4.5, control::Value{std::int64_t{60}});
    CHECK(named.base == 4.5 && named.source == BaseSource::Written);
    // A shape with its own values has no base, and needs none.
    Line own(control::ValueType::Integer);
    const LineMotion levels = own.move(at(0ms), std::make_shared<Levels>(), 40ms, std::nullopt,
                                       control::Value{std::int64_t{60}});
    CHECK(levels.has_value() && !levels.base && levels.source == BaseSource::None);
    CHECK(text(own.value(at(0ms))) == "2" && text(own.value(at(10ms))) == "1" && text(own.value(at(40ms))) == "1");
}

TEST_CASE(scenario_line_has_the_value_of_its_motion_at_any_time_as_the_type_has_it)
{
    // An integer from 0 to 10 over 100 ms: the value on the line at each time, rounded; from the
    // end on the end value.
    Line line(control::ValueType::Integer);
    line.set(at(0ms), control::Value{std::int64_t{0}});
    const LineMotion up = line.move(at(0ms), ramp_shape(10.0, 100ms), 100ms, std::nullopt, std::nullopt);
    CHECK(text(line.value(at(0ms))) == "0" && text(line.value(at(50ms))) == "5" && text(line.value(at(54ms))) == "5");
    CHECK(text(line.value(at(55ms))) == "6" && text(line.value(at(99ms))) == "10");
    CHECK(text(line.value(at(100ms))) == "10" && text(line.value(at(5s))) == "10");
    // Before the type: the number on the line, and the end value from the end on.
    CHECK(launch::detail::motion_number(up, at(54ms)) == 5.4 && launch::detail::motion_number(up, at(100ms)) == 10.0 &&
          launch::detail::motion_number(up, at(5s)) == 10.0);
    // The function the engine sends with is the same one.
    CHECK(text(launch::detail::motion_value(up, at(55ms))) == "6" && text(launch::detail::motion_value(up, at(100ms))) == "10");
    // A real is not rounded, a bool is true from a half.
    Line real(control::ValueType::Real);
    real.set(at(0ms), control::Value{0.0});
    real.move(at(0ms), ramp_shape(1.0, 100ms), 100ms, std::nullopt, std::nullopt);
    CHECK(text(real.value(at(25ms))) == "0.25");
    Line flag(control::ValueType::Bool);
    flag.set(at(0ms), control::Value{false});
    flag.move(at(0ms), ramp_shape(1.0, 100ms), 100ms, std::nullopt, std::nullopt);
    CHECK(text(flag.value(at(49ms))) == "false" && text(flag.value(at(50ms))) == "true");
}

TEST_CASE(scenario_line_gives_the_second_of_two_motions_at_one_time_the_value_of_the_first_at_that_time)
{
    // The line stands at 10. A ramp from 30 is put at 100 ms, and another motion at the same
    // time, before anything is written: its base is the 30 the first has at 100 ms, not the 10.
    Line line(control::ValueType::Integer);
    line.set(at(0ms), control::Value{std::int64_t{10}});
    line.move(at(100ms), ramp_shape(50.0, 100ms), 100ms, 30.0, std::nullopt);
    const LineMotion second = line.move(at(100ms), ramp_shape(0.0, 100ms), 100ms, std::nullopt, std::nullopt);
    CHECK(second.base == 30.0 && second.source == BaseSource::Line);
    CHECK(text(line.base()) == "30");
    // A motion put while another runs starts from that one's value then, as the type has it
    // (halfway from 30 to 0 at 150 ms is 15; at 155 ms 13.5, which an integer has as 14).
    Line running(control::ValueType::Integer);
    running.move(at(100ms), ramp_shape(0.0, 100ms), 100ms, 30.0, std::nullopt);
    CHECK(running.move(at(155ms), ramp_shape(0.0, 100ms), 100ms, std::nullopt, std::nullopt).base == 14.0);
    // After the end it is the end value, also between two steps of the ECU: the line knows no steps.
    Line over(control::ValueType::Integer);
    over.move(at(0ms), ramp_shape(80.0, 25ms), 25ms, 20.0, std::nullopt);
    CHECK(over.move(at(25ms), ramp_shape(0.0, 20ms), 20ms, std::nullopt, std::nullopt).base == 80.0);
    // A set takes the motion off the line.
    over.set(at(30ms), control::Value{std::int64_t{7}});
    CHECK(!over.motion() && text(over.value(at(40ms))) == "7");
}

TEST_CASE(scenario_line_adds_its_overlay_before_the_type_is_applied)
{
    using launch::detail::Periodic;
    // An integer whose ramp ended at 10.4: the line stands at 10 (where the next motion starts),
    // and its number is 10.4. The overlay is added to the number: at the top of a triangle of
    // 0.2 that is 10.6, written as 11. Added to the rounded value it would be 10.
    Line line(control::ValueType::Integer);
    line.move(at(0ms), ramp_shape(10.4, 100ms), 100ms, 0.0, std::nullopt);
    CHECK(text(line.value(at(100ms))) == "10" && line.number(at(100ms)) == 10.4);
    CHECK(text(line.written_value(at(110ms))) == "10"); // no overlay: the value as the type has it
    line.overlay(launch::detail::triangle_shape(0.2, Periodic{40ms, 0.0}));
    CHECK(text(line.written_value(at(130ms))) == "11" && text(line.written_value(at(120ms))) == "10");
    // What is put on the line leaves the overlay, and what starts from the line does not see it.
    CHECK(line.move(at(130ms), ramp_shape(0.0, 100ms), 100ms, std::nullopt, std::nullopt).base == 10.0);
    line.set(at(240ms), control::Value{std::int64_t{10}});
    // A half is rounded away from zero, at the top and at the bottom of a triangle of 0.5:
    // 10.5 is 11, and 9.5 is 10. In between (a quarter of the way up: 10.25) it is 10.
    line.overlay(launch::detail::triangle_shape(0.5, Periodic{40ms, 0.0}));
    CHECK(text(line.written_value(at(250ms))) == "11" && text(line.written_value(at(270ms))) == "10" &&
          text(line.written_value(at(245ms))) == "10" && text(line.written_value(at(240ms))) == "10");
    // A set's value and a motion's number get the same: written() is the one place.
    CHECK(text(line.written(control::Value{std::int64_t{20}}, at(250ms))) == "21" && text(line.written(20.0, at(250ms))) == "21");
    // Taken off: the value itself, also one that a double cannot hold, and a zero keeps its sign.
    line.overlay(nullptr);
    const std::int64_t large = (std::int64_t{1} << 53) + 1;
    CHECK(std::get<std::int64_t>(line.written(control::Value{large}, at(250ms))) == large);
    Line real(control::ValueType::Real);
    CHECK(std::signbit(std::get<double>(real.written(-0.0, at(0ms)))));
    // Nothing on the line: nothing to write, with or without an overlay.
    real.overlay(launch::detail::triangle_shape(1.0, Periodic{40ms, 0.0}));
    CHECK(!real.written_value(at(10ms)) && !real.number(at(10ms)));
}

TEST_CASE(scenario_line_overlay_is_a_function_of_the_virtual_time_itself)
{
    using launch::detail::Periodic;
    // The same bits as a wave around the same value that began at 0: base + amplitude * factor.
    Line line(control::ValueType::Real);
    line.set(at(0ms), control::Value{0.3});
    const auto triangle = launch::detail::triangle_shape(0.07, Periodic{40ms, 30.0});
    const auto noise = launch::detail::noise_shape(0.07, 10ms, 7, false);
    const auto square = launch::detail::square_shape(0.07, Periodic{40ms, 0.0}, 25.0);
    for (const auto& shape : {triangle, noise, square})
    {
        line.overlay(shape);
        for (const SimDuration t : {0ms, 3ms, 10ms, 17ms, 20ms, 39ms, 40ms, 1234ms})
        {
            CHECK(std::get<double>(*line.written_value(at(t))) == shape->value(t, 0.3));
        }
    }
    // When it was put does not matter: taken off and put again, it gives the same at the same time.
    line.overlay(triangle);
    const double before = std::get<double>(*line.written_value(at(57ms)));
    line.overlay(nullptr);
    line.set(at(50ms), control::Value{0.3});
    line.overlay(triangle);
    CHECK(std::get<double>(*line.written_value(at(57ms))) == before);
    // A dither has no length: after an hour the periodic shapes are where they were at the same
    // place of a period, and the noise is within its amplitude and stays for its hold.
    const SimDuration hour = std::chrono::hours(1);
    CHECK(triangle->value(hour + 17ms, 0.0) == triangle->value(17ms, 0.0));
    CHECK(square->value(hour + 5ms, 0.0) == 0.07 && square->value(hour + 15ms, 0.0) == -0.07);
    CHECK(launch::detail::sine_shape(1.0, Periodic{40ms, 0.0})->value(hour + 10ms, 0.0) > 0.999);
    const double late = noise->value(hour + 1ms, 0.0);
    CHECK(std::fabs(late) <= 0.07 && late == noise->value(hour + 9ms, 0.0) && late != noise->value(hour + 11ms, 0.0));
}
