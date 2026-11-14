// SPDX-License-Identifier: MIT
//
// What ParticipantRuntime::stop() promises about the CommunicationReady handlers, with real SIL
// Kit participants (in-process registry): once it has returned, none of them is called, and one
// that runs is waited for. The case that needs it is a start that timed out: SIL Kit takes no stop
// before Running, so the lifecycle lives on after stop() and gets to CommunicationReady when the
// participant it waited for comes after all, while the caller destroys what the handlers use.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::unit::coordinated_options;
using vehicle_sil::unit::eventually;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::scaled_timeout;
using vehicle_sil::unit::throws;

enum class Registered
{
    Set,   ///< set_communication_ready_handler()
    Added, ///< add_communication_ready_handler() alone
    Async, ///< set_communication_ready_handler_async(), with an added one
};

struct AfterStop
{
    int calls = 0;        ///< of any handler
    bool went_on = false; ///< the lifecycle got past CommunicationReady
};

/// A coordinated participant whose start times out (nobody else is there) and that is stopped;
/// then the system controller comes, and with it the participant's CommunicationReady.
AfterStop communication_ready_after_stop(Registered registered)
{
    const InProcessRegistry registry;
    auto options = coordinated_options("Late", registry.uri, VirtualTimeOptions{1ms});
    options.shutdown_timeout = 100ms; // stop() waits this long: nothing stops a lifecycle before Running
    ParticipantRuntime runtime(options);
    std::atomic<int> calls{0};
    runtime.set_step_handler([](SimTime, SimDuration) {});
    switch (registered)
    {
    case Registered::Set:
        runtime.set_communication_ready_handler([&] { ++calls; });
        break;
    case Registered::Added:
        runtime.add_communication_ready_handler([&] { ++calls; });
        break;
    case Registered::Async:
        runtime.add_communication_ready_handler([&] { ++calls; });
        runtime.set_communication_ready_handler_async([&] { ++calls; });
        break;
    }
    CHECK(throws<StartTimeoutError>([&] { runtime.start(100ms); }));
    runtime.stop("the start timed out");
    CHECK_EQ(calls.load(), 0);

    SimulationControllerOptions c;
    c.registry_uri = registry.uri;
    c.required_participants = {"Late"};
    c.shutdown_timeout = 2s;
    SimulationController controller(c);
    std::thread start_controller([&] {
        try
        {
            controller.start(scaled_timeout(30s));
        }
        catch (const std::exception&) // NOLINT(bugprone-empty-catch)
        {
            // "Late" stops the simulation at its first step: Running may or may not be seen.
        }
    });
    AfterStop result;
    result.went_on = eventually(
        [&] {
            switch (runtime.state())
            {
            case orch::ParticipantState::Invalid:
            case orch::ParticipantState::ServicesCreated:
            case orch::ParticipantState::CommunicationInitializing:
            case orch::ParticipantState::CommunicationInitialized:
                return false;
            default:
                return true;
            }
        },
        30s);
    start_controller.join();
    result.calls = calls.load();
    return result;
}

} // namespace

TEST_CASE(communication_ready_after_stop_does_not_call_the_handler_that_was_set)
{
    const auto result = communication_ready_after_stop(Registered::Set);
    CHECK_EQ(result.calls, 0);
    CHECK(result.went_on);
}

TEST_CASE(communication_ready_after_stop_does_not_call_an_added_handler)
{
    const auto result = communication_ready_after_stop(Registered::Added);
    CHECK_EQ(result.calls, 0);
    CHECK(result.went_on);
}

TEST_CASE(communication_ready_after_stop_completes_the_window_without_calling_its_handler)
{
    // Nobody is told that the window opened, so nobody would complete it: the runtime does, and
    // the lifecycle goes on to where the stop reaches it, as with the other two kinds.
    const auto result = communication_ready_after_stop(Registered::Async);
    CHECK_EQ(result.calls, 0);
    CHECK(result.went_on);
}

TEST_CASE(communication_ready_handler_that_runs_is_waited_for_by_stop)
{
    const InProcessRegistry registry;
    ParticipantRuntimeOptions options{"Slow", registry.uri};
    options.shutdown_timeout = 50ms;
    ParticipantRuntime runtime(options);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> done{false};
    runtime.add_communication_ready_handler([&] {
        // Both are allowed in a SIL Kit handler, and neither may wait for this handler.
        runtime.request_stop("asked by the handler");
        runtime.report_failure("reported by the handler");
        entered.store(true);
        (void)eventually([&] { return release.load(); }, 60s);
        done.store(true);
    });
    // The lifecycle stands in the handler, before Running.
    CHECK(throws<StartTimeoutError>([&] { runtime.start(50ms); }));
    CHECK(eventually([&] { return entered.load(); }, 30s));

    std::atomic<bool> done_when_stop_returned{false};
    std::thread stopper([&] {
        runtime.stop("the start timed out");
        done_when_stop_returned.store(done.load());
    });
    // Long enough for a stop() that only waits for the shutdown (shutdown_timeout) to return.
    std::this_thread::sleep_for(300ms);
    release.store(true);
    stopper.join();
    CHECK(done_when_stop_returned.load());
}
