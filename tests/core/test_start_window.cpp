// SPDX-License-Identifier: MIT
//
// The window before the start, with real SIL Kit participants (in-process registry): a
// participant that holds its communication-ready handler open keeps everyone before the first
// step until it completes; the controller's start window runs a function there; the observer of
// the other participants' lifecycle states.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::eventually;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::throws;

} // namespace

TEST_CASE(start_window_an_open_communication_ready_keeps_every_participant_before_its_first_step)
{
    const InProcessRegistry registry;
    SimulationControllerOptions c;
    c.registry_uri = registry.uri;
    c.required_participants = {"A", "B"};
    c.stop_at = 50ms;
    SimulationController controller(c);
    ParticipantRuntime a(coordinated_options("A", registry.uri, VirtualTimeOptions{1ms}));
    ParticipantRuntime b(coordinated_options("B", registry.uri, VirtualTimeOptions{1ms}));
    std::atomic<int> steps_a{0};
    std::atomic<int> steps_b{0};
    a.set_step_handler([&](SimTime, SimDuration) { ++steps_a; });
    b.set_step_handler([&](SimTime, SimDuration) { ++steps_b; });

    std::atomic<bool> open{false};
    a.set_communication_ready_handler_async([&] { open.store(true); });
    // Nothing to complete before the handler was called.
    CHECK(!a.complete_communication_ready());

    std::atomic<int> started{0};
    std::thread start_a([&] {
        a.start(60s);
        ++started;
    });
    std::thread start_b([&] {
        b.start(60s);
        ++started;
    });
    std::thread start_controller([&] {
        controller.start(60s);
        ++started;
    });
    CHECK(eventually([&] { return open.load(); }, 30s));
    // The window is open: nobody starts, nobody steps, however long it stays open.
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(started.load(), 0);
    CHECK_EQ(steps_a.load(), 0);
    CHECK_EQ(steps_b.load(), 0);
    CHECK(a.state() == orch::ParticipantState::CommunicationInitialized);
    CHECK(b.state() != orch::ParticipantState::Running);

    CHECK(a.complete_communication_ready());
    CHECK(!a.complete_communication_ready()); // once
    start_a.join();
    start_b.join();
    start_controller.join();
    CHECK_EQ(started.load(), 3);
    CHECK(controller.wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
    CHECK(steps_a.load() >= 50);
    CHECK(steps_b.load() >= 50);

    // Only before start().
    CHECK(throws<std::logic_error>([&] { a.set_communication_ready_handler_async([] {}); }));
    CHECK(throws<std::logic_error>([&] { a.set_peer_status_observer([](const std::string&, const PeerStatus&) {}); }));
}

TEST_CASE(start_window_the_controllers_function_runs_before_any_step_and_on_its_own_thread)
{
    const InProcessRegistry registry;
    SimulationControllerOptions c;
    c.registry_uri = registry.uri;
    c.required_participants = {"A"};
    c.stop_at = 20ms;
    SimulationController controller(c);
    ParticipantRuntime a(coordinated_options("A", registry.uri, VirtualTimeOptions{1ms}));
    std::atomic<int> steps{0};
    a.set_step_handler([&](SimTime, SimDuration) { ++steps; });

    std::thread::id window_thread;
    int steps_in_window = -1;
    bool cancelled_in_window = true;
    bool deadline_ahead = false;
    orch::ParticipantState state_of_a = orch::ParticipantState::Invalid;
    const auto begin = std::chrono::steady_clock::now();
    controller.set_start_window([&](const StartWindow& window) {
        window_thread = std::this_thread::get_id();
        std::this_thread::sleep_for(200ms);
        steps_in_window = steps.load();
        cancelled_in_window = window.cancelled();
        deadline_ahead = window.deadline() > std::chrono::steady_clock::now() + 30s && window.deadline() <= begin + 61s;
        state_of_a = a.state();
    });
    std::thread::id starter_thread;
    std::thread start_a([&] { a.start(60s); });
    std::thread start_controller([&] {
        starter_thread = std::this_thread::get_id();
        controller.start(60s);
    });
    start_controller.join();
    start_a.join();
    // start() returned after the function did: what it wrote is ours to read.
    CHECK_EQ(steps_in_window, 0);
    CHECK(!cancelled_in_window);
    CHECK(deadline_ahead);
    CHECK(window_thread != std::thread::id{});
    CHECK(window_thread != starter_thread);
    CHECK(state_of_a != orch::ParticipantState::Running);
    CHECK(std::chrono::steady_clock::now() - begin >= 200ms);
    CHECK(controller.wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
    CHECK(steps.load() >= 20);
    // After the start there is no window to set.
    CHECK(throws<std::logic_error>([&] { controller.set_start_window([](const StartWindow&) {}); }));
}

TEST_CASE(start_window_the_peer_status_observer_sees_the_other_participants_states)
{
    const InProcessRegistry registry;
    SimulationControllerOptions c;
    c.registry_uri = registry.uri;
    c.required_participants = {"A"};
    c.stop_at = 10ms;
    SimulationController controller(c);
    ParticipantRuntime a(coordinated_options("A", registry.uri, VirtualTimeOptions{1ms}));
    a.set_step_handler([](SimTime, SimDuration) {});

    std::mutex mutex;
    std::map<std::string, std::vector<orch::ParticipantState>> seen;
    std::atomic<int> concurrent{0};
    std::atomic<bool> overlapped{false};
    controller.runtime().set_peer_status_observer([&](const std::string& name, const PeerStatus& status) {
        overlapped.store(overlapped.load() || concurrent.fetch_add(1) != 0);
        if (status.state)
        {
            const std::lock_guard lock(mutex);
            seen[name].push_back(*status.state);
        }
        concurrent.fetch_sub(1);
    });
    std::thread start_a([&] { a.start(60s); });
    controller.start(60s);
    start_a.join();
    CHECK(controller.wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
    a.stop("done");
    const auto has = [&](const std::string& name, orch::ParticipantState state) {
        const std::lock_guard lock(mutex);
        const auto& states = seen[name];
        return std::find(states.begin(), states.end(), state) != states.end();
    };
    CHECK(eventually([&] { return has("A", orch::ParticipantState::Shutdown); }, 30s));
    CHECK(has("A", orch::ParticipantState::Running));
    CHECK(has("SimulationController", orch::ParticipantState::Running)); // its own states too
    CHECK(!overlapped.load()); // one call at a time
    {
        // What the observer was told last is what peer_status() says.
        const std::lock_guard lock(mutex);
        CHECK(seen["A"].back() == *controller.runtime().peer_status("A").state);
    }
}

TEST_CASE(start_window_that_opens_after_start_gave_up_is_completed_without_its_function)
{
    // The controller's start() gives up before the window opens (here: at once, with every
    // required participant already waiting). The window then opens with nobody waiting for it:
    // it is completed all the same (without the function), so that everyone gets to the first
    // step and the controller's stop ends the simulation in order, as without a window.
    for (const bool destroy_at_once : {false, true})
    {
        int late_windows = 0;
        for (int round = 0; round < 20 && late_windows < 3; ++round)
        {
            const InProcessRegistry registry;
            SimulationControllerOptions c;
            c.registry_uri = registry.uri;
            c.required_participants = {"A"};
            c.time_control = TimeControlOptions{};
            auto controller = std::make_unique<SimulationController>(c);
            ParticipantRuntime a(coordinated_options("A", registry.uri, VirtualTimeOptions{1ms}));
            a.set_step_handler([](SimTime, SimDuration) {});
            std::atomic<int> calls{0};
            controller->set_start_window([&](const StartWindow&) { ++calls; });

            std::atomic<bool> started{false};
            std::thread start_a([&] {
                try
                {
                    a.start(30s);
                    started.store(true);
                }
                catch (const std::exception&)
                {
                    // Stopped before it was seen running (the controller went at once).
                }
            });
            // Whatever a check below throws, the thread is joined (after the controller has gone,
            // which lets "A" start or fail).
            struct Join
            {
                std::unique_ptr<SimulationController>& controller;
                std::thread& thread;
                ~Join()
                {
                    controller.reset();
                    if (thread.joinable())
                    {
                        thread.join();
                    }
                }
            } const join{controller, start_a};
            CHECK(eventually([&] { return controller->missing_participants().empty(); }, 30s));
            // Publishes the workflow and gives up at once: as a rule the window has not opened yet
            // (it takes messages to and fro). If this thread is kept from running for about a
            // millisecond right after the lifecycle was started (seen on Windows under load),
            // the window opens and the simulation runs before start() looks at the time: start()
            // then returns, having called the function. That round shows nothing about a late
            // window and is not counted.
            std::string gave_up;
            bool returned = false;
            try
            {
                controller->start(0ms);
                returned = true;
            }
            catch (const StartTimeoutError&)
            {
            }
            catch (const std::exception& e)
            {
                gave_up = std::string("start(0 ms) threw another exception: ") + e.what();
            }
            if (!gave_up.empty())
            {
                throw vehicle_sil::unit::Failure(gave_up);
            }
            if (returned)
            {
                CHECK_EQ(calls.load(), 1); // start() returns only after the window's function did
            }
            if (!destroy_at_once)
            {
                // The window opens and is completed: "A" gets to run.
                CHECK(eventually([&] { return started.load(); }, 30s));
            }
            const auto begin = std::chrono::steady_clock::now();
            controller.reset(); // its stop ends the simulation in order, without waiting out its shutdown timeout
            CHECK(std::chrono::steady_clock::now() - begin < 4s);
            start_a.join();
            // In order: Shutdown, not an Error after a lost connection.
            CHECK(a.wait_until_finished(scaled_timeout(30s)) == orch::ParticipantState::Shutdown);
            late_windows += calls.load() == 0 ? 1 : 0;
        }
        // The window did open after start() had given up, and its function was not called.
        CHECK(late_windows >= 1);
    }
}

TEST_CASE(peer_statuses_can_be_re_read_from_several_threads_at_once)
{
    // refresh_peer_statuses() and wait_for_peer() add a status handler and remove it again. SIL
    // Kit's C++ API keeps the handlers of a system monitor in a table without a lock, so the
    // runtime must let only one thread at a time do that (ThreadSanitizer sees it otherwise).
    InProcessRegistry registry;
    ParticipantRuntimeOptions options;
    options.participant_name = "Watcher";
    options.registry_uri = registry.uri;
    ParticipantRuntime runtime(options);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&runtime] {
            for (int i = 0; i < 500; ++i)
            {
                runtime.refresh_peer_statuses();
            }
        });
    }
    // wait_for_peer() re-reads too (every 200 ms while it waits).
    CHECK(!runtime.wait_for_peer("Nobody", [](const auto& status) { return status.state.has_value(); }, 450ms));
    for (auto& thread : threads)
    {
        thread.join();
    }
}
