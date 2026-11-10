// SPDX-License-Identifier: MIT
//
// FailureCheck: when a participant that left a running simulation has failed it. Pure logic, with
// time points made up by the test.
#include "test_harness.hpp"

#include "vehicle_sil/core/failure_check.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;

const std::chrono::steady_clock::time_point t0{};

LeftParticipant left(const std::string& name, const std::string& state)
{
    return LeftParticipant{name, state, state == "Error"};
}

} // namespace

TEST_CASE(failure_check_fails_a_participant_in_error_at_once)
{
    FailureCheck check(5s);
    const auto failure = check.check(left("Engine", "Error"), t0);
    CHECK(failure.has_value());
    CHECK(failure->participant == "Engine");
    CHECK(failure->state == "Error");
    CHECK(failure->reason.find("left the simulation (state Error)") == 0);
    CHECK(failure->reason.find("Engine") == std::string::npos); // the caller puts the name in front
}

TEST_CASE(failure_check_fails_a_participant_that_stopped_only_after_the_grace)
{
    FailureCheck check(5s);
    CHECK(!check.check(std::nullopt, t0).has_value());
    CHECK(!check.check(left("Body", "Stopped"), t0 + 1s).has_value());
    CHECK(!check.check(left("Body", "Stopped"), t0 + 5999ms).has_value());
    const auto failure = check.check(left("Body", "Stopped"), t0 + 6s);
    CHECK(failure.has_value());
    CHECK(failure->participant == "Body");
    CHECK(failure->reason.find("left the simulation (state Stopped)") == 0);
    CHECK(failure->reason.find("within 5 s") != std::string::npos);
}

TEST_CASE(failure_check_starts_the_grace_anew_when_no_participant_is_gone)
{
    FailureCheck check(5s);
    CHECK(!check.check(left("Body", "Stopping"), t0).has_value());
    CHECK(!check.check(left("Body", "Stopping"), t0 + 4s).has_value());
    CHECK(!check.check(std::nullopt, t0 + 4500ms).has_value()); // back within the grace
    CHECK(!check.check(left("Body", "Stopping"), t0 + 6s).has_value());
    CHECK(!check.check(left("Body", "Stopping"), t0 + 10s).has_value());
    CHECK(check.check(left("Body", "Stopping"), t0 + 11s).has_value());
}

TEST_CASE(failure_check_states_the_grace_and_refuses_a_negative_one)
{
    CHECK(FailureCheck().grace() == FailureCheck::kDefaultGrace);
    FailureCheck check(250ms);
    CHECK(!check.check(left("Body", "Shutdown"), t0).has_value());
    const auto failure = check.check(left("Body", "Shutdown"), t0 + 250ms);
    CHECK(failure.has_value());
    CHECK(failure->reason.find("within 250 ms") != std::string::npos);
    bool refused = false;
    try
    {
        FailureCheck negative(-1ms);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    CHECK(refused);
}
