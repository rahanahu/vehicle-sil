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

TEST_CASE(registry_failure_at_an_address_that_is_not_the_loopback_says_what_to_check_where_the_registry_runs)
{
    const std::string what = "ERROR: Failed to connect to SIL Kit Registry";
    const auto text = [&](const std::string& uri) { return registry_failure_message(what, uri).value_or(""); };
    const auto elsewhere = [&](const std::string& uri, const std::string& host, const std::string& port) {
        return "cannot connect to the SIL Kit registry at " + uri + " (" + what + "). " + host +
               " is not the loopback address of this computer: check that the registry runs on the computer with that "
               "address (which may be this one), that it listens on an address reached from here ('sil-kit-registry "
               "--listen-uri silkit://0.0.0.0:" + port + "' there listens on all of its addresses, not only on its "
               "loopback), and that its firewall lets port " + port + " through.";
    };
    CHECK(text("silkit://192.168.10.5:8500") == elsewhere("silkit://192.168.10.5:8500", "192.168.10.5", "8500"));
    CHECK(text("silkit://sim-pc:8501") == elsewhere("silkit://sim-pc:8501", "sim-pc", "8501"));
    CHECK(text("silkit://[fe80::1]:8500") == elsewhere("silkit://[fe80::1]:8500", "fe80::1", "8500"));
    // The host is named in lower case; the address is repeated as it was given.
    CHECK(text("silkit://Sim-PC:8501") == elsewhere("silkit://Sim-PC:8501", "sim-pc", "8501"));
    // An address without a port: the example has the port of a registry started without one.
    CHECK(text("silkit://sim-pc") == elsewhere("silkit://sim-pc", "sim-pc", "8500"));
    // What only looks like a loopback address.
    CHECK(text("silkit://1270.0.0.1:8500") == elsewhere("silkit://1270.0.0.1:8500", "1270.0.0.1", "8500"));
    CHECK(text("silkit://127.example.com:8500") == elsewhere("silkit://127.example.com:8500", "127.example.com", "8500"));
    // The message does not say whose address it is: it may be this computer's own.
    CHECK(text("silkit://192.168.10.5:8500").find("another computer") == std::string::npos);
}

TEST_CASE(registry_failure_at_the_loopback_says_to_start_the_registry)
{
    const std::string what = "ERROR: Failed to connect to SIL Kit Registry";
    const auto text = [&](const std::string& uri) { return registry_failure_message(what, uri).value_or(""); };
    // This computer, however it is written, and an address that cannot be read.
    for (const std::string uri :
         {"silkit://localhost:8500", "silkit://LOCALHOST:8500", "silkit://Localhost", "silkit://127.0.0.1:8500",
          "silkit://127.1.2.3:8500", "silkit://[::1]:8500", "silkit://[::ffff:127.0.0.1]:8500",
          "silkit://[::FFFF:127.0.0.1]:8500", "silkit://[0:0:0:0:0:0:0:1]:8500", "::1", "silkit://", "", "silkit://[::1"})
    {
        CHECK(text(uri) == "cannot connect to the SIL Kit registry at " + uri + " (" + what +
                               "). Start the registry first ('sil-kit-registry --listen-uri " + uri +
                               "'), or give the address it listens at with --registry-uri.");
    }
}

TEST_CASE(registry_failure_at_an_address_to_listen_on_says_to_give_the_address_of_the_registry)
{
    const std::string what = "ERROR: Failed to connect to SIL Kit Registry";
    const auto text = [&](const std::string& uri) { return registry_failure_message(what, uri).value_or(""); };
    const auto any = [&](const std::string& uri, const std::string& host, const std::string& port) {
        return "cannot connect to the SIL Kit registry at " + uri + " (" + what + "). " + host +
               " stands for every address of a computer: a registry listens on it, a participant cannot connect to it. "
               "Give --registry-uri the address of the computer the registry runs on (silkit://127.0.0.1:" + port +
               " if it runs on this one).";
    };
    CHECK(text("silkit://0.0.0.0:8500") == any("silkit://0.0.0.0:8500", "0.0.0.0", "8500"));
    CHECK(text("silkit://[::]:8501") == any("silkit://[::]:8501", "::", "8501"));
    CHECK(text("silkit://::") == any("silkit://::", "::", "8500"));
    CHECK(text("silkit://0.0.0.0") == any("silkit://0.0.0.0", "0.0.0.0", "8500"));
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
