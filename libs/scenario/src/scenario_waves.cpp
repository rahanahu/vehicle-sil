// SPDX-License-Identifier: MIT
//
// The scenario's reader (scenario_reader.hpp): waves, their shapes and points (also from a CSV
// file), and what the ECU's step makes of them.
#include "scenario_reader.hpp"

namespace vehicle_sil::scenario::reader {

namespace {

/// A smooth shape with fewer steps of its ECU per period is coarse (a note).
constexpr std::int64_t kSmoothSteps = 8;
/// Two values of a shape (as a share of its amplitude) that are this close are the same value.
constexpr double kSameValue = 1e-9;
constexpr std::int64_t kMaxBounces = 1'000'000;

} // namespace

std::string Reader::span_text(SimDuration d)
{
    const std::int64_t ns = d.count();
    return (ns % 1'000'000 == 0 ? std::to_string(ns / 1'000'000) : number_text(static_cast<double>(ns) / 1e6)) + " ms";
}

std::optional<Reader::Parts> Reader::parts_of(ryml::id_type node, const std::string& what, const std::vector<std::string>& allowed,
                                              const std::string& must)
{
    if (!tree().is_map(node))
    {
        error("value", doc().line(node), what + " must be " + must + ", not " + shown_value(node));
        return std::nullopt;
    }
    Parts out;
    std::map<std::string, std::size_t> seen;
    for (auto part = tree().first_child(node); part != ryml::NONE; part = tree().next_sibling(part))
    {
        const std::string key = config::detail::to_string(tree().key(part));
        if (!first_occurrence(seen, key, part, what + ": "))
        {
            continue;
        }
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
            unknown_key(part, key, what + ": ", allowed);
            continue;
        }
        out.emplace(key, part);
    }
    return out;
}

std::optional<double> Reader::measure(ryml::id_type node, const std::string& label, double least, bool above, double most,
                                      const std::string& meaning)
{
    record_direct_param_usage(node, ParamType::Number);
    double d = 0;
    if (!plain_number(node, d))
    {
        error("value", doc().line(node),
              label + " must be a number (" + meaning + "), not " + shown_value(node) +
                  (quoted(node) ? ": write the number without quotes" : ""));
        return std::nullopt;
    }
    if (d < least || (above && d == least) || d > most)
    {
        error("value", doc().line(node),
              label + " is " + number_text(d) + ", but it must be " + (above ? "more than " : "from ") + number_text(least) +
                  (std::isfinite(most) ? std::string(above ? " and at most " : " to ") + number_text(most) : std::string{}) + " (" +
                  meaning + ")");
        return std::nullopt;
    }
    return d;
}

std::optional<bool> Reader::flag(ryml::id_type node, const std::string& label, const std::string& meaning)
{
    record_direct_param_usage(node, ParamType::Boolean);
    const std::string text = is_scalar(node) && !is_null(node) && !quoted(node) ? text_of(node) : std::string{};
    if (text != "true" && text != "false")
    {
        error("value", doc().line(node), label + " must be true or false (" + meaning + "), not " + shown_value(node));
        return std::nullopt;
    }
    return text == "true";
}

std::optional<std::string> Reader::choice(ryml::id_type node, const std::string& label, const std::vector<std::string>& options)
{
    const std::string text = is_scalar(node) && !is_null(node) ? text_of(node) : std::string{};
    if (std::find(options.begin(), options.end(), text) == options.end())
    {
        error("value", doc().line(node), label + " must be " + config::detail::or_list(options) + ", not " + shown_value(node));
        return std::nullopt;
    }
    return text;
}

std::optional<std::uint64_t> Reader::seed_of(ryml::id_type node, const std::string& label)
{
    record_direct_param_usage(node, ParamType::Integer);
    std::int64_t value = 0;
    if (!plain_integer(node, value) || value < 0)
    {
        error("value", doc().line(node),
              label + " must be a whole number from 0 (the same seed gives the same values in every run), not " + shown_value(node));
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value);
}

std::optional<double> Reader::level(ryml::id_type node, std::size_t variable, const std::string& key, const std::string& what,
                                    StepReadState* state)
{
    const ScenarioVariable& var = scenario_.variables[variable];
    if (var.type == control::ValueType::Bool || var.type == control::ValueType::Enum)
    {
        const auto value = value_of(node, variable, key + " of " + what, true, state);
        return value ? std::optional<double>{as_number(*value)} : std::nullopt;
    }
    record_direct_param_usage(node, ParamType::Number);
    double d = 0;
    if (!number_of(node, variable, d))
    {
        error("scenario-value", doc().line(node),
              key + " of " + what + " of " + var.text + " must be a number, not " + shown_value(node) +
                  (quoted(node) ? ": write the number without quotes" : hex_hint(node, var.type)));
        return std::nullopt;
    }
    if (!in_range(d, variable))
    {
        error("scenario-value", doc().line(node),
              key + " of " + what + " of " + var.text + " is " + written(node, d) + ", outside its range (" +
                  range_text(*info_of(variable)) + ")");
        return std::nullopt;
    }
    if (state != nullptr)
    {
        note_base(*state, variable, d, doc().line(node), key == "to"); // (what a lag or a chatter comes to rest at)
    }
    return d;
}

std::optional<detail::Periodic> Reader::periodic_of(const Parts& parts, const std::string& what, std::size_t line, std::string& text)
{
    detail::Periodic out;
    bool ok = true;
    const bool hasPeriod = parts.count("period_ms") > 0;
    const bool hasFrequency = parts.count("frequency_hz") > 0;
    if (hasPeriod && hasFrequency)
    {
        error("value", doc().line(parts.at("frequency_hz")),
              what + " has period_ms and frequency_hz: give one of them (frequency_hz: 50 is period_ms: 20)");
        ok = false;
    }
    else if (hasPeriod)
    {
        if (const auto ms = millis(parts.at("period_ms"), "period_ms", 1, "milliseconds of virtual time of one period"))
        {
            out.period = std::chrono::milliseconds(*ms);
            text += " period " + std::to_string(*ms) + " ms";
        }
        else
        {
            ok = false;
        }
    }
    else if (hasFrequency)
    {
        // A period of a whole number of nanoseconds, at least 1 and within the longest time.
        const double least = 1e3 / static_cast<double>(config::kMaxMillis);
        if (const auto hz = measure(parts.at("frequency_hz"), "frequency_hz of " + what, least, false, 1e9, "periods per second of virtual time"))
        {
            out.period = SimDuration{std::llround(1e9 / *hz)};
            text += " frequency " + number_text(*hz) + " Hz";
        }
        else
        {
            ok = false;
        }
    }
    else
    {
        error("missing-key", line, what + " needs period_ms or frequency_hz: how long one period is, or how many there are in a second");
        ok = false;
    }
    if (parts.count("phase_deg") > 0)
    {
        if (const auto phase = measure(parts.at("phase_deg"), "phase_deg of " + what, -36000.0, false, 36000.0,
                                       "where in its period the shape begins, in degrees: 360 is a whole period"))
        {
            out.phase_deg = *phase;
            if (*phase != 0.0)
            {
                text += " phase " + number_text(*phase) + " deg";
            }
        }
        else
        {
            ok = false;
        }
    }
    return ok ? std::optional<detail::Periodic>{out} : std::nullopt;
}

std::optional<SimDuration> Reader::sampling(std::size_t variable, const std::string& what, std::size_t line, bool quiet)
{
    if (quiet)
    {
        return std::nullopt;
    }
    const ScenarioParticipant& p = scenario_.participants[scenario_.variables[variable].participant];
    if (p.step <= SimDuration::zero())
    {
        add(config::Severity::Note, "scenario-wave-not-checked", line,
            "whether participant '" + p.name + "' can see " + what + " as it is written is not checked: the participant's step "
            "is not known",
            p.name);
        return std::nullopt;
    }
    return p.step;
}

std::string Reader::takes_inputs(std::size_t variable) const
{
    const ScenarioParticipant& p = scenario_.participants[scenario_.variables[variable].participant];
    return "participant '" + p.name + "' takes its inputs once per step of " + span_text(p.step);
}

std::string Reader::step_of(std::size_t variable) const
{
    const ScenarioParticipant& p = scenario_.participants[scenario_.variables[variable].participant];
    return "the step of participant '" + p.name + "' (" + span_text(p.step) + ")";
}

SimDuration Reader::step_ns(std::size_t variable) const
{
    return scenario_.participants[scenario_.variables[variable].participant].step;
}

const std::string& Reader::owner_of(std::size_t variable) const
{
    return scenario_.participants[scenario_.variables[variable].participant].name;
}

bool Reader::check_period(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration period,
                          const std::function<double(SimDuration)>& factor)
{
    const auto step = sampling(variable, what, line, quiet);
    if (!step)
    {
        return true;
    }
    const bool smooth = static_cast<bool>(factor);
    // What to make of a smooth shape: a period that shows it. Two values per period show a square.
    const std::string longer = smooth ? "Make the period " + span_text(kSmoothSteps * *step) + " or more (" +
                                            std::to_string(kSmoothSteps) + " values per period)"
                                      : "Make the period " + span_text(2 * *step) + " or more";
    if (period < 2 * *step)
    {
        add(config::Severity::Warning, "scenario-wave-undersampled", line,
            what + " has a period of " + span_text(period) + ", but " + takes_inputs(variable) +
                ": with fewer than two values per period the ECU sees a slower shape or a constant, not this one. " + longer +
                as_it_is_,
            owner_of(variable));
        return true;
    }
    if (smooth && period < kSmoothSteps * *step)
    {
        // The values the ECU takes repeat after `count` of them: if they are few, look at them.
        const std::int64_t count = period.count() / std::gcd(period.count(), step->count());
        if (count <= kSmoothSteps)
        {
            double low = factor(SimDuration::zero());
            double high = low;
            for (std::int64_t k = 1; k < count; ++k)
            {
                const double value = factor(k * *step);
                low = std::min(low, value);
                high = std::max(high, value);
            }
            if (high - low <= kSameValue)
            {
                const std::string shape = what.substr(0, what.find(" of "));
                add(config::Severity::Warning, "scenario-wave-undersampled", line,
                    what + " has a period of " + span_text(period) + ", and " + takes_inputs(variable) + ": at each of those times " +
                        shape + " is at " + (std::fabs(low) <= kSameValue ? "its middle" : "the same value") +
                        ", so the ECU sees a constant, not " + shape + ". " + longer + as_it_is_,
                    owner_of(variable));
                return true;
            }
        }
        add(config::Severity::Note, "scenario-wave-coarse", line,
            what + " has a period of " + span_text(period) + ", and " + takes_inputs(variable) + ": the ECU sees " +
                rounded_text(static_cast<double>(period.count()) / static_cast<double>(step->count())) +
                " values per period, so it can miss the highest and the lowest. A period of " +
                span_text(kSmoothSteps * *step) + " or more gives it " + std::to_string(kSmoothSteps) + as_it_is_,
            owner_of(variable));
    }
    return false;
}

void Reader::check_square(std::size_t variable, const std::string& what, std::size_t line, bool quiet, const detail::Periodic& periodic,
                          double duty)
{
    if (check_period(variable, what, line, quiet, periodic.period))
    {
        return;
    }
    const std::int64_t step = scenario_.participants[scenario_.variables[variable].participant].step.count();
    const std::int64_t period = periodic.period.count();
    if (period % step != 0)
    {
        add(config::Severity::Warning, "scenario-wave-duty", line,
            what + " has a period of " + span_text(periodic.period) + ", which is not a multiple of " + step_of(variable) +
                ": the ECU sees high and low times that change from period to period. Make the period a multiple of " +
                span_text(SimDuration{step}) + as_it_is_,
            owner_of(variable));
        return;
    }
    // The ECU takes the values at 0, step, 2 step, ... of the wave: those of them that are high.
    const std::int64_t high = signal::square_high_time(periodic.period, duty).count();
    const std::int64_t first = signal::time_in_period(SimDuration::zero(), periodic.period, periodic.phase_deg).count() % step;
    const std::int64_t seen = high > first ? (high - first + step - 1) / step * step : 0;
    if (seen != high)
    {
        add(config::Severity::Warning, "scenario-wave-duty", line,
            what + " is high for " + span_text(SimDuration{high}) + " of each period of " + span_text(periodic.period) + " (duty " +
                number_text(duty) + " %), but " + takes_inputs(variable) + ": the ECU sees it high for " +
                span_text(SimDuration{seen}) + " of each period (" +
                rounded_text(100.0 * static_cast<double>(seen) / static_cast<double>(period)) +
                " %). Make the high time (period x duty / 100) a multiple of " + span_text(SimDuration{step}) + as_it_is_,
            owner_of(variable));
    }
}

void Reader::check_span(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration span, const std::string& is,
                        const std::string& sees, const std::string& make)
{
    const auto step = sampling(variable, what, line, quiet);
    if (step && span < *step)
    {
        add(config::Severity::Warning, "scenario-wave-undersampled", line,
            what + " " + is + " " + step_of(variable) + ": " + sees + ". " + make + as_it_is_, owner_of(variable));
    }
}

Reader::Seen Reader::seen_between(SimDuration step, const std::optional<std::int64_t>& time, SimDuration from, SimDuration to, SimDuration length,
                                  SimDuration& written, SimDuration& ends)
{
    const std::int64_t s = step.count();
    const std::int64_t first = (from.count() + s - 1) / s * s; // the first regular write at or after `from`
    if (first >= to.count())
    {
        return Seen::No;
    }
    if (first + s <= length.count())
    {
        return Seen::Yes; // the write after it is a regular one
    }
    written = SimDuration{first};
    ends = length;
    if (!time)
    {
        return Seen::Depends;
    }
    const SimDuration begins = std::chrono::milliseconds(*time);
    written += begins;
    ends += begins;
    const std::int64_t applied = (written.count() + s - 1) / s * s; // the ECU's first step at or after the write
    return applied < ends.count() ? Seen::Yes : Seen::No;
}

void Reader::check_pulse(std::size_t variable, const std::string& what, std::size_t line, bool quiet, SimDuration width,
                         const std::optional<std::int64_t>& time)
{
    const auto step = sampling(variable, what, line, quiet);
    if (!step || width >= *step)
    {
        return;
    }
    SimDuration written{};
    SimDuration ends{};
    std::string sees;
    switch (seen_between(*step, time, SimDuration::zero(), width, width, written, ends))
    {
    case Seen::Yes:
        sees = "this step begins at " + span_text(written) + ", so the ECU has the pulse's value for one whole step of " +
               span_text(*step) + " (the one at " + span_text(SimDuration{(written.count() + step->count() - 1) / step->count() * step->count()}) +
               "), not for " + span_text(width);
        break;
    case Seen::No:
        sees = "this step begins at " + span_text(written) + ", and none of the ECU's steps is from then until the pulse ends (" +
               span_text(ends) + "), so the ECU never gets the pulse's value";
        break;
    case Seen::Depends:
        sees = "the ECU has the pulse's value for one whole step if one of its steps is from the pulse's beginning until its end, "
               "and never otherwise; when this step begins is not known before the run";
        break;
    }
    add(config::Severity::Warning, "scenario-wave-undersampled", line,
        what + " is " + span_text(width) + " wide, shorter than " + step_of(variable) + ": " + sees + ". Make width_ms " +
            whole_ms(*step) + " or more" + as_it_is_,
        owner_of(variable));
}

std::int64_t Reader::skipped_wave_ms(ryml::id_type node)
{
    if (!tree().is_map(node))
    {
        return 0;
    }
    std::int64_t length = 0;
    const auto child = [&](ryml::id_type of, const std::string& key) {
        if (tree().is_map(of))
        {
            for (auto part = tree().first_child(of); part != ryml::NONE; part = tree().next_sibling(part))
            {
                if (config::detail::to_string(tree().key(part)) == key)
                {
                    return part;
                }
            }
        }
        return static_cast<ryml::id_type>(ryml::NONE);
    };
    const auto whole = [&](ryml::id_type of) {
        std::int64_t value = 0;
        return of != ryml::NONE && plain_integer(of, value) && value >= 0 && value <= config::kMaxMillis ? value : std::int64_t{0};
    };
    if (const auto forMs = child(node, "for_ms"); forMs != ryml::NONE)
    {
        return millis(forMs, "for_ms", 1, "milliseconds of virtual time the wave takes").value_or(0);
    }
    if (const auto pulse = child(node, "pulse"); pulse != ryml::NONE)
    {
        length = whole(child(pulse, "width_ms"));
    }
    else if (const auto chatter = child(node, "chatter"); chatter != ryml::NONE)
    {
        const std::int64_t bounces = whole(child(chatter, "bounces"));
        const std::int64_t interval = whole(child(chatter, "interval_ms"));
        length = bounces <= kMaxBounces && interval <= config::kMaxMillis / (2 * kMaxBounces) ? 2 * bounces * interval : 0;
    }
    else if (const auto points = child(node, "points"); points != ryml::NONE && tree().is_seq(points) && tree().has_children(points))
    {
        const ryml::id_type last = tree().last_child(points);
        length = tree().is_seq(last) && tree().has_children(last) ? whole(tree().first_child(last)) : 0;
    }
    else if (points != ryml::NONE && tree().is_map(points))
    {
        // The time on the file's last line, if it can be read.
        const auto name = [&](const std::string& key) {
            const auto of = child(points, key);
            return of != ryml::NONE && is_scalar(of) && !is_null(of) ? std::optional<std::string>{text_of(of)} : std::nullopt;
        };
        const auto path = name("file");
        std::string normalizedPath;
        const CsvFile* file =
            path && !resolve_scoped_path(scope().dir_rel, *path, normalizedPath, false) ? &csv(normalizedPath) : nullptr;
        if (file != nullptr && file->table)
        {
            const auto column = name("time");
            const auto& columns = file->table->columns;
            const auto at = column ? std::find(columns.begin(), columns.end(), *column) : columns.begin();
            double ms = 0;
            const auto end = at == columns.end() ? std::nullopt
                                                 : cell_ms(file->table->rows.back().cells[static_cast<std::size_t>(at - columns.begin())],
                                                           name("time_unit") == std::optional<std::string>{"s"}, ms);
            length = end.value_or(0);
        }
    }
    return length;
}

std::int64_t Reader::read_wave(ryml::id_type node, ScenarioStep& step, StepReadState& state, const std::optional<std::int64_t>& time, bool quiet)
{
    std::int64_t longest = 0;
    if (!tree().is_map(node) || !tree().has_children(node))
    {
        error("value", doc().line(node),
              "wave must be a mapping of variables to a shape and how long it lasts ({ " + example_variable() +
                  ": { sine: { amplitude: 1, period_ms: 100 }, for_ms: 1000 } }), not " + shown_value(node));
        return longest;
    }
    std::map<std::string, std::size_t> seen;
    for (auto child = tree().first_child(node); child != ryml::NONE; child = tree().next_sibling(child))
    {
        if (!first_occurrence(seen, config::detail::to_string(tree().key(child)), child, "wave: "))
        {
            continue;
        }
        const std::size_t absentBefore = state.absent.size();
        const auto v = variable(child, state.absent);
        if (!v)
        {
            if (state.absent.size() > absentBefore)
            {
                longest = std::max(longest, skipped_wave_ms(child));
            }
            continue;
        }
        if (auto one = read_wave_of(child, *v, state, time, quiet))
        {
            longest = std::max(longest, one->length_ms);
            step.motions.push_back(std::move(*one));
        }
    }
    return longest;
}

bool Reader::read_shape(const std::string& shape, ryml::id_type body, const Parts& top_parts, std::size_t v, const std::string& what,
                        std::size_t line, const std::optional<std::int64_t>& time, bool quiet, bool& good, ScenarioMotion& out,
                        std::string& text, std::optional<std::int64_t>& length, bool& wantsFor, std::string& noAround,
                        std::optional<double>& swing, MakeShape& make, StepReadState* state)
{
    const Parts* top = &top_parts;
    const ScenarioVariable& var = scenario_.variables[v];
    const std::string name = var.text;
    const bool isBool = var.type == control::ValueType::Bool;
    static const std::map<std::string, std::vector<std::string>> kShapeKeys = {
        {"sine", {"amplitude", "period_ms", "frequency_hz", "phase_deg"}},
        {"triangle", {"amplitude", "period_ms", "frequency_hz", "phase_deg"}},
        {"sawtooth", {"amplitude", "period_ms", "frequency_hz", "phase_deg", "falling"}},
        {"square", {"low", "high", "amplitude", "period_ms", "frequency_hz", "duty", "phase_deg"}},
        {"pulse", {"value", "width_ms"}},
        {"noise", {"amplitude", "hold_ms", "seed", "distribution"}},
        {"chirp", {"amplitude", "from_hz", "to_hz", "sweep"}},
        {"lag", {"to", "tau_ms", "from", "settle"}},
        {"chatter", {"to", "interval_ms", "bounces", "seed", "from"}},
    };
    static const std::map<std::string, std::string> kShapeExample = {
        {"sine", "{ amplitude: 1, period_ms: 100 }"},
        {"triangle", "{ amplitude: 1, period_ms: 100 }"},
        {"sawtooth", "{ amplitude: 1, period_ms: 100 }"},
        {"square", "{ low: 0, high: 1, period_ms: 100 }"},
        {"pulse", "{ value: 1, width_ms: 50 }"},
        {"noise", "{ amplitude: 1, hold_ms: 10, seed: 1 }"},
        {"chirp", "{ amplitude: 1, from_hz: 1, to_hz: 10 }"},
        {"lag", "{ to: 1, tau_ms: 100 }"},
        {"chatter", "{ to: 1, bounces: 3, interval_ms: 10 }"},
    };
    const auto parts = parts_of(body, what, kShapeKeys.at(shape), "a mapping of its values (" + kShapeExample.at(shape) + ")");
    if (!parts)
    {
        return false;
    }
    const auto has = [&](const char* key) { return parts->count(key) > 0; };
    // A key the shape cannot do without: `meaning` says what it is.
    const auto need = [&](const char* key, const std::string& meaning) {
        if (!has(key))
        {
            error("missing-key", line, what + " needs " + key + ": " + meaning);
            good = false;
            return false;
        }
        return true;
    };
    const auto amplitude_of = [&](const std::string& meaning) {
        std::optional<double> amplitude;
        if (need("amplitude", meaning))
        {
            amplitude = measure(parts->at("amplitude"), "amplitude of " + what, 0.0, false, HUGE_VAL, meaning);
            good = good && amplitude.has_value();
            text += amplitude ? " amplitude " + number_text(*amplitude) : "";
        }
        return amplitude.value_or(0.0);
    };

    if (shape == "sine" || shape == "triangle" || shape == "sawtooth")
    {
        const double amplitude = amplitude_of("how far the value goes above and below the middle");
        const auto periodic = periodic_of(*parts, what, line, text);
        bool falling = false;
        if (has("falling"))
        {
            const auto said = flag(parts->at("falling"), "falling of " + what, "true: each period goes down and jumps up");
            good = good && said.has_value();
            falling = said.value_or(false);
            text += falling ? " falling" : "";
        }
        good = good && periodic.has_value();
        swing = amplitude;
        if (periodic)
        {
            const detail::Periodic p = *periodic;
            check_period(v, what, line, quiet, p.period, [&](SimDuration t) {
                return shape == "sine" ? signal::sine(t, p.period, p.phase_deg)
                       : shape == "triangle" ? signal::triangle(t, p.period, p.phase_deg)
                                             : signal::sawtooth(t, p.period, falling, p.phase_deg);
            });
            make = [=](SimDuration) {
                return shape == "sine" ? detail::sine_shape(amplitude, *periodic)
                       : shape == "triangle" ? detail::triangle_shape(amplitude, *periodic)
                                             : detail::sawtooth_shape(amplitude, *periodic, falling);
            };
        }
    }
    else if (shape == "square")
    {
        std::optional<double> low;
        std::optional<double> high;
        std::optional<double> amplitude;
        const bool levels = has("low") || has("high");
        if (isBool)
        {
            for (const char* key : {"low", "high", "amplitude"})
            {
                if (has(key))
                {
                    error("value", doc().line(parts->at(key)),
                          std::string(key) + " does not go with " + what + ": the square of a bool is false and true; remove it");
                    good = false;
                }
            }
            low = 0.0;
            high = 1.0;
        }
        else if (levels && has("amplitude"))
        {
            error("value", doc().line(parts->at("amplitude")),
                  what + " has amplitude and " + (has("low") ? "low" : "high") +
                      ": give low and high (its two values), or amplitude (so far above and below the middle)");
            good = false;
        }
        else if (levels)
        {
            if (need("low", "the value while it is low") && need("high", "the value while it is high"))
            {
                low = level(parts->at("low"), v, "low", "the square", state);
                high = level(parts->at("high"), v, "high", "the square", state);
                good = good && low && high;
                text += low && high ? " low " + number_text(*low) + " high " + number_text(*high) : "";
            }
        }
        else
        {
            amplitude = amplitude_of("how far the value goes above and below the middle; or give low and high, its two values");
            swing = amplitude;
        }
        const auto periodic = periodic_of(*parts, what, line, text);
        good = good && periodic.has_value();
        double duty = 50.0;
        if (has("duty"))
        {
            const auto said = measure(parts->at("duty"), "duty of " + what, 0.0, false, 100.0,
                                      "the percent of each period for which it is high");
            good = good && said.has_value();
            duty = said.value_or(50.0);
            text += said ? " duty " + number_text(duty) + " %" : "";
        }
        if (!amplitude)
        {
            noAround = isBool ? "a bool has no middle" : "low and high are the values themselves";
        }
        if (periodic)
        {
            check_square(v, what, line, quiet, *periodic, duty);
            if (amplitude)
            {
                make = [=](SimDuration) { return detail::square_shape(*amplitude, *periodic, duty); };
            }
            else if (low && high)
            {
                make = [=](SimDuration) { return detail::square_levels_shape(*low, *high, *periodic, duty); };
            }
        }
    }
    else if (shape == "pulse")
    {
        std::optional<double> value;
        if (isBool && !has("value"))
        {
            value = 1.0;
        }
        else if (need("value", "the value during the pulse"))
        {
            value = level(parts->at("value"), v, "value", "the pulse", state);
            good = good && value.has_value();
        }
        text += value ? " " + (isBool ? std::string(*value != 0.0 ? "true" : "false") : number_text(*value)) : "";
        if (need("width_ms", "how long the pulse lasts, in milliseconds of virtual time"))
        {
            length = millis(parts->at("width_ms"), "width_ms", 1, "milliseconds of virtual time the pulse lasts");
            good = good && length.has_value();
            text += length ? " width " + std::to_string(*length) + " ms" : "";
        }
        wantsFor = false;
        if (isBool)
        {
            noAround = "a bool has no middle (the pulse returns to the other value)";
            out.base = value.value_or(1.0) != 0.0 ? 0.0 : 1.0;
        }
        else
        {
            noAround = "a pulse returns to the value the variable has when the pulse begins (for another value after it, "
                       "write a set in the next step)";
            out.hint = "set the variable in a step before the pulse (set: { " + name + ": <value> })";
        }
        if (length)
        {
            check_pulse(v, what, line, quiet, std::chrono::milliseconds(*length), time);
        }
        if (value)
        {
            make = [=](SimDuration) { return detail::pulse_shape(*value); };
        }
    }
    else if (shape == "noise")
    {
        bool gaussian = false;
        if (has("distribution"))
        {
            const auto said = choice(parts->at("distribution"), "distribution of " + what, {"uniform", "gaussian"});
            good = good && said.has_value();
            gaussian = said && *said == "gaussian";
        }
        const double amplitude = amplitude_of(gaussian ? "the standard deviation of the values around the middle"
                                                       : "how far the values go above and below the middle at most");
        std::optional<std::int64_t> hold;
        if (need("hold_ms", "how long each value is kept, in milliseconds of virtual time"))
        {
            hold = millis(parts->at("hold_ms"), "hold_ms", 1, "milliseconds of virtual time each value is kept");
            good = good && hold.has_value();
            text += hold ? " hold " + std::to_string(*hold) + " ms" : "";
        }
        std::optional<std::uint64_t> seed;
        if (need("seed", "a whole number that picks the values; the same seed gives the same values in every run, so that a "
                         "failure can be run again"))
        {
            seed = seed_of(parts->at("seed"), "seed of " + what);
            good = good && seed.has_value();
            text += seed ? " seed " + std::to_string(*seed) : "";
        }
        text += gaussian ? " gaussian" : "";
        if (!gaussian)
        {
            swing = amplitude; // a gaussian value has no bound worth checking: the run refuses what is outside
        }
        if (hold && seed)
        {
            const SimDuration held = std::chrono::milliseconds(*hold);
            check_span(v, what, line, quiet, held, "changes every " + span_text(held) + ", faster than",
                       "the ECU gets only the value at the time of each of its steps; the values between are never applied",
                       "Make hold_ms " + whole_ms(step_ns(v)) + " or more");
            // (The variable's name is part of the seed: the same seed for two variables is two sequences.)
            make = [=, mixed = detail::seed_of_variable(*seed, name)](SimDuration) {
                return detail::noise_shape(amplitude, held, mixed, gaussian);
            };
        }
    }
    else if (shape == "chirp")
    {
        const double amplitude = amplitude_of("how far the value goes above and below the middle");
        std::optional<double> fromHz;
        std::optional<double> toHz;
        // (0 Hz is a frequency: a linear sweep can begin or end standing still.)
        if (need("from_hz", "the frequency at the beginning, in periods per second of virtual time"))
        {
            fromHz = measure(parts->at("from_hz"), "from_hz of " + what, 0.0, false, 1e9, "the frequency at the beginning");
            good = good && fromHz.has_value();
        }
        if (need("to_hz", "the frequency at the end (after for_ms)"))
        {
            toHz = measure(parts->at("to_hz"), "to_hz of " + what, 0.0, false, 1e9, "the frequency at the end");
            good = good && toHz.has_value();
        }
        text += fromHz && toHz ? " from " + number_text(*fromHz) + " Hz to " + number_text(*toHz) + " Hz" : "";
        signal::Sweep sweep = signal::Sweep::Linear;
        if (has("sweep"))
        {
            const auto said = choice(parts->at("sweep"), "sweep of " + what, {"linear", "log"});
            good = good && said.has_value();
            sweep = said && *said == "log" ? signal::Sweep::Log : signal::Sweep::Linear;
            text += sweep == signal::Sweep::Log ? " log" : "";
        }
        swing = amplitude;
        if (fromHz && toHz && sweep == signal::Sweep::Log && (*fromHz == 0.0 || *toHz == 0.0))
        {
            const char* key = *fromHz == 0.0 ? "from_hz" : "to_hz";
            error("value", doc().line(parts->at(key)),
                  std::string(key) + " of " + what + " is 0, but a log sweep changes the frequency by the same factor every second, "
                  "which neither begins nor ends at 0 Hz: give a frequency above 0, or write sweep: linear");
            good = false;
        }
        else if (fromHz && toHz && *fromHz == 0.0 && *toHz == 0.0)
        {
            error("value", doc().line(parts->at("to_hz")),
                  what + " goes from 0 Hz to 0 Hz, which does not move: give from_hz or to_hz a frequency above 0");
            good = false;
        }
        else if (fromHz && toHz)
        {
            check_chirp(v, what, line, quiet, std::max(*fromHz, *toHz));
            make = [=](SimDuration all) { return detail::chirp_shape(amplitude, *fromHz, *toHz, all, sweep); };
        }
    }
    else if (shape == "lag")
    {
        std::optional<double> to;
        if (need("to", "the value it goes towards"))
        {
            to = level(parts->at("to"), v, "to", "the lag", state);
            good = good && to.has_value();
        }
        if (has("from"))
        {
            out.base = level(parts->at("from"), v, "from", "the lag", state);
            good = good && out.base.has_value();
            text += out.base ? " from " + number_text(*out.base) : "";
        }
        text += to ? " to " + number_text(*to) : "";
        std::optional<std::int64_t> tau;
        if (need("tau_ms", "its time constant in milliseconds of virtual time: after this long it has gone 63 % of the way"))
        {
            tau = millis(parts->at("tau_ms"), "tau_ms", 1, "the time constant in milliseconds of virtual time");
            good = good && tau.has_value();
            text += tau ? " tau " + std::to_string(*tau) + " ms" : "";
        }
        bool settle = false;
        if (has("settle"))
        {
            const auto said = flag(parts->at("settle"), "settle of " + what, "true: at the end the value is set to `to`");
            good = good && said.has_value();
            settle = said.value_or(false);
            text += settle ? " settle" : "";
        }
        noAround = "the value it starts from is its from (lag: { from: <value>, ... })";
        out.hint = "give the lag from: <value>";
        if (to && tau)
        {
            make = [=](SimDuration all) { return detail::lag_shape(*to, std::chrono::milliseconds(*tau), all, settle); };
        }
    }
    else // chatter
    {
        std::optional<double> to;
        if (need("to", "the value it comes to rest at"))
        {
            to = level(parts->at("to"), v, "to", "the chatter", state);
            good = good && to.has_value();
        }
        if (has("from") && isBool)
        {
            error("value", doc().line(parts->at("from")),
                  "from does not go with " + what + ": a bool bounces between to and the other value; remove it");
            good = false;
        }
        else if (has("from"))
        {
            out.base = level(parts->at("from"), v, "from", "the chatter", state);
            good = good && out.base.has_value();
            text += out.base ? " from " + number_text(*out.base) : "";
        }
        else if (isBool && to)
        {
            out.base = *to != 0.0 ? 0.0 : 1.0;
        }
        text += to ? " to " + (isBool ? std::string(*to != 0.0 ? "true" : "false") : number_text(*to)) : "";
        std::optional<std::int64_t> interval;
        if (need("interval_ms", "how long it stays at a value between two changes, in milliseconds of virtual time"))
        {
            interval = millis(parts->at("interval_ms"), "interval_ms", 1, "milliseconds of virtual time between two changes");
            good = good && interval.has_value();
        }
        std::optional<std::uint64_t> seed;
        if (has("seed"))
        {
            seed = seed_of(parts->at("seed"), "seed of " + what);
            good = good && seed.has_value();
        }
        const bool hasFor = top->count("for_ms") > 0;
        if (has("bounces") && hasFor)
        {
            error("value", doc().line(top->at("for_ms")),
                  what + " has bounces and for_ms: give one of them (bounces: how often it falls back; for_ms: how long it bounces)");
            good = false;
        }
        else if (has("bounces"))
        {
            std::int64_t bounces = 0;
            if (!plain_integer(parts->at("bounces"), bounces) || bounces < 1 || bounces > kMaxBounces)
            {
                error("value", doc().line(parts->at("bounces")),
                      "bounces of " + what + " must be a whole number from 1 to " + std::to_string(kMaxBounces) +
                          " (how often it falls back before it rests), not " + shown_value(parts->at("bounces")));
                good = false;
            }
            else if (interval && *interval > config::kMaxMillis / (2 * bounces))
            {
                error("value", doc().line(parts->at("bounces")),
                      what + " lasts 2 x bounces x interval_ms, more than the longest time (" + std::to_string(config::kMaxMillis) + " ms): lower them");
                good = false;
            }
            else if (interval)
            {
                // To, back, to, back, ...: two stretches per bounce, then it rests.
                length = 2 * bounces * *interval;
                text += " " + std::to_string(bounces) + (bounces == 1 ? " bounce" : " bounces");
            }
            wantsFor = false;
        }
        else if (!hasFor)
        {
            error("missing-key", line,
                  what + " needs bounces (how often it falls back before it rests) or for_ms (how long it bounces)");
            good = false;
            wantsFor = false;
        }
        text += interval ? " interval " + std::to_string(*interval) + " ms" : "";
        text += seed ? " seed " + std::to_string(*seed) : "";
        noAround = "the value it bounces back to is its from (chatter: { from: <value>, ... }), or the value it has";
        out.hint = "give the chatter from: <value>";
        if (interval)
        {
            // With a seed two changes are as little as half an interval apart.
            const SimDuration regular = std::chrono::milliseconds(*interval);
            const SimDuration apart = seed ? regular / 2 : regular;
            check_span(v, what, line, quiet, apart,
                       seed ? "changes as little as " + span_text(apart) + " apart (interval_ms " + std::to_string(*interval) +
                                  " with a seed: a half to one and a half intervals), closer than"
                            : "changes every " + span_text(apart) + ", faster than",
                       "the ECU gets only the value at the time of each of its steps, so it sees some of the bounces or none",
                       "Make interval_ms " + whole_ms(seed ? 2 * step_ns(v) : step_ns(v)) + " or more");
        }
        if (to && interval)
        {
            const std::optional<std::uint64_t> mixed = seed ? std::optional{detail::seed_of_variable(*seed, name)} : std::nullopt;
            make = [=](SimDuration) { return detail::chatter_shape(*to, std::chrono::milliseconds(*interval), mixed); };
        }
    }
    // The shape as the text has it (what it starts from is the base, not of the shape).
    for (const std::string& key : kShapeKeys.at(shape))
    {
        if (key != "from" && has(key.c_str()) && is_scalar(parts->at(key)))
        {
            out.described.values.emplace_back(key, shape_value(parts->at(key), v));
        }
    }
    return true;
}

std::optional<ScenarioMotion> Reader::read_wave_of(ryml::id_type node, std::size_t v, StepReadState& state,
                                                   const std::optional<std::int64_t>& time, bool quiet)
{
    const std::size_t line = doc().line(node);
    const ScenarioVariable& var = scenario_.variables[v];
    const std::string name = var.text;
    bool good = writable(v, line, time, "wave");
    const auto top = parts_of(node, "wave of " + name, kWaveKeys,
                              "a shape with its values, and how long it lasts ({ sine: { amplitude: 1, period_ms: 100 }, for_ms: 1000 })");
    if (!top)
    {
        return std::nullopt;
    }
    std::vector<std::string> shapes;
    for (const std::string& known : kWaveShapes)
    {
        if (top->count(known) > 0)
        {
            shapes.push_back(known);
        }
    }
    if (shapes.size() != 1)
    {
        error("value", line,
              shapes.empty() ? "wave of " + name + " names no shape: give it one of " + config::detail::join(kWaveShapes)
                             : "wave of " + name + " has " + config::detail::join(shapes) + ": a variable takes one shape in a step; make a step of each");
        return std::nullopt;
    }
    const std::string& shape = shapes.front();
    const ryml::id_type body = top->at(shape);
    const std::string what = "the " + shape + " of " + name;
    const bool isBool = var.type == control::ValueType::Bool;
    const bool isEnum = var.type == control::ValueType::Enum;
    const bool number = !isBool && !isEnum;
    const bool switching = shape == "square" || shape == "pulse" || shape == "chatter";
    if (!(number || shape == "points" || (isBool && switching)))
    {
        error("scenario-value", line,
              shape + " needs a number, and " + var.text + " is " +
                  (isBool ? "a bool: for a bool use square, pulse, chatter or points" : "an enum: for an enum use points, or set"));
        return std::nullopt;
    }

    ScenarioMotion out;
    out.variable = v;
    out.line = line;
    out.hint = "give the wave around: <value>";
    std::string text = name + " " + shape;
    std::optional<std::int64_t> length; // of a shape that says it itself
    bool wantsFor = true;               // for_ms is how long it lasts
    std::string noAround;               // why around does not go with it, or ""
    std::optional<double> swing;        // how far from around it goes at most, if that is known
    // What makes the shape, once everything is read: (around or its other base, the length).
    MakeShape make;

    if (shape == "points")
    {
        good = read_points(body, top->count("interpolate") > 0 ? top->at("interpolate") : ryml::NONE, v, what, time, quiet, out, text, length, make, state) && good;
        wantsFor = false;
        noAround = "the points are the values themselves (for where it begins, start with a point at 0: [0, <value>])";
    }
    else if (!read_shape(shape, body, *top, v, what, line, time, quiet, good, out, text, length, wantsFor, noAround, swing, make, &state))
    {
        return std::nullopt;
    }
    out.described.type = shape;

    // What stands beside the shape.
    if (top->count("interpolate") > 0 && shape != "points")
    {
        error("value", doc().line(top->at("interpolate")), "interpolate goes with points, not with " + shape + ": remove it");
        good = false;
    }
    if (top->count("for_ms") > 0)
    {
        if (wantsFor)
        {
            length = millis(top->at("for_ms"), "for_ms", 1, "milliseconds of virtual time the wave takes");
            good = good && length.has_value();
        }
        else if (shape != "chatter") // (a chatter with bounces and for_ms: said above)
        {
            error("value", doc().line(top->at("for_ms")),
                  "for_ms does not go with " + shape + ": " +
                      (shape == "pulse" ? "a pulse lasts its width_ms" : "the points end at the last of them") + "; remove it");
            good = false;
        }
    }
    else if (wantsFor)
    {
        error("missing-key", line, "wave of " + name + " needs for_ms: how long the " + shape + " lasts, in milliseconds of virtual time");
        good = false;
    }
    std::optional<double> around;
    if (top->count("around") > 0)
    {
        if (!noAround.empty())
        {
            error("value", doc().line(top->at("around")), "around does not go with " + what + ": " + noAround + "; remove it");
            good = false;
        }
        else
        {
            around = level(top->at("around"), v, "around", "the " + shape, &state);
            good = good && around.has_value();
            out.base = around;
        }
    }
    if (good && around && swing && (!in_range(*around - *swing, v) || !in_range(*around + *swing, v)))
    {
        error("scenario-value", line,
              what + " goes from " + number_text(*around - *swing) + " to " + number_text(*around + *swing) + " (around " +
                  number_text(*around) + ", amplitude " + number_text(*swing) + "), outside its range (" +
                  range_text(*info_of(v)) + "): lower the amplitude, or move around");
        good = false;
    }
    if (!good || !length || !make)
    {
        return std::nullopt;
    }
    if (around && swing)
    {
        // Where the wave takes the input: what a dither on top of it starts from.
        note_base(state, v, *around - *swing, line, false);
        note_base(state, v, *around + *swing, line, false);
        note_base(state, v, *around, line, true); // (where it leaves the input)
    }
    out.length_ms = *length;
    if (wantsFor)
    {
        out.described.values.emplace_back("for_ms", *length);
    }
    out.shape = make(std::chrono::milliseconds(*length));
    out.text = text + (around ? " around " + number_text(*around) : "") +
               (shape == "pulse" || shape == "points" ? "" : (shape == "chatter" && !wantsFor ? "" : " for " + std::to_string(*length) + " ms"));
    return out;
}

void Reader::check_chirp(std::size_t variable, const std::string& what, std::size_t line, bool quiet, double top_hz)
{
    const auto step = sampling(variable, what, line, quiet);
    if (!step)
    {
        return;
    }
    const double period = 1e9 / top_hz; // in nanoseconds
    const double steps = period / static_cast<double>(step->count());
    const auto hz_at = [&](std::int64_t count) { return rounded_text(1e9 / static_cast<double>(count * step->count())); };
    if (steps < 2.0)
    {
        add(config::Severity::Warning, "scenario-wave-undersampled", line,
            what + " goes up to " + number_text(top_hz) + " Hz (a period of " + rounded_text(period / 1e6) + " ms), but " +
                takes_inputs(variable) + ": above " + hz_at(2) +
                " Hz (two steps per period) the ECU sees a slower shape, not the sweep. Keep from_hz and to_hz at " + hz_at(2) +
                " or less" + as_it_is_,
            owner_of(variable));
    }
    else if (steps < static_cast<double>(kSmoothSteps))
    {
        add(config::Severity::Note, "scenario-wave-coarse", line,
            what + " goes up to " + number_text(top_hz) + " Hz (a period of " + rounded_text(period / 1e6) + " ms), and " +
                takes_inputs(variable) + ": at that frequency the ECU sees " + rounded_text(steps) +
                " values per period, so it can miss the highest and the lowest. Up to " + hz_at(kSmoothSteps) + " Hz it gets " +
                std::to_string(kSmoothSteps) + " or more" + as_it_is_,
            owner_of(variable));
    }
}

bool Reader::read_points(ryml::id_type body, ryml::id_type interpolate, std::size_t v, const std::string& what,
                         const std::optional<std::int64_t>& time, bool quiet, ScenarioMotion& out, std::string& text, std::optional<std::int64_t>& length,
                         std::function<std::shared_ptr<const Waveform>(SimDuration)>& make, StepReadState& state)
{
    const ScenarioVariable& var = scenario_.variables[v];
    const bool number = var.type == control::ValueType::Integer || var.type == control::ValueType::Real;
    bool good = true;
    bool hold = !number;
    if (interpolate != ryml::NONE)
    {
        const auto said = choice(interpolate, "interpolate of " + what, {"linear", "hold"});
        if (!said)
        {
            good = false;
        }
        else if (*said == "linear" && !number)
        {
            error("scenario-value", doc().line(interpolate),
                  "interpolate: linear needs a number, and " + var.text + " is " +
                      (var.type == control::ValueType::Bool ? "a bool" : "an enum") +
                      ": write interpolate: hold (each value is kept until the next point), or leave it out");
            good = false;
        }
        else
        {
            hold = *said == "hold";
        }
    }
    std::vector<signal::Point> points;
    std::vector<std::string> names; // of each point, as the messages name it
    std::optional<std::int64_t> before;
    std::optional<std::pair<std::int64_t, std::int64_t>> closest; // the two times that are closest
    // The next point (`after`: how a message names the one before it), or false and an error at `line`.
    const auto add = [&](std::int64_t at, double value, std::string name, const std::string& after, std::size_t line) {
        if (before && at <= *before)
        {
            error("value", line,
                  "the times of " + what + " must go up: " + name + " is at " + std::to_string(at) + " ms, not after " + after + " (" +
                      std::to_string(*before) + " ms)" +
                      (at == *before && !hold ? "; for a value that jumps, write interpolate: hold beside the points (each value is "
                                                "then kept until the next point)"
                                              : ""));
            return false;
        }
        if (before && (!closest || at - *before < closest->second - closest->first))
        {
            closest = std::pair{*before, at};
        }
        before = at;
        points.push_back(signal::Point{std::chrono::milliseconds(at), value});
        names.push_back(std::move(name));
        return true;
    };
    std::string source; // " from <file> column <name>" for points from a file
    if (tree().is_map(body))
    {
        const bool read = file_points(body, v, what, add, source, out.described, state);
        if (!good || !read)
        {
            return false;
        }
        out.hint = "begin the file with a line at 0 ms (0,<value>)";
    }
    else
    {
        if (!tree().is_seq(body) || !tree().has_children(body))
        {
            error("value", doc().line(body),
                  "points of " + var.text + " must be a list of [<milliseconds since the step began>, <value>] ([[0, 0], [100, 50]]), or " +
                      "a CSV file and its column (" + kPointsFileExample + "), not " +
                      (tree().is_seq(body) ? std::string("an empty list") : shown_value(body)));
            return false;
        }
        std::size_t index = 0;
        for (auto item = tree().first_child(body); item != ryml::NONE; item = tree().next_sibling(item))
        {
            ++index;
            const std::string label = "point " + std::to_string(index);
            if (!tree().is_seq(item) || tree().num_children(item) != 2)
            {
                error("value", doc().line(item),
                      label + " of " + what + " must be [<milliseconds since the step began>, <value>], not " + shown_value(item));
                good = false;
                continue;
            }
            const ryml::id_type timeNode = tree().first_child(item);
            const ryml::id_type valueNode = tree().last_child(item);
            const auto at = millis(timeNode, "the time of " + label, 0, "milliseconds of virtual time since the step began");
            const auto value = level(valueNode, v, "the value of " + label, "the points", &state);
            if (!at || !value || !add(*at, *value, label, "point " + std::to_string(index - 1), doc().line(item)))
            {
                good = false;
            }
        }
        if (!good)
        {
            return false;
        }
        out.hint = "begin the points with one at 0 ms ([0, <value>])";
    }
    if (points.back().time <= SimDuration::zero())
    {
        error("value", doc().line(body),
              what + " end at 0 ms: a wave takes time; give a later point, or use set for a value at one time");
        return false;
    }
    length = points.back().time.count() / 1'000'000;
    text += source + " (" + std::to_string(points.size()) + (hold ? ", hold" : ", linear") + ") over " + std::to_string(*length) + " ms";
    if (hold)
    {
        check_held_points(v, what, doc().line(body), quiet, points, names, time);
    }
    else if (closest)
    {
        // On lines every point is a corner: closer than a step, the ECU misses some of them.
        const SimDuration apart = std::chrono::milliseconds(closest->second - closest->first);
        check_span(v, what, doc().line(body), quiet, apart,
                   "are as little as " + span_text(apart) + " apart (at " + std::to_string(closest->first) + " ms and " +
                       std::to_string(closest->second) + " ms), closer than",
                   "the ECU gets only the value at the time of each of its steps, so it does not see every point",
                   "Put the points " + span_text(step_ns(v)) + " or more apart");
    }
    out.described.values.insert(out.described.values.begin(),
                                {{"points", static_cast<std::int64_t>(points.size())}, {"interpolate", std::string(hold ? "hold" : "linear")}});
    make = [points = std::move(points), hold](SimDuration) { return detail::points_shape(points, hold); };
    return true;
}

const CsvFile& Reader::csv(const std::string& path)
{
    if (const auto it = cache_.files.find(path); it != cache_.files.end())
    {
        return it->second;
    }
    CsvFile file;
    if (!context_.read_file)
    {
        file.problem = "is not read here: points from a file are read with the scenario file (check --scenario, run --scenario)";
    }
    else if (const auto text = context_.read_file(path, file.problem))
    {
        file.table = detail::read_csv(*text, file.problem);
    }
    return cache_.files.emplace(path, std::move(file)).first->second;
}

std::optional<std::int64_t> Reader::cell_ms(const std::string& cell, bool seconds, double& ms)
{
    double read = 0;
    if (!read_number(cell, read))
    {
        ms = std::nan("");
        return std::nullopt;
    }
    ms = seconds ? read * 1000.0 : read;
    const double whole = std::round(ms);
    // A time in seconds is whole milliseconds when it differs from them only in its last bits (0.001 * 1000).
    if (std::abs(ms - whole) > 1e-6 + std::abs(ms) * 4 * DBL_EPSILON)
    {
        return std::nullopt;
    }
    ms = whole;
    if (whole < 0 || whole > static_cast<double>(config::kMaxMillis))
    {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(whole);
}

std::optional<double> Reader::cell_value(const std::string& cell, std::size_t variable, const std::string& where, std::size_t line,
                                         StepReadState& state)
{
    const ScenarioVariable& var = scenario_.variables[variable];
    const control::VariableInfo& info = *info_of(variable);
    const std::string pre = "the value on " + where + " for " + var.text + " (" + type_name(var.type) + ")";
    const Hex hex = read_hex(cell);
    switch (var.type)
    {
    case control::ValueType::Bool:
        if (cell == "true" || cell == "1" || cell == "false" || cell == "0")
        {
            return cell == "true" || cell == "1" ? 1.0 : 0.0;
        }
        error("scenario-value", line, pre + " must be true, false, 1 or 0, not " + detail::shown_cell(cell));
        return std::nullopt;
    case control::ValueType::Enum:
    {
        std::int64_t i = 0;
        const bool number = read_integer(cell, i) || hex.kind == Hex::Kind::Read;
        if (hex.kind == Hex::Kind::Read)
        {
            i = static_cast<std::int64_t>(hex.magnitude);
        }
        const auto item = std::find_if(info.items.begin(), info.items.end(),
                                       [&](const control::EnumItem& e) { return number ? e.value == i : e.name == cell; });
        if (item != info.items.end())
        {
            return static_cast<double>(item->value);
        }
        error("scenario-value", line,
              pre + " must be one of its items, by name or by value, not " + detail::shown_cell(cell) + ": the items are " +
                  items_text(info.items));
        return std::nullopt;
    }
    case control::ValueType::Integer:
    case control::ValueType::Real:
        break;
    }
    double d = 0;
    if (!read_number(cell, d))
    {
        if (var.type != control::ValueType::Integer || hex.kind != Hex::Kind::Read)
        {
            error("scenario-value", line, pre + " must be a number, not " + detail::shown_cell(cell));
            return std::nullopt;
        }
        d = static_cast<double>(hex.magnitude);
    }
    if (!in_range(d, variable))
    {
        error("scenario-value", line, pre + " is " + number_text(d) + ", outside its range (" + range_text(info) + ")");
        return std::nullopt;
    }
    note_base(state, variable, d, line, false);
    return d;
}

template <class Add>
bool Reader::file_points(ryml::id_type body, std::size_t v, const std::string& what, const Add& add, std::string& source,
                         ScenarioShapeText& described, StepReadState& state)
{
    const auto parts = parts_of(body, what, kPointsFileKeys, std::string("a CSV file and its column (") + kPointsFileExample + ")");
    if (!parts)
    {
        return false;
    }
    bool good = true;
    // The name in `key` (a column, the file), or empty and an error.
    const auto name = [&](const std::string& key, const std::string& meaning) -> std::optional<std::string> {
        if (parts->count(key) == 0)
        {
            return std::nullopt;
        }
        const ryml::id_type node = parts->at(key);
        if (!is_scalar(node) || is_null(node) || text_of(node).empty())
        {
            error("value", doc().line(node), key + " of " + what + " must be " + meaning + ", not " + shown_value(node));
            good = false;
            return std::nullopt;
        }
        return text_of(node);
    };
    for (const auto& [key, meaning] : {std::pair<std::string, std::string>{"file", "the CSV file, from the scenario file's directory"},
                                       {"value", "the name of the column of the values"}})
    {
        if (parts->count(key) == 0)
        {
            error("missing-key", doc().line(body), what + " from a file need " + key + ": " + meaning + " (" + kPointsFileExample + ")");
            good = false;
        }
    }
    const auto path = name("file", "the path of the CSV file from the scenario file's directory (traces/drive.csv)");
    const auto valueName = name("value", "the name of the column of the values");
    const auto timeName = name("time", "the name of the column of the times (without it, the first column)");
    bool seconds = false;
    if (parts->count("time_unit") > 0)
    {
        const auto unit = choice(parts->at("time_unit"), "time_unit of " + what, {"ms", "s"});
        good = unit.has_value() && good;
        seconds = unit == std::optional<std::string>{"s"};
    }
    if (!good || !path || !valueName)
    {
        return false;
    }
    const std::size_t line = doc().line(parts->at("file"));
    std::string normalizedPath;
    if (const auto wrong = resolve_scoped_path(scope().dir_rel, *path, normalizedPath, false))
    {
        error("scenario-points-file", line, what + ": the file '" + *path + "' " + *wrong);
        return false;
    }
    const CsvFile& file = csv(normalizedPath);
    if (!file.table)
    {
        error("scenario-points-file", line, what + ": " + *path + " " + file.problem);
        return false;
    }
    const detail::CsvTable& table = *file.table;
    // The index of a column, or empty and an error.
    const auto column = [&](const std::string& wanted, const std::string& key) -> std::optional<std::size_t> {
        std::vector<std::size_t> found;
        for (std::size_t i = 0; i < table.columns.size(); ++i)
        {
            if (table.columns[i] == wanted)
            {
                found.push_back(i + 1);
            }
        }
        if (found.size() == 1)
        {
            return found.front() - 1;
        }
        if (found.size() > 1)
        {
            error("scenario-points-file", doc().line(parts->at(key)),
                  what + ": " + *path + " has " + std::to_string(found.size()) + " columns named " + detail::shown_cell(wanted) +
                      " (columns " + std::to_string(found[0]) + " and " + std::to_string(found[1]) +
                      "): give its columns different names on its first line");
            return std::nullopt;
        }
        std::vector<std::string> shown;
        for (std::size_t i = 0; i < table.columns.size() && i < 10; ++i)
        {
            shown.push_back(detail::shown_cell(table.columns[i]));
        }
        const std::string guess = config::detail::closest(wanted, table.columns);
        error("scenario-points-file", doc().line(parts->at(key)),
              what + ": " + *path + " has no column " + detail::shown_cell(wanted) + " on its first line (line " +
                  std::to_string(table.header_line) + ")" +
                  (guess.empty() ? std::string{} : ": did you mean " + detail::shown_cell(guess) + "?") + "; its columns are " +
                  config::detail::join(shown) + (table.columns.size() > shown.size() ? " and " + std::to_string(table.columns.size() - shown.size()) + " more" : ""));
        return std::nullopt;
    };
    const auto valueColumn = column(*valueName, "value");
    const auto timeColumn = timeName ? column(*timeName, "time") : std::optional<std::size_t>{0};
    if (!valueColumn || !timeColumn)
    {
        return false;
    }
    const std::string& timeHead = table.columns[*timeColumn];
    if (*valueColumn == *timeColumn)
    {
        error("scenario-points-file", doc().line(parts->at("value")),
              what + ": value names the column of the times (" + detail::shown_cell(timeHead) + (timeName ? "" : ", the first column") +
                  "): name the column of the values" + (timeName ? "" : ", or the column of the times with time"));
        return false;
    }
    std::string after;
    for (const detail::CsvRow& row : table.rows)
    {
        const std::string here = "line " + std::to_string(row.line);
        const std::string at = here + " of " + *path;
        const std::string& cell = row.cells[*timeColumn];
        double ms = 0;
        const auto time = cell_ms(cell, seconds, ms);
        if (!time)
        {
            const std::string pre = "the time on " + at + " (column " + detail::shown_cell(timeHead) + ")";
            const std::string unit = seconds ? "seconds" : "milliseconds";
            error("value", line,
                  std::isnan(ms) ? pre + " must be a number of " + unit + " since the step began, not " + detail::shown_cell(cell) +
                                       (seconds || timeName ? "" : " (the times are in the first column unless time names another)")
                  : std::round(ms) != ms
                      ? pre + " is " + number_text(ms) + " ms: the times of points are whole milliseconds; round them in the file"
                      : pre + " is " + number_text(ms) + " ms, but it must be from 0 to " + std::to_string(config::kMaxMillis) +
                            " (milliseconds of virtual time since the step began)");
            return false;
        }
        const auto value = cell_value(row.cells[*valueColumn], v, at + " (column " + detail::shown_cell(*valueName) + ")", line, state);
        if (!value || !add(*time, *value, at, after, line))
        {
            return false;
        }
        after = here;
    }
    source = " from " + *path + " column " + *valueName;
    described.values.emplace_back("file", *path);
    described.values.emplace_back("value", *valueName);
    if (timeName)
    {
        described.values.emplace_back("time", *timeName);
    }
    if (parts->count("time_unit") > 0)
    {
        described.values.emplace_back("time_unit", std::string(seconds ? "s" : "ms"));
    }
    return true;
}

void Reader::check_held_points(std::size_t variable, const std::string& what, std::size_t line, bool quiet,
                               const std::vector<signal::Point>& points, const std::vector<std::string>& names,
                               const std::optional<std::int64_t>& time)
{
    const auto step = sampling(variable, what, line, quiet);
    if (!step)
    {
        return;
    }
    const std::string head = what + " keep ";
    const std::string fix = ". Put the points at multiples of " + span_text(*step) + kAsItIs;
    std::string unknown;
    for (std::size_t i = 0; i + 1 < points.size(); ++i)
    {
        SimDuration written{};
        SimDuration ends{};
        const Seen seen = seen_between(*step, time, points[i].time, points[i + 1].time, points.back().time, written, ends);
        const bool last = i + 2 == points.size();
        const std::string span = names[i] + " from " + span_text(points[i].time) + " to " + span_text(points[i + 1].time) +
                                 (last ? ", where they end" : "") + ", and " + takes_inputs(variable) + ": ";
        if (seen == Seen::No && written == SimDuration::zero() && ends == SimDuration::zero())
        {
            add(config::Severity::Warning, "scenario-wave-undersampled", line,
                head + span + "the wave writes a value every " + span_text(*step) + " from the step's beginning, and none of those "
                "times is in that span, so the value of " + names[i] + " is never applied" + fix,
                owner_of(variable));
            return;
        }
        if (seen == Seen::No)
        {
            add(config::Severity::Warning, "scenario-wave-undersampled", line,
                head + span + "this step begins at " + std::to_string(*time) + " ms, and none of the ECU's steps is from " +
                    span_text(written) + " (when the value of " + names[i] + " is written) until the points end (" +
                    span_text(ends) + "), so that value is never applied" + fix,
                owner_of(variable));
            return;
        }
        if (seen == Seen::Depends && unknown.empty())
        {
            unknown = head + span + "the value of " + names[i] + " is applied only if one of the ECU's steps is from " +
                      span_text(written) + " to " + span_text(ends) + " after this step begins, and when it begins is not known before the run" + fix;
        }
    }
    if (!unknown.empty())
    {
        add(config::Severity::Warning, "scenario-wave-undersampled", line, unknown, owner_of(variable));
    }
}

} // namespace vehicle_sil::scenario::reader
