// SPDX-License-Identifier: MIT
//
// What a bridge says when the SIL Kit registry does not answer: the usual first failure of a
// bridge, so the message names the address it tried and what to do next.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vehicle_sil::bridge_runtime {

/// The one-line message for a participant that could not be created because the registry at
/// `registry_uri` did not answer, from `what`, the text of the exception that creating the
/// participant threw: what happened, where the bridge looked (with SIL Kit's text, on one line),
/// and how to go on. nullopt if `what` is another failure: report that one as it is.
[[nodiscard]] std::optional<std::string> registry_failure_message(std::string_view what, std::string_view registry_uri);

} // namespace vehicle_sil::bridge_runtime
