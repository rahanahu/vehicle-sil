// SPDX-License-Identifier: MIT
//
// What a Watch leaves out when it asks: the operations of the clients it names
// (WatchRequest.operation_clients_excluded). The hub is fed by the test.
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;

/// The next operation the watch sends (an empty update if the stream ends first).
v1::Update next_operation(grpc::ClientReader<v1::Update>& reader)
{
    v1::Update u;
    while (reader.Read(&u))
    {
        if (u.has_operation())
        {
            return u;
        }
    }
    return {};
}

} // namespace

TEST_CASE(control_server_a_watch_leaves_out_the_operations_of_the_clients_it_names)
{
    control_grpc::ControlServer server({.listen = "127.0.0.1:0"});
    const auto stub = v1::ControlService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
    // A watch of the operations, read past its initial updates.
    const auto open = [&](grpc::ClientContext& context, const std::vector<std::string>& excluded) {
        context.set_deadline(std::chrono::system_clock::now() + 30s);
        v1::WatchRequest request;
        request.set_operations(true);
        for (const auto& client : excluded)
        {
            request.add_operation_clients_excluded(client);
        }
        auto reader = stub->Watch(&context, request);
        v1::Update u;
        while (reader->Read(&u) && !u.has_initial_done())
        {
        }
        return reader;
    };
    grpc::ClientContext every_context;
    grpc::ClientContext without_context;
    auto every = open(every_context, {});
    auto without = open(without_context, {"scenario"});

    // The launcher's scenario writes into the hub as the client "scenario"; then a client over gRPC.
    control::OperationRequest scenario_write;
    scenario_write.writes = {{"Nobody", "x", 1.0}};
    scenario_write.client = "scenario";
    const auto by_scenario = server.hub().write(scenario_write);
    grpc::ClientContext write_context;
    v1::WriteRequest write;
    auto* w = write.add_writes();
    w->set_peer("Nobody");
    w->set_name("x");
    w->mutable_value()->set_d(1.0);
    v1::WriteReply written;
    CHECK(stub->Write(&write_context, write, &written).ok());

    const auto first = next_operation(*every);
    CHECK(first.operation().seq() == by_scenario && first.operation().client() == "scenario");
    CHECK(next_operation(*every).operation().seq() == written.seq());
    // The other one gets the client's alone.
    const auto only = next_operation(*without);
    CHECK(only.operation().seq() == written.seq());
    CHECK(only.operation().client() != "scenario");

    every_context.TryCancel();
    without_context.TryCancel();
    static_cast<void>(every->Finish());
    static_cast<void>(without->Finish());
}

TEST_CASE(control_server_shutdown_wakes_active_watches_promptly)
{
    // Active Watch streams block inside Subscription::poll(64, 100ms). Shutting the server down
    // must close those subscriptions so shutdown() returns well before the 100 ms poll timeout
    // instead of stalling every cycle.
    constexpr int kCycles = 5;
    std::chrono::steady_clock::duration total_shutdown{};
    for (int i = 0; i < kCycles; ++i)
    {
        control_grpc::ControlServer server({.listen = "127.0.0.1:0"});
        const auto stub = v1::ControlService::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
        grpc::ClientContext ctx1;
        grpc::ClientContext ctx2;
        ctx1.set_deadline(std::chrono::system_clock::now() + 30s);
        ctx2.set_deadline(std::chrono::system_clock::now() + 30s);
        v1::WatchRequest req;
        req.set_operations(true);
        auto r1 = stub->Watch(&ctx1, req);
        auto r2 = stub->Watch(&ctx2, req);
        v1::Update u;
        while (r1->Read(&u) && !u.has_initial_done()) {}
        while (r2->Read(&u) && !u.has_initial_done()) {}

        // Drain both streams concurrently while shutting down so neither side blocks on write buffers.
        std::thread drain1([&] {
            v1::Update next;
            while (r1->Read(&next)) {}
            static_cast<void>(r1->Finish());
        });
        std::thread drain2([&] {
            v1::Update next;
            while (r2->Read(&next)) {}
            static_cast<void>(r2->Finish());
        });

        const auto began = std::chrono::steady_clock::now();
        server.shutdown();
        total_shutdown += std::chrono::steady_clock::now() - began;

        drain1.join();
        drain2.join();
    }
    // Without waking the 100 ms Subscription::poll(), 5 cycles take ~500 ms minimum.
    CHECK(total_shutdown < vehicle_sil::test::scaled_timeout(300ms));
}

TEST_CASE(control_server_shutdown_concurrent_with_opening_watches_is_safe)
{
    // Opening a Watch while shutdown() is in progress must neither hang nor race under TSan/ASan.
    for (int i = 0; i < 5; ++i)
    {
        control_grpc::ControlServer server({.listen = "127.0.0.1:0"});
        const auto stub = v1::ControlService::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials()));
        std::thread opener([&] {
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + 10s);
            auto reader = stub->Watch(&ctx, v1::WatchRequest{});
            v1::Update u;
            while (reader->Read(&u)) {}
            static_cast<void>(reader->Finish());
        });
        std::this_thread::sleep_for(2ms);
        server.shutdown();
        opener.join();
    }
}
