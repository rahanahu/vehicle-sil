// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_grpc/agent_client.hpp"

#include "convert.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace vehicle_sil::control_grpc {
namespace {

using namespace detail;

std::string make_incarnation()
{
    std::random_device random;
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return std::to_string(now) + "-" + std::to_string(random());
}

/// The first part of this client's connection identifiers: 16 random hexadecimal digits. Not
/// made from the incarnation, which may hold any characters (the server accepts only
/// A-Z a-z 0-9 . _ - there).
std::string make_connection_prefix()
{
    std::random_device random;
    const std::uint64_t bits = (static_cast<std::uint64_t>(random()) << 32U) ^ random();
    std::string hex(16, '0');
    for (std::size_t i = 0; i < hex.size(); ++i)
    {
        hex[hex.size() - 1 - i] = "0123456789abcdef"[(bits >> (4U * i)) & 0xFU];
    }
    return hex;
}

} // namespace

struct AgentClient::Impl
{
    control::Agent& agent;
    /// Held until this Impl goes, after its thread was joined: the agent's ECU checks it.
    control::TransportLease lease;
    AgentClientOptions options;
    std::string incarnation;
    std::shared_ptr<grpc::Channel> channel;

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool stopping = false;
    bool connected = false;
    grpc::ClientContext* current_context = nullptr; // the current session's, for stop()
    bool read_done = false;
    bool welcomed = false;
    std::uint64_t sessions = 0; ///< connections the server welcomed
    std::string last_error;
    bool replaced = false;              ///< another process took the participant over: no more reconnects
    std::string connection_id;          ///< the current (or last) connection's
    std::string connection_prefix = make_connection_prefix();
    std::deque<std::string> own_connections; ///< this client's recent connection identifiers
    std::uint64_t connection_count = 0;
    std::deque<v1::AgentMessage> outbound;  // acknowledgements from the reader

    // Transport bookkeeping (guarded by mutex).
    std::set<std::uint64_t> queued;          // queued by the agent; result not seen yet
    std::deque<control::CommandResult> recent; // the latest results, sent again after a reconnect
    bool had_session = false;

    std::thread thread;

    Impl(control::Agent& a, AgentClientOptions o) : agent(a), lease(a), options(std::move(o))
    {
        incarnation = options.incarnation.empty() ? make_incarnation() : options.incarnation;
        if (options.publish_period <= std::chrono::milliseconds::zero() || options.reconnect_min <= std::chrono::milliseconds::zero()
            || options.reconnect_max < options.reconnect_min)
        {
            throw std::invalid_argument("AgentClientOptions: publish_period and reconnect_min must be > 0, reconnect_max >= reconnect_min");
        }
        // The token goes into metadata, where gRPC aborts the process on a byte it refuses.
        const auto bad = std::find_if(options.token.begin(), options.token.end(),
                                      [](char c) { return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E; });
        if (bad != options.token.end())
        {
            throw std::invalid_argument("AgentClientOptions: the token has a character outside printable ASCII (0x20 to 0x7E) at byte "
                                        + std::to_string(bad - options.token.begin())
                                        + "; use the token exactly as written in the server's credentials, without a line break");
        }
        std::shared_ptr<grpc::ChannelCredentials> credentials;
        if (options.tls || !options.root_certificates_pem.empty())
        {
            grpc::SslCredentialsOptions ssl;
            ssl.pem_root_certs = options.root_certificates_pem; // empty: the system's roots
            credentials = grpc::SslCredentials(ssl);
        }
        else
        {
            credentials = grpc::InsecureChannelCredentials();
        }
        grpc::ChannelArguments arguments;
        // Keepalive: find a link that went silent within seconds, also while nothing flows.
        arguments.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
        arguments.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
        arguments.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        arguments.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        channel = grpc::CreateCustomChannel(options.server, credentials, arguments);
        // The schema goes to the server as it is now: complete it first (an ECU's agent gets the
        // outputs its driver adds, and no more variables after this).
        agent.complete_schema();
        thread = std::thread([this] { run(); });
    }

    void run()
    {
        const auto stub = v1::AgentService::NewStub(channel);
        auto delay = options.reconnect_min;
        while (true)
        {
            {
                const std::lock_guard lock(mutex);
                if (stopping)
                {
                    return;
                }
            }
            const auto outcome = session(*stub);
            if (outcome == Outcome::Replaced)
            {
                // Said once, so that whoever runs this process sees it stopped.
                std::string why;
                {
                    const std::lock_guard lock(mutex);
                    why = last_error;
                }
                if (options.on_replaced)
                {
                    options.on_replaced(why);
                }
                else
                {
                    std::cerr << "vehicle_sil::control_grpc: " << why << std::endl;
                }
                return;
            }
            if (outcome == Outcome::Welcomed)
            {
                delay = options.reconnect_min;
            }
            else if (outcome == Outcome::Refused)
            {
                delay = options.reconnect_max; // the server said no: do not hammer it
            }
            std::unique_lock lock(mutex);
            cv.wait_for(lock, delay, [&] { return stopping; });
            delay = std::min(options.reconnect_max, delay * 2);
        }
    }

    enum class Outcome
    {
        Failed,   ///< no connection (the server is not there, the link broke before the welcome)
        Refused,  ///< the server refused the agent (credentials, hello)
        Welcomed, ///< it was up
        Replaced, ///< another process's connection of the same participant replaced it
    };

    /// One connection.
    Outcome session(v1::AgentService::Stub& stub)
    {
        grpc::ClientContext context;
        if (!options.token.empty())
        {
            context.AddMetadata("authorization", "Bearer " + options.token);
        }
        {
            const std::lock_guard lock(mutex);
            if (stopping)
            {
                return Outcome::Failed;
            }
            current_context = &context;
            read_done = false;
            welcomed = false;
            outbound.clear();
        }
        auto stream = stub.Connect(&context);

        v1::AgentMessage hello;
        std::vector<control::CommandResult> resend;
        {
            auto* h = hello.mutable_hello();
            h->set_participant(options.participant);
            h->set_incarnation(incarnation);
            for (const auto& v : agent.schema())
            {
                detail::to_proto(v, *h->add_schema());
            }
            h->set_max_pending(agent.options().max_pending);
            const std::lock_guard lock(mutex);
            connection_id = connection_prefix + "-" + std::to_string(++connection_count);
            own_connections.push_back(connection_id);
            if (own_connections.size() > 64)
            {
                own_connections.pop_front();
            }
            h->set_connection(connection_id);
            if (had_session)
            {
                // What the agent still holds: queued without a result, and results that may not
                // have reached the server (sent again below).
                h->set_has_open_seqs(true);
                std::set<std::uint64_t> open = queued;
                for (const auto& r : recent)
                {
                    open.insert(r.seq);
                }
                for (const auto seq : open)
                {
                    h->add_open_seqs(seq);
                }
            }
            resend.assign(recent.begin(), recent.end());
        }
        bool up = stream->Write(hello);
        for (const auto& r : resend)
        {
            v1::AgentMessage m;
            detail::to_proto(r, *m.mutable_result());
            up = up && stream->Write(m);
        }
        std::thread reader;
        if (up)
        {
            // The reader marks the connection up when the server's welcome arrives.
            reader = std::thread([&] { read(*stream); });
            publish(*stream);
        }
        bool was_welcomed = false;
        bool server_ended = false;
        {
            const std::lock_guard lock(mutex);
            was_welcomed = welcomed;
            server_ended = read_done || !up;
            connected = false;
            current_context = nullptr;
        }
        cv.notify_all();
        if (!server_ended && stopping_now())
        {
            // We end it: after WritesDone() the server finishes the stream; give it a moment to
            // read the last messages, then cancel. (A failed write needs no cancel: the call is
            // over, the reader returns, and Finish() tells why, a refusal for instance.)
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return read_done; }))
            {
                lock.unlock();
                context.TryCancel();
            }
        }
        if (reader.joinable())
        {
            reader.join();
        }
        const auto status = stream->Finish();
        const auto& trailing = context.GetServerTrailingMetadata();
        const auto by = trailing.find(std::string(kReplacedBy));
        if (status.error_code() == grpc::StatusCode::ABORTED && by != trailing.end())
        {
            // The server says another connection of the same participant replaced this one. If
            // it is not one of this client's own (a second process with this name, or a client
            // that sends no identifier), the newer one wins: stop instead of taking the
            // participant back again and again. Without the metadata, ABORTED may come from
            // anything between (a proxy): a lost connection, reconnected below.
            const std::lock_guard lock(mutex);
            const bool own = std::find(own_connections.begin(), own_connections.end(), std::string(by->second.data(), by->second.size()))
                             != own_connections.end();
            last_error = status.error_message();
            if (own)
            {
                return Outcome::Failed; // replaced by its own newer connection: reconnect as usual
            }
            replaced = true;
            last_error += "; this agent no longer reconnects: check that only one process runs as the participant '"
                          + options.participant + "', then start this one again";
            return Outcome::Replaced;
        }
        const bool refused = status.error_code() == grpc::StatusCode::UNAUTHENTICATED
                             || status.error_code() == grpc::StatusCode::PERMISSION_DENIED
                             || status.error_code() == grpc::StatusCode::INVALID_ARGUMENT;
        {
            const std::lock_guard lock(mutex);
            if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED)
            {
                last_error = status.error_message();
            }
        }
        return was_welcomed ? Outcome::Welcomed : (refused ? Outcome::Refused : Outcome::Failed);
    }

    bool stopping_now()
    {
        const std::lock_guard lock(mutex);
        return stopping;
    }

    void read(grpc::ClientReaderWriter<v1::AgentMessage, v1::ServerMessage>& stream)
    {
        v1::ServerMessage message;
        while (stream.Read(&message))
        {
            if (message.has_welcome())
            {
                {
                    const std::lock_guard lock(mutex);
                    welcomed = true;
                    connected = true;
                    had_session = true;
                    ++sessions;
                    last_error.clear();
                }
                cv.notify_all();
                continue;
            }
            if (!message.has_command())
            {
                continue;
            }
            control::Command command;
            v1::AgentMessage ack;
            auto* s = ack.mutable_submitted();
            s->set_seq(message.command().seq());
            if (!detail::from_proto(message.command(), command))
            {
                s->set_status(v1::SUBMIT_STATUS_INVALID);
                s->set_reason("a write without a value");
            }
            else
            {
                const auto seq = command.seq;
                {
                    // Recorded before submit(): its result may be drained (and erased from
                    // `queued`) by the publisher before submit() even returns.
                    const std::lock_guard lock(mutex);
                    queued.insert(seq);
                }
                const auto result = agent.submit(std::move(command));
                s->set_status(result.status == control::SubmitStatus::Queued ? v1::SUBMIT_STATUS_QUEUED
                              : result.status == control::SubmitStatus::Busy ? v1::SUBMIT_STATUS_BUSY
                                                                            : v1::SUBMIT_STATUS_INVALID);
                s->set_reason(result.reason);
                if (result.status != control::SubmitStatus::Queued)
                {
                    const std::lock_guard lock(mutex);
                    queued.erase(seq);
                }
            }
            {
                const std::lock_guard lock(mutex);
                outbound.push_back(std::move(ack));
            }
            cv.notify_all();
        }
        {
            const std::lock_guard lock(mutex);
            read_done = true;
        }
        cv.notify_all();
    }

    void publish(grpc::ClientReaderWriter<v1::AgentMessage, v1::ServerMessage>& stream)
    {
        std::uint64_t sent_sequence = 0;
        std::optional<control::PeerStatus> sent_status;
        const std::size_t keep = agent.options().max_pending;
        bool last = false;
        while (!last)
        {
            std::deque<v1::AgentMessage> acks;
            {
                std::unique_lock lock(mutex);
                cv.wait_for(lock, options.publish_period, [&] { return stopping || read_done || !outbound.empty(); });
                if (read_done)
                {
                    return;
                }
                last = stopping; // one more pass to send what is left (results of finish(), last values)
                acks.swap(outbound);
            }
            for (const auto& m : acks)
            {
                if (!stream.Write(m))
                {
                    return;
                }
            }
            std::vector<control::CommandResult> results;
            agent.drain_results(results);
            {
                // All of them into `recent` before any is written: one that is not written now is
                // sent again after a reconnect.
                const std::lock_guard lock(mutex);
                for (const auto& r : results)
                {
                    queued.erase(r.seq);
                    recent.push_back(r);
                }
                while (recent.size() > keep)
                {
                    recent.pop_front();
                }
            }
            for (const auto& r : results)
            {
                v1::AgentMessage m;
                detail::to_proto(r, *m.mutable_result());
                if (!stream.Write(m))
                {
                    return;
                }
            }
            const auto snapshot = agent.latest();
            if (snapshot.sequence != 0 && snapshot.sequence != sent_sequence)
            {
                v1::AgentMessage m;
                detail::to_proto(snapshot, *m.mutable_values());
                if (!stream.Write(m))
                {
                    return;
                }
                sent_sequence = snapshot.sequence;
            }
            std::vector<control::Event> events;
            agent.drain_events(events);
            for (const auto& e : events)
            {
                v1::AgentMessage m;
                detail::to_proto(e, *m.mutable_event());
                if (!stream.Write(m))
                {
                    return;
                }
            }
            if (options.status)
            {
                const auto status = options.status();
                if (!sent_status || sent_status->failed != status.failed || sent_status->error != status.error)
                {
                    v1::AgentMessage m;
                    m.mutable_status()->set_failed(status.failed);
                    m.mutable_status()->set_error(status.error);
                    if (!stream.Write(m))
                    {
                        return;
                    }
                    sent_status = status;
                }
            }
        }
        stream.WritesDone(); // the server's read ends; it finishes the stream
    }

    void stop()
    {
        {
            // The publisher sends what is left and closes the stream (session() then cancels
            // what remains).
            std::unique_lock lock(mutex);
            stopping = true;
            cv.notify_all();
            // At most 3 s for the last pass; then the stream is cancelled.
            if (!cv.wait_for(lock, std::chrono::seconds(3), [&] { return current_context == nullptr; }))
            {
                current_context->TryCancel(); // a write that does not return
            }
        }
        cv.notify_all();
        if (thread.joinable())
        {
            thread.join();
        }
    }
};

AgentClient::AgentClient(control::Agent& agent, AgentClientOptions options) :
  impl_(std::make_unique<Impl>(agent, std::move(options)))
{
}

AgentClient::~AgentClient()
{
    stop();
}

bool AgentClient::connected() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->connected;
}

bool AgentClient::wait_connected(std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->connected; });
}

std::uint64_t AgentClient::sessions() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->sessions;
}

std::string AgentClient::last_error() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->last_error;
}

bool AgentClient::replaced() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->replaced;
}

std::string AgentClient::connection_id() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->connection_id;
}

const std::string& AgentClient::incarnation() const noexcept
{
    return impl_->incarnation;
}

void AgentClient::stop()
{
    impl_->stop();
}

} // namespace vehicle_sil::control_grpc
