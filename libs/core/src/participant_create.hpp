// SPDX-License-Identifier: MIT
//
// The retries of create_participant(), with the SIL Kit call and the notice passed in, so that a
// test can drive them without a registry. Internal to vehicle_sil::core and not installed.
#pragma once

#include <silkit/participant/IParticipant.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace vehicle_sil::detail {

// SIL Kit has no public way to tell its errors apart, so create_participant() goes by the text of
// the message. Every text it matches is here. When SIL Kit is updated, the test
// core.silkit_still_writes_the_texts_create_participant_matches checks that its sources still
// contain them.

/// The registry refused the name as in use (VAsioRegistry.cpp, SIL Kit v5.0.8: "A participant
/// with the same name '<name>' already exists in the simulation <name of the simulation>").
inline constexpr std::string_view kNameInUseText = "already exists in the simulation";

/// A participant on the list that the registry sent did not reply in time (VAsioConnection.cpp,
/// SIL Kit v5.0.8: "Timeout while waiting for replies from known participants: <who and how>").
/// It happens when the participant joins while another one leaves (docs/dependencies.md, "Known
/// upstream bugs").
inline constexpr std::string_view kJoinTimeoutText = "Timeout while waiting for replies from known participants";

[[nodiscard]] bool is_name_in_use(std::string_view what) noexcept;
[[nodiscard]] bool is_join_timeout(std::string_view what) noexcept;

using ParticipantFactory = std::function<std::unique_ptr<SilKit::IParticipant>()>;
using Notice = std::function<void(std::string_view)>;

/// create_participant() with `create` in place of SilKit::CreateParticipant(): calls it again
/// while the name is in use (until `name_in_use_wait` from now has passed) and once after a join
/// timeout, telling `notice` (one line) that it does so. Neither retry extends the other's limit.
[[nodiscard]] std::unique_ptr<SilKit::IParticipant> create_participant_retrying(const ParticipantFactory& create,
                                                                              const std::string& name,
                                                                              std::chrono::milliseconds name_in_use_wait,
                                                                              const Notice& notice);

} // namespace vehicle_sil::detail
