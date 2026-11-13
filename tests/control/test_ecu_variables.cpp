// SPDX-License-Identifier: MIT
//
// Control variables declared on vehicle_sil::Ecu (input(), output(), agent()): one ECU class run
// by Simulation on the local virtual buses and on SIL Kit (registry of this process) takes the
// same timed writes at the same steps, before the application, and is sampled at the same times,
// after the application; an ECU without variables has no agent; the set-up errors name the ECU;
// the agent goes after the networks and before the emplace()d objects its variables refer to.
#include "test_harness.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::Ecu;
using vehicle_sil::SilKitOptions;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::Simulation;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;
namespace control = vehicle_sil::control;

namespace {

SimTime at(std::chrono::nanoseconds t) { return SimTime{} + t; }
long long ms(SimTime t) { return (t - SimTime{}) / 1ms; }

enum class Gear : std::int8_t
{
    Park = 0,
    Drive = 1,
    Reverse = 2,
};

/// What the application saw in one step: (time, target, gear).
using Row = std::tuple<long long, double, int>;

/// An ECU with two inputs (a real one through a getter and a setter, an enum one) and two
/// outputs (one bound to a member, one through a getter). Its application records what it sees
/// in every step and moves `position` by `target`.
class Plant
{
public:
    explicit Plant(Ecu& ecu)
    {
        ecu.input(
            "plant.target", [this] { return target_; }, [this](double v) { target_ = v; }, {.unit = "mm", .min = -100.0, .max = 100.0});
        ecu.input_enum("plant.gear", &gear_, {{"park", 0}, {"drive", 1}, {"reverse", 2}});
        ecu.output("plant.position", &position_, {.unit = "mm"});
        ecu.output("plant.steps", [this] { return steps_; });
        ecu.every(10ms, [this](SimTime now) {
            rows.emplace_back(ms(now), target_, static_cast<int>(gear_));
            position_ += target_;
            ++steps_;
        });
    }
    std::vector<Row> rows;

private:
    double target_ = 0.0;
    Gear gear_ = Gear::Park;
    double position_ = 0.0;
    std::uint32_t steps_ = 0;
};

/// An ECU without variables.
class Idle
{
public:
    explicit Idle(Ecu& ecu)
    {
        ecu.every(10ms, [this](SimTime) { ++steps; });
    }
    int steps = 0;
};

struct PlantRun
{
    std::vector<Row> rows;
    std::vector<control::CommandResult> results;
    control::Snapshot last;
    std::vector<control::VariableInfo> schema;
};

/// Writes at a step boundary (30 ms), between two boundaries (55 ms) and two for the same step
/// (72 ms and 78 ms: both at 80 ms, in the order of their times), all queued before the run.
PlantRun run_plant(std::optional<SilKitOptions> silkit)
{
    PlantRun out;
    Simulation sim(silkit);
    Ecu& ecu = sim.add_ecu("Plant", 10ms);
    auto& plant = ecu.emplace<Plant>(ecu);
    Ecu& idle_ecu = sim.add_ecu("Idle", 10ms);
    auto& idle = idle_ecu.emplace<Idle>(idle_ecu);
    auto& agent = ecu.agent();
    const auto submit = [&](control::Command command) { CHECK(agent.submit(std::move(command)).status == control::SubmitStatus::Queued); };
    submit({1, {{"plant.target", 1.5}, {"plant.gear", std::int64_t{1}}}, at(30ms)});
    submit({2, {{"plant.target", 2.5}}, at(55ms)});
    submit({4, {{"plant.target", 7.0}}, at(78ms)});
    submit({3, {{"plant.target", 5.0}, {"plant.gear", std::int64_t{2}}}, at(72ms)});
    sim.run_for(150ms);
    CHECK_EQ(idle.steps, 15);
    // An ECU without variables has an agent from the run on, with ecu.connected only.
    const auto& idle_schema = idle_ecu.agent().schema();
    CHECK(!idle_schema.empty() && idle_schema.front().name == "ecu.connected");
    CHECK(silkit || idle_schema.size() == 1); // a peer adds its monitoring outputs
    agent.drain_results(out.results);
    out.rows = plant.rows;
    out.last = agent.latest();
    out.schema = agent.schema();
    return out;
}

/// The value of `name` in `snapshot`.
control::Value value_of(const PlantRun& run, const std::string& name)
{
    for (std::size_t i = 0; i < run.schema.size(); ++i)
    {
        if (run.schema[i].name == name)
        {
            return run.last.values.at(i);
        }
    }
    throw vehicle_sil::unit::Failure("no variable " + name);
}

void check_plant(const PlantRun& run)
{
    // Each write takes effect in the first step at or after its time, before the application.
    CHECK_EQ(run.rows.size(), std::size_t{15});
    for (const auto& [t, target, gear] : run.rows)
    {
        const double expected_target = t < 30 ? 0.0 : t < 60 ? 1.5 : t < 80 ? 2.5 : 7.0;
        const int expected_gear = t < 30 ? 0 : t < 80 ? 1 : 2;
        CHECK_EQ(target, expected_target);
        CHECK_EQ(gear, expected_gear);
    }
    CHECK_EQ(run.results.size(), std::size_t{4});
    for (const auto& r : run.results)
    {
        CHECK(r.status == control::CommandStatus::Applied);
        const long long expected = r.seq == 1 ? 30 : r.seq == 2 ? 60 : 80;
        CHECK(r.applied_at.has_value() && ms(*r.applied_at) == expected);
    }
    // The last sample (at 140 ms, after the application): 15 steps; position 3 * 1.5 + 2 * 2.5 + 7 * 7.
    CHECK_EQ(ms(run.last.time), 140LL);
    CHECK_EQ(std::get<double>(value_of(run, "plant.target")), 7.0);
    CHECK_EQ(std::get<std::int64_t>(value_of(run, "plant.gear")), std::int64_t{2});
    CHECK_EQ(std::get<double>(value_of(run, "plant.position")), 3 * 1.5 + 2 * 2.5 + 7 * 7.0);
    CHECK_EQ(std::get<std::int64_t>(value_of(run, "plant.steps")), std::int64_t{15});
    // ecu.connected first (every ECU has it), then the ECU's variables in the order declared.
    CHECK(run.schema.size() >= 5 && run.schema[0].name == "ecu.connected" && run.schema[1].name == "plant.target"
          && run.schema[2].name == "plant.gear" && run.schema[3].name == "plant.position" && run.schema[4].name == "plant.steps");
    CHECK(run.schema[2].type == control::ValueType::Enum && run.schema[2].items.size() == 3);
    CHECK(run.schema[4].causality == control::Causality::Output);
}

/// Records the times of the samples and the steps the application had run by then.
class Sampled
{
public:
    Sampled(Ecu& ecu, std::optional<SimDuration> period)
    {
        if (period)
        {
            ecu.set_agent_options({.sample_period = *period});
        }
        ecu.output("sampled.steps", [this] {
            samples.emplace_back(ms(now_), steps_);
            return steps_;
        });
        ecu.on_step([this](SimTime now) {
            now_ = now;
            ++steps_;
        });
    }
    std::vector<std::pair<long long, int>> samples; ///< (time, steps run including that one)

private:
    SimTime now_{};
    int steps_ = 0;
};

std::vector<std::pair<long long, int>> run_sampled(std::optional<SilKitOptions> silkit, std::optional<SimDuration> period)
{
    Simulation sim(silkit);
    auto& sampled = sim.add<Sampled>("Sampled", 4ms, period);
    sim.run_for(50ms);
    return sampled.samples;
}

/// Records whether the model was still there when the agent let go of the getter.
struct Lifetimes
{
    bool model_alive = false;
    bool getter_released = false;
    bool model_alive_at_release = false;
};

class Model
{
public:
    explicit Model(Ecu& ecu, Lifetimes& lifetimes) : lifetimes_(lifetimes)
    {
        lifetimes_.model_alive = true;
        /// Shared by every copy of the getter: destroyed with the last one, the agent's.
        struct Release
        {
            Model* model;
            Lifetimes* lifetimes;
            ~Release()
            {
                lifetimes->getter_released = true;
                lifetimes->model_alive_at_release = lifetimes->model_alive && model->value_ >= 0.0; // reads the model
            }
        };
        const std::shared_ptr<Release> release(new Release{this, &lifetimes_});
        ecu.output("model.value", [this, release] {
            static_cast<void>(release);
            return value_;
        });
        ecu.every(10ms, [this](SimTime) { value_ += 1.0; });
    }
    ~Model() { lifetimes_.model_alive = false; }
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

private:
    Lifetimes& lifetimes_;
    double value_ = 0.0;
};

} // namespace

TEST_CASE(ecu_variables_take_writes_before_the_application_locally)
{
    check_plant(run_plant(std::nullopt));
}

TEST_CASE(ecu_variables_take_writes_before_the_application_on_silkit)
{
    check_plant(run_plant(SilKitOptions{}));
}

TEST_CASE(ecu_variables_are_the_same_locally_and_on_silkit)
{
    const auto local = run_plant(std::nullopt);
    const auto silkit = run_plant(SilKitOptions{});
    check_plant(local); // not empty, and as expected
    check_plant(silkit);
    CHECK(local.rows == silkit.rows);
    CHECK(local.last.time == silkit.last.time);
    CHECK(local.last.sequence == silkit.last.sequence);
    // The local agent has ecu.connected and the ECU's variables only; the peer's adds its
    // monitoring outputs after them.
    CHECK_EQ(local.schema.size(), std::size_t{5});
    CHECK(silkit.schema.size() > 5);
    for (std::size_t i = 0; i < local.schema.size(); ++i)
    {
        CHECK(local.schema[i].name == silkit.schema[i].name);
        CHECK(local.last.values.at(i) == silkit.last.values.at(i));
    }
    CHECK_EQ(local.results.size(), silkit.results.size());
    for (std::size_t i = 0; i < local.results.size() && i < silkit.results.size(); ++i)
    {
        CHECK_EQ(local.results[i].seq, silkit.results[i].seq);
        CHECK(local.results[i].applied_at == silkit.results[i].applied_at);
    }
}

TEST_CASE(ecu_variables_are_sampled_after_the_application_at_the_same_times)
{
    // Step 4 ms. Period 0: every step. The default (10 ms): the first step at or after 0, 10, 20, ...
    std::vector<std::pair<long long, int>> every_step;
    for (long long t = 0; t < 50; t += 4)
    {
        every_step.emplace_back(t, static_cast<int>(t / 4) + 1);
    }
    const std::vector<std::pair<long long, int>> by_default{{0, 1}, {12, 4}, {20, 6}, {32, 9}, {40, 11}};
    for (const bool silkit : {false, true})
    {
        std::optional<SilKitOptions> where;
        if (silkit)
        {
            where.emplace();
        }
        CHECK(run_sampled(where, SimDuration::zero()) == every_step);
        CHECK(run_sampled(where, std::nullopt) == by_default);
    }
}

TEST_CASE(ecu_variables_set_up_errors_name_the_ecu)
{
    double x = 0.0;
    for (const bool silkit : {false, true})
    {
        std::optional<SilKitOptions> where;
        if (silkit)
        {
            where.emplace();
        }
        Simulation sim(where);
        Ecu& ecu = sim.add_ecu("Pump", 10ms);
        Ecu& other = sim.add_ecu("Valve", 10ms);
        // Bad options are refused where they are set, with the ECU named; the options stay.
        CHECK(throws_with<std::invalid_argument>([&] { other.set_agent_options({.max_pending = 0}); }, "\"Valve\""));
        CHECK(throws_with<std::invalid_argument>([&] { other.set_agent_options({.sample_period = -1ms}); }, "sample_period"));
        other.set_agent_options({.sample_period = 20ms});
        other.input("valve.x", &x);
        CHECK(other.agent().options().sample_period == 20ms);
        // Options only before the agent is made.
        ecu.input("pump.x", &x);
        CHECK(throws_with<std::logic_error>([&] { ecu.set_agent_options({}); }, "\"Pump\""));
        CHECK(throws_with<std::logic_error>([&] { ecu.set_agent_options({}); }, "before the first input()"));
        // A name once per ECU (another ECU may use it).
        CHECK(throws_with<std::invalid_argument>([&] { ecu.output("pump.x", &x); }, "\"Pump\""));
        CHECK(throws_with<std::invalid_argument>([&] { ecu.output("pump.x", &x); }, "'pump.x'"));
        other.output("pump.x", &x);
        Ecu& bare = sim.add_ecu("Bare", 10ms);
        sim.run_for(30ms);
        // Nothing is declared once the run has started.
        CHECK(throws_with<std::logic_error>([&] { ecu.input("pump.y", &x); }, "\"Pump\""));
        CHECK(throws_with<std::logic_error>([&] { ecu.output("pump.y", &x); }, "output()"));
        CHECK(throws<std::logic_error>([&] { ecu.set_agent_options({}); }));
        CHECK(throws<std::logic_error>([&] { other.set_agent_options({}); }));
        // The agent stays reachable (ecu.connected and pump.x, and the peer's monitoring outputs);
        // an ECU without variables has one too, made at the start.
        CHECK(silkit ? ecu.agent().schema().size() > 2 : ecu.agent().schema().size() == 2);
        CHECK(bare.agent().schema().front().name == "ecu.connected");
    }
}

TEST_CASE(ecu_variables_agent_goes_before_the_emplaced_objects)
{
    for (const bool silkit : {false, true})
    {
        Lifetimes lifetimes;
        {
            std::optional<SilKitOptions> where;
            if (silkit)
            {
                where.emplace();
            }
            Simulation sim(where);
            sim.add<Model>("Model", 10ms, lifetimes);
            sim.run_for(50ms);
            CHECK(!lifetimes.getter_released);
        }
        CHECK(lifetimes.getter_released);
        CHECK(lifetimes.model_alive_at_release);
        CHECK(!lifetimes.model_alive);
    }
}

TEST_CASE(ecu_variables_simulation_finds_an_ecu_by_name)
{
    // After add<App>(), Simulation::ecu(name) reaches the ECU and its agent, on both.
    for (const bool silkit : {false, true})
    {
        std::optional<SilKitOptions> where;
        if (silkit)
        {
            where.emplace();
        }
        Simulation sim(where);
        auto& plant = sim.add<Plant>("Plant", 10ms);
        sim.add<Idle>("Idle", 10ms);
        auto& agent = sim.ecu("Plant").agent();
        CHECK(agent.submit({1, {{"plant.target", 4.0}}, at(20ms)}).status == control::SubmitStatus::Queued);
        CHECK(throws_with<std::out_of_range>([&] { static_cast<void>(sim.ecu("Pump")); }, "\"Pump\""));
        CHECK(throws_with<std::out_of_range>([&] { static_cast<void>(sim.ecu("Pump")); }, "\"Plant\", \"Idle\""));
        sim.run_for(40ms);
        CHECK_EQ(plant.rows.size(), std::size_t{4});
        CHECK(plant.rows.size() == 4 && std::get<1>(plant.rows[2]) == 4.0 && std::get<1>(plant.rows[1]) == 0.0);
        CHECK(&sim.ecu("Plant").agent() == &agent);
    }
    Simulation empty;
    CHECK(throws_with<std::out_of_range>([&] { static_cast<void>(empty.ecu("X")); }, "none"));
}

TEST_CASE(ecu_variables_a_transport_left_on_the_agent_is_found)
{
    // The ECU's agent holder reports a transport still attached (its destructor then writes this
    // to stderr and stops the process); none once the transport is gone.
    vehicle_sil::detail::EcuAgent holder("vehicle_sil::LocalBench: ECU \"Pump\": ", [](control::Agent& a) { a.bind_driver(); });
    CHECK(holder.transports_left().empty()); // no agent yet
    auto& agent = holder.get();
    CHECK(holder.transports_left().empty());
    {
        control::TransportLease lease(agent);
        const std::string why = holder.transports_left();
        CHECK(why.find("\"Pump\"") != std::string::npos);
        CHECK(why.find("destroy the transport before the ECU") != std::string::npos);
        CHECK(why.find("do not emplace()") != std::string::npos);
    }
    CHECK(holder.transports_left().empty());
    // Through an ECU of a Simulation: the same agent is counted.
    Simulation sim;
    Ecu& ecu = sim.add_ecu("Valve", 10ms);
    control::TransportLease lease(ecu.agent());
    CHECK_EQ(ecu.agent().transports(), std::size_t{1});
}
