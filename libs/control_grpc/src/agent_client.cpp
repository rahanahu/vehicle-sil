// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_grpc/agent_client.hpp"

#include "convert.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
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

} // namespace

struct AgentClient::Impl
{
    control::Agent& agent;
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
    std::deque<v1::AgentMessage> outbound;  // acknowledgements from the reader

    // Transport bookkeeping (guarded by mutex).
    std::set<std::uint64_t> queued;          // queued by the agent; result not seen yet
    std::deque<control::CommandResult> recent; // the latest results, sent again after a reconnect
    bool had_session = false;

    std::thread thread;

    Impl(control::Agent& a, AgentClientOptions o) : agent(a), options(std::move(o))
    {
        incarnation = options.incarnation.empty() ? make_incarnation() : options.incarnation;
        if (options.publish_period <= std::chrono::milliseconds::zero() || options.reconnect_min <= std::chrono::milliseconds::zero()
            || options.reconnect_max < options.reconnect_min)
        {
            throw std::invalid_argument("AgentClientOptions: publish_period and reconnect_min must be > 0, reconnect_max >= reconnect_min");
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
            // At most 2 s for the last pass; then the stream is cancelled.
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

const std::string& AgentClient::incarnation() const noexcept
{
    return impl_->incarnation;
}

void AgentClient::stop()
{
    impl_->stop();
}

} // namespace vehicle_sil::control_grpc
