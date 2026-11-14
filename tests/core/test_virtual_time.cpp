// SPDX-License-Identifier: MIT
//
// ParticipantRuntime and SimulationController with real SIL Kit time synchronisation
// (in-process registry, several participants with different step sizes).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/core/tick_scheduler.hpp"

#include <silkit/services/orchestration/ISystemMonitor.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;
using Wall = std::chrono::steady_clock;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::start_together;
using vehicle_sil::unit::StateLog;
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
                vehicle_sil::unit::busy_for(peer.busy);
            }
            result->steps.push_back(now.time_since_epoch());
            log.add(peer.name, false, now);
        });
        runtimePointers.push_back(runtime.get());
        runtimes.push_back(std::move(runtime));
    }

    start_together(controller, runtimePointers, 30s);

    const auto controllerState = controller.wait_until_finished(scaled_timeout(60s));
    CHECK(controllerState.has_value());
    CHECK(*controllerState == orch::ParticipantState::Shutdown);
    for (auto& runtime : runtimes)
    {
        const auto state = runtime->wait_until_finished(scaled_timeout(30s));
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
    // The wall time runs from the controller's step at 0 to its step at stop_at. Physics' step at
    // 0 runs alongside the controller's (granted at the same time), so it may be over before the
    // wall time starts; the other 149 steps of at least 3 ms each lie between the two, as the
    // controller's step at stop_at waits for Physics' step from 298 ms to end.
    CHECK(wall >= (kStopAt / 2ms - 1) * 3ms);
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
    CHECK(controller.wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
    CHECK(runtime.wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);

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

    const auto state = runtime.wait_until_finished(scaled_timeout(10s));
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

namespace {

/// What "Ecu" (a ParticipantRuntime with a step of 10 ms and the given SIL Kit configuration) sees
/// next to "Driver": a plain SIL Kit participant with a step of 3 ms that asks every participant
/// for dynamic simulation steps (Experimental.TimeSynchronization.DynamicSimulationStep: true), as
/// a network simulator of another vendor may.
struct BesideADynamicStepDriver
{
    std::vector<std::pair<SimDuration, SimDuration>> steps; ///< time and size of each step of "Ecu"
    std::optional<orch::ParticipantState> state;            ///< the state "Ecu" ended in
    std::string step_error;
    std::size_t driver_steps = 0;
};

BesideADynamicStepDriver run_beside_a_dynamic_step_driver(const std::string& ecu_configuration)
{
    BesideADynamicStepDriver result;
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Ecu", "Driver"};
    controllerOptions.stop_at = 60ms;
    controllerOptions.shutdown_timeout = 2s;
    SimulationController controller(controllerOptions);

    auto options = coordinated_options("Ecu", registry.uri, VirtualTimeOptions{10ms, std::nullopt});
    options.participant_configuration = ecu_configuration;
    options.shutdown_timeout = 2s;
    ParticipantRuntime ecu(options);
    std::mutex mutex;
    ecu.set_step_handler([&](SimTime now, SimDuration step) {
        const std::lock_guard lock(mutex);
        result.steps.emplace_back(now.time_since_epoch(), step);
    });

    std::atomic<std::size_t> driverSteps{0};
    const auto driver = SilKit::CreateParticipant(
        // A configuration of the test's own (participant_configuration_or_default() keeps it as it is).
        SilKit::Config::ParticipantConfigurationFromString(participant_configuration_or_default(
            R"({"Middleware":{"EnableDomainSockets":false,"AcceptorUris":["tcp://127.0.0.1:0"]},)"
            R"("Experimental":{"TimeSynchronization":{"DynamicSimulationStep":true}}})",
            registry.uri)),
        "Driver", registry.uri);
    auto* driverLifecycle = driver->CreateLifecycleService({orch::OperationMode::Coordinated});
    driverLifecycle->CreateTimeSyncService()->SetSimulationStepHandler(
        [&driverSteps](std::chrono::nanoseconds, std::chrono::nanoseconds) { driverSteps.fetch_add(1); }, 3ms);
    auto driverDone = driverLifecycle->StartLifecycle();

    start_together(controller, {&ecu}, 30s);
    CHECK(vehicle_sil::unit::eventually(
        [&] { return controller.failed_participant().has_value() || controller.wait_until_finished(0ms).has_value(); }, 30s));
    result.state = ecu.wait_until_finished(10s);
    if (controller.failed_participant().has_value())
    {
        controller.abort(); // SIL Kit does not end the simulation for a participant in Error
    }
    CHECK(driverDone.wait_for(10s) == std::future_status::ready);
    result.step_error = ecu.step_error();
    result.driver_steps = driverSteps.load();
    const std::lock_guard lock(mutex);
    return result;
}

} // namespace

TEST_CASE(virtual_time_keeps_its_step_when_another_participant_asks_for_dynamic_steps)
{
    // The default configuration opts out: the steps stay on the participant's own grid.
    const auto result = run_beside_a_dynamic_step_driver("");
    CHECK(result.state == std::optional(orch::ParticipantState::Shutdown));
    CHECK(result.step_error.empty());
    CHECK(result.driver_steps >= 20u); // 60 ms in steps of 3 ms: the driver did run beside it
    CHECK(result.steps.size() >= 6u);
    for (std::size_t i = 0; i < result.steps.size(); ++i)
    {
        CHECK_EQ(result.steps[i].first.count(), (10ms * static_cast<long long>(i)).count() * 1'000'000);
        CHECK_EQ(result.steps[i].second.count(), SimDuration(10ms).count());
    }
}

TEST_CASE(virtual_time_fails_the_step_that_sil_kit_shortened)
{
    // A configuration of the caller's that leaves the key out follows the driver: SIL Kit cuts the
    // step of 10 ms down to the driver's 3 ms. The participant fails instead of running off its grid.
    const auto result = run_beside_a_dynamic_step_driver(
        R"({"Middleware":{"EnableDomainSockets":false,"AcceptorUris":["tcp://127.0.0.1:0"]}})");
    CHECK(result.state == std::optional(orch::ParticipantState::Error));
    CHECK(result.steps.empty()); // the first step is already the short one; the handler never ran
    std::fprintf(stderr, "%s\n", result.step_error.c_str());
    CHECK(result.step_error.find("SIL Kit gave this participant a step of 3000000 ns at t=0 ns, not its own 10000000 ns") !=
          std::string::npos);
    CHECK(result.step_error.find("this participant's SIL Kit configuration asks for dynamic simulation steps itself, or does "
                                 "not opt out while another participant asks for them. Set "
                                 "\"Experimental\":{\"TimeSynchronization\":{\"DynamicSimulationStep\":false}} in that "
                                 "configuration, or pass none") != std::string::npos);
}

/// A coordinated simulation of "Faulty" alone, with a controller that ends it only by an abort,
/// and a log of the states of "Faulty". The step handler at 30 ms tells `entered`, then waits until
/// the test calls release(); `fails` makes it throw then.
struct HeldStep
{
    explicit HeldStep(bool fails) :
      log(registry.uri, "Faulty"), controller(controller_options(registry.uri)), runtime(runtime_options(registry.uri))
    {
        runtime.set_step_handler([this, fails](SimTime now, SimDuration) {
            if (now == SimTime{} + 30ms)
            {
                // Like Peer: its Host has failed (Peer::failed() is true) before the exception
                // leaves the step handler.
                entered.set_value();
                if (release_future.wait_for(scaled_timeout(10s)) != std::future_status::ready)
                {
                    throw std::runtime_error("the test did not release the step");
                }
                if (fails)
                {
                    throw std::runtime_error("ECU model diverged");
                }
            }
            else if (now > SimTime{} + 30ms)
            {
                ++later_steps;
            }
        });
        start_together(controller, {&runtime}, 30s);
        CHECK(entered_future.wait_for(scaled_timeout(10s)) == std::future_status::ready);
    }

    ~HeldStep() { controller.abort(); }

    HeldStep(const HeldStep&) = delete;
    HeldStep& operator=(const HeldStep&) = delete;

    void release() { released.set_value(); }

    /// It left through Error, and the controller saw a failure. An ordinary stop first (Running,
    /// then Stopping) would end the coordinated simulation as if nothing had failed.
    void check_left_through_error()
    {
        CHECK(vehicle_sil::unit::eventually([&] { return log.reached(orch::ParticipantState::Shutdown); }, 10s));
        const auto next = log.after_running();
        if (next != orch::ParticipantState::Error)
        {
            std::fprintf(stderr, "  states of Faulty: %s\n", log.text().c_str());
        }
        CHECK(next == orch::ParticipantState::Error);
        CHECK(vehicle_sil::unit::eventually([&] { return controller.failed_participant() == std::optional<std::string>("Faulty"); }, 10s));
    }

    static SimulationControllerOptions controller_options(const std::string& uri)
    {
        SimulationControllerOptions o;
        o.registry_uri = uri;
        o.required_participants = {"Faulty"};
        o.stop_at = 60s; // ends only by the failure
        o.shutdown_timeout = 2s;
        return o;
    }

    static ParticipantRuntimeOptions runtime_options(const std::string& uri)
    {
        auto o = coordinated_options("Faulty", uri, VirtualTimeOptions{10ms, std::nullopt});
        o.shutdown_timeout = 5s;
        return o;
    }

    InProcessRegistry registry;
    StateLog log;
    SimulationController controller;
    std::promise<void> entered;
    std::future<void> entered_future = entered.get_future();
    std::promise<void> released;
    std::future<void> release_future = released.get_future();
    std::atomic<int> later_steps{0};
    ParticipantRuntime runtime; // last: destroyed first, before what its handler uses
};

TEST_CASE(virtual_time_stop_during_a_failing_step_leaves_through_error)
{
    // The owner sees the failure (Peer::failed()) while the handler still runs, and stops it.
    HeldStep held(true);
    std::thread owner([&] { held.runtime.stop("the owner saw the step fail"); });
    std::this_thread::sleep_for(50ms); // stop() waits for the handler meanwhile
    held.release();
    owner.join();
    held.check_left_through_error();
    CHECK(held.runtime.step_error().find("ECU model diverged") != std::string::npos);
}

TEST_CASE(virtual_time_stop_request_during_a_failing_step_leaves_through_error)
{
    // No stop(): the request itself goes out once the participant is in Error.
    HeldStep held(true);
    held.runtime.request_stop("the owner saw the step fail");
    held.release();
    held.check_left_through_error();
}

TEST_CASE(virtual_time_stop_request_during_a_step_goes_out_after_it)
{
    // A request from another thread while the handler runs (and does not fail) is kept until the
    // handler has returned, then delivered: the simulation stops in order.
    HeldStep held(false);
    held.runtime.request_stop("the owner is done");
    held.release();
    CHECK(held.runtime.wait_until_finished(scaled_timeout(10s)) == std::optional<orch::ParticipantState>(orch::ParticipantState::Shutdown));
    CHECK(held.log.after_running() == orch::ParticipantState::Stopping);
    CHECK(held.runtime.step_error().empty());
}

TEST_CASE(virtual_time_reported_failure_takes_a_kept_request_through_error)
{
    // report_failure() from another thread while the handler runs; a request made meanwhile is
    // kept past the handler (the participant is still Running) and delivered once it is in Error
    // (by its own status), without stop().
    HeldStep held(false);
    held.runtime.report_failure("the driver failed");
    held.runtime.request_stop("the owner saw the failure");
    held.release();
    held.check_left_through_error();
    CHECK(held.runtime.step_error().empty());
    CHECK_EQ(held.later_steps.load(), 0);
}

TEST_CASE(virtual_time_failure_reported_before_start_is_ignored)
{
    // SIL Kit ignores an error reported before the lifecycle starts; so does the runtime: the
    // participant runs, and its own stop request is not kept waiting for an Error.
    InProcessRegistry registry;
    SimulationControllerOptions controllerOptions;
    controllerOptions.registry_uri = registry.uri;
    controllerOptions.required_participants = {"Early"};
    controllerOptions.stop_at = 60s;
    SimulationController controller(controllerOptions);
    StateLog log(registry.uri, "Early");
    ParticipantRuntime runtime(coordinated_options("Early", registry.uri, VirtualTimeOptions{10ms, std::nullopt}));
    std::atomic<int> steps{0};
    runtime.set_step_handler([&runtime, &steps](SimTime now, SimDuration) {
        ++steps;
        if (now == SimTime{} + 20ms)
        {
            runtime.request_stop("done");
        }
    });
    runtime.report_failure("too early");
    start_together(controller, {&runtime}, 30s);
    CHECK(runtime.wait_until_finished(scaled_timeout(10s)) == std::optional<orch::ParticipantState>(orch::ParticipantState::Shutdown));
    CHECK(steps.load() < 50); // the request stopped it soon after 20 ms, not the controller at 60 s
    CHECK(log.after_running() == orch::ParticipantState::Stopping);
    CHECK(!log.reached(orch::ParticipantState::Error));
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

    const auto state = runtime.wait_until_finished(scaled_timeout(30s));
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

TEST_CASE(participant_runtime_waits_while_the_registry_still_has_the_name)
{
    // A participant with the name ends while another process (here: the same) joins with it: the
    // registry refuses the name until it has seen the end; the runtime tries again until then.
    InProcessRegistry registry;
    ParticipantRuntimeOptions options;
    options.participant_name = "Again";
    options.registry_uri = registry.uri;
    auto earlier = std::make_unique<ParticipantRuntime>(options);
    const auto began = std::chrono::steady_clock::now();
    std::thread ender([&] {
        std::this_thread::sleep_for(300ms);
        earlier.reset();
    });
    std::unique_ptr<ParticipantRuntime> again;
    try
    {
        options.name_in_use_wait = 20s;
        again = std::make_unique<ParticipantRuntime>(options);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "refused: %s\n", e.what());
    }
    ender.join();
    CHECK(again != nullptr);
    // It was refused at first (the earlier one still had the name).
    CHECK(std::chrono::steady_clock::now() - began >= 250ms);
}

TEST_CASE(a_refused_name_leaves_the_registry_able_to_relay_to_the_participant_that_has_it)
{
#ifndef _WIN32
    // "Engine" accepts connections only on a local-domain socket, so a participant without
    // local-domain sockets reaches it only through the registry (a connect request it relays:
    // Engine connects to it). A second "Engine" is refused; the registry must still know where
    // the first one is, or the participant after it cannot join.
    InProcessRegistry registry;
    const std::string socket =
        (std::filesystem::temp_directory_path() / ("vsil-relay-" + std::to_string(::getpid()) + ".sock")).string();
    // Configurations of the test's own (participant_configuration_or_default() keeps them as they are).
    const auto config = [&registry](const std::string& middleware) {
        return SilKit::Config::ParticipantConfigurationFromString(
            participant_configuration_or_default("{\"Middleware\":{" + middleware + "}}", registry.uri));
    };
    const auto engine = create_participant(
        config("\"EnableDomainSockets\":true,\"AcceptorUris\":[\"local://" + socket + "\"]"), "Engine", registry.uri, 0ms);
    const auto tcp_only = config("\"EnableDomainSockets\":false,\"AcceptorUris\":[\"tcp://127.0.0.1:0\"]");
    // The relay works (the reference).
    CHECK(create_participant(tcp_only, "Before", registry.uri, 0ms) != nullptr);
    CHECK(throws<SilKit::ProtocolError>([&] { static_cast<void>(create_participant(tcp_only, "Engine", registry.uri, 0ms)); }));
    std::this_thread::sleep_for(200ms); // the registry sees the refused one's end
    std::unique_ptr<SilKit::IParticipant> after;
    try
    {
        after = create_participant(tcp_only, "After", registry.uri, 0ms);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "the participant after the refused one could not join: %s\n", e.what());
    }
    CHECK(after != nullptr);
    std::filesystem::remove(socket);
#endif
}

TEST_CASE(participant_runtime_says_how_long_the_name_was_refused)
{
    InProcessRegistry registry;
    ParticipantRuntimeOptions options;
    options.participant_name = "Twice";
    options.registry_uri = registry.uri;
    ParticipantRuntime first(options);
    options.name_in_use_wait = 300ms;
    const auto began = std::chrono::steady_clock::now();
    std::string message;
    try
    {
        ParticipantRuntime second(options);
    }
    catch (const std::exception& e)
    {
        message = e.what();
    }
    CHECK(std::chrono::steady_clock::now() - began >= 300ms);
    CHECK(message.find("A participant with the same name 'Twice' already exists") != std::string::npos);
    CHECK(message.find("(the registry refused the name \"Twice\" for 300 ms: another participant of this simulation "
                       "uses it, or the registry has not yet seen the end of an earlier participant with this name; ") !=
          std::string::npos);
    // Without waiting: refused at once; a negative wait is a mistake.
    options.name_in_use_wait = 0ms;
    CHECK(throws<SilKit::ProtocolError>([&] { ParticipantRuntime second(options); }));
    options.name_in_use_wait = -1ms;
    CHECK(throws<std::invalid_argument>([&] { ParticipantRuntime second(options); }));
}
