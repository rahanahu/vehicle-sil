// SPDX-License-Identifier: MIT
//
// run_bridge() with fake hooks: the exit code, the order of stopping (statistics before the SIL
// Kit participant stops), failures, warnings, the monitor line, and a participant that is still
// starting. Stops are asked from inside the loop (hooks, participant), so nothing waits for time.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/bridge_runtime/run_bridge.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::bridge_runtime;
using State = SilKit::Services::Orchestration::ParticipantState;
using vehicle_sil::unit::InProcessRegistry;

/// What happened, in order: printed lines as written, calls as "<who>.<call>".
using Events = std::vector<std::string>;

void check_events(const Events& actual, const Events& expected, int line)
{
    if (actual == expected)
    {
        return;
    }
    std::ostringstream message;
    message << __FILE__ << ':' << line << ": events differ\n  actual:";
    for (const auto& e : actual)
    {
        message << "\n    " << e;
    }
    message << "\n  expected:";
    for (const auto& e : expected)
    {
        message << "\n    " << e;
    }
    throw vehicle_sil::unit::Failure(message.str());
}
#define CHECK_EVENTS(actual, ...) check_events((actual), Events{__VA_ARGS__}, __LINE__)

/// Hooks whose answers the test sets; every call is recorded.
class FakeHooks final : public BridgeHooks
{
public:
    explicit FakeHooks(Events& events) : events_(events) {}

    /// Called at the start of every fault() call with the number of the call (from 1).
    std::function<std::optional<std::string>(int)> on_fault = [](int) { return std::nullopt; };
    std::function<std::vector<std::string>(int)> on_warnings = [](int) { return std::vector<std::string>{}; };
    std::function<std::optional<std::string>(int)> on_health = [](int) { return std::nullopt; };
    std::function<void()> on_final_statistics = [] {};

    std::optional<std::string> fault() override { return on_fault(++faults_); }
    std::vector<std::string> take_warnings() override { return on_warnings(++warnings_); }
    std::optional<std::string> health_line() override { return on_health(++healths_); }
    void stop() noexcept override { events_.push_back("hooks.stop"); }
    std::vector<std::string> final_statistics() override
    {
        events_.push_back("hooks.final_statistics");
        on_final_statistics();
        return {"stats forwarded=3", "last error: none really"};
    }

private:
    Events& events_;
    int faults_ = 0;
    int warnings_ = 0;
    int healths_ = 0;
};

/// A participant whose states the test scripts: state() returns `next(call number from 1)`.
class ScriptedParticipant final : public BridgeParticipant
{
public:
    ScriptedParticipant(Events& events, std::function<State(int)> next) : events_(events), next_(std::move(next)) {}

    State state() const override { return next_(++calls_); }
    void stop(std::string_view reason) noexcept override { events_.push_back("participant.stop " + std::string(reason)); }
    [[nodiscard]] int calls() const { return calls_; }

private:
    Events& events_;
    std::function<State(int)> next_;
    mutable int calls_ = 0;
};

RunBridgeOptions test_options(Events& events)
{
    RunBridgeOptions options;
    options.ready_line = "bridging A <-> B; Ctrl+C to stop";
    options.health_interval = 0ms; // asked on every round
    options.poll_interval = 1ms;
    options.handle_stop_signals = false;
    options.print = [&events](const std::string& line) { events.push_back(line); };
    return options;
}

/// Running throughout; asks for a stop at the state() call number `stop_at`.
std::function<State(int)> running_until_stop(int stop_at)
{
    return [stop_at](int call) {
        if (call == stop_at)
        {
            request_stop();
        }
        return State::Running;
    };
}

} // namespace

TEST_CASE(run_bridge_judges_only_stopping_and_ended_states_as_failures)
{
    for (const State s : {State::Invalid, State::ServicesCreated, State::CommunicationInitializing,
                          State::CommunicationInitialized, State::ReadyToRun})
    {
        CHECK(judge_participant_state(s) == ParticipantVerdict::Starting);
    }
    CHECK(judge_participant_state(State::Running) == ParticipantVerdict::Running);
    CHECK(judge_participant_state(State::Paused) == ParticipantVerdict::Running);
    for (const State s :
         {State::Stopping, State::Stopped, State::Error, State::ShuttingDown, State::Shutdown, State::Aborting})
    {
        CHECK(judge_participant_state(s) == ParticipantVerdict::Ended);
    }
}

TEST_CASE(run_bridge_stops_on_request_with_the_statistics_before_the_silkit_participant_stops)
{
    InProcessRegistry registry;
    ParticipantRuntime runtime(ParticipantRuntimeOptions{"BridgeRuntimeTest", registry.uri});
    runtime.start(10s);

    Events events;
    FakeHooks hooks(events);
    hooks.on_warnings = [](int call) {
        return call == 1 ? std::vector<std::string>{"first", "second"} : std::vector<std::string>{};
    };
    hooks.on_health = [](int call) -> std::optional<std::string> {
        if (call == 2)
        {
            request_stop(); // the round ends, the loop sees the request before its next round
            return "XL bus: error passive; since the last report: error_frames +3";
        }
        return std::nullopt;
    };
    // The statistics come while the SIL Kit participant still runs.
    std::optional<State> stateAtStatistics;
    hooks.on_final_statistics = [&] { stateAtStatistics = runtime.state(); };

    const int code = run_bridge(runtime, hooks, test_options(events));

    CHECK_EQ(code, 0);
    CHECK_EVENTS(events, "bridging A <-> B; Ctrl+C to stop", "warning: first", "warning: second",
                 "XL bus: error passive; since the last report: error_frames +3", "stopping", "hooks.stop",
                 "hooks.final_statistics", "stats forwarded=3", "last error: none really", "stopped");
    CHECK(stateAtStatistics == State::Running);
    // run_bridge() stopped the participant after the statistics.
    CHECK(judge_participant_state(runtime.state()) == ParticipantVerdict::Ended);
}

TEST_CASE(run_bridge_a_fault_fails_with_exit_code_1_after_warnings_and_with_the_statistics)
{
    Events events;
    FakeHooks hooks(events);
    hooks.on_fault = [](int call) -> std::optional<std::string> {
        if (call == 2)
        {
            return "XL channel 'X' stopped working (device removed?): lost";
        }
        return std::nullopt;
    };
    hooks.on_warnings = [](int) { return std::vector<std::string>{"w"}; };
    ScriptedParticipant participant(events, [](int) { return State::Running; });

    const int code = run_bridge(participant, hooks, test_options(events));

    CHECK_EQ(code, 1);
    // The fault is looked at before the warnings of the same round.
    CHECK_EVENTS(events, "bridging A <-> B; Ctrl+C to stop", "warning: w",
                 "FAIL: XL channel 'X' stopped working (device removed?): lost", "stopping", "hooks.stop",
                 "hooks.final_statistics", "stats forwarded=3", "last error: none really",
                 "participant.stop bridge stopped", "stopped");
}

TEST_CASE(run_bridge_a_participant_that_leaves_running_fails_with_exit_code_1)
{
    Events events;
    FakeHooks hooks(events);
    ScriptedParticipant participant(events, [](int call) { return call < 3 ? State::Running : State::Error; });
    auto options = test_options(events);
    options.stop_reason = "custom reason";

    CHECK_EQ(run_bridge(participant, hooks, options), 1);
    CHECK_EVENTS(events, "bridging A <-> B; Ctrl+C to stop", "FAIL: SIL Kit participant left Running (state Error)",
                 "stopping", "hooks.stop", "hooks.final_statistics", "stats forwarded=3", "last error: none really",
                 "participant.stop custom reason", "stopped");
}

TEST_CASE(run_bridge_a_participant_that_stops_before_running_fails_with_exit_code_1)
{
    Events events;
    FakeHooks hooks(events);
    ScriptedParticipant participant(events, [](int call) { return call < 3 ? State::ReadyToRun : State::Stopping; });

    CHECK_EQ(run_bridge(participant, hooks, test_options(events)), 1);
    CHECK(events.at(1) == "FAIL: SIL Kit participant stopped before it reached Running (state Stopping)");
}

TEST_CASE(run_bridge_a_participant_still_starting_is_not_a_failure)
{
    // ParticipantRuntime::start() may see the start succeed a moment before the state is Running.
    Events events;
    FakeHooks hooks(events);
    ScriptedParticipant participant(events, [](int call) {
        if (call == 20)
        {
            request_stop();
        }
        return call < 10 ? State::ReadyToRun : State::Running;
    });
    auto options = test_options(events);
    options.starting_notice_after = 1h; // no notice line in this test
    options.starting_limit = 1h;

    CHECK_EQ(run_bridge(participant, hooks, options), 0);
    CHECK_EQ(participant.calls(), 20);
    CHECK_EVENTS(events, "bridging A <-> B; Ctrl+C to stop", "stopping", "hooks.stop", "hooks.final_statistics",
                 "stats forwarded=3", "last error: none really", "participant.stop bridge stopped", "stopped");
}

TEST_CASE(run_bridge_says_once_what_it_waits_for_while_the_participant_is_starting)
{
    Events events;
    FakeHooks hooks(events);
    const auto noticed = [&events] {
        for (const auto& e : events)
        {
            if (e.starts_with("waiting for"))
            {
                return true;
            }
        }
        return false;
    };
    // Starting until the notice is out, and a few rounds more; then Running, then a stop.
    int runningFrom = 0;
    ScriptedParticipant participant(events, [&](int call) {
        if (runningFrom == 0 && noticed())
        {
            runningFrom = call + 3;
        }
        if (runningFrom != 0 && call >= runningFrom + 3)
        {
            request_stop();
        }
        return runningFrom != 0 && call >= runningFrom ? State::Running : State::ReadyToRun;
    });
    auto options = test_options(events);
    options.starting_notice_after = 0ms;
    options.starting_limit = 1h;

    CHECK_EQ(run_bridge(participant, hooks, options), 0);
    CHECK_EVENTS(events, "bridging A <-> B; Ctrl+C to stop",
                 "waiting for the SIL Kit participant to reach Running (state ReadyToRun; at most 3600000 ms)",
                 "stopping", "hooks.stop", "hooks.final_statistics", "stats forwarded=3", "last error: none really",
                 "participant.stop bridge stopped", "stopped");
}

TEST_CASE(run_bridge_a_participant_that_never_reaches_running_fails_at_the_starting_limit)
{
    Events events;
    FakeHooks hooks(events);
    ScriptedParticipant participant(events, [](int) { return State::ReadyToRun; });
    auto options = test_options(events);
    options.starting_limit = 0ms;

    CHECK_EQ(run_bridge(participant, hooks, options), 1);
    CHECK_EVENTS(events, "bridging A <-> B; Ctrl+C to stop",
                 "FAIL: SIL Kit participant did not reach Running within 0 ms (state ReadyToRun); "
                 "its SIL Kit log may say why",
                 "stopping", "hooks.stop", "hooks.final_statistics", "stats forwarded=3", "last error: none really",
                 "participant.stop bridge stopped", "stopped");
}

TEST_CASE(run_bridge_forgets_a_stop_requested_before_it_began)
{
    request_stop();
    Events events;
    FakeHooks hooks(events);
    ScriptedParticipant participant(events, running_until_stop(5));

    CHECK_EQ(run_bridge(participant, hooks, test_options(events)), 0);
    CHECK_EQ(participant.calls(), 5);
}
