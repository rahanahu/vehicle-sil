// SPDX-License-Identifier: MIT
//
// The line of one input of a scenario (libs/launch/src/scenario_line.hpp) by itself: what the
// scenario's engine cannot be made to do from a scenario file (two motions put at one time, a
// motion read while it runs).

#include "scenario_line.hpp"
#include "test_harness.hpp"

#include <chrono>
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
