// SPDX-License-Identifier: MIT
//
// The step of the launcher's controller (plan_time_step()) and the warning about a step that is
// coarser than the ECUs' (coarse_time_step()). See time_plan.hpp.
#include "vehicle_sil/launch/time_plan.hpp"

#include "run_words.hpp"
#include "seen_ecu.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

using detail::kNanosPerMilli;
using detail::millis_text;

std::optional<double> RunTiming::actual_speed() const
{
    const std::int64_t ran = wall_ms - paused_ms;
    if (ran < 1)
    {
        return std::nullopt;
    }
    return std::round(static_cast<double>(sim_ms) / static_cast<double>(ran) * 1000.0) / 1000.0;
}

RunTiming detail::plan_time_step(const SimulationConfig& config, const CheckReport& check, const std::optional<double>& speed,
                                 bool fineStep)
{
    RunTiming t;
    // The synchronised ECUs' steps (an ECU without one runs in real time: the gate does not move it).
    bool known = true;
    std::vector<std::int64_t> steps;
    for (std::size_t i = 0; i < config.participants.size(); ++i)
    {
        // A bridge of --connect runs in real time: the gate does not move it.
        if (!config.participants[i].enabled || !config.participants[i].for_network.empty())
        {
            continue;
        }
        if (i < check.participants.size() && check.participants[i].status == ParticipantStatus::Described &&
            check.participants[i].ecu)
        {
            if (const std::optional<SimDuration> step = SeenEcu(config.participants[i], *check.participants[i].ecu).step())
            {
                steps.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(*step).count());
            }
        }
        else
        {
            known = false;
        }
    }
    SimulationConfig unwritten = config;
    unwritten.time_step_ms.reset();
    const std::int64_t coarsest = time_step_ms(unwritten);
    // The step at which every ECU step starts at a controller step.
    std::int64_t fine = coarsest * kNanosPerMilli;
    for (const std::int64_t step : steps)
    {
        fine = std::gcd(fine, step);
    }
    t.fine_step_ms = known && fine % kNanosPerMilli == 0 ? fine / kNanosPerMilli : 1;
    t.time_step_written = config.time_step_ms.has_value();
    t.time_step_ms = t.time_step_written ? *config.time_step_ms : speed || fineStep ? t.fine_step_ms : coarsest;
    if (known)
    {
        // An ECU step at T runs when the controller's first step at or after T is due.
        const std::int64_t gate = t.time_step_ms * kNanosPerMilli;
        std::int64_t delay = 0;
        for (const std::int64_t step : steps)
        {
            delay = std::max(delay, gate - std::gcd(gate, step));
        }
        t.step_delay_ns = delay;
        if (!steps.empty())
        {
            t.ecu_step_ns = *std::min_element(steps.begin(), steps.end());
        }
    }
    return t;
}

std::optional<Finding> detail::coarse_time_step(const RunTiming& t)
{
    // Only where a speed counts: a run as fast as possible keeps no real time anyway.
    if (!((t.speed || t.paced_steps > 0) && t.step_delay_ns.value_or(0) > 0 && t.fine_step_ms > 0 &&
          t.fine_step_ms < t.time_step_ms))
    {
        return std::nullopt;
    }
    const std::string delay = millis_text(*t.step_delay_ns);
    return Finding{Severity::Warning, "coarse-time-step",
            "the launcher's controller steps every " + std::to_string(t.time_step_ms) + " ms (" +
                (t.time_step_written ? "time_step_ms" : "the default of a run that starts without a speed") + "), but " +
                (t.ecu_step_ns && *t.ecu_step_ns < t.time_step_ms * kNanosPerMilli
                     ? "the ECUs' steps are as small as " + millis_text(*t.ecu_step_ns) + " ms"
                     : std::string("not every ECU step starts at one of its steps")) +
                ": with a speed, the launcher keeps real time only at the controller's steps. An ECU step that starts "
                "between two of them runs at the later one, up to " +
                delay + " ms after the real time it stands for (never before it), together with the other steps in "
                        "between; a device attached in real time gets the messages of those steps up to " +
                delay + " ms late and in a burst. Write time_step_ms: " + std::to_string(t.fine_step_ms) +
                (t.time_step_written ? " (or leave time_step_ms out: with a speed, the default is that)"
                                     : " (or give the speed from the start, with speed or run --speed: the default is then that)"), 0, "", ""};
}

} // namespace vehicle_sil::launch
