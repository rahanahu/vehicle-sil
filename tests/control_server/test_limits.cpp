// SPDX-License-Identifier: MIT
//
// The bounds and input checks of the control server: refused calls in the audit log, the size of
// a Write, the calls that wait, the options, and the time operations at the edges (the end of the
// simulation, the stop reason).
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
using control_grpc::Credential;
using control_grpc::Role;
using vehicle_sil::unit::throws;

struct AuditLines
{
    std::mutex mutex;
    std::vector<std::string> lines;

    std::function<void(const std::string&)> sink()
    {
        return [this](const std::string& line) {
            const std::lock_guard lock(mutex);
            lines.push_back(line);
        };
    }

    std::size_t count(const std::string& part)
    {
        const std::lock_guard lock(mutex);
        std::size_t n = 0;
        for (const auto& line : lines)
        {
            n += line.find(part) != std::string::npos ? 1 : 0;
        }
        return n;
    }

    std::size_t longest()
    {
        const std::lock_guard lock(mutex);
        std::size_t n = 0;
        for (const auto& line : lines)
        {
            n = std::max(n, line.size());
        }
        return n;
    }
};

std::shared_ptr<grpc::Channel> channel(const control_grpc::ControlServer& server)
{
    return grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials());
}

template <typename Pred>
bool eventually(Pred pred)
{
    return vehicle_sil::unit::eventually(pred, 10s, 10ms);
}

/// A time control that records the stop and reports the simulation as over.
class EndedTime final : public control_grpc::TimeControl
{
public:
    std::mutex mutex;
    std::string stop_reason;

    [[nodiscard]] control_grpc::TimeView state() const override
    {
        control_grpc::TimeView v;
        v.available = true;
        v.ended = true;
        return v;
    }
    void pause(std::optional<SimTime>) override {}
    void resume() override {}
    void run_until(SimTime) override {}
    void step(SimDuration) override {}
    void set_speed(std::optional<double>) override {}
    std::optional<SimTime> wait_paused(std::chrono::milliseconds) override { return std::nullopt; } // over: at once
    void stop(const std::string& reason) override
    {
        const std::lock_guard lock(mutex);
        stop_reason = reason;
    }
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> participants() const override { return {}; }
    [[nodiscard]] std::optional<LeftParticipant> left_participant() const override { return std::nullopt; }
};

} // namespace

TEST_CASE(control_server_thins_out_refused_calls_in_the_audit)
{
    // Anyone who reaches the port can make refused calls: they cannot flood the audit log.
    AuditLines audit;
    control_grpc::ServerOptions o;
    o.security.credentials = {Credential{"alice", "viewer-token-0123456789", Role::Viewer},
                              Credential{"bob", "operator-token-0123456789", Role::Operator}};
    o.audit = audit.sink();
    control_grpc::ControlServer server(std::move(o));
    auto stub = v1::ControlService::NewStub(channel(server));
    const auto list = [&] {
        grpc::ClientContext context;
        v1::ListPeersReply reply;
        return stub->ListPeers(&context, v1::ListPeersRequest{}, &reply).error_code();
    };
    for (int i = 0; i < 200; ++i)
    {
        CHECK(list() == grpc::StatusCode::UNAUTHENTICATED);
    }
    const auto logged = audit.count("list peers: refused");
    CHECK(logged >= 1);
    CHECK(logged < 100); // 20 per second
    // A refusal after the second is over reports how many were left out.
    std::this_thread::sleep_for(1100ms);
    CHECK(list() == grpc::StatusCode::UNAUTHENTICATED);
    CHECK(audit.count("more refused calls not logged") >= 1);

    // A long name from a client does not make a long line.
    grpc::ClientContext context;
    context.AddMetadata("authorization", "Bearer operator-token-0123456789");
    v1::WriteRequest request;
    auto* w = request.add_writes();
    w->set_peer(std::string(5000, 'P'));
    w->set_name("x");
    w->mutable_value()->set_d(1.0);
    v1::WriteReply reply;
    CHECK(stub->Write(&context, request, &reply).ok());
    CHECK(audit.count("bob@") == 1);
    CHECK(audit.longest() < 1100);
}

TEST_CASE(control_server_bounds_writes_and_waiting_calls)
{
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .max_waiting_calls = 2, .max_writes_per_request = 4});
    auto stub = v1::ControlService::NewStub(channel(server));
    const auto write = [&](int n) {
        grpc::ClientContext context;
        v1::WriteRequest request;
        for (int i = 0; i < n; ++i)
        {
            auto* w = request.add_writes();
            w->set_peer("Nobody");
            w->set_name("x" + std::to_string(i));
            w->mutable_value()->set_d(1.0);
        }
        v1::WriteReply reply;
        return stub->Write(&context, request, &reply).error_code();
    };
    CHECK(write(4) == grpc::StatusCode::OK);
    CHECK(write(5) == grpc::StatusCode::INVALID_ARGUMENT);

    // Two watches hold the two places for calls that wait.
    const auto watch = [&](grpc::ClientContext& context) {
        v1::WatchRequest request;
        request.set_values(true);
        auto reader = stub->Watch(&context, request);
        v1::Update first;
        static_cast<void>(reader->Read(&first)); // the initial state: the watch is running
        return reader;
    };
    grpc::ClientContext c1;
    grpc::ClientContext c2;
    auto w1 = watch(c1);
    auto w2 = watch(c2);
    const auto wait_operation = [&] {
        grpc::ClientContext context;
        v1::WaitOperationRequest request;
        request.set_seq(999999999);
        request.set_timeout_ms(100);
        v1::Operation reply;
        return stub->WaitOperation(&context, request, &reply).error_code();
    };
    CHECK(wait_operation() == grpc::StatusCode::RESOURCE_EXHAUSTED);
    {
        grpc::ClientContext c3;
        v1::WatchRequest request;
        auto reader = stub->Watch(&c3, request);
        v1::Update u;
        while (reader->Read(&u))
        {
        }
        CHECK(reader->Finish().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
    }
    // A watch that ends gives its place back.
    c1.TryCancel();
    static_cast<void>(w1->Finish());
    CHECK(eventually([&] { return wait_operation() == grpc::StatusCode::NOT_FOUND; }));
    c2.TryCancel();
    static_cast<void>(w2->Finish());
}

TEST_CASE(control_server_cuts_a_stop_reason_between_characters)
{
    EndedTime time;
    control_grpc::ControlServer server({.listen = "127.0.0.1:0", .time = &time});
    auto stub = v1::ControlService::NewStub(channel(server));
    grpc::ClientContext context;
    v1::StopRequest request;
    request.set_reason(std::string(199, 'x') + "\xc3\xa9" + "tail"); // a 2-byte character across byte 200
    v1::TimeState reply;
    CHECK(stub->Stop(&context, request, &reply).ok());
    const std::lock_guard lock(time.mutex);
    CHECK(time.stop_reason == std::string(199, 'x'));
}

TEST_CASE(control_server_checks_its_options)
{
    // Credentials given directly get the checks of load_credentials().
    const auto with = [](std::vector<Credential> credentials) {
        return [credentials] {
            control_grpc::ServerOptions o;
            o.security.credentials = credentials;
            control_grpc::ControlServer s(std::move(o));
        };
    };
    CHECK(throws<std::invalid_argument>(with({Credential{"a", "short", Role::Viewer}})));
    CHECK(throws<std::invalid_argument>(
        with({Credential{"a", "same-token-0123456789", Role::Viewer}, Credential{"b", "same-token-0123456789", Role::Admin}})));
    // A value that is no role (3 was the agents' role once) is refused, not taken for an admin.
    CHECK(vehicle_sil::unit::throws_if<std::invalid_argument>(
        with({Credential{"ecu", "agent-token-0123456789", static_cast<Role>(3)}}), [](const std::invalid_argument& e) {
            return std::string(e.what()).find("the credential of ecu has the role value 3, which is no role") != std::string::npos;
        }));
    CHECK(throws<std::invalid_argument>(with({Credential{"x", "minus-token-0123456789", static_cast<Role>(-1)}})));
    // A certificate without its key is an error, not a silent plain-text server.
    CHECK(throws<std::invalid_argument>([] {
        control_grpc::ServerOptions o;
        o.security.certificate_chain_pem = "-----BEGIN CERTIFICATE-----";
        control_grpc::ControlServer s(std::move(o));
    }));
    CHECK(throws<std::invalid_argument>([] { control_grpc::ControlServer s({.listen = "127.0.0.1:0", .max_waiting_calls = 0}); }));
    CHECK(throws<std::invalid_argument>([] { control_grpc::ControlServer s({.listen = "127.0.0.1:0", .max_writes_per_request = 0}); }));
}

TEST_CASE(control_server_time_operations_at_the_edges)
{
    AuditLines audit;
    EndedTime time;
    control_grpc::ServerOptions o;
    o.time = &time;
    o.audit = audit.sink();
    control_grpc::ControlServer server(std::move(o));
    auto stub = v1::ControlService::NewStub(channel(server));
    // Waiting for a pause after the end answers at once instead of spinning until the timeout.
    {
        grpc::ClientContext context;
        v1::WaitPausedRequest request;
        request.set_timeout_ms(10000);
        v1::TimeState reply;
        const auto begin = std::chrono::steady_clock::now();
        CHECK(stub->WaitPaused(&context, request, &reply).error_code() == grpc::StatusCode::FAILED_PRECONDITION);
        CHECK(std::chrono::steady_clock::now() - begin < 2s);
    }
    // The stop reason goes to every participant: cleaned, bounded, and in the audit log.
    {
        grpc::ClientContext context;
        v1::StopRequest request;
        request.set_reason("line one\nline two" + std::string(1000, 'x'));
        v1::TimeState reply;
        CHECK(stub->Stop(&context, request, &reply).ok());
        const std::lock_guard lock(time.mutex);
        CHECK(time.stop_reason.size() <= 200);
        CHECK(time.stop_reason.find('\n') == std::string::npos);
        CHECK(time.stop_reason.rfind("line one?line two", 0) == 0);
    }
    CHECK(audit.count("stop (line one?line two") == 1);
}
