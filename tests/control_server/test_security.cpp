// SPDX-License-Identifier: MIT
//
// Authentication, roles, TLS and the audit log of the control server.
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
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
#include <vector>

#ifndef _WIN32
#include <sys/un.h>
#endif

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

} // namespace

TEST_CASE(control_server_roles_decide_what_a_client_may_do)
{
    SecureServer s;
    auto stub = s.client();
    CHECK(call_list(*stub, "") == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(call_list(*stub, "wrong-token-0123456789") == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(call_list(*stub, kViewer) == grpc::StatusCode::OK);
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

TEST_CASE(control_server_refuses_an_open_address_without_security)
{
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { control_grpc::ControlServer s({.listen = "0.0.0.0:0"}); }));
    // allow_insecure lets an address other than the loopback through the check. Shown without
    // opening a port to the network (Windows's firewall would ask): 192.0.2.1 (TEST-NET-1) is on no
    // interface here, so the server gets past the check and then cannot bind (std::runtime_error,
    // not the check's std::invalid_argument).
    CHECK(vehicle_sil::unit::throws_with<std::runtime_error>(
        [] {
            control_grpc::ServerOptions o;
            o.listen = "192.0.2.1:0";
            o.security.allow_insecure = true;
            control_grpc::ControlServer allowed(std::move(o));
        },
        "cannot listen on 192.0.2.1:0"));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { control_grpc::ControlServer s({.listen = "192.0.2.1:0"}); }));
}

TEST_CASE(control_server_says_when_the_path_of_a_unix_socket_is_too_long)
{
    using control_grpc::unix_socket_path_problem;
    CHECK(unix_socket_path_problem("127.0.0.1:0").empty());
    CHECK(unix_socket_path_problem("unix:/tmp/control.sock").empty());
#ifdef _WIN32
    CHECK(unix_socket_path_problem("unix:/" + std::string(500, 'a')).empty());
#else
    // 107 bytes on Linux: one less than the address holds. Both ways of writing the address count the path only.
    const std::size_t most = sizeof(sockaddr_un{}.sun_path) - 1;
    const std::string fits = "/tmp/" + std::string(most - 5, 'a');
    const std::string longer = fits + "b";
    CHECK(unix_socket_path_problem("unix:" + fits).empty());
    CHECK(unix_socket_path_problem("unix://" + fits).empty());
    const std::string problem = "the path of the socket is " + std::to_string(most + 1) + " bytes long and this system takes " +
                                std::to_string(most) + " at most: give a shorter path, for example unix:/tmp/vehicle-sil-control.sock";
    CHECK(unix_socket_path_problem("unix:" + longer) == problem);
    CHECK(unix_socket_path_problem("unix://" + longer) == problem);
    // A %XX is the one byte it stands for (a space here); a '%' that starts no such triplet is itself.
    const std::string spaced = "/tmp/a%20" + std::string(most - 7, 'a');
    CHECK(unix_socket_path_problem("unix:" + spaced).empty());
    CHECK(unix_socket_path_problem("unix:" + spaced + "b") == problem);
    CHECK(unix_socket_path_problem("unix:/tmp/%2" + std::string(most - 7, 'g')).empty());
    CHECK(unix_socket_path_problem("unix:/tmp/%2" + std::string(most - 6, 'g')) == problem);
    CHECK(unix_socket_path_problem("unix:/tmp/" + std::string(most - 7, 'a') + "%4").empty());
    // The server says it, and the longest path that passes is one it can listen on.
    CHECK(vehicle_sil::unit::throws_with<std::runtime_error>(
        [&] { control_grpc::ControlServer s({.listen = "unix:" + longer}); },
        "vehicle_sil control server: cannot listen on unix:" + longer + ": " + problem));
    const std::string dir = (std::filesystem::temp_directory_path() / ("vsil_" + std::to_string(std::random_device{}()))).string();
    if (dir.size() + 2 <= most)
    {
        std::filesystem::create_directory(dir);
        const std::string socket = dir + "/" + std::string(most - dir.size() - 1, 's');
        {
            control_grpc::ControlServer s({.listen = "unix:" + socket});
            CHECK(std::filesystem::exists(socket));
        }
        std::filesystem::remove_all(dir);
    }
#endif
}

TEST_CASE(control_server_reads_a_credentials_file)
{
    const std::string path = (std::filesystem::temp_directory_path() / ("vsil_credentials_" + std::to_string(std::random_device{}()))).string();
    {
        std::ofstream out(path);
        out << "# role name token\n"
               "viewer  alice  aaaaaaaaaaaaaaaaaaaa\n"
               "admin   carol  bbbbbbbbbbbbbbbbbbbb   # the lead\n"
               "\n";
    }
    const auto credentials = control_grpc::load_credentials(path);
    CHECK_EQ(credentials.size(), std::size_t{2});
    CHECK(credentials[0].role == Role::Viewer && credentials[0].name == "alice");
    CHECK(credentials[1].role == Role::Admin && credentials[1].token == "bbbbbbbbbbbbbbbbbbbb");
    {
        std::ofstream out(path);
        out << "viewer joe tok#en-with-a-hash-0123\n";
    }
    CHECK(control_grpc::load_credentials(path).at(0).token == "tok#en-with-a-hash-0123"); // '#' inside a word is part of it
    const auto error_of = [](const std::string& text) {
        try
        {
            static_cast<void>(control_grpc::parse_credentials(text, "creds.txt"));
        }
        catch (const std::runtime_error& e)
        {
            return std::string(e.what());
        }
        return std::string();
    };
    // The same token twice, a word after the token, too few words, no role, a short token.
    CHECK(error_of("viewer a dddddddddddddddddddd\noperator b dddddddddddddddddddd\n").find("creds.txt:2: the token is used") == 0);
    CHECK(error_of("viewer a eeeeeeeeeeeeeeeeeeee Engine\n").find("creds.txt:1: expected <role> <name> <token>") == 0);
    CHECK(error_of("viewer bob\n").find("creds.txt:1: expected <role> <name> <token>") == 0);
    CHECK(error_of("superuser bob cccccccccccccccccccc\n").find("the roles are viewer, operator and admin") != std::string::npos);
    CHECK(error_of("viewer bob short\n").find("at least 16 characters") != std::string::npos);
    // An agent line of the time when the agents connected with a token: say that it goes.
    CHECK(error_of("viewer a aaaaaaaaaaaaaaaaaaaa\nagent ecu ffffffffffffffffffff Engine\n") ==
          "creds.txt:2: the role agent is no longer used: the ECUs' control agents reach the server over SIL Kit, without a "
          "token; delete this line");
    // No word of a line is repeated in a message: a line in the wrong order has its token first.
    const std::string token = "a-secret-token-0123456789";
    for (const std::string& text : {token + " bob viewer\n", "agent " + token + " " + token + "x\n",
                                    "viewer bob " + token + " " + token + "\n", "viewer bob " + token + "\nviewer alice " + token + "\n"})
    {
        const std::string message = error_of(text);
        CHECK(!message.empty() && message.rfind("creds.txt:", 0) == 0);
        CHECK(message.find(token) == std::string::npos);
    }
    std::remove(path.c_str());
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
