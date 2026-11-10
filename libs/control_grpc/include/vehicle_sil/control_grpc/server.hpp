// SPDX-License-Identifier: MIT
//
// The control server: a vehicle_sil::control::Hub served over gRPC (Control API v1), with the
// time gate of a SimulationController (vehicle_sil::control_grpc).
#pragma once

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/core/failure_check.hpp"
#include "vehicle_sil/core/sim_time.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vehicle_sil {
class SimulationController;
} // namespace vehicle_sil

namespace vehicle_sil::control_grpc {

/// Where the simulation time stands, as the server reports it.
struct TimeView
{
    bool available = false; ///< false: this server does not control the time
    SimTime now{};
    bool paused = false;
    std::optional<SimTime> pause_target;
    std::optional<double> speed;
    bool running = false;
    bool ended = false; ///< the simulation ran and is over (it will not stand still again)

    bool operator==(const TimeView&) const = default;
};

/// The time operations the server offers. All are thread safe and do not wait, except
/// wait_paused(). Errors: std::invalid_argument or std::logic_error (reported to the client).
class TimeControl
{
public:
    virtual ~TimeControl() = default;
    [[nodiscard]] virtual TimeView state() const = 0;
    virtual void pause(std::optional<SimTime> at) = 0;
    virtual void resume() = 0;
    virtual void run_until(SimTime t) = 0;
    virtual void step(SimDuration d) = 0;
    virtual void set_speed(std::optional<double> speed) = 0;
    virtual std::optional<SimTime> wait_paused(std::chrono::milliseconds timeout) = 0;
    /// Asks the simulation to stop; does not wait for it.
    virtual void stop(const std::string& reason) = 0;
    /// The participants of the simulation and their SIL Kit lifecycle state ("" if not seen yet),
    /// as the controller sees them through the registry (on any PC, with or without an agent).
    [[nodiscard]] virtual std::vector<std::pair<std::string, std::string>> participants() const = 0;
    /// The first required participant that has left the running simulation, with its state
    /// (FailureCheck::left_participant()); empty if none. Pure: an implementation that forgot it
    /// would let a simulation run on after a participant left, without a word.
    [[nodiscard]] virtual std::optional<LeftParticipant> left_participant() const = 0;
};

/// The time gate of `controller` (SimulationControllerOptions::time_control must be set).
/// The controller must outlive the result.
[[nodiscard]] std::unique_ptr<TimeControl> time_control_of(SimulationController& controller);

/// What a client may do. Each role includes the ones before it, except Agent, which may only
/// connect as an agent (AgentService).
enum class Role
{
    Viewer,   ///< read: peers, values, operations, watch, time
    Operator, ///< and change: write variables, pause, resume, step, run until, speed
    Admin,    ///< and end the simulation: stop
    Agent,    ///< connect an agent (only)
};

/// A client's credential: a bearer token ("authorization: Bearer <token>").
struct Credential
{
    std::string name;  ///< for the audit log
    std::string token; ///< a long random secret
    Role role = Role::Viewer;
    /// Agent: the participant names it may connect as, or kAnyParticipant for any name. An
    /// agent credential must name at least one (std::invalid_argument, a runtime_error from
    /// load_credentials()); other roles name none.
    std::vector<std::string> participants{};
};

/// In Credential::participants: the agent may connect as any participant name.
inline constexpr std::string_view kAnyParticipant = "*";

/// Reads credentials from a text file: one per line, "<role> <name> <token> [participant,...]"
/// (role: viewer, operator, admin, agent). An agent line must name the participants it may
/// connect as (separated by commas or spaces), or "*" for any; the other roles name none. A word
/// that starts with '#' starts a comment. A token has at least 16 characters and is not
/// repeated. std::runtime_error on errors, with the file, the line and how to fix it.
[[nodiscard]] std::vector<Credential> load_credentials(const std::string& path);

struct SecurityOptions
{
    /// PEM text of the server's certificate chain and private key. Both set: TLS.
    std::string certificate_chain_pem{};
    std::string private_key_pem{};
    /// Empty: no authentication; every client may do everything.
    std::vector<Credential> credentials{};
    /// Listening on an address other than the loopback needs TLS and credentials unless this is
    /// set (std::invalid_argument otherwise).
    bool allow_insecure = false;
};

struct ServerOptions
{
    /// gRPC address to listen on; port 0 picks a free port (see ControlServer::port()).
    std::string listen = "127.0.0.1:0";
    control::HubOptions hub{};
    /// The time operations; nullptr: GetTime reports available = false and the others fail.
    /// Must outlive the server.
    TimeControl* time = nullptr;
    /// Capacity of a Watch whose request leaves it 0, and the largest a request may ask for.
    std::size_t default_watch_capacity = 1024;
    std::size_t max_watch_capacity = 65536;
    /// The longest a WaitOperation or WaitPaused may wait.
    std::chrono::milliseconds max_wait{60000};
    /// Calls that wait (Watch, WaitOperation, WaitPaused) at the same time; each holds a thread
    /// of the server. More are refused with RESOURCE_EXHAUSTED. Must be > 0.
    std::size_t max_waiting_calls = 256;
    /// Writes in one Write request (INVALID_ARGUMENT beyond). Must be > 0.
    std::size_t max_writes_per_request = 4096;
    SecurityOptions security{};
    /// Called with one line for every call that changes something (write, time operations),
    /// agent connections, and refused calls: who, from where, what, and the outcome. Control
    /// characters become '?' and a line is cut at about 1000 bytes. Refused calls are thinned
    /// out (anyone who reaches the port can make them): at most 20 lines per second, and a line
    /// with the number left out. Must be thread safe.
    std::function<void(const std::string& line)> audit{};
    /// How often the server reads `time` for Watch, ListPeers and simulation() (0: only when
    /// simulation()->poll() is called), and the grace of its failure check (SimulationMonitor).
    std::chrono::milliseconds monitor_period{50};
    std::chrono::steady_clock::duration leave_grace = FailureCheck::kDefaultGrace;
};

class SimulationMonitor;

/// Serves the Control API: ControlService for clients and AgentService for agents
/// (control_grpc::AgentClient). Starts listening in the constructor (std::runtime_error if it
/// cannot) and stops in the destructor.
class ControlServer
{
public:
    explicit ControlServer(ServerOptions options);
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    /// The port the server listens on.
    [[nodiscard]] int port() const noexcept;
    [[nodiscard]] control::Hub& hub() noexcept;
    /// The monitor of `time` (its snapshot and its changes); nullptr without ServerOptions::time.
    [[nodiscard]] SimulationMonitor* simulation() noexcept;
    /// Drops every agent connection (the agents reconnect). For tests and administration.
    void disconnect_agents();
    /// Stops serving: open watches, waits and agent streams end. Idempotent.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vehicle_sil::control_grpc
