// SPDX-License-Identifier: MIT
//
// run --control: reads and checks its command line (control_arguments.hpp).
#include "control_arguments.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace vehicle_sil::launch_cli {
namespace {

std::string read_file(const std::string& path, const char* option)
{
    // The argument is UTF-8 (on Windows too): as a path, not as the narrow code page.
    std::ifstream in(process::path_from_utf8(path), std::ios::binary);
    if (!in)
    {
        throw cli::UsageError("cannot read " + path + " (" + option + ")");
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// --control's value as a gRPC address: a port alone is on 127.0.0.1.
std::string listen_address(const std::string& value)
{
    if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos)
    {
        if (value.size() > 5 || std::stoi(value) > 65535)
        {
            throw cli::UsageError("--control " + value + ": a port is from 0 to 65535 (0: any free port)");
        }
        return "127.0.0.1:" + value;
    }
    if (value.rfind("unix:", 0) == 0)
    {
        if (const std::string problem = control_grpc::unix_socket_path_problem(value); !problem.empty())
        {
            throw cli::UsageError("--control " + value + ": " + problem);
        }
        return value;
    }
    const auto colon = value.rfind(':');
    const std::string port = colon == std::string::npos ? std::string() : value.substr(colon + 1);
    if (colon == std::string::npos || colon == 0 || port.empty() || port.find_first_not_of("0123456789") != std::string::npos ||
        port.size() > 5 || std::stoi(port) > 65535)
    {
        throw cli::UsageError("--control must be <port> (on 127.0.0.1; 0: any free port), <host>:<port> (a port from 0 to "
                              "65535) or unix:<path>, not '" + value + "'");
    }
    return value;
}

} // namespace

control_grpc::ServerOptions read_control_options(const ControlArguments& arguments)
{
    control_grpc::ServerOptions options;
    options.listen = listen_address(arguments.address);
    if (arguments.tls_cert.empty() != arguments.tls_key.empty())
    {
        throw cli::UsageError("--control-tls-cert and --control-tls-key go together: TLS needs the certificate chain and "
                              "the private key");
    }
    if (!arguments.tls_cert.empty())
    {
        options.security.certificate_chain_pem = read_file(arguments.tls_cert, "--control-tls-cert");
        options.security.private_key_pem = read_file(arguments.tls_key, "--control-tls-key");
    }
    if (!arguments.credentials.empty())
    {
        const std::string text = read_file(arguments.credentials, "--control-credentials");
        try
        {
            options.security.credentials = control_grpc::parse_credentials(text, arguments.credentials);
        }
        catch (const std::exception& e)
        {
            throw cli::UsageError(std::string("--control-credentials: ") + e.what());
        }
        // An empty file would leave the server open to everyone, against what the option says.
        if (options.security.credentials.empty())
        {
            throw cli::UsageError("--control-credentials: " + arguments.credentials +
                                  " has no credential: write one line per token, <role> <name> <token> (the roles are "
                                  "viewer, operator and admin)");
        }
    }
    if (!control_grpc::is_loopback_address(options.listen) &&
        (options.security.certificate_chain_pem.empty() || options.security.credentials.empty()))
    {
        throw cli::UsageError("--control " + arguments.address +
                              " is reachable from other computers: a control server there needs TLS and credentials "
                              "(--control-tls-cert, --control-tls-key and --control-credentials); on this computer only, "
                              "give a port (on 127.0.0.1) or 127.0.0.1:<port>");
    }
    std::shared_ptr<std::ofstream> audit;
    if (!arguments.audit_log.empty())
    {
        audit = std::make_shared<std::ofstream>(process::path_from_utf8(arguments.audit_log), std::ios::app);
        if (!*audit)
        {
            throw cli::UsageError("cannot open " + arguments.audit_log + " (--control-audit-log)");
        }
        auto mutex = std::make_shared<std::mutex>();
        options.audit = [audit, mutex](const std::string& line) {
            const std::lock_guard lock(*mutex);
            *audit << line << std::endl;
        };
    }
    return options;
}

} // namespace vehicle_sil::launch_cli
