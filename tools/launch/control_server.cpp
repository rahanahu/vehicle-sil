// SPDX-License-Identifier: MIT
//
// run --control: the Control Server of vehicle_sil_launch (control_server.hpp). It takes the checked
// options (control_arguments.hpp reads them from the command line) and knows nothing of the command line.
//
// The server's time operations are commands of the run like those on standard input: they go
// through launch::TimeCommands with the source "control_server" (one mutex for every source, the
// command event before the effect, the same rules). Its view of the time and of the participants
// is read through TimeCommands::inspect() while the run's controller is attached, and kept from
// the last look once it is detached (the controller goes then), so no call of the server ever
// reaches a controller that has gone.
#include "control_server.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/core/failure_check.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/launch/events.hpp"

#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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
        apply(c);
    }
    void resume() override { apply(command("resume")); }
    void run_until(SimTime t) override
    {
        launch::Command c = command("run_until");
        c.at_ms = at_ms(t);
        apply(c);
    }
    void step(SimDuration d) override
    {
        launch::Command c = command("step");
        c.ms = d.count() > 0 ? ceil_ms(d.count()) : 0;
        apply(c);
    }
    void set_speed(std::optional<double> speed) override
    {
        launch::Command c = command("speed");
        c.speed = speed;
        apply(c);
    }
    std::optional<SimTime> wait_paused(std::chrono::milliseconds timeout) override { return commands_.wait_paused(timeout); }
    void stop(const std::string&) override
    {
        // Decided and written with the time commands (refused once the run is over), and, under
        // their mutex, a stop request as {"command":"stop"} on standard input: an accepted stop
        // reaches the run before it detaches the commands (not while it ends the participants).
        apply(command("stop"), stop_);
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
    /// exception the server answers with, chosen by the rule that refused (so the client and the
    /// command event name the same reason): std::invalid_argument (INVALID_ARGUMENT) when it was
    /// the argument (out-of-range, missing-argument), else control_grpc::Refused
    /// (FAILED_PRECONDITION, with the code of the refusal, as the command event's "reason", for
    /// the client's program).
    void apply(const launch::Command& c, const std::function<void()>& accepted = {})
    {
        const launch::Command done = commands_.apply(c, kSource, accepted);
        if (done.pending)
        {
            // Not while this server listens: the run gives its time commands the time lock before
            // it starts the server. If it ever happens, the client is told what became of its call.
            throw std::logic_error("the run has not read its configuration yet: the command waits and is decided once "
                                   "it has (its command event says how); it is not sent again by this call");
        }
        if (!done.accepted)
        {
            if (done.reason == launch::reason::kOutOfRange || done.reason == launch::reason::kMissingArgument)
            {
                throw std::invalid_argument(done.message);
            }
            throw control_grpc::Refused(done.reason, done.message);
        }
    }

    launch::TimeCommands& commands_;
    std::function<void()> stop_;
    mutable std::mutex mutex_; ///< after TimeCommands' mutex
    mutable control_grpc::TimeView last_view_{};
    mutable std::vector<std::pair<std::string, std::string>> last_participants_{};
    mutable std::optional<LeftParticipant> last_left_{};
};

/// The address with the port the server listens on (port 0 picks one).
std::string actual_address(const std::string& listen, int port)
{
    if (listen.rfind("unix:", 0) == 0)
    {
        return listen;
    }
    return listen.substr(0, listen.rfind(':') + 1) + std::to_string(port);
}

class Server final : public launch::ControlEndpoint
{
public:
    Server(control_grpc::ServerOptions options, std::unique_ptr<LaunchTimeControl> time) :
      options_(std::move(options)), time_(std::move(time))
    {
    }
    ~Server() override { end(); }

    launch::ControlServerInfo start(std::shared_ptr<control::Hub> hub) override
    {
        hub_ = std::move(hub);
        control_grpc::ServerOptions o = options_;
        o.external_hub = hub_.get();
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
    std::shared_ptr<control::Hub> hub_; ///< the run's, from start(); before server_, which serves it: it goes after it
    std::unique_ptr<control_grpc::ControlServer> server_;
};

} // namespace

std::unique_ptr<launch::ControlEndpoint> make_control_server(control_grpc::ServerOptions options, ControlHooks hooks)
{
    // A run that plays a scenario: what a client writes, and when it lets the time go on, is not
    // in the scenario's file: with it, the same scenario would not give the same result any more.
    // Reading changes nothing (the hub's subscriptions are bounded and never make the simulation
    // wait), and a stop is the run's to take as always. The rule is the controls'; the refusal is
    // the server's, in one place before the call.
    if (auto only = hooks.controls->always_refused(launch::Entry::ControlServer))
    {
        options.observe_only = control_grpc::ObserveOnly{std::move(only->reason), std::move(only->message)};
    }
    auto time = std::make_unique<LaunchTimeControl>(hooks.controls->time(), std::move(hooks.stop));
    return std::make_unique<Server>(std::move(options), std::move(time));
}

} // namespace vehicle_sil::launch_cli
