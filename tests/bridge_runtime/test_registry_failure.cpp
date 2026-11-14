// SPDX-License-Identifier: MIT
//
// The message of a bridge whose SIL Kit registry does not answer.
#include "test_harness.hpp"

#include "vehicle_sil/bridge_runtime/registry_failure.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"

#include <exception>
#include <optional>
#include <string>

using vehicle_sil::bridge_runtime::registry_failure_message;

TEST_CASE(registry_failure_says_where_the_bridge_looked_and_what_to_do)
{
    const auto message =
        registry_failure_message("SIL Kit: An error (1):\nERROR: Failed to connect to SIL Kit Registry\r\n", "silkit://127.0.0.1:8500");
    CHECK(message.has_value());
    CHECK(message.value_or("") ==
          "cannot connect to the SIL Kit registry at silkit://127.0.0.1:8500 (SIL Kit: An error (1): ERROR: Failed "
          "to connect to SIL Kit Registry  ). Start the registry first ('sil-kit-registry --listen-uri "
          "silkit://127.0.0.1:8500'), or give the address it listens at with --registry-uri.");
}

TEST_CASE(registry_failure_leaves_other_failures_as_they_are)
{
    CHECK(!registry_failure_message("the participant name must not be empty", "silkit://127.0.0.1:8500"));
    CHECK(!registry_failure_message("", "silkit://127.0.0.1:8500"));
}

TEST_CASE(registry_failure_knows_what_silkit_throws_when_no_registry_answers)
{
    // Port 1: nothing listens there. If SIL Kit changes its text, the bridges would fall back to
    // SIL Kit's bare message; this test notices.
    const std::string uri = "silkit://127.0.0.1:1";
    std::optional<std::string> message;
    std::string what;
    try
    {
        vehicle_sil::ParticipantRuntime runtime(vehicle_sil::ParticipantRuntimeOptions{"RegistryFailureTest", uri});
    }
    catch (const std::exception& e)
    {
        what = e.what();
        message = registry_failure_message(what, uri);
    }
    CHECK(!what.empty());
    CHECK(message.has_value());
    const std::string text = message.value_or("");
    CHECK(text.rfind("cannot connect to the SIL Kit registry at " + uri + " (", 0) == 0);
    CHECK(text.find('\n') == std::string::npos);
    CHECK(text.find("Start the registry first ('sil-kit-registry --listen-uri " + uri + "')") != std::string::npos);
}
