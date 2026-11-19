// SPDX-License-Identifier: MIT
//
// The publish/subscribe topics of vehicle_sil::control_silkit (internal).
#pragma once

#include <silkit/services/pubsub/PubSubSpec.hpp>
#include <silkit/util/Span.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::control_silkit::detail {

// One topic per direction and kind, the same for every participant: a message names its agent by
// the session number in it, so no topic carries a participant's name (nothing to escape, nothing
// to clash), and the hub's publishers of commands exist before anyone starts. The media type has
// no version: the messages carry it, so that a hello of another version still arrives and can be
// reported.
inline constexpr const char* kMediaType = "application/vnd.vehicle-sil.control";
inline constexpr const char* kHelloTopic = "vehicle_sil.control.hello";         ///< agent -> hub, history 1
inline constexpr const char* kStepTopic = "vehicle_sil.control.step";           ///< agent -> hub, from the steps
inline constexpr const char* kSubmittedTopic = "vehicle_sil.control.submitted"; ///< agent -> hub, from the receive handler
inline constexpr const char* kCommandTopic = "vehicle_sil.control.command";     ///< hub -> the agents of a group

inline SilKit::Services::PubSub::PubSubSpec spec(const char* topic)
{
    return SilKit::Services::PubSub::PubSubSpec{topic, kMediaType};
}

// Commands go to a group of agents, not to all: the hub has one publisher of commands per group,
// made before the start, and an agent subscribes with the label of its group. The group is a hash
// of the participant's name, so that every process (on any machine) finds the same one. Agents of
// one group still receive each other's commands and drop them by the session. Optional labels
// (SIL Kit): a subscriber or publisher without the label matches every group, so an agent without
// it receives every command, and a hub without it reaches every agent.
inline constexpr std::size_t kCommandGroups = 32;
inline constexpr const char* kGroupLabel = "vehicle_sil.control.group";

/// The group of the agent of `participant`: FNV-1a (32 bits) of its name, modulo kCommandGroups.
inline std::size_t command_group(std::string_view participant)
{
    std::uint32_t h = 2166136261U;
    for (const char c : participant)
    {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619U;
    }
    return h % kCommandGroups;
}

/// The command topic for the agents of `group`.
inline SilKit::Services::PubSub::PubSubSpec command_spec(std::size_t group)
{
    auto s = spec(kCommandTopic);
    s.AddLabel(kGroupLabel, std::to_string(group), SilKit::Services::MatchingLabel::Kind::Optional);
    return s;
}

inline SilKit::Util::Span<const std::uint8_t> span_of(const std::vector<std::uint8_t>& bytes)
{
    return SilKit::Util::Span<const std::uint8_t>{bytes.data(), bytes.size()};
}

} // namespace vehicle_sil::control_silkit::detail
