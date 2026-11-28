// SPDX-License-Identifier: MIT
//
// A model of ECUs that steps in virtual time (World), the catalog of its variables and the readers
// of a scenario against it: what the tests of the scenario's engine and of the timeline run
// against. Shared by tests/scenario and by the timeline file of the launcher's library
// (tests/launch), which runs the same model through the output.
#pragma once

#include "launch_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/scenario/scenario.hpp"
#include "vehicle_sil/scenario/scenario_run.hpp"
#include "vehicle_sil/scenario/timeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::scenario::test::scenario_model {

using namespace std::chrono_literals;

inline SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

inline std::int64_t ms_of(SimTime t)
{
    return to_ns(t) / 1'000'000;
}

/// The variables of the model ECU, in the order of its samples.
inline std::vector<control::VariableInfo> model_variables()
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

inline ScenarioCatalog catalog(SimDuration body_step = 10ms, SimDuration door_step = 10ms)
{
    ScenarioCatalog out;
    out.push_back(CatalogParticipant{"Body", "", body_step, model_variables()});
    out.push_back(CatalogParticipant{"Door", "", door_step, model_variables()});
    out.push_back(CatalogParticipant{"Spare", "is left out of this run (enabled: false, or --disable)", SimDuration{}, {}});
    return out;
}

inline Scenario read(const std::string& text, const ScenarioCatalog& c = catalog())
{
    ScenarioRead r = read_scenario(text, c);
    if (!r.scenario)
    {
        throw unit::Failure("the scenario has errors:\n" + config::test::lines(r.findings));
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
    SimDuration sample_period{10ms};
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
    /// Per settle() of the engine: how many writes it waited for.
    std::vector<std::size_t> settles;
    ScenarioEngine* engine = nullptr;
    const Scenario* scenario = nullptr;
    /// What the engine notes for a timeline, if set (run()).
    TimelineRecord* record = nullptr;
    ScenarioEngine::OnStep on_step{};
    /// If set: what `peer` answers at once to a command for the time `at` instead of taking it
    /// (its status and reason, and failed_write if it is about one write; empty: it takes the
    /// command as ever).
    std::function<std::optional<control::TargetResult>(const std::string& peer, SimTime at)> refuse;
    /// What `refuse` answers is the hub's own refusal (no link, no room): known when write()
    /// returns, not only after settle().
    bool refused_by_hub = false;
    /// The results of the writes for this time are forgotten by the time the engine has waited
    /// (as a hub that keeps too few).
    std::optional<SimTime> forgets{};

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
        std::map<std::string, std::uint32_t> positions; // by peer: of the next write in its command
        for (const auto& w : writes)
        {
            auto target = std::find_if(op.targets.begin(), op.targets.end(), [&w](const auto& t) { return t.peer == w.peer; });
            if (target == op.targets.end())
            {
                op.targets.push_back(control::TargetResult{w.peer, control::TargetStatus::Queued, std::nullopt, false, ""});
                target = op.targets.end() - 1;
            }
            const std::uint32_t position = positions[w.peer]++;
            const auto ecu = std::find_if(ecus.begin(), ecus.end(), [&w](const ModelEcu& e) { return e.name == w.peer; });
            if (ecu == ecus.end())
            {
                target->status = control::TargetStatus::Invalid;
                target->reason = "unknown peer";
                target->applied_writes = 0;
                continue;
            }
            if (const auto refused = refuse ? refuse(w.peer, time) : std::nullopt)
            {
                target->status = refused->status;
                target->reason = refused->reason;
                target->applied_writes = 0;
                target->failed_write = refused->failed_write;
                continue;
            }
            if (ecu->last_step && std::any_of(writes.begin(), writes.end(), [&w](const control::PeerWrite& other) {
                    return other.peer == w.peer && other.name == "gain";
                }))
            {
                // As control::Agent::submit(): a command that writes a parameter and arrives after
                // the first step is refused as a whole when it arrives (no write is the one).
                target->status = control::TargetStatus::Invalid;
                target->reason = "a parameter is writable only before the first step";
                target->applied_writes = 0;
                continue;
            }
            if (w.name == "level" && (std::get<std::int64_t>(w.value) < 0 || std::get<std::int64_t>(w.value) > 100))
            {
                // The agent's check: the hub knows the write from the agent's number.
                target->status = control::TargetStatus::Invalid;
                target->reason = "level: out of range";
                target->applied_writes = 0;
                target->failed_write = position;
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
        // What the agents answered is known once the engine has waited (settle()); until then
        // every target is on its way, as in the hub.
        control::OperationResult sent = op;
        for (auto& target : sent.targets)
        {
            const bool known = refused_by_hub && control::is_final(target.status) && target.status != control::TargetStatus::Applied;
            if (!known)
            {
                target = control::TargetResult{target.peer, control::TargetStatus::Sent, std::nullopt, false, ""};
            }
        }
        visible_[ticket] = std::move(sent);
        times_[ticket] = time;
        unsettled_.emplace_back(ticket, std::move(op));
        return ticket;
    }

    void settle() override
    {
        settles.push_back(unsettled_.size());
        for (auto& [ticket, op] : unsettled_)
        {
            if (forgets && times_[ticket] == *forgets)
            {
                visible_.erase(ticket);
                continue;
            }
            visible_[ticket] = std::move(op);
        }
        unsettled_.clear();
    }

    std::optional<control::OperationResult> result(std::uint64_t ticket) override
    {
        const auto it = visible_.find(ticket);
        return it == visible_.end() ? std::nullopt : std::optional<control::OperationResult>(it->second);
    }

    std::optional<control::OperationResult> wait_done(std::uint64_t ticket) override
    {
        const auto is_done = [](const std::optional<control::OperationResult>& r) {
            return !r || std::all_of(r->targets.begin(), r->targets.end(),
                                     [](const control::TargetResult& t) { return control::is_final(t.status); });
        };
        auto r = result(ticket);
        if (!is_done(r))
        {
            run_steps([this](const ModelEcu& m) { return m.next_step <= current_p_; });
            deliver([this](SimTime t) { return t <= current_p_; });
            r = result(ticket);
        }
        return r;
    }

    /// Runs the scenario to its end (or `limit`).
    ScenarioRun run(const Scenario& s, SimDuration limit = 5s)
    {
        scenario = &s;
        ScenarioEngine e(s, tick, *this, on_step, record, stop_at);
        engine = &e;
        std::vector<EnginePeer> peers;
        for (const auto& p : s.participants)
        {
            EnginePeer peer;
            peer.connected = std::any_of(ecus.begin(), ecus.end(), [&p](const ModelEcu& m) { return m.name == p.name; });
            peer.schema = model_variables();
            peers.push_back(std::move(peer));
        }
        if (!e.extra_participants().empty())
        {
            std::vector<EnginePeer> extraPeers;
            extraPeers.reserve(e.extra_participants().size());
            for (const std::string& pName : e.extra_participants())
            {
                EnginePeer peer;
                peer.connected = std::any_of(ecus.begin(), ecus.end(), [&pName](const ModelEcu& m) { return m.name == pName; });
                peer.schema = model_variables();
                extraPeers.push_back(std::move(peer));
            }
            e.begin_extra(extraPeers);
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
                current_p_ = p;
                e.stopped_at(p);
                break;
            }
            current_p_ = p;
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
            // As control::Hub fills them for an agent (see TargetResult): the positions count in
            // this command.
            const auto n = static_cast<std::uint32_t>(it->writes.size());
            control::TargetResult r{m.name, control::TargetStatus::Applied, now, false, "", n};
            if (m.last_step && *m.last_step >= it->at)
            {
                r = control::TargetResult{m.name, control::TargetStatus::Late, std::nullopt, true, "its time had passed", 0U};
            }
            else if (m.last_step &&
                     std::any_of(it->writes.begin(), it->writes.end(), [](const auto& w) { return w.first == "gain"; }))
            {
                // A command taken before the first step that writes a parameter at a later step:
                // refused as a whole, no setter is called.
                r = control::TargetResult{m.name, control::TargetStatus::Rejected, std::nullopt, false,
                                          "a parameter is writable only before the first step", 0U,
                                          n == 1 ? std::optional<std::uint32_t>(0) : std::nullopt};
            }
            else
            {
                std::uint32_t position = 0;
                for (const auto& [name, value] : it->writes)
                {
                    if (name == "level" && std::get<std::int64_t>(value) == 13)
                    {
                        r = control::TargetResult{m.name, control::TargetStatus::Rejected, std::nullopt, false,
                                                  "level: a setter threw: 13 is not a level", position, position};
                        break;
                    }
                    ++position;
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
        if (to_ns(now) % m.sample_period.count() == 0)
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
            const auto& extras = engine->extra_participants();
            for (std::size_t i = 0; i < extras.size(); ++i)
            {
                if (extras[i] == ecus[it->ecu].name && it->sample)
                {
                    engine->extra_sample(i, it->sample);
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
    std::vector<std::pair<std::uint64_t, control::OperationResult>> unsettled_; ///< in the order written
    std::map<std::uint64_t, SimTime> times_;                                     ///< of each write
    std::vector<Produced> produced_;
    SimTime current_p_{};
};

inline std::string time_text(const std::optional<SimTime>& t)
{
    return t ? std::to_string(ms_of(*t)) : "-";
}

/// One line per step: "<line> <status> start=<ms> at=<ms> end=<ms>[ <code>: <message>]".
inline std::string text_of(const ScenarioRun& run)
{
    std::string out = std::string(scenario_status_name(run.status)) + " over=" + time_text(run.over_at) + "\n";
    for (const auto& s : run.steps)
    {
        out += std::to_string(s.line) + " " + step_status_name(s.status) + " start=" + time_text(s.start) + " at=" + time_text(s.at) +
               " end=" + time_text(s.end) + (s.code.empty() ? "" : " " + s.code + ": " + s.message) + "\n";
    }
    return out;
}

inline std::string joined(const std::vector<std::string>& items)
{
    std::string out;
    for (const auto& item : items)
    {
        out += item + "\n";
    }
    return out;
}

/// The ways the outputs of the steps at a controller step can reach the engine.
inline std::vector<std::pair<std::string, World::Policy>> policies()
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

/// A ScenarioContext whose files are `files` (by the path the scenario writes); `reads` counts the
/// reads of each.
inline ScenarioContext with_files(std::map<std::string, std::string> files, std::shared_ptr<std::map<std::string, int>> reads = {})
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


const std::string kDitherHead = "vehicle_sil_scenario: 1\n";

/// The writes that were applied to one variable, "value@time of the write>step that applied it"
/// each, up to the writes for `until`.
inline std::string written_to(const World& world, const std::string& variable, SimDuration until)
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
inline std::string steps_without_one_write(const World& world, const std::string& variable, SimDuration from, SimDuration until, SimDuration step)
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

} // namespace vehicle_sil::scenario::test::scenario_model
