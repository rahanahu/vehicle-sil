// SPDX-License-Identifier: MIT
//
// The publish/subscribe topics of vehicle_sil::control_silkit (internal).
#pragma once

#include <silkit/services/pubsub/PubSubSpec.hpp>
#include <silkit/util/Span.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vehicle_sil::control_silkit::detail {

// One topic per direction and kind, the same for every participant: a message names its agent by
// the session number in it, so no topic carries a participant's name (nothing to escape, nothing
// to clash), and the hub's one publisher of commands exists before anyone starts. The media type
// has no version: the messages carry it, so that a hello of another version still arrives and
// can be reported.
inline constexpr const char* kMediaType = "application/vnd.vehicle-sil.control";
inline constexpr const char* kHelloTopic = "vehicle_sil.control.hello";         ///< agent -> hub, history 1
inline constexpr const char* kStepTopic = "vehicle_sil.control.step";           ///< agent -> hub, from the steps
inline constexpr const char* kSubmittedTopic = "vehicle_sil.control.submitted"; ///< agent -> hub, from the receive handler
inline constexpr const char* kCommandTopic = "vehicle_sil.control.command";     ///< hub -> every agent

inline SilKit::Services::PubSub::PubSubSpec spec(const char* topic)
{
    return SilKit::Services::PubSub::PubSubSpec{topic, kMediaType};
}

inline SilKit::Util::Span<const std::uint8_t> span_of(const std::vector<std::uint8_t>& bytes)
{
    return SilKit::Util::Span<const std::uint8_t>{bytes.data(), bytes.size()};
}

} // namespace vehicle_sil::control_silkit::detail
