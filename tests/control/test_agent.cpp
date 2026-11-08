// SPDX-License-Identifier: MIT
//
// vehicle_sil::control::Agent without a transport and without SIL Kit: the test drives the
// steps (on_step_begin/on_step_end) and plays the transport (submit/drain).
#include "test_harness.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/control/spsc_ring.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
using vehicle_sil::unit::throws;

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

enum class Gear : std::uint8_t
{
    P = 0,
    R = 1,
    N = 2,
    D = 3,
};

/// A small body model: what an application would own.
struct Model
{
    bool parking_brake = false;
    double coolant = 20.0;
    std::int16_t trim = 0;
    Gear gear = Gear::P;
    double mass = 1000.0;
    double speed = 0.0;
};

/// The model registered with an agent, and helpers to run steps and collect results.
struct Fixture
{
    Model model;
    Agent agent;
    std::vector<CommandResult> results;
    /// What the application saw at the start of each step (after on_step_begin).
    std::vector<std::pair<SimDuration, double>> seen_coolant;
    std::uint64_t seq = 0;

    explicit Fixture(AgentOptions options = {}) : agent(options)
    {
        agent.input("cabin.parking_brake", &model.parking_brake, {.description = "Parking brake switch"});
        agent.input("engine.coolant", &model.coolant, {.unit = "degC", .min = -40.0, .max = 210.0, .step = 0.5});
        agent.input("engine.trim", &model.trim);
        agent.input_enum("trans.gear", &model.gear, {{"P", 0}, {"R", 1}, {"N", 2}, {"D", 3}});
        agent.input("vehicle.mass", &model.mass, {.unit = "kg", .min = 500.0, .parameter = true});
        agent.output("vehicle.speed", [this] { return model.speed; }, {.unit = "km/h"});
    }

    SubmitResult write(std::string name, Value value, std::optional<SimTime> when = std::nullopt,
                       LatePolicy late = LatePolicy::Reject)
    {
        return agent.submit(Command{++seq, {{std::move(name), value}}, when, late});
    }

    /// One step; with `drain`, this thread also plays the transport and collects the results.
    void step(SimTime now, bool drain = true)
    {
        agent.on_step_begin(now);
        seen_coolant.emplace_back(now.time_since_epoch(), model.coolant); // the application
        model.speed += 1.0;
        agent.on_step_end(now);
        if (drain)
        {
            agent.drain_results(results);
        }
    }

    void run(SimDuration from, SimDuration to, SimDuration step_size)
    {
        for (auto t = from; t < to; t += step_size)
        {
            step(at(t));
        }
    }

    [[nodiscard]] const CommandResult& result(std::uint64_t s) const
    {
        for (const auto& r : results)
        {
            if (r.seq == s)
            {
                return r;
            }
        }
        throw vehicle_sil::unit::Failure("no result for seq " + std::to_string(s));
    }
};

} // namespace

TEST_CASE(control_spsc_ring_is_bounded_and_fifo)
{
    SpscRing<int> ring(3);
    CHECK_EQ(ring.capacity(), std::size_t{3});
    for (int i = 1; i <= 3; ++i)
    {
        CHECK(ring.try_push(i));
    }
    int extra = 4;
    CHECK(!ring.try_push(extra));
    CHECK_EQ(extra, 4); // untouched on failure
    CHECK_EQ(*ring.try_pop(), 1);
    CHECK(ring.try_push(extra));
    CHECK_EQ(*ring.try_pop(), 2);
    CHECK_EQ(*ring.try_pop(), 3);
    CHECK_EQ(*ring.try_pop(), 4);
    CHECK(!ring.try_pop().has_value());
    CHECK(throws<std::invalid_argument>([] { SpscRing<int> zero(0); }));
}

TEST_CASE(control_agent_schema_describes_the_variables)
{
    Fixture f;
    const auto& schema = f.agent.schema();
    CHECK_EQ(schema.size(), std::size_t{6});
    CHECK(schema[0].name == std::string("cabin.parking_brake"));
    CHECK(schema[0].type == ValueType::Bool);
    CHECK(schema[0].causality == Causality::Input);
    CHECK(schema[0].description == std::string("Parking brake switch"));
    CHECK(schema[1].type == ValueType::Real);
    CHECK(schema[1].unit == std::string("degC"));
    CHECK(schema[1].min == std::optional<double>(-40.0));
    CHECK(schema[1].max == std::optional<double>(210.0));
    CHECK(schema[1].step == std::optional<double>(0.5));
    CHECK(schema[2].type == ValueType::Integer);
    CHECK(schema[3].type == ValueType::Enum);
    CHECK_EQ(schema[3].items.size(), std::size_t{4});
    CHECK(schema[3].items[3].name == std::string("D"));
    CHECK(schema[4].causality == Causality::Parameter);
    CHECK(schema[5].causality == Causality::Output);
    CHECK(schema[5].unit == std::string("km/h"));
}

TEST_CASE(control_agent_registration_errors)
{
    Model m;
    Agent agent;
    agent.input("a", &m.coolant);
    CHECK(throws<std::invalid_argument>([&] { agent.input("a", &m.mass); }));
    CHECK(throws<std::invalid_argument>([&] { agent.input("", &m.mass); }));
    CHECK(throws<std::invalid_argument>([&] { agent.input("b", &m.mass, {.min = 2.0, .max = 1.0}); }));
    CHECK(throws<std::invalid_argument>([&] { agent.input_enum("c", &m.gear, {}); }));
    CHECK(throws<std::invalid_argument>([&] { agent.input_enum("c", &m.gear, {{"P", 0}, {"X", 300}}); })); // not a uint8_t
    CHECK(throws<std::invalid_argument>([&] { agent.input_enum("c", &m.gear, {{"P", 0}, {"P", 1}}); }));
    CHECK(throws<std::invalid_argument>([&] { agent.input_enum("c", &m.gear, {{"P", 0}, {"Q", 0}}); }));
    CHECK(throws<std::invalid_argument>([&] { agent.input("d", &m.mass, {.min = std::nan("")}); }));
    CHECK(throws<std::invalid_argument>([&] { agent.input("e", &m.trim, {.min = 1e19}); }));  // above int16
    CHECK(throws<std::invalid_argument>([&] { agent.input("e", &m.trim, {.max = -1e19}); })); // below int16
    std::uint64_t frames = 0;
    agent.output("frames", &frames); // a 64-bit unsigned output is fine (saturated when read)
    agent.on_step_begin(SimTime{});
    CHECK(throws<std::logic_error>([&] { agent.input("late", &m.mass); }));
    CHECK(throws<std::logic_error>([&] { agent.on_step_begin(SimTime{}); })); // time must increase
    CHECK(throws<std::invalid_argument>([] { Agent bad(AgentOptions{.max_pending = 0}); }));
    CHECK(throws<std::invalid_argument>([] { Agent bad(AgentOptions{.max_events = 0}); }));
}

TEST_CASE(control_agent_rejects_invalid_writes_at_submit)
{
    Fixture f;
    const auto invalid = [&](SubmitResult r) { return r.status == SubmitStatus::Invalid && !r.reason.empty(); };
    CHECK(invalid(f.write("nope", 1.0)));
    CHECK(invalid(f.write("vehicle.speed", 1.0)));                  // output
    CHECK(invalid(f.write("cabin.parking_brake", std::int64_t{1}))); // bool needs bool
    CHECK(invalid(f.write("engine.coolant", true)));
    CHECK(invalid(f.write("engine.coolant", 300.0))); // > max
    CHECK(invalid(f.write("engine.coolant", -41.0))); // < min
    CHECK(invalid(f.write("engine.coolant", std::nan(""))));
    CHECK(invalid(f.write("engine.trim", 1.5)));                   // not an integer
    CHECK(invalid(f.write("engine.trim", std::int64_t{40000})));   // does not fit int16
    CHECK(invalid(f.write("trans.gear", std::int64_t{7})));        // not an item
    CHECK(invalid(f.agent.submit(Command{99, {}, std::nullopt})));  // no writes
    // One bad write rejects the whole command: nothing of it is applied.
    CHECK(invalid(f.agent.submit(Command{100, {{"engine.coolant", 90.0}, {"engine.trim", 1.5}}, std::nullopt})));
    // Conversions that are allowed: an integer to a real, an integral real to an integer.
    CHECK(f.write("engine.coolant", std::int64_t{90}).status == SubmitStatus::Queued);
    CHECK(f.write("engine.trim", -3.0).status == SubmitStatus::Queued);
    f.step(SimTime{});
    CHECK_EQ(f.model.coolant, 90.0);
    CHECK_EQ(f.model.trim, std::int16_t{-3});
    CHECK_EQ(f.agent.stats().invalid, std::uint64_t{12});
    CHECK_EQ(f.agent.stats().applied, std::uint64_t{2});
}

TEST_CASE(control_agent_applies_at_the_first_step_at_or_after_its_time)
{
    Fixture f;
    CHECK(f.write("engine.coolant", 95.0, at(25ms)).status == SubmitStatus::Queued);
    CHECK(f.write("engine.coolant", 100.0, at(40ms)).status == SubmitStatus::Queued); // exactly a step
    f.run(0ms, 60ms, 10ms);
    CHECK(f.result(1).status == CommandStatus::Applied);
    CHECK(f.result(1).applied_at == std::optional<SimTime>(at(30ms)));
    CHECK(!f.result(1).late);
    CHECK(f.result(2).applied_at == std::optional<SimTime>(at(40ms)));
    // The application saw the new value in the applying step itself.
    CHECK_EQ(f.seen_coolant[2].second, 20.0); // step 20
    CHECK_EQ(f.seen_coolant[3].second, 95.0); // step 30
    CHECK_EQ(f.seen_coolant[4].second, 100.0); // step 40
}

TEST_CASE(control_agent_orders_by_time_then_seq)
{
    Fixture f;
    // Submitted out of order: seq 1 at 30, seq 2 at 20, seq 3 and 4 at 25 (applied in seq order).
    f.agent.submit(Command{1, {{"engine.trim", std::int64_t{30}}}, at(30ms)});
    f.agent.submit(Command{2, {{"engine.trim", std::int64_t{20}}}, at(20ms)});
    f.agent.submit(Command{4, {{"engine.trim", std::int64_t{254}}}, at(25ms)});
    f.agent.submit(Command{3, {{"engine.trim", std::int64_t{253}}}, at(25ms)});
    std::vector<std::int16_t> trims;
    for (auto t = 0ms; t < 50ms; t += 10ms)
    {
        f.step(at(t));
        trims.push_back(f.model.trim);
    }
    // Step 30 applies seq 3 and 4 (time 25) before seq 1 (time 30): 30 wins.
    CHECK((trims == std::vector<std::int16_t>{0, 0, 20, 30, 30}));
    std::vector<std::uint64_t> order;
    for (const auto& r : f.results)
    {
        order.push_back(r.seq);
    }
    CHECK((order == std::vector<std::uint64_t>{2, 3, 4, 1}));
}

TEST_CASE(control_agent_reports_late_commands)
{
    Fixture f;
    f.run(0ms, 40ms, 10ms); // last step 30
    f.write("engine.coolant", 50.0, at(30ms));                     // seq 1: step 30 already ran
    f.write("engine.coolant", 60.0, at(35ms));                     // seq 2: on time for step 40
    f.write("engine.coolant", 70.0, at(10ms), LatePolicy::ApplyNext); // seq 3: late but applied
    f.step(at(40ms));
    CHECK(f.result(1).status == CommandStatus::Late);
    CHECK(!f.result(1).applied_at.has_value());
    CHECK(f.result(2).status == CommandStatus::Applied);
    CHECK(!f.result(2).late);
    CHECK(f.result(3).status == CommandStatus::Applied);
    CHECK(f.result(3).late);
    CHECK(f.result(3).applied_at == std::optional<SimTime>(at(40ms)));
    // Order inside step 40: seq 3 (time 10) before seq 2 (time 35).
    CHECK_EQ(f.model.coolant, 60.0);
    CHECK_EQ(f.agent.stats().late, std::uint64_t{1});
}

TEST_CASE(control_agent_without_a_time_applies_in_the_next_step)
{
    Fixture f;
    f.run(0ms, 20ms, 10ms);
    f.write("cabin.parking_brake", true);
    CHECK(!f.model.parking_brake);
    f.step(at(20ms));
    CHECK(f.model.parking_brake);
    CHECK(f.result(1).applied_at == std::optional<SimTime>(at(20ms)));
}

TEST_CASE(control_agent_parameters_only_before_the_first_step)
{
    Fixture f;
    CHECK(f.write("vehicle.mass", 1500.0, SimTime{}).status == SubmitStatus::Queued); // before start
    f.step(SimTime{});
    CHECK_EQ(f.model.mass, 1500.0);
    CHECK(f.result(1).status == CommandStatus::Applied);
    CHECK(f.write("vehicle.mass", 1600.0).status == SubmitStatus::Invalid);

    // Queued before the first step but due later: rejected when its time comes.
    Fixture g;
    CHECK(g.write("vehicle.mass", 1500.0, at(20ms)).status == SubmitStatus::Queued);
    g.run(0ms, 30ms, 10ms);
    CHECK(g.result(1).status == CommandStatus::Rejected);
    CHECK_EQ(g.model.mass, 1000.0);
}

TEST_CASE(control_agent_bounds_the_commands_in_flight)
{
    Fixture f(AgentOptions{.max_pending = 3});
    for (int i = 0; i < 3; ++i)
    {
        CHECK(f.write("engine.trim", std::int64_t{i}, at(100ms)).status == SubmitStatus::Queued);
    }
    const auto busy = f.write("engine.trim", std::int64_t{9}, at(100ms));
    CHECK(busy.status == SubmitStatus::Busy);
    // Applied but not drained: still in flight.
    f.agent.on_step_begin(at(100ms));
    CHECK(f.write("engine.trim", std::int64_t{9}).status == SubmitStatus::Busy);
    std::vector<CommandResult> out;
    CHECK_EQ(f.agent.drain_results(out), std::size_t{3});
    CHECK(f.write("engine.trim", std::int64_t{9}).status == SubmitStatus::Queued);
    CHECK_EQ(f.agent.stats().busy, std::uint64_t{2});
}

TEST_CASE(control_agent_samples_values_at_the_sample_period)
{
    Fixture f(AgentOptions{.sample_period = 20ms});
    CHECK_EQ(f.agent.latest().sequence, std::uint64_t{0});
    f.run(0ms, 50ms, 10ms); // samples at 0, 20, 40
    const auto snap = f.agent.latest();
    CHECK_EQ(snap.sequence, std::uint64_t{3});
    CHECK(snap.time == at(40ms));
    CHECK_EQ(snap.values.size(), std::size_t{6});
    CHECK_EQ(std::get<double>(snap.values[5]), 5.0); // speed after 5 steps
    CHECK_EQ(std::get<bool>(snap.values[0]), false);
    CHECK_EQ(std::get<std::int64_t>(snap.values[3]), std::int64_t{0});
}

TEST_CASE(control_agent_events_are_bounded)
{
    Fixture f(AgentOptions{.max_events = 2});
    f.step(at(10ms));
    f.agent.log(EventLevel::Info, "one");
    f.agent.log(EventLevel::Warning, "two");
    f.agent.log(EventLevel::Error, "three"); // dropped
    std::vector<Event> events;
    CHECK_EQ(f.agent.drain_events(events), std::size_t{2});
    CHECK(events[1].text == std::string("two"));
    CHECK(events[1].time == at(10ms));
    CHECK_EQ(f.agent.stats().events_dropped, std::uint64_t{1});
}

TEST_CASE(control_agent_finish_cancels_what_is_waiting)
{
    Fixture f;
    f.write("engine.trim", std::int64_t{1}, at(500ms));
    f.run(0ms, 20ms, 10ms);
    f.write("engine.trim", std::int64_t{2}); // never stepped
    f.agent.finish();
    f.agent.drain_results(f.results);
    CHECK(f.result(1).status == CommandStatus::Cancelled);
    CHECK(f.result(2).status == CommandStatus::Cancelled);
    CHECK_EQ(f.model.trim, std::int16_t{0});
}

TEST_CASE(control_agent_same_schedule_gives_the_same_result_whatever_the_submit_timing)
{
    // Commands submitted from another thread at random wall-clock times, all due later than
    // the step at which they can arrive at the latest (the stepping waits until all are queued
    // before passing 100 ms): the applied times do not depend on the timing.
    std::vector<std::vector<std::pair<std::uint64_t, std::int64_t>>> runs;
    for (int run = 0; run < 3; ++run)
    {
        Fixture f(AgentOptions{.max_pending = 64});
        std::atomic<int> queued{0};
        std::thread transport([&] {
            for (std::uint64_t i = 1; i <= 40; ++i)
            {
                const auto when = at(100ms + std::chrono::milliseconds((i * 37) % 200));
                while (f.agent.submit(Command{i, {{"engine.trim", static_cast<std::int64_t>(i)}}, when}).status != SubmitStatus::Queued)
                {
                    std::this_thread::yield();
                }
                queued.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(50 * (run + 1)));
            }
        });
        std::vector<std::pair<std::uint64_t, std::int64_t>> applied;
        for (auto t = 0ms; t < 400ms; t += 10ms)
        {
            while (t >= 100ms && queued.load() < 40)
            {
                std::this_thread::yield();
            }
            f.step(at(t), false); // the results are the transport's: drained after the join
        }
        transport.join();
        f.agent.drain_results(f.results);
        for (const auto& r : f.results)
        {
            CHECK(r.status == CommandStatus::Applied);
            applied.emplace_back(r.seq, r.applied_at->time_since_epoch().count());
        }
        CHECK_EQ(applied.size(), std::size_t{40});
        runs.push_back(applied);
    }
    CHECK(runs[0] == runs[1]);
    CHECK(runs[1] == runs[2]);
}

TEST_CASE(control_agent_transport_and_steps_on_two_threads)
{
    // Many commands and reads from a transport thread while another thread steps: every command
    // gets exactly one result, and the step thread never waits (run under ThreadSanitizer).
    Fixture f(AgentOptions{.max_pending = 8, .sample_period = SimDuration::zero()});
    constexpr std::uint64_t kCommands = 5000;
    std::atomic<bool> done{false};
    std::vector<CommandResult> transport_results;
    std::thread transport([&] {
        std::uint64_t next = 1;
        while (transport_results.size() < kCommands)
        {
            if (next <= kCommands)
            {
                const auto r = f.agent.submit(Command{next, {{"engine.trim", static_cast<std::int64_t>(next % 100)}}, std::nullopt});
                if (r.status == SubmitStatus::Queued)
                {
                    ++next;
                }
            }
            f.agent.drain_results(transport_results);
            static_cast<void>(f.agent.latest());
        }
        done = true;
    });
    SimDuration t{};
    while (!done)
    {
        f.agent.on_step_begin(at(t));
        f.model.speed += 1.0;
        f.agent.on_step_end(at(t));
        t += 1ms;
    }
    transport.join();
    CHECK_EQ(transport_results.size(), static_cast<std::size_t>(kCommands));
    for (std::size_t i = 0; i < transport_results.size(); ++i)
    {
        CHECK_EQ(transport_results[i].seq, static_cast<std::uint64_t>(i + 1));
        CHECK(transport_results[i].status == CommandStatus::Applied);
    }
}

TEST_CASE(control_agent_transport_side_from_two_threads)
{
    // Two transport threads (say, two RPC handlers) submit, drain and read while a step thread
    // runs: every command gets one result, and the readers see whole samples.
    Fixture f(AgentOptions{.max_pending = 16, .sample_period = SimDuration::zero()});
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::mutex results_mutex;
    std::vector<CommandResult> results;
    std::atomic<std::uint64_t> next{1};
    constexpr std::uint64_t kCommands = 2000;
    const auto transport = [&] {
        std::vector<CommandResult> mine;
        while (true)
        {
            const auto seq = next.fetch_add(1);
            if (seq > kCommands)
            {
                break;
            }
            while (f.agent.submit(Command{seq, {{"engine.trim", std::int64_t{1}}}, std::nullopt}).status != SubmitStatus::Queued)
            {
                f.agent.drain_results(mine);
            }
            f.agent.drain_results(mine);
            const auto snap = f.agent.latest();
            if (snap.sequence > 0 && std::get<double>(snap.values[5]) != static_cast<double>(snap.sequence))
            {
                ++torn;
            }
        }
        const std::lock_guard lock(results_mutex);
        results.insert(results.end(), mine.begin(), mine.end());
    };
    std::thread a(transport), b(transport);
    std::thread steps([&] {
        for (auto t = 0ms; !done.load(); t += 1ms)
        {
            f.step(at(t), false);
        }
    });
    a.join();
    b.join();
    // The last commands may still wait for a step.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (results.size() < kCommands && std::chrono::steady_clock::now() < deadline)
    {
        f.agent.drain_results(results);
    }
    done = true;
    steps.join();
    CHECK_EQ(results.size(), static_cast<std::size_t>(kCommands));
    CHECK_EQ(torn.load(), 0);
}

TEST_CASE(control_agent_type_boundaries)
{
    std::int64_t big = 0;
    std::uint32_t u32 = 0;
    float f32 = 0.0f;
    std::int64_t limited = 0;
    std::uint64_t counter = std::numeric_limits<std::uint64_t>::max();
    Agent agent(AgentOptions{.sample_period = SimDuration::zero()});
    agent.output("counter", &counter); // read saturated at the largest int64
    agent.input("big", &big);
    agent.input("u32", &u32);
    agent.input("f32", &f32);
    agent.input("limited", &limited, {.max = 9007199254740992.0}); // 2^53
    std::uint64_t seq = 0;
    const auto put = [&](const char* name, Value v) { return agent.submit(Command{++seq, {{name, v}}, std::nullopt}).status; };
    CHECK(put("big", 9223372036854775808.0) == SubmitStatus::Invalid); // 2^63
    CHECK(put("big", -9223372036854775808.0) == SubmitStatus::Queued); // -2^63
    CHECK(put("big", std::numeric_limits<std::int64_t>::max()) == SubmitStatus::Queued);
    CHECK(put("u32", std::int64_t{-1}) == SubmitStatus::Invalid);
    CHECK(put("u32", std::int64_t{4294967296}) == SubmitStatus::Invalid);
    CHECK(put("u32", std::int64_t{4294967295}) == SubmitStatus::Queued);
    CHECK(put("f32", 1e300) == SubmitStatus::Invalid); // finite double, not a finite float
    CHECK(put("f32", std::numeric_limits<double>::infinity()) == SubmitStatus::Invalid);
    CHECK(put("f32", -std::numeric_limits<double>::infinity()) == SubmitStatus::Invalid);
    CHECK(put("f32", 1.5) == SubmitStatus::Queued);
    // Above 2^53 the limit is compared as an integer.
    CHECK(put("limited", std::int64_t{9007199254740993}) == SubmitStatus::Invalid);
    CHECK(put("limited", std::int64_t{9007199254740992}) == SubmitStatus::Queued);
    agent.on_step_begin(SimTime{});
    CHECK_EQ(big, std::numeric_limits<std::int64_t>::max());
    CHECK_EQ(u32, std::uint32_t{4294967295});
    CHECK_EQ(f32, 1.5f);
    CHECK_EQ(limited, std::int64_t{9007199254740992});
    agent.on_step_end(SimTime{});
    CHECK_EQ(std::get<std::int64_t>(agent.latest().values[0]), std::numeric_limits<std::int64_t>::max());
}

TEST_CASE(control_agent_untimed_and_timed_commands_in_one_step)
{
    Fixture f;
    f.run(0ms, 30ms, 10ms); // last step 20
    // Both due in step 30: seq 1 has no time (sorts as 30), seq 2 is for 25: seq 2 first.
    f.agent.submit(Command{1, {{"engine.trim", std::int64_t{1}}}, std::nullopt});
    f.agent.submit(Command{2, {{"engine.trim", std::int64_t{2}}}, at(25ms)});
    f.step(at(30ms));
    CHECK_EQ(f.model.trim, std::int16_t{1});
    CHECK_EQ(f.results[0].seq, std::uint64_t{2});
    CHECK_EQ(f.results[1].seq, std::uint64_t{1});
}

TEST_CASE(control_agent_a_throwing_setter_rejects_its_command_only)
{
    Model m;
    int y = 0;
    Agent agent;
    agent.input("x", &m.coolant);
    agent.input("y", [&] { return y; }, [&](int v) {
        if (v < 0)
        {
            throw std::runtime_error("negative");
        }
        y = v;
    });
    CHECK(agent.submit(Command{1, {{"x", 5.0}, {"y", std::int64_t{-1}}}, std::nullopt}).status == SubmitStatus::Queued);
    CHECK(agent.submit(Command{2, {{"y", std::int64_t{3}}}, std::nullopt}).status == SubmitStatus::Queued);
    agent.on_step_begin(SimTime{}); // does not throw
    std::vector<CommandResult> results;
    agent.drain_results(results);
    CHECK_EQ(results.size(), std::size_t{2});
    CHECK(results[0].status == CommandStatus::Rejected);
    CHECK(results[0].reason.find("negative") != std::string::npos);
    CHECK_EQ(m.coolant, 5.0); // the write before the throwing one stays applied (documented)
    CHECK(results[1].status == CommandStatus::Applied);
    CHECK_EQ(y, 3);
}

TEST_CASE(control_agent_submit_after_finish_is_refused)
{
    Fixture f;
    f.step(SimTime{});
    f.agent.finish();
    const auto r = f.write("engine.trim", std::int64_t{1});
    CHECK(r.status == SubmitStatus::Invalid);

    // finish() racing with submit() from another thread: every queued command gets a result.
    for (int round = 0; round < 50; ++round)
    {
        Fixture g(AgentOptions{.max_pending = 1024});
        std::atomic<bool> go{false};
        std::uint64_t queued = 0;
        std::thread transport([&] {
            while (!go.load())
            {
            }
            for (std::uint64_t i = 1; i <= 300; ++i)
            {
                if (g.agent.submit(Command{i, {{"engine.trim", std::int64_t{1}}}, at(1s)}).status == SubmitStatus::Queued)
                {
                    ++queued;
                }
            }
        });
        go = true;
        std::this_thread::sleep_for(std::chrono::microseconds(round * 3));
        g.agent.finish();
        transport.join();
        std::vector<CommandResult> results;
        g.agent.drain_results(results);
        CHECK_EQ(results.size(), static_cast<std::size_t>(queued));
    }
}

TEST_CASE(control_agent_bounds_with_mixed_results)
{
    // Late, Applied and Cancelled results fill the slots like any other: none is lost.
    Fixture f(AgentOptions{.max_pending = 3});
    f.run(0ms, 20ms, 10ms);
    f.write("engine.trim", std::int64_t{1}, at(5ms));   // Late
    f.write("engine.trim", std::int64_t{2});             // Applied
    f.write("engine.trim", std::int64_t{3}, at(900ms)); // Cancelled
    CHECK(f.write("engine.trim", std::int64_t{4}).status == SubmitStatus::Busy);
    f.agent.on_step_begin(at(20ms));
    f.agent.finish();
    CHECK(f.write("engine.trim", std::int64_t{5}).status == SubmitStatus::Invalid);
    std::vector<CommandResult> out;
    CHECK_EQ(f.agent.drain_results(out), std::size_t{3});
    CHECK(out[0].status == CommandStatus::Late);
    CHECK(out[1].status == CommandStatus::Applied);
    CHECK(out[2].status == CommandStatus::Cancelled);
}

TEST_CASE(control_agent_sample_grid_and_a_busy_reader)
{
    // 3 ms steps, 10 ms period: samples in the first steps at or after 0, 10, 20, 30.
    Fixture f(AgentOptions{.sample_period = 10ms});
    std::vector<std::int64_t> sampled;
    std::uint64_t last_sequence = 0;
    for (auto t = 0ms; t < 35ms; t += 3ms)
    {
        f.step(at(t));
        const auto snap = f.agent.latest();
        if (snap.sequence != last_sequence)
        {
            last_sequence = snap.sequence;
            sampled.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(snap.time.time_since_epoch()).count());
        }
    }
    CHECK((sampled == std::vector<std::int64_t>{0, 12, 21, 30}));

    // A reader that never stops reading still sees every new sample (no lock to starve on).
    Fixture g(AgentOptions{.sample_period = SimDuration::zero()});
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> seen{0};
    std::atomic<int> torn{0}; // checked after the join: a CHECK on this thread would terminate
    std::thread reader([&] {
        while (!stop.load())
        {
            const auto snap = g.agent.latest();
            // speed is written in the step before the sample: speed == sequence
            if (snap.sequence > 0
                && (snap.values.size() != 6 || std::get<double>(snap.values[5]) != static_cast<double>(snap.sequence)))
            {
                ++torn;
            }
            seen = snap.sequence;
        }
    });
    for (auto t = 0ms; t < 2000ms; t += 1ms)
    {
        g.step(at(t), false);
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (seen.load() != 2000 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    stop = true;
    reader.join();
    CHECK_EQ(seen.load(), std::uint64_t{2000});
    CHECK_EQ(torn.load(), 0);
}
