// SPDX-License-Identifier: MIT
//
// Connects a control agent to a control server over gRPC (vehicle_sil::control_grpc).
#pragma once

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/control/hub.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vehicle_sil::control_grpc {

struct AgentClientOptions
{
    /// The control server, "host:port".
    std::string server;
    /// The SIL Kit participant name of the agent's peer.
    std::string participant;
    /// New for every start of the process; empty: one is made from the clock and the process.
    std::string incarnation{};
    /// How often values, events, results and the status are sent (wall clock).
    std::chrono::milliseconds publish_period{50};
    /// Waits between connection attempts: doubled from min up to max.
    std::chrono::milliseconds reconnect_min{100};
    std::chrono::milliseconds reconnect_max{2000};
    /// The peer's state outside its steps, e.g. [&] { return PeerStatus{peer.failed(), peer.error()}; }.
    std::function<control::PeerStatus()> status{};
    /// The bearer token of an Agent credential (empty: none). Printable ASCII only (0x20 to 0x7E);
    /// anything else: std::invalid_argument.
    std::string token{};
    /// TLS. With root_certificates_pem empty, the server's certificate must chain to the
    /// system's root certificates.
    bool tls = false;
    /// PEM text of the root certificates that the server's certificate must chain to (set: TLS).
    std::string root_certificates_pem{};
    /// Called once, on the client's thread, when another process's connection of the same
    /// participant replaced this one and the client stopped reconnecting for good; `why` is
    /// last_error(). Empty: `why` is written to std::cerr as one line. It must not throw, call
    /// stop() or destroy the client (Peer::request_stop() is fine).
    std::function<void(const std::string& why)> on_replaced{};
};

/// Runs the transport side of `agent` on threads of its own: connects to the server (again and
/// again with back-off if the link is lost, waiting reconnect_max after a refusal, see
/// last_error(); the simulation is never held up). If another process connects as the same
/// participant, the newer connection wins: the server ends this client's connection with ABORTED
/// and names the replacing connection in the trailing metadata "vehicle-sil-replaced-by"; this
/// client then stops reconnecting for good (on_replaced, replaced(), last_error() says why).
/// Replaced by its own newer connection (a reconnect after a lost link that the server had not
/// noticed yet), it goes on. An ABORTED without that metadata is a lost connection like any other. It is connected once the server has welcomed it. It hands the server's
/// commands to Agent::submit(), and sends the agent's results, values, events and the peer's
/// status. After a reconnect it tells the server which commands the agent still holds and sends
/// the latest results again (the server ignores the ones it has). A result that could not be
/// sent and fell out of the latest results (more than max_pending since) is not sent again; the
/// server then settles its operation as Unknown.
///
/// Create it after the agent was attached to its peer (the schema is complete then) and destroy
/// it before the agent and before what `status` refers to (the peer); call Agent::finish() before
/// stop(), which sends what is left, waits up to about 3 s for the server to take it and then up to
/// 1.5 s for the stream to end (std::invalid_argument for non-positive periods or a token that is not
/// printable ASCII). It is the only transport-side user of the agent.
class AgentClient
{
public:
    AgentClient(control::Agent& agent, AgentClientOptions options);
    ~AgentClient();
    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;

    /// true while a connection is up (it may still be lost at any time).
    [[nodiscard]] bool connected() const;
    /// Waits until connected or the timeout expires.
    bool wait_connected(std::chrono::milliseconds timeout) const;
    /// Why the last connection failed or was refused (empty while connected or before).
    [[nodiscard]] std::string last_error() const;
    /// Another process's connection of the same participant replaced this one; the client no
    /// longer reconnects (see last_error()).
    [[nodiscard]] bool replaced() const;
    /// The identifier of the current (or last) connection (Hello::connection).
    [[nodiscard]] std::string connection_id() const;
    /// How many connections the server welcomed so far.
    [[nodiscard]] std::uint64_t sessions() const;
    [[nodiscard]] const std::string& incarnation() const noexcept;
    /// Stops the threads and closes the connection. Idempotent; the destructor calls it.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vehicle_sil::control_grpc
