// SPDX-License-Identifier: MIT
//
// run --control: the Control Server (gRPC, Control API v1) of vehicle_sil_launch, on the run's
// hub (the ECUs' variables over SIL Kit) and its time commands. Only in a build with
// VEHICLE_SIL_BUILD_CONTROL_SERVER; vehicle_sil_launch refuses --control otherwise.
#pragma once

#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <functional>
#include <memory>
#include <string>

namespace vehicle_sil::launch {

/// What the server does with the run.
struct ControlHooks
{
    /// The run's controls: the server's time operations go through their time commands. If their
    /// rules refuse a control server everything but a stop for the whole run (a run that plays a
    /// scenario: reason::kScenarioRunning), the server only shows the run: its clients
    /// read, wait and stop, and each other call is refused with that reason (the trailing metadata
    /// "vehicle-sil-reason" of FAILED_PRECONDITION) and that message.
    RunControls* controls = nullptr;
    /// A client asked to stop the run (Stop): as {"command":"stop"} on standard input. Thread safe,
    /// does not wait.
    std::function<void()> stop{};
};

/// The server while the run lasts (ControlEndpoint, for RunOptions::control_endpoint):
/// made here before the run from the checked options (read_control_options()), started by
/// run_file(), ended after it.
/// It keeps the run's hub from its start until it is destroyed (std::runtime_error from start() if
/// it cannot listen); its destructor ends it too.
/// The options must be checked already (listen address, TLS, credentials, audit); this reads no
/// command line and throws no usage error.
[[nodiscard]] std::unique_ptr<ControlEndpoint> make_control_server(control_grpc::ServerOptions options,
                                                                           ControlHooks hooks);

} // namespace vehicle_sil::launch
