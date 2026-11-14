// SPDX-License-Identifier: MIT
//
// The scenario file (scenario.hpp: reading and checking, the wording of its errors) and the
// engine that runs a scenario (scenario_run.hpp) against a model of ECUs that steps in virtual
// time: when the writes take effect, that no sample is skipped, the wording of the failures, and
// that the result is the same whenever the samples and results of a step arrive. At the end: the
// timeline of a run (timeline.hpp) that the engine notes against the same model, compared with
// the files of tests/launch/timeline.
#include "launch_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/launch/scenario.hpp"
#include "vehicle_sil/launch/scenario_run.hpp"
#include "vehicle_sil/launch/timeline.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
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
    /// The ECU writes its own input `level` in its step at this time (a writer the scenario does
    /// not know).
    std::optional<std::pair<SimTime, std::int64_t>> own_level{};
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
    /// A participant failed and ended the run: the engine is told so when the run is over.
    bool superseded = false;
    /// timeline_of(): the simulation ends here for another reason (none: after 5 s).
    std::optional<SimDuration> ends_after;
    /// "Body.level=5 for 130 applied at 130", in the order applied.
    std::vector<std::string> log;
    ScenarioEngine* engine = nullptr;
    const Scenario* scenario = nullptr;
    /// What the engine notes for a timeline, if set (run()).
    TimelineRecord* record = nullptr;

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
        ScenarioEngine e(s, tick, *this, {}, record);
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
        if (superseded)
        {
            e.superseded();
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
        if (m.own_level && m.own_level->first == now)
        {
            m.level = m.own_level->second;
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
               "E 3 unknown-key: unknown key 'step': did you mean 'steps'? (the keys here: vehicle_sil_scenario, name, only, params, inputs, steps)\n"
               "E 0 missing-key: the file has no vehicle_sil_scenario (the format version): add vehicle_sil_scenario: 1\n");
    check_text(errors(kHead + "  - sets: { Body.level: 1 }\n"),
               "E 3 unknown-key: unknown key 'sets': did you mean 'set'? (the keys here: at_ms, after_ms, set, ramp, wave, dither, wait_until, "
               "timeout_ms, expect, within_ms, hold_for_ms, undersampled)\n"
               "E 3 value: the step does nothing: give it one of set, ramp, wave, dither, wait_until, expect\n");
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
               "(1000000000000 ms): shorten the times (after_ms, over_ms, for_ms, hold_for_ms, the timeouts) of the steps up to here\n");
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

namespace {

ParamValue param(std::string name, std::string text, ParamSource source, bool quoted = false)
{
    ParamValue out;
    out.name = std::move(name);
    out.text = std::move(text);
    out.source = source;
    out.quoted = quoted;
    return out;
}

/// "name=text from", one per line.
std::string params_text(const std::vector<ParamValue>& params)
{
    std::string out;
    for (const auto& p : params)
    {
        out += p.name + "=" + (p.quoted ? "\"" + p.text + "\"" : p.text) + " " + param_source_name(p.source) + "\n";
    }
    return out;
}

/// The catalog with Body.boost and Door.boost said to be absent.
ScenarioCatalog catalog_without_boost()
{
    ScenarioCatalog c = catalog();
    c[0].absent = {"boost"};
    c[1].absent = {"boost"};
    return c;
}

} // namespace

TEST_CASE(scenario_parameters_stand_for_values_and_the_later_source_wins)
{
    const std::string text = "vehicle_sil_scenario: 1\n"
                             "params: { level: 5, mode: auto, wait: 100, unused: 1 }\n"
                             "steps:\n"
                             "  - set: { Body.level: \"${level}\", Door.mode: \"${mode}\", Door.lamp: \"${lamp}\" }\n"
                             "  - at_ms: ${wait}\n"
                             "    ramp:\n"
                             "      Body.ratio: { to: \"${ratio}\", over_ms: \"${wait}\" }\n"
                             "  - expect:\n"
                             "      Body.body.speed: { near: \"${level}\", tolerance: \"${ratio}\" }\n"
                             "    within_ms: ${wait}\n";
    ScenarioContext context;
    context.params = {param("level", "7", ParamSource::Configuration), param("lamp", "true", ParamSource::Configuration),
                      param("ratio", "0.25", ParamSource::Configuration), param("elsewhere", "1", ParamSource::Configuration),
                      param("level", "9", ParamSource::Variant), param("wait", "40", ParamSource::CommandLine)};
    const ScenarioRead r = read_scenario(text, catalog(), context);
    check_text(lines(r.findings), "");
    CHECK(r.scenario.has_value());
    std::string texts;
    for (const auto& step : r.scenario->steps)
    {
        texts += std::to_string(step.line) + ": " + step.text + "\n";
    }
    check_text(texts, "4: set Body.level = 9; Door.mode = auto (2); Door.lamp = true\n"
                      "5: ramp Body.ratio to 0.25 over 40 ms\n"
                      "8: expect Body.body.speed near 9 (tolerance 0.25) (within 40 ms)\n");
    CHECK_EQ(*r.scenario->steps[1].at_ms, std::int64_t{40});
    // What the scenario declares, then what it uses besides; not what only the configuration has.
    check_text(params_text(r.params), "level=9 variant\n"
                                      "mode=auto scenario\n"
                                      "wait=40 command_line\n"
                                      "unused=1 scenario\n"
                                      "lamp=true configuration\n"
                                      "ratio=0.25 configuration\n");
    // Without the context the defaults count, and what has none is an error once.
    const ScenarioRead alone = read_scenario(text, catalog());
    CHECK(!alone.scenario.has_value());
    check_text(lines(alone.findings),
               "E 4 scenario-param: ${lamp} has no value: give it a default in the scenario (params: { lamp: <value> }), or a "
               "value in the configuration's params, in a variant's params or with --param lamp=<value>\n"
               "E 7 scenario-param: ${ratio} has no value: give it a default in the scenario (params: { ratio: <value> }), or a "
               "value in the configuration's params, in a variant's params or with --param ratio=<value>\n");
    CHECK_EQ(alone.steps, std::size_t{3});
}

TEST_CASE(scenario_parameter_errors_say_what_to_write)
{
    const auto errors = [](const std::string& text, const ScenarioContext& context = {}) {
        return lines(read_scenario(text, catalog(), context).findings);
    };
    // Only a whole value; not a key, not a part, no expression.
    check_text(errors("vehicle_sil_scenario: 1\n"
                      "params: { n: 5, 2nd: 1, list: [1], other: \"${n}\" }\n"
                      "steps:\n"
                      "  - set: { \"Body.${n}\": 1 }\n"
                      "  - set: { Body.level: \"${n} \" }\n"
                      "  - set: { Body.level: \"${n+1}\" }\n"
                      "  - at_ms: \"${m}\"\n"
                      "    set: { Body.level: \"${m}\" }\n"),
               "E 2 value: params: '2nd' cannot be a parameter's name: use letters, digits and _ (not a digit first), as a "
               "scenario writes it in ${...}\n"
               "E 2 value: params: list must be one value (a number, true or false, or a text), not a list\n"
               "E 2 value: params: other is the text \"${n}\", but a parameter's value cannot use another parameter (${...}): "
               "write the value out\n"
               "E 4 scenario-param: 'Body.${n}': a parameter (${...}) stands for a value, not for a key or for a variable's "
               "name: write the name out\n"
               "E 5 scenario-param: '${n} ': a parameter is written ${name} (letters, digits and _) and is the whole value: it "
               "cannot be a part of a text, and there are no expressions; work the value out and give it as a parameter of "
               "its own\n"
               "E 6 scenario-param: '${n+1}': a parameter is written ${name} (letters, digits and _) and is the whole value: "
               "it cannot be a part of a text, and there are no expressions; work the value out and give it as a parameter "
               "of its own\n"
               "E 7 scenario-param: ${m} has no value (did you mean ${n}?): give it a default in the scenario (params: { m: "
               "<value> }), or a value in the configuration's params, in a variant's params or with --param m=<value>\n");
    // A value that does not fit says where it comes from.
    ScenarioContext context;
    context.params = {param("n", "high", ParamSource::CommandLine), param("t", "10", ParamSource::Variant, true)};
    check_text(errors("vehicle_sil_scenario: 1\n"
                      "params: { n: 5 }\n"
                      "steps:\n"
                      "  - set: { Body.level: \"${n}\" }\n"
                      "  - after_ms: \"${t}\"\n"
                      "    set: { Body.level: 1 }\n",
                      context),
               "E 4 scenario-value: the value of Body.level (integer) must be a whole number, not 'high' (the parameter n, from "
               "--param)\n"
               "E 5 value: after_ms must be a whole number (milliseconds of virtual time from the end of the step before it), "
               "not the text \"10\" (the parameter t, from the variant's params): write the number without quotes\n");
    // A --param that the scenario does not have changes nothing: an error, not silence. It is the
    // command line's (a usage error), not a finding of the file.
    context.params = {param("leve", "1", ParamSource::CommandLine), param("elsewhere", "1", ParamSource::Configuration)};
    const ScenarioRead near = read_scenario("vehicle_sil_scenario: 1\n"
                                            "params: { level: 5 }\n"
                                            "steps:\n"
                                            "  - set: { Body.level: \"${level}\" }\n",
                                            catalog(), context);
    check_text(lines(near.findings), "");
    check_text(lines(near.usage_errors),
               "E 0 usage: --param leve=1: the scenario has no parameter leve (did you mean level?): its parameters "
               "are level; remove the option or correct the name\n");
    const ScenarioRead none = read_scenario(kHead + "  - set: { Body.level: 2 }\n", catalog(), context);
    check_text(lines(none.findings), "");
    check_text(lines(none.usage_errors),
               "E 0 usage: --param leve=1: the scenario has no parameter leve: it uses none (a step uses one as "
               "${name}); remove the option or correct the name\n");
}

TEST_CASE(scenario_values_of_integers_and_enums_may_be_hexadecimal)
{
    // Where a value of an integer or an enum stands; the steps' texts stay decimal.
    ScenarioContext context;
    context.params = {param("code", "0X1f", ParamSource::CommandLine)};
    const ScenarioRead r = read_scenario(
        "vehicle_sil_scenario: 1\n"
        "params: { code: 0, top: 0x64 }\n"
        "steps:\n"
        "  - set: { Body.level: 0x0, Body.gain: 0x7FFFFFFFFFFFFFFF, Door.mode: 0x2, Door.level: \"${top}\" }\n"
        "  - ramp: { Body.level: { from: 0x0A, to: 0X64, over_ms: 100 } }\n"
        "  - expect: { Body.level: 0x64, Door.mode: { ne: 0x0 }, Body.gain: { ge: 0x10, le: 0x7fffffffffffffff, gt: 0xf, lt: 0x7FFFFFFFFFFFFFFF } }\n"
        "  - wait_until: { Body.level: { eq: \"${code}\" } }\n"
        "    timeout_ms: 10\n"
        "  - wave: { Body.level: { square: { low: 0x10, high: 0x20, period_ms: 100 }, for_ms: 200 },\n"
        "            Door.level: { sine: { amplitude: 5, period_ms: 100 }, around: 0x32, for_ms: 100 } }\n"
        "  - wave: { Body.level: { points: [[0, 0x1], [50, 0xa]] }, Door.mode: { points: [[0, 0x1], [10, 0x2]] } }\n",
        catalog(), context);
    check_text(lines(r.findings), "");
    CHECK(r.scenario.has_value());
    if (!r.scenario)
    {
        return;
    }
    std::string texts;
    for (const auto& step : r.scenario->steps)
    {
        texts += step.text + "\n";
    }
    check_text(texts, "set Body.level = 0; Body.gain = 9223372036854775807; Door.mode = auto (2); Door.level = 100\n"
                      "ramp Body.level from 10 to 100 over 100 ms\n"
                      "expect Body.level eq 100; Door.mode ne off (0); Body.gain ge 16, le 9223372036854775807, gt 15, lt "
                      "9223372036854775807\n"
                      "wait_until Body.level eq 31 (timeout 10 ms)\n"
                      "wave Body.level square low 16 high 32 period 100 ms for 200 ms; Door.level sine amplitude 5 period 100 ms "
                      "around 50 for 100 ms\n"
                      "wave Body.level points (2, linear) over 50 ms; Door.mode points (2, hold) over 10 ms\n");

    // What is not read, and what to write instead.
    const auto errors = [](const std::string& text) { return lines(read_scenario(text, catalog()).findings); };
    check_text(errors(kHead + "  - set: { Body.level: 0x65, Door.level: -0x10, Body.gain: \"0x10\", Body.ratio: 0x1 }\n"),
               "E 3 scenario-value: the value of Body.level (integer) is 0x65 (101), outside its range (from 0 to 100)\n"
               "E 3 scenario-value: the value of Door.level (integer) must be a whole number, not '-0x10': write a negative number "
               "in decimal (-16)\n"
               "E 3 scenario-value: the value of Body.gain (integer) must be a whole number, not the text \"0x10\": write the "
               "number without quotes\n"
               "E 3 scenario-value: the value of Body.ratio (real) must be a number, not '0x1': hexadecimal (0x...) is for "
               "integer and enum variables only; write 1\n");
    check_text(errors(kHead + "  - set: { Body.gain: 0x8000000000000000, Door.gain: 0x, Body.level: 0x1G, Door.level: 0o17 }\n"),
               "E 3 scenario-value: the value of Body.gain (integer) must be a whole number, not '0x8000000000000000': it is "
               "more than an integer holds (at most 0x7FFFFFFFFFFFFFFF)\n"
               "E 3 scenario-value: the value of Door.gain (integer) must be a whole number, not '0x'\n"
               "E 3 scenario-value: the value of Body.level (integer) must be a whole number, not '0x1G'\n"
               "E 3 scenario-value: the value of Door.level (integer) must be a whole number, not '0o17'\n");
    check_text(errors(kHead + "  - set: { Door.mode: -0x1, Body.mode: 0x5, Door.lamp: 0x1 }\n"),
               "E 3 scenario-value: the value of Door.mode (enum) must be one of its items, by name or by value, not '-0x1': "
               "write a negative number in decimal (-1); the items are off (0), on (1), auto (2)\n"
               "E 3 scenario-value: the value of Body.mode (enum) must be one of its items, by name or by value, not '0x5': the "
               "items are off (0), on (1), auto (2)\n"
               "E 3 scenario-value: the value of Door.lamp (bool) must be true or false, not '0x1'\n");
    check_text(errors(kHead + "  - ramp: { Body.level: { to: 0x65, over_ms: 10 }, Body.ratio: { to: 0x1, over_ms: 10 } }\n"
                              "  - expect: { Body.level: { ge: -0x1 }, Body.body.speed: { le: 0x10 } }\n"
                              "  - wave: { Body.level: { pulse: { value: 0x65, width_ms: 10 } } }\n"),
               "E 3 scenario-value: to of the ramp of Body.level is 0x65 (101), outside its range (from 0 to 100)\n"
               "E 3 scenario-value: to of the ramp of Body.ratio must be a number, not '0x1': hexadecimal (0x...) is for "
               "integer and enum variables only; write 1\n"
               "E 4 scenario-value: ge of Body.level (integer) must be a whole number, not '-0x1': write a negative number in "
               "decimal (-1)\n"
               "E 4 scenario-value: le of Body.body.speed (real) must be a number, not '0x10': hexadecimal (0x...) is for "
               "integer and enum variables only; write 16\n"
               "E 5 scenario-value: value of the pulse of Body.level is 0x65 (101), outside its range (from 0 to 100)\n");
}

TEST_CASE(scenario_only_names_the_variants_it_is_for)
{
    const std::string text = "vehicle_sil_scenario: 1\n"
                             "only: [small, medium]\n"
                             "steps:\n"
                             "  - set: { Body.nothing: 1 }\n";
    ScenarioContext context;
    context.variants = {"small", "medium", "large"};
    context.variant = "large";
    // Not for this variant: said, and the steps are not checked (they may not fit it).
    const ScenarioRead other = read_scenario(text, catalog(), context);
    CHECK(other.not_for_variant && !other.scenario.has_value());
    CHECK(other.only == std::vector<std::string>({"small", "medium"}));
    CHECK_EQ(other.steps, std::size_t{1});
    check_text(lines(other.findings),
               "E 2 scenario-variant: the scenario is only for the variants small, medium (its only), not for large: run it "
               "with --variant small or --variant medium, or add large to only\n");
    // For this variant: read as any other.
    context.variant = "medium";
    const ScenarioRead mine = read_scenario(text, catalog(), context);
    CHECK(!mine.not_for_variant);
    CHECK(lines(mine.findings).find("E 4 scenario-variable: 'Body.nothing'") == 0);
    // The names must be variants of the configuration.
    context.variants = {"small", "large"};
    context.variant = "small";
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nonly: [small, medum, larg]\nsteps:\n  - set: { Body.level: 1 }\n", catalog(), context).findings),
               "E 2 scenario-variant: only names the variant 'medum', which the configuration does not define; its variants are "
               "small, large\n"
               "E 2 scenario-variant: only names the variant 'larg', which the configuration does not define (did you mean "
               "'large'?); its variants are small, large\n");
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nonly: [small]\nsteps:\n  - set: { Body.level: 1 }\n", catalog()).findings),
               "E 2 scenario-variant: only names small, but the configuration defines no variants: remove only, or add variants "
               "to the configuration (docs/simulation-config.md)\n");
    check_text(lines(read_scenario("vehicle_sil_scenario: 1\nonly: small\nsteps:\n  - set: { Body.level: 1 }\n", catalog(), context).findings),
               "E 2 value: only must be a list of the variants the scenario is for (only: [small, medium]), not 'small'; remove "
               "it for a scenario for every variant\n");
}

TEST_CASE(scenario_steps_that_use_an_absent_variable_are_skipped_and_the_others_run)
{
    ScenarioContext context;
    context.variant = "small";
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                 "  - after_ms: 30\n"
                                                 "    set: { Body.boost: 2, Door.level: 7 }\n"
                                                 "  - after_ms: 20\n"
                                                 "    wait_until: { Body.boost: { ge: 1 }, Door.boost: 1 }\n"
                                                 "    timeout_ms: 500\n"
                                                 "  - after_ms: 10\n"
                                                 "    ramp: { Body.boost: { to: 3, over_ms: 100 } }\n"
                                                 "  - after_ms: 10\n"
                                                 "    expect: { Body.level: 5, Door.level: 0 }\n",
                                         catalog_without_boost(), context);
    check_text(lines(r.findings),
               "N 4 scenario-step-skipped: the step is skipped (set Body.boost; Door.level = 7): Body.boost is absent: participant "
               "'Body' does not have it in the variant small (absent: true in its variables); the other steps run, also "
               "those that would need what this one does\n"
               "N 6 scenario-step-skipped: the step is skipped (wait_until Body.boost; Door.boost (timeout 500 ms)): Body.boost, "
               "Door.boost are absent: the participants 'Body', 'Door' do not have them in the variant small (absent: true in "
               "their variables); the other steps run, also those that would need what this one does\n"
               "N 9 scenario-step-skipped: the step is skipped (ramp Body.boost): Body.boost is absent: participant 'Body' does "
               "not have it in the variant small (absent: true in its variables); the other steps run, also those that would "
               "need what this one does\n");
    CHECK(r.scenario.has_value());
    CHECK_EQ(r.skipped, std::size_t{3});
    // A skipped step writes nothing (Door.level stays 0) and takes the time its text says: the
    // ramp's 100 ms, nothing for the set and for the wait. It has no time that decided it.
    World world;
    std::vector<std::string> events;
    const ScenarioRun run = world.run(*r.scenario);
    check_text(text_of(run),
               "passed over=180\n"
               "3 passed start=0 at=0 end=0\n"
               "4 skipped start=30 at=- end=30 scenario-skipped: Body.boost is absent: participant 'Body' does not have it in "
               "the variant small (absent: true in its variables)\n"
               "6 skipped start=50 at=- end=50 scenario-skipped: Body.boost, Door.boost are absent: the participants 'Body', "
               "'Door' do not have them in the variant small (absent: true in their variables)\n"
               "9 skipped start=60 at=- end=160 scenario-skipped: Body.boost is absent: participant 'Body' does not have it in "
               "the variant small (absent: true in its variables)\n"
               "11 passed start=170 at=170 end=170\n");
    CHECK_EQ(run.skipped(), std::size_t{3});
    check_text(joined(world.log), "Body.level=5 for 0 applied at 0\n");
    // A variable that is not said to be absent is an error as before.
    CHECK(lines(read_scenario(kHead + "  - set: { Body.boost: 2 }\n", catalog()).findings).find("E 3 scenario-variable: 'Body.boost': participant 'Body' has no variable 'boost'") == 0);
    // Only skipped steps: the scenario passes, and says that it looked at nothing.
    const ScenarioRead none = read_scenario(kHead + "  - set: { Body.boost: 2 }\n", catalog_without_boost());
    CHECK(none.scenario.has_value());
    World empty;
    const ScenarioRun nothing = empty.run(*none.scenario);
    check_text(text_of(nothing), "passed over=0\n"
                                 "3 skipped start=0 at=- end=0 scenario-skipped: Body.boost is absent: participant 'Body' does not "
                                 "have it in this configuration (absent: true in its variables)\n");
    // A failure before a skipped step leaves it skipped; the simulation's end does not fail it.
    World stopped;
    stopped.stop_at = at(40ms);
    const ScenarioRead late = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                    "  - at_ms: 100\n"
                                                    "    set: { Body.boost: 2 }\n"
                                                    "  - at_ms: 200\n"
                                                    "    expect: { Body.level: 5 }\n",
                                            catalog_without_boost());
    CHECK(late.scenario.has_value());
    const ScenarioRun cut = stopped.run(*late.scenario);
    CHECK(cut.status == ScenarioStatus::Failed);
    CHECK(cut.steps[1].status == StepStatus::Skipped && cut.steps[2].status == StepStatus::Failed);
}

/// The errors alone (lines()), without the notes and the warnings.
std::string errors_only(const std::vector<Finding>& findings)
{
    std::vector<Finding> errors;
    std::copy_if(findings.begin(), findings.end(), std::back_inserter(errors), [](const Finding& f) { return f.severity == Severity::Error; });
    return lines(errors);
}

/// The starts of the steps that are not skipped, "line:start_ms" each.
std::string starts_of(const ScenarioRun& run)
{
    std::string out;
    for (const ScenarioStepRun& step : run.steps)
    {
        if (step.status != StepStatus::Skipped)
        {
            out += std::to_string(step.line) + ":" + time_text(step.start) + " ";
        }
    }
    return out;
}

TEST_CASE(scenario_times_are_the_same_whether_a_step_with_a_known_length_is_skipped_or_not)
{
    // The ramp's over_ms and the expect's hold_for_ms are times of the text: a configuration
    // that skips the steps gives the steps after them the times of one that runs them.
    const std::string text = kHead + "  - set: { Body.level: 0 }\n"
                                     "  - ramp: { Body.boost: { to: 3, over_ms: 100 }, Body.ratio: { to: 1, over_ms: 40 } }\n"
                                     "  - after_ms: 10\n"
                                     "    expect: { Body.boost: { ge: 0 } }\n"
                                     "    hold_for_ms: 200\n"
                                     "  - expect: { Body.pulse: false }\n"
                                     "  - at_ms: 400\n"
                                     "    set: { Body.lamp: true }\n";
    ScenarioCatalog with = catalog();
    auto& boost = with[0].variables.emplace_back();
    boost.name = "boost";
    boost.type = control::ValueType::Integer;
    boost.causality = control::Causality::Input;
    const ScenarioRead there = read_scenario(text, with);
    const ScenarioRead absent = read_scenario(text, catalog_without_boost());
    CHECK(there.scenario.has_value() && absent.scenario.has_value());
    CHECK_EQ(there.skipped, std::size_t{0});
    CHECK_EQ(absent.skipped, std::size_t{2});
    // The mixed ramp is skipped whole, with its longest over_ms, the absent variable's.
    CHECK_EQ(absent.scenario->steps[1].skipped_ms, std::int64_t{100});
    CHECK_EQ(absent.scenario->steps[2].skipped_ms, std::int64_t{200});
    World a;
    const ScenarioRun skipped = a.run(*absent.scenario);
    CHECK(skipped.status == ScenarioStatus::Passed);
    check_text(starts_of(skipped), "3:0 8:310 9:400 ");
    CHECK(skipped.steps[1].start == at(0ms) && skipped.steps[1].end == at(100ms) && !skipped.steps[1].at);
    CHECK(skipped.steps[2].start == at(110ms) && skipped.steps[2].end == at(310ms) && !skipped.steps[2].at);
    // Where time would go back is the same in both: at_ms inside the skipped ramp.
    const std::string back = kHead + "  - ramp: { Body.boost: { to: 3, over_ms: 100 } }\n"
                                     "  - at_ms: 50\n"
                                     "    set: { Body.lamp: true }\n";
    const std::string wrong = "E 4 scenario-time: at_ms is 50, before the end of the step before it (100 ms): the steps run in "
                              "the order of the file, and time does not go back; reorder the steps or raise at_ms\n";
    check_text(errors_only(read_scenario(back, with).findings), wrong);
    check_text(errors_only(read_scenario(back, catalog_without_boost()).findings), wrong);
    // A skipped step that waits takes no time (when it would end is not in the text), and the
    // text is read as if it waited: no at_ms after it.
    const ScenarioRead waits = read_scenario(kHead + "  - expect: { Body.boost: 1 }\n"
                                                     "    within_ms: 300\n"
                                                     "  - after_ms: 20\n"
                                                     "    expect: { Body.pulse: false }\n",
                                             catalog_without_boost());
    CHECK(waits.scenario.has_value());
    CHECK_EQ(waits.scenario->steps[0].skipped_ms, std::int64_t{0});
    World w;
    check_text(starts_of(w.run(*waits.scenario)), "5:20 ");
    CHECK(errors_only(read_scenario(kHead + "  - expect: { Body.boost: 1 }\n"
                                            "    within_ms: 300\n"
                                            "  - at_ms: 400\n"
                                            "    expect: { Body.pulse: false }\n",
                                    catalog_without_boost())
                          .findings)
              .find("E 5 scenario-time: at_ms after a step that waits (line 3)") == 0);
    // The over_ms of an absent variable is read like any other time; its to and from are not.
    check_text(errors_only(read_scenario(kHead + "  - ramp: { Body.boost: { to: high, over_ms: soon } }\n", catalog_without_boost()).findings),
               "E 3 value: over_ms must be a whole number (milliseconds of virtual time the ramp takes), not 'soon'\n");
}

TEST_CASE(scenario_steps_that_use_a_participant_the_variant_leaves_out_are_skipped)
{
    // Door is not in the variant (its enabled: false there): like an absent variable, whatever
    // the variable's name. Spare is left out in another way (--disable): an error as before.
    ScenarioCatalog c = catalog_without_boost();
    c[1] = CatalogParticipant{"Door", "", SimDuration{}, {}};
    c[1].not_in_variant = true;
    ScenarioContext context;
    context.variants = {"small", "medium"};
    context.variant = "medium";
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                 "  - set: { Door.level: 7 }\n"
                                                 "  - expect: { Door.anything: 1, Door.level: 7, Body.boost: 1 }\n"
                                                 "    hold_for_ms: 50\n"
                                                 "  - expect: { Body.level: 5 }\n",
                                         c, context);
    check_text(lines(r.findings),
               "N 4 scenario-step-skipped: the step is skipped (set Door.level): participant 'Door' is not in the variant medium "
               "(enabled: false in that variant); the other steps run, also those that would need what this one does\n"
               "N 5 scenario-step-skipped: the step is skipped (expect Door.anything; Door.level; Body.boost (for 50 ms)): "
               "participant 'Door' is not in the variant medium (enabled: false in that variant); Body.boost is absent: "
               "participant 'Body' does not have it in the variant medium (absent: true in its variables); the other steps "
               "run, also those that would need what this one does\n");
    CHECK(r.scenario.has_value());
    World world;
    const ScenarioRun run = world.run(*r.scenario);
    CHECK(run.status == ScenarioStatus::Passed);
    check_text(starts_of(run), "3:0 7:50 ");
    CHECK(run.steps[1].message == "participant 'Door' is not in the variant medium (enabled: false in that variant)");
    // The other ways of being left out do not say "variant", and do not skip.
    check_text(lines(read_scenario(kHead + "  - set: { Spare.level: 1 }\n", c, context).findings),
               "E 3 scenario-variable: 'Spare.level': participant 'Spare' is left out of this run (enabled: false, or --disable)\n");
}

TEST_CASE(scenario_that_checks_nothing_after_the_skips_gets_a_warning)
{
    ScenarioContext context;
    context.variants = {"small", "medium", "large"};
    context.variant = "small";
    // Every step that checks is skipped; the set that is left checks nothing.
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: 5 }\n"
                                                 "  - expect: { Body.boost: 1 }\n"
                                                 "  - wait_until: { Body.boost: 2 }\n"
                                                 "    timeout_ms: 100\n",
                                         catalog_without_boost(), context);
    CHECK(r.scenario.has_value());
    std::string warnings;
    for (const Finding& f : r.findings)
    {
        if (f.severity == Severity::Warning)
        {
            warnings += lines({f});
        }
    }
    check_text(warnings,
               "W 0 scenario-nothing-checked: the scenario checks nothing in the variant small: all its 2 steps that check "
               "(expect or wait_until, lines 4, 5) are skipped, so it passes whatever the ECUs do (the notes say why each is "
               "skipped). If the scenario is not for this variant, list the variants it is for in the scenario (only: [medium, "
               "large]); if it is, map what the steps look at to what the variant has (variables in the variant, "
               "docs/simulation-config.md)\n");
    // Without variants: no "only" to write.
    const ScenarioRead plain = read_scenario(kHead + "  - expect: { Body.boost: 1 }\n", catalog_without_boost());
    check_text(lines(plain.findings).substr(lines(plain.findings).find("W 0")),
               "W 0 scenario-nothing-checked: the scenario checks nothing in this configuration: its only step that checks "
               "(expect or wait_until, line 3) is skipped, so it passes whatever the ECUs do (the notes say why each is "
               "skipped). If the configuration has what the steps look at under other names, map them in the participant's "
               "variables (docs/simulation-config.md); else do not run this scenario with it\n");
    // One step that checks is left: no warning. Nor for a scenario that never checked anything.
    const auto warned = [&](const std::string& steps) {
        const ScenarioRead read = read_scenario(kHead + steps, catalog_without_boost(), context);
        return std::any_of(read.findings.begin(), read.findings.end(), [](const Finding& f) { return f.severity != Severity::Note; });
    };
    CHECK(!warned("  - expect: { Body.boost: 1 }\n  - expect: { Body.level: 0 }\n"));
    CHECK(!warned("  - set: { Body.boost: 1 }\n  - set: { Body.level: 1 }\n"));
    CHECK(!warned("  - set: { Body.boost: 1 }\n"));
}

TEST_CASE(scenario_parameter_without_quotes_in_braces_gets_a_hint)
{
    // YAML does not take "${" in { ... } without quotes: the syntax error says what to write.
    const ScenarioRead r = read_scenario(kHead + "  - set: { Body.level: ${n} }\n", catalog());
    CHECK_EQ(r.findings.size(), std::size_t{1});
    CHECK(r.findings[0].code == "syntax" && r.findings[0].line == 3);
    CHECK(r.findings[0].message.find(": inside { ... } or [ ... ], write a parameter in quotes (\"${name}\")") != std::string::npos);
}

// --- wave ---

namespace {

/// The findings of a scenario's steps (after kHead), with Body and Door stepping as given.
std::string wave_findings(const std::string& steps, SimDuration body_step = 10ms, SimDuration door_step = 10ms)
{
    return lines(read_scenario(kHead + steps, catalog(body_step, door_step)).findings);
}

/// What was applied to one variable, "value@time" each ("Body.level": "10@0 12@10 ").
std::string applied(const World& world, const std::string& variable)
{
    std::string out;
    for (const std::string& entry : world.log)
    {
        const std::string head = variable + "=";
        if (entry.rfind(head, 0) == 0)
        {
            const std::size_t forAt = entry.find(" for ");
            out += entry.substr(head.size(), forAt - head.size()) + "@" + entry.substr(entry.rfind(' ') + 1) + " ";
        }
    }
    return out;
}

} // namespace

TEST_CASE(scenario_wave_is_read_with_its_shape_and_how_long_it_lasts)
{
    const Scenario s = read(
        kHead + "  - wave: { Body.ratio: { sine: { amplitude: 0.2, period_ms: 200 }, around: 0.5, for_ms: 1000 } }\n"
                "  - wave: { Body.level: { triangle: { amplitude: 5, frequency_hz: 2.5, phase_deg: 90 }, around: 50, for_ms: 300 },\n"
                "            Door.level: { sawtooth: { amplitude: 5, period_ms: 100, falling: true }, around: 50, for_ms: 250 } }\n"
                "  - wave: { Body.level: { square: { low: 10, high: 20, period_ms: 100, duty: 30 }, for_ms: 200 } }\n"
                "  - wave: { Body.level: { square: { amplitude: 3, period_ms: 100 }, around: 9, for_ms: 200 }, Door.lamp: { square: { period_ms: 40 }, for_ms: 80 } }\n"
                "  - wave: { Door.lamp: { pulse: { width_ms: 30 } }, Body.level: { pulse: { value: 7, width_ms: 50 } } }\n"
                "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 20, seed: 7 }, around: 0.5, for_ms: 100 } }\n"
                "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 20, seed: 7, distribution: gaussian }, for_ms: 100 } }\n"
                "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 1, to_hz: 5, sweep: log }, around: 0.5, for_ms: 2000 } }\n"
                "  - wave: { Body.level: { lag: { from: 0, to: 80, tau_ms: 200, settle: true }, for_ms: 1000 } }\n"
                "  - wave: { Door.lamp: { chatter: { to: true, bounces: 3, interval_ms: 10 } }, Body.level: { chatter: { from: 0, to: 1, interval_ms: 20, seed: 4 }, for_ms: 100 } }\n"
                "  - wave: { Body.level: { points: [[0, 0], [100, 50], [400, 20]] }, Door.mode: { points: [[0, auto], [50, off], [80, 1]] },\n"
                "            Door.lamp: { points: [[10, true], [30, false]], interpolate: hold } }\n"
                "  - after_ms: 5\n"
                "    expect: { Body.level: 20 }\n");
    std::string texts;
    for (const auto& step : s.steps)
    {
        texts += std::to_string(step.line) + ": " + step.text + "\n";
    }
    check_text(texts,
               "3: wave Body.ratio sine amplitude 0.2 period 200 ms around 0.5 for 1000 ms\n"
               "4: wave Body.level triangle amplitude 5 frequency 2.5 Hz phase 90 deg around 50 for 300 ms; Door.level sawtooth "
               "amplitude 5 period 100 ms falling around 50 for 250 ms\n"
               "6: wave Body.level square low 10 high 20 period 100 ms duty 30 % for 200 ms\n"
               "7: wave Body.level square amplitude 3 period 100 ms around 9 for 200 ms; Door.lamp square period 40 ms for 80 ms\n"
               "8: wave Door.lamp pulse true width 30 ms; Body.level pulse 7 width 50 ms\n"
               "9: wave Body.ratio noise amplitude 0.1 hold 20 ms seed 7 around 0.5 for 100 ms\n"
               "10: wave Body.ratio noise amplitude 0.1 hold 20 ms seed 7 gaussian for 100 ms\n"
               "11: wave Body.ratio chirp amplitude 0.1 from 1 Hz to 5 Hz log around 0.5 for 2000 ms\n"
               "12: wave Body.level lag from 0 to 80 tau 200 ms settle for 1000 ms\n"
               "13: wave Door.lamp chatter to true 3 bounces interval 10 ms; Body.level chatter from 0 to 1 interval 20 ms seed 4 for 100 ms\n"
               "14: wave Body.level points (3, linear) over 400 ms; Door.mode points (3, hold) over 80 ms; Door.lamp points (2, hold) "
               "over 30 ms\n"
               "16: expect Body.level eq 20\n");
    CHECK(s.steps[0].kind == StepKind::Wave);
    CHECK(std::string(step_kind_name(StepKind::Wave)) == "wave");
    // Each step lasts as long as its longest wave: the last expect is 5 ms after the points end.
    World world;
    const ScenarioRun run = world.run(s, 10s);
    CHECK(run.status == ScenarioStatus::Passed);
    check_text(starts_of(run), "3:0 4:1000 6:1300 7:1500 8:1700 9:1750 10:1850 11:1950 12:3950 13:4950 14:5050 16:5455 ");
}

TEST_CASE(scenario_wave_writes_its_shape_in_the_steps_of_its_ecu_and_returns_to_the_middle)
{
    // A triangle of 80 ms around 10 for 100 ms (not a whole number of periods): a value per step
    // of Body from the time alone, rounded for the integer, and at the end the middle again.
    // Door's bool is high for a quarter of each 40 ms and low at the end.
    const Scenario s = read(kHead + "  - at_ms: 100\n"
                                    "    wave: { Body.level: { triangle: { amplitude: 4, period_ms: 80 }, around: 10, for_ms: 100 },\n"
                                    "            Door.lamp: { square: { period_ms: 40, duty: 25 }, for_ms: 80 } }\n"
                                    "  - expect: { Body.level: 10, Door.lamp: false }\n");
    World world;
    const ScenarioRun run = world.run(s);
    check_text(text_of(run), "passed over=210\n"
                             "3 passed start=100 at=200 end=200\n"
                             "6 passed start=200 at=200 end=200\n");
    check_text(applied(world, "Body.level"), "10@100 12@110 14@120 12@130 10@140 8@150 6@160 8@170 10@180 12@190 10@200 ");
    check_text(applied(world, "Door.lamp"), "true@100 false@110 false@120 false@130 true@140 false@150 false@160 false@170 false@180 ");
}

TEST_CASE(scenario_wave_without_around_swings_around_the_value_the_variable_has)
{
    // After a ramp to 80: around 80, and 80 again at the end. Then around what the wave left.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n"
                                    "  - ramp: { Body.level: { to: 80, over_ms: 20 } }\n"
                                    "  - wave: { Body.level: { sawtooth: { amplitude: 10, period_ms: 40 }, for_ms: 60 } }\n"
                                    "  - wave: { Body.level: { square: { amplitude: 5, period_ms: 20 }, for_ms: 30 } }\n");
    World world;
    const ScenarioRun run = world.run(s);
    check_text(text_of(run), "passed over=120\n"
                             "3 passed start=0 at=0 end=0\n"
                             "4 passed start=0 at=20 end=20\n"
                             "5 passed start=20 at=80 end=80\n"
                             "6 passed start=80 at=110 end=110\n");
    check_text(applied(world, "Body.level"), "20@0 20@0 50@10 80@20 "
                                             "70@20 75@30 80@40 85@50 70@60 75@70 80@80 "
                                             "85@80 75@90 85@100 80@110 ");
    // Without a value to swing around, the wave says so, and what to write.
    World empty;
    check_text(text_of(empty.run(read(kHead + "  - wave: { Body.level: { sine: { amplitude: 4, period_ms: 80 }, for_ms: 20 } }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=20 scenario-write: the wave of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 0 ms and the scenario has not written it; give the wave around: <value>\n");
    // A pulse returns to the value the variable has: without one, it says how to give it one.
    World bare;
    check_text(text_of(bare.run(read(kHead + "  - wave: { Body.level: { pulse: { value: 9, width_ms: 20 } } }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=20 scenario-write: the wave of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 0 ms and the scenario has not written it; set the variable in a step before the pulse (set: { "
               "Body.level: <value> })\n");
    // A shape that has its values itself needs none.
    World own;
    CHECK(own.run(read(kHead + "  - wave: { Body.level: { square: { low: 1, high: 2, period_ms: 20 }, for_ms: 20 }, Door.lamp: { pulse: { width_ms: 10 } },\n"
                               "            Door.level: { points: [[0, 3], [20, 5]] }, Body.lamp: { chatter: { to: true, bounces: 1, interval_ms: 10 } } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(own, "Body.level"), "2@0 1@10 1@20 ");
    check_text(applied(own, "Door.lamp"), "true@0 false@10 ");
    check_text(applied(own, "Door.level"), "3@0 4@10 5@20 ");
    check_text(applied(own, "Body.lamp"), "true@0 false@10 true@20 ");
}

TEST_CASE(scenario_ramp_and_wave_start_from_what_the_scenario_wrote_whatever_the_samples_since_say)
{
    // The ECU writes 60 to its own input at 30 ms, and its samples say so. The ramp without from
    // still starts from the 20 the scenario wrote, and the wave without around swings around the
    // 80 the ramp left.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n"
                                    "  - at_ms: 50\n"
                                    "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"
                                    "  - at_ms: 100\n"
                                    "    wave: { Body.level: { square: { amplitude: 5, period_ms: 20 }, for_ms: 20 } }\n");
    World world;
    world.ecus[0].own_level = std::pair{at(30ms), std::int64_t{60}};
    World later;
    later.ecus[0].own_level = std::pair{at(80ms), std::int64_t{60}};
    for (World* w : {&world, &later})
    {
        CHECK(w->run(s).status == ScenarioStatus::Passed);
        check_text(applied(*w, "Body.level"), "20@0 20@50 50@60 80@70 85@100 75@110 80@120 ");
    }
    // Before the scenario has written the variable, the sample is all there is: from the ECU's 60.
    World first;
    first.ecus[0].own_level = std::pair{at(30ms), std::int64_t{60}};
    CHECK(first.run(read(kHead + "  - at_ms: 50\n"
                                 "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(first, "Body.level"), "60@50 70@60 80@70 ");
}

TEST_CASE(scenario_wave_shapes_that_go_somewhere_stay_there)
{
    // A lag stays where it has come to (or at `to` with settle), a chatter at `to`, points at the
    // last of them; a pulse returns. (The third step lasts 30 ms, as its longest wave.)
    const Scenario s = read(kHead + "  - wave: { Body.level: { lag: { from: 0, to: 100, tau_ms: 20 }, for_ms: 40 },\n"
                                    "            Door.level: { lag: { from: 0, to: 100, tau_ms: 20, settle: true }, for_ms: 40 } }\n"
                                    "  - wave: { Body.level: { chatter: { to: 50, bounces: 2, interval_ms: 10 } } }\n"
                                    "  - wave: { Body.level: { pulse: { value: 9, width_ms: 20 } }, Door.mode: { points: [[0, on], [20, auto], [30, off]] } }\n"
                                    "  - wave: { Body.level: { points: [[20, 60], [40, 0]], interpolate: hold } }\n"
                                    "  - wave: { Body.level: { points: [[20, 60], [40, 10]] } }\n");
    World world;
    const ScenarioRun run = world.run(s);
    CHECK(run.status == ScenarioStatus::Passed);
    // 100 (1 - e^-0.5) = 39.3, (1 - e^-1) = 63.2, (1 - e^-1.5) = 77.7, (1 - e^-2) = 86.5.
    check_text(applied(world, "Body.level"), "0@0 39@10 63@20 78@30 86@40 "
                                             "50@40 86@50 50@60 86@70 50@80 "
                                             "9@80 9@90 50@100 "
                                             "50@110 50@120 60@130 60@140 0@150 "
                                             "0@150 30@160 60@170 35@180 10@190 ");
    check_text(applied(world, "Door.level"), "0@0 39@10 63@20 78@30 100@40 ");
    check_text(applied(world, "Door.mode"), "1@80 1@90 2@100 0@110 ");
}

TEST_CASE(scenario_wave_gives_the_same_value_at_the_same_time_whatever_the_steps_are)
{
    // The same noise and the same chatter to an ECU that steps every 10 ms and to the same ECU
    // stepping every 4 ms, with a controller of 10 ms and of 20 ms: at the times both step (every
    // 20 ms), the same values.
    const std::string text = kHead + "  - wave: { Body.ratio: { noise: { amplitude: 0.4, hold_ms: 20, seed: 11 }, around: 0.5, for_ms: 200 },\n"
                                     "            Body.level: { chatter: { from: 0, to: 9, interval_ms: 20, seed: 3 }, for_ms: 200 } }\n";
    const auto at_shared_times = [](const World& world, const std::string& variable) {
        std::string out;
        for (const std::string& entry : world.log)
        {
            const std::int64_t when = std::stoll(entry.substr(entry.rfind(' ') + 1));
            if (entry.rfind(variable + "=", 0) == 0 && when % 20 == 0)
            {
                out += entry.substr(entry.find('=') + 1) + "\n";
            }
        }
        return out;
    };
    std::string first;
    std::string firstLevel;
    for (const SimDuration step : {SimDuration{10ms}, SimDuration{4ms}})
    {
        for (const SimDuration tick : {SimDuration{10ms}, SimDuration{20ms}})
        {
            const Scenario s = read(text, catalog(step, 10ms));
            World world;
            world.tick = tick;
            world.ecus[0].step = step;
            CHECK(world.run(s).status == ScenarioStatus::Passed);
            const std::string ratio = at_shared_times(world, "Body.ratio");
            const std::string level = at_shared_times(world, "Body.level");
            // A value per stretch of hold_ms: 10 of them, and the middle at the end.
            CHECK_EQ(std::count(ratio.begin(), ratio.end(), '\n'), std::ptrdiff_t{11});
            CHECK(first.empty() || (first == ratio && firstLevel == level));
            first = ratio;
            firstLevel = level;
        }
    }
    // The values are those of the seed and of the variable's name, on every platform (whole
    // numbers alone make them): the first of them, and the middle at the end.
    check_text(first.substr(0, first.find('\n') + 1), "0.4353229041685524 for 0 applied at 0\n");
    CHECK(first.find("0.5 for 200 applied at 200\n") != std::string::npos);
    // Another seed gives other values. So does the same seed for another variable: two inputs
    // with one seed do not move together.
    const Scenario other = read(kHead + "  - wave: { Body.ratio: { noise: { amplitude: 0.4, hold_ms: 20, seed: 12 }, around: 0.5, for_ms: 200 },\n"
                                        "            Door.ratio: { noise: { amplitude: 0.4, hold_ms: 20, seed: 11 }, around: 0.5, for_ms: 200 },\n"
                                        "            Door.level: { chatter: { from: 0, to: 9, interval_ms: 20, seed: 3 }, for_ms: 200 } }\n");
    World world;
    CHECK(world.run(other).status == ScenarioStatus::Passed);
    CHECK(at_shared_times(world, "Body.ratio") != first);
    CHECK(at_shared_times(world, "Door.ratio") != first);
    CHECK(at_shared_times(world, "Door.ratio") != at_shared_times(world, "Body.ratio"));
    CHECK(at_shared_times(world, "Door.level") != firstLevel);
}

TEST_CASE(scenario_wave_rounds_a_half_for_an_integer_the_same_way_whatever_the_last_bits_are)
{
    // A sine of amplitude 1 and 120 ms is a half at 10 ms and at 50 ms, but for the last bits of
    // the sine: an integer gets 1 there (halves go away from zero), not 0 on one platform and 1
    // on another.
    World world;
    CHECK(world.run(read(kHead + "  - set: { Body.level: 0 }\n"
                                 "  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 120 }, for_ms: 60 } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "0@0 0@0 1@10 1@20 1@30 1@40 1@50 0@60 ");
}

TEST_CASE(scenario_pulse_shorter_than_a_step_is_seen_if_a_step_of_the_ecu_is_in_it)
{
    // Body steps at 0, 10, 20, ... ms. A pulse of 5 ms from 107 ms: its value is applied in the
    // step at 110 ms and what follows it in the one at 120 ms, so the ECU has it for one step.
    // From 201 ms: both are applied in the step at 210 ms, where the later one wins. From 305 ms,
    // ending at a step: the same.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n"
                                    "  - at_ms: 107\n    wave: { Body.level: { pulse: { value: 50, width_ms: 5 } } }\n    undersampled: ok\n"
                                    "  - at_ms: 201\n    wave: { Body.level: { pulse: { value: 60, width_ms: 5 } } }\n    undersampled: ok\n"
                                    "  - at_ms: 305\n    wave: { Body.level: { pulse: { value: 70, width_ms: 5 } } }\n    undersampled: ok\n");
    World world;
    CHECK(world.run(s).status == ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "20@0 50@110 20@120 60@210 20@210 70@310 20@310 ");
    // The check says which it is, where the time of the step is known before the run.
    check_text(wave_findings("  - set: { Body.level: 20 }\n"
                             "  - at_ms: 107\n    wave: { Body.level: { pulse: { value: 50, width_ms: 5 } } }\n"
                             "  - at_ms: 201\n    wave: { Body.level: { pulse: { value: 60, width_ms: 5 } } }\n"
                             "  - at_ms: 305\n    wave: { Body.level: { pulse: { value: 70, width_ms: 5 } } }\n"
                             "  - wait_until: { Body.level: 20 }\n    timeout_ms: 100\n"
                             "  - wave: { Body.level: { pulse: { value: 70, width_ms: 5 } } }\n"),
               "W 5 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 ms): "
               "this step begins at 107 ms, so the ECU has the pulse's value for one whole step of 10 ms (the one at 110 ms), not for 5 "
               "ms. Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 7 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 ms): "
               "this step begins at 201 ms, and none of the ECU's steps is from then until the pulse ends (206 ms), so the ECU never gets "
               "the pulse's value. Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 9 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 ms): "
               "this step begins at 305 ms, and none of the ECU's steps is from then until the pulse ends (310 ms), so the ECU never gets "
               "the pulse's value. Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 12 scenario-wave-undersampled: the pulse of Body.level is 5 ms wide, shorter than the step of participant 'Body' (10 "
               "ms): the ECU has the pulse's value for one whole step if one of its steps is from the pulse's beginning until its end, "
               "and never otherwise; when this step begins is not known before the run. Make width_ms 10 or more; to run it as it is "
               "without this message, add undersampled: ok to the step\n");
}

TEST_CASE(scenario_points_that_are_kept_may_jump_and_a_point_the_ecu_never_gets_is_said)
{
    // interpolate: hold is how a value jumps: points 1 ms apart say nothing, as long as each is
    // written once (Body steps every 10 ms; the wave writes at 0, 10, 20, ... ms of the step).
    check_text(wave_findings("  - wave: { Body.level: { points: [[0, 0], [20, 5], [21, 60], [40, 0]], interpolate: hold } }\n"), "");
    World world;
    CHECK(world.run(read(kHead + "  - wave: { Body.level: { points: [[0, 0], [20, 60], [40, 0]], interpolate: hold } }\n")).status ==
          ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "0@0 0@10 60@20 60@30 0@40 ");
    // A point that no write carries; a point before the end whose one write may meet the end's
    // in one step of the ECU: at a time that is known it does (101 ms) or does not (100 ms), and
    // after a wait that is not known.
    check_text(wave_findings("  - wave: { Body.level: { points: [[0, 0], [21, 5], [25, 6], [40, 0]], interpolate: hold } }\n"
                             "  - at_ms: 100\n    wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"
                             "  - at_ms: 201\n    wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"
                             "  - wait_until: { Body.level: 6 }\n    timeout_ms: 100\n"
                             "  - wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"),
               "W 3 scenario-wave-undersampled: the points of Body.level keep point 2 from 21 ms to 25 ms, and participant 'Body' takes "
               "its inputs once per step of 10 ms: the wave writes a value every 10 ms from the step's beginning, and none of those "
               "times is in that span, so the value of point 2 is never applied. Put the points at multiples of 10 ms; to run it as it "
               "is without this message, add undersampled: ok to the step\n"
               "W 7 scenario-wave-undersampled: the points of Body.level keep point 2 from 12 ms to 25 ms, where they end, and participant "
               "'Body' takes its inputs once per step of 10 ms: this step begins at 201 ms, and none of the ECU's steps is from 221 ms "
               "(when the value of point 2 is written) until the points end (226 ms), so that value is never applied. Put the points at "
               "multiples of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 10 scenario-wave-undersampled: the points of Body.level keep point 2 from 12 ms to 25 ms, where they end, and "
               "participant 'Body' takes its inputs once per step of 10 ms: the value of point 2 is applied only if one of the ECU's "
               "steps is from 20 ms to 25 ms after this step begins, and when it begins is not known before the run. Put the points at "
               "multiples of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n");
    // The span of a point ends where the next begins: a write at that time carries the next.
    check_text(wave_findings("  - wave: { Body.level: { points: [[0, 0], [15, 5], [20, 6], [40, 0]], interpolate: hold } }\n"),
               "W 3 scenario-wave-undersampled: the points of Body.level keep point 2 from 15 ms to 20 ms, and participant 'Body' takes "
               "its inputs once per step of 10 ms: the wave writes a value every 10 ms from the step's beginning, and none of those "
               "times is in that span, so the value of point 2 is never applied. Put the points at multiples of 10 ms; to run it as it "
               "is without this message, add undersampled: ok to the step\n");
    // What the check says of 201 ms is what happens: 5 and 6 are both applied at 230 ms.
    World late;
    CHECK(late.run(read(kHead + "  - at_ms: 201\n    wave: { Body.level: { points: [[0, 0], [12, 5], [25, 6]], interpolate: hold } }\n"
                                "    undersampled: ok\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(late, "Body.level"), "0@210 0@220 5@230 6@230 ");
}

TEST_CASE(scenario_wave_takes_parameters_where_a_number_stands)
{
    ScenarioContext context;
    context.params = {param("amp", "3", ParamSource::CommandLine), param("period", "80", ParamSource::CommandLine),
                      param("top", "9", ParamSource::CommandLine)};
    const ScenarioRead r = read_scenario(
        kHead + "  - wave: { Body.level: { triangle: { amplitude: \"${amp}\", period_ms: \"${period}\" }, around: 20, for_ms: \"${period}\" } }\n"
                "  - wave: { Body.level: { points: [[0, 1], [20, \"${top}\"]] } }\n",
        catalog(), context);
    check_text(lines(r.findings), "");
    check_text(r.scenario->steps[0].text + "\n" + r.scenario->steps[1].text + "\n",
               "wave Body.level triangle amplitude 3 period 80 ms around 20 for 80 ms\n"
               "wave Body.level points (2, linear) over 20 ms\n");
    World world;
    CHECK(world.run(*r.scenario).status == ScenarioStatus::Passed);
    // (21.5 and 18.5 are rounded away from zero, as a ramp's values are.)
    check_text(applied(world, "Body.level"), "20@0 22@10 23@20 22@30 20@40 19@50 17@60 19@70 20@80 1@80 5@90 9@100 ");
    // A parameter that does not fit says where it comes from.
    context.params[0] = param("amp", "big", ParamSource::CommandLine);
    check_text(lines(read_scenario(kHead + "  - wave: { Body.level: { sine: { amplitude: \"${amp}\", period_ms: 80 }, for_ms: 80 } }\n",
                                   catalog(), context)
                         .findings),
               "E 3 value: amplitude of the sine of Body.level must be a number (how far the value goes above and below the middle), "
               "not 'big' (the parameter amp, from --param)\n");
}

TEST_CASE(scenario_wave_errors_say_what_is_wrong_and_what_to_write)
{
    // The form.
    check_text(wave_findings("  - wave: 5\n"),
               "E 3 value: wave must be a mapping of variables to a shape and how long it lasts ({ Body.level: { sine: { amplitude: 1, "
               "period_ms: 100 }, for_ms: 1000 } }), not '5'\n");
    check_text(wave_findings("  - wave: { Body.level: { for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 100 }, square: { period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sinus: { amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 100 } } }\n"
                             "  - wave: { Body.level: { sine: 5, for_ms: 100 } }\n"),
               "E 3 value: wave of Body.level names no shape: give it one of sine, triangle, sawtooth, square, pulse, noise, chirp, lag, "
               "chatter, points\n"
               "E 4 value: wave of Body.level has sine, square: a variable takes one shape in a step; make a step of each\n"
               "E 5 unknown-key: wave of Body.level: unknown key 'sinus': did you mean 'sine'? (the keys here: sine, triangle, sawtooth, "
               "square, pulse, noise, chirp, lag, chatter, points, around, for_ms, interpolate)\n"
               "E 5 value: wave of Body.level names no shape: give it one of sine, triangle, sawtooth, square, pulse, noise, chirp, lag, "
               "chatter, points\n"
               "E 6 missing-key: wave of Body.level needs for_ms: how long the sine lasts, in milliseconds of virtual time\n"
               "E 7 value: the sine of Body.level must be a mapping of its values ({ amplitude: 1, period_ms: 100 }), not '5'\n");
    // What cannot run: a period or a width of 0, no seed, times that do not go up.
    check_text(wave_findings("  - wave: { Body.level: { sine: { amplitude: 1, period_ms: 0 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, frequency_hz: -2 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 1, frequency_hz: 5, period_ms: 200 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: -1 }, for_ms: 0 } }\n"
                             "  - wave: { Body.level: { pulse: { value: 1, width_ms: 0 } } }\n"
                             "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 10 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 1], [50, 2], [50, 3], [40, 4]] } }\n"
                             "  - wave: { Body.level: { points: [[0, 1]] } }\n"
                             "  - wave: { Body.level: { points: [[0, 1, 2], 5] } }\n"
                             "  - wave: { Body.level: { square: { low: 1, high: 2, period_ms: 100, duty: 101 }, for_ms: 100 } }\n"),
               "E 3 value: period_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time of one period)\n"
               "E 4 value: frequency_hz of the sine of Body.level is -2, but it must be from 1e-09 to 1e+09 (periods per second of virtual "
               "time)\n"
               "E 5 value: the sine of Body.level has period_ms and frequency_hz: give one of them (frequency_hz: 50 is period_ms: 20)\n"
               "E 6 value: amplitude of the sine of Body.level is -1, but it must be from 0 (how far the value goes above and below the "
               "middle)\n"
               "E 6 missing-key: the sine of Body.level needs period_ms or frequency_hz: how long one period is, or how many there are in "
               "a second\n"
               "E 6 value: for_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time the wave takes)\n"
               "E 7 value: width_ms is 0, but it must be from 1 to 1000000000000 (milliseconds of virtual time the pulse lasts)\n"
               "E 8 missing-key: the noise of Body.ratio needs seed: a whole number that picks the values; the same seed gives the same "
               "values in every run, so that a failure can be run again\n"
               "E 9 value: the times of the points of Body.level must go up: point 3 is at 50 ms, not after point 2 (50 ms); for a "
               "value that jumps, write interpolate: hold beside the points (each value is then kept until the next point)\n"
               "E 9 value: the times of the points of Body.level must go up: point 4 is at 40 ms, not after point 3 (50 ms)\n"
               "E 10 value: the points of Body.level end at 0 ms: a wave takes time; give a later point, or use set for a value at one "
               "time\n"
               "E 11 value: point 1 of the points of Body.level must be [<milliseconds since the step began>, <value>], not a list\n"
               "E 11 value: point 2 of the points of Body.level must be [<milliseconds since the step began>, <value>], not '5'\n"
               "E 12 value: duty of the square of Body.level is 101, but it must be from 0 to 100 (the percent of each period for which it "
               "is high)\n");
    // A shape that does not fit the variable's type.
    check_text(wave_findings("  - wave: { Door.lamp: { sine: { amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Door.mode: { square: { period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Door.lamp: { square: { low: 0, high: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Door.lamp: { points: [[0, true], [50, false]], interpolate: linear } }\n"
                             "  - wave: { Door.mode: { points: [[0, auto], [50, open]] } }\n"
                             "  - wave: { Door.lamp: { chatter: { to: 1, bounces: 2, interval_ms: 10 } } }\n"
                             "  - wave: { Body.level_changed_at_ms: { pulse: { value: 1, width_ms: 20 } } }\n"
                             "  - wave: { Body.gain: { pulse: { value: 1, width_ms: 20 } } }\n"),
               "E 3 scenario-value: sine needs a number, and Door.lamp is a bool: for a bool use square, pulse, chatter or points\n"
               "E 4 scenario-value: square needs a number, and Door.mode is an enum: for an enum use points, or set\n"
               "E 5 value: low does not go with the square of Door.lamp: the square of a bool is false and true; remove it\n"
               "E 5 value: high does not go with the square of Door.lamp: the square of a bool is false and true; remove it\n"
               "E 6 scenario-value: interpolate: linear needs a number, and Door.lamp is a bool: write interpolate: hold (each value is "
               "kept until the next point), or leave it out\n"
               "E 7 scenario-value: the value of point 2 of the points of Door.mode (enum) must be one of its items, by name or by value, "
               "not 'open': the items are off (0), on (1), auto (2)\n"
               "E 8 scenario-value: to of the chatter of Door.lamp (bool) must be true or false, not '1'\n"
               "E 9 scenario-not-writable: Body.level_changed_at_ms is an output of its ECU: it can be read (expect, wait_until), not "
               "written; write one of the ECU's inputs instead\n"
               "E 10 scenario-not-writable: Body.gain is a parameter: it can be written only before the first step, that is with set in a "
               "step at 0 (at_ms: 0, or the first steps without a time), not with wave\n");
    // Outside the range, as far as the text says where the wave goes.
    check_text(wave_findings("  - wave: { Body.level: { sine: { amplitude: 5, period_ms: 100 }, around: 98, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { square: { low: -1, high: 101, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { sine: { amplitude: 5, period_ms: 100 }, around: 120, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 0], [100, 500]] } }\n"
                             "  - wave: { Body.level: { lag: { to: 101, tau_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { noise: { amplitude: 50, hold_ms: 10, seed: 1, distribution: gaussian }, around: 50, for_ms: 100 } }\n"),
               "E 3 scenario-value: the sine of Body.level goes from 93 to 103 (around 98, amplitude 5), outside its range (from 0 to "
               "100): lower the amplitude, or move around\n"
               "E 4 scenario-value: low of the square of Body.level is -1, outside its range (from 0 to 100)\n"
               "E 4 scenario-value: high of the square of Body.level is 101, outside its range (from 0 to 100)\n"
               "E 5 scenario-value: around of the sine of Body.level is 120, outside its range (from 0 to 100)\n"
               "E 6 scenario-value: the value of point 2 of the points of Body.level is 500, outside its range (from 0 to 100)\n"
               "E 7 scenario-value: to of the lag of Body.level is 101, outside its range (from 0 to 100)\n");
    // A chirp may begin or end at 0 Hz on a linear sweep, not on a log one, and it has to move.
    check_text(wave_findings("  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 0, to_hz: 5 }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 0, to_hz: 5, sweep: log }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 5, to_hz: 0, sweep: log }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 0, to_hz: 0 }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: -1, to_hz: 5 }, around: 0.5, for_ms: 1000 } }\n"),
               "E 4 value: from_hz of the chirp of Body.ratio is 0, but a log sweep changes the frequency by the same factor every "
               "second, which neither begins nor ends at 0 Hz: give a frequency above 0, or write sweep: linear\n"
               "E 5 value: to_hz of the chirp of Body.ratio is 0, but a log sweep changes the frequency by the same factor every "
               "second, which neither begins nor ends at 0 Hz: give a frequency above 0, or write sweep: linear\n"
               "E 6 value: the chirp of Body.ratio goes from 0 Hz to 0 Hz, which does not move: give from_hz or to_hz a frequency above "
               "0\n"
               "E 7 value: from_hz of the chirp of Body.ratio is -1, but it must be from 0 to 1e+09 (the frequency at the beginning)\n");
    // Keys that do not go together.
    check_text(wave_findings("  - wave: { Body.level: { pulse: { value: 1, width_ms: 20 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 1], [20, 2]], for_ms: 100, around: 5 } }\n"
                             "  - wave: { Body.level: { square: { low: 1, high: 2, amplitude: 1, period_ms: 100 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { square: { low: 1, high: 2, period_ms: 100 }, around: 5, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { lag: { to: 1, tau_ms: 100 }, around: 5, for_ms: 100, interpolate: hold } }\n"
                             "  - wave: { Body.level: { chatter: { to: 1, bounces: 2, interval_ms: 10 }, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { chatter: { to: 1, interval_ms: 10 } } }\n"
                             "  - set: { Body.level: 1 }\n"
                             "    undersampled: ok\n"
                             "  - wave: { Body.level: { pulse: { value: 1, width_ms: 20 } } }\n"
                             "    undersampled: yes\n"
                             "  - wave: { Body.level: { pulse: { value: 1, width_ms: 20 }, around: 5 } }\n"),
               "E 3 value: for_ms does not go with pulse: a pulse lasts its width_ms; remove it\n"
               "E 4 value: for_ms does not go with points: the points end at the last of them; remove it\n"
               "E 4 value: around does not go with the points of Body.level: the points are the values themselves (for where it begins, "
               "start with a point at 0: [0, <value>]); remove it\n"
               "E 5 value: the square of Body.level has amplitude and low: give low and high (its two values), or amplitude (so far above "
               "and below the middle)\n"
               "E 6 value: around does not go with the square of Body.level: low and high are the values themselves; remove it\n"
               "E 7 value: interpolate goes with points, not with lag: remove it\n"
               "E 7 value: around does not go with the lag of Body.level: the value it starts from is its from (lag: { from: <value>, ... "
               "}); remove it\n"
               "E 8 value: the chatter of Body.level has bounces and for_ms: give one of them (bounces: how often it falls back; for_ms: "
               "how long it bounces)\n"
               "E 9 missing-key: the chatter of Body.level needs bounces (how often it falls back before it rests) or for_ms (how long it "
               "bounces)\n"
               "E 11 value: undersampled goes with wave and dither, not with set: remove it\n"
               "E 13 value: undersampled must be ok (run the step's waves as they are, also where they change faster than their ECU "
               "steps), not 'yes': write undersampled: ok, or remove it\n"
               "E 14 value: around does not go with the pulse of Body.level: a pulse returns to the value the variable has when the "
               "pulse begins (for another value after it, write a set in the next step); remove it\n");
    // A wave ends where it says: the step after it may not be earlier.
    check_text(wave_findings("  - wave: { Body.level: { pulse: { value: 1, width_ms: 100 } } }\n  - at_ms: 99\n    expect: { Body.level: 5 }\n"),
               "E 4 scenario-time: at_ms is 99, before the end of the step before it (100 ms): the steps run in the order of the file, "
               "and time does not go back; reorder the steps or raise at_ms\n");
}

TEST_CASE(scenario_wave_that_its_ecu_cannot_see_gets_a_warning_that_says_what_the_ecu_sees)
{
    // Body steps every 10 ms. A period under two steps; a smooth shape with few steps a period.
    check_text(wave_findings("  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 5 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { triangle: { amplitude: 0.1, frequency_hz: 40 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 80 }, around: 0.5, for_ms: 100 } }\n"),
               "W 3 scenario-wave-undersampled: the sine of Body.ratio has a period of 5 ms, but participant 'Body' takes its inputs once "
               "per step of 10 ms: with fewer than two values per period the ECU sees a slower shape or a constant, not this one. Make "
               "the period 80 ms or more (8 values per period); to run it as it is without this message, add undersampled: ok to the "
               "step\n"
               "N 4 scenario-wave-coarse: the triangle of Body.ratio has a period of 25 ms, and participant 'Body' takes its inputs once "
               "per step of 10 ms: the ECU sees 2.5 values per period, so it can miss the highest and the lowest. A period of 80 ms or "
               "more gives it 8; to run it as it is without this message, add undersampled: ok to the step\n");
    // A period of two steps: a sine and a triangle that begin at their middle are there at every
    // step of the ECU, which so sees a constant. Begun elsewhere they are two values (a note), and
    // a sawtooth is two values wherever it begins. When the scenario's step begins does not matter.
    check_text(wave_findings("  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 20 }, around: 0.5, for_ms: 100 } }\n"
                             "  - at_ms: 203\n"
                             "    wave: { Body.ratio: { triangle: { amplitude: 0.1, frequency_hz: 50, phase_deg: 180 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 20, phase_deg: 90 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.ratio: { sawtooth: { amplitude: 0.1, period_ms: 20 }, around: 0.5, for_ms: 100 } }\n"),
               "W 3 scenario-wave-undersampled: the sine of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs "
               "once per step of 10 ms: at each of those times the sine is at its middle, so the ECU sees a constant, not the sine. Make "
               "the period 80 ms or more (8 values per period); to run it as it is without this message, add undersampled: ok to the "
               "step\n"
               "W 5 scenario-wave-undersampled: the triangle of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs "
               "once per step of 10 ms: at each of those times the triangle is at its middle, so the ECU sees a constant, not the "
               "triangle. Make the period 80 ms or more (8 values per period); to run it as it is without this message, add "
               "undersampled: ok to the step\n"
               "N 6 scenario-wave-coarse: the sine of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs once per "
               "step of 10 ms: the ECU sees 2 values per period, so it can miss the highest and the lowest. A period of 80 ms or more "
               "gives it 8; to run it as it is without this message, add undersampled: ok to the step\n"
               "N 7 scenario-wave-coarse: the sawtooth of Body.ratio has a period of 20 ms, and participant 'Body' takes its inputs once "
               "per step of 10 ms: the ECU sees 2 values per period, so it can miss the highest and the lowest. A period of 80 ms or "
               "more gives it 8; to run it as it is without this message, add undersampled: ok to the step\n");
    // What the check says is what is written: the middle at every step of the ECU.
    {
        World constant;
        CHECK(constant.run(read(kHead + "  - at_ms: 203\n"
                                        "    wave: { Body.level: { sine: { amplitude: 9, period_ms: 20 }, around: 50, for_ms: 40 } }\n"
                                        "    undersampled: ok\n"))
                  .status == ScenarioStatus::Passed);
        check_text(applied(constant, "Body.level"), "50@210 50@220 50@230 50@240 50@250 ");
    }
    // A square whose high time is not a number of steps: the duty the ECU sees.
    check_text(wave_findings("  - wave: { Door.lamp: { square: { period_ms: 30, duty: 25 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 25 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 40, duty: 10 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 40, duty: 25 }, for_ms: 300 },\n"
                             "            Body.lamp: { square: { period_ms: 40, duty: 0 }, for_ms: 300 }, Body.level: { square: { low: 0, high: 1, period_ms: 20, duty: 100 }, for_ms: 300 } }\n"
                             // Begun 5 ms into its period, the 15 ms that are high hold one step of the ECU, not two.
                             "  - wave: { Door.lamp: { square: { period_ms: 40, duty: 37.5, phase_deg: 45 }, for_ms: 300 } }\n"
                             "  - wave: { Door.lamp: { square: { period_ms: 15 }, for_ms: 300 } }\n"),
               "W 3 scenario-wave-duty: the square of Door.lamp is high for 7.5 ms of each period of 30 ms (duty 25 %), but participant "
               "'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (33.3 %). Make the high time "
               "(period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 4 scenario-wave-duty: the square of Door.lamp has a period of 25 ms, which is not a multiple of the step of participant "
               "'Door' (10 ms): the ECU sees high and low times that change from period to period. Make the period a multiple of 10 ms; "
               "to run it as it is without this message, add undersampled: ok to the step\n"
               "W 5 scenario-wave-duty: the square of Door.lamp is high for 4 ms of each period of 40 ms (duty 10 %), but participant "
               "'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (25 %). Make the high time "
               "(period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 8 scenario-wave-duty: the square of Door.lamp is high for 15 ms of each period of 40 ms (duty 37.5 %), but participant "
               "'Door' takes its inputs once per step of 10 ms: the ECU sees it high for 10 ms of each period (25 %). Make the high time "
               "(period x duty / 100) a multiple of 10 ms; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 9 scenario-wave-undersampled: the square of Door.lamp has a period of 15 ms, but participant 'Door' takes its inputs "
               "once per step of 10 ms: with fewer than two values per period the ECU sees a slower shape or a constant, not this one. "
               "Make the period 20 ms or more; to run it as it is without this message, add undersampled: ok to the step\n");
    // What is shorter than a step: a pulse, the hold of a noise, the gap of two points, the
    // interval of a chatter (half of it with a seed); and the top of a chirp.
    check_text(wave_findings("  - wave: { Door.lamp: { pulse: { width_ms: 3 } } }\n"
                             "  - wave: { Body.ratio: { noise: { amplitude: 0.1, hold_ms: 2, seed: 1 }, around: 0.5, for_ms: 100 } }\n"
                             "  - wave: { Body.level: { points: [[0, 0], [20, 5], [25, 6], [28, 7], [100, 0]] } }\n"
                             "  - wave: { Door.lamp: { chatter: { to: true, bounces: 3, interval_ms: 2 } } }\n"
                             "  - wave: { Door.lamp: { chatter: { to: true, bounces: 3, interval_ms: 15, seed: 1 } } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 1, to_hz: 200 }, around: 0.5, for_ms: 1000 } }\n"
                             "  - wave: { Body.ratio: { chirp: { amplitude: 0.1, from_hz: 20, to_hz: 1 }, around: 0.5, for_ms: 1000 } }\n"),
               "W 3 scenario-wave-undersampled: the pulse of Door.lamp is 3 ms wide, shorter than the step of participant 'Door' (10 ms): "
               "this step begins at 0 ms, so the ECU has the pulse's value for one whole step of 10 ms (the one at 0 ms), not for 3 ms. "
               "Make width_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 4 scenario-wave-undersampled: the noise of Body.ratio changes every 2 ms, faster than the step of participant 'Body' "
               "(10 ms): the ECU gets only the value at the time of each of its steps; the values between are never applied. Make "
               "hold_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 5 scenario-wave-undersampled: the points of Body.level are as little as 3 ms apart (at 25 ms and 28 ms), closer "
               "than the step of participant 'Body' (10 ms): the ECU gets only the value at the time of each of its steps, so it does not see every "
               "point. Put the points 10 ms or more apart; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 6 scenario-wave-undersampled: the chatter of Door.lamp changes every 2 ms, faster than the step of participant 'Door' "
               "(10 ms): the ECU gets only the value at the time of each of its steps, so it sees some of the bounces or none. Make "
               "interval_ms 10 or more; to run it as it is without this message, add undersampled: ok to the step\n"
               "W 7 scenario-wave-undersampled: the chatter of Door.lamp changes as little as 7.5 ms apart (interval_ms 15 with a seed: a "
               "half to one and a half intervals), closer than the step of participant 'Door' (10 ms): the ECU gets only the value at the "
               "time of each of its steps, so it sees some of the bounces or none. Make interval_ms 20 or more; to run it as it is "
               "without this message, add undersampled: ok to the step\n"
               "W 8 scenario-wave-undersampled: the chirp of Body.ratio goes up to 200 Hz (a period of 5 ms), but participant 'Body' takes "
               "its inputs once per step of 10 ms: above 50 Hz (two steps per period) the ECU sees a slower shape, not the sweep. Keep "
               "from_hz and to_hz at 50 or less; to run it as it is without this message, add undersampled: ok to the step\n"
               "N 9 scenario-wave-coarse: the chirp of Body.ratio goes up to 20 Hz (a period of 50 ms), and participant 'Body' takes its "
               "inputs once per step of 10 ms: at that frequency the ECU sees 5 values per period, so it can miss the highest and the "
               "lowest. Up to 12.5 Hz it gets 8 or more; to run it as it is without this message, add undersampled: ok to the step\n");
    // The step of the ECU decides: the same waves for an ECU of 1 ms say nothing.
    check_text(wave_findings("  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 8 }, around: 0.5, for_ms: 100 }, Door.lamp: { pulse: { width_ms: 3 } } }\n",
                             1ms, 1ms),
               "");
    // undersampled: ok on the step: its waves run as they are, without a word; and they do run.
    const std::string quiet = "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 5 }, around: 0.5, for_ms: 20 }, Door.lamp: { square: { period_ms: 30, duty: 25 }, for_ms: 60 } }\n"
                              "    undersampled: ok\n";
    check_text(wave_findings(quiet), "");
    World world;
    CHECK(world.run(read(kHead + quiet)).status == ScenarioStatus::Passed);
    // A warning does not keep the scenario from running: it is read.
    CHECK(read_scenario(kHead + "  - wave: { Door.lamp: { pulse: { width_ms: 3 } } }\n", catalog()).scenario.has_value());
    // A participant whose step is not known: said, not guessed.
    ScenarioCatalog unknown = catalog();
    unknown[0].step = SimDuration{};
    check_text(lines(read_scenario(kHead + "  - wave: { Body.ratio: { sine: { amplitude: 0.1, period_ms: 5 }, around: 0.5, for_ms: 20 } }\n", unknown).findings),
               "N 3 scenario-wave-not-checked: whether participant 'Body' can see the sine of Body.ratio as it is written is not "
               "checked: the participant's step is not known\n");
}

TEST_CASE(scenario_times_are_the_same_whether_a_wave_is_skipped_or_not)
{
    // What a wave takes is in its text (for_ms, a pulse's width, the last point, the bounces): a
    // configuration without the variable gives the steps after it the same times.
    const std::string text = kHead + "  - set: { Body.level: 0 }\n"
                                     "  - wave: { Body.boost: { sine: { amplitude: 1, period_ms: 100 }, around: 5, for_ms: 100 }, Body.ratio: { pulse: { value: 1, width_ms: 40 } } }\n"
                                     "  - wave: { Body.boost: { pulse: { value: 1, width_ms: 30 } } }\n"
                                     "  - wave: { Body.boost: { points: [[0, 1], [20, 2], [70, 3]] } }\n"
                                     "  - wave: { Body.boost: { chatter: { to: 1, bounces: 3, interval_ms: 10 } } }\n"
                                     "  - set: { Body.lamp: true }\n";
    ScenarioCatalog with = catalog();
    auto& boost = with[0].variables.emplace_back();
    boost.name = "boost";
    boost.type = control::ValueType::Integer;
    boost.causality = control::Causality::Input;
    const ScenarioRead there = read_scenario(text, with);
    const ScenarioRead absent = read_scenario(text, catalog_without_boost());
    CHECK(there.scenario.has_value() && absent.scenario.has_value());
    CHECK_EQ(absent.skipped, std::size_t{4});
    CHECK_EQ(absent.scenario->steps[1].skipped_ms, std::int64_t{100});
    CHECK_EQ(absent.scenario->steps[2].skipped_ms, std::int64_t{30});
    CHECK_EQ(absent.scenario->steps[3].skipped_ms, std::int64_t{70});
    CHECK_EQ(absent.scenario->steps[4].skipped_ms, std::int64_t{60});
    check_text(absent.scenario->steps[1].text + "\n", "wave Body.boost; Body.ratio pulse 1 width 40 ms\n");
    World a;
    const ScenarioRun skipped = a.run(*absent.scenario);
    CHECK(skipped.status == ScenarioStatus::Passed);
    check_text(starts_of(skipped), "3:0 8:260 ");
    CHECK(skipped.steps[4].start == at(200ms) && skipped.steps[4].end == at(260ms) && !skipped.steps[4].at);
    // Nothing of a skipped step is written, also not the wave of the variable that is there.
    check_text(applied(a, "Body.ratio"), "");
}

// --- points from a CSV file ---

namespace {

/// A ScenarioContext whose files are `files` (by the path the scenario writes); `reads` counts the
/// reads of each.
ScenarioContext with_files(std::map<std::string, std::string> files, std::shared_ptr<std::map<std::string, int>> reads = {})
{
    ScenarioContext context;
    context.read_file = [files = std::move(files), reads](const std::string& path, std::string& problem) -> std::optional<std::string> {
        if (reads)
        {
            ++(*reads)[path];
        }
        const auto it = files.find(path);
        if (it == files.end())
        {
            problem = "does not exist";
            return std::nullopt;
        }
        return it->second;
    };
    return context;
}

} // namespace

TEST_CASE(scenario_points_from_a_file_write_what_the_same_points_in_the_text_write)
{
    // The same points in the text and in two files: a byte order mark, CRLF, a blank line, a name
    // in quotes, spaces around a cell, hexadecimal, an enum by name and by value, a bool as 0, the
    // times in seconds in a column that is not the first, one file for two variables.
    const std::string first = "  - set: { Body.ratio: 0, Door.lamp: false }\n";
    const std::string text =
        kHead + first +
        "  - wave: { Body.level: { points: [[0, 0], [50, 25.5], [80, 0x3C], [400, 20]] }, Door.mode: { points: [[0, auto], [50, off], [80, 1], [400, on]] },\n"
        "            Door.lamp: { points: [[10, true], [30, false]] },\n"
        "            Body.ratio: { points: [[10, 0.25], [30, 1]] } }\n"
        "  - expect: { Body.level: 20, Door.mode: on, Door.lamp: false, Body.ratio: 1 }\n";
    const std::string fromFiles =
        kHead + first +
        "  - wave: { Body.level: { points: { file: traces/drive.csv, value: level } }, Door.mode: { points: { file: traces/drive.csv, value: mode, time: time_ms } },\n"
        "            Door.lamp: { points: { file: lamp.csv, value: lamp, time: t, time_unit: s } },\n"
        "            Body.ratio: { points: { file: lamp.csv, value: ratio, time: t, time_unit: s } } }\n"
        "  - expect: { Body.level: 20, Door.mode: on, Door.lamp: false, Body.ratio: 1 }\n";
    const auto reads = std::make_shared<std::map<std::string, int>>();
    const ScenarioContext context = with_files({{"traces/drive.csv", "\xEF\xBB\xBF\"time_ms\", level ,mode\r\n0,0,auto\r\n\r\n50,25.5,off\r\n"
                                                                     "80,0x3C,1\r\n400,20,\"on\"\r\n"},
                                                {"lamp.csv", "lamp,t,ratio\ntrue,0.01,0.25\n0,0.03,1\n"}},
                                               reads);
    const ScenarioRead read = read_scenario(fromFiles, catalog(), context);
    check_text(lines(read.findings), "");
    CHECK(read.scenario.has_value());
    check_text(read.scenario->steps[1].text + "\n",
               "wave Body.level points from traces/drive.csv column level (4, linear) over 400 ms; Door.mode points from "
               "traces/drive.csv column mode (4, hold) over 400 ms; Door.lamp points from lamp.csv column lamp (2, hold) over 30 ms; "
               "Body.ratio points from lamp.csv column ratio (2, linear) over 30 ms\n");
    // Each file is read once.
    CHECK(*reads == (std::map<std::string, int>{{"lamp.csv", 1}, {"traces/drive.csv", 1}}));
    World inText;
    World inFiles;
    const ScenarioRun a = inText.run(read_scenario(text, catalog()).scenario.value());
    const ScenarioRun b = inFiles.run(*read.scenario);
    CHECK(a.status == ScenarioStatus::Passed && b.status == ScenarioStatus::Passed);
    check_text(text_of(b), text_of(a));
    for (const char* variable : {"Body.level", "Door.mode", "Door.lamp", "Body.ratio"})
    {
        check_text(applied(inFiles, variable), applied(inText, variable));
    }
    // 25.5 at 50 ms, 60 at 80 ms, then 1.25 less every 10 ms to 20 at 400 ms (rounded for the integer).
    check_text(applied(inFiles, "Body.level"), "0@0 5@10 10@20 15@30 20@40 26@50 37@60 49@70 60@80 59@90 58@100 56@110 55@120 "
                                               "54@130 53@140 51@150 50@160 49@170 48@180 46@190 45@200 44@210 43@220 41@230 40@240 "
                                               "39@250 38@260 36@270 35@280 34@290 33@300 31@310 30@320 29@330 28@340 26@350 25@360 "
                                               "24@370 23@380 21@390 20@400 ");
}

TEST_CASE(scenario_points_file_errors_say_the_file_the_line_and_the_column)
{
    std::string big = "time,level\n";
    for (int i = 0; i <= 100'000; ++i)
    {
        big += std::to_string(i) + ",1\n";
    }
    const ScenarioContext context =
        with_files({{"ok.csv", "time,level\n0,1\n50,2\n"},
                    {"twice.csv", "time,level,level\n0,1,2\n10,1,2\n"},
                    {"cells.csv", "time,level\n0,1\n10,this-is-not-a-number-at-all\n"},
                    {"times.csv", "time,level\n0,1\n12.5,2\n"},
                    {"seconds.csv", "time,level\n0,1\nsoon,2\n"},
                    {"negative.csv", "time,level\n-5,1\n10,2\n"},
                    {"order.csv", "time,level\n0,1\n50,2\n50,3\n"},
                    {"range.csv", "time,level\n0,1\n10,500\n"},
                    {"empty.csv", "\xEF\xBB\xBF\r\n  \r\n"},
                    {"header.csv", "time,level\r\n"},
                    {"ragged.csv", "time,level\n0,1\n10\n"},
                    {"quote.csv", "time,level\n0,\"1\n"},
                    {"utf16.csv", std::string("\xFF\xFEt\0", 4)},
                    {"door.csv", "time,lamp,mode\n0,yes,open\n10,1,1\n"},
                    {"big.csv", big}});
    const auto findings = [&](const std::string& steps, const ScenarioContext& c) { return lines(read_scenario(kHead + steps, catalog(), c).findings); };
    // The path: from the scenario's directory, below it, with '/'.
    check_text(findings("  - wave: { Body.level: { points: { file: /data/ok.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: 'C:/data/ok.csv', value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: traces/../../ok.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: 'traces\\ok.csv', value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: none.csv, value: level } } }\n",
                        context),
               "E 3 scenario-points-file: the points of Body.level: the file '/data/ok.csv' is an absolute path: give the path from the "
               "scenario file's directory (traces/drive.csv); a scenario reads only files in its own directory and below it\n"
               "E 4 scenario-points-file: the points of Body.level: the file 'C:/data/ok.csv' is an absolute path: give the path from the "
               "scenario file's directory (traces/drive.csv); a scenario reads only files in its own directory and below it\n"
               "E 5 scenario-points-file: the points of Body.level: the file 'traces/../../ok.csv' goes up a directory (..): a scenario "
               "reads only files in its own directory and below it; put the file there\n"
               "E 6 scenario-points-file: the points of Body.level: the file 'traces\\ok.csv' has '\\': write '/' between directories "
               "(traces/drive.csv), which every OS reads\n"
               "E 7 scenario-points-file: the points of Body.level: none.csv does not exist\n");
    // The keys.
    check_text(findings("  - wave: { Body.level: { points: { file: ok.csv } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: level, unit: s } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: level, time_unit: min } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: [level] } } }\n"
                        "  - wave: { Body.level: { points: 5 } }\n",
                        context),
               "E 3 missing-key: the points of Body.level from a file need value: the name of the column of the values (points: { file: "
               "traces/drive.csv, value: throttle })\n"
               "E 4 unknown-key: the points of Body.level: unknown key 'unit': remove it, or use one of the keys here (file, value, time, "
               "time_unit)\n"
               "E 5 value: time_unit of the points of Body.level must be ms or s, not 'min'\n"
               "E 6 value: value of the points of Body.level must be the name of the column of the values, not a list\n"
               "E 7 value: points of Body.level must be a list of [<milliseconds since the step began>, <value>] ([[0, 0], [100, 50]]), "
               "or a CSV file and its column (points: { file: traces/drive.csv, value: throttle }), not '5'\n");
    // The columns.
    check_text(findings("  - wave: { Body.level: { points: { file: ok.csv, value: levl } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: level, time: when } } }\n"
                        "  - wave: { Body.level: { points: { file: twice.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: ok.csv, value: time } } }\n",
                        context),
               "E 3 scenario-points-file: the points of Body.level: ok.csv has no column 'levl' on its first line (line 1): did you mean "
               "'level'?; its columns are 'time', 'level'\n"
               "E 4 scenario-points-file: the points of Body.level: ok.csv has no column 'when' on its first line (line 1); its columns "
               "are 'time', 'level'\n"
               "E 5 scenario-points-file: the points of Body.level: twice.csv has 2 columns named 'level' (columns 2 and 3): give its "
               "columns different names on its first line\n"
               "E 6 scenario-points-file: the points of Body.level: value names the column of the times ('time', the first column): name "
               "the column of the values, or the column of the times with time\n");
    // The cells: the first wrong one, cut short.
    check_text(findings("  - wave: { Body.level: { points: { file: cells.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: times.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: seconds.csv, value: level, time_unit: s } } }\n"
                        "  - wave: { Body.level: { points: { file: negative.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: order.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: range.csv, value: level } } }\n"
                        "  - wave: { Door.lamp: { points: { file: door.csv, value: lamp } } }\n"
                        "  - wave: { Door.mode: { points: { file: door.csv, value: mode } } }\n",
                        context),
               "E 3 scenario-value: the value on line 3 of cells.csv (column 'level') for Body.level (integer) must be a number, not "
               "'this-is-not-a-number...'\n"
               "E 4 value: the time on line 3 of times.csv (column 'time') is 12.5 ms: the times of points are whole milliseconds; round "
               "them in the file\n"
               "E 5 value: the time on line 3 of seconds.csv (column 'time') must be a number of seconds since the step began, not 'soon'\n"
               "E 6 value: the time on line 2 of negative.csv (column 'time') is -5 ms, but it must be from 0 to 1000000000000 "
               "(milliseconds of virtual time since the step began)\n"
               "E 7 value: the times of the points of Body.level must go up: line 4 of order.csv is at 50 ms, not after line 3 (50 ms); "
               "for a value that jumps, write interpolate: hold beside the points (each value is then kept until the next point)\n"
               "E 8 scenario-value: the value on line 3 of range.csv (column 'level') for Body.level (integer) is 500, outside its range "
               "(from 0 to 100)\n"
               "E 9 scenario-value: the value on line 2 of door.csv (column 'lamp') for Door.lamp (bool) must be true, false, 1 or 0, "
               "not 'yes'\n"
               "E 10 scenario-value: the value on line 2 of door.csv (column 'mode') for Door.mode (enum) must be one of its items, by "
               "name or by value, not 'open': the items are off (0), on (1), auto (2)\n");
    // The file as a whole.
    check_text(findings("  - wave: { Body.level: { points: { file: empty.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: header.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: ragged.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: quote.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: utf16.csv, value: level } } }\n"
                        "  - wave: { Body.level: { points: { file: big.csv, value: level } } }\n",
                        context),
               "E 3 scenario-points-file: the points of Body.level: empty.csv is empty: its first line names the columns (time,throttle), "
               "and each line below it gives a time and the values\n"
               "E 4 scenario-points-file: the points of Body.level: header.csv has no lines of values below the names of its columns "
               "(line 1): give one line for each point (0,50)\n"
               "E 5 scenario-points-file: the points of Body.level: ragged.csv has 1 cell on line 3, and its first line names 2 columns: "
               "give every line one cell for each column\n"
               "E 6 scenario-points-file: the points of Body.level: quote.csv has a cell in quotes on line 2 whose closing quote is not on "
               "the line: end a cell in quotes with a quote before the next ',' (write \"\" for a quote in it)\n"
               "E 7 scenario-points-file: the points of Body.level: utf16.csv is not UTF-8 text (it looks like UTF-16, or has bytes that "
               "are not UTF-8): save it as CSV in UTF-8\n"
               "E 8 scenario-points-file: the points of Body.level: big.csv has more than 100000 lines of values (one point each): keep "
               "fewer points, or split it into steps of their own\n");
    // Without a way to read files (read_scenario() without ScenarioContext::read_file).
    check_text(findings("  - wave: { Body.level: { points: { file: ok.csv, value: level } } }\n", ScenarioContext{}),
               "E 3 scenario-points-file: the points of Body.level: ok.csv is not read here: points from a file are read with the "
               "scenario file (check --scenario, run --scenario)\n");
    // A parameter may name the file and the column.
    check_text(findings("  - wave: { Body.level: { points: { file: \"${trace}\", value: \"${column}\" } } }\n"
                        "params: { trace: ok.csv, column: level }\n",
                        context),
               "");
}

TEST_CASE(scenario_points_file_of_a_skipped_wave_gives_its_length)
{
    // A skipped step lasts until the time on the last line of its file, as it would if it ran.
    const std::string text = kHead + "  - wave: { Body.boost: { points: { file: boost.csv, value: b, time: t } } }\n"
                                     "  - wave: { Body.boost: { points: { file: boost.csv, value: b, time: s, time_unit: s } } }\n"
                                     "  - wave: { Body.boost: { points: { file: none.csv, value: b } } }\n";
    const ScenarioRead absent =
        read_scenario(text, catalog_without_boost(), with_files({{"boost.csv", "b,t,s\n1,0,0\n2,70,0.25\n"}}));
    CHECK(absent.scenario.has_value());
    CHECK_EQ(absent.skipped, std::size_t{3});
    CHECK_EQ(absent.scenario->steps[0].skipped_ms, std::int64_t{70});
    CHECK_EQ(absent.scenario->steps[1].skipped_ms, std::int64_t{250});
    CHECK_EQ(absent.scenario->steps[2].skipped_ms, std::int64_t{0});
}

TEST_CASE(scenario_files_are_read_beside_the_scenario_file)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "points-files";
    fs::remove_all(dir);
    fs::create_directories(dir / "traces");
    // A name that is not ASCII: the path is UTF-8 on every OS.
    const std::string name = "traces/\xE8\xB5\xB0\xE8\xA1\x8C.csv";
    std::ofstream(dir / process::path_from_utf8(name), std::ios::binary) << "time,level\n0,1\n";
    const auto read = files_beside(dir / "s.yaml");
    std::string problem;
    CHECK(read(name, problem) == std::optional<std::string>{"time,level\n0,1\n"});
    CHECK(!read("traces", problem).has_value());
    check_text(problem, "is not a regular file: name the CSV file");
    CHECK(!read("none.csv", problem).has_value());
    CHECK(problem.rfind("does not exist (looked for ", 0) == 0);
    CHECK(problem.find("): the path is from the scenario file's directory; check it") != std::string::npos);
}

// --- an expect beside a ramp or a wave ---

TEST_CASE(scenario_expect_beside_a_ramp_is_read_with_the_ramp)
{
    const Scenario s = read(kHead + "  - set: { Body.level: 0 }\n"
                                    "  - ramp: { Body.level: { to: 10, over_ms: 100 } }\n"
                                    "    expect: { Body.level: { le: 10 }, Body.lamp: false }\n"
                                    "  - wave: { Door.ratio: { sine: { amplitude: 0.1, period_ms: 100 }, around: 0.5, for_ms: 200 } }\n"
                                    "    expect: { Door.ratio: { ge: 0.3 } }\n");
    CHECK_EQ(s.steps.size(), std::size_t{3});
    CHECK(s.steps[1].kind == StepKind::Ramp && s.steps[1].conditions.size() == 2 && s.steps[1].motions.size() == 1);
    CHECK(s.steps[2].kind == StepKind::Wave && s.steps[2].conditions.size() == 1);
    check_text(s.steps[1].text + "\n" + s.steps[2].text + "\n",
               "ramp Body.level to 10 over 100 ms, expecting Body.level le 10; Body.lamp eq false\n"
               "wave Door.ratio sine amplitude 0.1 period 100 ms around 0.5 for 200 ms, expecting Door.ratio ge 0.3\n");
    // It must be true all through the ramp: no within_ms or hold_for_ms; and only beside a ramp
    // or a wave.
    check_text(errors_only(read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                                 "    expect: { Body.level: { le: 10 } }\n"
                                                 "    within_ms: 50\n"
                                                 "  - wave: { Body.level: { pulse: { value: 1, width_ms: 50 } } }\n"
                                                 "    expect: { Body.level: { le: 10 } }\n"
                                                 "    hold_for_ms: 50\n"
                                                 "  - set: { Body.level: 1 }\n"
                                                 "    expect: { Body.level: 1 }\n",
                                         catalog())
                               .findings),
               "E 5 value: within_ms does not go with an expect beside a ramp: that expect must be true all through the ramp, "
               "from its time to its end; remove within_ms, or make the expect a step of its own after the ramp\n"
               "E 8 value: hold_for_ms does not go with an expect beside a wave: that expect must be true all through the "
               "wave, from its time to its end; remove hold_for_ms, or make the expect a step of its own after the wave\n"
               "E 9 value: the step has set, expect: a step does one thing; make a step of each\n");
}

namespace {

/// Runs `s` with every policy() and both orders of the ECUs, and returns its one result (text_of):
/// a test fails if two of them differ.
std::string one_result(const Scenario& s, const std::function<void(World&)>& setup = {})
{
    std::optional<std::string> first;
    for (const auto& [name, policy] : policies())
    {
        for (const bool reverse : {false, true})
        {
            World world;
            if (setup)
            {
                setup(world);
            }
            world.policy = policy;
            world.reverse = reverse;
            const std::string got = text_of(world.run(s));
            if (!first)
            {
                first = got;
            }
            check_text(got, *first, (name + (reverse ? ", the ECUs in reverse order" : "")).c_str());
        }
    }
    return *first;
}

} // namespace

TEST_CASE(scenario_expect_beside_a_ramp_holds_from_its_time_to_its_end_and_not_after)
{
    // Body.level goes 0, 1, ..., 9 at 100, 110, ..., 190 and 10 at 200, the end: each sample of
    // Body (every 10 ms) has the value written for its time. The set at 210 is after the end.
    const auto run = [](const std::string& expect) {
        return one_result(read(kHead + "  - set: { Body.level: 0 }\n"
                                       "  - at_ms: 100\n"
                                       "    ramp: { Body.level: { to: 10, over_ms: 100 } }\n"
                                       "    expect: { Body.level: " + expect + " }\n"
                                       "  - after_ms: 10\n"
                                       "    set: { Body.level: 50 }\n"));
    };
    // True at T, at every sample in (T, E] and not looked at after E.
    check_text(run("{ le: 10 }"), "passed over=220\n"
                                  "3 passed start=0 at=0 end=0\n"
                                  "4 passed start=100 at=200 end=200\n"
                                  "7 passed start=210 at=210 end=210\n");
    // The sample at T is looked at (the step at T applied the first write).
    check_text(run("{ ge: 1 }"), "failed over=110\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 failed start=100 at=100 end=200 scenario-failed: not true during the ramp (from 100 ms to 200 "
                                 "ms): at 100 ms Body.level ge 1, but it was 0 (its sample of 100 ms)\n"
                                 "7 not_run start=- at=- end=-\n");
    // And the sample at E, the last one of the ramp.
    check_text(run("{ le: 9 }"), "failed over=210\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 failed start=100 at=200 end=200 scenario-failed: not true during the ramp (from 100 ms to 200 "
                                 "ms): at 200 ms Body.level le 9, but it was 10 (its sample of 200 ms)\n"
                                 "7 unfinished start=210 at=- end=-\n");
}

TEST_CASE(scenario_expect_beside_a_ramp_between_the_steps_of_its_ecu_looks_from_the_first_write_to_the_end_value)
{
    // Door steps every 30 ms (0, 30, 60, ...), the controller every 10 ms. The ramp from 40 ms
    // writes 0 for 40 (applied at 60), 3 for 70 (at 90) and its end, 5 for 100, at 120. The
    // expect judges the samples of 60, 90 and 120: not that of 30 ms (50, before the ramp), and
    // the end value although E (100 ms) is between two steps.
    const auto run = [](const std::string& expect) {
        return one_result(read(kHead + "  - set: { Door.level: 50 }\n"
                                       "  - at_ms: 40\n"
                                       "    ramp: { Door.level: { from: 0, to: 5, over_ms: 60 } }\n"
                                       "    expect: { Door.level: " + expect + " }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; });
    };
    check_text(run("{ le: 5 }"), "passed over=130\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 passed start=40 at=100 end=100\n");
    check_text(run("{ le: 4 }"), "failed over=130\n"
                                 "3 passed start=0 at=0 end=0\n"
                                 "4 failed start=40 at=120 end=100 scenario-failed: not true during the ramp (from 40 ms to 120 "
                                 "ms, where its end (100 ms) takes effect): at 120 ms Door.level le 4, but it was 5 (its sample "
                                 "of 120 ms)\n");
    // An expect of another ECU is judged up to the same time: Body's samples of 40 to 120 ms.
    check_text(one_result(read(kHead + "  - set: { Door.level: 50 }\n"
                                       "  - at_ms: 40\n"
                                       "    ramp: { Door.level: { from: 0, to: 5, over_ms: 60 } }\n"
                                       "    expect: { Body.body.speed: { lt: 0 } }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; }),
               "failed over=50\n"
               "3 passed start=0 at=0 end=0\n"
               "4 failed start=40 at=40 end=100 scenario-failed: not true during the ramp (from 40 ms to 120 ms, where its end "
               "(100 ms) takes effect): at 40 ms Body.body.speed lt 0, but it was 0 (its sample of 40 ms)\n");
    // A variable whose ECU sends no sample in that time was not looked at: the step fails. Door
    // samples at 60 and 90; the ramp of Body from 61 ms to 71 ms looks until 80 ms.
    check_text(one_result(read(kHead + "  - at_ms: 61\n"
                                       "    ramp: { Body.level: { from: 0, to: 5, over_ms: 10 } }\n"
                                       "    expect: { Door.level: 0 }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; }),
               "failed over=90\n"
               "3 failed start=61 at=80 end=71 scenario-failed: not checked during the ramp (from 61 ms to 80 ms, where its end "
               "(71 ms) takes effect): no sample of Door.level in that time (its ECU sends one every sample_period); make the "
               "ramp longer, or check in an expect step with within_ms after it\n");
}

TEST_CASE(scenario_expect_beside_a_motion_decides_the_step_with_its_writes)
{
    // The step passes when both its writes and its expect have: a failing expect after it is
    // still looked at, and an expect that fails at the end fails a ramp that is the last step.
    check_text(one_result(read(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                       "    expect: { Body.level: { le: 10 } }\n"
                                       "  - expect: { Body.level: 0 }\n")),
               "failed over=110\n"
               "3 passed start=0 at=100 end=100\n"
               "5 failed start=100 at=100 end=100 scenario-failed: not true at 100 ms: Body.level eq 0, but it was 10 (its "
               "sample of 100 ms)\n");
    check_text(one_result(read(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                       "    expect: { Body.level: { lt: 10 } }\n")),
               "failed over=110\n"
               "3 failed start=0 at=100 end=100 scenario-failed: not true during the ramp (from 0 ms to 100 ms): at 100 ms "
               "Body.level lt 10, but it was 10 (its sample of 100 ms)\n");
    // The writes decide it too: Door steps every 30 ms, so the end of its ramp (100 ms) takes
    // effect at 120 ms and is judged at 130 ms, after the expect on Body closed (at 120 ms, seen
    // at 130 ms). Door rejects that last write (13): the step fails, though its expect held.
    check_text(one_result(read(kHead + "  - ramp: { Door.level: { from: 0, to: 13, over_ms: 100 } }\n"
                                       "    expect: { Body.level: 0 }\n",
                               catalog(10ms, 30ms)),
                          [](World& w) { w.ecus[1].step = 30ms; }),
               "failed over=130\n"
               "3 failed start=0 at=100 end=100 scenario-write: the write for 100 ms was not applied by participant 'Door': "
               "the ECU rejected it (a setter threw: 13 is not a level)\n");
}

TEST_CASE(scenario_expect_beside_a_wave_fails_with_what_the_launcher_learns_first)
{
    // The wave around 90 writes 102 for 110 ms, which the agent refuses at once (out of range);
    // the expect is already false at 100 ms. With a tick of 10 ms the write for 110 ms goes out
    // (and is refused) at 100 ms, before the sample of 100 ms is looked at (at 110 ms); with a
    // tick of 5 ms that sample is looked at first, at 105 ms.
    const auto run = [](SimDuration tick) {
        return one_result(read(kHead + "  - set: { Body.level: 90 }\n"
                                       "  - at_ms: 100\n"
                                       "    wave: { Body.level: { sine: { amplitude: 20, period_ms: 100 }, for_ms: 100 } }\n"
                                       "    expect: { Body.level: { le: 80 } }\n"),
                          [tick](World& w) { w.tick = tick; });
    };
    check_text(run(10ms), "failed over=100\n"
                          "3 passed start=0 at=0 end=0\n"
                          "4 failed start=100 at=110 end=200 scenario-write: the write for 110 ms was not taken by participant "
                          "'Body': the ECU's agent refused it (level: out of range)\n");
    check_text(run(5ms), "failed over=105\n"
                         "3 passed start=0 at=0 end=0\n"
                         "4 failed start=100 at=100 end=200 scenario-failed: not true during the wave (from 100 ms to 200 ms): at "
                         "100 ms Body.level le 80, but it was 90 (its sample of 100 ms)\n");
    // A variable of the expect without a link fails the step as a read, not as a write.
    World alone;
    alone.ecus.pop_back();
    check_text(text_of(alone.run(read(kHead + "  - ramp: { Body.level: { from: 0, to: 10, over_ms: 100 } }\n"
                                              "    expect: { Door.lamp: false }\n"))),
               "failed over=0\n"
               "3 failed start=0 at=0 end=- scenario-failed: Door.lamp cannot be read: participant 'Door' has no control "
               "link in this run: its agent did not connect before the simulation started (the run's control-link findings "
               "and its lines say why)\n");
}

TEST_CASE(scenario_expect_beside_a_ramp_that_uses_an_absent_variable_is_skipped_and_the_ramp_runs)
{
    ScenarioContext context;
    context.variants = {"small", "large"};
    context.variant = "small";
    const ScenarioRead r = read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 2, over_ms: 20 } }\n"
                                                 "    expect: { Body.boost: 0, Body.level: { le: 2 } }\n"
                                                 "  - ramp: { Body.boost: { to: 3, over_ms: 50 } }\n"
                                                 "    expect: { Body.level: 2 }\n"
                                                 "  - set: { Body.lamp: true }\n",
                                         catalog_without_boost(), context);
    check_text(lines(r.findings),
               "N 3 scenario-step-skipped: the ramp runs; its expect is not checked (ramp Body.level from 0 to 2 over 20 ms, "
               "expect not checked: Body.boost; Body.level le 2): Body.boost is absent: participant 'Body' does not have it in "
               "the variant small (absent: true in its variables)\n"
               "N 5 scenario-step-skipped: the step is skipped (ramp Body.boost, expecting Body.level eq 2): Body.boost is "
               "absent: participant 'Body' does not have it in the variant small (absent: true in its variables); the other "
               "steps run, also those that would need what this one does\n"
               "W 0 scenario-nothing-checked: the scenario checks nothing in the variant small: all its 2 checks (expect, "
               "wait_until, or an expect beside a ramp or a wave; lines 3, 5) are skipped or not checked, so it passes whatever "
               "the ECUs do (the notes say why for each). If the scenario is not for this variant, list the variants it is for "
               "in the scenario (only: [large]); if it is, map what the steps look at to what the variant has (variables in "
               "the variant, docs/simulation-config.md)\n");
    CHECK(r.scenario.has_value());
    CHECK_EQ(r.skipped, std::size_t{1});
    CHECK_EQ(r.unchecked_expects, std::size_t{1});
    CHECK(r.scenario->steps[0].conditions.empty() && r.scenario->steps[0].skipped.empty());
    check_text(r.scenario->steps[0].expect_skipped,
               "Body.boost is absent: participant 'Body' does not have it in the variant small (absent: true in its variables)");
    // The first ramp writes, unchecked; the second is skipped and takes its 50 ms.
    World world;
    const ScenarioRun run = world.run(*r.scenario);
    check_text(text_of(run), "passed over=80\n"
                             "3 passed start=0 at=20 end=20\n"
                             "5 skipped start=20 at=- end=70 scenario-skipped: Body.boost is absent: participant 'Body' does "
                             "not have it in the variant small (absent: true in its variables)\n"
                             "7 passed start=70 at=70 end=70\n");
    check_text(applied(world, "Body.level"), "0@0 1@10 2@20 ");
    CHECK(run.steps[0].expect_skipped == r.scenario->steps[0].expect_skipped);
    CHECK(run.steps[1].expect_skipped.empty());
    CHECK_EQ(run.unchecked_expects(), std::size_t{1});
    CHECK_EQ(run.skipped(), std::size_t{1});
    // When the expect beside is the only check and is not checked, the warning says so.
    const ScenarioRead only = read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 2, over_ms: 20 } }\n"
                                                    "    expect: { Body.boost: 0 }\n",
                                            catalog_without_boost());
    CHECK(lines(only.findings).find("W 0 scenario-nothing-checked: the scenario checks nothing in this configuration: its only "
                                    "check (an expect beside a ramp or a wave, line 3) is not checked, so it passes whatever the "
                                    "ECUs do (the notes say why for each). ") != std::string::npos);
    // A scenario with another check left gets no warning.
    const ScenarioRead checked = read_scenario(kHead + "  - ramp: { Body.level: { from: 0, to: 2, over_ms: 20 } }\n"
                                                       "    expect: { Body.boost: 0 }\n"
                                                       "  - expect: { Body.level: 2 }\n",
                                               catalog_without_boost(), context);
    CHECK(std::none_of(checked.findings.begin(), checked.findings.end(), [](const Finding& f) { return f.severity != Severity::Note; }));
}

// --- the timeline of a run (timeline.hpp) ---

TEST_CASE(scenario_motion_starts_from_where_the_one_before_it_left_the_variable)
{
    // An integer. The lag ends at 77.7, which the ECU gets as 78; the ramp after it starts from
    // that 78, not from 77.7: halfway to 77 it writes 77.5, which is 78 (from 77.7 it would be 77).
    const Scenario rounded = read(kHead + "  - wave: { Body.level: { lag: { from: 0, to: 100, tau_ms: 20 }, for_ms: 30 } }\n"
                                          "  - ramp: { Body.level: { to: 77, over_ms: 20 } }\n");
    World world;
    CHECK(world.run(rounded).status == ScenarioStatus::Passed);
    check_text(applied(world, "Body.level"), "0@0 39@10 63@20 78@30 78@30 78@40 77@50 ");
    // A ramp that ends between two steps of its ECU (25 ms, steps of 10 ms): the one after it
    // starts from its end value (80), not from the last value on a step (68).
    const Scenario between = read(kHead + "  - set: { Body.level: 20 }\n"
                                          "  - ramp: { Body.level: { to: 80, over_ms: 25 } }\n"
                                          "  - ramp: { Body.level: { to: 0, over_ms: 20 } }\n");
    World off;
    CHECK(off.run(between).status == ScenarioStatus::Passed);
    check_text(applied(off, "Body.level"), "20@0 20@0 44@10 68@20 80@30 80@30 40@40 0@50 ");
}

TEST_CASE(scenario_first_motion_without_a_base_takes_the_sample_the_controllers_step_has_let_it_see)
{
    // A ramp at 25 ms that does not say where it starts, of a variable the scenario has not
    // written: its base is a sample. With a controller that steps every 10 or 20 ms the samples
    // of 0 and 10 ms were looked at before the writes for 25 ms went out. With one that steps
    // every 30 ms those writes go out at 0 ms, before any sample: the same scenario fails. (The
    // message says the way out: give it from.)
    const Scenario s = read(kHead + "  - at_ms: 25\n"
                                    "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n");
    for (const SimDuration tick : {SimDuration{10ms}, SimDuration{20ms}})
    {
        World world;
        world.tick = tick;
        CHECK(world.run(s).status == ScenarioStatus::Passed);
        check_text(applied(world, "Body.level"), "0@30 40@40 80@50 ");
    }
    World coarse;
    coarse.tick = 30ms;
    check_text(text_of(coarse.run(s)),
               "failed over=0\n"
               "3 failed start=25 at=25 end=45 scenario-write: the ramp of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 25 ms and the scenario has not written it; give the ramp from: <value>\n");
    // Where it turns: the writes for the times up to the controller's first step (30 ms, that
    // time too) go out at 0 ms. A ramp at 30 ms fails as well; one at 40 ms goes out at 30 ms, with
    // the samples of 0 to 20 ms looked at.
    World edge;
    edge.tick = 30ms;
    check_text(text_of(edge.run(read(kHead + "  - at_ms: 30\n"
                                             "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"))),
               "failed over=0\n"
               "3 failed start=30 at=30 end=50 scenario-write: the ramp of Body.level has no value to start from: no sample of its "
               "ECU has arrived before 30 ms and the scenario has not written it; give the ramp from: <value>\n");
    World after;
    after.tick = 30ms;
    CHECK(after.run(read(kHead + "  - at_ms: 40\n"
                                 "    ramp: { Body.level: { to: 80, over_ms: 20 } }\n"))
              .status == ScenarioStatus::Passed);
    check_text(applied(after, "Body.level"), "0@40 40@50 80@60 ");
}

/// The timeline file of `text` run against the model, as `vehicle_sil_launch run --timeline`
/// makes it (the findings: those of the failed steps, as the run's).
std::string timeline_of(const std::string& text, SimDuration tick, const std::function<void(World&)>& setup = {},
                        const ScenarioCatalog& described = catalog(), TimelineLimits limits = {}, const ScenarioContext& context = {})
{
    ScenarioRead r = read_scenario(text, described, context);
    if (!r.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + lines(r.findings));
    }
    const Scenario s = *r.scenario;
    World world;
    world.tick = tick;
    if (setup)
    {
        setup(world);
    }
    TimelineRecord record(s, limits);
    world.record = &record;
    ScenarioRun run = world.run(s, world.ends_after.value_or(5s));
    run.file = "scenario.yaml";
    TimelineInput input;
    input.file = "simulation.yaml";
    input.tick_ms = ms_of(at(tick));
    input.scenario = &s;
    input.run = &run;
    input.catalog = &described;
    input.record = &record;
    for (const ScenarioStepRun& step : run.steps)
    {
        if (step.status == StepStatus::Failed)
        {
            input.findings.push_back(Finding{Severity::Error, step.code, step.text + ": " + step.message, step.line, "", "", "", run.file});
        }
    }
    return format_timeline_json(input);
}

/// A timeline file without what is not compared between runs with different steps of the
/// controller and between builds: the version, the files' names, when the controller saw the
/// scenario over, its step.
std::string compared(const std::string& timeline)
{
    std::istringstream in(timeline);
    std::string out;
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const bool left_out = std::ranges::any_of(
            std::vector<std::string>{"  \"vehicle_sil\": ", "  \"file\": ", "  \"tick_ms\": ", "    \"file\": ", "    \"over_at_ms\": "},
            [&line](const std::string& start) { return line.rfind(start, 0) == 0; });
        if (!left_out)
        {
            out += line + "\n";
        }
    }
    return out;
}

/// The part of a timeline file from `from` up to (not with) `to`.
std::string part(const std::string& timeline, const std::string& from, const std::string& to)
{
    const auto begin = timeline.find(from);
    const auto end = begin == std::string::npos ? begin : timeline.find(to, begin);
    if (end == std::string::npos)
    {
        throw unit::Failure("the timeline has no '" + from + "' ... '" + to + "':\n" + timeline);
    }
    return timeline.substr(begin, end - begin);
}

TEST_CASE(timeline_of_a_run_does_not_depend_on_the_controllers_step)
{
    // A scenario that passes, one that fails at a sample and one whose wait runs out: the file is
    // the same whether the controller steps every 10, 20, 30 or 4 ms, though the engine then looks
    // at the samples and learns the results at other times. Door steps every 4 ms (it samples
    // every 20 ms then). None has a step that waits and becomes true: such a step ends one step
    // of the controller after the engine knows (ScenarioEngine), so it and the steps after it
    // have other times with another step.
    const std::string passes = kHead + "  - set: { Body.level: 10, Door.level: 3 }\n"
                                       "  - at_ms: 20\n"
                                       "    expect: { Body.body.speed: { ge: 100 } }\n"
                                       "  - after_ms: 15\n"
                                       "    ramp: { Door.level: { to: 9, over_ms: 50 }, Body.level: { to: 12, over_ms: 35 } }\n"
                                       "    expect: { Door.level: { ge: 3 }, Body.body.speed: { le: 120 } }\n"
                                       "  - set: { Door.lamp: true }\n"
                                       "  - after_ms: 20\n"
                                       "    expect: { Door.level: 9 }\n"
                                       "    hold_for_ms: 45\n"
                                       "  - set: { Body.level: 0 }\n";
    const std::string fails = kHead + "  - set: { Body.level: 10 }\n"
                                      "  - at_ms: 15\n"
                                      "    expect: { Body.body.speed: { le: 60 }, Door.level: 0 }\n"
                                      "    hold_for_ms: 100\n"
                                      "  - set: { Body.level: 0 }\n";
    const std::string times_out = kHead + "  - wave: { Door.level: { square: { low: 1, high: 2, period_ms: 16 }, for_ms: 64 } }\n"
                                          "    expect: { Door.level: { ge: 0 } }\n"
                                          "  - wait_until: { Body.body.speed: { gt: 0 }, Door.level: 2 }\n"
                                          "    timeout_ms: 75\n";
    const auto door4 = [](World& w) { w.ecus[1].step = 4ms; };
    for (const std::string& text : {passes, fails, times_out})
    {
        const std::string first = compared(timeline_of(text, 10ms, door4, catalog(10ms, 4ms)));
        CHECK(first.find("\"recorded_to_ms\": null") == std::string::npos);
        for (const SimDuration tick : {SimDuration{20ms}, SimDuration{30ms}, SimDuration{4ms}, SimDuration{5ms}})
        {
            check_text(compared(timeline_of(text, tick, door4, catalog(10ms, 4ms))), first, "the timeline with another step");
        }
        // Nor on when what a step makes arrives.
        for (const auto& [name, policy] : policies())
        {
            check_text(compared(timeline_of(text, 10ms, [&](World& w) { door4(w); w.policy = policy; }, catalog(10ms, 4ms))), first,
                       name.c_str());
        }
    }
    // The lines of the inputs are the same too (they are in the files compared above). Here the
    // expect fails at 15 ms. The ramp at 25 ms is put on its line by a controller that steps
    // every 30 ms (it sends the writes up to 30 ms at 0 ms), and not by one that steps every 5 or
    // 10 ms (it knows of the failure at 20 ms, before the writes for 25 ms). The file has the
    // lines put up to "recorded to" (15 ms): the set, and with no step the ramp.
    const std::string ahead = kHead + "  - set: { Body.level: 10 }\n"
                                      "  - at_ms: 15\n"
                                      "    expect: { Body.body.speed: { le: 60 } }\n"
                                      "  - at_ms: 25\n"
                                      "    ramp: { Door.level: { from: 1, to: 9, over_ms: 30 } }\n";
    const auto lines_of = [](const std::string& timeline) { return part(timeline, "  \"lines\": [", "  \"findings\""); };
    for (const SimDuration tick : {SimDuration{5ms}, SimDuration{10ms}, SimDuration{30ms}})
    {
        const std::string made = timeline_of(ahead, tick);
        CHECK(made.find("    \"recorded_to_ms\": 15\n") != std::string::npos);
        check_text(lines_of(made),
                   "  \"lines\": [\n    {\n      \"variable\": \"Body.level\",\n      \"kind\": \"set\",\n      \"line\": 3,\n"
                   "      \"step\": 1,\n      \"from_ms\": 0,\n      \"to_ms\": 0,\n      \"base\": 10,\n"
                   "      \"base_source\": \"written\",\n      \"shape\": null,\n      \"text\": \"Body.level = 10\"\n    }\n  ],\n",
                   "the lines with another step");
        // (The step's own list follows the lines of the file.)
        CHECK(made.find("      \"writes\": [0],\n") != std::string::npos && made.find("      \"writes\": [1],\n") == std::string::npos);
    }
    // The watch beside the ramp goes to the step of Door that applies the end value (88 ms), past
    // the step's end (85 ms); the last step passed at 150 ms, and that is where the file ends.
    const std::string passed = timeline_of(passes, 20ms, door4, catalog(10ms, 4ms));
    check_text(part(passed, "        \"mode\": \"during\",", "        \"conditions\""),
               "        \"mode\": \"during\",\n        \"from_ms\": 35,\n        \"until_ms\": 88,\n");
    CHECK(passed.find("      \"start_ms\": 35,\n      \"at_ms\": 85,\n      \"end_ms\": 85,\n") != std::string::npos);
    check_text(part(passed, "  \"scenario\": {", "  \"variables\""),
               "  \"scenario\": {\n    \"file\": \"scenario.yaml\",\n    \"name\": null,\n    \"status\": \"passed\",\n"
               "    \"over_at_ms\": 160,\n    \"recorded_to_ms\": 150\n  },\n");
    // What is in it: the hold from 15 ms fails at once with the sample of 10 ms, so the file has
    // what was written and sampled up to 15 ms and nothing after it, though the controller (every
    // 30 ms) saw the scenario over at 30 ms, with the samples of 20 ms there.
    const std::string failed = timeline_of(fails, 30ms, door4, catalog(10ms, 4ms));
    CHECK(failed.find("    \"over_at_ms\": 30,\n    \"recorded_to_ms\": 15\n") != std::string::npos);
    CHECK(failed.find("      \"variable\": \"Body.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
                      "      \"points\": [\n        [0, 10]\n      ]") != std::string::npos);
    CHECK(failed.find("      \"variable\": \"Body.body.speed\",\n      \"source\": \"sampled\",\n      \"encoding\": \"changes\",\n"
                      "      \"samples\": 2,\n      \"first_ms\": 0,\n      \"last_ms\": 10,\n      \"points\": [\n"
                      "        [0, 50],\n        [10, 100]\n      ]") != std::string::npos);
    CHECK(failed.find("      \"failed\": {\n        \"at_ms\": 15,\n        \"code\": \"scenario-failed\",\n        \"conditions\": [\n"
                      "          {\n            \"condition\": 0,\n            \"value\": 100,\n            \"sample_ms\": 10\n          }\n"
                      "        ],\n        \"write\": null\n      }") != std::string::npos);
}

std::string file_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw unit::Failure("cannot read " + path.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::string out = text.str();
    std::erase(out, '\r');
    return out;
}

TEST_CASE(timeline_files_are_the_references)
{
    // tests/launch/timeline: each scenario with the timeline file the engine writes for it against
    // the model (README.md there: what is not compared, and how to write the references again).
    // The second list: the other steps of the controller that give the same file (none for a
    // scenario with a step that waits and becomes true, or with a write that an ECU refuses: what
    // the engine learns first then depends on the step).
    const std::filesystem::path directory = std::filesystem::path(VEHICLE_SIL_SOURCE_DIR) / "tests" / "launch" / "timeline";
    const char* update = std::getenv("VEHICLE_SIL_UPDATE_TIMELINE_REFERENCES");
    const bool writing = update != nullptr && std::string(update) == "1";
    const std::vector<std::pair<std::string, std::vector<SimDuration>>> references = {
        {"motions", {20ms, 30ms}},
        {"fails_beside", {20ms, 30ms}},
        {"fails_wait", {20ms, 30ms}},
        {"waits", {}},
        {"write_refused", {}},
        {"points_file", {20ms, 30ms}},
    };
    for (const auto& [name, steps] : references)
    {
        const std::string scenario = file_text(directory / (name + ".yaml"));
        const std::filesystem::path reference = directory / (name + ".timeline.json");
        // The points of a wave may be in a CSV file beside the scenario: <name>.csv.
        ScenarioContext context;
        if (std::filesystem::exists(directory / (name + ".csv")))
        {
            context = with_files({{name + ".csv", file_text(directory / (name + ".csv"))}});
        }
        const auto timeline = [&](SimDuration step) { return timeline_of(scenario, step, {}, catalog(), {}, context); };
        const std::string made = timeline(10ms);
        if (writing)
        {
            std::ofstream out(reference, std::ios::binary | std::ios::trunc);
            out << made;
            if (!out.flush())
            {
                throw unit::Failure("cannot write " + reference.string());
            }
        }
        check_text(compared(made), compared(file_text(reference)),
                   (name + ".timeline.json (if the change is meant: tests/launch/timeline/README.md says how to write it again)").c_str());
        for (const SimDuration step : steps)
        {
            check_text(compared(timeline(step)), compared(made), (name + " with another step of the controller").c_str());
        }
    }
}

TEST_CASE(timeline_keeps_the_newest_points_at_its_limits)
{
    // A ramp of 300 ms (a write every 10 ms) between two sets, and its speed in every sample: with
    // 8 points a series, the 8 newest of the ramp stay, and both sets; of the samples the 8 newest.
    const std::string text = kHead + "  - set: { Body.level: 2 }\n"
                                     "  - ramp: { Body.level: { to: 92, over_ms: 300 } }\n"
                                     "    expect: { Body.body.speed: { ge: 0 } }\n"
                                     "  - set: { Body.level: 0 }\n"
                                     "  - after_ms: 20\n"
                                     "    expect: { Body.body.speed: { ge: 0 } }\n";
    const std::string limited = timeline_of(text, 10ms, {}, catalog(), TimelineLimits{8, 1000});
    check_text(part(limited, "  \"series\": [", "  \"lines\""),
               "  \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"points\": [\n"
               "        [0, 2],\n"
               "        [230, 71],\n"
               "        [240, 74],\n"
               "        [250, 77],\n"
               "        [260, 80],\n"
               "        [270, 83],\n"
               "        [280, 86],\n"
               "        [290, 89],\n"
               "        [300, 92],\n"
               "        [300, 0]\n"
               "      ]\n"
               "    },\n"
               "    {\n"
               "      \"variable\": \"Body.body.speed\",\n"
               "      \"source\": \"sampled\",\n"
               "      \"encoding\": \"changes\",\n"
               "      \"samples\": 33,\n"
               "      \"first_ms\": 0,\n"
               "      \"last_ms\": 320,\n"
               "      \"points\": [\n"
               "        [250, 770],\n"
               "        [260, 800],\n"
               "        [270, 830],\n"
               "        [280, 860],\n"
               "        [290, 890],\n"
               "        [300, 840],\n"
               "        [310, 790],\n"
               "        [320, 740]\n"
               "      ]\n"
               "    }\n"
               "  ],\n");
    check_text(part(limited, "  \"limits\": {", "\n}\n"),
               "  \"limits\": {\n"
               "    \"points_per_series\": 8,\n"
               "    \"points_total\": 1000,\n"
               "    \"dropped\": [\n"
               "      {\n"
               "        \"variable\": \"Body.level\",\n"
               "        \"source\": \"written\",\n"
               "        \"dropped\": 23,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 230\n"
               "      },\n"
               "      {\n"
               "        \"variable\": \"Body.body.speed\",\n"
               "        \"source\": \"sampled\",\n"
               "        \"dropped\": 25,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 250\n"
               "      }\n"
               "    ]\n"
               "  }");
    // The same with another step of the controller, which has looked further ahead when the
    // scenario is over.
    check_text(compared(timeline_of(text, 30ms, {}, catalog(), TimelineLimits{8, 1000})), compared(limited), "limited, another step");
    // All series together: the ones with the most points give first, down to the same number.
    const std::string total = timeline_of(text, 10ms, {}, catalog(), TimelineLimits{1000, 10});
    check_text(part(total, "  \"limits\": {", "\n}\n"),
               "  \"limits\": {\n"
               "    \"points_per_series\": 1000,\n"
               "    \"points_total\": 10,\n"
               "    \"dropped\": [\n"
               "      {\n"
               "        \"variable\": \"Body.level\",\n"
               "        \"source\": \"written\",\n"
               "        \"dropped\": 26,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 260\n"
               "      },\n"
               "      {\n"
               "        \"variable\": \"Body.body.speed\",\n"
               "        \"source\": \"sampled\",\n"
               "        \"dropped\": 28,\n"
               "        \"dropped_from_ms\": 0,\n"
               "        \"dropped_before_ms\": 280\n"
               "      }\n"
               "    ]\n"
               "  }");
    check_text(compared(timeline_of(text, 30ms, {}, catalog(), TimelineLimits{1000, 10})), compared(total), "total, another step");
}

TEST_CASE(timeline_counts_the_samples_up_to_the_time_it_is_closed_at)
{
    // Samples of one value are kept as one point with their number; closed at a time between
    // them, the record has the samples up to that time, also where they do not come evenly.
    const Scenario s = read(kHead + "  - expect: { Body.level: 0, Body.body.speed: 0 }\n");
    const auto closed_at = [&s](SimDuration to) {
        TimelineRecord record(s);
        for (const std::int64_t t : {0, 10, 20, 25, 35, 45, 55})
        {
            record.sampled(0, at(std::chrono::milliseconds(t)), control::Value{std::int64_t{7}});
            record.sampled(1, at(std::chrono::milliseconds(t)), t < 35 ? control::Value{0.0} : control::Value{-0.0});
        }
        record.close(at(to));
        std::string out;
        for (const auto& series : record.series())
        {
            out += std::to_string(series.sample_count) + " samples, last " + time_text(series.last_sample) + ":";
            for (const auto& point : series.samples)
            {
                out += point.repeats ? "" : " " + std::to_string(ms_of(point.first)) + "=" + value_text(point.value);
            }
            out += "\n";
        }
        return out;
    };
    // 0 and -0 are written differently: a point each.
    check_text(closed_at(100ms), "7 samples, last 55: 0=7\n7 samples, last 55: 0=0 35=-0\n");
    check_text(closed_at(54ms), "6 samples, last 45: 0=7\n6 samples, last 45: 0=0 35=-0\n");
    check_text(closed_at(35ms), "5 samples, last 35: 0=7\n5 samples, last 35: 0=0 35=-0\n");
    check_text(closed_at(34ms), "4 samples, last 25: 0=7\n4 samples, last 25: 0=0\n");
    check_text(closed_at(19ms), "2 samples, last 10: 0=7\n2 samples, last 10: 0=0\n");
    check_text(closed_at(0ms), "1 samples, last 0: 0=7\n1 samples, last 0: 0=0\n");
    // Of the findings the file has those of the scenario, and that an ECU's values came late (its
    // code alone); nothing of the pace in real time, which differs from run to run.
    bool code_only = true;
    CHECK(timeline_takes_finding("scenario-failed", code_only) && !code_only);
    CHECK(timeline_takes_finding("control-link-late-values", code_only) && code_only);
    CHECK(!timeline_takes_finding("behind-real-time", code_only));
    CHECK(!timeline_takes_finding("timeline-write", code_only));
    CHECK(!timeline_takes_finding("exit-code", code_only));
    // Nothing is noted once it is closed.
    TimelineRecord record(s);
    record.close(at(10ms));
    record.sampled(0, at(0ms), control::Value{std::int64_t{1}});
    CHECK(record.series()[0].samples.empty());
}

TEST_CASE(timeline_marks_the_write_that_was_refused)
{
    // A written point is what was sent. The write a step failed of is a point too, and the series
    // says from where on its points were not applied; the failure names the point.
    // (A description without the range of level: the check lets the value through, as with an
    // ECU that says less than its agent checks.)
    ScenarioCatalog loose = catalog();
    loose[0].variables[0].max.reset();
    const auto refused = [&loose](const std::string& text, SimDuration tick) {
        const std::string timeline = timeline_of(text, tick, {}, loose);
        return part(timeline, "  \"scenario\": {", "  \"variables\"") + part(timeline, "        \"write\": {", "      }\n") +
               part(timeline, "  \"series\": [", "  \"lines\"");
    };
    // Refused at once by the agent (out of range), between two steps of the controller: over at
    // 30 ms, the write is for 40 ms.
    check_text(refused(kHead + "  - set: { Body.level: 7 }\n"
                               "  - set: { Body.level: 95 }\n"
                               "  - at_ms: 30\n"
                               "    wave: { Body.level: { triangle: { amplitude: 10, period_ms: 40 }, for_ms: 40 } }\n",
                       10ms),
               "  \"scenario\": {\n"
               "    \"file\": \"scenario.yaml\",\n"
               "    \"name\": null,\n"
               "    \"status\": \"failed\",\n"
               "    \"over_at_ms\": 30,\n"
               "    \"recorded_to_ms\": 40\n"
               "  },\n"
               "        \"write\": {\n"
               "          \"variable\": \"Body.level\",\n"
               "          \"at_ms\": 40,\n"
               "          \"value\": 105,\n"
               "          \"point\": 3\n"
               "    \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"not_applied_from\": 3,\n"
               "      \"points\": [\n"
               "        [0, 7],\n"
               "        [0, 95],\n"
               "        [30, 95],\n"
               "        [40, 105]\n"
               "      ]\n"
               "    }\n"
               "  ],\n");
    // The same at a step of the controller: the write for 0 ms is refused at 0 ms, where the
    // scenario is over.
    check_text(refused(kHead + "  - set: { Body.level: 102 }\n", 10ms),
               "  \"scenario\": {\n"
               "    \"file\": \"scenario.yaml\",\n"
               "    \"name\": null,\n"
               "    \"status\": \"failed\",\n"
               "    \"over_at_ms\": 0,\n"
               "    \"recorded_to_ms\": 0\n"
               "  },\n"
               "        \"write\": {\n"
               "          \"variable\": \"Body.level\",\n"
               "          \"at_ms\": 0,\n"
               "          \"value\": 102,\n"
               "          \"point\": 0\n"
               "    \"series\": [\n"
               "    {\n"
               "      \"variable\": \"Body.level\",\n"
               "      \"source\": \"written\",\n"
               "      \"encoding\": \"all\",\n"
               "      \"not_applied_from\": 0,\n"
               "      \"points\": [\n"
               "        [0, 102]\n"
               "      ]\n"
               "    }\n"
               "  ],\n");
}

TEST_CASE(timeline_of_a_failed_run_has_the_steps_as_the_controllers_step_saw_them)
{
    // An expect that the sample of 10 ms fails, and a set for 20 ms after it. What becomes of the
    // set depends on the controller's step: with 5 ms the failure is seen before the set is sent;
    // with 10 ms the set is sent and not judged yet; with 30 ms it was sent at 0 ms and is seen
    // applied at 30 ms, before the sample is looked at.
    const std::string text = kHead + "  - set: { Body.level: 5 }\n"
                                     "  - at_ms: 10\n"
                                     "    expect: { Body.body.speed: 0 }\n"
                                     "  - at_ms: 20\n"
                                     "    set: { Body.level: 6 }\n";
    const auto steps = [&text](SimDuration tick) {
        const std::string timeline = timeline_of(text, tick);
        std::string out = part(timeline, "    \"recorded_to_ms\"", "  },");
        std::istringstream in(part(timeline, "  \"steps\": [", "  \"series\""));
        for (std::string line; std::getline(in, line);)
        {
            for (const char* key : {"\"index\"", "\"status\"", "\"start_ms\"", "\"at_ms\"", "\"end_ms\""})
            {
                if (line.rfind(std::string("      ") + key, 0) == 0)
                {
                    out += line.substr(6) + (std::string(key) == "\"end_ms\"" ? "\n" : " ");
                }
            }
        }
        return out + part(timeline, "  \"series\": [", "  \"lines\"");
    };
    // The series and where they end are the same: they go to the failure, and the set for 20 ms
    // is no point of them, also where its step passed.
    const std::string same =
        "    \"recorded_to_ms\": 10\n"
        "\"index\": 1, \"status\": \"passed\", \"start_ms\": 0, \"at_ms\": 0, \"end_ms\": 0,\n"
        "\"index\": 2, \"status\": \"failed\", \"start_ms\": 10, \"at_ms\": 10, \"end_ms\": 10,\n";
    const std::string series =
        "  \"series\": [\n"
        "    {\n"
        "      \"variable\": \"Body.level\",\n"
        "      \"source\": \"written\",\n"
        "      \"encoding\": \"all\",\n"
        "      \"points\": [\n"
        "        [0, 5]\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"variable\": \"Body.body.speed\",\n"
        "      \"source\": \"sampled\",\n"
        "      \"encoding\": \"changes\",\n"
        "      \"samples\": 2,\n"
        "      \"first_ms\": 0,\n"
        "      \"last_ms\": 10,\n"
        "      \"points\": [\n"
        "        [0, 50]\n"
        "      ]\n"
        "    }\n"
        "  ],\n";
    check_text(steps(5ms),
               same + "\"index\": 3, \"status\": \"not_run\", \"start_ms\": null, \"at_ms\": null, \"end_ms\": null,\n" + series);
    check_text(steps(10ms),
               same + "\"index\": 3, \"status\": \"unfinished\", \"start_ms\": 20, \"at_ms\": null, \"end_ms\": null,\n" + series);
    check_text(steps(30ms),
               same + "\"index\": 3, \"status\": \"passed\", \"start_ms\": 20, \"at_ms\": 20, \"end_ms\": 20,\n" + series);
}

TEST_CASE(timeline_of_a_run_that_did_not_get_to_its_end_says_how_far_it_is_recorded)
{
    const auto head = [](const std::string& text, const std::function<void(World&)>& setup) {
        const std::string timeline = timeline_of(text, 10ms, setup);
        std::string out = part(timeline, "    \"status\"", "  },");
        std::istringstream in(part(timeline, "  \"series\": [", "  \"lines\""));
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind("      \"not_applied_from\"", 0) == 0 || line.rfind("      \"last_ms\"", 0) == 0)
            {
                out += line.substr(6) + "\n";
            }
        }
        std::istringstream steps(part(timeline, "  \"steps\": [", "  \"series\""));
        for (std::string line; std::getline(steps, line);)
        {
            if (line.rfind("      \"failed\"", 0) == 0)
            {
                out += line.substr(6) + "\n";
            }
        }
        return out;
    };
    const std::string refused = kHead + "  - set: { Body.level: 7 }\n"
                                        "  - at_ms: 40\n"
                                        "    set: { Body.level: 13 }\n"
                                        "  - expect: { Body.level: 13 }\n"
                                        "    within_ms: 100\n";
    // stop_at_ms before the scenario's end: failed at that time, and recorded to it.
    check_text(head(refused, [](World& w) { w.stop_at = at(30ms); }),
               "    \"status\": \"failed\",\n"
               "    \"over_at_ms\": 30,\n"
               "    \"recorded_to_ms\": 30\n"
               "\"last_ms\": 20,\n"
               "\"failed\": null\n"
               "\"failed\": {\n"
               "\"failed\": null\n");
    // A participant failed after a write to it had failed: the scenario is incomplete and no step
    // has failed, but the record ends where it was closed, at the write, and its point is marked.
    check_text(head(refused, [](World& w) { w.superseded = true; }),
               "    \"status\": \"incomplete\",\n"
               "    \"over_at_ms\": null,\n"
               "    \"recorded_to_ms\": 40\n"
               "\"not_applied_from\": 1,\n"
               "\"last_ms\": 40,\n"
               "\"failed\": null\n"
               "\"failed\": null\n"
               "\"failed\": null\n");
    // The simulation ended for another reason: recorded to the last sample that was looked at.
    check_text(head(kHead + "  - set: { Body.level: 7 }\n"
                            "  - wait_until: { Body.level: 8 }\n"
                            "    timeout_ms: 9000\n",
                    [](World& w) { w.ends_after = 300ms; }),
               "    \"status\": \"incomplete\",\n"
               "    \"over_at_ms\": null,\n"
               "    \"recorded_to_ms\": 290\n"
               "\"last_ms\": 290,\n"
               "\"failed\": null\n"
               "\"failed\": null\n");
}

TEST_CASE(timeline_keeps_the_writes_of_the_times_before_a_motion_that_finds_no_base)
{
    // The controller steps every 30 ms, so the writes for 10 and for 25 ms go out together at
    // 0 ms, before any sample. The ramp at 25 ms finds no value to start from; the set of the
    // other variable at 10 ms was sent before that is said, and is in its series.
    const std::string text = kHead + "  - at_ms: 10\n"
                                     "    set: { Door.level: 3 }\n"
                                     "  - at_ms: 25\n"
                                     "    ramp: { Body.level: { to: 5, over_ms: 20 } }\n";
    const std::string made = timeline_of(text, 30ms);
    check_text(part(made, "    \"status\": ", "  },\n  \"variables\""),
               "    \"status\": \"failed\",\n    \"over_at_ms\": 0,\n    \"recorded_to_ms\": 25\n");
    // (Sent, not applied: the simulation ended at 0 ms.) The ramp wrote nothing.
    check_text(part(made, "  \"series\": [", "  \"lines\""),
               "  \"series\": [\n"
               "    {\n      \"variable\": \"Door.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
               "      \"not_applied_from\": 0,\n      \"points\": [\n        [10, 3]\n      ]\n    },\n"
               "    {\n      \"variable\": \"Body.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
               "      \"points\": []\n    }\n  ],\n");
    CHECK(made.find("      \"status\": \"unfinished\",\n      \"start_ms\": 10,") != std::string::npos);
}

TEST_CASE(timeline_lines_say_where_each_motion_starts_and_where_that_is_from)
{
    // What each step put on the line of a variable, with the base of a motion and where it is
    // from: the step ("written"), the line, or a sample.
    const auto lines_of = [](const std::string& timeline) {
        // "<kind> <variable> <from>-<to> base=<base> <source>" per line of the file's lines.
        std::istringstream in(part(timeline, "  \"lines\": [", "  \"findings\""));
        std::string out;
        const auto value = [](const std::string& line) {
            std::string text = line.substr(line.find(": ") + 2);
            std::erase(text, '"');
            std::erase(text, ',');
            return text;
        };
        for (std::string line; std::getline(in, line);)
        {
            for (const auto& [key, after] : std::vector<std::pair<std::string, std::string>>{
                     {"kind", " "}, {"variable", " "}, {"from_ms", "-"}, {"to_ms", " base="}, {"base", " "}, {"base_source", "\n"}})
            {
                if (line.rfind("      \"" + key + "\": ", 0) == 0)
                {
                    out += value(line) + after;
                }
            }
        }
        return out;
    };
    // Door.level is 0 in the model, and the scenario has not written it: the first ramp starts
    // from the sample. The ramp of Body.level starts from the set, the wave after it swings around
    // where the ramp ends, the lag names its start, and the square has its own values.
    const std::string made =
        timeline_of(kHead + "  - set: { Body.level: 20 }\n"
                            "  - at_ms: 30\n"
                            "    ramp: { Body.level: { to: 80, over_ms: 25 }, Door.level: { to: 4, over_ms: 20 } }\n"
                            "  - wave: { Body.level: { triangle: { amplitude: 5, period_ms: 40 }, for_ms: 40 },\n"
                            "            Door.ratio: { lag: { from: 0.25, to: 1, tau_ms: 20 }, for_ms: 40 },\n"
                            "            Door.lamp: { square: { period_ms: 20 }, for_ms: 40 } }\n",
                    10ms);
    // (The variable comes before the kind in the file.)
    check_text(lines_of(made), "Body.level set 0-0 base=20 written\n"
                               "Body.level ramp 30-55 base=20 line\n"
                               "Door.level ramp 30-50 base=0 sample\n"
                               "Body.level wave 55-95 base=80 line\n"
                               "Door.ratio wave 55-95 base=0.25 written\n"
                               "Door.lamp wave 55-95 base=null null\n");
    CHECK(made.find("      \"shape\": {\n        \"type\": \"lag\",\n        \"to\": 1,\n        \"tau_ms\": 20,\n"
                    "        \"for_ms\": 40\n      },\n"
                    "      \"text\": \"Door.ratio lag from 0.25 to 1 tau 20 ms for 40 ms\"") != std::string::npos);
    CHECK(made.find("      \"writes\": [1, 2],\n") != std::string::npos && made.find("      \"writes\": [3, 4, 5],\n") != std::string::npos);

    // The file has what happened: a line of which no write was sent is not in it, and not in the
    // writes of its step. A set that its agent refuses at once (sent: its point is in the series,
    // marked), and at the same time a ramp, of which nothing went out: the set's line alone, and
    // the ramp's step (not run) has no writes.
    ScenarioCatalog loose = catalog();
    loose[0].variables[0].max.reset();
    const std::string refused = timeline_of(kHead + "  - at_ms: 40\n"
                                                    "    set: { Body.level: 105 }\n"
                                                    "  - ramp: { Body.level: { to: 50, over_ms: 20 } }\n",
                                            10ms, {}, loose);
    CHECK(refused.find("    \"status\": \"failed\",") != std::string::npos);
    check_text(lines_of(refused), "Body.level set 40-40 base=105 written\n");
    const auto writes_of = [](const std::string& timeline) {
        std::istringstream in(timeline);
        std::string out;
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind("      \"writes\": ", 0) == 0)
            {
                out += line.substr(16) + " ";
            }
        }
        return out;
    };
    check_text(writes_of(refused), "[0], [], ");
    // One step moves three variables. The second finds no base (the scenario has not written it
    // and no sample is there at 0 ms): the first was sent and is a line, the second and the third
    // (which names its base) were not and are not. Whatever the controller's step.
    for (const SimDuration tick : {SimDuration{5ms}, SimDuration{10ms}, SimDuration{30ms}})
    {
        const std::string part_sent = timeline_of(kHead + "  - set: { Door.lamp: true }\n"
                                                          "  - ramp: { Door.ratio: { from: 0, to: 1, over_ms: 20 }, Body.level: { to: 5, over_ms: 20 },\n"
                                                          "            Door.level: { from: 0, to: 9, over_ms: 20 } }\n"
                                                          "  - set: { Body.level: 1 }\n",
                                                tick);
        check_text(lines_of(part_sent), "Door.lamp set 0-0 base=true written\n"
                                        "Door.ratio ramp 0-20 base=0 written\n",
                   "the lines of a step that sent a part of its writes");
        check_text(writes_of(part_sent), "[0], [1], [], ");
        CHECK(part_sent.find("      \"variable\": \"Door.level\",\n      \"source\": \"written\",\n      \"encoding\": \"all\",\n"
                             "      \"points\": []\n") != std::string::npos);
    }
    // A run that never began has no lines, and says so with an empty list.
    const Scenario s = read(kHead + "  - set: { Body.level: 20 }\n");
    ScenarioRun run = scenario_not_run(s);
    TimelineInput input;
    input.file = "simulation.yaml";
    input.scenario = &s;
    input.run = &run;
    const std::string none = format_timeline_json(input);
    CHECK(none.find("  \"lines\": [],\n") != std::string::npos && none.find("      \"writes\": [],\n") != std::string::npos);
}

TEST_CASE(timeline_line_after_a_motion_that_returns_to_a_sample_starts_from_that_sample_too)
{
    // Door's input is 20, and Door writes 60 to it in its step at 30 ms. The wave at 45 ms names no middle and
    // the scenario has not written the variable: it swings around a sample, and returns to it.
    // The ramp after it starts from the line, which is that sample still. A controller that steps
    // every 10 ms sends the writes for 45 ms at 40 ms and has looked at the sample of 30 ms (60);
    // one that steps every 30 ms sends them at 30 ms and has looked at the one of 20 ms (20).
    const std::string text = kHead + "  - at_ms: 45\n"
                                     "    wave: { Door.level: { square: { amplitude: 5, period_ms: 20 }, for_ms: 20 } }\n"
                                     "  - ramp: { Door.level: { to: 10, over_ms: 20 } }\n";
    const auto bases = [&text](SimDuration tick) {
        const std::string made = timeline_of(text, tick, [](World& w) {
            w.ecus[1].level = 20;
            w.ecus[1].last_level = 20;
            w.ecus[1].own_level = std::pair{at(30ms), std::int64_t{60}};
        });
        std::istringstream in(part(made, "  \"lines\": [", "  \"findings\""));
        std::string out;
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind("      \"base", 0) == 0)
            {
                out += line.substr(6) + " ";
            }
        }
        return out;
    };
    check_text(bases(10ms), "\"base\": 60, \"base_source\": \"sample\", \"base\": 60, \"base_source\": \"line\", ");
    check_text(bases(30ms), "\"base\": 20, \"base_source\": \"sample\", \"base\": 20, \"base_source\": \"line\", ");
}

// --- dither: a shape on top of an input ---

namespace {

const std::string kDitherHead = "vehicle_sil_scenario: 1\n";

/// The writes that were applied to one variable, "value@time of the write>step that applied it"
/// each, up to the writes for `until`.
std::string written_to(const World& world, const std::string& variable, SimDuration until)
{
    std::string out;
    const std::string head = variable + "=";
    for (const std::string& entry : world.log)
    {
        const std::size_t forAt = entry.find(" for ");
        if (entry.rfind(head, 0) != 0 || forAt == std::string::npos)
        {
            continue;
        }
        const std::size_t applied = entry.find(" applied at ");
        const std::string time = entry.substr(forAt + 5, applied - forAt - 5);
        if (std::stoll(time) <= ms_of(at(until)))
        {
            out += entry.substr(head.size(), forAt - head.size()) + "@" + time + ">" + entry.substr(applied + 12) + " ";
        }
    }
    return out;
}

/// How many writes of one variable each step of its ECU applied, "<step>:<count>" for the steps
/// up to `until` that did not apply exactly one.
std::string steps_without_one_write(const World& world, const std::string& variable, SimDuration from, SimDuration until, SimDuration step)
{
    std::map<std::int64_t, int> count;
    const std::string head = variable + "=";
    for (const std::string& entry : world.log)
    {
        if (entry.rfind(head, 0) == 0)
        {
            ++count[std::stoll(entry.substr(entry.rfind(' ') + 1))];
        }
    }
    std::string out;
    for (SimDuration t = from; t <= until; t += step)
    {
        const int n = count[ms_of(at(t))];
        if (n != 1)
        {
            out += std::to_string(ms_of(at(t))) + ":" + std::to_string(n) + " ";
        }
    }
    return out;
}

} // namespace

TEST_CASE(scenario_dither_is_read_from_the_inputs_and_from_its_step)
{
    const ScenarioRead r = read_scenario(kDitherHead + "inputs:\n"
                                                       "  Body.ratio: { dither: { noise: { amplitude: 0.05, hold_ms: 10, seed: 7 } } }\n"
                                                       "  Door.level:\n"
                                                       "    dither: { triangle: { amplitude: 2, period_ms: 80 } }\n"
                                                       "steps:\n"
                                                       "  - set: { Body.ratio: 0.5 }\n"
                                                       "  - after_ms: 100\n"
                                                       "    dither: { Body.ratio: none, Door.level: { square: { amplitude: 1, frequency_hz: 25, duty: 25 } } }\n",
                                         catalog());
    check_text(lines(r.findings), "");
    if (!r.scenario)
    {
        throw unit::Failure("no scenario");
    }
    const Scenario& s = *r.scenario;
    // The inputs come first among the variables, wherever a step uses them.
    CHECK(s.inputs.size() == 2 && s.variables.size() == 2);
    if (s.inputs.size() != 2 || s.steps.size() != 2 || s.steps[1].overlays.size() != 2)
    {
        throw unit::Failure("the scenario was not read as written");
    }
    CHECK(s.variables[0].text == "Body.ratio" && s.variables[1].text == "Door.level");
    CHECK(s.inputs[0].variable == 0 && s.inputs[0].line == 3 && s.inputs[0].shape != nullptr);
    CHECK(s.inputs[0].text == "Body.ratio noise amplitude 0.05 hold 10 ms seed 7");
    CHECK(s.inputs[1].variable == 1 && s.inputs[1].line == 5);
    CHECK(s.inputs[1].text == "Door.level triangle amplitude 2 period 80 ms");
    CHECK(s.inputs[1].described.type == "triangle" && s.inputs[1].described.values.size() == 2);
    CHECK(s.steps[1].kind == StepKind::Dither && std::string(step_kind_name(s.steps[1].kind)) == "dither");
    check_text(s.steps[1].text, "dither Body.ratio none; Door.level square amplitude 1 frequency 25 Hz duty 25 %");
    CHECK(s.steps[1].overlays[0].shape == nullptr && s.steps[1].overlays[0].described.type.empty());
    CHECK(s.steps[1].overlays[1].shape != nullptr && s.steps[1].overlays[1].described.type == "square");
    // A dither step takes no time: the next step counts from its time.
    const Scenario timed = read(kHead + "  - at_ms: 50\n    dither: { Body.level: none }\n  - after_ms: 10\n    set: { Body.level: 1 }\n");
    World world;
    check_text(text_of(world.run(timed)), "passed over=70\n"
                                          "3 passed start=50 at=50 end=50\n"
                                          "5 passed start=60 at=60 end=60\n");
    // A number of the shape may be a parameter, as in a wave.
    const Scenario withParam = read(kDitherHead + "params: { swing: 3 }\ninputs:\n  Body.level: { dither: { triangle: { amplitude: \"${swing}\", period_ms: 40 } } }\n"
                                                  "steps:\n  - set: { Body.level: 50 }\n");
    CHECK(withParam.inputs.size() == 1 && withParam.inputs[0].text == "Body.level triangle amplitude 3 period 40 ms");
}

TEST_CASE(scenario_dither_adds_to_whatever_the_scenario_puts_on_the_input)
{
    // A triangle of 2 every 40 ms on Body.level: 0, +2, 0, -2 at the steps of Body. It is on the
    // value of the set, on the ramp (which starts from the 50 the set left, not from a dithered
    // value) and on the ramp's end value; none takes it off with one write of the value alone;
    // a dither put later is at the place of its period that the time says (250 ms: the top).
    const Scenario s = read(kDitherHead + "inputs:\n"
                                          "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
                                          "steps:\n"
                                          "  - set: { Body.level: 50 }\n"
                                          "  - at_ms: 100\n"
                                          "    ramp: { Body.level: { to: 80, over_ms: 60 } }\n"
                                          "  - at_ms: 200\n"
                                          "    dither: { Body.level: none }\n"
                                          "  - at_ms: 250\n"
                                          "    dither: { Body.level: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
                                          "  - at_ms: 280\n"
                                          "    expect: { Body.level: 80 }\n");
    for (const auto& [name, policy] : policies())
    {
        for (const SimDuration tick : {5ms, 10ms, 30ms})
        {
            World world;
            world.tick = tick;
            world.policy = policy;
            const ScenarioRun run = world.run(s);
            const std::string where = name + ", tick " + std::to_string(tick.count());
            check_text(written_to(world, "Body.level", 280ms),
                       "50@0>0 52@10>10 50@20>20 48@30>30 50@40>40 52@50>50 50@60>60 48@70>70 50@80>80 52@90>90 "
                       "50@100>100 53@110>110 60@120>120 67@130>130 70@140>140 73@150>150 80@160>160 "
                       "82@170>170 80@180>180 78@190>190 80@200>200 "
                       "82@250>250 80@260>260 78@270>270 80@280>280 ",
                       where.c_str());
            check_text(text_of(run).substr(text_of(run).find('\n') + 1), "5 passed start=0 at=0 end=0\n"
                                                                          "6 passed start=100 at=160 end=160\n"
                                                                          "8 passed start=200 at=200 end=200\n"
                                                                          "10 passed start=250 at=250 end=250\n"
                                                                          "12 passed start=280 at=280 end=280\n",
                       where.c_str());
        }
    }
}

TEST_CASE(scenario_dither_writes_once_per_step_of_its_ecu_where_no_step_writes_and_no_motion_runs)
{
    // The values and the times of the writes do not depend on the controller's step or on when
    // the samples arrive, also in a stretch after a step that waits: the dither is a function
    // of the virtual time. Body.level: a ramp that begins and ends between two steps of Body
    // (107 ms to 193 ms). Body.ratio: set once, with a noise. Door.level has no dither.
    const std::string steps = "steps:\n"
                              "  - set: { Body.level: 20, Body.ratio: 0.5, Door.level: 7 }\n"
                              "  - at_ms: 107\n"
                              "    ramp: { Body.level: { to: 60, over_ms: 86 }, Door.level: { to: 9, over_ms: 86 } }\n"
                              "  - wait_until: { Body.body.speed: { ge: 615 } }\n"
                              "    timeout_ms: 2000\n"
                              "  - expect: { Body.level: { ge: 50 } }\n"
                              "    hold_for_ms: 100\n";
    const Scenario s = read(kDitherHead + "inputs:\n"
                                          "  Body.level: { dither: { triangle: { amplitude: 2, period_ms: 40 } } }\n"
                                          "  Body.ratio: { dither: { noise: { amplitude: 0.05, hold_ms: 10, seed: 7 } } }\n" +
                            steps);
    const Scenario plain = read(kDitherHead + steps);
    std::string first;
    std::string firstRatio;
    std::string door;
    for (const auto& [name, policy] : policies())
    {
        for (const SimDuration tick : {5ms, 10ms, 30ms})
        {
            const std::string where = name + ", tick " + std::to_string(tick.count());
            World world;
            world.tick = tick;
            world.policy = policy;
            const ScenarioRun run = world.run(s);
            check_text(scenario_status_name(run.status), "passed", where.c_str());
            // The wait became true at a sample, whose time is the same in every run; the stretch
            // after it is 100 ms of the dither alone.
            const SimDuration waited = *run.steps[2].at - SimTime{};
            check_text(std::to_string(waited.count() / 1000000), "210", where.c_str()); // (the speed follows the level: 620 with the 62 of 210 ms)
            const std::string level = written_to(world, "Body.level", waited + 100ms);
            const std::string ratio = written_to(world, "Body.ratio", waited + 100ms);
            if (first.empty())
            {
                first = level;
                firstRatio = ratio;
            }
            check_text(level, first, where.c_str());
            check_text(ratio, firstRatio, where.c_str());
            // One write per step of the ECU, at the step's own time, but for: the ramp's writes,
            // which keep their own times (107, 117, ...: each is applied by the next step), and
            // its end value at 193 ms, which the step at 200 ms applies before the dither's write
            // of 200 ms.
            check_text(steps_without_one_write(world, "Body.level", 0ms, waited + 100ms, 10ms), "200:2 ", where.c_str());
            check_text(steps_without_one_write(world, "Body.ratio", 0ms, waited + 100ms, 10ms), "", where.c_str());
            // An input without a dither is written as in a scenario without any: the same writes.
            World other;
            other.tick = tick;
            other.policy = policy;
            other.run(plain); // (its wait runs out: without the dither the speed stays below 615)
            check_text(written_to(world, "Door.level", 5s), written_to(other, "Door.level", 5s), where.c_str());
            if (door.empty())
            {
                door = written_to(world, "Door.level", 5s);
            }
            check_text(written_to(world, "Door.level", 5s), door, where.c_str());
            // The ramp writes at the same times with and without the dither.
            const auto times = [](const std::string& writes) {
                std::string out;
                for (std::size_t at = writes.find('@'); at != std::string::npos; at = writes.find('@', at + 1))
                {
                    out += writes.substr(at, writes.find(' ', at) - at) + " ";
                }
                return out;
            };
            const std::string ramp = times(written_to(other, "Body.level", 193ms));
            check_text(ramp, "@0>0 @107>110 @117>120 @127>130 @137>140 @147>150 @157>160 @167>170 @177>180 @187>190 @193>200 ", where.c_str());
            CHECK(times(level).find(" @100>100 " + ramp.substr(ramp.find("@107")) + "@200>200 ") != std::string::npos);
        }
    }
    check_text(first.substr(0, first.find("@200>200")),
               "20@0>0 22@10>10 20@20>20 18@30>30 20@40>40 22@50>50 20@60>60 18@70>70 20@80>80 22@90>90 20@100>100 "
               "19@107>110 24@117>120 31@127>130 35@137>140 37@147>150 43@157>160 49@167>170 53@177>180 56@187>190 59@193>200 60");
    check_text(door, "7@0>0 7@107>110 7@117>120 7@127>130 8@137>140 8@147>150 8@157>160 8@167>170 9@177>180 9@187>190 9@193>200 ");
}

TEST_CASE(scenario_dither_is_added_before_an_integer_is_rounded)
{
    // Body.level goes from 10 to 11 in 100 ms. At 40 ms the ramp is at 10.4, which is written as
    // 10; the triangle of 0.2 is at its top there (a quarter of 160 ms), and 10.6 is written as
    // 11. Added to the rounded 10 it would stay 10.
    const std::string ramp = "steps:\n  - ramp: { Body.level: { from: 10, to: 11, over_ms: 100 } }\n";
    World plain;
    plain.run(read(kDitherHead + ramp));
    check_text(written_to(plain, "Body.level", 100ms), "10@0>0 10@10>10 10@20>20 10@30>30 10@40>40 11@50>50 11@60>60 11@70>70 11@80>80 11@90>90 11@100>100 ");
    World world;
    world.run(read(kDitherHead + "inputs:\n  Body.level: { dither: { triangle: { amplitude: 0.2, period_ms: 160 } } }\n" + ramp));
    check_text(written_to(world, "Body.level", 100ms), "10@0>0 10@10>10 10@20>20 10@30>30 11@40>40 11@50>50 11@60>60 11@70>70 11@80>80 11@90>90 11@100>100 ");
    // A real input gets the sum as it is: the same bits as a wave around the same value from 0.
    World real;
    real.run(read(kDitherHead + "inputs:\n  Body.ratio: { dither: { triangle: { amplitude: 0.07, period_ms: 40, phase_deg: 30 } } }\n"
                                "steps:\n  - set: { Body.ratio: 0.3 }\n  - at_ms: 100\n    expect: { Body.ratio: { ge: 0 } }\n"));
    World wave;
    wave.run(read(kDitherHead + "steps:\n  - wave: { Body.ratio: { triangle: { amplitude: 0.07, period_ms: 40, phase_deg: 30 }, around: 0.3, for_ms: 200 } }\n"));
    check_text(written_to(real, "Body.ratio", 100ms), written_to(wave, "Body.ratio", 100ms));
    CHECK(written_to(real, "Body.ratio", 100ms).find("@100>100") != std::string::npos);
}

TEST_CASE(scenario_dither_waits_for_the_first_value_and_a_dither_step_writes_once)
{
    // A declared dither rides from the first thing the scenario puts on the input: nothing is
    // written before the set at 30 ms.
    {
        World world;
        const ScenarioRun run = world.run(read(kDitherHead + "inputs:\n  Body.level: { dither: { square: { amplitude: 1, period_ms: 20 } } }\n"
                                                             "steps:\n  - at_ms: 30\n    set: { Body.level: 50 }\n"
                                                             "  - at_ms: 60\n    expect: { Body.level: 51 }\n"));
        check_text(written_to(world, "Body.level", 60ms), "49@30>30 51@40>40 49@50>50 51@60>60 ");
        check_text(scenario_status_name(run.status), "passed");
    }
    // A dither step on an input without a value writes nothing and passes at its time, whatever
    // the controller's step is; the dither is there when the set comes. A set and a dither step
    // at one time: both write, each the set's value with the dither that is put at that time
    // (the square is low at 50 ms). none writes the value alone, once.
    const Scenario s = read(kHead + "  - at_ms: 20\n"
                                    "    dither: { Body.level: { square: { amplitude: 1, period_ms: 20 } }, Door.level: none }\n"
                                    "  - at_ms: 30\n"
                                    "    set: { Body.level: 50 }\n"
                                    "  - at_ms: 50\n"
                                    "    set: { Body.level: 70 }\n"
                                    "  - at_ms: 50\n"
                                    "    dither: { Body.level: { square: { amplitude: 5, period_ms: 20 } } }\n"
                                    "  - at_ms: 80\n"
                                    "    dither: { Body.level: none }\n"
                                    "  - at_ms: 120\n"
                                    "    expect: { Body.level: 70 }\n");
    for (const SimDuration tick : {5ms, 10ms, 30ms})
    {
        World world;
        world.tick = tick;
        const ScenarioRun run = world.run(s);
        const std::string where = "tick " + std::to_string(tick.count());
        check_text(text_of(run).substr(text_of(run).find('\n') + 1), "3 passed start=20 at=20 end=20\n"
                                                                      "5 passed start=30 at=30 end=30\n"
                                                                      "7 passed start=50 at=50 end=50\n"
                                                                      "9 passed start=50 at=50 end=50\n"
                                                                      "11 passed start=80 at=80 end=80\n"
                                                                      "13 passed start=120 at=120 end=120\n",
                   where.c_str());
        check_text(written_to(world, "Body.level", 120ms), "49@30>30 51@40>40 65@50>50 65@50>50 75@60>60 65@70>70 70@80>80 ", where.c_str());
        check_text(written_to(world, "Door.level", 120ms), "", where.c_str());
    }
}

TEST_CASE(scenario_dither_write_that_is_refused_fails_the_first_step_that_is_not_decided)
{
    // 99 with a square of 2 that is low for 50 ms and then high: the write of 50 ms is 101, which
    // Body refuses when it gets it. It is no step's write. The set passed long before; the step
    // that fails is the first that is not decided when the launcher sends the write, which
    // depends on the controller's step: every 10 ms the expect of 30 ms has passed, every 30 ms
    // it has not (the write of 50 ms goes out at 30 ms, with those up to 60 ms).
    const std::string head = kDitherHead + "inputs:\n"
                                           "  Body.level: { dither: { square: { amplitude: 2, period_ms: 100, phase_deg: 180 } } }\n"
                                           "steps:\n"
                                           "  - set: { Body.level: 99 }\n"
                                           "  - at_ms: 30\n"
                                           "    expect: { Body.level: 97 }\n";
    const Scenario two = read(head + "  - at_ms: 60\n    expect: { Body.level: 97 }\n");
    const std::string message = "scenario-write: the dither of Body.level (line 3) wrote 101 at 50 ms, above its range 0 to 100: lower "
                                "its amplitude, or keep the value further from the end of the range\n";
    {
        World world;
        world.tick = 10ms;
        check_text(text_of(world.run(two)), "failed over=40\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 passed start=30 at=30 end=30\n"
                                            "8 failed start=60 at=50 end=60 " + message);
    }
    {
        World world;
        world.tick = 30ms;
        check_text(text_of(world.run(two)), "failed over=30\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 failed start=30 at=50 end=30 " + message +
                                            "8 not_run start=- at=- end=-\n");
    }
    // The same in the last step of the controller before the scenario is over: with a step of
    // 10 ms the scenario has passed before the write of 50 ms is sent, with 30 ms it fails.
    const Scenario one = read(head);
    {
        World world;
        world.tick = 10ms;
        check_text(text_of(world.run(one)), "passed over=40\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 passed start=30 at=30 end=30\n");
    }
    {
        World world;
        world.tick = 30ms;
        check_text(text_of(world.run(one)), "failed over=30\n"
                                            "5 passed start=0 at=0 end=0\n"
                                            "6 failed start=30 at=50 end=30 " + message);
    }
    // Refused by the ECU in its step (the setter of Body.level throws at 13): known when the
    // result is judged. The dithers of one time for one participant are one command, so the
    // message says how many other inputs were in it.
    const Scenario rejected = read(kDitherHead + "inputs:\n"
                                                 "  Body.ratio: { dither: { square: { amplitude: 0.1, period_ms: 20 } } }\n"
                                                 "  Body.level: { dither: { square: { amplitude: 1, period_ms: 40, phase_deg: 180 } } }\n"
                                                 "  Door.level: { dither: { square: { amplitude: 1, period_ms: 40 } } }\n"
                                                 "steps:\n"
                                                 "  - set: { Body.level: 12, Body.ratio: 0.5, Door.level: 5 }\n"
                                                 "  - at_ms: 100\n"
                                                 "    expect: { Body.level: 11 }\n");
    World world;
    check_text(text_of(world.run(rejected)), "failed over=30\n"
                                             "7 passed start=0 at=0 end=0\n"
                                             "8 failed start=100 at=20 end=100 scenario-write: the dither of Body.ratio (line 3) wrote 0.6 at 20 ms, "
                                             "and participant 'Body' did not apply the write: the ECU rejected it (a setter threw: 13 is not a "
                                             "level); the write was one command with those of 1 other dithered input of that participant\n");
    check_text(written_to(world, "Door.level", 20ms), "6@0>0 6@10>10 4@20>20 ");
}

TEST_CASE(scenario_dither_errors_say_what_is_wrong_and_what_to_write)
{
    const auto errors = [](const std::string& text) { return lines(read_scenario(kDitherHead + text, catalog()).findings); };
    const std::string steps = "steps:\n  - set: { Body.level: 1 }\n";
    // What a wave has and a dither cannot have.
    check_text(errors("inputs:\n"
                      "  Body.level: { dither: { triangle: { amplitude: 1, period_ms: 100 }, for_ms: 500 } }\n"
                      "  Body.ratio: { dither: { sine: { amplitude: 0.1, period_ms: 100 }, around: 0.5 } }\n"
                      "  Door.level: { dither: { pulse: { value: 1, width_ms: 50 } } }\n"
                      "  Door.ratio: { dither: { square: { low: 0, high: 1, period_ms: 100 } } }\n" +
                      steps),
               "E 3 value: the dither of Body.level has for_ms: a dither has no end, it stays until a dither step changes it; remove "
               "for_ms (dither: { Body.level: none } takes it off)\n"
               "E 4 value: the dither of Body.ratio has around: a dither swings around the value the scenario puts on the input (set, "
               "ramp, wave); remove around\n"
               "E 5 value: pulse cannot be a dither (it has a length): use sine, triangle, sawtooth, square with amplitude or noise, or "
               "write it as a wave step\n"
               "E 6 value: the dither of Door.ratio is a square with low: a dither swings around the value of the input, it has no "
               "values of its own; give the square amplitude instead (square: { amplitude: 1, period_ms: 100 })\n");
    // The variable: a number, an input, one the configuration has.
    check_text(errors("inputs:\n"
                      "  Body.lamp: { dither: { square: { period_ms: 100 } } }\n"
                      "  Body.mode: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n"
                      "  Body.gain: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n"
                      "  Body.pulse: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n"
                      "  Body.levle: { dither: { triangle: { amplitude: 1, period_ms: 100 } } }\n" +
                      steps),
               "E 3 scenario-value: Body.lamp is a bool: a dither needs a number (an integer or a real input); remove it\n"
               "E 4 scenario-value: Body.mode is an enum: a dither needs a number (an integer or a real input); remove it\n"
               "E 5 scenario-not-writable: Body.gain is a parameter: it can be written only before the first step, that is with set in "
               "a step at 0 (at_ms: 0, or the first steps without a time), not with a dither\n"
               "E 6 scenario-not-writable: Body.pulse is an output of its ECU: it can be read (expect, wait_until), not written; write "
               "one of the ECU's inputs instead\n"
               "E 7 scenario-variable: 'Body.levle': participant 'Body' has no variable 'levle': did you mean 'Body.level'? "
               "(vehicle_sil_launch check --json lists the variables)\n");
    // The form of inputs, and of an entry. clip is of a later version: an unknown key here.
    check_text(errors("inputs: [Body.level]\n" + steps),
               "E 2 value: inputs must be a mapping of inputs to what is on top of them ({ Body.level: { dither: { triangle: { "
               "amplitude: 1, period_ms: 100 } } } }), not a list\n");
    check_text(errors("inputs:\n"
                      "  Body.level: { dither: { triangle: { amplitude: 1, period_ms: 100 } }, clip: true }\n"
                      "  Body.ratio: { undersampled: ok }\n"
                      "  Door.level: { dither: none }\n"
                      "  Door.ratio: { dither: { triangle: { amplitude: 0.1, period_ms: 100 } }, undersampled: yes }\n"
                      "  Door.level: { dither: { noise: { amplitude: 1, hold_ms: 10, seed: 1 } } }\n" +
                      steps),
               "E 3 unknown-key: inputs: Body.level: unknown key 'clip': remove it, or use one of the keys here (dither, undersampled)\n"
               "E 4 missing-key: inputs: Body.ratio has no dither: say what is on top of the input (dither: { triangle: { amplitude: "
               "1, period_ms: 100 } }), or remove the entry\n"
               "E 5 value: the dither of Door.level in inputs is none: an input without a dither needs no entry; remove it\n"
               "E 6 value: undersampled must be ok (run the input's dither as it is, also where it changes faster than its ECU steps), "
               "not 'yes': write undersampled: ok, or remove it\n"
               "E 7 duplicate-key: inputs: 'Door.level' appears twice (first at line 5); keep one of them\n");
    // The step: none or a shape, one shape, the shape's own keys.
    check_text(errors("steps:\n"
                      "  - dither: { Body.level: off }\n"
                      "  - dither: { Body.level: false, Body.ratio: \"none\" }\n" // (none in quotes is none: JSON has no other way)
                      "  - dither: Body.level\n"
                      "  - dither: { Body.level: { } }\n"
                      "  - dither: { Body.level: { sine: { amplitude: 1, period_ms: 100 }, noise: { amplitude: 1, hold_ms: 10, seed: 1 } } }\n"
                      "  - dither: { Body.level: { triangle: { amplitude: 1 } } }\n"
                      "  - dither: { Body.level: { triangle: { amplitude: 1, period_ms: 100, duty: 10 } } }\n"
                      "  - dither: { Body.level: { wobble: { amplitude: 1 } } }\n"
                      "  - dither: { Body.level: none }\n"
                      "    hold_for_ms: 10\n"),
               "E 3 value: the dither of Body.level must be none (which takes the dither off) or a shape ({ triangle: { amplitude: 1, "
               "period_ms: 100 } }), not 'off': write none, or a shape\n"
               "E 4 value: the dither of Body.level must be none (which takes the dither off) or a shape ({ triangle: { amplitude: 1, "
               "period_ms: 100 } }), not 'false': write none, or a shape\n"
               "E 5 value: dither must be a mapping of inputs to a shape or none ({ Body.level: { triangle: { amplitude: 1, period_ms: "
               "100 } } }, or { Body.level: none } to take a dither off), not 'Body.level'\n"
               "E 6 value: the dither of Body.level names no shape: give it one of sine, triangle, sawtooth, square, noise\n"
               "E 7 value: the dither of Body.level has sine, noise: an input has one dither; keep one of them\n"
               "E 8 missing-key: the triangle dither of Body.level needs period_ms or frequency_hz: how long one period is, or how many "
               "there are in a second\n"
               "E 9 unknown-key: the triangle dither of Body.level: unknown key 'duty': remove it, or use one of the keys here "
               "(amplitude, period_ms, frequency_hz, phase_deg)\n"
               "E 10 unknown-key: dither of Body.level: unknown key 'wobble': remove it, or use one of the keys here (sine, triangle, "
               "sawtooth, square, noise)\n"
               "E 12 value: hold_for_ms goes with expect, not with dither: remove it\n");
    // A parameter cannot stand for none or for the shape.
    check_text(errors("params: { what: none }\nsteps:\n  - dither: { Body.level: \"${what}\" }\n"),
               "E 4 value: the dither of Body.level is the parameter what: a parameter cannot stand for none or for a shape; write none "
               "or the shape here (its numbers can be parameters)\n");
}

TEST_CASE(scenario_dither_of_a_variable_the_configuration_does_not_have_is_skipped)
{
    // A dither step that uses an absent variable is skipped whole, as a set is; a declared
    // dither of one is left out, and a note says so. The other inputs keep theirs.
    ScenarioContext context;
    context.variant = "small";
    const ScenarioRead r = read_scenario(kDitherHead + "inputs:\n"
                                                       "  Body.boost: { dither: { triangle: { amplitude: 1, period_ms: 40 } } }\n"
                                                       "  Body.level: { dither: { square: { amplitude: 1, period_ms: 20 } } }\n"
                                                       "steps:\n"
                                                       "  - set: { Body.level: 5 }\n"
                                                       "  - at_ms: 20\n"
                                                       "    dither: { Body.boost: none, Body.level: none }\n"
                                                       "  - at_ms: 40\n"
                                                       "    expect: { Body.level: 6 }\n",
                                         catalog_without_boost(), context);
    check_text(lines(r.findings),
               "N 3 scenario-input-skipped: the dither of Body.boost is not used: Body.boost is absent: participant 'Body' does not "
               "have it in the variant small (absent: true in its variables); the steps that write Body.boost are skipped too\n"
               "N 7 scenario-step-skipped: the step is skipped (dither Body.boost; Body.level none): Body.boost is absent: participant "
               "'Body' does not have it in the variant small (absent: true in its variables); the other steps run, also those that "
               "would need what this one does\n");
    if (!r.scenario)
    {
        throw unit::Failure("no scenario");
    }
    CHECK(r.scenario->inputs.size() == 1 && r.skipped == 1);
    World world;
    const ScenarioRun run = world.run(*r.scenario);
    check_text(text_of(run), "passed over=50\n"
                             "6 passed start=0 at=0 end=0\n"
                             "7 skipped start=20 at=- end=20 scenario-skipped: Body.boost is absent: participant 'Body' does not have it in "
                             "the variant small (absent: true in its variables)\n"
                             "9 passed start=40 at=40 end=40\n");
    // The skipped step took nothing off: the dither goes on.
    check_text(written_to(world, "Body.level", 40ms), "6@0>0 4@10>10 6@20>20 4@30>30 6@40>40 ");
}
