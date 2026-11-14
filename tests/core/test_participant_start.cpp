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

#include <silkit/services/orchestration/string_utils.hpp>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
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
