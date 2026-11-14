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
/// and how to go on. The way to go on depends on how the address is written (the host without
/// its case): the loopback of this computer (localhost, 127.x.y.z, ::1; also an address that
/// cannot be read) - start the registry; 0.0.0.0 or :: - that is an address to listen on, give
/// the address of the computer with the registry; any other host - check that the registry runs
/// on the computer with that address and can be reached (the address it listens on, the
/// firewall). The message does not say whose address the last kind is: it may be another
/// computer's or one of this computer's own. Where an example needs a port and the address names
/// none, it is 8500, the port of a registry started without one.
/// nullopt if `what` is another failure: report that one as it is.
[[nodiscard]] std::optional<std::string> registry_failure_message(std::string_view what, std::string_view registry_uri);

} // namespace vehicle_sil::bridge_runtime
