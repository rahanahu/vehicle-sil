// SPDX-License-Identifier: MIT
//
// The gRPC toolchain of the control server: generated code, an in-process server on a local port,
// a client call. Then the control server's own Ping: the release it was built from, which the
// Python client compares with its own.
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/ecu/describe.hpp"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace {

namespace v1 = vehicle_sil::control::v1;

class PingService final : public v1::ControlService::Service
{
    grpc::Status Ping(grpc::ServerContext*, const v1::PingRequest* request, v1::PingReply* reply) override
    {
        reply->set_text("pong:" + request->text());
        reply->set_server_version("test");
        return grpc::Status::OK;
    }
};

} // namespace

TEST_CASE(control_server_grpc_ping_round_trip)
{
    PingService service;
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    const auto server = builder.BuildAndStart();
    CHECK(server != nullptr);
    CHECK(port > 0);

    const auto stub = v1::ControlService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    for (int i = 0; i < 3; ++i)
    {
        grpc::ClientContext context;
        v1::PingRequest request;
        request.set_text("hello" + std::to_string(i));
        v1::PingReply reply;
        const auto status = stub->Ping(&context, request, &reply);
        CHECK(status.ok());
        CHECK(reply.text() == "pong:hello" + std::to_string(i));
        CHECK(reply.server_version() == "test");
    }
    server->Shutdown();
}

TEST_CASE(control_server_ping_names_the_release)
{
    vehicle_sil::control_grpc::ControlServer server({.listen = "127.0.0.1:0"});
    const auto stub = v1::ControlService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    v1::PingRequest request;
    request.set_text("hello");
    v1::PingReply reply;
    const auto status = stub->Ping(&context, request, &reply);
    CHECK(status.ok());
    CHECK(reply.text() == "hello");
    CHECK(reply.server_version() == "vehicle-sil control v1");
    CHECK(!reply.vehicle_sil().empty());
    CHECK(reply.vehicle_sil() == vehicle_sil::version());
}
