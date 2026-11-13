// SPDX-License-Identifier: MIT
//
// A server that only shows the simulation (ServerOptions::observe_only): which calls it refuses,
// how, and that a refused call reaches neither the hub nor the time. The expected table is
// written out here, apart from the server's code.
#include "test_harness.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
using control_grpc::Credential;
using control_grpc::Role;

constexpr const char* kViewer = "viewer-token-0123456789";
constexpr const char* kOperator = "operator-token-0123456789";
constexpr const char* kAdmin = "admin-token-0123456789";

/// A time that counts what reaches it.
class CountingTime final : public control_grpc::TimeControl
{
public:
    std::atomic<int> changes{0};
    std::atomic<int> stops{0};

    [[nodiscard]] control_grpc::TimeView state() const override
    {
        control_grpc::TimeView v;
        v.available = true;
        v.running = true;
        v.now = SimTime{} + 30ms;
        return v;
    }
    void pause(std::optional<SimTime>) override { ++changes; }
    void resume() override { ++changes; }
    void run_until(SimTime) override { ++changes; }
    void step(SimDuration) override { ++changes; }
    void set_speed(std::optional<double>) override { ++changes; }
    std::optional<SimTime> wait_paused(std::chrono::milliseconds) override { return SimTime{} + 30ms; }
    void stop(const std::string&) override { ++stops; }
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> participants() const override { return {}; }
    [[nodiscard]] std::optional<LeftParticipant> left_participant() const override { return std::nullopt; }
};

/// An agent's link that counts the commands the hub hands it.
struct CountingLink final : control::AgentLink
{
    std::atomic<int> sent{0};
    bool send(const control::Command&) override
    {
        ++sent;
        return true;
    }
};

struct Server
{
    control::Hub hub;
    CountingTime time;
    std::shared_ptr<CountingLink> link = std::make_shared<CountingLink>();
    std::mutex mutex;
    std::vector<std::string> audit;
    std::unique_ptr<control_grpc::ControlServer> server;
    std::shared_ptr<grpc::Channel> channel;

    explicit Server(bool observe_only, bool credentials = false)
    {
        // One peer with a variable that can be written, and one that is gone (for ForgetPeer).
        control::VariableInfo level;
        level.name = "level";
        level.type = control::ValueType::Real;
        level.causality = control::Causality::Input;
        hub.agent_connected(control::PeerHello{"Engine", "e1", {level}, 16}, link);
        const auto gone = hub.agent_connected(control::PeerHello{"Gone", "g1", {level}, 16}, std::make_shared<CountingLink>());
        hub.agent_disconnected(gone);

        control_grpc::ServerOptions o;
        o.external_hub = &hub;
        o.time = &time;
        o.monitor_period = 0ms;
        if (observe_only)
        {
            o.observe_only = control_grpc::ObserveOnly{"scenario-running", "a scenario plays; read, or stop"};
        }
        if (credentials)
        {
            o.security.credentials = {Credential{"alice", kViewer, Role::Viewer}, Credential{"bob", kOperator, Role::Operator},
                                      Credential{"carol", kAdmin, Role::Admin}};
        }
        o.audit = [this](const std::string& line) {
            const std::lock_guard lock(mutex);
            audit.push_back(line);
        };
        server = std::make_unique<control_grpc::ControlServer>(std::move(o));
        channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(server->port()), grpc::InsecureChannelCredentials());
    }

    std::vector<std::string> lines()
    {
        const std::lock_guard lock(mutex);
        return audit;
    }
};

struct Answer
{
    grpc::Status status;
    std::string reason; ///< the trailing metadata vehicle-sil-reason ("" if none)
};

std::string reason_of(const grpc::ClientContext& context)
{
    const auto& trailing = context.GetServerTrailingMetadata();
    const auto found = trailing.find(control_grpc::kRefusalReasonKey);
    return found == trailing.end() ? std::string() : std::string(found->second.data(), found->second.size());
}

using Call = std::function<Answer(grpc::Channel&, const char* token)>;

void prepare(grpc::ClientContext& context, const char* token)
{
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    if (token != nullptr)
    {
        context.AddMetadata("authorization", std::string("Bearer ") + token);
    }
}

template <typename Request, typename Reply, typename F>
Call unary(F method, std::function<void(Request&)> fill = {})
{
    return [method, fill](grpc::Channel& channel, const char* token) {
        auto stub = v1::ControlService::NewStub(std::shared_ptr<grpc::Channel>(&channel, [](grpc::Channel*) {}));
        grpc::ClientContext context;
        prepare(context, token);
        Request request;
        if (fill)
        {
            fill(request);
        }
        Reply reply;
        Answer a;
        a.status = ((*stub).*method)(&context, request, &reply);
        a.reason = reason_of(context);
        return a;
    };
}

/// Every method with a request that the server takes when it is not observe-only.
std::map<std::string, Call> calls()
{
    using S = v1::ControlService::Stub;
    std::map<std::string, Call> m;
    m["Ping"] = unary<v1::PingRequest, v1::PingReply>(&S::Ping);
    m["ListPeers"] = unary<v1::ListPeersRequest, v1::ListPeersReply>(&S::ListPeers);
    m["GetValues"] = unary<v1::GetValuesRequest, v1::GetValuesReply>(&S::GetValues, [](v1::GetValuesRequest& r) { r.set_peer("Engine"); });
    m["Write"] = unary<v1::WriteRequest, v1::WriteReply>(&S::Write, [](v1::WriteRequest& r) {
        auto* w = r.add_writes();
        w->set_peer("Engine");
        w->set_name("level");
        w->mutable_value()->set_d(1.0);
    });
    m["GetOperation"] = unary<v1::GetOperationRequest, v1::Operation>(&S::GetOperation);
    m["WaitOperation"] = unary<v1::WaitOperationRequest, v1::Operation>(&S::WaitOperation);
    m["ForgetPeer"] = unary<v1::ForgetPeerRequest, v1::ForgetPeerReply>(&S::ForgetPeer, [](v1::ForgetPeerRequest& r) { r.set_peer("Gone"); });
    m["GetTime"] = unary<v1::GetTimeRequest, v1::TimeState>(&S::GetTime);
    m["GetSnapshot"] = unary<v1::GetSnapshotRequest, v1::GetSnapshotReply>(&S::GetSnapshot);
    m["Pause"] = unary<v1::PauseRequest, v1::TimeState>(&S::Pause);
    m["Resume"] = unary<v1::ResumeRequest, v1::TimeState>(&S::Resume);
    m["RunUntil"] = unary<v1::RunUntilRequest, v1::TimeState>(&S::RunUntil, [](v1::RunUntilRequest& r) { r.set_at_ns(50'000'000); });
    m["Step"] = unary<v1::StepRequest, v1::TimeState>(&S::Step, [](v1::StepRequest& r) { r.set_duration_ns(10'000'000); });
    m["SetSpeed"] = unary<v1::SetSpeedRequest, v1::TimeState>(&S::SetSpeed);
    m["WaitPaused"] = unary<v1::WaitPausedRequest, v1::TimeState>(&S::WaitPaused, [](v1::WaitPausedRequest& r) { r.set_timeout_ms(1000); });
    m["Stop"] = unary<v1::StopRequest, v1::TimeState>(&S::Stop);
    m["Watch"] = [](grpc::Channel& channel, const char* token) {
        auto stub = v1::ControlService::NewStub(std::shared_ptr<grpc::Channel>(&channel, [](grpc::Channel*) {}));
        grpc::ClientContext context;
        prepare(context, token);
        auto reader = stub->Watch(&context, v1::WatchRequest{});
        v1::Update u;
        Answer a;
        if (reader->Read(&u))
        {
            context.TryCancel(); // served: it streams
            reader->Finish();
            return a;
        }
        a.status = reader->Finish();
        a.reason = reason_of(context);
        return a;
    };
    return m;
}

/// The calls that change the simulation: an observe-only server refuses exactly these. Stop is
/// not one of them (a simulation can always be ended), and neither is any call that reads or waits.
const std::vector<std::string>& changing()
{
    static const std::vector<std::string> names = {"Write", "ForgetPeer", "Pause", "Resume", "RunUntil", "Step", "SetSpeed"};
    return names;
}

bool changes(const std::string& method)
{
    return std::find(changing().begin(), changing().end(), method) != changing().end();
}

} // namespace

TEST_CASE(control_server_observe_only_table_covers_every_rpc)
{
    // A method added to the proto without a call here fails this test: decide whether it changes
    // the simulation, and write it into changing() if it does.
    const auto* service = v1::PingRequest::descriptor()->file()->FindServiceByName("ControlService");
    CHECK(service != nullptr);
    const auto table = calls();
    for (int i = 0; i < service->method_count(); ++i)
    {
        const std::string method(service->method(i)->name());
        if (table.count(method) == 0)
        {
            std::fprintf(stderr, "no call for %s\n", method.c_str());
        }
        CHECK(table.count(method) == 1);
    }
    CHECK_EQ(table.size(), static_cast<std::size_t>(service->method_count()));
    for (const auto& name : changing())
    {
        CHECK(table.count(name) == 1);
    }
}

TEST_CASE(control_server_that_only_observes_refuses_every_change_and_serves_the_rest)
{
    Server server(true);
    for (const auto& [method, call] : calls())
    {
        const Answer a = call(*server.channel, nullptr);
        if (changes(method))
        {
            if (a.status.error_code() != grpc::StatusCode::FAILED_PRECONDITION || a.reason != "scenario-running")
            {
                std::fprintf(stderr, "%s: status %d '%s', reason '%s'\n", method.c_str(), static_cast<int>(a.status.error_code()),
                             a.status.error_message().c_str(), a.reason.c_str());
            }
            CHECK(a.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
            CHECK(a.reason == "scenario-running");
            // What was refused, why, and what to do: the server's action, then the owner's words.
            CHECK(a.status.error_message().rfind("cannot ", 0) == 0);
            CHECK(a.status.error_message().find(": a scenario plays; read, or stop") != std::string::npos);
        }
        else
        {
            // Served as always (a status of the call itself, never the refusal).
            if (!a.reason.empty() || (a.status.error_code() != grpc::StatusCode::OK && a.status.error_code() != grpc::StatusCode::NOT_FOUND))
            {
                std::fprintf(stderr, "%s: status %d '%s', reason '%s'\n", method.c_str(), static_cast<int>(a.status.error_code()),
                             a.status.error_message().c_str(), a.reason.c_str());
            }
            CHECK(a.reason.empty());
            // GetOperation and WaitOperation of an operation that is not there: NOT_FOUND.
            CHECK(a.status.ok() || a.status.error_code() == grpc::StatusCode::NOT_FOUND);
        }
    }
    // Nothing of the refused calls got through: no command to the agent, no operation, the peer
    // that is gone still there, the time untouched. The stop did.
    CHECK_EQ(server.link->sent.load(), 0);
    CHECK_EQ(server.time.changes.load(), 0);
    CHECK_EQ(server.time.stops.load(), 1);
    CHECK_EQ(server.hub.peers().size(), std::size_t{2});
    // Each refused change left its line, with the reason.
    std::size_t refused = 0;
    for (const auto& line : server.lines())
    {
        refused += line.find("refused (scenario-running)") != std::string::npos ? 1 : 0;
    }
    CHECK_EQ(refused, changing().size());
}

TEST_CASE(control_server_that_does_not_only_observe_takes_the_same_calls)
{
    // The same requests on a server without observe_only: each change gets through. (So the
    // test above refuses calls that would have changed something.)
    Server server(false);
    for (const auto& [method, call] : calls())
    {
        const Answer a = call(*server.channel, nullptr);
        if (changes(method))
        {
            if (!a.status.ok())
            {
                std::fprintf(stderr, "%s: status %d '%s'\n", method.c_str(), static_cast<int>(a.status.error_code()),
                             a.status.error_message().c_str());
            }
            CHECK(a.status.ok());
        }
        CHECK(a.reason.empty());
    }
    CHECK_EQ(server.link->sent.load(), 1);
    CHECK_EQ(server.time.changes.load(), 5);
    CHECK_EQ(server.time.stops.load(), 1);
    CHECK_EQ(server.hub.peers().size(), std::size_t{1}); // "Gone" was forgotten
}

TEST_CASE(control_server_that_only_observes_refuses_whatever_the_role_after_the_authorization)
{
    Server server(true, true);
    const auto table = calls();
    // Who may not call at all is told so first (nothing about the scenario to a stranger).
    CHECK(table.at("Write")(*server.channel, nullptr).status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
    const Answer viewer = table.at("Write")(*server.channel, kViewer);
    CHECK(viewer.status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
    CHECK(viewer.reason.empty());
    // Who may: refused for the scenario, the admin too.
    for (const char* token : {kOperator, kAdmin})
    {
        for (const char* method : {"Write", "Pause", "Step"})
        {
            const Answer a = table.at(method)(*server.channel, token);
            CHECK(a.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
            CHECK(a.reason == "scenario-running");
        }
    }
    // The viewer reads, the admin stops.
    CHECK(table.at("GetSnapshot")(*server.channel, kViewer).status.ok());
    CHECK(table.at("Stop")(*server.channel, kAdmin).status.ok());
    CHECK_EQ(server.time.stops.load(), 1);
    CHECK_EQ(server.time.changes.load(), 0);
    CHECK_EQ(server.link->sent.load(), 0);
}

TEST_CASE(control_server_observe_only_needs_a_reason_that_can_be_sent)
{
    for (const char* reason : {"", "Scenario", "a b", "x\n"})
    {
        control_grpc::ServerOptions o;
        o.observe_only = control_grpc::ObserveOnly{reason, "why"};
        bool refused = false;
        try
        {
            const control_grpc::ControlServer server(std::move(o));
        }
        catch (const std::invalid_argument&)
        {
            refused = true;
        }
        CHECK(refused);
    }
}
