// SPDX-License-Identifier: MIT
//
// The gRPC toolchain of the control server: generated code, an in-process server on a local port,
// a client call.
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"

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
