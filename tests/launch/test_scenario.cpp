// SPDX-License-Identifier: MIT
//
// The scenario file (scenario.hpp: reading and checking, the wording of its errors) and the
// engine that runs a scenario (scenario_run.hpp) against a model of ECUs that steps in virtual
// time: when the writes take effect, that no sample is skipped, the wording of the failures, and
// that the result is the same whenever the samples and results of a step arrive.
#include "launch_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/launch/scenario_run.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;
using vehicle_sil::launch::test::lines;

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

std::int64_t ms_of(SimTime t)
{
    return to_ns(t) / 1'000'000;
}

/// The variables of the model ECU, in the order of its samples.
std::vector<control::VariableInfo> model_variables()
{
    using control::Causality;
    using control::ValueType;
    std::vector<control::VariableInfo> out;
    const auto add = [&out](std::string name, ValueType type, Causality causality) -> control::VariableInfo& {
        control::VariableInfo info;
        info.name = std::move(name);
        info.type = type;
        info.causality = causality;
        out.push_back(std::move(info));
        return out.back();
    };
    auto& level = add("level", ValueType::Integer, Causality::Input);
    level.min = 0.0;
    level.max = 100.0;
    add("gain", ValueType::Integer, Causality::Parameter);
    add("mode", ValueType::Enum, Causality::Input).items = {{"off", 0}, {"on", 1}, {"auto", 2}};
    add("lamp", ValueType::Bool, Causality::Input);
    auto& ratio = add("ratio", ValueType::Real, Causality::Input);
    ratio.min = 0.0;
    ratio.max = 1.0;
    add("level_changed_at_ms", ValueType::Integer, Causality::Output);
    add("gain_at_0", ValueType::Integer, Causality::Output);
    add("pulse", ValueType::Bool, Causality::Output);
    add("body.speed", ValueType::Real, Causality::Output);
    return out;
}

ScenarioCatalog catalog(SimDuration body_step = 10ms, SimDuration door_step = 10ms)
{
    ScenarioCatalog out;
    out.push_back(CatalogParticipant{"Body", "", body_step, model_variables()});
    out.push_back(CatalogParticipant{"Door", "", door_step, model_variables()});
    out.push_back(CatalogParticipant{"Spare", "is left out of this run (enabled: false, or --disable)", SimDuration{}, {}});
    return out;
}

Scenario read(const std::string& text, const ScenarioCatalog& c = catalog())
{
    ScenarioRead r = read_scenario(text, c);
    if (!r.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(r.findings));
    }
    return *r.scenario;
}

const std::string kHead = "vehicle_sil_scenario: 1\nsteps:\n";

/// One ECU of the model: it steps every `step`, samples every 10 ms, applies the commands whose
/// time has come at the start of a step as control::Agent does (a command that arrives after a
/// step at or after its time is late), and has a small model: `speed` moves by up to 50 towards
/// 10 * level per step; `pulse` is true in the step at 200 ms only.
struct ModelEcu
{
    std::string name;
    SimDuration step{10ms};
    std::int64_t level = 0, gain = 0, mode = 0, changed_at = -1, gain_at_0 = -1;
    bool lamp = false, pulse = false;
    double ratio = 0.0, speed = 0.0;
    std::int64_t last_level = 0;
    struct Command
    {
        std::uint64_t ticket;
        SimTime at;
        std::vector<std::pair<std::string, control::Value>> writes;
    };
    std::vector<Command> queue{};
    std::optional<SimTime> last_step{};
    SimTime next_step{};
    std::uint64_t sequence = 0;
};

/// The model of the simulation, and the engine's port to it.
class World : public ScenarioPort
{
public:
    /// When what an ECU's step at the controller's step P produces gets to the engine: before
    /// tick(P) (0), after it (1), or the step itself runs after tick(P) (2). All three happen in a
    /// real run; steps before P are always there.
    using Policy = std::function<int(std::size_t ecu, SimTime p)>;

    SimDuration tick{10ms};
    std::vector<ModelEcu> ecus;
    Policy policy = [](std::size_t, SimTime) { return 0; };
    bool reverse = false; ///< the ECUs step in the reverse order
    std::optional<SimTime> stop_at;
    /// "Body.level=5 for 130 applied at 130", in the order applied.
    std::vector<std::string> log;
    ScenarioEngine* engine = nullptr;
    const Scenario* scenario = nullptr;

    World()
    {
        ecus.resize(2);
        ecus[0].name = "Body";
        ecus[1].name = "Door";
    }

    std::uint64_t write(std::vector<control::PeerWrite> writes, SimTime time) override
    {
        const std::uint64_t ticket = ++tickets_;
        control::OperationResult op;
        op.seq = ticket;
        for (const auto& w : writes)
        {
            auto target = std::find_if(op.targets.begin(), op.targets.end(), [&w](const auto& t) { return t.peer == w.peer; });
            if (target == op.targets.end())
            {
                op.targets.push_back(control::TargetResult{w.peer, control::TargetStatus::Queued, std::nullopt, false, ""});
                target = op.targets.end() - 1;
            }
            const auto ecu = std::find_if(ecus.begin(), ecus.end(), [&w](const ModelEcu& e) { return e.name == w.peer; });
            if (ecu == ecus.end())
            {
                target->status = control::TargetStatus::Invalid;
                target->reason = "unknown peer";
                continue;
            }
            if (w.name == "level" && (std::get<std::int64_t>(w.value) < 0 || std::get<std::int64_t>(w.value) > 100))
            {
                target->status = control::TargetStatus::Invalid;
                target->reason = "level: out of range";
                continue;
            }
            auto command = std::find_if(ecu->queue.begin(), ecu->queue.end(), [ticket](const auto& c) { return c.ticket == ticket; });
            if (command == ecu->queue.end())
            {
                ecu->queue.push_back(ModelEcu::Command{ticket, time, {}});
                command = ecu->queue.end() - 1;
            }
            command->writes.emplace_back(w.name, w.value);
        }
        visible_[ticket] = op;
        return ticket;
    }

    std::optional<control::OperationResult> result(std::uint64_t ticket) override
    {
        const auto it = visible_.find(ticket);
        return it == visible_.end() ? std::nullopt : std::optional<control::OperationResult>(it->second);
    }

    /// Runs the scenario to its end (or `limit`).
    ScenarioRun run(const Scenario& s, SimDuration limit = 5s)
    {
        scenario = &s;
        ScenarioEngine e(s, tick, *this);
        engine = &e;
        std::vector<EnginePeer> peers;
        for (const auto& p : s.participants)
        {
            EnginePeer peer;
            peer.connected = std::any_of(ecus.begin(), ecus.end(), [&p](const ModelEcu& m) { return m.name == p.name; });
            peer.schema = model_variables();
            peers.push_back(std::move(peer));
        }
        e.begin(peers);
        for (SimTime p{}; p <= at(limit); p += tick)
        {
            // Everything of the steps before p is there, in any order.
            run_steps([p](const ModelEcu& m) { return m.next_step < p; });
            deliver([p](SimTime t) { return t < p; });
            // The steps at p itself: as the policy says.
            for (std::size_t i = 0; i < ecus.size(); ++i)
            {
                const std::size_t index = reverse ? ecus.size() - 1 - i : i;
                if (ecus[index].next_step == p && policy(index, p) != 2)
                {
                    const bool now = policy(index, p) == 0;
                    step(index);
                    if (now)
                    {
                        deliver([p](SimTime t) { return t <= p; });
                    }
                }
            }
            if (stop_at && p >= *stop_at)
            {
                e.stopped_at(p);
                break;
            }
            if (e.tick(p))
            {
                break;
            }
        }
        if (!e.over())
        {
            e.cut_short();
        }
        engine = nullptr;
        return e.run();
    }

private:
    struct Produced
    {
        SimTime time;
        std::size_t ecu;
        std::shared_ptr<const control::Snapshot> sample;
        std::vector<std::pair<std::uint64_t, control::TargetResult>> results;
    };

    void run_steps(const std::function<bool(const ModelEcu&)>& due)
    {
        for (bool any = true; any;)
        {
            any = false;
            for (std::size_t i = 0; i < ecus.size(); ++i)
            {
                const std::size_t index = reverse ? ecus.size() - 1 - i : i;
                if (due(ecus[index]))
                {
                    step(index);
                    any = true;
                }
            }
        }
    }

    void step(std::size_t index)
    {
        ModelEcu& m = ecus[index];
        const SimTime now = m.next_step;
        Produced out{now, index, nullptr, {}};
        std::stable_sort(m.queue.begin(), m.queue.end(), [](const auto& a, const auto& b) {
            return a.at != b.at ? a.at < b.at : a.ticket < b.ticket;
        });
        for (auto it = m.queue.begin(); it != m.queue.end();)
        {
            if (it->at > now)
            {
                ++it;
                continue;
            }
            control::TargetResult r{m.name, control::TargetStatus::Applied, now, false, ""};
            if (m.last_step && *m.last_step >= it->at)
            {
                r = control::TargetResult{m.name, control::TargetStatus::Late, std::nullopt, true, "its time had passed"};
            }
            else
            {
                for (const auto& [name, value] : it->writes)
                {
                    if (name == "gain" && m.last_step)
                    {
                        r = control::TargetResult{m.name, control::TargetStatus::Rejected, std::nullopt, false,
                                                  "a parameter is writable only before the first step"};
                        break;
                    }
                    if (name == "level" && std::get<std::int64_t>(value) == 13)
                    {
                        r = control::TargetResult{m.name, control::TargetStatus::Rejected, std::nullopt, false,
                                                  "a setter threw: 13 is not a level"};
                        break;
                    }
                    if (name == "level")
                    {
                        m.level = std::get<std::int64_t>(value);
                    }
                    else if (name == "gain")
                    {
                        m.gain = std::get<std::int64_t>(value);
                    }
                    else if (name == "mode")
                    {
                        m.mode = std::get<std::int64_t>(value);
                    }
                    else if (name == "lamp")
                    {
                        m.lamp = std::get<bool>(value);
                    }
                    else if (name == "ratio")
                    {
                        m.ratio = std::get<double>(value);
                    }
                    log.push_back(m.name + "." + name + "=" + value_text(value) + " for " + std::to_string(ms_of(it->at)) +
                                  " applied at " + std::to_string(ms_of(now)));
                }
            }
            if (r.status == control::TargetStatus::Late)
            {
                log.push_back(m.name + " LATE for " + std::to_string(ms_of(it->at)) + " at " + std::to_string(ms_of(now)));
            }
            out.results.emplace_back(it->ticket, r);
            it = m.queue.erase(it);
        }
        // The model.
        if (!m.last_step)
        {
            m.gain_at_0 = m.gain;
        }
        if (m.level != m.last_level)
        {
            m.last_level = m.level;
            m.changed_at = ms_of(now);
        }
        m.pulse = now == at(200ms);
        const double target = 10.0 * static_cast<double>(m.level);
        m.speed += std::clamp(target - m.speed, -50.0, 50.0);
        if (to_ns(now) % SimDuration(10ms).count() == 0)
        {
            control::Snapshot sample;
            sample.time = now;
            sample.sequence = ++m.sequence;
            sample.values = {control::Value{m.level},      control::Value{m.gain},      control::Value{m.mode},
                             control::Value{m.lamp},       control::Value{m.ratio},     control::Value{m.changed_at},
                             control::Value{m.gain_at_0},  control::Value{m.pulse},     control::Value{m.speed}};
            out.sample = std::make_shared<const control::Snapshot>(std::move(sample));
        }
        m.last_step = now;
        m.next_step = now + m.step;
        produced_.push_back(std::move(out));
    }

    void deliver(const std::function<bool(SimTime)>& which)
    {
        for (auto it = produced_.begin(); it != produced_.end();)
        {
            if (!which(it->time))
            {
                ++it;
                continue;
            }
            for (std::size_t i = 0; i < scenario->participants.size(); ++i)
            {
                if (scenario->participants[i].name == ecus[it->ecu].name && it->sample)
                {
                    engine->sample(i, it->sample);
                }
            }
            for (const auto& [ticket, result] : it->results)
            {
                for (auto& target : visible_[ticket].targets)
                {
                    if (target.peer == result.peer)
                    {
                        target = result;
                    }
                }
            }
            it = produced_.erase(it);
        }
    }

    std::uint64_t tickets_ = 0;
    std::map<std::uint64_t, control::OperationResult> visible_;
    std::vector<Produced> produced_;
};

std::string time_text(const std::optional<SimTime>& t)
{
    return t ? std::to_string(ms_of(*t)) : "-";
}

/// One line per step: "<line> <status> start=<ms> at=<ms> end=<ms>[ <code>: <message>]".
std::string text_of(const ScenarioRun& run)
{
    std::string out = std::string(scenario_status_name(run.status)) + " over=" + time_text(run.over_at) + "\n";
    for (const auto& s : run.steps)
    {
        out += std::to_string(s.line) + " " + step_status_name(s.status) + " start=" + time_text(s.start) + " at=" + time_text(s.at) +
               " end=" + time_text(s.end) + (s.code.empty() ? "" : " " + s.code + ": " + s.message) + "\n";
    }
    return out;
}

std::string joined(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += item + "\n";
    }
    return out;
}

/// The ways the outputs of the steps at a controller step can reach the engine.
std::vector<std::pair<std::string, World::Policy>> policies()
{
    std::vector<std::pair<std::string, World::Policy>> out;
    out.emplace_back("early", [](std::size_t, SimTime) { return 0; });
    out.emplace_back("after the tick", [](std::size_t, SimTime) { return 1; });
    out.emplace_back("step after the tick", [](std::size_t, SimTime) { return 2; });
    for (const std::uint64_t seed : {1u, 2u, 3u, 4u, 5u})
    {
        out.emplace_back("mixed " + std::to_string(seed), [seed](std::size_t ecu, SimTime p) {
            // A fixed scramble of the ECU, the time and the seed.
            std::uint64_t x = (static_cast<std::uint64_t>(to_ns(p)) / 1'000'000 + 1) * 0x9E3779B97F4A7C15ull + ecu * 0xBF58476D1CE4E5B9ull + seed;
            x ^= x >> 29;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 32;
            return static_cast<int>(x % 3);
        });
    }
    return out;
}

} // namespace

TEST_CASE(scenario_file_is_read_with_its_steps_and_what_each_does)
{
    const Scenario s = read("vehicle_sil_scenario: 1\n"
                            "name: all the steps\n"
                            "steps:\n"
                            "  - at_ms: 0\n"
                            "    set: { Body.level: 5, Body.gain: 3, Door.mode: auto, Door.lamp: true }\n"
                            "  - at_ms: 100\n"
                            "    ramp: { Body.ratio: { to: 0.5, over_ms: 40 }, Body.level: { from: 10, to: 20, over_ms: 100 } }\n"
                            "  - wait_until: { Body.body.speed: { ge: 150.5 }, Door.mode: auto }\n"
                            "    timeout_ms: 2000\n"
                            "  - after_ms: 20\n"
                            "    expect: { Body.level: 20, Door.mode: { ne: 0 }, Body.body.speed: { near: 200, tolerance: 0.5 } }\n"
                            "  - expect: { Door.lamp: true }\n"
                            "    within_ms: 50\n"
                            "  - expect: { Body.level: { gt: 1, lt: 30 } }\n"
                            "    hold_for_ms: 30\n");
    CHECK(s.name == "all the steps");
    CHECK_EQ(s.steps.size(), std::size_t{6});
    CHECK_EQ(s.participants.size(), std::size_t{2});
    CHECK(s.participants[0].name == "Body" && s.participants[1].name == "Door");
    std::string texts;
    for (const auto& step : s.steps)
    {
        texts += std::to_string(step.line) + ": " + step.text + "\n";
    }
    check_text(texts, "4: set Body.level = 5; Body.gain = 3; Door.mode = auto (2); Door.lamp = true\n"
                      "6: ramp Body.ratio to 0.5 over 40 ms; Body.level from 10 to 20 over 100 ms\n"
                      "8: wait_until Body.body.speed ge 150.5; Door.mode eq auto (2) (timeout 2000 ms)\n"
                      "10: expect Body.level eq 20; Door.mode ne off (0); Body.body.speed near 200 (tolerance 0.5)\n"
                      "12: expect Door.lamp eq true (within 50 ms)\n"
                      "14: expect Body.level gt 1, lt 30 (for 30 ms)\n");
    CHECK(s.steps[2].waits() && s.steps[4].waits() && !s.steps[5].waits());
    // JSON is read as well.
    const Scenario json = read(R"({"vehicle_sil_scenario": 1, "steps": [{"set": {"Body.level": 1}}]})");
    CHECK(json.steps.at(0).text == "set Body.level = 1");
}

TEST_CASE(scenario_file_errors_say_the_line_what_is_wrong_and_how_to_fix_it)
{
    const auto errors = [](const std::string& text) { return lines(read_scenario(text, catalog()).findings); };
    // The form.
    check_text(errors(""), "E 0 not-a-scenario: the file is empty: write vehicle_sil_scenario: 1 and steps (docs/scenario.md)\n");
    check_text(errors("vehicle_sil_scenario: 2\nsteps: []\n"),
               "E 1 format-version: vehicle_sil_scenario is 2, but this vehicle_sil_launch reads format 1 only: use the "
               "vehicle-sil that the file was written for\n");
    check_text(errors("steps:\n  - set: { Body.level: 1 }\nstep: 1\n"),
               "E 3 unknown-key: unknown key 'step': did you mean 'steps'? (the keys here: vehicle_sil_scenario, name, steps)\n"
               "E 0 missing-key: the file has no vehicle_sil_scenario (the format version): add vehicle_sil_scenario: 1\n");
    check_text(errors(kHead + "  - sets: { Body.level: 1 }\n"),
               "E 3 unknown-key: unknown key 'sets': did you mean 'set'? (the keys here: at_ms, after_ms, set, ramp, wait_until, "
               "timeout_ms, expect, within_ms, hold_for_ms)\n"
               "E 3 value: the step does nothing: give it one of set, ramp, wait_until, expect\n");
    check_text(errors(kHead + "  - set: { Body.level: 1 }\n    expect: { Body.level: 1 }\n"),
               "E 3 value: the step has set, expect: a step does one thing; make a step of each\n");
    // The variables.
    check_text(errors(kHead + "  - set: { Bdy.level: 1 }\n"),
               "E 3 scenario-variable: 'Bdy.level': the configuration has no participant 'Bdy' (the name ends at the first "
               "'.'): did you mean 'Body.level'?\n");
    check_text(errors(kHead + "  - expect: { Body.body.sped: 1 }\n"),
               "E 3 scenario-variable: 'Body.body.sped': participant 'Body' has no variable 'body.sped': did you mean "
               "'Body.body.speed'? (vehicle_sil_launch check --json lists the variables)\n");
    check_text(errors(kHead + "  - expect: { Spare.level: 1 }\n"),
               "E 3 scenario-variable: 'Spare.level': participant 'Spare' is left out of this run (enabled: false, or --disable)\n");
    // What can be written, and when.
    check_text(errors(kHead + "  - set: { Body.pulse: true }\n"),
               "E 3 scenario-not-writable: Body.pulse is an output of its ECU: it can be read (expect, wait_until), not written; "
               "write one of the ECU's inputs instead\n");
    check_text(errors(kHead + "  - at_ms: 10\n    set: { Body.gain: 2 }\n"),
               "E 4 scenario-not-writable: Body.gain is a parameter: it can be written only before the first step, that is with "
               "set in a step at 0 (at_ms: 0, or the first steps without a time), not at 10 ms\n");
    // The values.
    check_text(errors(kHead + "  - set: { Body.level: 101 }\n"),
               "E 3 scenario-value: the value of Body.level (integer) is 101, outside its range (from 0 to 100)\n");
    check_text(errors(kHead + "  - set: { Body.level: 1.5, Door.lamp: 1, Door.mode: of }\n"),
               "E 3 scenario-value: the value of Body.level (integer) must be a whole number, not '1.5'\n"
               "E 3 scenario-value: the value of Door.lamp (bool) must be true or false, not '1'\n"
               "E 3 scenario-value: the value of Door.mode (enum) must be one of its items, by name or by value, not 'of': did "
               "you mean 'off'? the items are off (0), on (1), auto (2)\n");
    check_text(errors(kHead + "  - ramp: { Door.lamp: { to: 1, over_ms: 10 } }\n"),
               "E 3 scenario-value: ramp needs a number, and Door.lamp is a bool: use set\n");
    check_text(errors(kHead + "  - expect: { Door.mode: { ge: 1 }, Body.level: { near: 5 } }\n"),
               "E 3 scenario-value: ge compares numbers, and Door.mode is an enum: use eq or ne\n"
               "E 3 value: Body.level: near needs tolerance (how far from the value it may be): add tolerance: <number>\n");
    // The times.
    check_text(errors(kHead + "  - wait_until: { Body.level: 1 }\n"),
               "E 3 missing-key: wait_until needs timeout_ms: how long to wait at most, in milliseconds of virtual time (when it "
               "passes, the step fails)\n");
    check_text(errors(kHead + "  - at_ms: 100\n    set: { Body.level: 1 }\n  - at_ms: 50\n    set: { Body.level: 2 }\n"),
               "E 5 scenario-time: at_ms is 50, before the end of the step before it (100 ms): the steps run in the order of the "
               "file, and time does not go back; reorder the steps or raise at_ms\n");
    check_text(errors(kHead + "  - expect: { Body.level: 1 }\n    within_ms: 10\n  - at_ms: 500\n    set: { Body.level: 2 }\n"),
               "E 5 scenario-time: at_ms after a step that waits (line 3): when that step ends is not known before the run; give "
               "this step after_ms (from the end of the step before it) instead\n");
    check_text(errors(kHead + "  - at_ms: 5\n    after_ms: 5\n    set: { Body.level: 2 }\n    timeout_ms: 3\n"),
               "E 6 value: timeout_ms goes with wait_until, not with set: remove it\n"
               "E 4 value: the step has at_ms and after_ms: give it one time, at_ms (the virtual time) or after_ms (from the end "
               "of the step before it)\n");
    // A ramp ends where it says: the step after it may not be earlier.
    check_text(errors(kHead + "  - ramp: { Body.level: { to: 5, over_ms: 100 } }\n  - at_ms: 99\n    expect: { Body.level: 5 }\n"),
               "E 4 scenario-time: at_ms is 99, before the end of the step before it (100 ms): the steps run in the order of the "
               "file, and time does not go back; reorder the steps or raise at_ms\n");
    // The times added up stay within the longest time (said once).
    check_text(errors(kHead + "  - after_ms: 600000000000\n    set: { Body.level: 1 }\n"
                              "  - wait_until: { Body.level: 2 }\n    timeout_ms: 300000000000\n"
                              "  - after_ms: 200000000000\n    set: { Body.level: 3 }\n"
                              "  - after_ms: 200000000000\n    set: { Body.level: 4 }\n"),
               "E 7 scenario-time: the scenario can last 1100000000000 ms up to the end of this step, more than the longest time "
               "(1000000000000 ms): shorten the times (after_ms, over_ms, hold_for_ms, the timeouts) of the steps up to here\n");
    // A syntax error has its line.
    const auto syntax = read_scenario("vehicle_sil_scenario: 1\nsteps: [\n", catalog()).findings;
    CHECK_EQ(syntax.size(), std::size_t{1});
    CHECK(syntax.at(0).code == "syntax");
}

TEST_CASE(scenario_writes_take_effect_in_the_step_of_their_time_however_the_steps_interleave)
{
    // Values for 0 (a parameter among them), for a time on the ECUs' grid, for a time between two
    // steps (the next step takes it), for a time that is no step of the controller, and right
    // after a wait. Door steps every 4 ms, the controller every 10 ms.
    const std::string file = kHead + "  - set: { Body.level: 5, Body.gain: 3, Door.level: 1 }\n"
                                     "  - at_ms: 130\n"
                                     "    set: { Body.level: 7 }\n"
                                     "  - at_ms: 135\n"
                                     "    set: { Body.level: 8, Door.level: 2 }\n"
                                     "  - at_ms: 150\n"
                                     "    set: { Body.level: 9 }\n"
                                     "  - at_ms: 150\n"
                                     "    set: { Body.level: 10 }\n"
                                     "  - wait_until: { Body.pulse: true }\n"
                                     "    timeout_ms: 100\n"
                                     "  - set: { Body.level: 11 }\n";
    const Scenario s = read(file, catalog(10ms, 4ms));
    const std::string expected = "Body.level=5 for 0 applied at 0\n"
                                 "Body.gain=3 for 0 applied at 0\n"
                                 "Door.level=1 for 0 applied at 0\n"
                                 "Body.level=7 for 130 applied at 130\n"
                                 "Door.level=2 for 135 applied at 136\n"
                                 "Body.level=8 for 135 applied at 140\n"
                                 "Body.level=9 for 150 applied at 150\n"
                                 "Body.level=10 for 150 applied at 150\n"
                                 // The pulse is in the sample of 200 ms: known at 210, the next step is at 220.
                                 "Body.level=11 for 220 applied at 220\n";
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            world.ecus[1].step = 4ms;
            world.policy = policy;
            world.reverse = reverse;
            const ScenarioRun run = world.run(s);
            if (run.status != ScenarioStatus::Passed)
            {
                throw unit::Failure(name + ": " + text_of(run) + joined(world.log));
            }
            // Per ECU the order is fixed; between the ECUs it is the order of the model's steps.
            std::vector<std::string> got = world.log;
            std::vector<std::string> want;
            for (std::size_t from = 0; from < expected.size();)
            {
                const auto to = expected.find('\n', from);
                want.push_back(expected.substr(from, to - from));
                from = to + 1;
            }
            const auto per_ecu = [](std::vector<std::string> all, const std::string& ecu) {
                all.erase(std::remove_if(all.begin(), all.end(), [&ecu](const std::string& l) { return l.rfind(ecu, 0) != 0; }), all.end());
                return joined(all);
            };
            check_text(per_ecu(got, "Body"), per_ecu(want, "Body"), name.c_str());
            check_text(per_ecu(got, "Door"), per_ecu(want, "Door"), name.c_str());
            CHECK_EQ(world.ecus[0].gain_at_0, std::int64_t{3});
        }
    }
}

TEST_CASE(scenario_result_is_the_same_whenever_the_samples_and_results_of_a_step_arrive)
{
    const std::string file = kHead + "  - set: { Body.level: 20, Door.mode: on }\n"                      // 3
                                     "  - wait_until: { Body.body.speed: { ge: 200 } }\n"                // 4
                                     "    timeout_ms: 500\n"
                                     "  - expect: { Body.body.speed: 200, Door.mode: on }\n"             // 6
                                     "    hold_for_ms: 40\n"
                                     "  - ramp: { Body.level: { to: 0, over_ms: 50 }, Door.ratio: { from: 1, to: 0, over_ms: 30 } }\n" // 8
                                     "  - expect: { Body.level: 0 }\n"                                   // 9
                                     "  - after_ms: 10\n"                                                // 10
                                     "    expect: { Body.body.speed: { le: 100 } }\n"
                                     "    within_ms: 300\n"
                                     "  - expect: { Door.ratio: { near: 0, tolerance: 0.001 }, Body.level_changed_at_ms: 140 }\n" // 13
                                     "  - after_ms: 5\n"                                                 // 14
                                     "    set: { Door.lamp: true }\n"
                                     "  - after_ms: 15\n"                                                // 16
                                     "    expect: { Door.lamp: true }\n";
    // speed: +50 per 10 ms step from the step at 0: 200 in the sample of 30 ms. Known at 40, so the
    // wait ends at 50; the hold is over [50, 90]; the ramp from 90 to 140 (level 0 written for 140).
    // The speed follows the level down at once (it is 0 in the sample of 140 ms), so the wait from
    // 150 is true at 150: known at 160, it ends at 170. The set for 175 is applied by the step at 180.
    const std::string expected = "passed over=200\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 passed start=0 at=30 end=50\n"
                                 "6 passed start=50 at=90 end=90\n"
                                 "8 passed start=90 at=140 end=140\n"
                                 "9 passed start=140 at=140 end=140\n"
                                 "10 passed start=150 at=150 end=170\n"
                                 "13 passed start=170 at=170 end=170\n"
                                 "14 passed start=175 at=180 end=175\n"
                                 "16 passed start=190 at=190 end=190\n";
    const Scenario s = read(file);
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            world.policy = policy;
            world.reverse = reverse;
            check_text(text_of(world.run(s)), expected, name.c_str());
            CHECK(std::none_of(world.log.begin(), world.log.end(), [](const std::string& l) { return l.find("LATE") != std::string::npos; }));
        }
    }
}

TEST_CASE(scenario_conditions_look_at_every_sample)
{
    // `pulse` is true in one sample only (200 ms). A wait sees it; a hold over it is broken by it,
    // at that sample; an expect at 205 ms sees the latest sample at or before it (the one of 200 ms).
    for (const auto& [name, policy] : policies())
    {
        World wait;
        wait.policy = policy;
        check_text(text_of(wait.run(read(kHead + "  - wait_until: { Body.pulse: true }\n    timeout_ms: 1000\n"
                                                 "  - expect: { Body.pulse: false }\n"))),
                   "passed over=230\n"
                   "3 passed start=0 at=200 end=220\n"
                   "5 passed start=220 at=220 end=220\n",
                   name.c_str());
        World hold;
        hold.policy = policy;
        check_text(text_of(hold.run(read(kHead + "  - at_ms: 150\n    expect: { Body.pulse: false }\n    hold_for_ms: 100\n"))),
                   "failed over=210\n"
                   "3 failed start=150 at=200 end=250 scenario-failed: not true for 100 ms (from 150 ms): at 200 ms Body.pulse eq "
                   "false, but it was true (its sample of 200 ms)\n",
                   name.c_str());
        World between;
        between.policy = policy;
        check_text(text_of(between.run(read(kHead + "  - at_ms: 205\n    expect: { Body.pulse: true }\n"
                                                    "  - at_ms: 210\n    expect: { Body.pulse: false }\n"))),
                   "passed over=220\n"
                   "3 passed start=205 at=205 end=205\n"
                   "5 passed start=210 at=210 end=210\n",
                   name.c_str());
    }
}

TEST_CASE(scenario_samples_of_one_tick_are_looked_at_in_the_order_of_their_times)
{
    // The controller steps every 20 ms, the ECUs sample every 10 ms: two samples of each ECU per
    // tick, and conditions that begin and end between them. Both ECUs have the pulse in their
    // sample of 200 ms only: a condition on both is true there, whichever sample came first.
    const Scenario s = read(kHead + "  - at_ms: 150\n"
                                    "    expect: { Body.pulse: false }\n"
                                    "    hold_for_ms: 40\n"
                                    "  - wait_until: { Body.pulse: true, Door.pulse: true }\n"
                                    "    timeout_ms: 15\n"
                                    "  - expect: { Door.pulse: false, Body.pulse: false }\n"
                                    "    within_ms: 100\n"
                                    "  - after_ms: 21\n"
                                    "    set: { Body.level: 2 }\n"
                                    "  - after_ms: 18\n"
                                    "    expect: { Body.level_changed_at_ms: 310, Body.level: 2 }\n",
                            catalog(10ms, 4ms));
    // The hold ends at 190; the wait from 190 is true at 200 (known at 220, it ends at 240); the
    // pulse is gone in the sample of 240 (known at 260, ends at 280).
    const std::string expected = "passed over=320\n"
                                 "3 passed start=150 at=190 end=190\n"
                                 "6 passed start=190 at=200 end=240\n"
                                 "8 passed start=240 at=240 end=280\n"
                                 "10 passed start=301 at=310 end=301\n"
                                 "12 passed start=319 at=319 end=319\n";
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            world.tick = 20ms;
            world.ecus[1].step = 4ms;
            world.policy = policy;
            world.reverse = reverse;
            check_text(text_of(world.run(s)), expected, name.c_str());
        }
    }
    // The same wait misses the pulse by one sample: it runs out at 195, between two samples of a tick.
    World late;
    late.tick = 20ms;
    check_text(text_of(late.run(read(kHead + "  - at_ms: 180\n    wait_until: { Body.pulse: true, Door.pulse: true }\n    timeout_ms: 15\n"))),
               "failed over=200\n"
               "3 failed start=180 at=195 end=- scenario-timeout: timed out at 195 ms (15 ms after 180 ms): Body.pulse eq true, but "
               "it was false (its sample of 190 ms); Door.pulse eq true, but it was false (its sample of 190 ms)\n");
}

TEST_CASE(scenario_failures_say_the_time_and_the_values)
{
    const auto run = [](const std::string& steps, const std::function<void(World&)>& change = {}) {
        World world;
        if (change)
        {
            change(world);
        }
        return text_of(world.run(read(kHead + steps)));
    };
    // A wait that runs out: when, and what the values were then.
    check_text(run("  - set: { Body.level: 20 }\n"
                   "  - wait_until: { Body.body.speed: { ge: 250 }, Body.level: 20 }\n    timeout_ms: 100\n"
                   "  - set: { Body.level: 0 }\n"),
               "failed over=110\n"
               "3 passed start=0 at=0 end=0\n"
               "4 failed start=0 at=100 end=- scenario-timeout: timed out at 100 ms (100 ms after 0 ms): Body.body.speed ge 250, "
               "but it was 200 (its sample of 100 ms)\n"
               "6 not_run start=- at=- end=-\n");
    // An expectation at one time, and one within a time.
    check_text(run("  - at_ms: 20\n    expect: { Body.level: 1, Body.body.speed: { lt: 10 } }\n"),
               "failed over=30\n"
               "3 failed start=20 at=20 end=20 scenario-failed: not true at 20 ms: Body.level eq 1, but it was 0 (its sample of 20 ms)\n");
    check_text(run("  - expect: { Door.mode: on }\n    within_ms: 25\n"),
               "failed over=30\n"
               "3 failed start=0 at=25 end=- scenario-failed: not true at any time within 25 ms (from 0 ms to 25 ms): at the end "
               "Door.mode eq on (1), but it was off (0) (its sample of 20 ms)\n");
    // Before the first sample there is no value.
    check_text(run("  - expect: { Body.level: 0 }\n    within_ms: 5\n", [](World& w) { w.ecus[0].step = 20ms; w.tick = 5ms; }),
               "passed over=5\n"
               "3 passed start=0 at=0 end=10\n");
    // A write the ECU rejects in its step: said at the controller's step after it, before the
    // expectations of that time are looked at.
    check_text(run("  - at_ms: 50\n    set: { Body.level: 13 }\n  - expect: { Body.level: 13 }\n"),
               "failed over=60\n"
               "3 failed start=50 at=50 end=50 scenario-write: the write for 50 ms was not applied by participant 'Body': the ECU "
               "rejected it (a setter threw: 13 is not a level)\n"
               "5 not_run start=- at=- end=-\n");
    // A participant without a link: the first step that uses it says so, before anything runs.
    check_text(run("  - at_ms: 50\n    set: { Body.level: 1 }\n  - expect: { Door.lamp: false }\n",
                   [](World& w) { w.ecus.pop_back(); }),
               "failed over=0\n"
               "3 not_run start=- at=- end=-\n"
               "5 failed start=50 at=50 end=- scenario-failed: Door.lamp cannot be read: participant 'Door' has no control link in "
               "this run: its agent did not connect before the simulation started (the run's control-link findings and its "
               "lines say why)\n");
    // The simulation stops by itself first: what could be decided is, the next step is out of time.
    check_text(run("  - at_ms: 80\n    set: { Body.level: 1 }\n  - at_ms: 90\n    expect: { Body.level: 1 }\n"
                   "  - at_ms: 100\n    expect: { Body.level: 1 }\n  - at_ms: 150\n    set: { Body.level: 2 }\n",
                   [](World& w) { w.stop_at = at(100ms); }),
               "failed over=100\n"
               "3 passed start=80 at=80 end=80\n"
               "5 passed start=90 at=90 end=90\n"
               "7 failed start=100 at=100 end=100 scenario-out-of-time: the simulation stopped at 100 ms (stop_at_ms) before this "
               "step could be decided (its time is 100 ms): there was not enough time for the scenario; raise stop_at_ms, or "
               "leave it out (the run then ends with the scenario)\n"
               "9 not_run start=- at=- end=-\n");
    // It stops while the result of a write is not due yet: that step is out of time.
    check_text(run("  - at_ms: 95\n    set: { Body.level: 1 }\n", [](World& w) { w.stop_at = at(100ms); }),
               "failed over=100\n"
               "3 failed start=95 at=100 end=95 scenario-out-of-time: the simulation stopped at 100 ms (stop_at_ms) before this "
               "step was decided (it began at 95 ms): there was not enough time for the scenario; raise stop_at_ms, or leave it "
               "out (the run then ends with the scenario)\n");
    // A participant failed and ended the run: a step that failed (of that) is not the result.
    {
        World world;
        world.ecus.pop_back();
        const Scenario s = read(kHead + "  - set: { Body.level: 1 }\n  - at_ms: 50\n    set: { Body.level: 13 }\n");
        ScenarioEngine engine(s, 10ms, world);
        engine.begin({EnginePeer{true, model_variables()}});
        // Body goes after the first tick: the result of the write for 0 never comes.
        CHECK(!engine.tick(at(0ms)));
        world.ecus.clear();
        for (SimTime p = at(10ms); !engine.tick(p); p += 10ms)
        {
        }
        CHECK(engine.run().status == ScenarioStatus::Failed);
        engine.superseded();
        CHECK(engine.over());
        check_text(text_of(engine.run()), "incomplete over=-\n"
                                          "3 unfinished start=0 at=- end=-\n"
                                          "4 not_run start=- at=- end=-\n");
    }
    // The simulation ends for another reason: incomplete, and how far it got.
    {
        World world;
        const Scenario s = read(kHead + "  - set: { Body.level: 1 }\n  - wait_until: { Body.level: 2 }\n    timeout_ms: 9000\n");
        check_text(text_of(world.run(s, 300ms)), "incomplete over=-\n"
                                                 "3 passed start=0 at=0 end=0\n"
                                                 "4 unfinished start=0 at=- end=-\n");
    }
}

TEST_CASE(scenario_ramp_writes_a_line_in_the_steps_of_its_ecu)
{
    // From what the scenario wrote last (1, in the step before it for the same time: no sample
    // has it yet) to 0 over 16 ms, in Door's 4 ms steps; and an integer from a given value,
    // rounded, whose end (125 ms) is between two steps of its ECU: the step at 130 ms takes it.
    const Scenario s = read(kHead + "  - set: { Door.ratio: 0.5 }\n"
                                    "  - at_ms: 100\n"
                                    "    set: { Door.ratio: 1 }\n"
                                    "  - at_ms: 100\n"
                                    "    ramp: { Door.ratio: { to: 0, over_ms: 16 }, Body.level: { from: 1, to: 4, over_ms: 25 } }\n"
                                    "  - after_ms: 5\n"
                                    "    expect: { Door.ratio: 0, Body.level: 4 }\n",
                            catalog(10ms, 4ms));
    World world;
    world.ecus[1].step = 4ms;
    const ScenarioRun run = world.run(s);
    check_text(text_of(run), "passed over=140\n"
                             "3 passed start=0 at=0 end=0\n"
                             "4 passed start=100 at=100 end=100\n"
                             "6 passed start=100 at=125 end=125\n"
                             "8 passed start=130 at=130 end=130\n");
    check_text(joined(world.log), "Door.ratio=0.5 for 0 applied at 0\n"
                                  "Body.level=1 for 100 applied at 100\n"
                                  "Door.ratio=1 for 100 applied at 100\n"
                                  "Door.ratio=1 for 100 applied at 100\n"
                                  "Door.ratio=0.75 for 104 applied at 104\n"
                                  "Door.ratio=0.5 for 108 applied at 108\n"
                                  "Body.level=2 for 110 applied at 110\n"
                                  "Door.ratio=0.25 for 112 applied at 112\n"
                                  "Door.ratio=0 for 116 applied at 116\n"
                                  "Body.level=3 for 120 applied at 120\n"
                                  "Body.level=4 for 125 applied at 130\n");
    // Without a value to start from, the ramp says so.
    World empty;
    check_text(text_of(empty.run(read(kHead + "  - ramp: { Body.level: { to: 4, over_ms: 20 } }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=20 scenario-write: the ramp of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 0 ms and the scenario has not written it; give the ramp from: <value>\n");
}
