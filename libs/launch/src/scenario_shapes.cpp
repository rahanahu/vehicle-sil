// SPDX-License-Identifier: MIT
#include "scenario_shapes.hpp"

#include <utility>

namespace vehicle_sil::launch::detail {

namespace {

class Ramp final : public Waveform
{
public:
    Ramp(double to, SimDuration length) : to_(to), length_(length) {}

    double value(SimDuration t, double base) const override
    {
        const double share = static_cast<double>(t.count()) / static_cast<double>(length_.count());
        return base + (to_ - base) * share;
    }
    double end(double) const override { return to_; }

private:
    double to_;
    SimDuration length_;
};

/// base + amplitude * factor(t), and the base at the end.
template <class Factor>
class Around final : public Waveform
{
public:
    Around(double amplitude, Factor factor) : amplitude_(amplitude), factor_(std::move(factor)) {}

    double value(SimDuration t, double base) const override { return base + amplitude_ * factor_(t); }
    double end(double base) const override { return base; }

private:
    double amplitude_;
    Factor factor_;
};

template <class Factor>
std::shared_ptr<const Waveform> around(double amplitude, Factor factor)
{
    return std::make_shared<Around<Factor>>(amplitude, std::move(factor));
}

class Pulse final : public Waveform
{
public:
    explicit Pulse(double value) : value_(value) {}

    double value(SimDuration, double) const override { return value_; }
    double end(double base) const override { return base; }

private:
    double value_;
};

class SquareLevels final : public Waveform
{
public:
    SquareLevels(double low, double high, Periodic periodic, double duty) : low_(low), high_(high), periodic_(periodic), duty_(duty) {}

    double value(SimDuration t, double) const override
    {
        return signal::square(t, periodic_.period, duty_, periodic_.phase_deg) ? high_ : low_;
    }
    double end(double) const override { return low_; }
    bool uses_base() const override { return false; }

private:
    double low_;
    double high_;
    Periodic periodic_;
    double duty_;
};

class Lag final : public Waveform
{
public:
    Lag(double to, SimDuration tau, SimDuration length, bool settle) : to_(to), tau_(tau), length_(length), settle_(settle) {}

    double value(SimDuration t, double base) const override { return base + (to_ - base) * signal::lag(t, tau_); }
    double end(double base) const override { return settle_ ? to_ : value(length_, base); }

private:
    double to_;
    SimDuration tau_;
    SimDuration length_;
    bool settle_;
};

class Chatter final : public Waveform
{
public:
    Chatter(double to, SimDuration interval, std::optional<std::uint64_t> seed) : to_(to), interval_(interval), seed_(seed) {}

    double value(SimDuration t, double base) const override { return signal::chatter(t, interval_, seed_) ? to_ : base; }
    double end(double) const override { return to_; }

private:
    double to_;
    SimDuration interval_;
    std::optional<std::uint64_t> seed_;
};

class Points final : public Waveform
{
public:
    Points(std::vector<signal::Point> points, bool hold) : points_(std::move(points)), hold_(hold) {}

    double value(SimDuration t, double base) const override
    {
        return hold_ ? signal::points_hold(points_, t, base) : signal::points_linear(points_, t, base);
    }
    double end(double base) const override { return points_.empty() ? base : points_.back().value; }
    bool uses_base() const override { return points_.empty() || points_.front().time > SimDuration::zero(); }

private:
    std::vector<signal::Point> points_;
    bool hold_;
};

} // namespace

std::shared_ptr<const Waveform> ramp_shape(double to, SimDuration length)
{
    return std::make_shared<Ramp>(to, length);
}

std::shared_ptr<const Waveform> sine_shape(double amplitude, Periodic p)
{
    return around(amplitude, [p](SimDuration t) { return signal::sine(t, p.period, p.phase_deg); });
}

std::shared_ptr<const Waveform> triangle_shape(double amplitude, Periodic p)
{
    return around(amplitude, [p](SimDuration t) { return signal::triangle(t, p.period, p.phase_deg); });
}

std::shared_ptr<const Waveform> sawtooth_shape(double amplitude, Periodic p, bool falling)
{
    return around(amplitude, [p, falling](SimDuration t) { return signal::sawtooth(t, p.period, falling, p.phase_deg); });
}

std::shared_ptr<const Waveform> square_shape(double amplitude, Periodic p, double duty)
{
    return around(amplitude, [p, duty](SimDuration t) { return signal::square(t, p.period, duty, p.phase_deg) ? 1.0 : -1.0; });
}

std::shared_ptr<const Waveform> noise_shape(double amplitude, SimDuration hold, std::uint64_t seed, bool gaussian)
{
    return around(amplitude, [hold, seed, gaussian](SimDuration t) {
        return gaussian ? signal::noise_gaussian(t, hold, seed) : signal::noise_uniform(t, hold, seed);
    });
}

std::shared_ptr<const Waveform> chirp_shape(double amplitude, double from_hz, double to_hz, SimDuration length, signal::Sweep sweep)
{
    return around(amplitude, [=](SimDuration t) { return signal::chirp(t, from_hz, to_hz, length, sweep); });
}

std::shared_ptr<const Waveform> pulse_shape(double value)
{
    return std::make_shared<Pulse>(value);
}

std::shared_ptr<const Waveform> square_levels_shape(double low, double high, Periodic periodic, double duty)
{
    return std::make_shared<SquareLevels>(low, high, periodic, duty);
}

std::shared_ptr<const Waveform> lag_shape(double to, SimDuration tau, SimDuration length, bool settle)
{
    return std::make_shared<Lag>(to, tau, length, settle);
}

std::shared_ptr<const Waveform> chatter_shape(double to, SimDuration interval, std::optional<std::uint64_t> seed)
{
    return std::make_shared<Chatter>(to, interval, seed);
}

std::shared_ptr<const Waveform> points_shape(std::vector<signal::Point> points, bool hold)
{
    return std::make_shared<Points>(std::move(points), hold);
}

} // namespace vehicle_sil::launch::detail
