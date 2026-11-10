// SPDX-License-Identifier: MIT
//
// The bounds and input checks of the control server: refused calls in the audit log, the size of
// a Write, the calls that wait, the agent's hello, a replaced agent connection, the options, and
// the time operations at the edges (the end of the simulation, the stop reason).
#include "convert.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/agent_client.hpp"
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
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline && !pred())
    {
        std::this_thread::sleep_for(10ms);
    }
    return pred();
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
};

/// An agent's stream, written by hand (a fake agent).
struct RawAgent
{
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<v1::AgentMessage, v1::ServerMessage>> stream;

    RawAgent(v1::AgentService::Stub& stub, const v1::Hello& hello)
    {
        context.set_deadline(std::chrono::system_clock::now() + 20s);
        stream = stub.Connect(&context);
        v1::AgentMessage m;
        *m.mutable_hello() = hello;
        stream->Write(m);
    }

    /// The next message, or nullopt when the stream ended.
    std::optional<v1::ServerMessage> read()
    {
        v1::ServerMessage m;
        if (!stream->Read(&m))
        {
            return std::nullopt;
        }
        return m;
    }
};

v1::Hello hello(const std::string& participant, const std::string& incarnation)
{
    v1::Hello h;
    h.set_participant(participant);
    h.set_incarnation(incarnation);
    h.set_max_pending(4);
    return h;
}

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

TEST_CASE(control_server_checks_the_agent_hello)
{
    AuditLines audit;
    control_grpc::ServerOptions o;
    o.audit = audit.sink();
    control_grpc::ControlServer server(std::move(o));
    auto stub = v1::AgentService::NewStub(channel(server));
    // More open_seqs than an agent with max_pending 4 can hold (4 queued and 4 recent results).
    auto h = hello("Engine", "e1");
    h.set_has_open_seqs(true);
    for (std::uint64_t i = 0; i < 9; ++i)
    {
        h.add_open_seqs(i);
    }
    RawAgent too_many(*stub, h);
    CHECK(!too_many.read().has_value());
    CHECK(too_many.stream->Finish().error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    // An invalid hello is in the audit log.
    CHECK(eventually([&] { return audit.count("agent Engine: refused (more open_seqs") == 1; }));
    RawAgent empty(*stub, hello("", "e1"));
    CHECK(!empty.read().has_value());
    CHECK(empty.stream->Finish().error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    CHECK(eventually([&] { return audit.count("refused (the first message must be a valid hello)") == 1; }));
    CHECK(server.hub().peers().empty());
    // Exactly as many as it can hold is fine.
    auto full = hello("Engine", "e1");
    full.set_has_open_seqs(true);
    for (std::uint64_t i = 0; i < 8; ++i)
    {
        full.add_open_seqs(i);
    }
    RawAgent fits(*stub, full);
    const auto welcome = fits.read();
    CHECK(welcome.has_value() && welcome->has_welcome());
    fits.context.TryCancel();
    static_cast<void>(fits.stream->Finish());
}

TEST_CASE(control_server_ends_a_replaced_agent_connection)
{
    // A second process with the same participant name (or the same process reconnecting) takes
    // over; the first connection is ended instead of being ignored silently.
    AuditLines audit;
    control_grpc::ServerOptions o;
    o.audit = audit.sink();
    control_grpc::ControlServer server(std::move(o));
    auto stub = v1::AgentService::NewStub(channel(server));
    RawAgent first(*stub, hello("Engine", "e1"));
    const auto welcome = first.read();
    CHECK(welcome.has_value() && welcome->has_welcome());
    RawAgent second(*stub, hello("Engine", "e2"));
    const auto welcome2 = second.read();
    CHECK(welcome2.has_value() && welcome2->has_welcome());
    CHECK(!first.read().has_value());
    CHECK(first.stream->Finish().error_code() == grpc::StatusCode::ABORTED);
    CHECK(eventually([&] { return audit.count("replaced by another connection") == 1; }));
    const auto peers = server.hub().peers();
    CHECK_EQ(peers.size(), std::size_t{1});
    CHECK(peers.at(0).connected);
    CHECK(peers.at(0).incarnation == "e2");
    second.context.TryCancel();
    static_cast<void>(second.stream->Finish());
}

TEST_CASE(control_server_a_second_agent_with_one_name_wins_and_the_first_stops)
{
    // Two live processes claim the same participant (a configuration error): the newer
    // connection wins, and the older agent stops reconnecting and says why, instead of the two
    // taking the participant from each other again and again.
    AuditLines audit;
    control_grpc::ServerOptions o;
    o.audit = audit.sink();
    control_grpc::ControlServer server(std::move(o));
    const auto address = "127.0.0.1:" + std::to_string(server.port());
    control::Agent a;
    a.bind_driver();
    control::Agent b;
    b.bind_driver();
    control_grpc::AgentClient first(a, {.server = address, .participant = "Engine", .incarnation = "a",
                                        .reconnect_min = 20ms, .reconnect_max = 100ms});
    CHECK(first.wait_connected(10s));
    control_grpc::AgentClient second(b, {.server = address, .participant = "Engine", .incarnation = "b",
                                         .reconnect_min = 20ms, .reconnect_max = 100ms});
    CHECK(eventually([&] { return first.replaced(); }));
    CHECK(first.last_error().find("replaced by another connection of the participant 'Engine' (incarnation 'b'") != std::string::npos);
    CHECK(first.last_error().find("only one process runs as the participant 'Engine'") != std::string::npos);
    CHECK(second.wait_connected(10s));
    // Several of the first one's back-offs later: it has not taken the participant back.
    std::this_thread::sleep_for(500ms);
    CHECK(!first.connected());
    CHECK(!second.replaced() && second.connected());
    CHECK_EQ(second.sessions(), std::uint64_t{1});
    CHECK_EQ(audit.count("replaced by another connection"), std::size_t{1});
    second.stop();
    first.stop();
}

TEST_CASE(control_server_an_agent_replaced_by_its_own_connection_goes_on)
{
    // The server still holds an agent's old stream when the agent's newer connection arrives
    // (a lost link the server has not noticed). Seen from the agent, the replacing connection is
    // its own: it reconnects instead of stopping.
    control_grpc::ControlServer server(control_grpc::ServerOptions{});
    const auto address = "127.0.0.1:" + std::to_string(server.port());
    control::Agent a;
    a.bind_driver();
    control_grpc::AgentClient agent(a, {.server = address, .participant = "Engine", .incarnation = "a",
                                        .reconnect_min = 20ms, .reconnect_max = 100ms});
    CHECK(agent.wait_connected(10s));
    auto stub = v1::AgentService::NewStub(channel(server));
    auto own = hello("Engine", "a");
    own.set_connection(agent.connection_id());
    RawAgent same(*stub, own);
    CHECK(eventually([&] { return agent.sessions() >= 2 && agent.connected(); }));
    CHECK(!agent.replaced());
    agent.stop();
    same.context.TryCancel();
    static_cast<void>(same.stream->Finish());
}

TEST_CASE(control_server_names_the_replacing_connection_and_tells_the_watches)
{
    AuditLines audit;
    control_grpc::ServerOptions o;
    o.audit = audit.sink();
    control_grpc::ControlServer server(std::move(o));
    auto clients = v1::ControlService::NewStub(channel(server));
    grpc::ClientContext watch_context;
    v1::WatchRequest request;
    request.set_events(true);
    auto watch = clients->Watch(&watch_context, request);
    v1::Update u;
    while (watch->Read(&u) && !u.initial_done())
    {
    }

    auto stub = v1::AgentService::NewStub(channel(server));
    auto h1 = hello("Engine", "e1");
    h1.set_connection("c1");
    RawAgent first(*stub, h1);
    CHECK(first.read().has_value()); // welcome
    auto h2 = hello("Engine", "e2");
    h2.set_connection("c2");
    RawAgent second(*stub, h2);
    CHECK(second.read().has_value());
    CHECK(!first.read().has_value());
    const auto status = first.stream->Finish();
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK(status.error_message().find("participant 'Engine' (incarnation 'e2'") != std::string::npos);
    const auto& trailing = first.context.GetServerTrailingMetadata();
    const auto by = trailing.find("vehicle-sil-replaced-by");
    CHECK(by != trailing.end() && std::string(by->second.data(), by->second.size()) == "c2");
    CHECK(eventually([&] { return audit.count("agent Engine (e1): replaced by another connection") == 1; }));

    // The watch sees a warning of Engine's new connection.
    bool warned = false;
    while (!warned && watch->Read(&u))
    {
        warned = u.has_event() && u.event().peer() == "Engine" && u.event().incarnation() == "e2"
                 && u.event().event().level() == v1::EVENT_LEVEL_WARNING
                 && u.event().event().text().find("replaced the connection of 'Engine' (incarnation 'e1'") != std::string::npos;
    }
    CHECK(warned);
    watch_context.TryCancel();
    second.context.TryCancel();
    static_cast<void>(second.stream->Finish());
}

TEST_CASE(control_server_refuses_a_connection_identifier_it_cannot_send_back)
{
    // The identifier goes back in the metadata of the connection it replaces, where gRPC aborts
    // the server on a byte it does not allow: a hello with such an identifier is refused, and
    // the server goes on.
    AuditLines audit;
    control_grpc::ServerOptions o;
    o.audit = audit.sink();
    control_grpc::ControlServer server(std::move(o));
    auto stub = v1::AgentService::NewStub(channel(server));
    auto clients = v1::ControlService::NewStub(channel(server));
    const auto ping = [&] {
        grpc::ClientContext context;
        v1::PingReply reply;
        return clients->Ping(&context, v1::PingRequest{}, &reply).ok();
    };
    auto h1 = hello("Engine", "e1");
    h1.set_connection("c1");
    RawAgent first(*stub, h1);
    CHECK(first.read().has_value()); // welcome

    struct Bad
    {
        std::string connection;
        std::string says;
    };
    const std::vector<Bad> bad = {
        {"\xe6\x8e\xa5\xe7\xb6\x9a\n1", "(it has 8 bytes, 7 of them outside A-Z a-z 0-9 . _ -)"}, // non-ASCII and a line break
        {std::string(65, 'a'), "(it has 65 bytes, 0 of them outside"},
        {"a b", "(it has 3 bytes, 1 of them outside"},
    };
    for (const auto& b : bad)
    {
        auto h = hello("Engine", "e2");
        h.set_connection(b.connection);
        RawAgent agent(*stub, h);
        CHECK(!agent.read().has_value());
        const auto status = agent.stream->Finish();
        CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        CHECK(status.error_message().find("the hello's connection identifier is not allowed " + b.says) != std::string::npos);
        CHECK(status.error_message().find("send 1 to 64 of those characters") != std::string::npos);
        CHECK(ping());
    }
    CHECK(eventually([&] { return audit.count("agent Engine: refused (the hello's connection identifier is not allowed") == 3; }));
    CHECK_EQ(audit.count("\xe6\x8e\xa5"), std::size_t{0}); // the value itself is not logged
    // The first connection was not replaced.
    auto peers = server.hub().peers();
    CHECK(peers.size() == 1 && peers.at(0).connected && peers.at(0).incarnation == "e1");

    // The longest identifier allowed, of every allowed kind of character, replaces it.
    const std::string longest = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-";
    CHECK_EQ(longest.size(), std::size_t{65});
    auto h2 = hello("Engine", "e2");
    h2.set_connection(longest.substr(1));
    RawAgent second(*stub, h2);
    CHECK(second.read().has_value());
    CHECK(!first.read().has_value());
    CHECK(first.stream->Finish().error_code() == grpc::StatusCode::ABORTED);
    const auto& trailing = first.context.GetServerTrailingMetadata();
    const auto by = trailing.find("vehicle-sil-replaced-by");
    CHECK(by != trailing.end() && std::string(by->second.data(), by->second.size()) == longest.substr(1));
    CHECK(ping());
    second.context.TryCancel();
    static_cast<void>(second.stream->Finish());
}

TEST_CASE(control_server_an_agent_whose_names_are_not_ascii_reconnects_over_its_old_stream)
{
    // A participant name and an incarnation in Japanese with a tab. The server still holds the
    // agent's old stream when the agent reconnects (a lost link it has not noticed); it ends that
    // stream with the new connection's identifier, which the agent made of characters gRPC
    // allows in metadata.
    control_grpc::ControlServer server(control_grpc::ServerOptions{});
    const auto address = "127.0.0.1:" + std::to_string(server.port());
    const std::string participant = "\xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xb8\xe3\x83\xb3";   // エンジン
    const std::string incarnation = "\xe8\xa9\xa6\xe4\xbd\x9c\t1";                       // 試作<tab>1
    control::Agent a;
    a.bind_driver();
    control_grpc::AgentClient agent(a, {.server = address, .participant = participant, .incarnation = incarnation,
                                        .reconnect_min = 20ms, .reconnect_max = 100ms});
    CHECK(agent.wait_connected(10s));
    CHECK(control_grpc::detail::valid_connection_id(agent.connection_id()));
    // The stand-in for the old stream: same participant and incarnation. It takes over the
    // agent's connection (with the agent's own identifier, so the agent reconnects), and the
    // agent's new connection then replaces it.
    auto stub = v1::AgentService::NewStub(channel(server));
    auto own = hello(participant, incarnation);
    own.set_connection(agent.connection_id());
    RawAgent old_stream(*stub, own);
    CHECK(eventually([&] { return agent.sessions() >= 2 && agent.connected(); }));
    auto message = old_stream.read();
    if (message.has_value())
    {
        CHECK(message->has_welcome()); // when it took over
        message = old_stream.read();
    }
    CHECK(!message.has_value());
    const auto status = old_stream.stream->Finish();
    CHECK(status.error_code() == grpc::StatusCode::ABORTED);
    CHECK(status.error_message().find("participant '" + participant + "' (incarnation '" + incarnation + "'") != std::string::npos);
    const auto& trailing = old_stream.context.GetServerTrailingMetadata();
    const auto by = trailing.find("vehicle-sil-replaced-by");
    CHECK(by != trailing.end() && std::string(by->second.data(), by->second.size()) == agent.connection_id());
    CHECK(!agent.replaced());
    CHECK(agent.connected());
    const auto peers = server.hub().peers();
    CHECK(peers.size() == 1 && peers.at(0).participant == participant && peers.at(0).connected
          && peers.at(0).incarnation == incarnation);
    agent.stop();
}

TEST_CASE(control_server_an_agent_replaced_by_a_client_without_an_identifier_stops_and_says_so)
{
    // A client that sends no connection identifier (an older one) replaces this agent: the
    // server still says so (the metadata, empty), and the agent stops and tells, once.
    control_grpc::ControlServer server(control_grpc::ServerOptions{});
    const auto address = "127.0.0.1:" + std::to_string(server.port());
    std::mutex mutex;
    std::vector<std::string> reasons;
    control::Agent a;
    a.bind_driver();
    control_grpc::AgentClient agent(a, {.server = address, .participant = "Engine", .incarnation = "a", .reconnect_min = 20ms,
                                        .reconnect_max = 100ms, .on_replaced = [&](const std::string& why) {
                                            const std::lock_guard lock(mutex);
                                            reasons.push_back(why);
                                        }});
    CHECK(agent.wait_connected(10s));
    auto stub = v1::AgentService::NewStub(channel(server));
    RawAgent other(*stub, hello("Engine", "b"));
    CHECK(other.read().has_value()); // welcome
    CHECK(eventually([&] {
        const std::lock_guard lock(mutex);
        return !reasons.empty();
    }));
    CHECK(agent.replaced());
    agent.stop();
    const std::lock_guard lock(mutex);
    CHECK_EQ(reasons.size(), std::size_t{1});
    CHECK(reasons.at(0) == agent.last_error());
    CHECK(reasons.at(0).find("replaced by another connection of the participant 'Engine' (incarnation 'b'") != std::string::npos);
    CHECK(reasons.at(0).find("this agent no longer reconnects") != std::string::npos);
    other.context.TryCancel();
    static_cast<void>(other.stream->Finish());
}

namespace {

/// An agent service that ends the first connection with ABORTED and no metadata (as something
/// between the agent and the server might), and welcomes the ones after it.
class AbortsFirst final : public v1::AgentService::Service
{
public:
    grpc::Status Connect(grpc::ServerContext*, grpc::ServerReaderWriter<v1::ServerMessage, v1::AgentMessage>* stream) override
    {
        v1::AgentMessage m;
        if (!stream->Read(&m))
        {
            return grpc::Status::CANCELLED;
        }
        if (connects_++ == 0)
        {
            return {grpc::StatusCode::ABORTED, "aborted on the way"};
        }
        v1::ServerMessage welcome;
        welcome.mutable_welcome();
        stream->Write(welcome);
        while (stream->Read(&m))
        {
        }
        return grpc::Status::OK;
    }

private:
    std::atomic<int> connects_{0};
};

} // namespace

TEST_CASE(control_server_an_agent_ended_with_aborted_and_no_identifier_reconnects)
{
    // Only the server's word that another connection replaced it stops an agent; an ABORTED
    // without it is a lost connection.
    AbortsFirst service;
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    const auto server = builder.BuildAndStart();
    CHECK(server != nullptr && port != 0);
    {
        control::Agent a;
        a.bind_driver();
        control_grpc::AgentClient agent(a, {.server = "127.0.0.1:" + std::to_string(port), .participant = "Engine",
                                            .reconnect_min = 20ms, .reconnect_max = 100ms});
        CHECK(agent.wait_connected(10s));
        CHECK(!agent.replaced());
        CHECK_EQ(agent.sessions(), std::uint64_t{1});
        agent.stop();
    }
    server->Shutdown();
}

TEST_CASE(control_server_agent_client_refuses_a_token_it_cannot_send)
{
    // gRPC would abort the agent's process when it puts such a token into the metadata.
    control_grpc::ControlServer server(control_grpc::ServerOptions{});
    const auto address = "127.0.0.1:" + std::to_string(server.port());
    control::Agent a;
    a.bind_driver();
    CHECK(vehicle_sil::unit::throws_if<std::invalid_argument>(
        [&] { control_grpc::AgentClient client(a, {.server = address, .participant = "Engine", .token = "agent-token\n"}); },
        [](const std::invalid_argument& e) {
            return std::string(e.what()).find("the token has a character outside printable ASCII (0x20 to 0x7E) at byte 11")
                   != std::string::npos;
        }));
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
    CHECK(throws<std::invalid_argument>(with({Credential{"a", "viewer-token-0123456789", Role::Viewer, {"Engine"}}})));
    // An agent credential names the participants it may connect as, or "*".
    CHECK(throws<std::invalid_argument>(with({Credential{"ecu", "agent-token-0123456789", Role::Agent}})));
    CHECK(!throws<std::invalid_argument>(with({Credential{"ecu", "agent-token-0123456789", Role::Agent, {"*"}}})));
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
