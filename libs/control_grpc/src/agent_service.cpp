// SPDX-License-Identifier: MIT
#include "agent_service.hpp"

#include "convert.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"

#include <algorithm>
#include <deque>
#include <set>
#include <utility>

namespace vehicle_sil::control_grpc::detail {
namespace {

constexpr std::uint64_t kMaxAgentPending = 65536;

// ---- the agent stream ------------------------------------------------------------------------

/// One agent connection: a bidirectional stream. Owned by gRPC (deleted in OnDone).
class AgentStream final : public grpc::ServerBidiReactor<v1::AgentMessage, v1::ServerMessage>
{
public:
    /// The hub's link to this stream. Outlives the stream (the hub may hold it); once the stream
    /// is done, send() refuses.
    struct Link final : AgentLink
    {
        std::mutex mutex;
        AgentStream* stream = nullptr; // guarded by mutex; nullptr once done
        bool send(const Command& command) override
        {
            const std::lock_guard lock(mutex);
            return stream != nullptr && stream->queue(command);
        }
        void close() override
        {
            // Finished with ABORTED, which the agent takes as a refusal (it waits reconnect_max:
            // two live processes with one name do not take the connection from each other many
            // times a second). Not under this mutex: OnDone() locks the handle and then this
            // mutex. While the handle's mutex is held and its context is set, OnDone() has not
            // begun, so the stream is alive.
            AgentStream* s = nullptr;
            std::shared_ptr<StreamRegistry::Handle> handle;
            {
                const std::lock_guard lock(mutex);
                if (stream == nullptr)
                {
                    return;
                }
                s = stream;
                handle = stream->handle_;
            }
            const std::lock_guard lock(handle->mutex);
            if (handle->context == nullptr)
            {
                return;
            }
            // Not thinned with the refused calls: a flood of those must not hide a takeover.
            s->audit_(s->who_.name + "@" + s->peer_ + " agent: replaced by another connection of the same participant");
            s->finish(grpc::Status(grpc::StatusCode::ABORTED, "replaced by another connection of the same participant"));
        }
    };

    AgentStream(Hub& hub, Who who, std::string peer, const AuditLog& audit,
                std::shared_ptr<StreamRegistry::Handle> handle) :
      hub_(hub),
      link_(std::make_shared<Link>()),
      who_(std::move(who)),
      peer_(std::move(peer)),
      audit_(audit),
      handle_(std::move(handle))
    {
        link_->stream = this;
        StartRead(&in_);
    }

    void OnReadDone(bool ok) override
    {
        if (!ok)
        {
            finish(grpc::Status::OK);
            return;
        }
        if (!dispatch())
        {
            return;
        }
        {
            // Another thread may have finished the stream meanwhile (close(), OnCancel()).
            const std::lock_guard lock(write_mutex_);
            if (finished_)
            {
                return;
            }
        }
        in_.Clear();
        StartRead(&in_);
    }

    void OnWriteDone(bool ok) override
    {
        const std::lock_guard lock(write_mutex_);
        out_.pop_front();
        writing_ = false;
        if (!ok)
        {
            broken_ = true;
            return;
        }
        start_write();
    }

    void OnDone() override
    {
        {
            const std::lock_guard lock(handle_->mutex);
            handle_->context = nullptr;
        }
        {
            const std::lock_guard lock(link_->mutex);
            link_->stream = nullptr;
        }
        if (connection_ != 0)
        {
            hub_.agent_disconnected(connection_);
        }
        delete this;
    }

    void OnCancel() override { finish(grpc::Status::CANCELLED); }

private:
    /// Called with link_->mutex held.
    bool queue(const Command& command)
    {
        const std::lock_guard lock(write_mutex_);
        if (broken_ || finished_)
        {
            return false;
        }
        v1::ServerMessage message;
        to_proto(command, *message.mutable_command());
        out_.push_back(std::move(message));
        start_write();
        return true;
    }

    /// write_mutex_ held.
    void start_write()
    {
        if (!writing_ && !out_.empty() && !finished_)
        {
            writing_ = true;
            StartWrite(&out_.front());
        }
    }

    void finish(grpc::Status status)
    {
        {
            const std::lock_guard lock(write_mutex_);
            if (std::exchange(finished_, true))
            {
                return;
            }
        }
        Finish(std::move(status));
    }

    /// @returns false if the stream was finished.
    bool dispatch()
    {
        if (connection_ == 0)
        {
            const auto refuse = [&](grpc::StatusCode code, const std::string& why) {
                audit_.refused(who_.name + "@" + peer_ + " agent "
                               + (in_.has_hello() ? in_.hello().participant() : std::string("?")) + ": refused (" + why + ")");
                finish(grpc::Status(code, why));
                return false;
            };
            if (!in_.has_hello() || in_.hello().participant().empty() || in_.hello().max_pending() == 0
                || in_.hello().max_pending() > kMaxAgentPending)
            {
                return refuse(grpc::StatusCode::INVALID_ARGUMENT, "the first message must be a valid hello");
            }
            const auto& h = in_.hello();
            {
                std::set<std::string> names;
                for (const auto& v : h.schema())
                {
                    if (v.name().empty() || !names.insert(v.name()).second)
                    {
                        return refuse(grpc::StatusCode::INVALID_ARGUMENT, "the schema has an empty or repeated name");
                    }
                }
            }
            // An agent holds at most max_pending queued commands and as many recent results.
            if (static_cast<std::uint64_t>(h.open_seqs_size()) > 2 * h.max_pending())
            {
                return refuse(grpc::StatusCode::INVALID_ARGUMENT, "more open_seqs than the agent can hold");
            }
            if (!who_.participants.empty()
                && std::find(who_.participants.begin(), who_.participants.end(), h.participant()) == who_.participants.end())
            {
                return refuse(grpc::StatusCode::PERMISSION_DENIED, "this credential may not connect as " + h.participant());
            }
            audit_(who_.name + "@" + peer_ + " agent " + h.participant() + " (" + h.incarnation() + "): connected");
            PeerHello hello;
            hello.participant = h.participant();
            hello.incarnation = h.incarnation();
            for (const auto& v : h.schema())
            {
                hello.schema.push_back(from_proto(v));
            }
            hello.max_pending = static_cast<std::size_t>(h.max_pending());
            if (h.has_open_seqs())
            {
                hello.open_seqs = std::vector<std::uint64_t>(h.open_seqs().begin(), h.open_seqs().end());
            }
            {
                // The welcome goes into the queue before the hub knows the link, so it is written
                // before any command (the hub may send one as soon as agent_connected() swapped
                // the link in).
                v1::ServerMessage welcome;
                welcome.mutable_welcome();
                const std::lock_guard lock(write_mutex_);
                out_.push_back(std::move(welcome));
                start_write();
            }
            connection_ = hub_.agent_connected(std::move(hello), link_);
            return true;
        }
        switch (in_.kind_case())
        {
        case v1::AgentMessage::kSubmitted:
        {
            const auto& s = in_.submitted();
            SubmitResult r;
            r.status = s.status() == v1::SUBMIT_STATUS_QUEUED ? SubmitStatus::Queued
                       : s.status() == v1::SUBMIT_STATUS_BUSY ? SubmitStatus::Busy
                                                              : SubmitStatus::Invalid; // also UNSPECIFIED and unknown
            r.reason = s.reason();
            hub_.agent_submitted(connection_, s.seq(), r);
            break;
        }
        case v1::AgentMessage::kResult:
            if (const auto result = from_proto(in_.result()))
            {
                hub_.agent_result(connection_, *result);
            }
            break; // a status this server does not know: the operation stays open
        case v1::AgentMessage::kValues:
            hub_.agent_values(connection_, from_proto(in_.values()));
            break;
        case v1::AgentMessage::kEvent:
            hub_.agent_events(connection_, {from_proto(in_.event())});
            break;
        case v1::AgentMessage::kStatus:
            hub_.agent_status(connection_, control::PeerStatus{in_.status().failed(), in_.status().error()});
            break;
        default:
            break; // a second hello or an unknown message: ignored
        }
        return true;
    }

    Hub& hub_;
    std::shared_ptr<Link> link_;
    Who who_;
    std::string peer_;
    const AuditLog& audit_;
    std::shared_ptr<StreamRegistry::Handle> handle_;
    v1::AgentMessage in_;
    ConnectionId connection_ = 0;
    std::mutex write_mutex_;
    std::deque<v1::ServerMessage> out_; // bounded by the hub: at most max_pending commands are open per peer
    bool writing_ = false;
    bool broken_ = false;
    bool finished_ = false;
};

/// A stream refused before it began.
class RefusedStream final : public grpc::ServerBidiReactor<v1::AgentMessage, v1::ServerMessage>
{
public:
    explicit RefusedStream(grpc::Status status) { Finish(std::move(status)); }
    void OnDone() override { delete this; }
};

class AgentServiceImpl final : public v1::AgentService::CallbackService
{
public:
    AgentServiceImpl(Hub& hub, const Authorizer& auth, const AuditLog& audit, StreamRegistry& streams) :
      hub_(hub), auth_(auth), audit_(audit), streams_(streams)
    {
    }
    grpc::ServerBidiReactor<v1::AgentMessage, v1::ServerMessage>* Connect(grpc::CallbackServerContext* context) override
    {
        static const RpcRule* const rule = find_rule(kAgentService, "Connect");
        const auto who = auth_.identify(*context);
        if (!who || rule == nullptr || !allows(*who, rule->need))
        {
            audit_.refused((who ? who->name : std::string("unauthenticated")) + "@" + context->peer() + " agent: refused");
            return new RefusedStream(grpc::Status(who ? grpc::StatusCode::PERMISSION_DENIED : grpc::StatusCode::UNAUTHENTICATED,
                                                  who ? "this credential may not connect an agent" : "a valid agent token is required"));
        }
        return new AgentStream(hub_, *who, context->peer(), audit_, streams_.add(context));
    }

private:
    Hub& hub_;
    const Authorizer& auth_;
    const AuditLog& audit_;
    StreamRegistry& streams_;
};

} // namespace

std::unique_ptr<grpc::Service> make_agent_service(Hub& hub, const Authorizer& auth, const AuditLog& audit, StreamRegistry& streams)
{
    return std::make_unique<AgentServiceImpl>(hub, auth, audit, streams);
}

} // namespace vehicle_sil::control_grpc::detail
