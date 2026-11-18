// SPDX-License-Identifier: MIT
//
// run --control: its command line, read and checked (the server itself is launch/control_server.hpp in libs/launch).
#pragma once

#include "vehicle_sil/control_grpc/server.hpp"

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

/// Reads and checks the arguments: the options for make_control_server() (listen address, TLS
/// PEM texts, credentials, audit function). cli::UsageError for a file that cannot be read,
/// credentials that are wrong, TLS with only one of its files, an address that is not the loopback
/// without TLS and credentials, an audit log that cannot be opened.
[[nodiscard]] control_grpc::ServerOptions read_control_options(const ControlArguments& arguments);

} // namespace vehicle_sil::launch_cli
