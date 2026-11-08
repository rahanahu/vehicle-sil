// SPDX-License-Identifier: MIT
//
// Authentication, roles, TLS and the audit log of the control server.
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/agent_client.hpp"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef VEHICLE_SIL_TEST_DATA_DIR
#error VEHICLE_SIL_TEST_DATA_DIR must name tests/control_server/testdata
#endif

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
using control_grpc::Credential;
using control_grpc::Role;

std::string read(const std::string& name)
{
    std::ifstream in(std::string(VEHICLE_SIL_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

const std::string kViewer = "viewer-token-0123456789";
const std::string kOperator = "operator-token-0123456789";
const std::string kAdmin = "admin-token-0123456789";
const std::string kAgent = "agent-token-0123456789";

struct SecureServer
{
    std::mutex mutex;
    std::vector<std::string> audit;
    std::unique_ptr<control_grpc::ControlServer> server;
    std::string address;

    SecureServer()
    {
        control_grpc::ServerOptions o;
        o.listen = "127.0.0.1:0";
        o.security.certificate_chain_pem = read("server.crt");
        o.security.private_key_pem = read("server.key");
        o.security.credentials = {
            Credential{"alice", kViewer, Role::Viewer},
            Credential{"bob", kOperator, Role::Operator},
            Credential{"carol", kAdmin, Role::Admin},
            Credential{"engine-ecu", kAgent, Role::Agent, {"Engine"}},
        };
        o.audit = [this](const std::string& line) {
            const std::lock_guard lock(mutex);
            audit.push_back(line);
        };
        server = std::make_unique<control_grpc::ControlServer>(std::move(o));
        address = "127.0.0.1:" + std::to_string(server->port());
    }

    std::unique_ptr<v1::ControlService::Stub> client(bool tls = true) const
    {
        grpc::SslCredentialsOptions ssl;
        ssl.pem_root_certs = read("server.crt");
        return v1::ControlService::NewStub(
            grpc::CreateChannel(address, tls ? grpc::SslCredentials(ssl) : grpc::InsecureChannelCredentials()));
    }

    bool audited(const std::string& part)
    {
        const std::lock_guard lock(mutex);
        for (const auto& line : audit)
        {
            if (line.find(part) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

grpc::StatusCode call_list(v1::ControlService::Stub& stub, const std::string& token)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    if (!token.empty())
    {
        context.AddMetadata("authorization", "Bearer " + token);
    }
    v1::ListPeersReply reply;
    return stub.ListPeers(&context, v1::ListPeersRequest{}, &reply).error_code();
}

grpc::StatusCode call_write(v1::ControlService::Stub& stub, const std::string& token)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    context.AddMetadata("authorization", "Bearer " + token);
    v1::WriteRequest request;
    auto* w = request.add_writes();
    w->set_peer("Nobody");
    w->set_name("x");
    w->mutable_value()->set_d(1.0);
    v1::WriteReply reply;
    return stub.Write(&context, request, &reply).error_code();
}

grpc::StatusCode call_stop(v1::ControlService::Stub& stub, const std::string& token)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    context.AddMetadata("authorization", "Bearer " + token);
    v1::TimeState reply;
    return stub.Stop(&context, v1::StopRequest{}, &reply).error_code();
}

template <typename Pred>
bool eventually(Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline && !pred())
    {
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

} // namespace

TEST_CASE(control_server_roles_decide_what_a_client_may_do)
{
    SecureServer s;
    auto stub = s.client();
    CHECK(call_list(*stub, "") == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(call_list(*stub, "wrong-token-0123456789") == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(call_list(*stub, kViewer) == grpc::StatusCode::OK);
    CHECK(call_list(*stub, kAgent) == grpc::StatusCode::PERMISSION_DENIED); // an agent is no client
    CHECK(call_write(*stub, kViewer) == grpc::StatusCode::PERMISSION_DENIED);
    CHECK(call_write(*stub, kOperator) == grpc::StatusCode::OK);
    CHECK(call_stop(*stub, kOperator) == grpc::StatusCode::PERMISSION_DENIED);
    // Admin passes the check; this server does not control the time.
    CHECK(call_stop(*stub, kAdmin) == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK(s.audited("alice@"));
    CHECK(s.audited("write: refused"));
    CHECK(s.audited("bob@"));
    CHECK(s.audited("write Nobody/x: seq"));
    CHECK(s.audited("carol@"));
}

TEST_CASE(control_server_needs_tls_when_it_has_it)
{
    SecureServer s;
    auto plain = s.client(false);
    CHECK(call_list(*plain, kViewer) == grpc::StatusCode::UNAVAILABLE);
}

TEST_CASE(control_server_agents_need_an_agent_token_for_their_name)
{
    SecureServer s;
    const auto roots = read("server.crt");
    control::Agent engine;
    double x = 0.0;
    engine.input("x", &x);
    engine.bind_driver();
    control_grpc::AgentClient ok(engine, {.server = s.address, .participant = "Engine", .token = kAgent, .root_certificates_pem = roots});
    CHECK(ok.wait_connected(10s));
    CHECK(eventually([&] { return s.server->hub().peers().size() == 1; }));
    CHECK(s.audited("engine-ecu@"));
    CHECK(s.audited("agent Engine"));

    // The same agent token for another participant, an operator token, and no token: refused.
    control::Agent other;
    other.bind_driver();
    control_grpc::AgentClient wrong_name(other, {.server = s.address, .participant = "Body", .token = kAgent,
                                                 .root_certificates_pem = roots});
    control::Agent other2;
    other2.bind_driver();
    control_grpc::AgentClient wrong_role(other2, {.server = s.address, .participant = "Engine2", .token = kOperator,
                                                  .root_certificates_pem = roots});
    control::Agent other3;
    other3.bind_driver();
    control_grpc::AgentClient no_token(other3, {.server = s.address, .participant = "Engine3", .root_certificates_pem = roots});
    CHECK(eventually([&] { return s.audited("agent Body: refused") && s.audited("bob@") && s.audited("unauthenticated@"); }));
    CHECK(!wrong_name.connected());
    CHECK(eventually([&] { return !wrong_name.last_error().empty(); })); // the server's refusal reaches the agent
    std::this_thread::sleep_for(200ms);
    CHECK_EQ(s.server->hub().peers().size(), std::size_t{1});
    for (auto* c : {&ok, &wrong_name, &wrong_role, &no_token})
    {
        c->stop();
    }
}

TEST_CASE(control_server_refuses_an_open_address_without_security)
{
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { control_grpc::ControlServer s({.listen = "0.0.0.0:0"}); }));
    control_grpc::ServerOptions o;
    o.listen = "0.0.0.0:0";
    o.security.allow_insecure = true;
    control_grpc::ControlServer allowed(std::move(o));
    CHECK(allowed.port() > 0);
}

TEST_CASE(control_server_reads_a_credentials_file)
{
    const std::string path = (std::filesystem::temp_directory_path() / ("vsil_credentials_" + std::to_string(std::random_device{}()))).string();
    {
        std::ofstream out(path);
        out << "# role name token participants\n"
               "viewer  alice  aaaaaaaaaaaaaaaaaaaa\n"
               "agent   ecu    bbbbbbbbbbbbbbbbbbbb  Engine,Body   # two names\n"
               "\n";
    }
    const auto credentials = control_grpc::load_credentials(path);
    CHECK_EQ(credentials.size(), std::size_t{2});
    CHECK(credentials[0].role == Role::Viewer);
    CHECK(credentials[1].role == Role::Agent);
    CHECK((credentials[1].participants == std::vector<std::string>{"Engine", "Body"}));
    {
        std::ofstream out(path);
        out << "agent ecu bbbbbbbbbbbbbbbbbbbb Engine, Body # a comma and a space\n"
               "viewer joe tok#en-with-a-hash-0123\n";
    }
    const auto spaced = control_grpc::load_credentials(path);
    CHECK((spaced[0].participants == std::vector<std::string>{"Engine", "Body"}));
    CHECK(spaced[1].token == "tok#en-with-a-hash-0123"); // '#' inside a word is part of it
    {
        std::ofstream out(path);
        out << "viewer a dddddddddddddddddddd\noperator b dddddddddddddddddddd\n"; // the same token twice
    }
    CHECK(vehicle_sil::unit::throws<std::runtime_error>([&] { static_cast<void>(control_grpc::load_credentials(path)); }));
    {
        std::ofstream out(path);
        out << "viewer a eeeeeeeeeeeeeeeeeeee Engine\n"; // participants only for agents
    }
    CHECK(vehicle_sil::unit::throws<std::runtime_error>([&] { static_cast<void>(control_grpc::load_credentials(path)); }));
    {
        std::ofstream out(path);
        out << "superuser bob cccccccccccccccccccc\n";
    }
    CHECK(vehicle_sil::unit::throws<std::runtime_error>([&] { static_cast<void>(control_grpc::load_credentials(path)); }));
    {
        std::ofstream out(path);
        out << "viewer bob short\n";
    }
    CHECK(vehicle_sil::unit::throws<std::runtime_error>([&] { static_cast<void>(control_grpc::load_credentials(path)); }));
    std::remove(path.c_str());
}

TEST_CASE(control_server_refused_agents_back_off)
{
    // A refused agent waits reconnect_max before it tries again (not reconnect_min).
    SecureServer s;
    control::Agent agent;
    agent.bind_driver();
    control_grpc::AgentClient refused(agent, {.server = s.address, .participant = "Engine", .reconnect_min = 20ms,
                                              .reconnect_max = 1s, .token = kViewer, .root_certificates_pem = read("server.crt")});
    std::this_thread::sleep_for(2500ms);
    refused.stop();
    int refusals = 0;
    {
        const std::lock_guard lock(s.mutex);
        for (const auto& line : s.audit)
        {
            refusals += line.find("agent: refused") != std::string::npos ? 1 : 0;
        }
    }
    CHECK(refusals >= 1);
    CHECK(refusals <= 3); // with reconnect_min it would be about 100
}

TEST_CASE(control_server_forget_peer_needs_an_operator)
{
    SecureServer s;
    auto stub = s.client();
    const auto forget = [&](const std::string& token) {
        grpc::ClientContext context;
        context.AddMetadata("authorization", "Bearer " + token);
        v1::ForgetPeerRequest request;
        request.set_peer("Nobody");
        v1::ForgetPeerReply reply;
        return stub->ForgetPeer(&context, request, &reply).error_code();
    };
    CHECK(forget(kViewer) == grpc::StatusCode::PERMISSION_DENIED);
    CHECK(forget(kOperator) == grpc::StatusCode::FAILED_PRECONDITION); // no such disconnected peer
    // "bearer" in any case.
    grpc::ClientContext context;
    context.AddMetadata("authorization", "bEaReR " + kViewer);
    v1::ListPeersReply reply;
    CHECK(stub->ListPeers(&context, v1::ListPeersRequest{}, &reply).ok());
}

TEST_CASE(control_server_loopback_names)
{
    control_grpc::ControlServer local({.listen = "localhost:0"});
    CHECK(local.port() > 0);
    // Only numeric 127.x.y.z, localhost and [::1] are the loopback.
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { control_grpc::ControlServer s({.listen = "[::]:0"}); }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>(
        [] { control_grpc::ControlServer s({.listen = "127.0.0.1.example.com:0"}); }));
}
