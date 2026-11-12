// SPDX-License-Identifier: MIT
//
// GetSnapshot: the time state and the latest values of several peers in one call. With a time
// control and a hub the test feeds (the shape of the reply, the mark `consistent`, the order of the
// two reads, the bounds), and with a coordinated simulation whose agents reach the hub over SIL
// Kit (the values are never older than the time, while it runs and when it stands still).
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/control_silkit/agent_transport.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
using vehicle_sil::unit::InProcessRegistry;

std::int64_t ns(std::chrono::nanoseconds d)
{
    return d.count();
}

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

/// A time control whose state the test sets; `on_state` runs in every state() before it answers.
class SetTime final : public control_grpc::TimeControl
{
public:
    mutable std::mutex mutex;
    control_grpc::TimeView view;
    std::function<void()> on_state;
    bool fails = false;

    void set(SimTime now, bool paused, bool ended = false)
    {
        const std::lock_guard lock(mutex);
        view.available = true;
        view.now = now;
        view.paused = paused;
        view.running = !ended;
        view.ended = ended;
    }

    [[nodiscard]] control_grpc::TimeView state() const override
    {
        if (on_state)
        {
            on_state();
        }
        const std::lock_guard lock(mutex);
        if (fails)
        {
            throw std::runtime_error("the time cannot be read now");
        }
        return view;
    }
    void pause(std::optional<SimTime>) override {}
    void resume() override {}
    void run_until(SimTime) override {}
    void step(SimDuration) override {}
    void set_speed(std::optional<double>) override {}
    std::optional<SimTime> wait_paused(std::chrono::milliseconds) override { return std::nullopt; }
    void stop(const std::string&) override {}
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> participants() const override { return {}; }
    [[nodiscard]] std::optional<LeftParticipant> left_participant() const override { return std::nullopt; }
};

/// A link to an agent that takes every command and never answers.
struct SilentLink final : control::AgentLink
{
    bool send(const control::Command&) override { return true; }
};

/// A peer of the hub with one output, whose samples the test hands in.
struct FedPeer
{
    control::Hub& hub;
    control::ConnectionId connection = 0;
    std::uint64_t sequence = 0;

    FedPeer(control::Hub& h, const std::string& name, const std::string& incarnation) : hub(h)
    {
        double value = 0.0;
        control::Agent agent;
        agent.output("value", &value);
        agent.complete_schema();
        connection = hub.agent_connected(control::PeerHello{name, incarnation, agent.schema(), 4}, std::make_shared<SilentLink>());
    }

    void sample(SimTime time, double value)
    {
        control::Snapshot s;
        s.time = time;
        s.sequence = ++sequence;
        s.values = {control::Value{value}};
        hub.agent_values(connection, std::move(s));
    }
};

struct Client
{
    std::unique_ptr<v1::ControlService::Stub> stub;

    explicit Client(const control_grpc::ControlServer& server) :
      stub(v1::ControlService::NewStub(
          grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials())))
    {
    }

    grpc::Status snapshot(const std::vector<std::string>& peers, v1::GetSnapshotReply& reply) const
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 20s);
        v1::GetSnapshotRequest request;
        for (const auto& peer : peers)
        {
            request.add_peers(peer);
        }
        reply.Clear();
        return stub->GetSnapshot(&context, request, &reply);
    }
};

PeerOptions coordinated(std::string name, const std::string& uri, SimDuration step)
{
    PeerOptions o;
    o.participant = std::move(name);
    o.registry_uri = uri;
    o.lifecycle = LifecycleMode::Coordinated;
    o.virtual_time = VirtualTimeOptions{step, std::nullopt, std::nullopt};
    return o;
}

} // namespace

TEST_CASE(control_server_snapshot_has_the_time_and_the_values_of_the_peers_asked_for)
{
    SetTime time;
    time.set(at(100ms), true);
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .time = &time});
    FedPeer engine(server.hub(), "Engine", "e1");
    FedPeer body(server.hub(), "Body", "b1");
    FedPeer quiet(server.hub(), "Quiet", "q1");
    engine.sample(at(99ms), 1.5);
    body.sample(at(100ms), 2.5); // the step at the time itself
    const Client client(server);
    v1::GetSnapshotReply reply;

    // Without names: every peer of the hub, by name, with the time; all of one moment.
    CHECK(client.snapshot({}, reply).ok());
    CHECK(reply.time().available() && reply.time().paused() && reply.time().now_ns() == ns(100ms));
    CHECK(reply.consistent());
    CHECK_EQ(reply.values_size(), 3);
    CHECK(reply.values(0).peer() == "Body" && reply.values(0).incarnation() == "b1" && reply.values(0).has_snapshot());
    CHECK(reply.values(0).snapshot().time_ns() == ns(100ms) && reply.values(0).snapshot().values(0).d() == 2.5);
    CHECK(reply.values(1).peer() == "Engine" && reply.values(1).incarnation() == "e1");
    CHECK(reply.values(1).snapshot().time_ns() == ns(99ms) && reply.values(1).snapshot().sequence() == 1
          && reply.values(1).snapshot().values(0).d() == 1.5);
    // A peer without a sample yet: its incarnation, no snapshot.
    CHECK(reply.values(2).peer() == "Quiet" && reply.values(2).incarnation() == "q1" && !reply.values(2).has_snapshot());

    // With names: in the order given, each once; a name the hub does not know has neither.
    CHECK(client.snapshot({"Engine", "Nobody", "Engine", "Body"}, reply).ok());
    CHECK_EQ(reply.values_size(), 3);
    CHECK(reply.values(0).peer() == "Engine" && reply.values(0).has_snapshot());
    CHECK(reply.values(1).peer() == "Nobody" && reply.values(1).incarnation().empty() && !reply.values(1).has_snapshot());
    CHECK(reply.values(2).peer() == "Body" && reply.values(2).has_snapshot());
    CHECK(reply.consistent());

    // Not of one moment: a sample of a step after the time (the simulation went on meanwhile, or
    // the peer has another clock). The values are there all the same.
    engine.sample(at(101ms), 3.5);
    CHECK(client.snapshot({}, reply).ok());
    CHECK(!reply.consistent());
    CHECK(reply.time().paused() && reply.values(1).snapshot().time_ns() == ns(101ms));
    // The others alone are.
    CHECK(client.snapshot({"Body", "Quiet"}, reply).ok());
    CHECK(reply.consistent());
    // Nor while the time goes on, nor once the simulation is over.
    time.set(at(200ms), false);
    CHECK(client.snapshot({}, reply).ok());
    CHECK(!reply.consistent() && !reply.time().paused() && reply.time().now_ns() == ns(200ms));
    time.set(at(200ms), true, true);
    CHECK(client.snapshot({}, reply).ok());
    CHECK(!reply.consistent() && reply.time().ended());
    time.set(at(200ms), true);
    CHECK(client.snapshot({}, reply).ok());
    CHECK(reply.consistent());

    // The time cannot be read: the call fails (no values without their time).
    {
        const std::lock_guard lock(time.mutex);
        time.fails = true;
    }
    const auto failed = client.snapshot({}, reply);
    CHECK(failed.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK(failed.error_message() == "the time cannot be read now");
}

TEST_CASE(control_server_snapshot_reads_the_time_before_the_values)
{
    // Between the two reads the simulation may go on. Here it always does: reading the time lets
    // "a step" happen first (a new sample, and the time after it). Read in the right order, the
    // values hold every step before the time of the reply. Read the other way round, they would
    // be the old ones under the new time.
    SetTime time;
    // No monitor thread: only the calls read the time.
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .time = &time, .monitor_period = 0ms});
    FedPeer engine(server.hub(), "Engine", "e1");
    std::mutex step_mutex;
    std::int64_t step = 0;
    time.on_state = [&] {
        const std::lock_guard lock(step_mutex);
        ++step;
        engine.sample(at(1ms * step - 1ms), static_cast<double>(step)); // the step before the time
        time.set(at(1ms * step), true);
    };
    const Client client(server);
    for (int i = 0; i < 20; ++i)
    {
        v1::GetSnapshotReply reply;
        CHECK(client.snapshot({"Engine"}, reply).ok());
        CHECK(reply.time().now_ns() > 0);
        CHECK(reply.values(0).has_snapshot());
        CHECK_EQ(reply.values(0).snapshot().time_ns(), reply.time().now_ns() - ns(1ms));
        CHECK(reply.consistent());
    }
}

TEST_CASE(control_server_snapshot_without_a_time_control_and_its_bounds)
{
    // A server that does not control the time: the values, the time not available, never the mark.
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .max_peers_per_request = 3, .max_snapshot_bytes = 96});
    FedPeer engine(server.hub(), "Engine", "e1");
    FedPeer body(server.hub(), "Body", "b1");
    engine.sample(at(10ms), 1.0);
    const Client client(server);
    v1::GetSnapshotReply reply;
    CHECK(client.snapshot({}, reply).ok());
    CHECK(!reply.time().available() && !reply.consistent());
    CHECK_EQ(reply.values_size(), 2);
    CHECK(reply.values(1).peer() == "Engine" && reply.values(1).snapshot().values(0).d() == 1.0);

    // More names than a request may have.
    CHECK(client.snapshot({"Engine", "Body", "x"}, reply).ok());
    const auto many = client.snapshot({"Engine", "Body", "x", "y"}, reply);
    CHECK(many.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(many.error_message() == "at most 3 peers in one request");

    // A reply larger than a message may be: refused, with what to do. Fewer peers fit.
    const std::string long_name(80, 'n');
    const auto large = client.snapshot({"Engine", long_name}, reply);
    CHECK(large.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
    CHECK(large.error_message().find("the snapshot of 2 peers has ") == 0);
    CHECK(large.error_message().find(" bytes, more than the 96 a reply may have: name fewer peers in one request "
                                     "(GetSnapshotRequest.peers)")
          != std::string::npos);
    CHECK_EQ(reply.values_size(), 0);
    CHECK(client.snapshot({"Engine"}, reply).ok());

    CHECK(vehicle_sil::unit::throws<std::invalid_argument>(
        [] { control_grpc::ControlServer s({.listen = "127.0.0.1:0", .max_peers_per_request = 0}); }));
}

// The promise with a real simulation: clients ask again and again while the simulation runs free,
// is stopped and steps. Every reply's values hold every step before the reply's time; a reply with
// the mark has no value of a step after its time; and right after a stop, with nobody letting the
// simulation go on, the reply has the mark and the time of the stop.
TEST_CASE(control_server_snapshots_are_never_older_than_their_time_while_the_simulation_runs_and_stops)
{
    InProcessRegistry registry;
    SimulationControllerOptions controlOptions;
    controlOptions.registry_uri = registry.uri;
    controlOptions.required_participants = {"Engine", "Body"};
    controlOptions.time_control = TimeControlOptions{.step = 10ms, .start_paused = true};
    SimulationController controller(controlOptions);
    const auto time = control_grpc::time_control_of(controller);
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .time = time.get()});
    control_silkit::HubTransport hub_link(server.hub(), controller.runtime());

    // Samples in every step: the last step before a time is known exactly.
    const SimDuration steps[] = {3ms, 1ms}; // Body, Engine: the order of a reply without names
    double speed = 0.0;
    double lamp = 0.0;
    PeerOptions engine_options = coordinated("Engine", registry.uri, steps[1]);
    engine_options.monitor = false;
    Peer engine(engine_options);
    engine.set_agent_options(control::AgentOptions{.sample_period = SimDuration::zero()});
    engine.output("engine.speed", &speed);
    engine.on_step([&](SimTime) { speed += 1.0; });
    PeerOptions body_options = coordinated("Body", registry.uri, steps[0]);
    body_options.monitor = false;
    Peer body(body_options);
    body.set_agent_options(control::AgentOptions{.sample_period = SimDuration::zero()});
    body.output("cabin.lamp", &lamp);
    auto engine_link = std::make_unique<control_silkit::AgentTransport>(engine.agent(), engine.runtime());
    auto body_link = std::make_unique<control_silkit::AgentTransport>(body.agent(), body.runtime());
    start_together(controller, {&engine, &body});
    CHECK(time->wait_paused(20s) == SimTime{});

    // What is wrong with a reply ("" if nothing).
    const auto wrong = [&](const v1::GetSnapshotReply& reply) -> std::string {
        if (!reply.time().available() || reply.values_size() != 2 || reply.values(0).peer() != "Body"
            || reply.values(1).peer() != "Engine")
        {
            return "not the time and the two peers";
        }
        const std::int64_t now = reply.time().now_ns();
        for (int i = 0; i < 2; ++i)
        {
            const auto& values = reply.values(i);
            const std::int64_t step = ns(steps[i]);
            const std::int64_t last_before = (now + step - 1) / step * step - step;
            if (now > 0 && (!values.has_snapshot() || values.snapshot().time_ns() < last_before))
            {
                return values.peer() + " at " + std::to_string(values.has_snapshot() ? values.snapshot().time_ns() : -1)
                       + " ns is older than the last step before " + std::to_string(now) + " ns";
            }
            if (reply.consistent() && values.has_snapshot() && values.snapshot().time_ns() > now)
            {
                return values.peer() + " at " + std::to_string(values.snapshot().time_ns()) + " ns is after " + std::to_string(now)
                       + " ns in a reply that says consistent";
            }
        }
        if (reply.consistent() && !reply.time().paused())
        {
            return "consistent while the time goes on";
        }
        return {};
    };

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> moving{0};
    std::atomic<std::uint64_t> consistent{0};
    std::mutex failure_mutex;
    std::string failure;
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r)
    {
        readers.emplace_back([&] {
            const Client client(server);
            v1::GetSnapshotReply reply;
            while (!stop.load())
            {
                const auto status = client.snapshot({}, reply);
                const std::string what = status.ok() ? wrong(reply) : "GetSnapshot failed: " + status.error_message();
                if (!what.empty())
                {
                    const std::lock_guard lock(failure_mutex);
                    failure = what;
                    return;
                }
                moving.fetch_add(reply.time().paused() ? 0 : 1);
                consistent.fetch_add(reply.consistent() ? 1 : 0);
            }
        });
    }

    const Client client(server);
    const auto give_up = std::chrono::steady_clock::now() + 120s;
    constexpr std::uint64_t kMoving = 3000;
    int rounds = 0;
    for (; (rounds < 20 || moving.load() < kMoving) && std::chrono::steady_clock::now() < give_up; ++rounds)
    {
        if (rounds % 4 != 3)
        {
            time->resume();
            std::this_thread::sleep_for(std::chrono::milliseconds(2 + rounds % 5));
            time->pause(std::nullopt);
        }
        else
        {
            time->step(13ms);
        }
        const auto paused = time->wait_paused(30s);
        CHECK(paused.has_value());
        v1::GetSnapshotReply reply;
        CHECK(client.snapshot({}, reply).ok());
        CHECK(wrong(reply).empty());
        CHECK(reply.consistent());
        CHECK(reply.time().paused() && reply.time().now_ns() == paused->time_since_epoch().count());
    }
    stop.store(true);
    for (auto& reader : readers)
    {
        reader.join();
    }
    {
        const std::lock_guard lock(failure_mutex);
        if (!failure.empty())
        {
            throw vehicle_sil::unit::Failure(failure);
        }
    }
    CHECK(moving.load() >= kMoving);
    CHECK(consistent.load() > 0);
    CHECK_EQ(hub_link.stats().late_messages, std::uint64_t{0});

    controller.stop("done");
    engine.stop();
    body.stop();
    engine_link.reset();
    body_link.reset();
}
