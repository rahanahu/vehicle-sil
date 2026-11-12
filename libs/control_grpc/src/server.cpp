// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_grpc/server.hpp"

#include "agent_service.hpp"
#include "audit.hpp"
#include "auth.hpp"
#include "control_service.hpp"
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::control_grpc {
namespace {

using namespace detail;

} // namespace

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
        if (c.token.size() < 16)
        {
            throw std::invalid_argument("vehicle_sil control server: the token of " + c.name + " has fewer than 16 characters");
        }
        if (!c.participants.empty() && c.role != Role::Agent)
        {
            throw std::invalid_argument("vehicle_sil control server: only an agent credential names participants (" + c.name + ")");
        }
        if (c.participants.empty() && c.role == Role::Agent)
        {
            throw std::invalid_argument("vehicle_sil control server: the agent credential '" + c.name
                                        + "' names no participant; set its participants to the participant names it may "
                                          "connect as, or to {\"*\"} (kAnyParticipant) for any name");
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
    StreamRegistry streams;
    std::unique_ptr<SimulationMonitor> monitor; ///< outlives the services' calls (Watch subscribes)
    std::unique_ptr<grpc::Service> agents;
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
      agents(options.serve_agents ? make_agent_service(hub, auth, audit, streams, monitor.get()) : nullptr),
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
        grpc::ServerBuilder builder;
        // Keepalive: a link that went silent (a NAT or firewall dropped it) is found within
        // seconds, also while the simulation stands still and nothing flows.
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        builder.AddListeningPort(options.listen, credentials, &port);
        if (agents)
        {
            builder.RegisterService(agents.get());
        }
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

void ControlServer::disconnect_agents()
{
    impl_->streams.cancel_all();
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
