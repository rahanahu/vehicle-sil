// SPDX-License-Identifier: MIT
//
// The availability of the variables over gRPC: the declaration in the schema (ListPeers, Watch),
// the states a Watch passes on only when it asks, and the new incarnation that starts again from
// its declaration. The hub is fed by the test, as in the snapshot tests.
#include "test_harness.hpp"

#include "convert.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
using control::Availability;

struct SilentLink final : control::AgentLink
{
    bool send(const control::Command&) override { return true; }
};

/// A peer of the hub with "level" (reports, starts Unknown), "lamp" (does not report) and "speed"
/// (an output that reports, starts Inactive).
struct ReportingPeer
{
    control::Hub& hub;
    control::ConnectionId connection = 0;
    double value = 0.0;
    control::Agent agent;

    ReportingPeer(control::Hub& h, const std::string& name, const std::string& incarnation) : hub(h)
    {
        agent.input("level", &value, {.availability = Availability::Unknown});
        agent.input("lamp", &value);
        agent.output("speed", &value, {.availability = Availability::Inactive});
        agent.complete_schema();
        connection = hub.agent_connected(control::PeerHello{name, incarnation, agent.schema(), 4}, std::make_shared<SilentLink>());
    }

    void set(const std::string& variable, Availability state)
    {
        agent.set_availability(variable, state);
        const auto states = agent.availability();
        hub.agent_availability(connection, agent.availability_version(), std::vector<Availability>(states.begin(), states.end()));
    }
};

/// A Watch collected on a thread of its own.
struct Watcher
{
    std::mutex mutex;
    std::vector<v1::Update> updates;
    grpc::ClientContext context;
    std::thread thread;

    Watcher(const std::shared_ptr<grpc::Channel>& channel, const v1::WatchRequest& request)
    {
        thread = std::thread([this, stub = v1::ControlService::NewStub(channel), request]() mutable {
            auto reader = stub->Watch(&context, request);
            v1::Update update;
            while (reader->Read(&update))
            {
                const std::lock_guard lock(mutex);
                updates.push_back(update);
            }
            static_cast<void>(reader->Finish());
        });
    }
    ~Watcher()
    {
        context.TryCancel();
        thread.join();
    }

    std::vector<v1::Update> copy()
    {
        const std::lock_guard lock(mutex);
        return updates;
    }
    bool done()
    {
        for (const auto& u : copy())
        {
            if (u.has_initial_done())
            {
                return true;
            }
        }
        return false;
    }
    /// The availability updates so far, in order.
    std::vector<v1::PeerAvailability> availability()
    {
        std::vector<v1::PeerAvailability> out;
        for (const auto& u : copy())
        {
            if (u.has_availability())
            {
                out.push_back(u.availability());
            }
        }
        return out;
    }
};

template <typename Pred>
bool eventually(Pred pred)
{
    return vehicle_sil::unit::eventually(pred, 10s, 10ms);
}

std::shared_ptr<grpc::Channel> channel_to(const control_grpc::ControlServer& server)
{
    return grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials());
}

v1::WatchRequest watch(bool availability, std::vector<std::string> peers = {})
{
    v1::WatchRequest request;
    request.set_availability(availability);
    for (const auto& p : peers)
    {
        request.add_peers(p);
    }
    return request;
}

} // namespace

TEST_CASE(control_server_declares_the_availability_in_the_schema)
{
    control_grpc::ControlServer server({.listen = "127.0.0.1:0"});
    ReportingPeer engine(server.hub(), "Engine", "e1");
    const auto stub = v1::ControlService::NewStub(channel_to(server));
    grpc::ClientContext context;
    v1::ListPeersReply peers;
    CHECK(stub->ListPeers(&context, v1::ListPeersRequest{}, &peers).ok());
    CHECK_EQ(peers.peers_size(), 1);
    const auto& schema = peers.peers(0).schema();
    for (const auto& v : schema)
    {
        if (v.name() == "level")
        {
            CHECK(v.availability() == v1::AVAILABILITY_UNKNOWN);
        }
        else if (v.name() == "speed")
        {
            CHECK(v.availability() == v1::AVAILABILITY_INACTIVE);
        }
        else
        {
            CHECK(v.availability() == v1::AVAILABILITY_UNSPECIFIED); // "lamp" and the built-in ones do not report
        }
    }

    // The same declaration in the PeerInfo of a Watch.
    Watcher watcher(channel_to(server), watch(false));
    CHECK(eventually([&] { return watcher.done(); }));
    bool seen = false;
    for (const auto& u : watcher.copy())
    {
        if (u.has_peer())
        {
            seen = true;
            for (const auto& v : u.peer().schema())
            {
                CHECK((v.name() == "level") == (v.availability() == v1::AVAILABILITY_UNKNOWN));
            }
        }
    }
    CHECK(seen);
}

TEST_CASE(control_server_watch_passes_the_availability_on_only_when_asked)
{
    control_grpc::ControlServer server({.listen = "127.0.0.1:0"});
    ReportingPeer engine(server.hub(), "Engine", "e1");
    ReportingPeer body(server.hub(), "Body", "b1");
    Watcher plain(channel_to(server), watch(false));
    Watcher asking(channel_to(server), watch(true));
    Watcher only_body(channel_to(server), watch(true, {"Body"}));
    CHECK(eventually([&] { return plain.done() && asking.done() && only_body.done(); }));

    // The state now comes with the initial updates: after the peers, before initial_done.
    const auto initial = asking.copy();
    std::size_t first_peer = initial.size(), last_peer = 0, first_availability = initial.size(), done = initial.size();
    for (std::size_t i = 0; i < initial.size(); ++i)
    {
        if (initial[i].has_peer())
        {
            first_peer = std::min(first_peer, i);
            last_peer = i;
        }
        else if (initial[i].has_availability())
        {
            first_availability = std::min(first_availability, i);
        }
        else if (initial[i].has_initial_done())
        {
            done = i;
        }
    }
    CHECK(first_peer < first_availability);
    CHECK(last_peer < first_availability);
    CHECK(first_availability < done);
    const auto now = asking.availability();
    CHECK_EQ(now.size(), std::size_t{2});
    for (const auto& a : now)
    {
        CHECK(a.incarnation() == (a.peer() == "Engine" ? "e1" : "b1"));
        // The schema's order: level, lamp, speed.
        CHECK_EQ(a.states_size(), 3);
        CHECK(a.states(a.states_size() - 3) == v1::AVAILABILITY_UNKNOWN);  // level
        CHECK(a.states(a.states_size() - 2) == v1::AVAILABILITY_ACTIVE);   // lamp: does not report
        CHECK(a.states(a.states_size() - 1) == v1::AVAILABILITY_INACTIVE); // speed
    }
    // peers: the ones asked for only.
    const auto filtered = only_body.availability();
    CHECK_EQ(filtered.size(), std::size_t{1});
    CHECK(filtered.at(0).peer() == "Body");

    // A change comes as one update to the ones that asked (and, filtered, only for their peer).
    engine.set("level", Availability::Active);
    CHECK(eventually([&] { return asking.availability().size() == 3; }));
    const auto changed = asking.availability().back();
    CHECK(changed.peer() == "Engine");
    CHECK(changed.states(changed.states_size() - 3) == v1::AVAILABILITY_ACTIVE);
    body.set("level", Availability::Inactive);
    CHECK(eventually([&] { return only_body.availability().size() == 2; }));
    CHECK(only_body.availability().back().states(changed.states_size() - 3) == v1::AVAILABILITY_INACTIVE);
    CHECK(eventually([&] { return asking.availability().size() == 4; }));

    // Without the request nothing came: only what the Watch always sends.
    for (const auto& u : plain.copy())
    {
        CHECK(!u.has_availability());
    }
    CHECK_EQ(only_body.availability().size(), std::size_t{2});
}

TEST_CASE(control_server_a_new_incarnation_starts_from_its_declaration)
{
    control_grpc::ControlServer server({.listen = "127.0.0.1:0"});
    auto engine = std::make_unique<ReportingPeer>(server.hub(), "Engine", "e1");
    Watcher asking(channel_to(server), watch(true));
    CHECK(eventually([&] { return asking.done(); }));
    engine->set("level", Availability::Active);
    CHECK(eventually([&] { return asking.availability().size() == 2; }));

    // Another process under the same name: the declaration again, after its PeerInfo.
    ReportingPeer restarted(server.hub(), "Engine", "e2");
    CHECK(eventually([&] { return asking.availability().size() == 3; }));
    const auto latest = asking.availability().back();
    CHECK(latest.incarnation() == "e2");
    CHECK(latest.states(latest.states_size() - 3) == v1::AVAILABILITY_UNKNOWN);
}
