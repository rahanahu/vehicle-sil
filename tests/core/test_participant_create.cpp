// SPDX-License-Identifier: MIT
//
// The retries of create_participant() with a made-up SIL Kit call (detail::create_participant_retrying),
// and a check that SIL Kit's sources still contain the texts the retries go by.
#include "participant_create.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_if;
using vehicle_sil::unit::throws_with;

const std::string kTimeout = std::string(detail::kJoinTimeoutText) + ": Gone (unreachable) is waiting for reply";
const std::string kNameInUse = "A participant with the same name 'Engine' " + std::string(detail::kNameInUseText) + " default";

/// Plays SIL Kit's CreateParticipant(): each call throws the next message of `failures` as a
/// SilKit::ProtocolError; after the last one, it succeeds (with no participant, which the retries
/// only pass on).
struct FakeSilKit
{
    explicit FakeSilKit(std::vector<std::string> failures_first) : failures(std::move(failures_first)) {}

    std::vector<std::string> failures;
    std::size_t calls = 0;
    std::vector<std::string> notices;

    std::unique_ptr<SilKit::IParticipant> create(std::chrono::milliseconds name_in_use_wait = 0ms)
    {
        return detail::create_participant_retrying(
            [this]() -> std::unique_ptr<SilKit::IParticipant> {
                const auto i = calls++;
                if (i < failures.size())
                {
                    throw SilKit::ProtocolError(failures[i]);
                }
                return nullptr;
            },
            "Engine", name_in_use_wait, [this](std::string_view text) { notices.emplace_back(text); });
    }
};

bool contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE(create_participant_creates_once_more_after_a_join_timeout)
{
    FakeSilKit silkit({kTimeout});
    CHECK(silkit.create() == nullptr);
    CHECK_EQ(silkit.calls, 2u);
    CHECK_EQ(silkit.notices.size(), 1u);
    CHECK(contains(silkit.notices.at(0), "\"Engine\" timed out joining the simulation and is created once more"));
    CHECK(contains(silkit.notices.at(0), kTimeout));
}

TEST_CASE(create_participant_gives_up_after_a_second_join_timeout)
{
    FakeSilKit silkit({kTimeout, kTimeout, kTimeout});
    CHECK(throws_if<SilKit::ProtocolError>([&] { static_cast<void>(silkit.create()); },
                                           [](const SilKit::ProtocolError& e) {
                                               const std::string what = e.what();
                                               return contains(what, kTimeout) &&
                                                      contains(what, "timed out a second time after it was created once more");
                                           }));
    CHECK_EQ(silkit.calls, 2u);
    CHECK_EQ(silkit.notices.size(), 1u);
}

TEST_CASE(create_participant_does_not_retry_other_failures)
{
    FakeSilKit silkit({"Timeout while connecting to the registry"});
    CHECK(throws_with<SilKit::ProtocolError>([&] { static_cast<void>(silkit.create(5s)); },
                                             "Timeout while connecting to the registry"));
    CHECK_EQ(silkit.calls, 1u);
    CHECK(silkit.notices.empty());
}

TEST_CASE(create_participant_join_retry_does_not_extend_the_name_in_use_wait)
{
    // The join timeout's retry comes after the wait for the name has run out: a refusal of the
    // name then ends the attempts at once.
    FakeSilKit silkit({kTimeout, kNameInUse});
    CHECK(throws<NameInUseError>([&] { static_cast<void>(silkit.create(0ms)); }));
    CHECK_EQ(silkit.calls, 2u);
}

TEST_CASE(create_participant_retries_a_join_timeout_once_in_all)
{
    // Waiting for the name between two join timeouts does not allow a second join retry.
    FakeSilKit silkit({kTimeout, kNameInUse, kTimeout});
    CHECK(throws_with<SilKit::ProtocolError>([&] { static_cast<void>(silkit.create(5s)); }, "a second time"));
    CHECK_EQ(silkit.calls, 3u);
    CHECK_EQ(silkit.notices.size(), 1u);
}

TEST_CASE(silkit_still_writes_the_texts_create_participant_matches)
{
    // SIL Kit has no error kinds; create_participant() goes by these texts. If this fails after an
    // update of SIL Kit, find the new texts and change them in participant_create.hpp.
    const std::string silkit = VEHICLE_SIL_SILKIT_SOURCE_DIR;
    if (!std::ifstream(silkit + "/CMakeLists.txt"))
    {
        throw unit::Skipped("SIL Kit's sources are not on this computer (" + silkit + ")");
    }
    const auto source_contains = [&](const std::string& file, std::string_view text) {
        std::ifstream in(silkit + "/SilKit/source/core/vasio/" + file);
        const std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        return !content.empty() && contains(content, text);
    };
    CHECK(source_contains("VAsioConnection.cpp", detail::kJoinTimeoutText));
    CHECK(source_contains("VAsioRegistry.cpp", detail::kNameInUseText));
}
