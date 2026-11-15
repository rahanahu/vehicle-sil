// SPDX-License-Identifier: MIT
//
// The line of one input of a scenario: where the scenario left the variable (its base), and the
// ramp or wave that moves it now. The scenario's engine (scenario_run.hpp) keeps one per variable
// it writes: a step that begins puts what it does on the line (a set its value, a ramp or a wave
// its motion), and a motion that does not name where it starts reads that from the line.
//
// The value of a motion at a time comes from one function, motion_number(), and motion_value()
// is that value as the variable's type has it: the engine sends what it gives, and whatever shows
// a motion without running it has to call the same.
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/core/sim_time.hpp"
#include "vehicle_sil/launch/scenario.hpp"

#include <memory>
#include <optional>

namespace vehicle_sil::launch::detail {

/// A value as a number: a bool is 0 or 1.
[[nodiscard]] double number_of(const control::Value& value);

/// A number as a value of a variable's type: an integer is rounded to the nearest (halves away
/// from zero), a bool is true from 0.5.
///
/// Before an integer is rounded, the value is put on a grid of 2^-30: a value that is a half but
/// for the last bits of the arithmetic (a sine at 30 degrees is 0.49999999999999994 with one C
/// library and 0.5 with another) then rounds the same way everywhere. What can still differ by
/// one between platforms is a value that close to the middle between two points of the grid,
/// which a scenario does not meet by writing round numbers.
[[nodiscard]] control::Value value_of_type(double value, control::ValueType type);

/// Where the base of a motion comes from.
enum class BaseSource
{
    None,    ///< it has none: its shape does not use one, or none was known
    Written, ///< the step names it (a ramp's from, a wave's around, the other value of a bool)
    Line,    ///< the value of the variable's line when the motion begins
    Sample,  ///< the line had no value yet: the latest sample of the variable
};
/// "written", "line", "sample"; null for None.
[[nodiscard]] const char* base_source_name(BaseSource source);

/// A ramp or a wave of one variable, put at a time.
struct LineMotion
{
    std::shared_ptr<const Waveform> shape;
    SimTime from{};
    SimDuration length{}; ///< > 0
    control::ValueType type = control::ValueType::Real;
    /// What the shape starts from or swings around (Waveform's base); empty: see `source`.
    std::optional<double> base{};
    BaseSource source = BaseSource::None;

    /// Whether it can give values: it has a base, or its shape uses none.
    [[nodiscard]] bool has_value() const { return base.has_value() || !shape->uses_base(); }
};

/// The value of `motion` at `t` (at or after its `from`; it must have a value, has_value()) as a
/// number, before the variable's type is applied: the shape's value before the end of its length,
/// and what the shape leaves (Waveform::end()) from there on. The one place that knows when a
/// motion is over. What adds to a motion's value before it is rounded (a dither on top of it)
/// calls this one, and rounds the sum.
[[nodiscard]] double motion_number(const LineMotion& motion, SimTime t);

/// motion_number() as the variable's type has it (value_of_type()): what the engine sends.
[[nodiscard]] control::Value motion_value(const LineMotion& motion, SimTime t);

/// The line of one variable. Times given to it never go back.
class Line
{
public:
    explicit Line(control::ValueType type) : type_(type) {}

    /// A set at `t`: the line stands at `value` from there.
    void set(SimTime t, control::Value value);
    /// A ramp or a wave from `t`. Its base is `named` if the step names one; else the value of
    /// the line at `t`, which is that of the motion before it at `t` if two are put at one time;
    /// else, while the line has no value, `sample` (the latest sample of the variable, if there
    /// is one). A shape that uses no base has none.
    /// @returns the motion as it was put (LineMotion::has_value() says whether it found its base).
    const LineMotion& move(SimTime t, std::shared_ptr<const Waveform> shape, SimDuration length, std::optional<double> named,
                           const std::optional<control::Value>& sample);

    /// The value at `t`, at or after the time of what was put last: of the motion (its end value
    /// once it is over), else where the line stands. Empty if nothing was put yet, or the motion
    /// has no value.
    [[nodiscard]] std::optional<control::Value> value(SimTime t) const;
    /// Where the line stood when what is on it was put: the value of a set, and for a motion the
    /// value before it. Empty: nothing then.
    [[nodiscard]] const std::optional<control::Value>& base() const noexcept { return base_; }
    /// When that was; empty if nothing was put yet.
    [[nodiscard]] const std::optional<SimTime>& placed() const noexcept { return placed_; }
    /// The motion put last, also once it is over; empty after a set.
    [[nodiscard]] const std::optional<LineMotion>& motion() const noexcept { return motion_; }

private:
    control::ValueType type_;
    std::optional<control::Value> base_{};
    std::optional<SimTime> placed_{};
    std::optional<LineMotion> motion_{};
};

} // namespace vehicle_sil::launch::detail
