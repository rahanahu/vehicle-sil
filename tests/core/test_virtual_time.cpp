// SPDX-License-Identifier: MIT
//
// ParticipantRuntime and SimulationController with real SIL Kit time synchronisation
// (in-process registry, several participants with different step sizes).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/tick_scheduler.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
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

/// One begin/end record per step, in the wall-clock order in which they happened.
struct StepLog
{
    struct Event
    {
        std::string participant;
        bool begin;
        SimDuration time;
    };

    std::mutex mutex;
    std::vector<Event> events;

    void add(const std::string& participant, bool begin, SimTime t)
    {
        const std::lock_guard lock(mutex);
        events.push_back({participant, begin, t.time_since_epoch()});
    }
};

struct PeerSpec
{
    std::string name;
    SimDuration step;
    std::chrono::microseconds busy{0}; ///< wall-clock work per step
    std::optional<double> real_time_factor{};
};

struct PeerResult
{
    std::vector<SimDuration> steps;
};

/// Runs a coordinated simulation of `peers` until `stop_at` and returns the wall time the
/// controller measured between T = 0 and T = stop_at.
std::chrono::nanoseconds run_simulation(const std::vector<PeerSpec>& peers, SimDuration stop_at, StepLog& log,
                                        std::map<std::string, PeerResult>& results)
{
    InProcessRegistry registry;

    std::vector<std::string> names;
    for (const auto& peer : peers)
    {
        names.push_back(peer.name);
    }
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = names;
    controllerOptions.stop_at = stop_at;
    SimulationController controller(controllerOptions);

    std::vector<std::unique_ptr<ParticipantRuntime>> runtimes;
    std::vector<ParticipantRuntime*> runtimePointers;
    for (const auto& peer : peers)
    {
        auto runtime = std::make_unique<ParticipantRuntime>(
            coordinated_options(peer.name, registry.uri, VirtualTimeOptions{peer.step, peer.real_time_factor}));
        auto* result = &results[peer.name];
        runtime->set_step_handler([&log, result, peer](SimTime now, SimDuration step) {
            if (step != peer.step)
            {
                throw std::logic_error("unexpected step size");
            }
            log.add(peer.name, true, now);
            if (peer.busy.count() > 0)
            {
                std::this_thread::sleep_for(peer.busy);
            }
            result->steps.push_back(now.time_since_epoch());
            log.add(peer.name, false, now);
        });
        runtimePointers.push_back(runtime.get());
        runtimes.push_back(std::move(runtime));
    }

    start_together(controller, runtimePointers, 30s);

    const auto controllerState = controller.wait_until_finished(60s);
    CHECK(controllerState.has_value());
    CHECK(*controllerState == orch::ParticipantState::Shutdown);
    for (auto& runtime : runtimes)
    {
        const auto state = runtime->wait_until_finished(30s);
        CHECK(state.has_value());
        CHECK(*state == orch::ParticipantState::Shutdown);
        CHECK(runtime->step_error().empty());
    }
    const auto wall = controller.wall_time_to_stop_at();
    CHECK(wall.has_value());
    return *wall;
}

/// Every peer ran exactly the steps 0, step, 2*step, ... below stop_at (and possibly the step at
/// stop_at itself, which runs concurrently with the controller's stop).
void check_step_grid(const std::vector<PeerSpec>& peers, SimDuration stop_at, std::map<std::string, PeerResult>& results)
{
    for (const auto& peer : peers)
    {
        const auto& steps = results[peer.name].steps;
        const auto expected = static_cast<std::size_t>(stop_at / peer.step);
        CHECK(steps.size() == expected || steps.size() == expected + 1);
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            CHECK(steps[i] == peer.step * static_cast<long long>(i));
        }
    }
}

/// The SIL Kit synchronisation invariant: a participant begins its step at T only after every
/// other participant has finished all of its steps before T.
void check_nobody_ran_ahead(const std::vector<PeerSpec>& peers, StepLog& log)
{
    std::map<std::string, std::vector<SimDuration>> finished;
    for (const auto& event : log.events)
    {
        if (!event.begin)
        {
            finished[event.participant].push_back(event.time);
            continue;
        }
        for (const auto& other : peers)
        {
            if (other.name == event.participant)
            {
                continue;
            }
            // Steps of `other` that start before event.time: 0, step, ... < event.time.
            const auto required = static_cast<std::size_t>((event.time + other.step - SimDuration(1)) / other.step);
            const auto& done = finished[other.name];
            if (done.size() < required)
            {
                throw vehicle_sil::unit::Failure(event.participant + " began t=" + std::to_string(event.time.count()) +
                                                 " ns while " + other.name + " had finished only " +
                                                 std::to_string(done.size()) + " of " + std::to_string(required) +
                                                 " earlier steps");
            }
        }
    }
}

} // namespace

TEST_CASE(virtual_time_different_steps_stay_synchronised_and_run_faster_than_real_time)
{
    // Large steps keep the number of synchronisation rounds low, so even sanitizer builds are
    // far faster than real time.
    const std::vector<PeerSpec> peers{{"Physics", 20ms}, {"J1939Ecu", 100ms}, {"LinMaster", 50ms}};
    constexpr SimDuration kStopAt = 10s;
    StepLog log;
    std::map<std::string, PeerResult> results;
    const auto wall = run_simulation(peers, kStopAt, log, results);

    check_step_grid(peers, kStopAt, results);
    check_nobody_ran_ahead(peers, log);
    std::printf("simulated %lld ms in %lld ms wall time\n", static_cast<long long>(kStopAt / 1ms),
                static_cast<long long>(wall / 1ms));
    CHECK(wall < kStopAt / 2); // well above real time
}

TEST_CASE(virtual_time_waits_for_the_slowest_peer)
{
    // Physics needs 3 ms of wall time per 2 ms step, so the whole simulation (including the
    // fast peers) runs at no more than 2/3 of real-time speed.
    const std::vector<PeerSpec> peers{{"Physics", 2ms, 3000us}, {"J1939Ecu", 10ms}, {"LinMaster", 5ms}};
    constexpr SimDuration kStopAt = 300ms;
    StepLog log;
    std::map<std::string, PeerResult> results;
    const auto wall = run_simulation(peers, kStopAt, log, results);

    check_step_grid(peers, kStopAt, results);
    check_nobody_ran_ahead(peers, log);
    std::printf("simulated %lld ms in %lld ms wall time\n", static_cast<long long>(kStopAt / 1ms),
                static_cast<long long>(wall / 1ms));
    // 150 physics steps of at least 3 ms each before the controller's step at stop_at.
    CHECK(wall >= 450ms);
    CHECK(wall > kStopAt); // slower than real time
}

TEST_CASE(virtual_time_pacing_runs_at_the_requested_speed)
{
    struct Case
    {
        double factor;
        SimDuration stop_at;
    };
    for (const Case c : {Case{4.0, 2s}, Case{0.5, 400ms}})
    {
        // Only one participant is paced; the others follow because they wait for it.
        const std::vector<PeerSpec> peers{{"Paced", 10ms, 0us, c.factor}, {"Free", 5ms}};
        StepLog log;
        std::map<std::string, PeerResult> results;
        const auto wall = run_simulation(peers, c.stop_at, log, results);
        check_step_grid(peers, c.stop_at, results);
        check_nobody_ran_ahead(peers, log);

        const auto expected = std::chrono::duration_cast<std::chrono::nanoseconds>(c.stop_at / c.factor);
        std::printf("factor %.2f: simulated %lld ms in %lld ms wall time (expected %lld ms)\n", c.factor,
                    static_cast<long long>(c.stop_at / 1ms), static_cast<long long>(wall / 1ms),
                    static_cast<long long>(expected / 1ms));
        // The paced participant's step [stop_at - 10 ms, stop_at) ends no earlier than
        // stop_at / factor after its own first step began; the controller's step at stop_at
        // follows. The controller measures from its own first step, which may begin a few ms
        // after the paced participant's, hence the tolerance.
        // The start skew between participants reached 20 ms in sanitizer builds.
        CHECK(wall >= expected - 50ms);
        CHECK(wall < expected + expected / 4 + 200ms);
    }
}

TEST_CASE(virtual_time_tick_scheduler_drives_a_fixed_period_tick)
{
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Ecu"};
    controllerOptions.stop_at = 100ms;
    SimulationController controller(controllerOptions);

    ParticipantRuntime runtime(coordinated_options("Ecu", registry.uri, VirtualTimeOptions{5ms, std::nullopt}));

    // An existing ECU with a 10 ms tick() and a 20 ms background task, hosted on a 5 ms step.
    std::vector<long long> ticks;
    std::vector<long long> background;
    TickScheduler scheduler(5ms);
    const auto tickIndex = scheduler.add("tick", 10ms, [&](SimTime t) { ticks.push_back(to_ns(t) / 1'000'000); });
    scheduler.add("background", 20ms, [&](SimTime t) { background.push_back(to_ns(t) / 1'000'000); });
    runtime.set_step_handler([&](SimTime now, SimDuration) {
        if (now < SimTime{} + 100ms)
        {
            scheduler.run_due(now);
        }
    });

    start_together(controller, {&runtime}, 30s);
    CHECK(controller.wait_until_finished(30s) == orch::ParticipantState::Shutdown);
    CHECK(runtime.wait_until_finished(30s) == orch::ParticipantState::Shutdown);

    const std::vector<long long> expectedTicks{0, 10, 20, 30, 40, 50, 60, 70, 80, 90};
    const std::vector<long long> expectedBackground{0, 20, 40, 60, 80};
    CHECK(ticks == expectedTicks);
    CHECK(background == expectedBackground);
    CHECK_EQ(scheduler.tick_count(tickIndex), 10u);
}

TEST_CASE(virtual_time_step_handler_exception_is_reported)
{
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Faulty"};
    controllerOptions.stop_at = 1s;
    controllerOptions.shutdown_timeout = 2s;
    SimulationController controller(controllerOptions);

    auto options = coordinated_options("Faulty", registry.uri, VirtualTimeOptions{10ms, std::nullopt});
    options.shutdown_timeout = 2s;
    ParticipantRuntime runtime(options);
    runtime.set_step_handler([](SimTime now, SimDuration) {
        if (now >= SimTime{} + 30ms)
        {
            throw std::runtime_error("ECU model diverged");
        }
    });

    start_together(controller, {&runtime}, 30s);

    const auto state = runtime.wait_until_finished(10s);
    CHECK(state.has_value());
    CHECK(*state == orch::ParticipantState::Error);
    CHECK(runtime.step_error().find("ECU model diverged") != std::string::npos);
    CHECK(runtime.step_error().find("t=30000000 ns") != std::string::npos);
    CHECK_EQ(runtime.steps_completed(), 3u);
    // SIL Kit does not end the simulation by itself: the controller has to see the failure.
    CHECK(vehicle_sil::unit::eventually([&] { return controller.failed_participant() == std::optional<std::string>("Faulty"); }, 10s));
    CHECK(!controller.wait_until_finished(50ms).has_value()); // still waiting for time to advance
    controller.abort();
}

TEST_CASE(virtual_time_end_limits_the_handled_steps)
{
    // The simulation runs past `end`; the handler must see exactly the steps before it.
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Ended"};
    controllerOptions.stop_at = 100ms;
    SimulationController controller(controllerOptions);

    ParticipantRuntime runtime(
        coordinated_options("Ended", registry.uri, VirtualTimeOptions{10ms, std::nullopt, SimTime{} + 45ms}));
    std::vector<long long> handled;
    runtime.set_step_handler([&](SimTime now, SimDuration) { handled.push_back(now.time_since_epoch() / 1ms); });

    start_together(controller, {&runtime}, 30s);

    const auto state = runtime.wait_until_finished(30s);
    CHECK(state.has_value());
    CHECK(*state == orch::ParticipantState::Shutdown);
    CHECK(runtime.now() >= SimTime{} + 90ms); // the steps after `end` did take place
    const std::vector<long long> expected{0, 10, 20, 30, 40};
    CHECK(handled == expected);
    CHECK_EQ(runtime.steps_completed(), 5u);
}

TEST_CASE(virtual_time_api_misuse_is_rejected)
{
    InProcessRegistry registry;
    {
        ParticipantRuntimeOptions options;
        options.participant_name = "NoSync";
        options.registry_uri = registry.uri;
        ParticipantRuntime runtime(options);
        CHECK(!runtime.uses_virtual_time());
        CHECK(throws<std::logic_error>([&] { runtime.set_step_handler([](SimTime, SimDuration) {}); }));
    }
    {
        ParticipantRuntimeOptions options;
        options.participant_name = "NoHandler";
        options.registry_uri = registry.uri;
        options.virtual_time = VirtualTimeOptions{1ms, std::nullopt};
        ParticipantRuntime runtime(options);
        CHECK(runtime.uses_virtual_time());
        CHECK(throws<std::logic_error>([&] { runtime.start(1s); }));
    }
    int invalidIndex = 0;
    for (const auto& vt : {VirtualTimeOptions{0ms, std::nullopt}, VirtualTimeOptions{1ms, 0.0}})
    {
        ParticipantRuntimeOptions options;
        // Distinct names: the registry may not have dropped the previous connection yet.
        options.participant_name = "Invalid" + std::to_string(invalidIndex++);
        options.registry_uri = registry.uri;
        options.virtual_time = vt;
        CHECK(throws<std::invalid_argument>([&] { ParticipantRuntime runtime(options); }));
    }
    {
        SimulationControllerOptions options;
        options.registry_uri = registry.uri;
        // No required participants.
        CHECK(throws<std::invalid_argument>([&] { SimulationController controller(options); }));
    }
}
