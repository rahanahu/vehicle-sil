// SPDX-License-Identifier: MIT
//
// The time gate of SimulationController (pause, run_until, step, speed) and the StepHold of
// ParticipantRuntime it is built on, with real SIL Kit time synchronisation (in-process registry,
// participants with different step sizes).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;
using Wall = std::chrono::steady_clock;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;
using vehicle_sil::unit::throws;

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

/// A coordinated participant that records the start time of every step it runs.
struct Recorder
{
    std::string name;
    SimDuration step;
    std::unique_ptr<ParticipantRuntime> runtime;
    mutable std::mutex mutex;
    std::vector<SimDuration> steps;

    Recorder(std::string n, SimDuration s, const std::string& uri) : name(std::move(n)), step(s)
    {
        runtime = std::make_unique<ParticipantRuntime>(coordinated_options(name, uri, VirtualTimeOptions{step, std::nullopt}));
        runtime->set_step_handler([this](SimTime now, SimDuration) {
            const std::lock_guard lock(mutex);
            steps.push_back(now.time_since_epoch());
        });
    }

    [[nodiscard]] std::vector<SimDuration> copy() const
    {
        const std::lock_guard lock(mutex);
        return steps;
    }
    [[nodiscard]] SimDuration last() const
    {
        const std::lock_guard lock(mutex);
        return steps.empty() ? SimDuration(-1) : steps.back();
    }
};

/// Three recorders (1, 3 and 7 ms) and a controller with a 10 ms time gate.
struct GateSim
{
    InProcessRegistry registry;
    std::unique_ptr<SimulationController> controller;
    std::vector<std::unique_ptr<Recorder>> peers;

    explicit GateSim(TimeControlOptions time_control, std::optional<SimDuration> stop_at = std::nullopt)
    {
        const std::vector<std::pair<std::string, SimDuration>> specs{{"A", 1ms}, {"B", 3ms}, {"C", 7ms}};
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        for (const auto& [name, step] : specs)
        {
            options.required_participants.push_back(name);
        }
        options.stop_at = stop_at;
        options.time_control = time_control;
        controller = std::make_unique<SimulationController>(options);
        for (const auto& [name, step] : specs)
        {
            peers.push_back(std::make_unique<Recorder>(name, step, registry.uri));
        }
    }

    void start()
    {
        std::vector<ParticipantRuntime*> runtimes;
        for (auto& peer : peers)
        {
            runtimes.push_back(peer->runtime.get());
        }
        start_together(*controller, runtimes, 30s);
    }

    /// Every peer's last step is the last one of its grid that starts at or before `p`, and it
    /// stays so while the simulation stands still.
    void check_stands_still_at(SimDuration p)
    {
        // The peers' steps at or before p may still be running when the controller reports it.
        std::this_thread::sleep_for(200ms);
        std::vector<SimDuration> before;
        for (auto& peer : peers)
        {
            const auto expected = (p / peer->step) * peer->step;
            CHECK_EQ(peer->last().count(), expected.count());
            before.push_back(peer->last());
        }
        std::this_thread::sleep_for(200ms);
        for (std::size_t i = 0; i < peers.size(); ++i)
        {
            CHECK_EQ(peers[i]->last().count(), before[i].count());
        }
    }

    ~GateSim()
    {
        controller->stop("test done");
        for (auto& peer : peers)
        {
            peer->runtime->stop("test done");
        }
    }
};

/// Every peer ran 0, step, 2 * step, ... without gaps.
void check_grid(const Recorder& peer)
{
    const auto steps = peer.copy();
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        CHECK_EQ(steps[i].count(), (peer.step * static_cast<long long>(i)).count());
    }
}

} // namespace

TEST_CASE(virtual_time_gate_stands_still_at_a_controller_step)
{
    GateSim sim(TimeControlOptions{.step = 10ms});
    sim.controller->pause_at(at(45ms)); // the first controller step at or after 45 ms is 50 ms
    sim.start();

    auto p = sim.controller->wait_until_paused(10s);
    CHECK(p.has_value());
    CHECK_EQ(p->time_since_epoch().count(), SimDuration(50ms).count());
    CHECK(sim.controller->time_state().paused);
    sim.check_stands_still_at(50ms); // A 50, B 48, C 49

    sim.controller->run_until(at(100ms));
    p = sim.controller->wait_until_paused(10s);
    CHECK(p.has_value());
    CHECK_EQ(p->time_since_epoch().count(), SimDuration(100ms).count());
    sim.check_stands_still_at(100ms); // A 100, B 99, C 98

    sim.controller->step(30ms);
    p = sim.controller->wait_until_paused(10s);
    CHECK(p.has_value());
    CHECK_EQ(p->time_since_epoch().count(), SimDuration(130ms).count());
    sim.check_stands_still_at(130ms);

    // Running on and pausing without a target stops at some later controller step.
    sim.controller->resume();
    CHECK(!sim.controller->time_state().paused);
    std::this_thread::sleep_for(20ms);
    sim.controller->pause();
    p = sim.controller->wait_until_paused(10s);
    CHECK(p.has_value());
    CHECK(*p > at(130ms));
    CHECK_EQ((p->time_since_epoch() % 10ms).count(), 0);
    sim.check_stands_still_at(p->time_since_epoch());

    for (auto& peer : sim.peers)
    {
        check_grid(*peer);
    }
}

TEST_CASE(virtual_time_gate_start_paused_runs_only_the_steps_at_zero)
{
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true});
    sim.start();
    const auto p = sim.controller->wait_until_paused(10s);
    CHECK(p.has_value());
    CHECK(*p == SimTime{});
    std::this_thread::sleep_for(200ms);
    for (auto& peer : sim.peers)
    {
        const auto steps = peer->copy();
        CHECK_EQ(steps.size(), std::size_t{1});
        CHECK(steps.at(0) == SimDuration::zero());
    }
}

TEST_CASE(virtual_time_gate_pausing_does_not_change_the_steps)
{
    // Free run with the old controller (no gate) and a run that is paused, stepped and paced
    // many times: every peer runs the same steps.
    constexpr auto kEnd = 300ms;
    std::vector<std::vector<SimDuration>> free_run;
    {
        InProcessRegistry registry;
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        options.required_participants = {"A", "B", "C"};
        options.stop_at = kEnd;
        SimulationController controller(options);
        Recorder a("A", 1ms, registry.uri), b("B", 3ms, registry.uri), c("C", 7ms, registry.uri);
        start_together(controller, {a.runtime.get(), b.runtime.get(), c.runtime.get()}, 30s);
        CHECK(controller.wait_until_finished(30s).has_value());
        for (auto* r : {&a, &b, &c})
        {
            r->runtime->wait_until_finished(10s);
            free_run.push_back(r->copy());
        }
    }

    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true}, kEnd);
    sim.start();
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    for (int i = 0; i < 8; ++i)
    {
        sim.controller->step(10ms * (i % 3 + 1));
        CHECK(sim.controller->wait_until_paused(10s).has_value());
    }
    sim.controller->set_speed(2.0);
    sim.controller->run_until(at(200ms));
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    sim.controller->set_speed(std::nullopt);
    sim.controller->resume();
    CHECK(sim.controller->wait_until_finished(30s).has_value());
    for (std::size_t i = 0; i < sim.peers.size(); ++i)
    {
        sim.peers[i]->runtime->wait_until_finished(10s);
        auto gated = sim.peers[i]->copy();
        auto free = free_run[i];
        // The step at kEnd itself may or may not run (it races with the stop); compare below it.
        const auto below = [&](std::vector<SimDuration>& v) {
            while (!v.empty() && v.back() >= kEnd)
            {
                v.pop_back();
            }
        };
        below(gated);
        below(free);
        CHECK(gated == free);
        CHECK_EQ(gated.size(), static_cast<std::size_t>((kEnd - SimDuration(1)) / sim.peers[i]->step + 1));
    }
}

TEST_CASE(virtual_time_gate_speed_paces_and_changes)
{
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true, .speed = 2.0});
    sim.start();
    CHECK(sim.controller->wait_until_paused(10s).has_value());

    // 200 ms of simulation at speed 2: about 100 ms of wall-clock time.
    auto begin = Wall::now();
    sim.controller->run_until(at(200ms));
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    const auto at2 = Wall::now() - begin;
    CHECK(at2 >= 90ms);
    CHECK(at2 < 1s);

    // Speed 0.5: 100 ms of simulation take about 200 ms, counted from the resume (the time
    // spent standing still before it does not count).
    sim.controller->set_speed(0.5);
    std::this_thread::sleep_for(300ms);
    begin = Wall::now();
    sim.controller->step(100ms);
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    const auto half = Wall::now() - begin;
    CHECK(half >= 180ms);
    CHECK(half < 1500ms);
    CHECK(sim.controller->time_state().speed == std::optional<double>(0.5));
}

TEST_CASE(virtual_time_gate_pause_takes_effect_at_the_step_held_for_pacing)
{
    // At speed 0.01 the controller holds each 10 ms step for about 1 s of wall-clock time, so
    // pause() almost always finds a held step and stands still there at once.
    GateSim sim(TimeControlOptions{.step = 10ms, .speed = 0.01});
    sim.start();
    std::this_thread::sleep_for(100ms);
    const auto before = sim.controller->time_state();
    sim.controller->pause();
    const auto after = sim.controller->time_state();
    CHECK(after.paused);
    CHECK(after.now == before.now);
    sim.check_stands_still_at(after.now.time_since_epoch());
}

TEST_CASE(virtual_time_gate_stop_while_paused_shuts_down)
{
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true});
    sim.start();
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    const auto begin = Wall::now();
    sim.controller->stop("stop while paused");
    CHECK(Wall::now() - begin < 3s);
    for (auto& peer : sim.peers)
    {
        const auto state = peer->runtime->wait_until_finished(5s);
        CHECK(state.has_value());
        CHECK(*state == orch::ParticipantState::Shutdown);
    }
    // After the end, waiting for a pause gives up instead of waiting for the timeout.
    const auto waited = Wall::now();
    CHECK(!sim.controller->wait_until_paused(5s).has_value());
    CHECK(Wall::now() - waited < 2s);
}

TEST_CASE(virtual_time_gate_stop_requested_before_start_ends_the_simulation)
{
    // A stop asked for while the controller still waits for the participants (before Running,
    // where SIL Kit ignores Stop()) is kept and ends the simulation once it runs, instead of being
    // lost and leaving the participants running free with the time gate switched off.
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true});
    sim.controller->runtime().request_stop("stop before start");
    std::vector<std::jthread> starters; // joined also when a check throws
    for (auto& peer : sim.peers)
    {
        // A peer may see the simulation stopping before its own start() saw it run.
        starters.emplace_back([&peer] {
            try
            {
                peer->runtime->start(30s);
            }
            catch (const std::runtime_error&)
            {
            }
        });
    }
    sim.controller->start(30s);
    for (auto& starter : starters)
    {
        starter.join();
    }
    for (auto& peer : sim.peers)
    {
        const auto state = peer->runtime->wait_until_finished(10s);
        CHECK(state.has_value());
        CHECK(peer->last() < SimDuration(1s));
    }
    CHECK(sim.controller->wait_until_finished(10s).has_value());
}

TEST_CASE(virtual_time_gate_stop_requested_while_waiting_for_the_others_ends_the_simulation)
{
    // A participant whose lifecycle has started but waits for the others (not Running yet: SIL
    // Kit ignores Stop() there) is asked to stop: the stop is delivered once it runs.
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true});
    std::vector<std::jthread> starters; // joined also when a check throws
    for (auto& peer : sim.peers)
    {
        starters.emplace_back([&peer] {
            try
            {
                peer->runtime->start(30s);
            }
            catch (const std::runtime_error&)
            {
            }
        });
    }
    auto& first = *sim.peers.front()->runtime;
    const auto deadline = Wall::now() + 10s;
    while (!first.started() && Wall::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(200ms); // its lifecycle waits for the controller
    CHECK(first.state() != orch::ParticipantState::Running);
    first.request_stop("stop while waiting for the others");
    try
    {
        sim.controller->start(30s);
    }
    catch (const std::runtime_error&)
    {
        // The peer stops at its first step; the simulation may end before the controller's
        // first step, and then its start() reports that it did not see the simulation run.
    }
    for (auto& starter : starters)
    {
        starter.join();
    }
    for (auto& peer : sim.peers)
    {
        CHECK(peer->runtime->wait_until_finished(10s).has_value());
        CHECK(peer->last() < SimDuration(1s));
    }
    CHECK(sim.controller->wait_until_finished(10s).has_value());
}

TEST_CASE(virtual_time_gate_a_step_past_the_end_of_time_saturates)
{
    // step(d) with a d from outside (the Control API) that reaches past the largest time: the
    // target is the largest time, not an overflowed one in the past.
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true});
    sim.start();
    sim.controller->step(10ms);
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    CHECK(sim.controller->time_state().now == at(10ms));
    sim.controller->step(SimDuration::max());
    const auto target = sim.controller->time_state().pause_target;
    CHECK(target.has_value() && *target == SimTime::max());
    sim.controller->pause();
    CHECK(sim.controller->wait_until_paused(10s).has_value());
}

TEST_CASE(virtual_time_gate_api_misuse_is_rejected)
{
    InProcessRegistry registry;
    {
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        options.participant_name = "NoGate";
        options.required_participants = {"X"};
        SimulationController controller(options);
        CHECK(throws<std::logic_error>([&] { controller.pause(); }));
        CHECK(throws<std::logic_error>([&] { static_cast<void>(controller.time_state()); }));
    }
    {
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        options.participant_name = "BadStep";
        options.required_participants = {"X"};
        options.time_control = TimeControlOptions{.step = SimDuration::zero()};
        CHECK(throws<std::invalid_argument>([&] { SimulationController controller(options); }));
        options.time_control = TimeControlOptions{.step = 10ms, .speed = 0.0};
        CHECK(throws<std::invalid_argument>([&] { SimulationController controller(options); }));
    }
    {
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        options.participant_name = "Gate";
        options.required_participants = {"X"};
        options.time_control = TimeControlOptions{};
        SimulationController controller(options);
        CHECK(throws<std::invalid_argument>([&] { controller.step(SimDuration::zero()); }));
        CHECK(throws<std::invalid_argument>([&] { controller.set_speed(-1.0); }));
    }
    {
        ParticipantRuntimeOptions options;
        options.participant_name = "Realtime";
        options.registry_uri = registry.uri;
        ParticipantRuntime runtime(options);
        CHECK(throws<std::logic_error>([&] { runtime.set_step_hold([](SimTime, SimDuration) { return false; }); }));
        runtime.release_step(); // nothing held: no effect
    }
}

TEST_CASE(virtual_time_gate_speed_change_releases_a_step_held_for_pacing)
{
    // Speed 0.01 holds each 10 ms step for about 1 s; dropping the speed lets it go at once.
    GateSim sim(TimeControlOptions{.step = 10ms, .speed = 0.01});
    sim.start();
    std::this_thread::sleep_for(100ms);
    const auto before = sim.controller->time_state().now;
    sim.controller->set_speed(std::nullopt);
    std::this_thread::sleep_for(300ms);
    CHECK(sim.controller->time_state().now > before + 50ms);
}

TEST_CASE(virtual_time_gate_setting_the_same_speed_does_not_restart_the_wait)
{
    // Speed 0.1: each 10 ms step takes about 100 ms. Setting 0.1 every 30 ms must not keep a
    // step held forever; a change keeps the part of the wait that has passed.
    GateSim sim(TimeControlOptions{.step = 10ms, .speed = 0.1});
    sim.start();
    const auto begin = sim.controller->time_state().now;
    for (int i = 0; i < 20; ++i)
    {
        sim.controller->set_speed(0.1);
        std::this_thread::sleep_for(30ms);
    }
    const auto same = sim.controller->time_state().now;
    CHECK(same >= begin + 30ms); // about 60 ms in 600 ms
    for (int i = 0; i < 20; ++i)
    {
        sim.controller->set_speed(i % 2 == 0 ? 0.1 : 0.11);
        std::this_thread::sleep_for(30ms);
    }
    CHECK(sim.controller->time_state().now >= same + 30ms);
}

TEST_CASE(virtual_time_gate_targets_in_the_past_and_resume)
{
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true});
    sim.start();
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    sim.controller->run_until(at(50ms));
    CHECK(sim.controller->wait_until_paused(10s) == std::optional<SimTime>(at(50ms)));
    // Already at or after the target: run_until stays.
    sim.controller->run_until(at(30ms));
    CHECK(sim.controller->time_state().paused);
    CHECK(sim.controller->time_state().now == at(50ms));
    // A target in the past while paused does not move it; once resumed it stops at the next step.
    sim.controller->pause_at(at(20ms));
    CHECK(sim.controller->time_state().pause_target == std::optional<SimTime>(at(20ms)));
    CHECK(sim.controller->time_state().paused);
    // resume() clears the target: it runs on past 60 ms.
    sim.controller->resume();
    CHECK(!sim.controller->time_state().pause_target.has_value());
    std::this_thread::sleep_for(100ms);
    sim.controller->pause_at(at(20ms)); // passed: the next controller step
    const auto p = sim.controller->wait_until_paused(10s);
    CHECK(p.has_value());
    CHECK(*p > at(60ms));
    sim.check_stands_still_at(p->time_since_epoch());
}

TEST_CASE(virtual_time_gate_abort_while_paused_ends_the_simulation)
{
    GateSim sim(TimeControlOptions{.step = 10ms, .start_paused = true});
    sim.start();
    CHECK(sim.controller->wait_until_paused(10s).has_value());
    sim.controller->abort();
    for (auto& peer : sim.peers)
    {
        CHECK(peer->runtime->wait_until_finished(10s).has_value());
    }
    CHECK(!sim.controller->wait_until_paused(1s).has_value());
    CHECK(!sim.controller->time_state().paused);
}

TEST_CASE(virtual_time_gate_stop_at_between_controller_steps)
{
    // stop_at 295 ms with 10 ms controller steps: the controller stops at its step at 300 ms, so
    // no peer runs a step that starts after 300 ms.
    GateSim sim(TimeControlOptions{.step = 10ms}, 295ms);
    sim.start();
    CHECK(sim.controller->wait_until_finished(30s).has_value());
    for (auto& peer : sim.peers)
    {
        peer->runtime->wait_until_finished(10s);
        const auto last = peer->last();
        CHECK(last <= SimDuration(300ms));
        CHECK(last > SimDuration(300ms) - peer->step - peer->step);
        check_grid(*peer);
    }
}

TEST_CASE(virtual_time_step_hold_of_a_participant_runtime)
{
    // A plain participant holding its own step at 50 ms: a 1 ms peer stands still at 50 ms until
    // another thread releases it; a hold that throws completes the step; after a stop the hold is
    // not called and release_step() is harmless.
    InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"Holder", "A"};
    SimulationController controller(options);
    Recorder holder("Holder", 10ms, registry.uri);
    Recorder a("A", 1ms, registry.uri);
    std::atomic<int> holds_after_stop{0};
    std::atomic<bool> stopping{false};
    // The holder asks to stop from its step at 250 ms: the hold of that step is not called.
    holder.runtime->set_step_handler([&](SimTime now, SimDuration) {
        {
            const std::lock_guard lock(holder.mutex);
            holder.steps.push_back(now.time_since_epoch());
        }
        if (now == at(250ms))
        {
            stopping = true;
            holder.runtime->request_stop("stop from a step");
        }
    });
    holder.runtime->set_step_hold([&](SimTime now, SimDuration) {
        if (stopping)
        {
            ++holds_after_stop;
        }
        if (now == at(100ms))
        {
            throw std::runtime_error("hold failed");
        }
        return now == at(50ms) || now == at(200ms) || now == at(250ms);
    });
    start_together(controller, {holder.runtime.get(), a.runtime.get()}, 30s);
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(a.last().count(), SimDuration(50ms).count());
    std::thread releaser([&] { holder.runtime->release_step(); });
    releaser.join();
    // Past the throwing hold at 100 ms up to the next hold at 200 ms.
    const auto deadline = Wall::now() + 10s;
    while (a.last() < SimDuration(200ms) && Wall::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(a.last().count(), SimDuration(200ms).count());
    CHECK(holder.runtime->step_error().empty());
    // Released at 200 ms, it runs on to 250 ms and stops itself there.
    holder.runtime->release_step();
    CHECK(holder.runtime->wait_until_finished(10s).has_value());
    CHECK(stopping.load());
    CHECK_EQ(holds_after_stop.load(), 0);
    controller.stop("done");
    holder.runtime->stop("done");
    holder.runtime->release_step(); // after the stop: safe (nothing is held, or the participant has stopped)
    a.runtime->stop("done");
}
