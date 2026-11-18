// SPDX-License-Identifier: MIT
//
// run --control: the control server that run_file() makes for its run from the request
// (RunHost::control). The one in control_server.cpp is in a build with
// VEHICLE_SIL_BUILD_CONTROL_SERVER; a build without it has control_server_none.cpp.
#pragma once

#include "vehicle_sil/launch/request.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <memory>
#include <vector>

namespace vehicle_sil::launch::detail {

/// The server of one run, not started yet, or why the request for it is refused.
struct ControlServerOfRun
{
    std::shared_ptr<ControlEndpoint> server; ///< null if `findings` is not empty
    /// What is wrong with the request, as a usage error (code "usage"): the first thing found.
    std::vector<Finding> findings{};
};

/// Reads and checks what `asked` names, in this order: the form of the address, TLS with both of
/// its files, the certificate, the key, the credentials (read, parsed, not empty), an address that
/// is not the loopback only with TLS and credentials, the audit log (opened for appending). Then
/// makes the server on `controls` (its time operations go to their time commands, a client's Stop
/// to their stop requests as stop_source::kControlStop), which must outlive it. Reads files;
/// starts nothing and listens nowhere (ControlEndpoint::start() does).
/// In a build without the control server: throws std::invalid_argument.
[[nodiscard]] ControlServerOfRun make_control_server_of_run(const ControlHost& asked, RunControls& controls);

} // namespace vehicle_sil::launch::detail
