// SPDX-License-Identifier: MIT
//
// The step of the launcher's controller (plan_time_step()) and the warning about a step that is
// coarser than the ECUs' (coarse_time_step()); the time lock of a run that is connected outside
// the simulation (time_lock(), planned_speed(), time_lock_findings()). See time_plan.hpp.
#include "vehicle_sil/launch/time_plan.hpp"

#include "vehicle_sil/launch/check.hpp"

#include "check_words.hpp"
#include "seen_ecu.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace vehicle_sil::launch {

using detail::and_list;
using detail::kNanosPerMilli;
using detail::millis_text;

const char* to_string(TimePolicy policy)
{
    return policy == TimePolicy::Free ? "free" : "realtime";
}

std::optional<TimePolicy> time_policy_named(std::string_view name) noexcept
{
    if (name == "realtime")
    {
        return TimePolicy::Realtime;
    }
    if (name == "free")
    {
        return TimePolicy::Free;
    }
    return std::nullopt;
}

std::vector<std::string> TimeLock::because() const
{
    std::vector<std::string> out = connected;
    out.insert(out.end(), bridges.begin(), bridges.end());
    return out;
}

TimeLock time_lock(const SimulationConfig& config, std::span<const ConnectRequest> connect, TimePolicy policy)
{
    TimeLock lock;
    for (const ConnectRequest& request : connect)
    {
        if (request.target != kNoTarget)
        {
            lock.connected.push_back(request.network);
        }
    }
    for (const ParticipantConfig& p : config.participants)
    {
        if (p.enabled && p.role == ParticipantRole::Bridge)
        {
            lock.bridges.push_back(p.name);
        }
    }
    lock.locked = policy == TimePolicy::Realtime && (!lock.connected.empty() || !lock.bridges.empty());
    return lock;
}

std::optional<double> planned_speed(const TimeLock& lock, const std::optional<std::optional<double>>& asked,
                                    const std::optional<double>& otherwise)
{
    if (lock.locked)
    {
        return 1.0;
    }
    return asked ? *asked : otherwise;
}

namespace {

/// "--connect connects CAN1 to a real bus", "participant 'Gateway' is a bridge (role: bridge)", or both.
std::string connected_outside(const TimeLock& lock)
{
    std::string out;
    if (!lock.connected.empty())
    {
        out = "--connect connects " + and_list(lock.connected) + " to a real bus";
    }
    if (!lock.bridges.empty())
    {
        std::vector<std::string> quoted;
        for (const std::string& name : lock.bridges)
        {
            quoted.push_back("'" + name + "'");
        }
        out += out.empty() ? "" : ", and ";
        out += (quoted.size() == 1 ? "participant " + quoted.front() + " is a bridge" : "participants " + and_list(quoted) + " are bridges") +
               std::string(" (role: bridge) to something outside the simulation");
    }
    return out;
}

/// How to run on SIL Kit only: "without --connect", "with --disable Gateway", or both.
std::string not_connected(const TimeLock& lock)
{
    std::string out = lock.connected.empty() ? "" : "without --connect";
    for (const std::string& name : lock.bridges)
    {
        out += (out.empty() ? "with --disable " : " and --disable ") + name;
    }
    return out;
}

constexpr const char* kFreeCarries = "the bridge still carries frames both ways";

} // namespace

std::vector<Finding> time_lock_findings(const TimeLock& lock, const std::optional<std::optional<double>>& asked,
                                        const std::optional<double>& otherwise, bool start_paused)
{
    std::vector<Finding> out;
    if (lock.connected.empty() && lock.bridges.empty())
    {
        return out;
    }
    const std::string what = connected_outside(lock);
    const std::string freely = "give --time-policy free if what is connected there does not mind (" + std::string(kFreeCarries) + ")";
    const std::optional<double> unlocked = asked ? *asked : otherwise;
    const bool realTime = unlocked == std::optional<double>(1.0);
    if (lock.locked)
    {
        if (asked && !realTime)
        {
            out.push_back(Finding{Severity::Error, "outside-real-time",
                                  what + ", which does not wait for the virtual time, so the run goes at speed 1: --speed " +
                                      speed_text(*asked) +
                                      " is refused. Leave --speed out; to go at another speed, run " + not_connected(lock) +
                                      ", or " + freely,
                                  0, "", ""});
        }
        if (!asked && !realTime)
        {
            out.push_back(Finding{Severity::Note, "outside-speed-set",
                                  what + ", which does not wait for the virtual time: the run goes at speed 1 (real time), not " +
                                      (otherwise ? "at the speed " + speed_text(otherwise) + " of the file"
                                                 : std::string("as fast as possible")) +
                                      ". To go at another speed, run " + not_connected(lock) + ", or " + freely,
                                  0, "", ""});
        }
        if (start_paused)
        {
            out.push_back(Finding{Severity::Error, "outside-real-time",
                                  what + ", which does not stand still: --start-paused is refused. To stop the simulation "
                                         "and look at it, run " +
                                      not_connected(lock) + ", or " + freely,
                                  0, "", ""});
        }
        return out;
    }
    if (!realTime)
    {
        std::string runs;
        if (asked)
        {
            runs = unlocked ? "--speed " + speed_text(unlocked) + " runs the simulation at that speed"
                            : std::string("--speed max runs the simulation as fast as possible");
        }
        else
        {
            runs = unlocked ? "the simulation runs at the speed " + speed_text(unlocked) + " of the file"
                            : std::string("the simulation runs as fast as possible");
        }
        out.push_back(Finding{Severity::Note, "outside-not-real-time",
                              what + ", which does not wait for the virtual time, and with --time-policy free " + runs +
                                  ": what is connected there gets the messages at the wrong times, and " + kFreeCarries +
                                  ". To run in real time, leave --time-policy free out",
                              0, "", ""});
    }
    if (start_paused)
    {
        out.push_back(Finding{Severity::Note, "outside-not-real-time",
                              what + ", which does not stand still, and with --time-policy free --start-paused makes the "
                                     "simulation wait before its first step: " +
                                  kFreeCarries +
                                  ", so frames from outside pile up while the simulation stands still. To run in "
                                  "real time, leave --time-policy free and --start-paused out",
                              0, "", ""});
    }
    return out;
}

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
