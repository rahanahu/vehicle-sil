// SPDX-License-Identifier: MIT
//
// run --control: the Control Server of vehicle_sil_launch (control_server.hpp).
//
// The server's time operations are commands of the run like those on standard input: they go
// through launch::TimeCommands with the source "control_server" (one mutex for every source, the
// command event before the effect, the same rules). Its view of the time and of the participants
// is read through TimeCommands::inspect() while the run's controller is attached, and kept from
// the last look once it is detached (the controller goes then), so no call of the server ever
// reaches a controller that has gone.
#include "control_server.hpp"

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/core/failure_check.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::launch_cli {
namespace {

namespace orch = SilKit::Services::Orchestration;

constexpr const char* kSource = "control_server";

/// Nanoseconds as whole milliseconds, rounded up (the controller stands still only at whole ones).
std::int64_t ceil_ms(std::int64_t ns)
{
    constexpr std::int64_t kNs = 1'000'000;
    return ns / kNs + (ns % kNs > 0 ? 1 : 0);
}

/// The time as the server reports it, read from the controller (under TimeCommands' mutex).
control_grpc::TimeView view_of(SimulationController& c, bool holding)
{
    control_grpc::TimeView v;
    v.available = true;
    const TimeState s = c.time_state();
    v.speed = s.speed;
    if (holding)
    {
        // Before the first step: it stands still at 0, and has not started.
        v.paused = true;
        return v;
    }
    v.now = s.now;
    v.pause_target = s.pause_target;
    const auto state = c.runtime().state();
    const bool finished = c.runtime().started() && has_finished(PeerStatus{state});
    // One answer from two looks: a simulation that ended between them stands still nowhere (as
    // time_state() says of one that had ended before).
    v.paused = s.paused && !finished;
    v.running = !finished && state == orch::ParticipantState::Running;
    v.ended = finished;
    if (finished)
    {
        // Where it ended, as the result's sim_ms counts it.
        v.now = std::max(v.now, c.runtime().now());
    }
    return v;
}

std::vector<std::pair<std::string, std::string>> participants_of(SimulationController& c)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& name : c.required_participants())
    {
        const auto status = c.runtime().peer_status(name);
        out.emplace_back(name, status.state ? orch::to_string(*status.state) : std::string());
    }
    return out;
}

class LaunchTimeControl final : public control_grpc::TimeControl
{
public:
    LaunchTimeControl(launch::TimeCommands& commands, std::function<void()> stop) :
      commands_(commands), stop_(std::move(stop))
    {
        // The last look, as the controller goes: what the server reports from then on is where the
        // run ended (the result's sim_ms), not the last time someone happened to look.
        commands_.set_on_detach([this](SimulationController& c) {
            control_grpc::TimeView v = view_of(c, false);
            v.now = std::max(v.now, c.runtime().now()); // as the result's sim_ms
            v.paused = false;
            v.pause_target.reset();
            v.running = false;
            v.ended = true;
            auto participants = participants_of(c);
            auto left = FailureCheck::left_participant(c);
            const std::lock_guard lock(mutex_);
            last_view_ = v;
            last_participants_ = std::move(participants);
            if (left)
            {
                last_left_ = std::move(left);
            }
        });
    }
    ~LaunchTimeControl() override { commands_.set_on_detach({}); }
    LaunchTimeControl(const LaunchTimeControl&) = delete;
    LaunchTimeControl& operator=(const LaunchTimeControl&) = delete;

    control_grpc::TimeView state() const override
    {
        control_grpc::TimeView v;
        v.available = true;
        // Kept under TimeCommands' mutex too: a later look never loses to an earlier one.
        const auto stage = commands_.inspect([&](SimulationController& c, bool holding) {
            v = view_of(c, holding);
            const std::lock_guard lock(mutex_);
            last_view_ = v;
        });
        if (stage == launch::TimeCommands::Stage::Detached)
        {
            // The run is over (or did not start): where it got, for good.
            const std::lock_guard lock(mutex_);
            v = last_view_;
            v.available = true;
            v.ended = true;
        }
        return v;
    }

    void pause(std::optional<SimTime> at) override
    {
        launch::Command c = command("pause");
        if (at)
        {
            c.at_ms = at_ms(*at);
        }
        apply(c, at && *at < SimTime{});
    }
    void resume() override { apply(command("resume"), false); }
    void run_until(SimTime t) override
    {
        launch::Command c = command("run_until");
        c.at_ms = at_ms(t);
        apply(c, t < SimTime{});
    }
    void step(SimDuration d) override
    {
        launch::Command c = command("step");
        c.ms = d.count() > 0 ? ceil_ms(d.count()) : 0;
        apply(c, d.count() <= 0 || *c.ms > launch::kMaxMillis);
    }
    void set_speed(std::optional<double> speed) override
    {
        launch::Command c = command("speed");
        c.speed = speed;
        apply(c, speed && !(*speed >= launch::kMinSpeed && *speed <= launch::kMaxSpeed));
    }
    std::optional<SimTime> wait_paused(std::chrono::milliseconds timeout) override { return commands_.wait_paused(timeout); }
    void stop(const std::string&) override
    {
        // Decided and written with the time commands (refused once the run is over), and, under
        // their mutex, a stop request as {"command":"stop"} on standard input: an accepted stop
        // reaches the run before it detaches the commands (not while it ends the participants).
        apply(command("stop"), false, stop_);
    }

    std::vector<std::pair<std::string, std::string>> participants() const override
    {
        std::vector<std::pair<std::string, std::string>> out;
        const auto stage = commands_.inspect([&](SimulationController& c, bool) {
            out = participants_of(c);
            const std::lock_guard lock(mutex_);
            last_participants_ = out;
        });
        if (stage == launch::TimeCommands::Stage::Detached)
        {
            const std::lock_guard lock(mutex_);
            out = last_participants_;
        }
        return out;
    }

    std::optional<LeftParticipant> left_participant() const override
    {
        std::optional<LeftParticipant> left;
        const auto stage = commands_.inspect([&](SimulationController& c, bool) {
            left = FailureCheck::left_participant(c);
            const std::lock_guard lock(mutex_);
            if (left)
            {
                last_left_ = left;
            }
        });
        if (stage == launch::TimeCommands::Stage::Detached)
        {
            const std::lock_guard lock(mutex_);
            left = last_left_;
        }
        return left;
    }

private:
    static launch::Command command(const char* name)
    {
        launch::Command c;
        c.name = name;
        c.accepted = true;
        return c;
    }

    /// A time in whole milliseconds, rounded up; a time before 0 as -1 (refused).
    static std::int64_t at_ms(SimTime t)
    {
        const std::int64_t ns = t.time_since_epoch().count();
        return ns < 0 ? -1 : ceil_ms(ns);
    }

    /// The command through the run's time commands (its event, its rules; `accepted` as
    /// TimeCommands::apply() calls it); a refusal as the
    /// exception the server answers with: std::invalid_argument (INVALID_ARGUMENT) for a
    /// `bad_argument` while the run lasts, else std::logic_error (FAILED_PRECONDITION).
    void apply(const launch::Command& c, bool bad_argument, const std::function<void()>& accepted = {})
    {
        const launch::Command done = commands_.apply(c, kSource, accepted);
        if (!done.accepted)
        {
            if (bad_argument && commands_.inspect([](SimulationController&, bool) {}) != launch::TimeCommands::Stage::Detached)
            {
                throw std::invalid_argument(done.message);
            }
            throw std::logic_error(done.message);
        }
    }

    launch::TimeCommands& commands_;
    std::function<void()> stop_;
    mutable std::mutex mutex_; ///< after TimeCommands' mutex
    mutable control_grpc::TimeView last_view_{};
    mutable std::vector<std::pair<std::string, std::string>> last_participants_{};
    mutable std::optional<LeftParticipant> last_left_{};
};

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

/// The address with the port the server listens on (port 0 picks one).
std::string actual_address(const std::string& listen, int port)
{
    if (listen.rfind("unix:", 0) == 0)
    {
        return listen;
    }
    return listen.substr(0, listen.rfind(':') + 1) + std::to_string(port);
}

class Server final : public LauncherControlServer
{
public:
    Server(control_grpc::ServerOptions options, std::unique_ptr<LaunchTimeControl> time,
           std::shared_ptr<std::ofstream> audit) :
      options_(std::move(options)), time_(std::move(time)), audit_(std::move(audit))
    {
    }
    ~Server() override { end(); }

    launch::ControlServerInfo start(control::Hub& hub) override
    {
        control_grpc::ServerOptions o = options_;
        o.external_hub = &hub;
        o.time = time_.get();
        server_ = std::make_unique<control_grpc::ControlServer>(std::move(o));
        launch::ControlServerInfo info;
        info.address = actual_address(options_.listen, server_->port());
        info.tls = !options_.security.certificate_chain_pem.empty();
        info.credentials = !options_.security.credentials.empty();
        return info;
    }

    void end() override
    {
        if (server_)
        {
            server_->shutdown();
        }
    }

private:
    control_grpc::ServerOptions options_;
    std::unique_ptr<LaunchTimeControl> time_; ///< outlives the server
    std::shared_ptr<std::ofstream> audit_;
    std::unique_ptr<control_grpc::ControlServer> server_;
};

} // namespace

std::unique_ptr<LauncherControlServer> make_control_server(const ControlArguments& arguments, ControlHooks hooks)
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
    if (arguments.scenario)
    {
        // What a client writes, and when it lets the time go on, is not in the scenario's file:
        // with it, the same scenario would not give the same result any more. Reading changes
        // nothing (the hub's subscriptions are bounded and never make the simulation wait), and a
        // stop is the run's to take as always.
        options.observe_only = control_grpc::ObserveOnly{
            kScenarioRunning,
            "this run plays a scenario (run --scenario), and until it ends only the scenario writes variables and "
            "moves the time, so that the same scenario always gives the same result; a client can read (ListPeers, "
            "GetValues, GetSnapshot, Watch, GetTime, WaitPaused) and Stop; to write or to move the time from a "
            "client, run without --scenario"};
    }
    auto time = std::make_unique<LaunchTimeControl>(*hooks.commands, std::move(hooks.stop));
    return std::make_unique<Server>(std::move(options), std::move(time), std::move(audit));
}

} // namespace vehicle_sil::launch_cli
