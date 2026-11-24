// SPDX-License-Identifier: MIT
//
// What ParticipantRuntime::start() promises when it returns, with real SIL Kit participants
// (in-process registry): state() says Running or a later state, not a state before it.
//
// The places where start() and the SIL Kit thread can overtake each other are a few machine
// instructions wide, so a test that only looks often would pass without the code that closes them.
// The tests with a probe (participant_runtime_probe.hpp) stand at those places and decide the
// order themselves; their waits have upper bounds only for the case that the code is wrong.
#include "participant_runtime_probe.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"
#include "test_timeout_scale.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include <silkit/experimental/participant/ParticipantExtensions.hpp>
#include <silkit/experimental/services/orchestration/ISystemController.hpp>
#include <silkit/services/orchestration/string_utils.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace orch = SilKit::Services::Orchestration;
using vehicle_sil::detail::StartProbePoint;
using vehicle_sil::unit::InProcessRegistry;

/// Sets the probe for its lifetime. Declare it before the runtime, so that the runtime (and its
/// SIL Kit thread) is gone before the probe is.
class ProbeInPlace
{
public:
    explicit ProbeInPlace(detail::StartProbe probe) : probe_(std::move(probe)) { detail::set_start_probe(&probe_); }
    ~ProbeInPlace() { detail::set_start_probe(nullptr); }
    ProbeInPlace(const ProbeInPlace&) = delete;
    ProbeInPlace& operator=(const ProbeInPlace&) = delete;

private:
    detail::StartProbe probe_;
};

/// What the two threads tell each other. A wait that runs out is noted and not thrown: the probe
/// runs inside SIL Kit's handlers too.
struct Meeting
{
    std::mutex mutex;
    std::condition_variable changed;
    bool flagsRead = false;       ///< start() has read its flags for the first time
    bool handlerEntered = false;  ///< the starting handler has run as far as the probe
    int roundsSinceHandler = 0;   ///< rounds of start() that began after that
    bool startReturned = false;
    bool waitRanOut = false;

    template <typename Predicate>
    void wait_for(std::unique_lock<std::mutex>& lock, Predicate done)
    {
        if (!changed.wait_for(lock, unit::scaled_timeout(20s), done))
        {
            waitRanOut = true;
        }
    }
};

/// Holds the SIL Kit thread before ReadyToRun until start() has read its flags (both unset then)
/// and stands between its two reads.
void hold_the_lifecycle_until_the_flags_are_read(ParticipantRuntime& runtime, Meeting& meeting)
{
    runtime.set_communication_ready_handler([&meeting] {
        std::unique_lock lock(meeting.mutex);
        meeting.wait_for(lock, [&meeting] { return meeting.flagsRead; });
    });
}

} // namespace

TEST_CASE(participant_start_without_virtual_time_returns_once_the_state_is_running)
{
    // The ordinary path, with no probe. It does not show that start() waits for Running (SIL Kit
    // sets it a moment after the starting handler, and nothing here makes that moment long): the
    // tests below do.
    InProcessRegistry registry;
    for (int i = 0; i < 10; ++i)
    {
        ParticipantRuntime runtime(ParticipantRuntimeOptions{"Starter" + std::to_string(i), registry.uri});
        runtime.start(10s);
        const auto state = runtime.state();
        if (state != orch::ParticipantState::Running)
        {
            throw vehicle_sil::unit::Failure("start() returned in state " + orch::to_string(state) + " (round "
                                             + std::to_string(i) + "), expected Running");
        }
        runtime.stop("test");
    }
}

TEST_CASE(participant_start_does_not_return_while_the_starting_handler_runs)
{
    // The order that must not let start() return in ReadyToRun:
    //   1. start() reads "the starting handler has not run" and is overtaken;
    //   2. the SIL Kit thread sets ReadyToRun and calls the starting handler, which stays there;
    //   3. start() goes on, for at least two more rounds.
    // SIL Kit sets Running only after the handler has returned, so start() must still be waiting.
    InProcessRegistry registry;
    Meeting meeting;
    const ProbeInPlace probe([&meeting](StartProbePoint point) {
        std::unique_lock lock(meeting.mutex);
        if (point == StartProbePoint::BetweenFlagsAndState)
        {
            if (!meeting.flagsRead)
            {
                meeting.flagsRead = true;
                meeting.changed.notify_all();
                meeting.wait_for(lock, [&meeting] { return meeting.handlerEntered; });
            }
            else if (meeting.handlerEntered)
            {
                ++meeting.roundsSinceHandler;
                meeting.changed.notify_all();
            }
            return;
        }
        if (point != StartProbePoint::StartingHandlerEntered)
        {
            return;
        }
        meeting.handlerEntered = true;
        meeting.changed.notify_all();
        // The first of those rounds read its flags after the handler's; when the second begins,
        // the first has decided not to return.
        meeting.wait_for(lock, [&meeting] { return meeting.roundsSinceHandler >= 2 || meeting.startReturned; });
    });
    ParticipantRuntime runtime(ParticipantRuntimeOptions{"Starter", registry.uri});
    hold_the_lifecycle_until_the_flags_are_read(runtime, meeting);

    runtime.start(unit::scaled_timeout(60s));
    const auto state = runtime.state();
    int rounds = 0;
    bool waitRanOut = false;
    {
        const std::lock_guard lock(meeting.mutex);
        meeting.startReturned = true; // lets the handler go if start() left it behind
        rounds = meeting.roundsSinceHandler;
        waitRanOut = meeting.waitRanOut;
    }
    meeting.changed.notify_all();
    runtime.stop("test");

    if (state != orch::ParticipantState::Running)
    {
        throw vehicle_sil::unit::Failure("start() returned in state " + orch::to_string(state)
                                         + " while SIL Kit's starting handler had not returned, expected Running");
    }
    CHECK(rounds >= 2);
    CHECK(!waitRanOut);
}

TEST_CASE(participant_start_takes_a_run_that_ended_between_its_two_looks_for_a_start)
{
    // The other side of reading the flags before the state: after start() has read "the starting
    // handler has not run", the participant gets to Running and is stopped; start() then reads a
    // state after Running. That is a short run, not a start that failed.
    InProcessRegistry registry;
    Meeting meeting;
    ParticipantRuntime* started = nullptr;
    bool sawRunning = false;
    bool sawTheEnd = false;
    const ProbeInPlace probe([&](StartProbePoint point) {
        if (point != StartProbePoint::BetweenFlagsAndState)
        {
            return;
        }
        {
            const std::lock_guard lock(meeting.mutex);
            if (meeting.flagsRead)
            {
                return;
            }
            meeting.flagsRead = true;
        }
        meeting.changed.notify_all();
        const auto wait_for_state = [&](auto reached) {
            const auto deadline = std::chrono::steady_clock::now() + unit::scaled_timeout(20s);
            while (!reached(started->state()))
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(1ms);
            }
            return true;
        };
        sawRunning = wait_for_state([](orch::ParticipantState s) { return s == orch::ParticipantState::Running; });
        started->request_stop("test: stopped while start() looks");
        sawTheEnd = wait_for_state([](orch::ParticipantState s) {
            return s == orch::ParticipantState::Stopping || s == orch::ParticipantState::Stopped
                   || s == orch::ParticipantState::ShuttingDown || s == orch::ParticipantState::Shutdown;
        });
    });
    ParticipantRuntime runtime(ParticipantRuntimeOptions{"Starter", registry.uri});
    started = &runtime;
    hold_the_lifecycle_until_the_flags_are_read(runtime, meeting);

    std::string thrown;
    try
    {
        runtime.start(unit::scaled_timeout(60s));
    }
    catch (const std::exception& e)
    {
        thrown = e.what();
    }
    const auto state = runtime.state();
    runtime.stop("test");

    CHECK(sawRunning);
    CHECK(sawTheEnd);
    if (!thrown.empty())
    {
        throw vehicle_sil::unit::Failure("start() threw for a participant that was Running and then stopped: " + thrown);
    }
    CHECK(state != orch::ParticipantState::Running && state != orch::ParticipantState::ReadyToRun);
}

TEST_CASE(participant_start_takes_a_run_that_ended_while_the_caller_was_asked_whether_to_give_up_for_a_start)
{
    // A short run at full speed can begin and end between two looks of start(). The order:
    //   1. start() reads "no step has run" and a state before Running, and asks give_up();
    //   2. the first step runs and stops the participant; its state is Stopping or later;
    //   3. give_up() looks at the simulation now, and takes a participant that is stopping
    //      without a start for one that left before the start: it says so.
    // The step has run, so the simulation did start: start() must return and not throw.
    InProcessRegistry registry;
    Meeting meeting;
    ParticipantRuntimeOptions options{"Starter", registry.uri};
    options.virtual_time = VirtualTimeOptions{1ms, std::nullopt};
    ParticipantRuntime runtime(options);
    int steps = 0;
    runtime.set_step_handler([&](SimTime, SimDuration) {
        ++steps;
        runtime.request_stop("test: the run is over at its first step");
    });
    hold_the_lifecycle_until_the_flags_are_read(runtime, meeting);

    int asked = 0;
    bool sawTheEnd = false;
    const auto give_up = [&]() -> std::string {
        if (++asked > 1)
        {
            return "a participant left before the simulation started";
        }
        {
            const std::lock_guard lock(meeting.mutex);
            meeting.flagsRead = true; // start() has made both of its looks of this round
        }
        meeting.changed.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + unit::scaled_timeout(20s);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto state = runtime.state();
            if (state == orch::ParticipantState::Stopping || state == orch::ParticipantState::Stopped
                || state == orch::ParticipantState::ShuttingDown || state == orch::ParticipantState::Shutdown)
            {
                sawTheEnd = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        return "a participant left before the simulation started";
    };

    std::string thrown;
    try
    {
        runtime.start(unit::scaled_timeout(60s), give_up);
    }
    catch (const std::exception& e)
    {
        thrown = e.what();
    }
    runtime.stop("test");

    CHECK(sawTheEnd);
    CHECK_EQ(asked, 1);
    CHECK(steps >= 1);
    if (!thrown.empty())
    {
        throw vehicle_sil::unit::Failure("start() threw for a simulation that ran and ended: " + thrown);
    }
}

TEST_CASE(participant_start_goes_on_waiting_when_it_is_told_to_give_up_while_the_starting_handler_runs)
{
    // Without virtual time, the same order with the starting handler in place of the first step:
    // give_up() says "give up" after the starting handler has begun. The lifecycle is on its way
    // to Running, so start() neither throws nor returns in ReadyToRun: it returns in Running.
    InProcessRegistry registry;
    Meeting meeting;
    const ProbeInPlace probe([&meeting](StartProbePoint point) {
        if (point != StartProbePoint::StartingHandlerEntered)
        {
            return;
        }
        std::unique_lock lock(meeting.mutex);
        meeting.handlerEntered = true;
        meeting.changed.notify_all();
        // Stays in ReadyToRun until start() has been told to give up at least twice.
        meeting.wait_for(lock, [&meeting] { return meeting.roundsSinceHandler >= 2; });
    });
    ParticipantRuntime runtime(ParticipantRuntimeOptions{"Starter", registry.uri});
    hold_the_lifecycle_until_the_flags_are_read(runtime, meeting);

    const auto give_up = [&]() -> std::string {
        std::unique_lock lock(meeting.mutex);
        if (!meeting.flagsRead)
        {
            meeting.flagsRead = true;
            meeting.changed.notify_all();
            meeting.wait_for(lock, [&meeting] { return meeting.handlerEntered; });
        }
        else
        {
            ++meeting.roundsSinceHandler;
            meeting.changed.notify_all();
        }
        return "a participant left before the simulation started";
    };

    std::string thrown;
    std::optional<orch::ParticipantState> state;
    try
    {
        runtime.start(unit::scaled_timeout(60s), give_up);
        state = runtime.state();
    }
    catch (const std::exception& e)
    {
        thrown = e.what();
    }
    runtime.stop("test");

    CHECK(!meeting.waitRanOut);
    if (!thrown.empty())
    {
        throw vehicle_sil::unit::Failure("start() threw while the starting handler ran: " + thrown);
    }
    CHECK(state == std::optional(orch::ParticipantState::Running));
}

TEST_CASE(participant_without_virtual_time_passes_on_a_stop_when_it_runs_after_start_ran_out_of_time)
{
    // start() runs out of time before Running, and the caller asks for a stop, which SIL Kit
    // does not take before Running. Without virtual time there is no first step that could pass
    // the request on later, and start() is not looking any more: the runtime passes it on when it
    // sees its own Running.
    InProcessRegistry registry;
    Meeting meeting;
    ParticipantRuntime runtime(ParticipantRuntimeOptions{"Starter", registry.uri});
    hold_the_lifecycle_until_the_flags_are_read(runtime, meeting); // here: until the test lets go

    bool ranOut = false;
    try
    {
        runtime.start(50ms);
    }
    catch (const StartTimeoutError&)
    {
        ranOut = true;
    }
    runtime.request_stop("test: stop after start() ran out of time");
    {
        const std::lock_guard lock(meeting.mutex);
        meeting.flagsRead = true;
    }
    meeting.changed.notify_all();
    const auto state = runtime.wait_until_finished(unit::scaled_timeout(10s));
    runtime.stop("test");

    CHECK(ranOut);
    CHECK(!meeting.waitRanOut);
    CHECK(state == std::optional(orch::ParticipantState::Shutdown));
}

TEST_CASE(participant_start_reports_a_failure_that_a_handler_reported_before_start_was_back_from_the_lifecycle)
{
    // An autonomous participant's CommunicationReady handlers run on the SIL Kit thread as soon as
    // StartLifecycle() has queued the start, which can be before start() is back from it. The
    // probe holds start() there until the handlers are through; the failure of the first one must
    // not be lost.
    InProcessRegistry registry;
    Meeting meeting;
    const ProbeInPlace probe([&meeting](StartProbePoint point) {
        if (point == StartProbePoint::AfterStartLifecycle)
        {
            std::unique_lock lock(meeting.mutex);
            meeting.wait_for(lock, [&meeting] { return meeting.handlerEntered; });
        }
    });
    ParticipantRuntime runtime(ParticipantRuntimeOptions{"Starter", registry.uri});
    runtime.add_communication_ready_handler([] { throw std::runtime_error("the controller did not start"); });
    // Runs after the first one's failure has been reported.
    runtime.add_communication_ready_handler([&meeting] {
        {
            const std::lock_guard lock(meeting.mutex);
            meeting.handlerEntered = true;
        }
        meeting.changed.notify_all();
    });

    try
    {
        runtime.start(unit::scaled_timeout(30s));
    }
    catch (const std::exception&)
    {
        // start() may say that the participant failed; what counts is the state.
    }
    const auto state = runtime.wait_until_finished(unit::scaled_timeout(10s));
    runtime.stop("test");

    CHECK(!meeting.waitRanOut);
    CHECK(state == std::optional(orch::ParticipantState::Error));
}

namespace {

/// A coordinated participant with virtual time whose run is short: the simulation is stopped in
/// its first step. It reports a failure in the way `way` names, and the probe holds its start()
/// behind StartLifecycle() until others have seen it in Error or in Shutdown. Throws unless they
/// saw the Error with the failure's reason.
void expect_the_error_of_a_short_run_whose_start_is_held(const std::string& way)
{
    InProcessRegistry registry;
    unit::StateLog seen(registry.uri, "Body");
    bool heldUntilTheEnd = false;
    // Only the body is started here, so the probe is its start().
    const ProbeInPlace probe([&](StartProbePoint point) {
        if (point == StartProbePoint::AfterStartLifecycle)
        {
            heldUntilTheEnd = unit::eventually(
                [&] { return seen.reached(orch::ParticipantState::Error) || seen.reached(orch::ParticipantState::Shutdown); }, 20s);
        }
    });
    ParticipantRuntimeOptions options{"Body", registry.uri};
    options.lifecycle_mode = LifecycleMode::Coordinated;
    options.virtual_time = VirtualTimeOptions{1ms, std::nullopt};
    ParticipantRuntime body(options);
    const auto fail = [&body] { body.report_failure("the controller did not start"); };
    if (way == "in an added CommunicationReady handler")
    {
        body.add_communication_ready_handler(fail);
    }
    else if (way == "in the CommunicationReady handler that was set")
    {
        body.set_communication_ready_handler(fail);
    }
    else if (way == "in the asynchronous CommunicationReady handler")
    {
        body.set_communication_ready_handler_async([&body, fail] {
            fail();
            body.complete_communication_ready();
        });
    }
    body.set_step_handler([&body, fail, inStep = way == "in the first step"](SimTime, SimDuration) {
        if (inStep)
        {
            fail();
        }
        body.request_stop("test: the run is over");
    });
    ParticipantRuntime boss(ParticipantRuntimeOptions{"Boss", registry.uri});
    auto* const controller = SilKit::Experimental::Participant::CreateSystemController(&boss.participant());
    controller->SetWorkflowConfiguration(orch::WorkflowConfiguration{{"Body"}});

    std::string startError;
    try
    {
        body.start(unit::scaled_timeout(30s));
    }
    catch (const std::exception& e)
    {
        startError = e.what();
    }
    const bool sawError = unit::eventually([&] { return seen.reached(orch::ParticipantState::Error); }, 2s);
    const std::string reason = seen.reason_of(orch::ParticipantState::Error);
    body.stop("test");

    if (!heldUntilTheEnd || !sawError || reason.find("the controller did not start") == std::string::npos)
    {
        throw vehicle_sil::unit::Failure(
            "the failure reported " + way + " was lost: others " +
            (sawError ? "saw the participant in Error for the reason \"" + reason + "\"" : "never saw the participant in Error") +
            " (the states they saw: " + seen.text() + "; start() " +
            (startError.empty() ? "returned" : "threw \"" + startError + "\"") +
            (heldUntilTheEnd ? ")" : "; they saw neither Error nor Shutdown while start() was held)"));
    }
}

} // namespace

TEST_CASE(participant_start_does_not_lose_a_handler_failure_when_the_run_ends_before_start_is_back_from_the_lifecycle)
{
    // The same place as above, for a run that can end while start() stands there. A failure that
    // is only kept for start() to report would come too late: the handler returns, the participant
    // goes on to Running, the step stops the run, and SIL Kit drops an error reported after
    // Shutdown. The failure must reach SIL Kit while the handler runs, whichever way the handler
    // was given.
    for (const std::string way : {"in an added CommunicationReady handler", "in the CommunicationReady handler that was set",
                                  "in the asynchronous CommunicationReady handler"})
    {
        expect_the_error_of_a_short_run_whose_start_is_held(way);
    }
}

TEST_CASE(participant_start_does_not_lose_a_step_failure_of_a_participant_without_handlers_when_the_run_ends_first)
{
    // As above, for a participant that was given no CommunicationReady handler: its first step
    // reports the failure and returns. SIL Kit takes errors from the moment it would have called
    // such a handler, so the failure is not kept until start() comes back.
    expect_the_error_of_a_short_run_whose_start_is_held("in the first step");
}

TEST_CASE(participant_start_reports_a_failure_that_came_before_it_noted_that_the_lifecycle_takes_errors)
{
    // The other way into the same place: a failure reported before SIL Kit has called the
    // CommunicationReady handler and before start() has noted that SIL Kit takes an error. It is
    // kept, and start() reports it. The participant is coordinated and nobody says which
    // participants are required, so SIL Kit does not get to the handler.
    InProcessRegistry registry;
    unit::StateLog seen(registry.uri, "Starter");
    ParticipantRuntime* started = nullptr;
    const ProbeInPlace probe([&started](StartProbePoint point) {
        if (point == StartProbePoint::AfterStartLifecycle)
        {
            started->report_failure("reported before start() was back");
        }
    });
    ParticipantRuntimeOptions options{"Starter", registry.uri};
    options.lifecycle_mode = LifecycleMode::Coordinated;
    ParticipantRuntime runtime(options);
    started = &runtime;

    try
    {
        runtime.start(unit::scaled_timeout(30s));
    }
    catch (const std::exception&) // NOLINT(bugprone-empty-catch)
    {
        // start() says that the participant failed; what counts is what others see.
    }
    const bool sawError = unit::eventually([&] { return seen.reached(orch::ParticipantState::Error); }, 10s);
    const std::string reason = seen.reason_of(orch::ParticipantState::Error);
    runtime.stop("test");

    CHECK(sawError);
    CHECK(reason.find("reported before start() was back") != std::string::npos);
    CHECK(!seen.reached(orch::ParticipantState::CommunicationInitialized));
}

TEST_CASE(participant_start_takes_a_run_whose_first_step_failed_between_its_two_looks_for_a_start)
{
    // Only an Error before the participant began to run is a start that failed. Here start() has
    // read "no step has run" when the first step runs and fails: the participant is in Error, and
    // its status has been handled, when start() reads the state. That is a run that began and
    // failed, not a start that failed: start() returns, and the failure is the run's.
    InProcessRegistry registry;
    Meeting meeting;
    ParticipantRuntime* started = nullptr;
    std::atomic<int> endSeen{0};
    bool sawError = false;
    const ProbeInPlace probe([&](StartProbePoint point) {
        if (point != StartProbePoint::BetweenFlagsAndState)
        {
            return;
        }
        {
            const std::lock_guard lock(meeting.mutex);
            if (meeting.flagsRead)
            {
                return;
            }
            meeting.flagsRead = true;
        }
        meeting.changed.notify_all();
        // The stop observer is called when the runtime has handled the status of its own end.
        sawError = unit::eventually(
            [&] { return endSeen.load() > 0 && started->state() == orch::ParticipantState::Error; }, 20s);
    });
    ParticipantRuntimeOptions options{"Starter", registry.uri};
    options.virtual_time = VirtualTimeOptions{1ms, std::nullopt};
    ParticipantRuntime runtime(options);
    started = &runtime;
    runtime.set_step_handler([](SimTime, SimDuration) { throw std::runtime_error("the first step fails"); });
    runtime.set_stop_observer([&endSeen] { ++endSeen; });
    hold_the_lifecycle_until_the_flags_are_read(runtime, meeting);

    std::string thrown;
    try
    {
        runtime.start(unit::scaled_timeout(60s));
    }
    catch (const std::exception& e)
    {
        thrown = e.what();
    }
    runtime.stop("test");

    CHECK(sawError);
    CHECK(!meeting.waitRanOut);
    if (!thrown.empty())
    {
        throw vehicle_sil::unit::Failure("start() threw for a participant whose first step had run (and failed): " + thrown);
    }
}

TEST_CASE(participant_start_fails_for_a_participant_that_is_not_required_and_it_stays_in_error)
{
    // A coordinated participant that the system controller does not require: SIL Kit puts it into
    // Error when the required names arrive. Here they arrive when the stranger knows that the one
    // required participant is in ServicesCreated, so the same message also begins the stranger's
    // start (the wait for the other participants' replies) before the Error is set. The replies
    // come after the Error. An unpatched SIL Kit then sets the next state over the Error and the
    // stranger goes on with the simulation, to Running (patches/sil-kit/0006). The stranger must
    // stay in Error: start() fails, no step runs, a stop takes it to Shutdown at once, and the
    // simulation of the others is not touched when it leaves.
    InProcessRegistry registry;
    ParticipantRuntimeOptions memberOptions{"Member", registry.uri};
    memberOptions.lifecycle_mode = LifecycleMode::Coordinated;
    ParticipantRuntime member(memberOptions);
    ParticipantRuntimeOptions strangerOptions{"Stranger", registry.uri};
    strangerOptions.lifecycle_mode = LifecycleMode::Coordinated;
    strangerOptions.virtual_time = VirtualTimeOptions{};
    ParticipantRuntime stranger(strangerOptions);
    std::atomic<int> steps{0};
    stranger.set_step_handler([&steps](SimTime, SimDuration) { ++steps; });
    unit::StateLog seen(registry.uri, "Stranger");
    ParticipantRuntime boss(ParticipantRuntimeOptions{"Boss", registry.uri});
    auto* const controller = SilKit::Experimental::Participant::CreateSystemController(&boss.participant());

    std::string memberError;
    std::thread memberStart([&] {
        try
        {
            member.start(unit::scaled_timeout(20s));
        }
        catch (const std::exception& e)
        {
            memberError = e.what();
        }
    });
    std::string strangerError;
    bool strangerStarted = false;
    std::thread strangerStart([&] {
        try
        {
            stranger.start(unit::scaled_timeout(20s));
            strangerStarted = true;
        }
        catch (const std::exception& e)
        {
            strangerError = e.what();
        }
    });
    const auto created = [](const PeerStatus& status) { return status.state == orch::ParticipantState::ServicesCreated; };
    // The stranger itself has to know the member's state: what the controller knows came over
    // another connection, and says nothing of the order in which the stranger learns it.
    const bool bothWait = boss.wait_for_peer("Member", created, unit::scaled_timeout(20s))
                          && boss.wait_for_peer("Stranger", created, unit::scaled_timeout(20s))
                          && stranger.wait_for_peer("Member", created, unit::scaled_timeout(20s));
    controller->SetWorkflowConfiguration(orch::WorkflowConfiguration{{"Member"}});
    strangerStart.join();
    memberStart.join();
    const auto strangerState = stranger.state();
    stranger.stop("test");
    // In Error, a stop is the way to Shutdown; a stranger that had gone on would be stopped only
    // in Running, or not at all.
    const auto strangerEnd = stranger.state();
    const bool endSeen =
        unit::eventually([&] { return seen.reached(orch::ParticipantState::Shutdown); }, 10s);
    // The stranger has left; the member still runs.
    const auto memberState = member.state();
    member.stop("test");

    std::string states;
    for (const auto state : seen.states())
    {
        states += " " + orch::to_string(state);
    }
    CHECK(bothWait);
    CHECK(seen.reached(orch::ParticipantState::Error));
    if (strangerStarted || strangerError.find("failed to start, state: Error") == std::string::npos)
    {
        throw vehicle_sil::unit::Failure(
            std::string("start() of a participant that SIL Kit had put into Error ") +
            (strangerStarted ? "returned" : "threw \"" + strangerError + "\"") + " in state " + orch::to_string(strangerState) +
            ", expected it to throw \"... failed to start, state: Error ...\" (the states others saw:" + states + ")");
    }
    for (const auto state : {orch::ParticipantState::CommunicationInitializing, orch::ParticipantState::CommunicationInitialized,
                             orch::ParticipantState::ReadyToRun, orch::ParticipantState::Running})
    {
        if (seen.reached(state))
        {
            throw vehicle_sil::unit::Failure(
                "SIL Kit let a participant that it had put into Error go on (is patches/sil-kit/0006 applied?); the states "
                "others saw:" + states);
        }
    }
    CHECK(strangerError.find("state: Error (") == std::string::npos); // no other state when start() looked
    CHECK(strangerEnd == orch::ParticipantState::Shutdown);
    CHECK(endSeen);
    CHECK_EQ(steps.load(), 0);
    CHECK(memberError.empty());
    CHECK(memberState == orch::ParticipantState::Running);
}

TEST_CASE(participant_stop_after_a_failed_start_returns_once_the_lifecycle_is_in_shutdown)
{
    // The same stranger as above, with one more participant that replies late: its SIL Kit thread
    // is held from before the required names are sent until the stranger's stop() has returned.
    // So the stranger's start still waits for the replies when its Error comes and when its stop
    // goes out. SIL Kit takes one wait for replies at a time and drops the one of the shutdown:
    // the lifecycle gets to Shutdown, but the final state is never set. stop() must end at
    // Shutdown and not wait for shutdown_timeout.
    InProcessRegistry registry;
    std::mutex holdMutex;
    std::condition_variable holdChanged;
    bool held = false;
    bool released = false;
    auto laggard = SilKit::CreateParticipant(
        SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(registry.uri)),
        "Laggard", registry.uri);
    laggard->CreateSystemMonitor()->AddParticipantStatusHandler([&](const orch::ParticipantStatus& status) {
        if (status.participantName != "Stranger" || status.state != orch::ParticipantState::ServicesCreated)
        {
            return;
        }
        std::unique_lock lock(holdMutex);
        held = true;
        holdChanged.notify_all();
        holdChanged.wait_for(lock, unit::scaled_timeout(60s), [&] { return released; });
    });
    const auto release = [&] {
        const std::lock_guard lock(holdMutex);
        released = true;
        holdChanged.notify_all();
    };

    ParticipantRuntimeOptions memberOptions{"Member", registry.uri};
    memberOptions.lifecycle_mode = LifecycleMode::Coordinated;
    ParticipantRuntime member(memberOptions);
    ParticipantRuntimeOptions strangerOptions{"Stranger", registry.uri};
    strangerOptions.lifecycle_mode = LifecycleMode::Coordinated;
    strangerOptions.virtual_time = VirtualTimeOptions{};
    const auto shutdownTimeout = unit::scaled_timeout(15s);
    strangerOptions.shutdown_timeout = shutdownTimeout;
    ParticipantRuntime stranger(strangerOptions);
    stranger.set_step_handler([](SimTime, SimDuration) {});
    ParticipantRuntime boss(ParticipantRuntimeOptions{"Boss", registry.uri});
    auto* const controller = SilKit::Experimental::Participant::CreateSystemController(&boss.participant());

    std::thread memberStart([&] {
        try
        {
            member.start(unit::scaled_timeout(60s));
        }
        catch (const std::exception&)
        {
        }
    });
    std::string strangerError;
    std::thread strangerStart([&] {
        try
        {
            stranger.start(unit::scaled_timeout(20s));
        }
        catch (const std::exception& e)
        {
            strangerError = e.what();
        }
    });
    const auto created = [](const PeerStatus& status) { return status.state == orch::ParticipantState::ServicesCreated; };
    const bool ready = stranger.wait_for_peer("Member", created, unit::scaled_timeout(20s))
                       && unit::eventually(
                           [&] {
                               const std::lock_guard lock(holdMutex);
                               return held;
                           },
                           unit::scaled_timeout(20s));
    controller->SetWorkflowConfiguration(orch::WorkflowConfiguration{{"Member"}});
    strangerStart.join();
    const auto before = std::chrono::steady_clock::now();
    stranger.stop("test");
    const auto took = std::chrono::steady_clock::now() - before;
    const auto strangerEnd = stranger.state();
    const auto finalState = stranger.wait_until_finished(0ms);
    release();
    memberStart.join();
    member.stop("test");

    CHECK(ready);
    CHECK(strangerError.find("failed to start, state: Error") != std::string::npos);
    CHECK(strangerEnd == orch::ParticipantState::Shutdown);
    CHECK(finalState == orch::ParticipantState::Shutdown);
    const auto bound = unit::scaled_timeout(3s);
    if (took >= bound)
    {
        throw vehicle_sil::unit::Failure(
            "stop() after a start that SIL Kit failed took "
            + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms (shutdown_timeout "
            + std::to_string(shutdownTimeout.count()) + " ms), expected under " + std::to_string(bound.count())
            + " ms: it has to end once the lifecycle is in Shutdown, as SIL Kit may never set the final state then");
    }
}

TEST_CASE(participant_start_takes_a_virtual_time_run_aborted_before_its_first_step_for_a_start)
{
    // A virtual-time participant can reach Running and be aborted by the system controller before
    // its first step executes and before start() polls again (for example when the launcher aborts
    // immediately after the start window). Without virtual time, StartingHandler marks the start;
    // with virtual time, SIL Kit does not call StartingHandler, so start() must recognise that the
    // lifecycle already entered Running and return rather than throwing "failed to start".
    InProcessRegistry registry;

    // A second virtual-time participant held in CommunicationInitialized keeps Engine's step 0
    // from running once Engine reaches Running, so step_entered_ is guaranteed to stay false.
    auto blocker = SilKit::CreateParticipant(
        SilKit::Config::ParticipantConfigurationFromString(vehicle_sil::default_participant_configuration(registry.uri)),
        "Blocker", registry.uri);
    auto* const blockerLifecycle = blocker->CreateLifecycleService({orch::OperationMode::Autonomous});
    std::atomic<bool> blockerReady{false};
    blockerLifecycle->SetCommunicationReadyHandlerAsync([&blockerReady] { blockerReady.store(true); });
    auto* const blockerTime = blockerLifecycle->CreateTimeSyncService();
    blockerTime->SetSimulationStepHandler([](std::chrono::nanoseconds, std::chrono::nanoseconds) {}, 10ms);
    auto blockerDone = blockerLifecycle->StartLifecycle();
    CHECK(unit::eventually([&] { return blockerReady.load(); }, 20s));

    ParticipantRuntime boss(ParticipantRuntimeOptions{"Boss", registry.uri});
    auto* const controller = SilKit::Experimental::Participant::CreateSystemController(&boss.participant());

    Meeting meeting;
    ParticipantRuntime* started = nullptr;
    bool sawRunning = false;
    bool sawShutdown = false;
    const ProbeInPlace probe([&](StartProbePoint point) {
        if (point != StartProbePoint::BetweenFlagsAndState)
        {
            return;
        }
        {
            const std::lock_guard lock(meeting.mutex);
            if (meeting.flagsRead)
            {
                return;
            }
            meeting.flagsRead = true;
        }
        meeting.changed.notify_all();
        sawRunning = unit::eventually([&] { return started->state() == orch::ParticipantState::Running; }, 20s);
        controller->AbortSimulation();
        sawShutdown = unit::eventually([&] { return started->state() == orch::ParticipantState::Shutdown; }, 20s);
    });

    ParticipantRuntimeOptions options{"Engine", registry.uri};
    options.virtual_time = VirtualTimeOptions{10ms, std::nullopt};
    ParticipantRuntime engine(options);
    started = &engine;
    std::atomic<int> steps{0};
    engine.set_step_handler([&steps](SimTime, SimDuration) { ++steps; });
    hold_the_lifecycle_until_the_flags_are_read(engine, meeting);

    std::string thrown;
    try
    {
        engine.start(unit::scaled_timeout(60s));
    }
    catch (const std::exception& e)
    {
        thrown = e.what();
    }
    const auto finalState = engine.wait_until_finished(unit::scaled_timeout(10s));
    engine.stop("test");
    if (blockerDone.wait_for(unit::scaled_timeout(5s)) != std::future_status::ready)
    {
        blockerLifecycle->CompleteCommunicationReadyHandlerAsync();
    }
    blocker.reset();

    CHECK(sawRunning);
    CHECK(sawShutdown);
    CHECK(!meeting.waitRanOut);
    CHECK_EQ(steps.load(), 0);
    if (!thrown.empty())
    {
        throw vehicle_sil::unit::Failure("start() threw for a virtual-time participant aborted after Running: " + thrown);
    }
    CHECK(finalState == std::optional(orch::ParticipantState::Shutdown));
}

TEST_CASE(participant_stop_after_a_start_timeout_returns_without_waiting_for_shutdown_timeout)
{
    // A coordinated participant whose start() timed out before Running (nobody started the
    // simulation): SIL Kit ignores Stop() before Running, so stop() must not stall for the full
    // shutdown_timeout (5 s by default, 15 s here) before returning.
    InProcessRegistry registry;
    const auto shutdownTimeout = unit::scaled_timeout(15s);
    {
        ParticipantRuntimeOptions options{"TimedOut", registry.uri};
        options.lifecycle_mode = LifecycleMode::Coordinated;
        options.virtual_time = VirtualTimeOptions{10ms, std::nullopt};
        options.shutdown_timeout = shutdownTimeout;
        ParticipantRuntime runtime(options);
        runtime.set_step_handler([](SimTime, SimDuration) {});

        bool timedOut = false;
        try
        {
            runtime.start(50ms);
        }
        catch (const StartTimeoutError&)
        {
            timedOut = true;
        }
        CHECK(timedOut);

        const auto before = std::chrono::steady_clock::now();
        runtime.stop("start timed out");
        const auto took = std::chrono::steady_clock::now() - before;
        CHECK(runtime.stop_requested());
        const auto bound = unit::scaled_timeout(1s);
        if (took >= bound)
        {
            throw vehicle_sil::unit::Failure(
                "stop() after a start timeout took "
                + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count())
                + " ms (shutdown_timeout " + std::to_string(shutdownTimeout.count()) + " ms), expected under "
                + std::to_string(bound.count()) + " ms");
        }
    }
    {
        // With an asynchronous CommunicationReady handler (a controller's start window) and no
        // peers starting the simulation, stop() also bounds its pre-Running wait instead of
        // waiting out shutdown_timeout.
        ParticipantRuntimeOptions options{"TimedOutAsync", registry.uri};
        options.lifecycle_mode = LifecycleMode::Coordinated;
        options.virtual_time = VirtualTimeOptions{10ms, std::nullopt};
        options.shutdown_timeout = shutdownTimeout;
        ParticipantRuntime runtime(options);
        runtime.set_step_handler([](SimTime, SimDuration) {});
        runtime.set_communication_ready_handler_async([] {});

        bool timedOut = false;
        try
        {
            runtime.start(50ms);
        }
        catch (const StartTimeoutError&)
        {
            timedOut = true;
        }
        CHECK(timedOut);

        const auto before = std::chrono::steady_clock::now();
        runtime.stop("start timed out");
        const auto took = std::chrono::steady_clock::now() - before;
        CHECK(runtime.stop_requested());
        const auto bound = unit::scaled_timeout(2s);
        if (took >= bound)
        {
            throw vehicle_sil::unit::Failure(
                "stop() with async CommunicationReady after a start timeout took "
                + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count())
                + " ms (shutdown_timeout " + std::to_string(shutdownTimeout.count()) + " ms), expected under "
                + std::to_string(bound.count()) + " ms");
        }
    }
}
