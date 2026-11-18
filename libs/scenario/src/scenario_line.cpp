// SPDX-License-Identifier: MIT
#include "vehicle_sil/scenario/detail/scenario_line.hpp"

#include <cmath>
#include <cstdint>
#include <utility>

namespace vehicle_sil::launch::detail {

double number_of(const control::Value& value)
{
    if (const auto* b = std::get_if<bool>(&value))
    {
        return *b ? 1.0 : 0.0;
    }
    if (const auto* i = std::get_if<std::int64_t>(&value))
    {
        return static_cast<double>(*i);
    }
    return std::get<double>(value);
}

control::Value value_of_type(double value, control::ValueType type)
{
    switch (type)
    {
    case control::ValueType::Integer:
    case control::ValueType::Enum:
        if (std::fabs(value) < 0x1p22) // (times 2^30 it still has bits below one)
        {
            value = std::round(value * 0x1p30) * 0x1p-30;
        }
        return control::Value{static_cast<std::int64_t>(std::llround(value))};
    case control::ValueType::Bool: return control::Value{value >= 0.5};
    case control::ValueType::Real: break;
    }
    return control::Value{value};
}

const char* base_source_name(BaseSource source)
{
    switch (source)
    {
    case BaseSource::None: break;
    case BaseSource::Written: return "written";
    case BaseSource::Line: return "line";
    case BaseSource::Sample: return "sample";
    }
    return nullptr;
}

double motion_number(const LineMotion& motion, SimTime t)
{
    const double base = motion.base.value_or(0.0);
    const SimDuration since = t - motion.from;
    return since >= motion.length ? motion.shape->end(base) : motion.shape->value(since, base);
}

control::Value motion_value(const LineMotion& motion, SimTime t)
{
    return value_of_type(motion_number(motion, t), motion.type);
}

void Line::set(SimTime t, control::Value value)
{
    base_ = std::move(value);
    placed_ = t;
    motion_.reset();
}

const LineMotion& Line::move(SimTime t, std::shared_ptr<const Waveform> shape, SimDuration length, std::optional<double> named,
                             const std::optional<control::Value>& sample)
{
    // Where the line stands at `t`, before this motion: what the next one reads if it comes at
    // the same time.
    const std::optional<control::Value> before = value(t);
    LineMotion motion;
    motion.shape = std::move(shape);
    motion.from = t;
    motion.length = length;
    motion.type = type_;
    if (named)
    {
        motion.base = named;
        motion.source = BaseSource::Written;
    }
    else if (!motion.shape->uses_base())
    {
        motion.source = BaseSource::None;
    }
    else if (before)
    {
        // Where the scenario left the variable, whatever the samples since say (another writer
        // does not move the start).
        motion.base = number_of(*before);
        motion.source = BaseSource::Line;
    }
    else if (sample)
    {
        motion.base = number_of(*sample);
        motion.source = BaseSource::Sample;
    }
    base_ = before;
    placed_ = t;
    motion_ = std::move(motion);
    return *motion_;
}

std::optional<control::Value> Line::value(SimTime t) const
{
    if (!motion_)
    {
        return base_;
    }
    if (!motion_->has_value())
    {
        return std::nullopt;
    }
    return motion_value(*motion_, t);
}

std::optional<double> Line::number(SimTime t) const
{
    if (!motion_)
    {
        return base_ ? std::optional<double>(number_of(*base_)) : std::nullopt;
    }
    if (!motion_->has_value())
    {
        return std::nullopt;
    }
    return motion_number(*motion_, t);
}

control::Value Line::written(double number, SimTime t) const
{
    // (Nothing is added without an overlay: not even a zero, which would turn -0 into 0.)
    return value_of_type(overlay_ ? number + overlay_->value(t - SimTime{}, 0.0) : number, type_);
}

control::Value Line::written(const control::Value& value, SimTime t) const
{
    return overlay_ ? written(number_of(value), t) : value;
}

std::optional<control::Value> Line::written_value(SimTime t) const
{
    const std::optional<double> at = number(t);
    return at ? std::optional<control::Value>(written(*at, t)) : std::nullopt;
}

} // namespace vehicle_sil::launch::detail
