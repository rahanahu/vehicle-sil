// SPDX-License-Identifier: MIT
//
// The scenario's reader (scenario_reader.hpp): "inputs", dithers, and what the steps put on the
// inputs as the text says it.
#include "scenario_reader.hpp"

namespace vehicle_sil::scenario::reader {

void Reader::note_base(std::size_t variable, double value, std::size_t line, bool stays)
{
    step_bases_.push_back(Base{variable, value, current_file_, line, stays});
}

void Reader::note_lines(const ScenarioStep& step, const std::optional<std::int64_t>& time)
{
    const std::size_t index = scenario_.steps.size(); // (it is pushed after this)
    for (const Base& base : step_bases_)
    {
        flow_.push_back(Flow{index, base, std::nullopt});
    }
    for (const Dither& dither : step_dithers_)
    {
        flow_.push_back(Flow{index, std::nullopt, dithers_.size()});
        dithers_.push_back(dither);
    }
    const auto put = [&](std::size_t variable, const std::optional<std::int64_t>& length, const std::string& text,
                         const ScenarioShapeText* shape) {
        ScenarioLineText line;
        line.variable = scenario_.variables[variable].text;
        line.kind = step_kind_name(step.kind);
        line.file = step.file;
        line.line = step.line;
        line.step = index + 1;
        line.from_ms = time;
        if (time && length)
        {
            line.to_ms = *time + *length;
        }
        line.time_source = time ? ScenarioLineText::TimeSource::Text : ScenarioLineText::TimeSource::AfterWait;
        line.after_wait_file = time ? std::string{} : wait_file_;
        line.after_wait = time ? 0 : wait_line_;
        line.text = text;
        if (shape != nullptr && !shape->type.empty())
        {
            line.shape = *shape;
        }
        line_variables_.push_back(variable);
        lines_.push_back(std::move(line));
    };
    for (const ScenarioWrite& w : step.writes)
    {
        placed_.insert(w.variable);
        put(w.variable, 0, w.text, nullptr);
    }
    for (const ScenarioMotion& m : step.motions)
    {
        placed_.insert(m.variable);
        put(m.variable, m.length_ms, m.text, &m.described);
    }
    for (const ScenarioOverlay& o : step.overlays)
    {
        put(o.variable, std::nullopt, o.text, &o.described);
    }
}

double Reader::as_written(double value, std::size_t variable) const
{
    const control::ValueType type = scenario_.variables[variable].type;
    return type == control::ValueType::Integer ? detail::number_of(detail::value_of_type(value, type)) : value;
}

bool Reader::reaches_outside(double value, double reach, std::size_t variable) const
{
    return !in_range(as_written(value - reach, variable), variable) || !in_range(as_written(value + reach, variable), variable);
}

void Reader::check_dithers()
{
    const std::size_t before = findings_.size();
    // Which dither is on which value, by the order of the steps in the file (the steps that
    // run come one after the other, also after one that waits): per input the dither that is
    // on it now, and the value the last step left it at. A value is under the dither that is
    // on when its step comes; a dither that a step puts is on the value the input was left at.
    std::vector<std::vector<Base>> under(dithers_.size());
    std::map<std::size_t, std::size_t> on;                    // per variable: its dither
    std::map<std::size_t, Base> left;                         // per variable: where the last step left it
    std::map<std::size_t, std::size_t> valued;                // per variable: the step that put values on it last
    for (std::size_t i = 0; i < declared_; ++i)
    {
        on[dithers_[i].variable] = i;
    }
    for (const Flow& flow : flow_)
    {
        if (flow.value)
        {
            const Base& base = *flow.value;
            if (const auto at = valued.find(base.variable); at == valued.end() || at->second != flow.step)
            {
                valued[base.variable] = flow.step;
                left.erase(base.variable); // (a step that does not say where it leaves the input: not known)
            }
            if (const auto dither = on.find(base.variable); dither != on.end())
            {
                under[dither->second].push_back(base);
            }
            if (base.stays)
            {
                left.insert_or_assign(base.variable, base);
            }
        }
        else
        {
            const Dither& dither = dithers_[*flow.dither];
            if (dither.off)
            {
                on.erase(dither.variable);
                continue;
            }
            on[dither.variable] = *flow.dither;
            if (const auto base = left.find(dither.variable); base != left.end())
            {
                under[*flow.dither].push_back(base->second);
            }
        }
    }
    for (std::size_t i = 0; i < dithers_.size(); ++i)
    {
        const Dither& dither = dithers_[i];
        if (dither.off)
        {
            continue;
        }
        const ScenarioVariable& var = scenario_.variables[dither.variable];
        const std::string range = range_text(*info_of(dither.variable));
        std::vector<const Base*> outside;
        for (const Base& base : under[i])
        {
            const bool said = std::any_of(outside.begin(), outside.end(),
                                          [&base](const Base* b) { return b->value == base.value && b->file == base.file && b->line == base.line; });
            if (!said && reaches_outside(base.value, dither.reach, dither.variable))
            {
                outside.push_back(&base);
            }
        }
        if (!outside.empty())
        {
            // One finding per dither, with every value it may leave the range on.
            const Base& first = *outside.front();
            std::string others;
            if (outside.size() > 1)
            {
                std::vector<std::string> where;
                for (std::size_t k = 1; k < outside.size(); ++k)
                {
                    const std::string line = short_loc(outside[k]->file, outside[k]->line);
                    if (std::find(where.begin(), where.end(), line) == where.end())
                    {
                        where.push_back(line);
                    }
                }
                others = "; so on " + std::to_string(outside.size() - 1) + " other value" + (outside.size() > 2 ? "s" : "") + " (line" +
                         (where.size() > 1 ? "s " : " ") + config::detail::join(where) + ")";
            }
            add_at(config::Severity::Warning, "scenario-dither-range", dither.file, dither.line,
                   "the dither of " + var.text + " may take it out of its range (" + range + "): on the " + number_text(first.value) +
                       " of " + loc_text(first.file, first.line) + ", the values written go " + (dither.about ? "about " : "") + "from " +
                       number_text(as_written(first.value - dither.reach, dither.variable)) + " to " +
                       number_text(as_written(first.value + dither.reach, dither.variable)) +
                       (dither.about ? " (three times the amplitude of a gaussian noise, which is its standard deviation)" : "") + others +
                       ". This is an estimate from the text (the dither that is on when the step comes, not the times): if it is "
                       "right, the run fails at the write that is outside. Lower the amplitude, keep the values of " + var.text +
                       " further from the ends of its range, or take the dither off before them (dither: { " + var.text + ": none })",
                   owner_of(dither.variable));
        }
        if (var.type == control::ValueType::Integer && dither.swing && *dither.swing < 0.5 && !dither.quiet)
        {
            // What to do about it must not be what the finding above warns of: not "0.5 or
            // more" where a value under the dither is so near an end that 0.5 leaves the range.
            const auto end = std::find_if(under[i].begin(), under[i].end(),
                                          [&](const Base& base) { return reaches_outside(base.value, 0.5, dither.variable); });
            add_at(config::Severity::Warning, "scenario-wave-undersampled", dither.file, dither.line,
                   "the " + dither.shape + " dither of " + var.text + " has an amplitude of " + number_text(*dither.swing) + ", but " +
                       var.text +
                       " is an integer: on a whole value every sum is rounded back to that value, so the ECU sees no dither there (only "
                       "where a ramp or a wave is between two whole values). " +
                       (end == under[i].end() ? std::string("Make the amplitude 0.5 or more")
                                              : "An amplitude of 0.5 or more would show, but on the " + number_text(end->value) + " of " +
                                                    loc_text(end->file, end->line) + " it would leave the range (" + range +
                                                    "): keep that value further from the end of the range first, or remove the dither") +
                       dither.as_it_is,
                   owner_of(dither.variable));
        }
    }
    for (const ScenarioOverlay& input : scenario_.inputs)
    {
        if (placed_.count(input.variable) == 0)
        {
            const std::string& name = scenario_.variables[input.variable].text;
            add_at(config::Severity::Warning, "scenario-dither-unused", input.file, input.line,
                   "the dither of " + name + " is never written: no step that runs puts a value on " + name +
                       " (set, ramp or wave), and a dither is added to what the steps put; add a set of " + name +
                       ", or remove the entry",
                   owner_of(input.variable));
        }
    }
    if (findings_.size() > before)
    {
        // Among the findings of the steps, which are in the order of the file.
        std::stable_sort(findings_.begin(), findings_.end(), [](const config::Finding& x, const config::Finding& y) {
            return x.file != y.file ? x.file < y.file : x.line < y.line;
        });
    }
    // The list of the lines is for a scenario with a dither: another one's output for people is
    // as it was, and its JSON has the lists empty. It has the inputs that a dither is ever put on or taken off: what is under
    // the dithers, not every write of the scenario.
    std::set<std::size_t> listed;
    for (const Dither& dither : dithers_)
    {
        listed.insert(dither.variable);
    }
    if (!scenario_.inputs.empty() || !listed.empty() || inputs_written_)
    {
        scenario_.has_dither = true;
        std::vector<ScenarioLineText> all = scenario_inputs_text(scenario_);
        for (std::size_t i = 0; i < lines_.size(); ++i)
        {
            if (listed.count(line_variables_[i]) > 0)
            {
                all.push_back(std::move(lines_[i]));
            }
        }
        scenario_.lines = std::move(all);
    }
}

void Reader::read_inputs(ryml::id_type node)
{
    if (!tree_.is_map(node))
    {
        error("value", doc_.line(node),
              "inputs must be a mapping of inputs to what is on top of them ({ " + example_variable() + ": { dither: " +
                  kDitherExample + " } }), not " + shown_value(node));
        return;
    }
    std::map<std::string, std::size_t> seen;
    for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
    {
        const std::string name = config::detail::to_string(tree_.key(child));
        if (!first_occurrence(seen, name, child, "inputs: "))
        {
            continue;
        }
        absent_.clear();
        const auto v = variable(child);
        const auto parts = parts_of(child, "inputs: " + name, kInputKeys,
                                    std::string("a mapping with the input's dither ({ dither: ") + kDitherExample + " })");
        if (!parts)
        {
            continue;
        }
        bool quiet = false;
        if (parts->count("undersampled") > 0)
        {
            const ryml::id_type said = parts->at("undersampled");
            quiet = is_scalar(said) && !is_null(said) && text_of(said) == "ok";
            if (!quiet)
            {
                error("value", doc_.line(said),
                      "undersampled must be ok (run the input's dither as it is, also where it changes faster than its ECU "
                      "steps), not " + shown_value(said) + ": write undersampled: ok, or remove it");
            }
        }
        if (parts->count("dither") == 0)
        {
            error("missing-key", doc_.line(child),
                  "inputs: " + name + " has no dither: say what is on top of the input (dither: " + kDitherExample +
                      "), or remove the entry");
            continue;
        }
        if (!v)
        {
            if (!absent_.empty())
            {
                // As a step that uses the variable: not in this configuration, and said.
                add(config::Severity::Note, "scenario-input-skipped", doc_.line(child),
                    "the dither of " + name + " is not used: " + skip_reason() +
                        "; the steps that write " + name + " are skipped too");
            }
            continue;
        }
        as_it_is_ = kAsItIsInput;
        // (Known by the entry's line, also if its dither is on a line below.)
        auto overlay = read_overlay(parts->at("dither"), *v, quiet, false, doc_.line(child));
        as_it_is_ = kAsItIs;
        if (overlay)
        {
            scenario_.inputs.push_back(std::move(*overlay));
        }
    }
    absent_.clear();
}

void Reader::read_dither(ryml::id_type node, ScenarioStep& step, bool quiet)
{
    if (!tree_.is_map(node) || !tree_.has_children(node))
    {
        error("value", doc_.line(node),
              "dither must be a mapping of inputs to a shape or none ({ " + example_variable() + ": " + kDitherExample + " }, or { " +
                  example_variable() + ": none } to take a dither off), not " + shown_value(node));
        return;
    }
    std::map<std::string, std::size_t> seen;
    for (auto child = tree_.first_child(node); child != ryml::NONE; child = tree_.next_sibling(child))
    {
        if (!first_occurrence(seen, config::detail::to_string(tree_.key(child)), child, "dither: "))
        {
            continue;
        }
        if (const auto v = variable(child))
        {
            if (auto overlay = read_overlay(child, *v, quiet, true, step.line))
            {
                step.overlays.push_back(std::move(*overlay));
            }
        }
    }
}

std::optional<ScenarioOverlay> Reader::read_overlay(ryml::id_type node, std::size_t v, bool quiet, bool in_step, std::size_t shown)
{
    const std::size_t line = doc_.line(node);
    const ScenarioVariable& var = scenario_.variables[v];
    const std::string& name = var.text;
    const std::string what = "the dither of " + name;
    bool good = writable(v, line, std::nullopt, "a dither");
    if (!good)
    {
        return std::nullopt; // (what else is wrong with it does not matter: it cannot be written)
    }
    ScenarioOverlay out;
    out.variable = v;
    out.file = current_file_;
    out.line = shown;
    if (var.type == control::ValueType::Bool || var.type == control::ValueType::Enum)
    {
        error("scenario-value", line,
              name + " is " + (var.type == control::ValueType::Bool ? "a bool" : "an enum") +
                  ": a dither needs a number (an integer or a real input); remove it");
        return std::nullopt;
    }
    if (!tree_.is_map(node))
    {
        // none takes it off. (Not "off": a YAML 1.1 tool reads that as false.)
        const bool none = is_scalar(node) && !is_null(node) && param_at(node) == nullptr && text_of(node) == "none";
        if (none && in_step)
        {
            step_dithers_.push_back(Dither{v, current_file_, shown, 0.0, false, std::nullopt, true, true, as_it_is_, {}});
            out.text = name + " none";
            return good ? std::optional<ScenarioOverlay>(std::move(out)) : std::nullopt;
        }
        if (const config::ParamValue* param = param_at(node))
        {
            error("value", line,
                  what + " is the parameter " + param->name + ": a parameter cannot stand for none or for a shape; write " +
                      (in_step ? "none or " : "") + "the shape here (its numbers can be parameters)");
            return std::nullopt;
        }
        error("value", line,
              none ? what + " in inputs is none: an input without a dither needs no entry; remove it"
                   : what + " must be " + (in_step ? "none (which takes the dither off) or " : "") + "a shape (" + kDitherExample +
                         "), not " + shown_value(node) + (in_step ? ": write none, or a shape" : ": write a shape"));
        return std::nullopt;
    }
    // The keys by hand: what a wave has and a dither cannot have gets a sentence of its own.
    Parts parts;
    std::vector<std::string> shapes;
    std::map<std::string, std::size_t> seen;
    for (auto part = tree_.first_child(node); part != ryml::NONE; part = tree_.next_sibling(part))
    {
        const std::string key = config::detail::to_string(tree_.key(part));
        if (!first_occurrence(seen, key, part, "dither of " + name + ": "))
        {
            continue;
        }
        if (std::find(kDitherShapes.begin(), kDitherShapes.end(), key) != kDitherShapes.end())
        {
            parts.emplace(key, part);
            shapes.push_back(key);
            continue;
        }
        good = false;
        if (key == "for_ms")
        {
            error("value", doc_.line(part),
                  what + " has for_ms: a dither has no end, it stays until a dither step changes it; remove for_ms (dither: { " +
                      name + ": none } takes it off)");
        }
        else if (key == "around")
        {
            error("value", doc_.line(part),
                  what + " has around: a dither swings around the value the scenario puts on the input (set, ramp, wave); "
                         "remove around");
        }
        else if (std::find(kWaveShapes.begin(), kWaveShapes.end(), key) != kWaveShapes.end())
        {
            error("value", doc_.line(part),
                  key + " cannot be a dither (it has a length): use sine, triangle, sawtooth, square with amplitude or noise, or "
                        "write it as a wave step");
        }
        else
        {
            unknown_key(part, key, "dither of " + name + ": ", kDitherShapes);
        }
    }
    if (shapes.size() != 1)
    {
        if (good || shapes.size() > 1)
        {
            error("value", line,
                  shapes.empty() ? what + " names no shape: give it one of " + config::detail::join(kDitherShapes)
                                 : what + " has " + config::detail::join(shapes) + ": an input has one dither; keep one of them");
        }
        return std::nullopt;
    }
    const std::string& shape = shapes.front();
    const ryml::id_type body = parts.at(shape);
    if (shape == "square" && tree_.is_map(body))
    {
        for (auto part = tree_.first_child(body); part != ryml::NONE; part = tree_.next_sibling(part))
        {
            const std::string key = config::detail::to_string(tree_.key(part));
            if (key == "low" || key == "high")
            {
                error("value", doc_.line(part),
                      what + " is a square with " + key + ": a dither swings around the value of the input, it has no values of "
                             "its own; give the square amplitude instead (square: { amplitude: 1, period_ms: 100 })");
                return std::nullopt;
            }
        }
    }
    ScenarioMotion read; // what a wave of this shape would be: the parts a dither has too
    std::string text = name + " " + shape;
    std::optional<std::int64_t> length;
    bool wantsFor = true;
    std::string noAround;
    std::optional<double> swing;
    MakeShape make;
    if (!read_shape(shape, body, Parts{}, v, "the " + shape + " dither of " + name, line, std::nullopt, quiet, good, read, text, length,
                    wantsFor, noAround, swing, make) ||
        !good || !make)
    {
        return std::nullopt;
    }
    out.shape = make(SimDuration{});
    out.text = std::move(text);
    out.described = std::move(read.described);
    out.described.type = shape;
    // How far from the value under it the dither goes: its amplitude, or for a gaussian noise
    // (whose amplitude is the standard deviation) three times that, which nearly every value is within.
    Dither noted{v, current_file_, shown, swing.value_or(0.0), !swing.has_value(), swing, quiet, false, as_it_is_, shape};
    if (!swing)
    {
        for (const auto& [key, value] : out.described.values)
        {
            if (key == "amplitude")
            {
                const double* real = std::get_if<double>(&value);
                const std::int64_t* whole = std::get_if<std::int64_t>(&value);
                noted.reach = 3.0 * (real != nullptr ? *real : whole != nullptr ? static_cast<double>(*whole) : 0.0);
            }
        }
    }
    (in_step ? step_dithers_ : dithers_).push_back(noted);
    return out;
}

} // namespace vehicle_sil::scenario::reader
