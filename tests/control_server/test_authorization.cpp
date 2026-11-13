// SPDX-License-Identifier: MIT
//
// Who may call what: every RPC of the service, called by every role, by an unknown token and
// without a token, and whether the call leaves a line in the audit log. The expected table is
// written out here, apart from the server's code, so that a change of the server's rules shows
// up as a failure.
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
using control_grpc::Credential;
using control_grpc::Role;

/// Who calls.
enum class Caller
{
    None,     ///< no token
    Unknown,  ///< a token the server does not know
    Viewer,
    Operator,
    Admin,
    Anyone, ///< a server without credentials: no token, and every call is allowed
};

const char* caller_name(Caller c)
{
    switch (c)
    {
    case Caller::None:
        return "no token";
    case Caller::Unknown:
        return "unknown token";
    case Caller::Viewer:
        return "viewer";
    case Caller::Operator:
        return "operator";
    case Caller::Admin:
        return "admin";
    case Caller::Anyone:
        return "server without credentials";
    }
    return "?";
}

std::optional<std::string> token_of(Caller c)
{
    switch (c)
    {
    case Caller::Unknown:
        return "unknown-token-0123456789";
    case Caller::Viewer:
        return "viewer-token-0123456789";
    case Caller::Operator:
        return "operator-token-0123456789";
    case Caller::Admin:
        return "admin-token-0123456789";
    default:
        return std::nullopt;
    }
}

/// The least role a method needs, and whether an allowed call leaves an audit line. A refused
/// call always leaves one.
struct Expected
{
    Role need;
    bool audited;
};

const std::map<std::string, Expected>& expected()
{
    static const std::map<std::string, Expected> table = {
        {"/vehicle_sil.control.v1.ControlService/Ping", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/ListPeers", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/GetValues", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/Write", {Role::Operator, true}},
        {"/vehicle_sil.control.v1.ControlService/GetOperation", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/WaitOperation", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/Watch", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/ForgetPeer", {Role::Operator, true}},
        {"/vehicle_sil.control.v1.ControlService/GetTime", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/GetSnapshot", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/Pause", {Role::Operator, true}},
        {"/vehicle_sil.control.v1.ControlService/Resume", {Role::Operator, true}},
        {"/vehicle_sil.control.v1.ControlService/RunUntil", {Role::Operator, true}},
        {"/vehicle_sil.control.v1.ControlService/Step", {Role::Operator, true}},
        {"/vehicle_sil.control.v1.ControlService/SetSpeed", {Role::Operator, true}},
        {"/vehicle_sil.control.v1.ControlService/WaitPaused", {Role::Viewer, false}},
        {"/vehicle_sil.control.v1.ControlService/Stop", {Role::Admin, true}},
    };
    return table;
}

bool may(Caller c, Role need)
{
    switch (c)
    {
    case Caller::Anyone:
        return true;
    case Caller::None:
    case Caller::Unknown:
        return false;
    case Caller::Viewer:
        return need == Role::Viewer;
    case Caller::Operator:
        return need == Role::Viewer || need == Role::Operator;
    case Caller::Admin:
        return true;
    }
    return false;
}

struct Server
{
    std::mutex mutex;
    std::vector<std::string> audit;
    std::unique_ptr<control_grpc::ControlServer> server;
    std::shared_ptr<grpc::Channel> channel;

    explicit Server(bool credentials)
    {
        control_grpc::ServerOptions o;
        if (credentials)
        {
            o.security.credentials = {
                Credential{"alice", *token_of(Caller::Viewer), Role::Viewer},
                Credential{"bob", *token_of(Caller::Operator), Role::Operator},
                Credential{"carol", *token_of(Caller::Admin), Role::Admin},
            };
        }
        o.audit = [this](const std::string& line) {
            const std::lock_guard lock(mutex);
            audit.push_back(line);
        };
        server = std::make_unique<control_grpc::ControlServer>(std::move(o));
        channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(server->port()), grpc::InsecureChannelCredentials());
    }

    std::size_t lines()
    {
        const std::lock_guard lock(mutex);
        return audit.size();
    }
};

void prepare(grpc::ClientContext& context, Caller c)
{
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    if (const auto token = token_of(c))
    {
        context.AddMetadata("authorization", "Bearer " + *token);
    }
}

/// Calls every method once with an empty (or harmless) request; returns the status by method.
using Call = std::function<grpc::Status(grpc::Channel&, Caller)>;

template <typename Request, typename Reply, typename F>
Call unary(F method)
{
    return [method](grpc::Channel& channel, Caller c) {
        auto stub = v1::ControlService::NewStub(std::shared_ptr<grpc::Channel>(&channel, [](grpc::Channel*) {}));
        grpc::ClientContext context;
        prepare(context, c);
        Request request;
        Reply reply;
        return ((*stub).*method)(&context, request, &reply);
    };
}

std::map<std::string, Call> calls()
{
    using S = v1::ControlService::Stub;
    const std::string p = "/vehicle_sil.control.v1.ControlService/";
    std::map<std::string, Call> m;
    m[p + "Ping"] = unary<v1::PingRequest, v1::PingReply>(&S::Ping);
    m[p + "ListPeers"] = unary<v1::ListPeersRequest, v1::ListPeersReply>(&S::ListPeers);
    m[p + "GetValues"] = unary<v1::GetValuesRequest, v1::GetValuesReply>(&S::GetValues);
    m[p + "Write"] = unary<v1::WriteRequest, v1::WriteReply>(&S::Write);
    m[p + "GetOperation"] = unary<v1::GetOperationRequest, v1::Operation>(&S::GetOperation);
    m[p + "WaitOperation"] = unary<v1::WaitOperationRequest, v1::Operation>(&S::WaitOperation);
    m[p + "ForgetPeer"] = unary<v1::ForgetPeerRequest, v1::ForgetPeerReply>(&S::ForgetPeer);
    m[p + "GetTime"] = unary<v1::GetTimeRequest, v1::TimeState>(&S::GetTime);
    m[p + "GetSnapshot"] = unary<v1::GetSnapshotRequest, v1::GetSnapshotReply>(&S::GetSnapshot);
    m[p + "Pause"] = unary<v1::PauseRequest, v1::TimeState>(&S::Pause);
    m[p + "Resume"] = unary<v1::ResumeRequest, v1::TimeState>(&S::Resume);
    m[p + "RunUntil"] = unary<v1::RunUntilRequest, v1::TimeState>(&S::RunUntil);
    m[p + "Step"] = unary<v1::StepRequest, v1::TimeState>(&S::Step);
    m[p + "SetSpeed"] = unary<v1::SetSpeedRequest, v1::TimeState>(&S::SetSpeed);
    m[p + "WaitPaused"] = unary<v1::WaitPausedRequest, v1::TimeState>(&S::WaitPaused);
    m[p + "Stop"] = unary<v1::StopRequest, v1::TimeState>(&S::Stop);
    m[p + "Watch"] = [](grpc::Channel& channel, Caller c) {
        auto stub = v1::ControlService::NewStub(std::shared_ptr<grpc::Channel>(&channel, [](grpc::Channel*) {}));
        grpc::ClientContext context;
        prepare(context, c);
        auto reader = stub->Watch(&context, v1::WatchRequest{});
        v1::Update u;
        if (reader->Read(&u))
        {
            context.TryCancel(); // allowed: it streams
            reader->Finish();
            return grpc::Status::OK;
        }
        return reader->Finish();
    };
    return m;
}

std::vector<std::string> methods_of_the_services()
{
    std::vector<std::string> out;
    const auto* file = v1::PingRequest::descriptor()->file();
    // ControlService only: a service added to the proto needs a table of its own here.
    CHECK_EQ(file->service_count(), 1);
    for (const auto* service : {file->FindServiceByName("ControlService")})
    {
        for (int i = 0; i < service->method_count(); ++i)
        {
            out.push_back("/" + std::string(service->full_name()) + "/" + std::string(service->method(i)->name()));
        }
    }
    return out;
}

bool refused(const grpc::Status& s)
{
    return s.error_code() == grpc::StatusCode::UNAUTHENTICATED || s.error_code() == grpc::StatusCode::PERMISSION_DENIED;
}

} // namespace

TEST_CASE(control_server_authorization_table_covers_every_rpc)
{
    // A method added to the proto without a row here fails this test.
    const auto all = methods_of_the_services();
    const auto table = calls();
    for (const auto& method : all)
    {
        if (expected().count(method) == 0 || table.count(method) == 0)
        {
            std::fprintf(stderr, "no expected rule or call for %s\n", method.c_str());
        }
        CHECK(expected().count(method) == 1);
        CHECK(table.count(method) == 1);
    }
    CHECK_EQ(expected().size(), all.size());
}

TEST_CASE(control_server_allows_and_audits_every_rpc_by_role)
{
    const auto table = calls();
    for (const Caller c : {Caller::None, Caller::Unknown, Caller::Viewer, Caller::Operator, Caller::Admin, Caller::Anyone})
    {
        // A server per caller: at most 16 refused calls, below the audit's thinning (20 a second).
        Server server(c != Caller::Anyone);
        for (const auto& [method, call] : table)
        {
            const auto& rule = expected().at(method);
            const auto before = server.lines();
            const auto status = call(*server.channel, c);
            const bool allowed = !refused(status);
            const bool audited = server.lines() > before;
            const bool want_allowed = may(c, rule.need);
            const bool want_audited = !want_allowed || rule.audited;
            if (allowed != want_allowed || audited != want_audited)
            {
                std::fprintf(stderr, "%s by %s: allowed %d (expected %d), audited %d (expected %d), status %d %s\n",
                             method.c_str(), caller_name(c), allowed, want_allowed, audited, want_audited,
                             static_cast<int>(status.error_code()), status.error_message().c_str());
            }
            CHECK(allowed == want_allowed);
            CHECK(audited == want_audited);
            if (!allowed)
            {
                // Unauthenticated callers are told a token is needed, others that the role is not enough.
                const bool unauthenticated = c == Caller::None || c == Caller::Unknown;
                CHECK(status.error_code()
                      == (unauthenticated ? grpc::StatusCode::UNAUTHENTICATED : grpc::StatusCode::PERMISSION_DENIED));
            }
        }
    }
}
