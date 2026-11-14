// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_grpc/server.hpp"

#include "audit.hpp"
#include "auth.hpp"
#include "control_service.hpp"
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace vehicle_sil::control_grpc {
namespace {

using namespace detail;

} // namespace

std::string unix_socket_path_problem(const std::string& listen)
{
#ifndef _WIN32
    constexpr std::string_view scheme = "unix:";
    if (listen.rfind(scheme, 0) != 0)
    {
        return {};
    }
    // unix:<path> and unix://<absolute path> name the same file.
    std::string_view path = std::string_view(listen).substr(scheme.size());
    if (path.rfind("//", 0) == 0)
    {
        path.remove_prefix(2);
    }
    // gRPC turns each %XX (two hex digits) into one byte before it makes the address: count the
    // bytes of the file's name, not of what was written.
    const auto is_hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    std::size_t size = 0;
    for (std::size_t i = 0; i < path.size(); ++i, ++size)
    {
        if (path[i] == '%' && i + 2 < path.size() && is_hex(path[i + 1]) && is_hex(path[i + 2]))
        {
            i += 2;
        }
    }
    // The address keeps the path and the 0 that ends it.
    const std::size_t most = sizeof(sockaddr_un{}.sun_path) - 1;
    if (size > most)
    {
        return "the path of the socket is " + std::to_string(size) + " bytes long and this system takes " +
               std::to_string(most) + " at most: give a shorter path, for example unix:/tmp/vehicle-sil-control.sock";
    }
#else
    static_cast<void>(listen);
#endif
    return {};
}

bool is_loopback_address(const std::string& listen)
{
    if (listen.rfind("unix:", 0) == 0)
    {
        return true; // a local socket
    }
    const auto colon = listen.rfind(':');
    const std::string host = colon == std::string::npos ? listen : listen.substr(0, colon);
    if (host == "localhost" || host == "[::1]")
    {
        return true;
    }
    // 127.x.y.z, numbers only.
    int parts = 0;
    int first = -1;
    std::size_t pos = 0;
    while (pos <= host.size())
    {
        const auto dot = host.find('.', pos);
        const auto part = host.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos || std::stoi(part) > 255)
        {
            return false;
        }
        if (parts == 0)
        {
            first = std::stoi(part);
        }
        ++parts;
        if (dot == std::string::npos)
        {
            break;
        }
        pos = dot + 1;
    }
    return parts == 4 && first == 127;
}

namespace {

/// The checks of load_credentials() also for credentials given directly, and the other options.
ServerOptions check(ServerOptions o)
{
    const auto& sec = o.security;
    if (sec.certificate_chain_pem.empty() != sec.private_key_pem.empty())
    {
        throw std::invalid_argument("vehicle_sil control server: TLS needs both the certificate chain and the private key");
    }
    for (std::size_t i = 0; i < sec.credentials.size(); ++i)
    {
        const auto& c = sec.credentials[i];
        if (c.role != Role::Viewer && c.role != Role::Operator && c.role != Role::Admin)
        {
            throw std::invalid_argument("vehicle_sil control server: the credential of " + c.name + " has the role value " +
                                        std::to_string(static_cast<int>(c.role)) +
                                        ", which is no role: use Role::Viewer, Role::Operator or Role::Admin");
        }
        if (c.token.size() < 16)
        {
            throw std::invalid_argument("vehicle_sil control server: the token of " + c.name + " has fewer than 16 characters");
        }
        for (std::size_t j = 0; j < i; ++j)
        {
            if (sec.credentials[j].token == c.token)
            {
                throw std::invalid_argument("vehicle_sil control server: " + c.name + " and " + sec.credentials[j].name
                                            + " have the same token");
            }
        }
    }
    if (o.monitor_period < std::chrono::milliseconds::zero() || o.leave_grace < std::chrono::steady_clock::duration::zero())
    {
        throw std::invalid_argument("vehicle_sil control server: monitor_period and leave_grace must not be negative");
    }
    if (o.max_waiting_calls == 0 || o.max_writes_per_request == 0)
    {
        throw std::invalid_argument("vehicle_sil control server: max_waiting_calls and max_writes_per_request must be > 0");
    }
    if (o.max_peers_per_request == 0)
    {
        throw std::invalid_argument("vehicle_sil control server: max_peers_per_request must be > 0");
    }
    if (o.observe_only)
    {
        // It is sent as the value of a metadata entry, and programs compare it.
        const std::string& reason = o.observe_only->reason;
        if (reason.empty() || reason.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
        {
            throw std::invalid_argument("vehicle_sil control server: observe_only's reason must be a word of a-z, 0-9 "
                                        "and '-' (it is sent as metadata), not '" + reason + "'");
        }
    }
    return o;
}

} // namespace

struct ControlServer::Impl
{
    ServerOptions options;
    AuditLog audit;
    std::unique_ptr<control::Hub> own_hub; ///< without ServerOptions::external_hub
    control::Hub& hub;
    std::atomic<bool> stopping{false};
    Authorizer auth;
    std::unique_ptr<SimulationMonitor> monitor; ///< outlives the service's calls (Watch subscribes)
    std::unique_ptr<grpc::Service> clients;
    std::unique_ptr<grpc::Server> server;
    int port = 0;
    std::mutex shutdown_mutex;
    bool shut_down = false;

    explicit Impl(ServerOptions o) :
      options(check(std::move(o))),
      audit(options.audit),
      own_hub(options.external_hub == nullptr ? std::make_unique<control::Hub>(options.hub) : nullptr),
      hub(options.external_hub != nullptr ? *options.external_hub : *own_hub),
      auth(options.security),
      monitor(options.time != nullptr
                  ? std::make_unique<SimulationMonitor>(*options.time, SimulationMonitorOptions{options.monitor_period, options.leave_grace, {}})
                  : nullptr),
      clients(make_control_service(hub, options, monitor.get(), stopping, auth, audit))
    {
        const auto& sec = options.security;
        const bool tls = !sec.certificate_chain_pem.empty() && !sec.private_key_pem.empty();
        if (!is_loopback_address(options.listen) && !sec.allow_insecure && (!tls || sec.credentials.empty()))
        {
            throw std::invalid_argument("vehicle_sil control server: listening on " + options.listen
                                        + " (not the loopback) needs TLS and credentials, or allow_insecure");
        }
        std::shared_ptr<grpc::ServerCredentials> credentials;
        if (tls)
        {
            grpc::SslServerCredentialsOptions ssl;
            ssl.pem_key_cert_pairs.push_back({sec.private_key_pem, sec.certificate_chain_pem});
            credentials = grpc::SslServerCredentials(ssl);
        }
        else
        {
            credentials = grpc::InsecureServerCredentials();
        }
        if (const std::string problem = unix_socket_path_problem(options.listen); !problem.empty())
        {
            throw std::runtime_error("vehicle_sil control server: cannot listen on " + options.listen + ": " + problem);
        }
        grpc::ServerBuilder builder;
        // Keepalive: a link that went silent (a NAT or firewall dropped it) is found within
        // seconds, also while the simulation stands still and nothing flows.
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        // One server per port: with SO_REUSEPORT (gRPC's default where the system has it), a second
        // launcher would listen on the same port too, and which of the two a client reaches
        // would be open. Without it, the second one fails as for a port any other program has.
        builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
        builder.AddListeningPort(options.listen, credentials, &port);
        builder.RegisterService(clients.get());
        server = builder.BuildAndStart();
        if (!server || port == 0)
        {
            throw std::runtime_error("vehicle_sil control server: cannot listen on " + options.listen);
        }
    }
};

ControlServer::ControlServer(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

ControlServer::~ControlServer()
{
    shutdown();
}

int ControlServer::port() const noexcept
{
    return impl_->port;
}

control::Hub& ControlServer::hub() noexcept
{
    return impl_->hub;
}

SimulationMonitor* ControlServer::simulation() noexcept
{
    return impl_->monitor.get();
}

void ControlServer::shutdown()
{
    const std::lock_guard lock(impl_->shutdown_mutex);
    if (std::exchange(impl_->shut_down, true))
    {
        return;
    }
    impl_->stopping = true;
    impl_->server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    impl_->server->Wait();
}

} // namespace vehicle_sil::control_grpc
