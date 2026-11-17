// SPDX-License-Identifier: MIT
//
// run --control: the Control Server (gRPC, Control API v1) of vehicle_sil_launch, on the run's
// hub (the ECUs' variables over SIL Kit) and its time commands. Only in a build with
// VEHICLE_SIL_BUILD_CONTROL_SERVER; main.cpp refuses --control otherwise.
#pragma once

#include "vehicle_sil/launch/run.hpp"

#include <functional>
#include <memory>
#include <string>

namespace vehicle_sil::launch_cli {

/// The command line of run --control, as given ("" if not).
struct ControlArguments
{
    std::string address;     ///< --control: <port> (on 127.0.0.1) or <host>:<port>, or unix:<path>
    std::string credentials; ///< --control-credentials: the file of tokens and roles
    std::string tls_cert;    ///< --control-tls-cert: PEM certificate chain
    std::string tls_key;     ///< --control-tls-key: PEM private key
    std::string audit_log;   ///< --control-audit-log: appended with who changed what
};

/// What the server does with the run.
struct ControlHooks
{
    /// The run's controls: the server's time operations go through their time commands. If their
    /// rules refuse a control server everything but a stop for the whole run (a run that plays a
    /// scenario: launch::reason::kScenarioRunning), the server only shows the run: its clients
    /// read, wait and stop, and each other call is refused with that reason (the trailing metadata
    /// "vehicle-sil-reason" of FAILED_PRECONDITION) and that message.
    launch::RunControls* controls = nullptr;
    /// A client asked to stop the run (Stop): as {"command":"stop"} on standard input. Thread safe,
    /// does not wait.
    std::function<void()> stop{};
};

/// The server while the run lasts: made before the run (reads and checks the files), started by
/// the run (RunOptions::serve_control), ended after it.
class LauncherControlServer
{
public:
    virtual ~LauncherControlServer() = default;
    /// For RunOptions::serve_control: starts listening on `hub`, the run's (never null), and keeps
    /// it until the server is destroyed (std::runtime_error if it cannot listen).
    virtual launch::ControlServerInfo start(std::shared_ptr<control::Hub> hub) = 0;
    /// Ends every call (the waits and watches return); idempotent. Also the destructor.
    virtual void end() = 0;
};

/// Reads and checks the arguments: cli::UsageError for a file that cannot be read, credentials
/// that are wrong, TLS with only one of its files, an address that is not the loopback without TLS
/// and credentials.
[[nodiscard]] std::unique_ptr<LauncherControlServer> make_control_server(const ControlArguments& arguments,
                                                                         ControlHooks hooks);

} // namespace vehicle_sil::launch_cli
