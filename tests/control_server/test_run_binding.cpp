// SPDX-License-Identifier: MIT
//
// The run a control server belongs to (ServerOptions::run, run_required): a call that names
// another run, or none where the run is required, is refused before it reaches the hub or the
// time, with FAILED_PRECONDITION and the reason as trailing metadata; Ping is never refused for it
// and reports the run; the check comes after the authentication and the role; the audit lines
// carry the run. Last, what the check is for: a client of an earlier run whose channel reconnects
// by itself to the next server at the same address is refused there.
#include "test_harness.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
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
        v.paused = true;
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

struct Options
{
    std::string run;
    bool required = false;
    bool credentials = false;
    std::string listen = "127.0.0.1:0";
};

/// A control server of a run, with a peer that has a writable variable and one that is gone.
struct Server
{
    control::Hub hub;
    CountingTime time;
    std::shared_ptr<CountingLink> link = std::make_shared<CountingLink>();
    std::mutex mutex;
    std::vector<std::string> audit;
    std::unique_ptr<control_grpc::ControlServer> server;
    std::shared_ptr<grpc::Channel> channel;

    explicit Server(const Options& options)
    {
        control::VariableInfo level;
        level.name = "level";
        level.type = control::ValueType::Real;
        level.causality = control::Causality::Input;
        hub.agent_connected(control::PeerHello{"Engine", "e1", {level}, 16}, link);
        const auto gone = hub.agent_connected(control::PeerHello{"Gone", "g1", {level}, 16}, std::make_shared<CountingLink>());
        hub.agent_disconnected(gone);

        control_grpc::ServerOptions o;
        o.listen = options.listen;
        o.external_hub = &hub;
        o.time = &time;
        o.monitor_period = 0ms;
        o.run = options.run;
        o.run_required = options.required;
        if (options.credentials)
        {
            o.security.credentials = {Credential{"alice", kViewer, Role::Viewer}, Credential{"carol", kAdmin, Role::Admin}};
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

    bool audited(const std::string& part)
    {
        for (const auto& line : lines())
        {
            if (line.find(part) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// Whether anything reached the hub's peers or the time.
    bool touched() const { return time.changes != 0 || time.stops != 0 || link->sent != 0; }
};

struct Answer
{
    grpc::Status status;
    std::string reason; ///< the trailing metadata vehicle-sil-reason ("" if none)
    std::string run;    ///< PingReply.run, for Ping
};

/// How a call is made: its token and the run it names (nullptr: none).
struct Caller
{
    const char* token = nullptr;
    const char* run = nullptr;
};

std::string reason_of(const grpc::ClientContext& context)
{
    const auto& trailing = context.GetServerTrailingMetadata();
    const auto found = trailing.find(control_grpc::kRefusalReasonKey);
    return found == trailing.end() ? std::string() : std::string(found->second.data(), found->second.size());
}

void prepare(grpc::ClientContext& context, const Caller& caller)
{
    context.set_deadline(std::chrono::system_clock::now() + unit::scaled_timeout(10s));
    if (caller.token != nullptr)
    {
        context.AddMetadata("authorization", std::string("Bearer ") + caller.token);
    }
    if (caller.run != nullptr)
    {
        context.AddMetadata(control_grpc::kRunKey, caller.run);
    }
}

using Call = std::function<Answer(const std::shared_ptr<grpc::Channel>&, const Caller&)>;

template <typename Request, typename Reply, typename F>
Call unary(F method, std::function<void(Request&)> fill = {})
{
    return [method, fill](const std::shared_ptr<grpc::Channel>& channel, const Caller& caller) {
        auto stub = v1::ControlService::NewStub(channel);
        grpc::ClientContext context;
        prepare(context, caller);
        Request request;
        if (fill)
        {
            fill(request);
        }
        Reply reply;
        Answer a;
        a.status = ((*stub).*method)(&context, request, &reply);
        a.reason = reason_of(context);
        if constexpr (std::is_same_v<Reply, v1::PingReply>)
        {
            a.run = reply.run();
        }
        return a;
    };
}

/// Every RPC of the service, with a request the server takes.
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
    m["WaitOperation"] = unary<v1::WaitOperationRequest, v1::Operation>(&S::WaitOperation, [](v1::WaitOperationRequest& r) { r.set_timeout_ms(100); });
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
    m["Watch"] = [](const std::shared_ptr<grpc::Channel>& channel, const Caller& caller) {
        auto stub = v1::ControlService::NewStub(channel);
        grpc::ClientContext context;
        prepare(context, caller);
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

bool refused_for_run(const Answer& a, const char* reason)
{
    return a.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION && a.reason == reason;
}

/// Not refused for the run (it may fail for its own reasons: no operation 0, for example).
bool not_refused_for_run(const Answer& a)
{
    return a.reason != control_grpc::kRunRequired && a.reason != control_grpc::kRunChanged;
}

} // namespace

TEST_CASE(control_server_run_table_covers_every_rpc)
{
    // A method added to the proto without a call here fails this test, so that every RPC is
    // checked for the run below.
    const auto* service = v1::PingRequest::descriptor()->file()->FindServiceByName("ControlService");
    CHECK(service != nullptr);
    const auto table = calls();
    for (int i = 0; i < service->method_count(); ++i)
    {
        CHECK(table.count(std::string(service->method(i)->name())) == 1);
    }
    CHECK_EQ(table.size(), static_cast<std::size_t>(service->method_count()));
}

TEST_CASE(control_server_run_required_refuses_every_call_without_the_run_but_ping)
{
    Server s({.run = "4f2a-1", .required = true});
    for (const auto& [method, call] : calls())
    {
        const Answer a = call(s.channel, Caller{});
        if (method == "Ping")
        {
            CHECK(a.status.ok());
            CHECK(a.run == std::string("4f2a-1"));
            continue;
        }
        if (!refused_for_run(a, control_grpc::kRunRequired))
        {
            throw unit::Failure(method + " without the run: " + std::to_string(a.status.error_code()) + " '" + a.reason + "'");
        }
        CHECK(a.status.error_message().find("send the metadata vehicle-sil-run") != std::string::npos);
    }
    CHECK(!s.touched());
}

TEST_CASE(control_server_run_refuses_every_call_for_another_run_but_ping)
{
    for (const bool required : {true, false})
    {
        Server s({.run = "4f2a-2", .required = required});
        for (const auto& [method, call] : calls())
        {
            const Answer a = call(s.channel, Caller{.run = "4f2a-1"});
            if (method == "Ping")
            {
                CHECK(a.status.ok());
                CHECK(a.run == std::string("4f2a-2"));
                continue;
            }
            if (!refused_for_run(a, control_grpc::kRunChanged))
            {
                throw unit::Failure(method + " for another run: " + std::to_string(a.status.error_code()) + " '" + a.reason + "'");
            }
            CHECK(a.status.error_message().find("the call is for the run '4f2a-1', and this control server belongs to the run '4f2a-2'") !=
                  std::string::npos);
        }
        CHECK(!s.touched());
    }
}

TEST_CASE(control_server_run_serves_the_calls_for_its_run)
{
    for (const bool required : {true, false})
    {
        Server s({.run = "4f2a-1", .required = required});
        for (const auto& [method, call] : calls())
        {
            const Answer a = call(s.channel, Caller{.run = "4f2a-1"});
            if (!not_refused_for_run(a))
            {
                throw unit::Failure(method + " for its run: refused (" + a.reason + ")");
            }
        }
        // Each changing call reached the hub or the time.
        CHECK_EQ(s.time.changes.load(), 5); // Pause, Resume, RunUntil, Step, SetSpeed
        CHECK_EQ(s.time.stops.load(), 1);
        CHECK(s.link->sent.load() >= 1); // Write
    }
}

TEST_CASE(control_server_run_optional_serves_calls_without_the_run)
{
    // run (not serve): a client that names no run is served, as before the runs had IDs.
    Server s({.run = "4f2a-1", .required = false});
    for (const auto& [method, call] : calls())
    {
        const Answer a = call(s.channel, Caller{});
        if (!not_refused_for_run(a))
        {
            throw unit::Failure(method + " without the run: refused (" + a.reason + ")");
        }
    }
    CHECK_EQ(s.time.stops.load(), 1);
}

TEST_CASE(control_server_without_a_run_refuses_a_call_that_names_one)
{
    // A client bound to a run never reaches a server that belongs to none.
    Server s({});
    const auto table = calls();
    const Answer ping = table.at("Ping")(s.channel, Caller{.run = "4f2a-1"});
    CHECK(ping.status.ok());
    CHECK(ping.run == std::string());
    CHECK(refused_for_run(table.at("Stop")(s.channel, Caller{.run = "4f2a-1"}), control_grpc::kRunChanged));
    CHECK(not_refused_for_run(table.at("Stop")(s.channel, Caller{})));
    CHECK_EQ(s.time.stops.load(), 1);
}

TEST_CASE(control_server_run_every_value_of_the_metadata_must_name_the_run)
{
    Server s({.run = "4f2a-1", .required = true});
    auto stub = v1::ControlService::NewStub(s.channel);
    grpc::ClientContext context;
    prepare(context, Caller{.run = "4f2a-1"});
    context.AddMetadata(control_grpc::kRunKey, "4f2a-0");
    v1::TimeState reply;
    const auto status = stub->Stop(&context, v1::StopRequest{}, &reply);
    CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK(reason_of(context) == std::string(control_grpc::kRunChanged));
    CHECK(!s.touched());
}

TEST_CASE(control_server_run_is_checked_after_the_authentication_and_the_role)
{
    Server s({.run = "4f2a-2", .required = true, .credentials = true});
    const auto table = calls();
    // No token, or a wrong one: UNAUTHENTICATED, whatever the run, with no reason; and Ping does
    // not tell the run either.
    for (const char* token : {static_cast<const char*>(nullptr), "wrong-token-0123456789"})
    {
        for (const char* run : {static_cast<const char*>(nullptr), "4f2a-1", "4f2a-2"})
        {
            for (const char* method : {"Ping", "GetTime", "Stop"})
            {
                const Answer a = table.at(method)(s.channel, Caller{token, run});
                CHECK(a.status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
                CHECK(a.reason == std::string());
                CHECK(a.run == std::string());
            }
        }
    }
    // A role that may not make the call: PERMISSION_DENIED, whatever the run.
    CHECK(table.at("Stop")(s.channel, Caller{kViewer, "4f2a-1"}).status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
    CHECK(table.at("Stop")(s.channel, Caller{kViewer, nullptr}).status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
    // One that may: the run decides.
    CHECK(refused_for_run(table.at("GetTime")(s.channel, Caller{kViewer, "4f2a-1"}), control_grpc::kRunChanged));
    CHECK(refused_for_run(table.at("Stop")(s.channel, Caller{kAdmin, "4f2a-1"}), control_grpc::kRunChanged));
    CHECK(refused_for_run(table.at("Stop")(s.channel, Caller{kAdmin, nullptr}), control_grpc::kRunRequired));
    const Answer ping = table.at("Ping")(s.channel, Caller{kViewer, "4f2a-1"});
    CHECK(ping.status.ok());
    CHECK(ping.run == std::string("4f2a-2"));
    CHECK(table.at("Stop")(s.channel, Caller{kAdmin, "4f2a-2"}).status.ok());
    CHECK_EQ(s.time.stops.load(), 1);
}

TEST_CASE(control_server_run_audit_lines_carry_the_run)
{
    Server s({.run = "4f2a-2", .required = true});
    const auto table = calls();
    CHECK(table.at("Pause")(s.channel, Caller{.run = "4f2a-2"}).status.ok());
    CHECK(refused_for_run(table.at("Resume")(s.channel, Caller{.run = "4f2a-1"}), control_grpc::kRunChanged));
    CHECK(refused_for_run(table.at("Resume")(s.channel, Caller{}), control_grpc::kRunRequired));
    const auto lines = s.lines();
    CHECK_EQ(lines.size(), static_cast<std::size_t>(3));
    for (const auto& line : lines)
    {
        CHECK(line.rfind("[run 4f2a-2] anonymous@", 0) == 0);
    }
    CHECK(s.audited(" pause: ok"));
    CHECK(s.audited(" resume: refused (run-changed, the call names run 4f2a-1)"));
    CHECK(s.audited(" resume: refused (run-required)"));

    // Without a run, the lines are as they were.
    Server plain({});
    CHECK(table.at("Pause")(plain.channel, Caller{}).status.ok());
    CHECK(plain.lines().size() == 1 && plain.lines()[0].rfind("anonymous@", 0) == 0);
}

TEST_CASE(control_server_run_options_are_checked)
{
    const auto make = [](std::string run, bool required) {
        control_grpc::ServerOptions o;
        o.run = std::move(run);
        o.run_required = required;
        return control_grpc::ControlServer(std::move(o));
    };
    CHECK(unit::throws_with<std::invalid_argument>([&] { make("", true); }, "run_required needs the run's ID"));
    CHECK(unit::throws_with<std::invalid_argument>([&] { make("a run", false); }, "printable ASCII without spaces"));
    CHECK(unit::throws_with<std::invalid_argument>([&] { make("line\nbreak", false); }, "printable ASCII without spaces"));
    CHECK(unit::throws_with<std::invalid_argument>([&] { make(std::string(129, 'a'), false); }, "at most 128 characters"));
    make(std::string(128, 'a'), true);
    make("0123456789abcdef-12", true);
}

TEST_CASE(control_server_run_an_earlier_runs_client_at_the_same_address_is_refused)
{
    // The server of run A, then that of run B at the same address (as two runs of one launcher
    // with a fixed --control address). One channel, made for A, stays: gRPC connects it again by
    // itself to B. Bound to A, it is refused there; it reached B (Ping says B's run).
    auto a = std::make_unique<Server>(Options{.run = "4f2a-1", .required = true});
    const std::string address = "127.0.0.1:" + std::to_string(a->server->port());
    const auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    const auto table = calls();
    CHECK(table.at("GetTime")(channel, Caller{.run = "4f2a-1"}).status.ok());
    a.reset(); // the run ends: its server stops listening

    Server b({.run = "4f2a-2", .required = true, .listen = address});
    Answer stop;
    CHECK(unit::eventually(
        [&] {
            stop = table.at("Stop")(channel, Caller{.run = "4f2a-1"});
            return stop.status.error_code() != grpc::StatusCode::UNAVAILABLE;
        },
        10s, 50ms));
    CHECK(refused_for_run(stop, control_grpc::kRunChanged));
    for (const char* method : {"Write", "Pause", "Step", "GetOperation", "Watch"})
    {
        CHECK(refused_for_run(table.at(method)(channel, Caller{.run = "4f2a-1"}), control_grpc::kRunChanged));
    }
    // Not bound: refused too where the run is required; Ping tells the new run.
    CHECK(refused_for_run(table.at("Stop")(channel, Caller{}), control_grpc::kRunRequired));
    const Answer ping = table.at("Ping")(channel, Caller{.run = "4f2a-1"});
    CHECK(ping.status.ok());
    CHECK(ping.run == std::string("4f2a-2"));
    CHECK(!b.touched());
    CHECK(b.audited("[run 4f2a-2] anonymous@"));
    CHECK(b.audited("stop: refused (run-changed, the call names run 4f2a-1)"));
}
