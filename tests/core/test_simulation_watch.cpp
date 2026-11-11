// SPDX-License-Identifier: MIT
//
// watch_simulation(): the loop that every tool with a controller shares, with a made-up
// controller and clock (no SIL Kit participant).
#include "test_harness.hpp"

#include "vehicle_sil/core/simulation_watch.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using State = SilKit::Services::Orchestration::ParticipantState;

/// A controller that ends after `polls_until_end` waits (never if empty), with a participant that
/// leaves at the wait `left_at` (never if empty), and a clock that advances 1 s per wait.
struct FakeController
{
    std::optional<int> polls_until_end{};
    std::optional<int> left_at{};
    bool left_in_error = true;
    int polls = 0;
    std::vector<std::string> calls{};
    std::chrono::steady_clock::time_point now{};

    WatchTarget target()
    {
        WatchTarget t;
        t.wait_until_finished = [this](std::chrono::milliseconds) -> std::optional<State> {
            ++polls;
            now += 1s;
            if (!calls.empty() && (calls.back() == "stop" || calls.back() == "abort"))
            {
                return State::Shutdown;
            }
            if (polls_until_end && polls >= *polls_until_end)
            {
                return State::Shutdown;
            }
            return std::nullopt;
        };
        t.left_participant = [this]() -> std::optional<LeftParticipant> {
            if (left_at && polls >= *left_at)
            {
                return LeftParticipant{"Engine", left_in_error ? "Error" : "Shutdown", left_in_error};
            }
            return std::nullopt;
        };
        t.stop = [this](std::string_view) { calls.emplace_back("stop"); };
        t.abort = [this] { calls.emplace_back("abort"); };
        return t;
    }
    WatchOptions options()
    {
        WatchOptions o;
        o.clock = [this] { return now; };
        o.leave_grace = 5s;
        return o;
    }
};

} // namespace

TEST_CASE(watch_returns_when_the_simulation_ends)
{
    FakeController c;
    c.polls_until_end = 3;
    const WatchResult r = watch_simulation(c.target(), c.options());
    CHECK(r.end == WatchEnd::Ended);
    CHECK(r.state == State::Shutdown);
    CHECK(c.calls.empty());
    CHECK_EQ(c.polls, 3);
}

TEST_CASE(watch_stops_an_interrupted_simulation_in_order)
{
    FakeController c;
    WatchOptions o = c.options();
    o.interrupted = [&c] { return c.polls >= 2; };
    o.failed = [&c] { return c.polls >= 2; }; // the interrupt is looked at first
    const WatchResult r = watch_simulation(c.target(), o);
    CHECK(r.end == WatchEnd::Interrupted);
    CHECK(c.calls == std::vector<std::string>{"stop"});
    CHECK(r.state == State::Shutdown); // the wait after the stop
}

TEST_CASE(watch_aborts_when_the_caller_sees_a_failure)
{
    FakeController c;
    c.left_at = 2; // the caller's failure is looked at before the failure check
    WatchOptions o = c.options();
    o.failed = [&c] { return c.polls >= 2; };
    const WatchResult r = watch_simulation(c.target(), o);
    CHECK(r.end == WatchEnd::Failed);
    CHECK(!r.failure.has_value());
    CHECK(c.calls == std::vector<std::string>{"abort"});
    CHECK_EQ(c.polls, 2);
}

TEST_CASE(watch_aborts_at_once_for_a_participant_in_error)
{
    FakeController c;
    c.left_at = 2;
    const WatchResult r = watch_simulation(c.target(), c.options());
    CHECK(r.end == WatchEnd::Failed);
    CHECK(r.failure.has_value());
    CHECK(r.failure->participant == "Engine");
    CHECK(r.failure->reason.find("left the simulation (state Error)") == 0);
    CHECK(c.calls == std::vector<std::string>{"abort"});
    CHECK_EQ(c.polls, 2);
}

TEST_CASE(watch_gives_a_participant_that_stopped_the_grace)
{
    FakeController c;
    c.left_at = 2;
    c.left_in_error = false;
    const WatchResult r = watch_simulation(c.target(), c.options());
    CHECK(r.end == WatchEnd::Failed);
    CHECK(r.failure.has_value());
    CHECK(r.failure->state == "Shutdown");
    // Seen gone at the 2nd wait (t = 2 s); failed once 5 s have passed (t = 7 s).
    CHECK_EQ(c.polls, 7);
    // It ends the simulation within the grace: no failure.
    FakeController ok;
    ok.left_at = 2;
    ok.left_in_error = false;
    ok.polls_until_end = 6;
    const WatchResult ended = watch_simulation(ok.target(), ok.options());
    CHECK(ended.end == WatchEnd::Ended);
    CHECK(ok.calls.empty());
}

TEST_CASE(watch_aborts_at_the_deadline)
{
    FakeController c;
    WatchOptions o = c.options();
    o.deadline = c.now + 4s;
    const WatchResult r = watch_simulation(c.target(), o);
    CHECK(r.end == WatchEnd::TimedOut);
    CHECK(c.calls == std::vector<std::string>{"abort"});
    CHECK_EQ(c.polls, 4);
}
